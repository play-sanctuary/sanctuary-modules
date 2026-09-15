--[[
    Sanctuary Minimap - tracking more than one thing at a time.

    The minimap's tracking button opens THIS menu instead of the client's. That is the whole
    shape of it: the stock menu is a one-of-many list, because the client can hold one kind
    of tracking at a time, and no amount of persuading changes that. Ours is a list of
    tick boxes, and it keeps what it is told.

    Two kinds of thing are in it, and they work differently underneath.

    SPELL TRACKING - Find Herbs, Find Minerals, Track Humanoids - already stacks on the
    server: tracking lives in two bitmasks there, PLAYER_TRACK_CREATURES and
    PLAYER_TRACK_RESOURCES, and each spell sets only its own bit. What enforced one-at-a-time
    was the client's SetTracking(), which cancels whatever was tracked before casting the
    next. This never calls it: a tick casts the spell, and clearing one cancels its buff.

    PLACE TRACKING - mailbox, banker, repair, the vendors, the trainers - the client keeps in
    a single slot, remembered between sessions in its `minimapTrackedInfo` cvar. There is no
    mask to set and SetTracking takes one index, so this does not use the client's place
    tracking at all. It keeps a mask of its own, asks the server where things are, and draws
    the pins itself.

    The server sends each pin as a kind and an offset in yards, north and east: an addon has
    no world position of its own in 3.3.5, so a coordinate would be useless to it. Offsets
    go stale as you walk, so they arrive once a second and are slid along in between from the
    player's own map position - see Interpolate, which works out what a unit of map position
    is worth in yards by watching the two disagree.

    Names, icons and what the menu offers all come from the client's own tracking list, so a
    mailbox here is the mailbox the stock menu showed. Only the ORDER of the place kinds is
    ours, because the server's mask is in that order - PLACE_BITS below is the contract, and
    neither half may be reordered without the other.
]]

local PREFIX = "SMM"

-- The bit each place kind sits on in the mask sent to the server. The names are the
-- client's own (GlobalStrings' MINIMAP_TRACKING_*), which is how a menu entry is matched
-- to a bit. Changing this list means changing the enum in SanctuaryMinimap.cpp with it.
local PLACE_BITS = {
    [MINIMAP_TRACKING_MAILBOX or "Mailbox"] = 0,
    [MINIMAP_TRACKING_BANKER or "Banker"] = 1,
    [MINIMAP_TRACKING_AUCTIONEER or "Auctioneer"] = 2,
    [MINIMAP_TRACKING_INNKEEPER or "Innkeeper"] = 3,
    [MINIMAP_TRACKING_FLIGHTMASTER or "FlightMaster"] = 4,
    [MINIMAP_TRACKING_STABLEMASTER or "StableMaster"] = 5,
    [MINIMAP_TRACKING_BATTLEMASTER or "BattleMaster"] = 6,
    [MINIMAP_TRACKING_REPAIR or "Repair"] = 7,
    [MINIMAP_TRACKING_TRAINER_CLASS or "Class Trainer"] = 8,
    [MINIMAP_TRACKING_TRAINER_PROFESSION or "Profession Trainers"] = 9,
    [MINIMAP_TRACKING_VENDOR_AMMO or "Ammunition"] = 11,
    [MINIMAP_TRACKING_VENDOR_FOOD or "Food & Drink"] = 12,
    [MINIMAP_TRACKING_VENDOR_POISON or "Poisons"] = 13,
    [MINIMAP_TRACKING_VENDOR_REAGENT or "Reagents"] = 14,
}

--[[
    How far the minimap sees, in yards across, per zoom step.

    These are the numbers every map addon uses (Astrolabe's, and GatherMate's after it). The
    indoor set is different because the minimap draws closer indoors, and which set applies
    is read off the cvars: the client keeps the outdoor zoom in `minimapZoom` and the indoor
    one in `minimapInsideZoom`, and normally only one of them matches the zoom in use.
]]
local WIDTH_OUTDOOR = { [0] = 466.66, 400, 333.33, 266.66, 200, 133.33 }
local WIDTH_INDOOR = { [0] = 300, 240, 180, 120, 80, 50 }

local MAX_PINS = 40
local MAX_BUFFS = 40

