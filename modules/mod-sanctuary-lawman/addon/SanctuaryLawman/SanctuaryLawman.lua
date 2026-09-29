--[[
    Sanctuary Lawman

    One button, on the minimap, for going on and off duty. It only ever appears for somebody
    who actually holds the office: it stays hidden until the server says otherwise, and the
    server is asked rather than the client deciding for itself.

    The button never sets its own state. Everything it shows is the last thing the server
    said, because the server can refuse - and a button that lies about that is worse than no
    button.

    There was a panel too, with a status line and a toggle of its own. It said nothing this
    does not: the icon carries duty as colour, the tooltip carries the title and the rest,
    and both sent the identical request. Two controls for one decision is one too many.
]]

local ADDON_PREFIX = "SLAW"

local playerName
local state = { lawman = false, duty = false, title = "" }
local haveState = false

--------------------------------------------------------------------------
-- Talking to the server
--------------------------------------------------------------------------

local function RequestSync()
    SendAddonMessage(ADDON_PREFIX, "SYNC", "WHISPER", playerName or UnitName("player"))
end

-- AzerothCore's addon command channel, not a chat line starting with a dot: an
-- unrecognised dot command falls through and gets published to /say, which would announce
-- your business to the room.
local commandCounter = 0

local function SendServerCommand(command)
    commandCounter = (commandCounter + 1) % 10000
    SendAddonMessage("AzerothCore",
        string.format("i%04d%s", commandCounter, command),
        "WHISPER",
        playerName or UnitName("player"))
end

--------------------------------------------------------------------------
-- Reading what the server last said
--------------------------------------------------------------------------

local function HandleState(body)
    local lawman = string.match(body, "lawman=(%d)")
    local duty = string.match(body, "duty=(%d)")
    local title = string.match(body, "title=(%S+)")

    state.lawman = (lawman == "1")
    state.duty = (duty == "1")
    state.title = (title ~= "-" and title) or "Lawman"

    haveState = true

    -- Defined further down, with the button. Safe unguarded: nothing reaches here until the
    -- server answers, which is long after the file has finished loading.
    SanctuaryLawman_RefreshButton()
end

--------------------------------------------------------------------------
-- Input
--------------------------------------------------------------------------

SLASH_SANCTUARYLAWMAN1 = "/lawman"
SlashCmdList["SANCTUARYLAWMAN"] = function(input)
    input = string.lower(string.trim and string.trim(input) or input or "")

    -- show/hide used to open and close the panel. They govern the minimap button now, which
    -- is the only thing left to show or hide. Worth keeping: somebody who holds the office
    -- permanently may not want the badge on their minimap permanently.
    if input == "show" then
        SanctuaryLawmanDB.hidden = false
        SanctuaryLawman_RefreshButton()
    elseif input == "hide" then
        SanctuaryLawmanDB.hidden = true
        SanctuaryLawman_RefreshButton()
    else
        SendServerCommand("lawman " .. (input ~= "" and input or "status"))
    end
end

--------------------------------------------------------------------------
-- Events
--------------------------------------------------------------------------

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("VARIABLES_LOADED")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")

listener:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "VARIABLES_LOADED" then
        SanctuaryLawmanDB = SanctuaryLawmanDB or {}

        -- The button was placed at load from the default; now that the saved angle has
        -- loaded, put it back where the player left it.
        if SanctuaryLawmanDB.buttonAngle and SanctuaryLawman_PlaceButton then
            SanctuaryLawman_PlaceButton(SanctuaryLawmanDB.buttonAngle)
        end
        return
    end

    if event == "CHAT_MSG_ADDON" then
        if arg1 ~= ADDON_PREFIX then return end

        local verb, body = string.match(arg2, "^(%u+)%s*(.*)$")
        if verb == "STATE" then
            HandleState(body)
        elseif verb == "SEARCH" then
            -- Defined with the search popup at the end of the file. Safe unguarded:
            -- nothing reaches here until the server answers, which is long after the
            -- file has finished loading.
            SanctuaryLawman_HandleSearchAnswer(body)
        end
        return
    end

    -- PLAYER_ENTERING_WORLD
    playerName = UnitName("player")

    SanctuaryLawmanDB = SanctuaryLawmanDB or {}

    SanctuaryLawman_RefreshButton()
end)

--[[
    The server pushes state at login, but that push can land while the client is still on
    the loading screen, and after a /reload this addon knows nothing at all. So it asks,
    and keeps asking until answered - then rarely, since duty changes only when the player
    presses the button.
]]
local elapsed = 0
listener:SetScript("OnUpdate", function(self, delta)
    elapsed = elapsed + delta

    local interval = haveState and 30 or 2
    if elapsed < interval then return end

    elapsed = 0
    RequestSync()
end)

--------------------------------------------------------------------------
-- The button
--------------------------------------------------------------------------