local pins = {}                     -- pin frames, made as needed
local tracked = {}                  -- what the server last sent: { kind, north, east }
local pending                       -- a batch still arriving
local seen = 0                      -- pins that batch has described, kept or not
local mask = 0                      -- the place kinds we want
local places = {}                   -- { bit, name, texture }, from the client's own list
local spells = {}                   -- { id, name, texture }, likewise
local lastMapX, lastMapY
local yardsPerMapX, yardsPerMapY    -- nil until we have seen ourselves move

local frame = CreateFrame("Frame", "SanctuaryMinimapFrame", Minimap)

-- Everything the server told us, dropped: a half-arrived batch with it, so the next one
-- is not mistaken for the rest of this one.
local function Forget()
    tracked, pending, seen = {}, nil, 0
end

--------------------------------------------------------------------------
-- Talking to the server
--------------------------------------------------------------------------

-- %d rather than concatenation: the mask is built with ^, which gives a float here, and
-- how a float prints is the number formatter's business. The server reads an integer.
local function Tell()
    SendAddonMessage(PREFIX, string.format("WANT %d", mask), "WHISPER", UnitName("player"))
end

--[[
    The mask, one bit per place kind, without the bit library.

    Lua numbers here are doubles and the mask is at most fifteen bits, so arithmetic says
    exactly what band and bor would and needs nothing to be loaded for it.
]]
local function HasKind(kind)
    return math.floor(mask / (2 ^ kind)) % 2 == 1
end

local function SetKind(kind, wanted)
    if HasKind(kind) == wanted then
        return false
    end

    mask = wanted and (mask + (2 ^ kind)) or (mask - (2 ^ kind))
    return true
end

--------------------------------------------------------------------------
-- What the client offers, read once
--------------------------------------------------------------------------

local function ReadTrackingTypes()
    places, spells = {}, {}

    for id = 1, GetNumTrackingTypes() do
        local name, texture, _, category = GetTrackingInfo(id)

        if name then
            if category == "spell" then
                table.insert(spells, { id = id, name = name, texture = texture })
            else
                local bit = PLACE_BITS[name]

                -- A kind the client has and we do not is left out rather than guessed at;
                -- the server would not know what to look for.
                if bit then
                    table.insert(places, { bit = bit, name = name, texture = texture })
                end
            end
        end
    end
end

--[[
    The client's own place slot, emptied.

    It restores whatever it tracked last session from the `minimapTrackedInfo` cvar, and our
    menu can no longer put anything there, so whatever is in it is a leftover - from before
    this addon, or from the stock menu on another character. Left alone the client draws its
    own blips for that kind underneath our pins for the same thing, and everything looks
    doubled.

    Only a PLACE kind is cleared. SetTracking(nil) takes a tracking spell's aura with it, and
    those survive a logout on purpose: someone who logged out with Find Herbs up wants it up.
]]
local function ClearClientPlaceSlot()
    for id = 1, GetNumTrackingTypes() do
        local _, _, active, category = GetTrackingInfo(id)

        if active and category ~= "spell" then
            SetTracking(nil)
            return
        end
    end
end

local function TextureForKind(kind)
    for _, place in ipairs(places) do
        if place.bit == kind then
            return place.texture
        end
    end
end

--------------------------------------------------------------------------
-- The pins
--------------------------------------------------------------------------

local function Pin(index)
    if pins[index] then
        return pins[index]
    end

    local pin = CreateFrame("Frame", nil, Minimap)
    pin:SetWidth(14)
    pin:SetHeight(14)
    pin:SetFrameLevel(Minimap:GetFrameLevel() + 5)

    pin.icon = pin:CreateTexture(nil, "OVERLAY")
    pin.icon:SetAllPoints(pin)
    pin:Hide()

    pins[index] = pin
    return pin
end

local function Width()
    local zoom = Minimap:GetZoom()
    local inside = tonumber(GetCVar("minimapInsideZoom"))
    local outside = tonumber(GetCVar("minimapZoom"))

    local indoors = inside == zoom

    -- Both remembered zooms can be the same number, and then the zoom says nothing about
    -- where we are; the world has to be asked instead.
    if indoors and outside == zoom then
        indoors = IsIndoors and IsIndoors() or false
    end

    return (indoors and WIDTH_INDOOR[zoom] or WIDTH_OUTDOOR[zoom]) or WIDTH_OUTDOOR[0]
end

--[[
    How far the player has moved since the offsets were measured, in yards.

    Offsets are true for the instant they were taken, and a second of running is seven
    yards. The map position IS available to an addon, so its change gives the movement -
    once we know what a unit of it is worth in yards, which differs in every zone and is
    worked out in TakeBatch from the two sources disagreeing.
]]
local function Interpolate()
    local x, y = GetPlayerMapPosition("player")

    -- No position at all: an instance, or riding a taxi. Nothing to slide by.
    if not x or (x == 0 and y == 0) or not lastMapX then
        return 0, 0
    end

    --[[
        Each axis is learned from the player moving along it, so one can be known while the
        other is not - run due east and only the east one is. An axis with no scale yet is
        left at zero rather than guessed at, which costs at most the second until the next
        answer arrives; multiplying by a nil scale would throw instead.

        Map position runs east and south; pins are north and east.
    ]]
    local north = yardsPerMapY and -((y - lastMapY) * yardsPerMapY) or 0
    local east = yardsPerMapX and ((x - lastMapX) * yardsPerMapX) or 0

    return north, east
end

--[[
    The zoom and the rotate setting, looked up rarely rather than every frame.

    Draw runs on every frame - it has to, or a pin moves in visible steps when the player
    does - and the three cvar reads behind it were the only part of it worth avoiding.
    GetZoom is a cheap call and catches the common change; the rest is caught by
    CVAR_UPDATE, which invalidates this.
]]
local VIEW_STALE = 1            -- seconds; a backstop, not the mechanism

local viewZoom, viewYards, viewRotating, viewAt

local function View()
    local zoom = Minimap:GetZoom()
    local now = GetTime()

    -- Re-read once a second regardless. CVAR_UPDATE is what normally catches a change, and
    -- this is here so that anything which slips past it costs a second of staleness rather
    -- than lasting until the next zoom - one cvar read a second is not worth saving.
    if zoom ~= viewZoom or not viewAt or (now - viewAt) > VIEW_STALE then
        viewZoom, viewYards = zoom, nil
    end

    if not viewYards then
        viewYards = Width()
        viewRotating = GetCVar("rotateMinimap") == "1"
        viewAt = now
    end

    return viewYards, viewRotating
end

local function ForgetView()
    viewYards = nil
end