--[[
    Within reach of the minimap, and never opening anything.

    Hidden outright for anybody who does not hold the office - not greyed, not empty, absent.
    Somebody who is not a lawman is never told there was anything to see.
]]

local lawmanButton = CreateFrame("Button", "SanctuaryLawmanButton", Minimap)
lawmanButton:SetWidth(31)
lawmanButton:SetHeight(31)
lawmanButton:SetFrameStrata("MEDIUM")
lawmanButton:SetFrameLevel(8)
lawmanButton:RegisterForClicks("LeftButtonUp")
lawmanButton:RegisterForDrag("LeftButton")
lawmanButton:SetMovable(true)
lawmanButton:Hide()

local lawmanIcon = lawmanButton:CreateTexture(nil, "BACKGROUND")
lawmanIcon:SetWidth(20)
lawmanIcon:SetHeight(20)
lawmanIcon:SetPoint("TOPLEFT", 7, -6)
lawmanIcon:SetTexture("Interface\\Icons\\INV_Shirt_GuildTabard_01")

local lawmanBorder = lawmanButton:CreateTexture(nil, "OVERLAY")
lawmanBorder:SetWidth(53)
lawmanBorder:SetHeight(53)
lawmanBorder:SetPoint("TOPLEFT", 0, 0)
lawmanBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

--- Places the button on the minimap's edge at the given angle, in degrees.
local function PlaceLawmanButton(angle)
    -- math.rad because the global cos/sin the WoW API adds take degrees and these do not.
    local x = 80 * math.cos(math.rad(angle))
    local y = 80 * math.sin(math.rad(angle))
    lawmanButton:SetPoint("CENTER", Minimap, "CENTER", x, y)
end

local function SavedLawmanAngle()
    if SanctuaryLawmanDB and SanctuaryLawmanDB.buttonAngle then
        return SanctuaryLawmanDB.buttonAngle
    end
    -- Not on the arc the other Sanctuary buttons share (faction -113.41, disguise -132.51,
    -- outlaw -152.22, profile -171.25): this one sits by itself above and to the left, where
    -- NotPlater's button used to be. Drag it if it lands somewhere awkward; the angle is
    -- remembered.
    return 147.66
end

local function DragLawmanButton(self)
    local mx, my = Minimap:GetCenter()
    local cx, cy = GetCursorPosition()
    local scale = UIParent:GetScale()

    local angle = math.deg(math.atan2((cy / scale) - my, (cx / scale) - mx))

    PlaceLawmanButton(angle)

    if SanctuaryLawmanDB then
        SanctuaryLawmanDB.buttonAngle = angle
    end
end

lawmanButton:SetScript("OnDragStart", function(self)
    self:SetScript("OnUpdate", DragLawmanButton)
end)

lawmanButton:SetScript("OnDragStop", function(self)
    self:SetScript("OnUpdate", nil)
end)

--- The whole of the addon's drawing: whether the badge is there, and what colour.
function SanctuaryLawman_RefreshButton()
    -- Not a lawman, or asked to be put away: gone entirely.
    if not state.lawman or (SanctuaryLawmanDB and SanctuaryLawmanDB.hidden) then
        lawmanButton:Hide()
        return
    end

    lawmanButton:Show()

    if not haveState then
        lawmanIcon:SetVertexColor(0.5, 0.5, 0.5)
    elseif state.duty then
        lawmanIcon:SetVertexColor(0.5, 0.9, 0.42)
    else
        lawmanIcon:SetVertexColor(1.0, 1.0, 1.0)
    end
end

lawmanButton:SetScript("OnClick", function()
    -- Only ever a request. The server decides and the refresh draws whatever it decided.
    SendServerCommand(state.duty and "lawman off" or "lawman on")
end)

lawmanButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")

    if not haveState then
        GameTooltip:SetText("Lawman")
        GameTooltip:AddLine("Asking the server...", 1, 1, 1, true)
    elseif state.duty then
        GameTooltip:SetText("|cff7fb069" .. state.title .. "|r, on duty")
        GameTooltip:AddLine("Wearing the tabard and carrying the writ. Guards stay friendly to you.", 1, 1, 1, true)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine("Click to stand down.", 0.6, 0.6, 0.6, true)
    else
        GameTooltip:SetText(state.title .. ", off duty")
        GameTooltip:AddLine("Out of uniform, and treated as anybody else.", 1, 1, 1, true)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine("Click to go on duty.", 0.6, 0.6, 0.6, true)
        GameTooltip:AddLine("Drag to move.", 0.6, 0.6, 0.6, true)
    end

    GameTooltip:Show()
end)

lawmanButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

--- Exposed so VARIABLES_LOADED can reposition once the saved angle is available.
function SanctuaryLawman_PlaceButton(angle)
    PlaceLawmanButton(angle)
end

PlaceLawmanButton(SavedLawmanAngle())
SanctuaryLawman_RefreshButton()


--------------------------------------------------------------------------
-- Searching whoever cannot walk away
--------------------------------------------------------------------------

--[[
    The Search Gloves, as a button on the target rather than an item in the bag.

    Using the gloves out of the pack works and still does, but it is the wrong shape for
    what it is: you have already selected the person, and the answer to "can I search them"
    is written on them in a debuff either way. So when you target somebody who cannot walk
    away, the offer appears.

    The server decides, and this only draws the answer.

    It was written the other way first - scanning the target for the shackle and bleed-out
    auras and counting the gloves with GetItemCount - and that put a copy of a server rule
    on the client, where it was wrong: the button never appeared for an unconscious target,
    which is the case it is most wanted in. Whatever the exact reason, the client had no
    business deciding it. Being shackled and being downed are server state, and only the
    server can answer without guessing.

    So the addon asks on every target change and the server replies with the same checks
    the search itself runs, which also means the two can never disagree.

    Deliberately NOT shown to somebody without the gloves: a button that exists only to
    refuse teaches people to ignore buttons. That rule is on the server with the rest.
]]

local canSearch = false

local searchPopup = CreateFrame("Frame", "SanctuaryLawmanSearchFrame", UIParent)
searchPopup:SetWidth(150)
searchPopup:SetHeight(46)
searchPopup:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 24,
    insets = { left = 8, right = 8, top = 8, bottom = 8 },
})
searchPopup:SetFrameStrata("HIGH")
searchPopup:Hide()

-- Under the target's portrait, which is where the player is already looking. Falls back to
-- the middle of the screen if some other addon has replaced the stock frame.
if TargetFrame then
    searchPopup:SetPoint("TOP", TargetFrame, "BOTTOM", 0, 6)
else
    searchPopup:SetPoint("CENTER", UIParent, "CENTER", 0, 120)
end

local searchButton = CreateFrame("Button", nil, searchPopup, "UIPanelButtonTemplate")
searchButton:SetWidth(126)
searchButton:SetHeight(22)
searchButton:SetPoint("CENTER", searchPopup, "CENTER", 0, 0)
searchButton:SetText("Search them")

-- Over the addon channel like everything else. The findings still reach the chat frame:
-- the command writes them through the player's session rather than through the handler it
-- was given, which is what makes them land on screen either way.
searchButton:SetScript("OnClick", function()
    SendServerCommand("search")
end)

searchButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText("Search them", 1, 1, 1)
    GameTooltip:AddLine("Go through their pack and count their purse. They will feel it.",
        nil, nil, nil, true)
    GameTooltip:Show()
end)
searchButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

local function RefreshSearchOffer()
    -- The obvious half is still done here, because it needs no round trip and stops the
    -- addon asking the server about every squirrel and mailbox the player clicks.
    if not UnitExists("target") or not UnitIsPlayer("target") or UnitIsUnit("target", "player") then
        canSearch = false
        searchPopup:Hide()
        return
    end

    if canSearch then
        searchPopup:Show()
    else
        searchPopup:Hide()
    end
end

--- Exposed for the addon-message handler further up, which runs before this file's end.
function SanctuaryLawman_HandleSearchAnswer(body)
    canSearch = (string.match(body, "ok=(%d)") == "1")
    RefreshSearchOffer()
end

local function AskCanSearch()
    if not UnitExists("target") or not UnitIsPlayer("target") or UnitIsUnit("target", "player") then
        return
    end

    SendAddonMessage(ADDON_PREFIX, "CANSEARCH", "WHISPER", playerName or UnitName("player"))
end

local searchWatcher = CreateFrame("Frame")
searchWatcher:RegisterEvent("PLAYER_TARGET_CHANGED")
searchWatcher:RegisterEvent("BAG_UPDATE")
searchWatcher:RegisterEvent("PLAYER_ENTERING_WORLD")

searchWatcher:SetScript("OnEvent", function(self, event)
    -- A new target's answer is not known yet, so the old one must not linger on screen.
    if event == "PLAYER_TARGET_CHANGED" then
        canSearch = false
        RefreshSearchOffer()
    end

    AskCanSearch()
end)

--[[
    Asked again while a target is held.

    Everything the answer depends on can change with no event to hang this on: they can be
    shackled or knocked down, they can come round, and either of you can walk out of range.
    A second is often enough to be the difference between offering the button and not, and
    the reply is two words.
]]
local searchSince = 0
searchWatcher:SetScript("OnUpdate", function(self, delta)
    searchSince = searchSince + delta

    if searchSince < 1.0 then
        return
    end
    searchSince = 0

    AskCanSearch()
    RefreshSearchOffer()
end)