local function Draw()
    local half = Minimap:GetWidth() / 2
    local yards, rotating = View()
    local perYard = half / (yards / 2)
    local edge = (half - 7) * (half - 7)

    local movedNorth, movedEast = Interpolate()

    local facing = rotating and GetPlayerFacing() or 0
    local sin, cos = math.sin(facing), math.cos(facing)

    -- Far enough to cover both what is tracked now and any pin left over from a longer
    -- set, which still has to be hidden. Pins are made in order, so #pins is the highest
    -- that exists.
    for index = 1, math.max(#tracked, #pins) do
        local data = tracked[index]

        if not data then
            if pins[index] then
                pins[index]:Hide()
            end
        else
            local north = data.north - movedNorth
            local east = data.east - movedEast

            --[[
                North is up, unless the player has the minimap turning with them; then the
                way they face is up.

                GetPlayerFacing is 0 at north and grows COUNTER-clockwise, because the
                game's own angles are atan2(west, north) - so facing pi/2 is west. Up the
                screen is therefore (cos, -sin) in north-and-east terms and right of it is
                (sin, cos), which is where these two lines come from. Getting the signs the
                other way round mirrors the pins left-to-right, and only for the players who
                turned this cvar on.
            ]]
            local px, py = east, north

            if rotating then
                px = (north * sin) + (east * cos)
                py = (north * cos) - (east * sin)
            end

            px, py = px * perYard, py * perYard

            local pin = Pin(index)

            -- Past the edge is hidden, not pinned to the rim: a pin on the rim says
            -- "over there somewhere", which is what the edge of a map already says.
            if (px * px) + (py * py) > edge then
                pin:Hide()
            else
                -- Only when it actually changes: this runs every frame now.
                if pin.kindShown ~= data.kind then
                    local texture = TextureForKind(data.kind)

                    if texture then
                        pin.icon:SetTexture(texture)
                        pin.kindShown = data.kind
                    end
                end

                pin:SetPoint("CENTER", Minimap, "CENTER", px, py)
                pin:Show()
            end
        end
    end
end

--------------------------------------------------------------------------
-- What the server says
--------------------------------------------------------------------------

local function TakeBatch(total, perNorth, perEast, mapX, mapY, body)
    pending = pending or {}

    for kind, north, east in string.gmatch(body, "(%-?%d+),(%-?%d+),(%-?%d+);") do
        seen = seen + 1

        -- More pins than the minimap can hold legibly are dropped, and the nearest came
        -- first, so what is dropped is the far ones. They are still counted: the count is
        -- what says the answer is complete.
        if #pending < MAX_PINS then
            -- Tenths of a yard on the wire, so a pin does not shimmer half a pixel each
            -- time an answer lands.
            table.insert(pending, {
                kind = tonumber(kind),
                north = tonumber(north) / 10,
                east = tonumber(east) / 10,
            })
        end
    end

    -- Swapped in one go once the whole answer is in, so a set split across two messages
    -- does not flicker through half of itself.
    if seen < total then
        return
    end

    -- The zone's scale, worked out exactly by the server from the zone's own map bounds.
    -- Zero means a place with no map of its own - an instance - where the client gives us
    -- no map position either, so there is nothing to slide against.
    yardsPerMapY = perNorth > 0 and perNorth or nil
    yardsPerMapX = perEast > 0 and perEast or nil

    --[[
        The anchor these offsets were measured from - the server's, not ours.

        Reading our own position here instead looks equivalent and is not: the offsets were
        true when the server took them, and this message arrives a latency later. Anchoring
        on arrival bakes however far we travelled in that time into every pin, which at a
        flying mount's speed is a couple of yards, and since latency wobbles and the error
        grows with speed it reads as pins that shake when moving quickly.

        Sent in hundred-thousandths of the map, the client's own being 0..1.
    ]]
    lastMapX = mapX / 100000
    lastMapY = mapY / 100000

    tracked, pending, seen = pending, nil, 0
    Draw()
end

--------------------------------------------------------------------------
-- The menu, in place of the client's
--------------------------------------------------------------------------

local function CancelBuff(wanted)
    for index = 1, MAX_BUFFS do
        local name = UnitBuff("player", index)

        -- Buffs are contiguous: the first empty slot is the end of them.
        if not name then
            return false
        end

        if name == wanted then
            CancelUnitBuff("player", index)
            return true
        end
    end

    return false
end

local function SetPlace(kind, wanted)
    if not SetKind(kind, wanted) then
        return
    end

    SanctuaryMinimapDB = SanctuaryMinimapDB or {}
    SanctuaryMinimapDB.mask = mask

    Tell()

    if mask == 0 then
        Forget()
    end

    Draw()
end

local dropdown = CreateFrame("Frame", "SanctuaryMinimapDropDown", Minimap, "UIDropDownMenuTemplate")

local function Build()
    -- The client's list can change in a session: a profession learned brings Find Minerals
    -- with it, so it is re-read each time the menu opens rather than once at login.
    ReadTrackingTypes()

    local info

    for _, spell in ipairs(spells) do
        local _, _, active = GetTrackingInfo(spell.id)

        info = UIDropDownMenu_CreateInfo()
        info.text = spell.name
        info.icon = spell.texture
        info.checked = active
        info.keepShownOnClick = true
        info.func = function()
            -- Cast, or cancel its buff. SetTracking is never called: the cancel inside it
            -- is the whole reason tracking used to be one-at-a-time.
            local _, _, isOn = GetTrackingInfo(spell.id)

            if isOn then
                CancelBuff(spell.name)
            else
                CastSpellByName(spell.name)
            end
        end

        -- The spell icons are spell art and want the same trim the stock menu gave them.
        info.tCoordLeft, info.tCoordRight = 0.0625, 0.9
        info.tCoordTop, info.tCoordBottom = 0.0625, 0.9

        UIDropDownMenu_AddButton(info)
    end

    for _, place in ipairs(places) do
        info = UIDropDownMenu_CreateInfo()
        info.text = place.name
        info.icon = place.texture
        info.checked = HasKind(place.bit)
        info.keepShownOnClick = true
        info.func = function()
            SetPlace(place.bit, not HasKind(place.bit))
        end

        UIDropDownMenu_AddButton(info)
    end

    info = UIDropDownMenu_CreateInfo()
    info.text = NONE
    info.notCheckable = true
    info.func = function()
        for _, spell in ipairs(spells) do
            local _, _, active = GetTrackingInfo(spell.id)

            if active then
                CancelBuff(spell.name)
            end
        end

        mask = 0
        SanctuaryMinimapDB = SanctuaryMinimapDB or {}
        SanctuaryMinimapDB.mask = 0
        Forget()
        Tell()
        Draw()

        -- The client may still be holding a place kind of its own from before this addon
        -- existed, or from another character; this is the one time it is told anything.
        SetTracking(nil)
        CloseDropDownMenus()
    end

    UIDropDownMenu_AddButton(info)
end

UIDropDownMenu_Initialize(dropdown, Build, "MENU")

--[[
    The minimap's tracking button opens ours - but only once the server has said it is
    there.

    This matters. Our menu cannot set the client's own place slot, so replacing the stock
    menu on a realm whose worldserver has no mod-sanctuary-minimap would take mailboxes and
    bankers away and put nothing back: the ticks would do nothing at all. The same addon is
    served to every realm from one directory, and one of them may be a version behind, so
    the addon has to find out rather than assume. The server answers OK to the WANT sent at
    login, and until that arrives the stock menu stands untouched.

    Only OnClick is replaced - the press animation hangs off OnMouseDown and OnMouseUp and
    is left alone - and it is replaced rather than hooked, so the stock one-of-many menu
    cannot be reached by accident once ours is in. The anchor is the same frame and offset
    the stock click used, so the menu appears exactly where it always did.
]]
local ready = false

local function TakeOverTheMenu()
    if ready then
        return
    end

    local button = MiniMapTrackingButton or MiniMapTracking

    if not button then
        return
    end

    ready = true

    button:SetScript("OnClick", function()
        ToggleDropDownMenu(1, nil, dropdown, "MiniMapTracking", 0, -5)
        PlaySound("igMainMenuOptionCheckBoxOn")
    end)

    -- Safe to do only now: on a realm without the server half this would clear the one
    -- place kind the player still had, with no way to set it again.
    ClearClientPlaceSlot()
end

--------------------------------------------------------------------------
-- Events
--------------------------------------------------------------------------

frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")

-- A new zone is a new map, so the position the pins were last measured against means
-- nothing now. The baseline is dropped and set again by the next answer, a second away.
-- Only NEW_AREA: plain ZONE_CHANGED also fires for subzones, which share the zone's map.
frame:RegisterEvent("ZONE_CHANGED_NEW_AREA")

-- The minimap's width in yards and whether it turns with the player are cached; this is
-- what notices the player changing either in the options.
frame:RegisterEvent("CVAR_UPDATE")

frame:SetScript("OnEvent", function(_, event, prefix, message)
    if event == "PLAYER_ENTERING_WORLD" then
        -- The server remembers nothing across a session, so say what this character wants.
        -- The mask itself is kept per character in the saved variable.
        mask = (SanctuaryMinimapDB and SanctuaryMinimapDB.mask) or 0
        Forget()
        yardsPerMapX, yardsPerMapY, lastMapX, lastMapY = nil, nil, nil, nil

        ReadTrackingTypes()
        Tell()
        return
    end

    if event == "CVAR_UPDATE" then
        ForgetView()
        return
    end

    if event == "ZONE_CHANGED_NEW_AREA" then
        yardsPerMapX, yardsPerMapY, lastMapX, lastMapY = nil, nil, nil, nil
        return
    end

    if prefix ~= PREFIX or not message then
        return
    end

    -- The answer to our WANT, and the only proof that this realm has the server half.
    if message == "OK" then
        TakeOverTheMenu()
        return
    end

    -- The header is repeated in every message of a split batch, and they all carry the same
    -- figures, so taking them from whichever arrives is right.
    local total, perNorth, perEast, mapX, mapY, body =
        string.match(message, "^PINS (%d+) (%d+) (%d+) (%d+) (%d+):(.*)$")

    if total then
        TakeBatch(tonumber(total), tonumber(perNorth), tonumber(perEast),
                  tonumber(mapX), tonumber(mapY), body or "")
    end
end)

--[[
    Sliding the pins along between answers, every frame.

    This was throttled to twenty a second, on the reasoning that nobody can see a pin move
    faster than that. They can: on a flying mount a twentieth of a second is more than a
    yard, which is more than a pixel, so the pins advanced in visible steps exactly when
    moving fastest. The cost of every frame is one map position and a walk over the pins -
    the cvar reads behind it are cached in View.
]]
frame:SetScript("OnUpdate", function()
    if mask ~= 0 then
        Draw()
    end
end)
