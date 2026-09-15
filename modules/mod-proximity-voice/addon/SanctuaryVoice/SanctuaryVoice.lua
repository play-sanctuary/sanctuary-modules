--[[
    Sanctuary Voice - in-game companion for mod-proximity-voice

    The world server decides everything; this addon shows it and asks for it.
    Devices and microphone settings live in the desktop launcher, because the
    game client cannot touch audio hardware.

    What it gives you in game:
      * who is speaking near you right now, shown over their nameplate rather
        than in a list, so you look at the person instead of the interface
      * your speaking distance, changed with the buttons on the panel
      * whether your voice client is attached

    Requests it makes are only ever requests. The world server clamps the range
    it is asked for and reports what it actually applied, so nothing here is a
    trust boundary.
]]

local ADDON_PREFIX = "SVOICE"

local state = {
    range = 25,
    minRange = 5,
    maxRange = 100,
    muted = false,
    connected = false,
    -- Replaced by the world server's first status message, and only stands in until that
    -- arrives - so it names the realm rather than the player's own machine.
    host = "sanctuary-wow.com",
    port = 7789,
}

-- guid -> { name = ..., since = ... }
local speakers = {}

--------------------------------------------------------------------------
-- Frames
--------------------------------------------------------------------------

local BACKDROP = {
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true,
    tileSize = 32,
    edgeSize = 16,
    insets = { left = 5, right = 5, top = 5, bottom = 5 },
}

-- Deliberately small. The list of who is talking is shown where the player is already
-- looking: over the speaker's head.
local hud = CreateFrame("Frame", "SanctuaryVoiceHUD", UIParent)
hud:SetWidth(176)
hud:SetHeight(82)          -- a row for the language picker under the status line
hud:SetPoint("CENTER", UIParent, "CENTER", 300, 120)
hud:SetBackdrop(BACKDROP)
hud:SetBackdropColor(0, 0, 0, 0.65)
hud:SetMovable(true)
hud:EnableMouse(true)
hud:RegisterForDrag("LeftButton")
hud:SetScript("OnDragStart", hud.StartMoving)
hud:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    if SanctuaryVoiceDB then
        local point, _, relativePoint, x, y = self:GetPoint()
        SanctuaryVoiceDB.point = point
        SanctuaryVoiceDB.relativePoint = relativePoint
        SanctuaryVoiceDB.x = x
        SanctuaryVoiceDB.y = y
    end
end)

local statusLine = hud:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
statusLine:SetPoint("TOPLEFT", hud, "TOPLEFT", 12, -10)
statusLine:SetJustifyH("LEFT")

-- The wording that used to sit on the frame moved here, so the state is still
-- explainable without spending a line of the display on it.
hud:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_TOP")
    GameTooltip:SetText("Proximity Voice")

    if state.connected then
        GameTooltip:AddLine("Desktop client attached.", 0.5, 0.9, 0.6, true)
    else
        GameTooltip:AddLine("Desktop client not attached. Open the Sanctuary launcher.",
            0.9, 0.6, 0.3, true)
    end

    if state.muted then
        GameTooltip:AddLine("Your microphone is muted.", 0.9, 0.3, 0.3, true)
    end

    GameTooltip:AddLine("Heard up to " .. state.range .. " yards away.", 0.8, 0.8, 0.8, true)
    GameTooltip:AddLine("/svoice for options.", 0.6, 0.6, 0.6, true)
    GameTooltip:Show()
end)

hud:SetScript("OnLeave", function() GameTooltip:Hide() end)

--------------------------------------------------------------------------
-- Rendering
--------------------------------------------------------------------------

-- Assigned further down, once the command channel it needs exists. Declared
-- here so Refresh can close over it: a local created later would be a
-- different variable, and Refresh would never see it.
local UpdateRangeButtons

local RefreshLanguage      -- defined with the language picker, below

local function Refresh()
    -- One dot carries the whole state: attached and able to speak, attached but muted,
    -- or not attached at all. The tooltip spells out whichever it is.
    local dot = "|cff7fb069*|r"

    if not state.connected then
        dot = "|cffe0a458*|r"
    elseif state.muted then
        dot = "|cffd9534f*|r"
    end

    statusLine:SetText(string.format("%s  %d yd", dot, state.range))

    if RefreshLanguage then
        RefreshLanguage()
    end

    if UpdateRangeButtons then
        UpdateRangeButtons()
    end
end

--------------------------------------------------------------------------
-- Server messages
--------------------------------------------------------------------------

--- Parses "key=value key=value" into a table. The server escapes nothing here
--- because none of these values can contain a space.
local function ParseFields(body)
    local fields = {}
    for key, value in string.gmatch(body, "(%w+)=([^%s]*)") do
        fields[key] = value
    end
    return fields
end

local function HandleConfig(body)
    local fields = ParseFields(body)

    state.range = tonumber(fields.range) or state.range
    state.minRange = tonumber(fields.min) or state.minRange
    state.maxRange = tonumber(fields.max) or state.maxRange
    state.muted = fields.muted == "1"
    state.connected = fields.conn == "1"
    state.host = fields.host or state.host
    state.port = tonumber(fields.port) or state.port

    -- What you speak and what you know, as the server numbers them. Sent on every push,
    -- so the picker is never behind the realm.
    state.language = tonumber(fields.lang) or state.language
    -- "1,7:40": a bare id is a language known outright, id:percent one known in part.
    state.languages = {}
    state.proficiency = {}

    for item in string.gmatch(fields.langs or "", "[^,]+") do
        local id, percent = string.match(item, "^(%d+):?(%d*)$")

        if id then
            table.insert(state.languages, tonumber(id))
            state.proficiency[tonumber(id)] = tonumber(percent) or 100
        end
    end

    Refresh()
end

--------------------------------------------------------------------------
-- Speaking indicator over the speaker's head
--------------------------------------------------------------------------

--[[
    A 3.3.5a nameplate carries no unit token, so there is no way to ask the game
    which plate belongs to whom. The only handle is the name drawn on it, which
    is why the server sends the speaker's name alongside the guid.

    A plate can only be decorated while the client is drawing it, which the
    player controls with the V key. When plates are hidden there is nothing to
    attach to - 3.3.5a exposes no way to place a frame at a world position - so
    the HUD list stays as the fallback that always works.
]]

--[[
    Who counts as speaking, and for how long.

    Held as an expiry rather than a flag, for two reasons.

    A stop is not the end of a conversation. The relay reports one after 300ms of silence,
    which is every pause between sentences, so clearing on the spot makes the indicator
    blink through anything longer than a single breath. A stop therefore schedules an
    expiry instead of removing the entry.

    And a start is not refreshed. The relay only reports transitions, so a minute-long
    speech is one message; anything that expects periodic updates will drop the indicator
    part way through. Actively speaking is a far-future expiry, not a countdown.
]]

-- name -> the moment they stop counting as speaking.
local speakingUntil = {}

--- How long the indicator stays up after a sentence ends.
local SPEAKER_LINGER = 0.9

--- Safety net, only reached if a stop message is ever lost entirely.
local SPEAKER_MAX = 90

local function IsSpeakingName(name)
    local expires = name and speakingUntil[name]
    return expires ~= nil and GetTime() < expires
end

--[[
    The plate outlives the bars.

    The bars follow speech closely: nine tenths of a second after a sentence ends they go,
    which is right for an indicator. The PLATE used the same clock, and that was wrong: a
    pause for breath longer than that is every second sentence, so a talking plate blinked
    out and back with the rhythm of the conversation. Somebody who spoke a moment ago is
    still the person you are looking at. Their plate stays for a few seconds after their
    last word, and only then is it handed back to the filter.
]]
local visibleUntil = {}
local VISIBLE_LINGER = 5.0

local function IsRecentlyHeardName(name)
    local expires = name and visibleUntil[name]
    return expires ~= nil and GetTime() < expires
end

local function AnyoneRecentlyHeard()
    local now = GetTime()

    for _, expires in pairs(visibleUntil) do
        if now < expires then
            return true
        end
    end

    return false
end

--[[
    Speakers the server says are fighting, held visible until it says they have stopped.

    The client cannot ask whether another character is in combat - a nameplate has no unit
    token to ask about - so the server says, for anyone it recently heard. A held plate
    stays on screen after the speech ends, without the bars, for as long as the fight
    lasts. Each speaker is held on their own, so two people in the same fight are two
    holds, and which of them the player is targeting does not come into it.

    The expiry is a safety net for a release that never arrives, not a duration.
]]
local combatUntil = {}
local COMBAT_HOLD_MAX = 180

local function IsHeldForCombat(name)
    local expires = name and combatUntil[name]
    return expires ~= nil and GetTime() < expires
end

local function AnyHeldForCombat()
    local now = GetTime()

    for _, expires in pairs(combatUntil) do
        if now < expires then
            return true
        end
    end

    return false
end

-- plate -> the icon we attached to it.
local plateIcons = {}

--[[
    Recognising a nameplate.

    Plates are anonymous children of WorldFrame, so the only tell is the border texture,
    which nothing else in the default UI uses.

    Every region is checked rather than the first. This used to read only
    `frame:GetRegions()`, which returns region one - and on a 3.3.5a plate that is the threat
    glow, not the border. It therefore matched nothing, ever. The failure was quiet and
    misleading: forcing plates on is a CVar and needs no detection, so plates dutifully
    appeared while the indicator that depends on this never drew.
]]
local function IsNamePlate(frame)
    if frame:GetName() then
        return false
    end

    -- Plates always have at least the health bar as a child.
    if frame:GetNumChildren() < 1 then
        return false
    end

    for _, region in ipairs({ frame:GetRegions() }) do
        if region.GetObjectType and region:GetObjectType() == "Texture" and region.GetTexture then
            local texture = region:GetTexture()

            -- Any nameplate art at all, case-insensitively. Matching one exact path has now
            -- failed twice: first by reading only region one, which is the threat glow and
            -- not the border, and then because the casing and spelling of these paths is
            -- not worth betting on across builds and patched clients.
            if type(texture) == "string" and string.find(string.lower(texture), "nameplate") then
                return true
            end
        end
    end

    return false
end

local function PlateName(plate)
    for _, region in ipairs({ plate:GetRegions() }) do
        if region.GetObjectType and region:GetObjectType() == "FontString" then
            local text = region:GetText()
            if text and not tonumber(text) then
                return text
            end
        end
    end

    return nil
end

--[[
    The speaking indicator: three rising bars, like a level meter.

    Built from WHITE8X8 and tinted rather than from a piece of Blizzard art. Every plausible
    icon path - the voice chat speaker, the chat bubble, the state icon - is a guess at what
    this client's MPQ actually holds, and a missing texture draws nothing at all: no error,
    no warning, just an indicator that never appears. WHITE8X8 is used by half the addons
    ever written and is certainly present.

    Anchored above the plate rather than inside it, so it cannot end up behind the name.
]]
local BAR_COUNT = 3

-- The group's full width: three 4-unit bars with 2-unit gaps.
local INDICATOR_WIDTH = (BAR_COUNT * 6) - 2

--[[
    Where the bars sit relative to the health bar, in UI units.

    The box is anchored by its bottom-CENTRE to the health bar's top-centre, and it still
    drew a touch to the left - by what looked like half its own width, which is the
    signature of something along the way treating that anchor as a left edge. Shifting
    right by exactly half the width is the correction for that; it was arrived at by
    eye, so if it ever looks wrong again this is the number to move, not the anchor.
]]
local INDICATOR_X_NUDGE = INDICATOR_WIDTH / 2
local INDICATOR_HEIGHT = 26

--[[
    NotPlater, when it is there: the indicator becomes one of its elements.

    Its settings live in NotPlater's profile beside every other element's, so they follow
    profile switches and profile sharing; its section is built from NotPlater's own position
    and size prototypes, so the controls are the ones the player already knows; and a change
    made in the window lays every indicator out again through NotPlater's own Reload, the
    same path the rest of the plate takes.

    None of it touches NotPlater's files. The section is injected into the options table at
    the moment NotPlater registers it - which happens lazily, the first time the window is
    opened - so this works whichever addon loaded first and survives NotPlater being updated
    underneath it. Without NotPlater, none of this runs and the fixed offsets below apply.
]]
local layoutStamp = 0      -- bumped on Reload, so every holder lays itself out again once

local DEFAULT_COLOR = { 1.0, 0.82, 0.35, 1.0 }

local function IndicatorConfig()
    return NotPlater and NotPlater.db and NotPlater.db.profile
        and NotPlater.db.profile.speakingIndicator or nil
end

local function IndicatorEnabled()
    local cfg = IndicatorConfig()
    return not cfg or cfg.general.enable ~= false
end

-- Filled in rather than declared to AceDB: the key is not in NotPlater's own defaults, so a
-- fresh profile has none of it, and a profile switched to mid-session may not either.
local function EnsureIndicatorDefaults()
    local profile = NotPlater.db.profile
    profile.speakingIndicator = profile.speakingIndicator or {}
    local cfg = profile.speakingIndicator

    cfg.general = cfg.general or {}
    if cfg.general.enable == nil then cfg.general.enable = true end
    cfg.general.color = cfg.general.color or { DEFAULT_COLOR[1], DEFAULT_COLOR[2], DEFAULT_COLOR[3], DEFAULT_COLOR[4] }

    -- Same-point anchoring, because that is NotPlater's convention for every element: the
    -- holder's TOP goes to the target's TOP plus the offsets. 41 lands the bars where the
    -- fixed layout put them.
    cfg.position = cfg.position or {}
    cfg.position.anchorTarget = cfg.position.anchorTarget or "healthBar"
    cfg.position.anchor = cfg.position.anchor or "TOP"
    cfg.position.xOffset = cfg.position.xOffset or 0
    cfg.position.yOffset = cfg.position.yOffset or 41

    -- Above the auras when there are any, so the bars never sit on top of a buff icon.
    -- The gap is measured from the top of the topmost shown aura row; the offsets above
    -- are not reused there, because they describe a distance from the health bar.
    if cfg.position.riseAboveAuras == nil then cfg.position.riseAboveAuras = true end
    cfg.position.auraGap = cfg.position.auraGap or 4

    cfg.size = cfg.size or {}
    cfg.size.width = cfg.size.width or 4
    cfg.size.height = cfg.size.height or 15
    cfg.size.spacing = cfg.size.spacing or 2
end

-- The same get/set NotPlater's own sections use: the option's path IS the path into the
-- profile, and a table value (a colour) is unpacked on the way out and rebuilt on the way in.
local function GetIndicatorValue(info)
    local node = NotPlater.db.profile
    for i = 1, #info do
        node = node[info[i]]
    end
    if type(node) == "table" then
        return unpack(node)
    end
    return node
end

local function SetIndicatorValue(info, ...)
    local node = NotPlater.db.profile
    for i = 1, #info - 1 do
        node = node[info[i]]
    end
    local values = { ... }
    node[info[#info]] = #values > 1 and values or (...)
    NotPlater:Reload()
end

local function BuildIndicatorOptions()
    local proto = NotPlater.ConfigPrototypes

    local position = proto:GetGeneralisedPositionConfig()
    position.order = 1
    position.args.riseAboveAuras = {
        order = 4,
        type = "toggle",
        name = "Rise Above Auras",
        desc = "When buffs or debuffs are shown on the plate, sit above them instead of where the anchor puts you.",
    }
    position.args.auraGap = {
        order = 5,
        type = "range",
        name = "Aura Gap",
        desc = "Distance above the topmost aura row while rising above auras.",
        min = 0, max = 40, step = 1,
    }

    local size = proto:GetGeneralisedSizeConfig()
    size.order = 2
    size.args.spacing = { order = 2, type = "range", name = "Spacing", min = 0, max = 20, step = 1 }

    return {
        order = 90,
        type = "group",
        name = "Speaking Indicator",
        get = GetIndicatorValue,
        set = SetIndicatorValue,
        args = {
            general = {
                order = 0,
                type = "group",
                inline = true,
                name = "General",
                args = {
                    enable = { order = 0, type = "toggle", name = "Enable" },
                    color = { order = 1, type = "color", name = "Color", hasAlpha = true },
                },
            },
            position = position,
            size = size,
        },
    }
end

local function InjectIndicatorOptions(options)
    if type(options) ~= "table" or type(options.args) ~= "table" or options.args.speakingIndicator then
        return
    end

    options.args.speakingIndicator = BuildIndicatorOptions()
end

--[[
    The shipped look is also NotPlater's DEFAULT, not merely the profile everyone is
    switched to.

    Three things read the defaults rather than a named profile: a brand-new character,
    which AceDB starts on a fresh profile of its own before our switch has run; a profile
    somebody creates from scratch; and "Reset Profile", which returns to the defaults. With
    stock defaults all three land on NotPlater's own look. Registering ours makes every one
    of them land on Sanctuary.

    Merged, not swapped. What NotPlaterProfile.lua carries is only what differed from the
    stock defaults - AceDB saves overrides, nothing else - so registering it alone would
    leave every setting the operator never touched as nil and the addon indexing into
    nothing. The stock table is copied and the shipped values laid over it; the copy is so
    NotPlater's own table is never mutated. Defaults are not persisted, so this runs every
    session, not once per version.
]]
local function RegisterShippedDefaults()
    local shipped = SanctuaryVoiceNotPlaterProfile

    if not shipped or type(shipped.data) ~= "table" or not NotPlater.db.RegisterDefaults then
        return
    end

    -- NotPlater.defaults is the table NotPlater built and handed to AceDB, and nothing
    -- writes to it afterwards - so it is stock however many times this runs. db.defaults
    -- is whatever was registered last, which after the first run is our own merge.
    local stock = NotPlater.defaults or NotPlater.db.defaults

    if type(stock) ~= "table" or type(stock.profile) ~= "table" then
        return
    end

    local function copy(source)
        local out = {}
        for k, v in pairs(source) do
            out[k] = type(v) == "table" and copy(v) or v
        end
        return out
    end

    local function overlay(base, top)
        for k, v in pairs(top) do
            if type(v) == "table" and type(base[k]) == "table" then
                overlay(base[k], v)
            else
                base[k] = type(v) == "table" and copy(v) or v
            end
        end
    end

    local merged = copy(stock)
    overlay(merged.profile, shipped.data)

    NotPlater.db:RegisterDefaults(merged)
end

--[[
    The look everybody gets: a NotPlater profile shipped with this addon.

    NotPlaterProfile.lua (generated by tools/notplater-profile.py from the operator's own
    profile) defines SanctuaryVoiceNotPlaterProfile = { version, name, data }. This does
    what NotPlater's own import does after decoding a share string - writes the table into
    its profiles and switches to it - which is why it needs no sanitising and survives
    NotPlater updates: it goes through NotPlater's database, not around it.

    Per version, per character. SanctuaryVoiceDB is per character, so each one is switched
    on its first login after a push; the profile write itself is idempotent. A version the
    character has already received is left alone, so someone who tweaked a slider is not
    fought with every login - only overwritten by the next push, which is the point of a
    push. Their previous profile is not deleted; it stays in NotPlater's list.
]]
--[[
    Where NotPlater's minimap button sits, as part of the push.

    It is not a profile setting. NotPlater keeps it in NotPlaterDB.minimap, outside every
    profile, and hands that very table to LibDBIcon, which writes the button's angle into
    it as the button is dragged. So a profile push never carried it, and each machine kept
    the button wherever it had last been left.

    Set on that same table - LibDBIcon holds it by reference, and replacing it would leave
    the library writing drags into a table nobody saves - and then redrawn, so the button
    moves now rather than next login. Once per version, like the look: a player who drags
    it somewhere else afterwards keeps it there until the next push.
]]
local function ApplyShippedMinimapButton(shipped)
    local angle = tonumber(shipped.minimapPos)

    if not angle or type(NotPlaterDB) ~= "table" then
        return
    end

    NotPlaterDB.minimap = NotPlaterDB.minimap or { hide = false }
    NotPlaterDB.minimap.minimapPos = angle

    local icon = LibStub and LibStub("LibDBIcon-1.0", true)

    if icon and icon.Refresh and (not icon.IsRegistered or icon:IsRegistered("NotPlater")) then
        icon:Refresh("NotPlater", NotPlaterDB.minimap)
    end
end

local function ApplyShippedNotPlaterProfile()
    local shipped = SanctuaryVoiceNotPlaterProfile

    if not shipped or type(shipped.data) ~= "table" or not shipped.name then
        return
    end

    SanctuaryVoiceDB = SanctuaryVoiceDB or {}

    if SanctuaryVoiceDB.notPlaterProfileVersion == shipped.version then
        return
    end

    local function copy(source)
        local out = {}
        for k, v in pairs(source) do
            out[k] = type(v) == "table" and copy(v) or v
        end
        return out
    end

    -- Defaults first, so the refill below is seated against the look it ships with, and
    -- so a version arriving mid-session (the harness does this) stays coherent.
    RegisterShippedDefaults()

    local profiles = NotPlater.db.profiles

    if type(profiles[shipped.name]) ~= "table" then
        profiles[shipped.name] = {}
    end

    local profile = profiles[shipped.name]

    --[[
        IN PLACE, never by replacing the table.

        Whenever the character already sits on Sanctuary from an earlier session, this
        table is the very one AceDB has bound as db.profile - and SetProfile to the profile
        you are already on is a no-op, so nothing would ever re-bind it. Assigning a fresh
        table here left NotPlater reading and editing an orphan for the whole session:
        displayed, edited, and never saved, while the new table sat in the saved variables
        untouched and was written out in full. That was the bug behind "the defaults keep
        reverting" - and it quietly lost every edit made in such a session.
    ]]
    for key in pairs(profile) do
        profile[key] = nil
    end

    for key, value in pairs(copy(shipped.data)) do
        profile[key] = value
    end

    -- Re-seat the defaults on the refilled table through the public API (it strips and
    -- re-copies them on the active sections), then switch - a no-op if already on it -
    -- and reload either way, because a no-op switch fires nothing.
    NotPlater.db:RegisterDefaults(NotPlater.db.defaults)
    NotPlater.db:SetProfile(shipped.name)
    NotPlater:Reload()

    -- The Reload hook re-seeds our settings and re-lays the bars out; done here as well
    -- so the indicator is whole the instant the look changes, whatever fires.
    EnsureIndicatorDefaults()
    layoutStamp = layoutStamp + 1

    ApplyShippedMinimapButton(shipped)

    SanctuaryVoiceDB.notPlaterProfileVersion = shipped.version

    DEFAULT_CHAT_FRAME:AddMessage(string.format(
        "|cff00ff96Voice:|r nameplates set to the %s look (version %s).",
        shipped.name, tostring(shipped.version)))
end

local notPlaterIntegrated = false

local function TryIntegrateNotPlater()
    if notPlaterIntegrated then
        return
    end

    if not (NotPlater and NotPlater.db and NotPlater.db.profile and NotPlater.ConfigPrototypes) then
        return
    end

    notPlaterIntegrated = true

    --[[
        The shipped look first, then our own fallbacks - never the other way round.

        A logout drops every setting equal to the registered defaults, so a character
        comes back with the shipped values missing and they only reappear when the
        shipped look is registered again. EnsureIndicatorDefaults fills in whatever is
        missing with its own values; run first, it found the shipped indicator settings
        gone and wrote its 41 and 4 in their place, as real settings that the defaults
        registered a moment later could not override. Every push that moved the bars
        was undone on each character's second login - which is why two machines showed
        them in two places, depending on how often each had logged in since the push.
    ]]
    RegisterShippedDefaults()
    EnsureIndicatorDefaults()

    -- Every change in the window ends in Reload, and so does a profile switch. The
    -- switched-to profile may not carry our key yet, which is why the defaults are
    -- ensured here and not only at login.
    hooksecurefunc(NotPlater, "Reload", function()
        EnsureIndicatorDefaults()
        layoutStamp = layoutStamp + 1
    end)

    local registry = LibStub and LibStub("AceConfigRegistry-3.0", true)

    if not registry then
        return
    end

    -- Both orders. If the window was opened before we got here the table already exists;
    -- otherwise it appears when NotPlater registers it, and the hook catches that.
    local existing = registry:GetOptionsTable("NotPlater", "dialog", "SanctuaryVoice")

    if existing then
        InjectIndicatorOptions(existing)
        registry:NotifyChange("NotPlater")
    end

    hooksecurefunc(registry, "RegisterOptionsTable", function(_, appName, options)
        if appName == "NotPlater" then
            InjectIndicatorOptions(options)
        end
    end)
end

--[[
    Which bar the indicator sits over.

    Off the health bar rather than the plate: the plate *frame* is taller than the art
    inside it, so anchoring to the frame puts the bars somewhere above the part of the
    plate the player is actually looking at.

    Which health bar is the question. NotPlater hides the Blizzard one and draws its own
    StatusBar on the same plate, kept on the plate frame as `healthBar` - and a hidden
    frame still has a position, so anchoring to the first child put the bars over a bar
    nobody could see, at whatever offset the two happen to differ by. The replacement is
    preferred when it exists; a plain Blizzard plate has no such field and falls through.

    Re-checked on every scan rather than chosen once, because NotPlater prepares a plate on
    its own schedule. Build the indicator first and the replacement bar is not there yet;
    the anchor has to move to it when it appears, and the comparison makes that free.
]]
--[[
    The aura container to rise above, or nil when there is nothing shown to rise above.

    NotPlater keeps two containers on a plate - debuffs and buffs - and hides each when it
    is empty. Hidden is the only honest signal: a hidden container keeps its last size,
    so anchoring to it statically would leave the bars floating above where auras used
    to be. Buffs stack above debuffs by default, but either can be re-anchored in
    NotPlater's own settings, so the topmost is chosen by its actual top edge rather than
    by index.
]]
local function TopmostShownAuraContainer(plate)
    local containers = plate.npAuras and plate.npAuras.frames

    if not containers then
        return nil
    end

    local best

    for i = 1, #containers do
        local container = containers[i]

        if container and container:IsShown() then
            if not best or (container:GetTop() or 0) > (best:GetTop() or 0) then
                best = container
            end
        end
    end

    return best
end

local function LayoutIndicator(plate, holder)
    local cfg = IndicatorConfig()
    local target
    local aboveAuras = false

    if cfg then
        -- NotPlater resolves the chosen target - health bar, name text, cast bar, any of
        -- its elements - from the plate the holder sits on. Its own health bar is the
        -- fallback for a target that has not been built on this plate yet.
        target = NotPlater:GetAnchorTargetFrame(holder, cfg.position.anchorTarget, plate.healthBar)
            or plate.healthBar or plate:GetChildren()

        -- Auras take precedence over the chosen target while any are shown. This runs
        -- every scan, so the bars step up when a buff lands and back down when it fades.
        if cfg.position.riseAboveAuras ~= false then
            local container = TopmostShownAuraContainer(plate)

            if container then
                target = container
                aboveAuras = true
            end
        end
    else
        target = plate.healthBar or plate:GetChildren()
    end

    if holder.anchoredTo == (target or plate) and holder.layoutStamp == layoutStamp
        and holder.aboveAuras == aboveAuras then
        return
    end

    local width = cfg and cfg.size.width or 4
    local height = cfg and cfg.size.height or 15
    local gap = cfg and cfg.size.spacing or 2
    local color = cfg and cfg.general.color or DEFAULT_COLOR

    holder:SetWidth((BAR_COUNT * (width + gap)) - gap)
    holder:SetHeight(height)
    holder.barMax = height

    for i, bar in ipairs(holder.bars) do
        bar:SetWidth(width)
        bar:ClearAllPoints()
        bar:SetPoint("BOTTOMLEFT", holder, "BOTTOMLEFT", (i - 1) * (width + gap), 0)
        bar:SetVertexColor(color[1], color[2], color[3], color[4] or 1)
    end

    holder:ClearAllPoints()

    if aboveAuras then
        -- Bottom of the bars to the top of the aura row: the configured anchor and Y
        -- offset describe a distance from the health bar and would be wrong up here.
        holder:SetPoint("BOTTOM", target, "TOP", cfg.position.xOffset, cfg.position.auraGap)
    elseif cfg and target then
        holder:SetPoint(cfg.position.anchor, target, cfg.position.anchor, cfg.position.xOffset, cfg.position.yOffset)
    elseif target then
        holder:SetPoint("BOTTOM", target, "TOP", INDICATOR_X_NUDGE, INDICATOR_HEIGHT)
    else
        -- Only reached on a plate with no health bar child, which a real 3.3.5a
        -- nameplate never is. Kept so a malformed plate still gets an indicator
        -- somewhere rather than a nil anchor, but do not tune it expecting to see it.
        holder:SetPoint("BOTTOM", plate, "TOP", 0, 8)
    end

    holder.anchoredTo = target or plate
    holder.layoutStamp = layoutStamp
    holder.aboveAuras = aboveAuras
end

local function PlateIcon(plate)
    if plateIcons[plate] then
        LayoutIndicator(plate, plateIcons[plate])
        return plateIcons[plate]
    end

    -- A frame rather than loose textures, so the bars show and hide as one thing.
    local holder = CreateFrame("Frame", nil, plate)
    holder:SetWidth(INDICATOR_WIDTH)
    holder:SetHeight(16)

    holder.bars = {}

    for i = 1, BAR_COUNT do
        local bar = holder:CreateTexture(nil, "OVERLAY")
        bar:SetTexture("Interface\\Buttons\\WHITE8X8")
        holder.bars[i] = bar
    end

    -- Sized, coloured and placed once the bars exist: the layout owns all three, whether
    -- they come from NotPlater's profile or from the fixed values.
    LayoutIndicator(plate, holder)

    holder:Hide()

    plateIcons[plate] = holder
    return holder
end

--- Phase of the bar animation, advanced by the scan tick that is already running.
local barPhase = 0

--- Gives the bars a life of their own, so a speaking plate reads as active at a glance.
local function AnimateBars(holder)
    for i = 1, BAR_COUNT do
        --[[
            Symmetric about the middle bar, so the shape is always centred on its box.

            The wave used to travel across the bars, each a step behind the last. The
            box was exactly over the health bar the whole time, but a group whose left
            bar is tall while its right bar is short LEANS, and the eye centres on the
            mass rather than the bounds - so it read as sitting off to one side. Driving
            the outer bars together, with the middle one leading, keeps the silhouette
            balanced in every frame of the animation.
        ]]
        local distance = math.abs(i - ((BAR_COUNT + 1) / 2))
        local wave = math.sin(barPhase - (distance * 1.2))

        -- Between forty percent of the configured height and all of it; at the fixed
        -- default of 15 that is the same 6 to 15 sweep as before.
        local top = holder.barMax or 15
        local bottom = top * 0.4
        holder.bars[i]:SetHeight(bottom + math.floor((top - bottom) * ((wave + 1) / 2)))
    end
end

--[[
    Forcing plates on while someone talks.

    3.3.5a has no per-unit nameplate control, so this is all-or-nothing: the two
    CVars below are global. While anyone within earshot is speaking, everyone's
    plates appear, and the speaker is the one wearing the icon.

    The player's own setting is borrowed, not taken. It is restored a few seconds
    after the last voice stops, and only if it still holds the value we wrote -
    if they changed it themselves mid-conversation, theirs wins.
]]

local NAMEPLATE_CVARS = { "nameplateShowFriends", "nameplateShowEnemies" }

-- How long plates stay up after the last person stops. Without a linger, the
-- pauses between sentences would strobe the setting.
--[[
    How long the borrowed nameplate setting is held after the last person stops.

    Generous on purpose, and it costs nothing to look at: every plate except the speaker's
    is dimmed to alpha zero, and the speaker's own is dimmed as soon as their linger runs
    out. So a few seconds after the last word there is nothing on screen either way, and
    this only decides when the CVar quietly goes back.

    Holding it is what stops the thrash. Turning the setting off and on again between
    sentences makes the client destroy and rebuild every plate, and each rebuild is a fresh
    chance for one to flash before it can be dimmed.
]]
local PLATE_LINGER = 6.0

local plateOverride = {
    active = false,
    -- Whether the plates on screen are there because of us, rather than the player.
    borrowed = false,
    previous = {},
    wrote = {},
    quietFor = 0,
}

local function ForcingEnabled()
    -- Default on. SanctuaryVoiceDB does not exist until PLAYER_LOGIN, so a nil
    -- table here means "not configured yet", not "disabled".
    return not SanctuaryVoiceDB or SanctuaryVoiceDB.forcePlates ~= false
end

local function ForceNameplatesOn()
    if plateOverride.active or not ForcingEnabled() then
        return
    end

    plateOverride.borrowed = false

    for _, cvar in ipairs(NAMEPLATE_CVARS) do
        local before = GetCVar(cvar)
        plateOverride.previous[cvar] = before

        -- Whether this is our doing at all. A player who already runs with plates on has
        -- chosen to see plates, and hiding everyone but the speaker would be taking that
        -- away; the indicator just appears on the plate that is already there.
        if before == "0" then
            plateOverride.borrowed = true
        end

        SetCVar(cvar, "1")
        -- Read back rather than assuming "1": the client normalises some values,
        -- and the comparison on restore has to be against what actually landed.
        plateOverride.wrote[cvar] = GetCVar(cvar)
    end

    plateOverride.active = true
end

-- Defined below, once the tables it works on exist. Declared here so RestoreNameplates can
-- close over it: a local created later would be a different variable.
local RevealHiddenPlates

local function RestoreNameplates()
    if not plateOverride.active then
        return
    end

    for _, cvar in ipairs(NAMEPLATE_CVARS) do
        local previous = plateOverride.previous[cvar]

        -- Only put back what we changed. If it no longer reads as the value we
        -- wrote, the player has since set it themselves and we leave it be.
        if previous and GetCVar(cvar) == plateOverride.wrote[cvar] then
            SetCVar(cvar, previous)
        end
    end

    plateOverride.active = false
    plateOverride.borrowed = false
    plateOverride.previous = {}
    plateOverride.wrote = {}

    -- Give back anything the speaker-only filter hid, or plates would stay invisible for
    -- players who had them switched on all along.
    if RevealHiddenPlates then
        RevealHiddenPlates()
    end
end

local function AnyoneSpeaking()
    local now = GetTime()

    for _, expires in pairs(speakingUntil) do
        if now < expires then
            return true
        end
    end

    return false
end

--- Drops entries whose linger has run out. Cheap, and keeps the table from growing.
local function PruneSpeakers()
    local now = GetTime()

    for name, expires in pairs(speakingUntil) do
        if now >= expires then
            speakingUntil[name] = nil
        end
    end

    for name, expires in pairs(combatUntil) do
        if now >= expires then
            combatUntil[name] = nil
        end
    end

    for name, expires in pairs(visibleUntil) do
        if now >= expires then
            visibleUntil[name] = nil
        end
    end
end

--[[
    Showing only the speaker.

    3.3.5a has no per-unit nameplate control - the two CVars are global, so asking the client
    for one plate is not possible. What *is* possible is the other direction: a plate is an
    ordinary frame, so the ones we do not want can be hidden.

    That only applies while the setting is borrowed. If the player runs with plates on because
    they want to see plates, hiding them mid-conversation would be taking something away they
    chose; in that case the indicator simply appears on the plate that is already there.
]]

--[[
    Dimming, not hiding.

    Hiding a plate is a one-way door. Plate frames are pooled and the client tracks which of
    them are shown, so a plate *we* hid is one it still believes is on screen and will never
    Show again - there is no way back. And calling Show ourselves on one the client has since
    retired resurrects a dead plate that then sits there with a stale name forever.

    Alpha has neither problem. The client keeps complete control of Show and Hide, so nothing
    can desynchronise and a recycled plate simply stops being ours. The cost is that the
    client writes alpha too, for its own target dimming, so ours is reasserted every frame
    rather than once per scan.
]]

-- Plates this addon is dimming, so it hands back only what it took.
local plateDimmed = {}

-- What the last scan found: plate -> whether that plate's unit is speaking.
local plateSpeaking = {}

local function ShouldFilter()
    return plateOverride.active and plateOverride.borrowed
end

-- frame -> whether it is a nameplate. Cached because deciding costs a walk over every
-- region, and this runs every frame. Plates are pooled, so it converges within a second.
local plateChecked = {}

--[[
    Catches plates the scan has not seen yet, and dims them on sight.

    Without this, a plate the client has only just created is absent from plateSpeaking and
    so renders at full brightness until the next scan classifies it - six frames at sixty
    fps. That is the jitter: every time nameplates come on, or anyone walks into range,
    their plate flashes fully visible before being dimmed.

    Defaulting a new plate to dimmed is the right way round. Nobody is the speaker until the
    scan says so, and the speaker's own plate is dimmed for at most one scan before it is
    revealed - far less noticeable than everybody else flashing.
]]
--[[
    Guarded on the child count, because this runs on EVERY rendered frame.

    `{ WorldFrame:GetChildren() }` builds a fresh table of every child each time it is
    called. While somebody is speaking the addon has forced nameplates on, so that set is
    at its largest exactly when this is at its most frequent - and hovering the mouse over
    NPCs churns it, because a mouseover forces a plate of its own. The result was a stall
    in the client big enough to starve the launcher's audio thread in another process
    entirely, heard at the far end as stuttering.

    The count is a safe trigger. 3.3.5a pools plate frames: children are added to
    WorldFrame and then reused forever, never removed, so the count only ever rises. If it
    has not risen there is no child this has not already seen, and both tables below are
    write-only - nothing clears them - so there is nothing to redo.
]]
local lastPlateChildCount = 0

local function ClassifyNewPlates()
    if not ShouldFilter() then
        return
    end

    local children = WorldFrame:GetNumChildren()

    if children == lastPlateChildCount then
        return
    end

    lastPlateChildCount = children

    for _, frame in ipairs({ WorldFrame:GetChildren() }) do
        if plateChecked[frame] == nil then
            plateChecked[frame] = IsNamePlate(frame)
        end

        if plateChecked[frame] and plateSpeaking[frame] == nil then
            plateSpeaking[frame] = false
        end
    end
end

--- Reasserts alpha on every plate the scan knows about. Cheap, and runs every frame.
local function ApplyPlateAlpha()
    if not ShouldFilter() then
        return
    end

    for frame, speaking in pairs(plateSpeaking) do
        if speaking then
            if plateDimmed[frame] then
                plateDimmed[frame] = nil
                frame:SetAlpha(1)
            end
        else
            plateDimmed[frame] = true
            frame:SetAlpha(0)
        end
    end
end

--- Records what the scan saw. Visibility itself is applied by ApplyPlateAlpha.
local function ApplyPlateFilter(frame, speaking)
    plateSpeaking[frame] = speaking and true or false
end

--- Hands back the alpha this addon took. Safe to call at any time, and reversible.
RevealHiddenPlates = function()
    for frame in pairs(plateDimmed) do
        frame:SetAlpha(1)
    end

    plateDimmed = {}
    plateSpeaking = {}
end

--[[
    A plate the player is fighting is never taken away mid-fight.

    Two things remove a plate once its owner stops talking: the speaker-only filter dims it
    to nothing after the speaker linger, and the borrowed CVar goes back after the plate
    linger, which destroys every plate at once. Both are right in a conversation and wrong
    in a fight - the plate of the thing hitting the player is the last one they want to
    lose, and it was only ever on screen because its owner happened to speak.

    So while the player is in combat, their current target's plate counts as visible
    whether or not it is talking, and the setting is not handed back. Both resume on the
    first quiet scan after combat ends; the player's own choice is deferred, not abandoned.

    The target rather than "everyone" on purpose. Revealing every plate in combat would be
    taking something from a player who runs with plates off, and the target is the plate
    the fight is actually about.
]]
local function InCombat()
    return UnitAffectingCombat and UnitAffectingCombat("player") or false
end

local function IsCombatTargetName(name)
    if not name or not InCombat() then
        return false
    end

    return UnitName("target") == name
end

local plateScanner = CreateFrame("Frame")
local sinceScan = 0

plateScanner:SetScript("OnUpdate", function(self, elapsed)
    -- Every frame, before the rate limit. The client writes plate alpha for its own target
    -- dimming, and creates plates at any moment, so both of these have to keep up with it
    -- rather than run on the scan's schedule.
    ClassifyNewPlates()
    ApplyPlateAlpha()

    sinceScan = sinceScan + elapsed

    if sinceScan < 0.1 then
        return
    end

    local slice = sinceScan
    sinceScan = 0

    barPhase = (barPhase + (slice * 6)) % (math.pi * 2)

    PruneSpeakers()

    -- A held plate wants the setting as much as a speaking one does: handing the CVar
    -- back would destroy it along with everything else. And a hold can arrive on its own,
    -- for a recent speaker who has just entered combat, so it can bring plates back too.
    if AnyoneSpeaking() or AnyoneRecentlyHeard() or AnyHeldForCombat() then
        plateOverride.quietFor = 0
        ForceNameplatesOn()
    elseif plateOverride.active then
        plateOverride.quietFor = plateOverride.quietFor + slice

        -- Not mid-fight: restoring the CVar destroys every plate at once, the one on the
        -- thing currently hitting the player included. It goes back on the first quiet
        -- scan after combat ends instead, quietFor having kept counting all the while.
        if plateOverride.quietFor >= PLATE_LINGER and not InCombat() then
            RestoreNameplates()
        end
    end

    for _, frame in ipairs({ WorldFrame:GetChildren() }) do
        if IsNamePlate(frame) then
            local name = PlateName(frame)
            local speaking = IsSpeakingName(name)
            local icon = PlateIcon(frame)

            if speaking and frame:IsShown() and IndicatorEnabled() then
                AnimateBars(icon)
                icon:Show()
            else
                icon:Hide()
            end

            -- Read whether it is shown *after* the filter has had its say, or a plate we
            -- hid a moment ago would be treated as absent.
            --
            -- Visibility is wider than the indicator. The player's combat target, and any
            -- speaker the server says is fighting, stay visible whether or not they are
            -- talking - but only wear the bars while they are.
            ApplyPlateFilter(frame, speaking or IsRecentlyHeardName(name)
                or IsCombatTargetName(name) or IsHeldForCombat(name))
        end
    end

    --[[
        NotPlater's simulator: the preview plate in its config window.

        Not a real plate - it is named, and it hangs off the simulator rather than
        WorldFrame - so the scan above rightly never sees it, and nobody is ever speaking
        on it. It gets an indicator of its own, lit for as long as the simulator is shown,
        so a change made in the window is seen on the preview the way every other
        element's is. The layout is the same one real plates use, read from the same
        profile, which is the whole point of previewing it.
    ]]
    local simulator = NotPlater and NotPlater.simulatorFrame

    if simulator and simulator.defaultFrame then
        local icon = PlateIcon(simulator.defaultFrame)

        if simulator:IsShown() and IndicatorEnabled() then
            AnimateBars(icon)
            icon:Show()
        else
            icon:Hide()
        end
    end
end)

local function HandleSpeaking(body)
    -- "SPK <guid> <0|1> <name with spaces>"
    local guid, on, name = string.match(body, "^(%d+)%s+([01])%s+(.+)$")
    if not guid then
        return
    end

    if on == "1" then
        speakers[guid] = { name = name, since = GetTime() }
        speakingUntil[name] = GetTime() + SPEAKER_MAX
        visibleUntil[name] = GetTime() + SPEAKER_MAX
    else
        local previous = speakers[guid]

        if previous then
            -- Lingers rather than clearing. This message arrives after 300ms of silence,
            -- which is every pause for breath, not the end of the conversation.
            speakingUntil[previous.name] = GetTime() + SPEAKER_LINGER
            visibleUntil[previous.name] = GetTime() + VISIBLE_LINGER
        end

        speakers[guid] = nil
    end

    Refresh()
end

local function HandleCombat(body)
    -- "CMB <guid> <0|1> <name with spaces>" - the same shape as SPK, for the same reason:
    -- the name is the only handle the addon has on a plate, and it comes last so it may
    -- contain anything.
    local guid, fighting, name = string.match(body, "^(%d+)%s+([01])%s+(.+)$")
    if not guid then
        return
    end

    if fighting == "1" then
        combatUntil[name] = GetTime() + COMBAT_HOLD_MAX
    else
        combatUntil[name] = nil
    end
end

--------------------------------------------------------------------------
-- Events
--------------------------------------------------------------------------

local events = CreateFrame("Frame")
--------------------------------------------------------------------------
-- Asking the server for our own state
--------------------------------------------------------------------------

--[[
    Everything the HUD shows is pushed by the server at moments it chooses, and
    the addon has no way to know whether it caught them. At login the client is
    usually still on the loading screen; after a /reload the addon has nothing at
    all; and a voice client attaching mid-login used to be announced before this
    addon could hear it. The symptom was a HUD stuck on "client off" until
    something forced a fresh push.

    So the addon asks. It keeps asking until it gets an answer, and asks again
    now and then while no voice client is attached, which self-heals anything
    missed without the player doing anything.
]]

local haveConfig = false

local function RequestSync()
    SendAddonMessage(ADDON_PREFIX, "SYNC", "WHISPER", UnitName("player"))
end

local syncTimer = CreateFrame("Frame")
local sinceSync = 0

syncTimer:SetScript("OnUpdate", function(self, elapsed)
    sinceSync = sinceSync + elapsed

    -- Insistent until answered, then an occasional check while nothing is
    -- attached. Once a client is connected there is nothing left to discover.
    local interval = haveConfig and 15 or 2

    if sinceSync < interval then
        return
    end

    sinceSync = 0

    if haveConfig and state.connected then
        return
    end

    RequestSync()
end)

events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("PLAYER_LOGIN")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
-- Both, because a /reload fires LEAVING_WORLD but not LOGOUT. Leaving the CVars
-- forced would silently change a setting the player never chose.
events:RegisterEvent("PLAYER_LOGOUT")
events:RegisterEvent("PLAYER_LEAVING_WORLD")

events:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "PLAYER_LOGOUT" or event == "PLAYER_LEAVING_WORLD" then
        RestoreNameplates()
        return
    end

    if event == "PLAYER_LOGIN" then
        SanctuaryVoiceDB = SanctuaryVoiceDB or {}
        TryIntegrateNotPlater()
        if SanctuaryVoiceDB.point then
            hud:ClearAllPoints()
            hud:SetPoint(SanctuaryVoiceDB.point, UIParent, SanctuaryVoiceDB.relativePoint,
                SanctuaryVoiceDB.x, SanctuaryVoiceDB.y)
        end

        -- No hidden check here any more. `/svoice show|hide` is gone, so a saved hidden=true
        -- would leave the HUD invisible with nothing left to bring it back.

        Refresh()
        return
    end

    if event == "PLAYER_ENTERING_WORLD" then
        -- Covers a fresh login and a /reload alike: in both, this fires once the
        -- client is genuinely ready to receive.
        TryIntegrateNotPlater()

        -- Here rather than at PLAYER_LOGIN: switching profiles makes NotPlater reload
        -- every plate, and at login it may not have finished enabling itself yet.
        if notPlaterIntegrated then
            ApplyShippedNotPlaterProfile()
        end

        haveConfig = false
        sinceSync = 0
        RequestSync()
        return
    end

    if event == "CHAT_MSG_ADDON" and arg1 == ADDON_PREFIX then
        local verb, body = string.match(arg2, "^(%u+)%s*(.*)$")

        -- `/svoice trace` prints every speaking and combat message as it lands. Whether a
        -- plate should be held is decided entirely by these, so when one is not, this is
        -- the fastest way to see whether the message never came or came with a name that
        -- matches no plate.
        if SanctuaryVoiceDB and SanctuaryVoiceDB.trace and (verb == "SPK" or verb == "CMB") then
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Voice trace:|r " .. verb .. " " .. tostring(body))
        end

        if verb == "CFG" then
            haveConfig = true
            HandleConfig(body)
        elseif verb == "SPK" then
            HandleSpeaking(body)
        elseif verb == "CMB" then
            HandleCombat(body)
        end
    end
end)

-- A speaker whose "stopped" message went missing would otherwise stay listed
-- forever, so entries expire on their own.
local ticker = CreateFrame("Frame")
local elapsedSinceSweep = 0
ticker:SetScript("OnUpdate", function(self, elapsed)
    elapsedSinceSweep = elapsedSinceSweep + elapsed
    if elapsedSinceSweep < 0.5 then
        return
    end

    elapsedSinceSweep = 0
    local now = GetTime()
    local changed = false

    -- Only forgets people whose linger has already run out. This used to drop anyone
    -- three seconds after they *started*, which cut the indicator off part way through
    -- any sentence longer than that - the relay reports a start once and never repeats it.
    for guid, speaker in pairs(speakers) do
        if not IsSpeakingName(speaker.name) and now - speaker.since > SPEAKER_MAX then
            speakers[guid] = nil
            changed = true
        end
    end

    if changed then
        Refresh()
    end
end)

--------------------------------------------------------------------------
-- Slash commands
--------------------------------------------------------------------------

-- Commands go out over AzerothCore's addon command channel rather than as a
-- chat line starting with a dot. If the command is not recognised, a dot
-- command falls through and is published to /say; this channel just goes
-- nowhere. The counter is echoed back by the server to match up replies.
local commandCounter = 0

local function SendServerCommand(command)
    commandCounter = (commandCounter + 1) % 10000
    SendAddonMessage("AzerothCore",
        string.format("i%04d%s", commandCounter, command),
        "WHISPER",
        UnitName("player"))
end

--------------------------------------------------------------------------
-- Speaking language
--------------------------------------------------------------------------

--[[
    Which language your voice is carried in.

    With the barrier enforced, a listener who has not learned it hears noise - the same
    rule /say has always followed. The server tells the HUD what you speak and what you
    know on every CFG push; this is the only place a player can change it, because the
    typed `.voice lang` is a game master's command under the GUI-only policy and the
    launcher has no picker of its own.

    The table mirrors PVLanguages.cpp: the number is how the world server names the
    language, the key is what `.voice lang` accepts, the name is what a player sees.
]]
local LANGUAGES = {
    { id = 0,  key = "universal",  name = "Universal"  },
    { id = 1,  key = "orcish",     name = "Orcish"     },
    { id = 2,  key = "darnassian", name = "Darnassian" },
    { id = 3,  key = "taurahe",    name = "Taurahe"    },
    { id = 6,  key = "dwarvish",   name = "Dwarvish"   },
    { id = 7,  key = "common",     name = "Common"     },
    { id = 8,  key = "demonic",    name = "Demonic"    },
    { id = 9,  key = "titan",      name = "Titan"      },
    { id = 10, key = "thalassian", name = "Thalassian" },
    { id = 11, key = "draconic",   name = "Draconic"   },
    { id = 12, key = "kalimag",    name = "Kalimag"    },
    { id = 13, key = "gnomish",    name = "Gnomish"    },
    { id = 14, key = "troll",      name = "Troll"      },
}

local function LanguageById(id)
    for _, language in ipairs(LANGUAGES) do
        if language.id == id then
            return language
        end
    end
end

-- "Orcish", or "Orcish (40%)" for a language only partly known.
local function LanguageLabel(language)
    local known = state.proficiency and state.proficiency[language.id] or 100

    if known < 100 then
        return string.format("%s (%d%%)", language.name, known)
    end

    return language.name
end

--[[
    The row: "Speaking:" and a button reading the language, opening the list on a click.

    The button is the same template, height and right-hand edge as the range presets
    under it, so the two rows line up. The first version showed Blizzard's dropdown box
    here, and it did not fit: its art is taller than anything else in this window and
    carries a wide strip of invisible padding either side, so a box placed by its frame
    lands somewhere else - it ran into the window's border and lined up with nothing.
    The dropdown is still what holds the list, because its list is the one players know;
    it is simply never shown itself.
]]
local LANGUAGE_ROW_Y = -38      -- the row's middle, from the window's top edge

local languageLabel = hud:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
languageLabel:SetPoint("LEFT", hud, "TOPLEFT", 12, LANGUAGE_ROW_Y)
languageLabel:SetText("Speaking:")

-- Named, because the template builds its parts as $parentButton and friends.
local languageMenu = CreateFrame("Frame", "SanctuaryVoiceLanguageMenu", hud, "UIDropDownMenuTemplate")
languageMenu:SetPoint("TOPLEFT", hud, "TOPLEFT", 0, 0)
languageMenu:Hide()

-- Right edge 9 in from the window's, as the last range preset's is: 9 + 4 x 32 + 30 = 167
-- of 176. Wide enough for the longest language name in the button's font.
local languageButton = CreateFrame("Button", "SanctuaryVoiceLanguageButton", hud, "UIPanelButtonTemplate")
languageButton:SetWidth(100)
languageButton:SetHeight(18)
languageButton:SetPoint("RIGHT", hud, "TOPRIGHT", -9, LANGUAGE_ROW_Y)

languageButton:SetScript("OnClick", function(self)
    -- The list opens under the button, as a dropdown's would under its box.
    ToggleDropDownMenu(1, nil, languageMenu, self, 0, 0)
end)

languageButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_TOP")
    GameTooltip:SetText("The language your voice is carried in")
    GameTooltip:AddLine("Listeners who have not learned it hear you muffled.", 1, 1, 1, true)
    GameTooltip:Show()
end)

languageButton:SetScript("OnLeave", function()
    GameTooltip:Hide()
end)

local function ChooseLanguage(self)
    -- Only ever a request, like a range preset: the server checks you know it and
    -- answers with a fresh CFG, which is what the picker then shows.
    SendServerCommand("voice lang " .. self.value)
end

UIDropDownMenu_Initialize(languageMenu, function(self, level)
    for _, id in ipairs(state.languages or {}) do
        local language = LanguageById(id)

        if language then
            local info = UIDropDownMenu_CreateInfo()
            info.text = LanguageLabel(language)
            info.value = language.key
            info.checked = (id == state.language)
            info.func = ChooseLanguage
            UIDropDownMenu_AddButton(info, level)
        end
    end
end)

RefreshLanguage = function()
    -- The name alone: "Darnassian (40%)" would not fit, and the list says how much.
    local current = LanguageById(state.language or 0)
    languageButton:SetText(current and current.name or "...")
end

--------------------------------------------------------------------------
-- Speaking distance
--------------------------------------------------------------------------

-- The five distances the realm offers. A whisper you have to stand next to
-- someone to hear, through to carrying across a town square.
local RANGE_PRESETS = { 5, 10, 25, 50, 100 }

-- No caption: the numbers sit under a status line that reads "25 yd", and each button
-- explains itself on hover.
local rangeButtons = {}

for index, yards in ipairs(RANGE_PRESETS) do
    local button = CreateFrame("Button", nil, hud, "UIPanelButtonTemplate")
    button:SetWidth(30)
    button:SetHeight(18)
    button:SetPoint("BOTTOMLEFT", hud, "BOTTOMLEFT", 9 + ((index - 1) * 32), 8)
    button:SetText(tostring(yards))

    button:SetScript("OnClick", function()
        -- Only ever a request. The world server clamps it to the realm's window
        -- and echoes back what was actually applied, which is what the highlight
        -- below reflects -- so a preset outside the window visibly does not take.
        SendServerCommand("voice range " .. yards)
    end)

    button:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_TOP")
        GameTooltip:SetText(yards .. " yards")

        if yards <= 5 then
            GameTooltip:AddLine("Only someone stood beside you.", 1, 1, 1, true)
        elseif yards <= 10 then
            GameTooltip:AddLine("A small circle around you.", 1, 1, 1, true)
        elseif yards <= 25 then
            GameTooltip:AddLine("Those gathered around you.", 1, 1, 1, true)
        elseif yards <= 50 then
            GameTooltip:AddLine("A tavern room, or a small crowd.", 1, 1, 1, true)
        else
            GameTooltip:AddLine("Across a town square. Use sparingly.", 1, 1, 1, true)
        end

        GameTooltip:Show()
    end)

    button:SetScript("OnLeave", function() GameTooltip:Hide() end)

    rangeButtons[index] = { button = button, yards = yards }
end

-- Assigns the forward-declared local from the rendering section above.
UpdateRangeButtons = function()
    for _, entry in ipairs(rangeButtons) do
        local available = entry.yards >= state.minRange and entry.yards <= state.maxRange

        -- Enable/Disable, not SetEnabled: the latter does not exist in 3.3.5a and
        -- would throw on the first Refresh, taking the whole HUD down with it.
        if available then
            entry.button:Enable()
        else
            entry.button:Disable()
        end

        -- The active preset is locked down so it reads as selected rather than
        -- merely clickable. Anything the realm does not allow is greyed instead.
        if available and math.abs(state.range - entry.yards) < 0.5 then
            entry.button:LockHighlight()
        else
            entry.button:UnlockHighlight()
        end
    end
end

UpdateRangeButtons()

SLASH_SANCTUARYVOICE1 = "/svoice"
SLASH_SANCTUARYVOICE2 = "/pv"

SlashCmdList["SANCTUARYVOICE"] = function(input)
    local command, argument = string.match(input or "", "^(%S*)%s*(.*)$")
    command = string.lower(command or "")

    if command == "" or command == "help" then
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Sanctuary Voice|r")
        DEFAULT_CHAT_FRAME:AddMessage("  /svoice range <yards> - change your speaking distance")
        DEFAULT_CHAT_FRAME:AddMessage("  /svoice plates - show nameplates while people speak")
        DEFAULT_CHAT_FRAME:AddMessage("  /svoice test - light up every nearby plate for five seconds")
        DEFAULT_CHAT_FRAME:AddMessage("  /svoice debug - what the nameplate scan can see")
        DEFAULT_CHAT_FRAME:AddMessage("  Devices and push-to-talk live in the desktop client.")
        return
    end

    if command == "plates" then
        local enabled = not ForcingEnabled()
        SanctuaryVoiceDB.forcePlates = enabled

        if enabled then
            DEFAULT_CHAT_FRAME:AddMessage(
                "|cff00ff96Voice:|r the speaker's nameplate will appear while they talk. "
                .. "Everyone else's stays hidden, and your own setting is put back "
                .. "afterwards.")
        else
            -- Put the setting back now rather than leaving it forced until the
            -- current conversation happens to end.
            RestoreNameplates()
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Voice:|r nameplates left as you set them.")
        end
    elseif command == "test" then
        --[[
            Lights up every plate on screen for a few seconds, as though everyone were
            talking.

            This splits the one question that cannot be answered from outside the game into
            two. If bars appear, drawing and plate detection are both fine and the problem is
            upstream - the names the server sends not matching the names on the plates. If
            nothing appears, the addon either cannot see the plates or cannot draw on them,
            and /svoice debug says which.
        ]]
        local lit = 0

        for _, frame in ipairs({ WorldFrame:GetChildren() }) do
            if IsNamePlate(frame) then
                local name = PlateName(frame)

                if name then
                    speakingUntil[name] = GetTime() + 5
                    lit = lit + 1
                end
            end
        end

        if lit > 0 then
            DEFAULT_CHAT_FRAME:AddMessage(string.format(
                "|cff00ff96Voice:|r pretending %d nearby plate(s) are speaking for five "
                .. "seconds. If no bars appear, the addon cannot draw on them - run "
                .. "|cffffffff/svoice debug|r.", lit))
        else
            DEFAULT_CHAT_FRAME:AddMessage(
                "|cff00ff96Voice:|r no nameplates found. Press |cffffffffV|r to show them, "
                .. "stand near somebody, and try again - or run |cffffffff/svoice debug|r.")
        end

    elseif command == "trace" then
        SanctuaryVoiceDB = SanctuaryVoiceDB or {}
        SanctuaryVoiceDB.trace = not SanctuaryVoiceDB.trace
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Voice:|r message trace "
            .. (SanctuaryVoiceDB.trace and "on - SPK and CMB messages will be printed as they arrive."
                or "off."))
    elseif command == "debug" then
        --[[
            Everything the nameplate scan sees, in one place.

            Plate detection has been the hard part of this feature: it is invisible when it
            fails, and it fails differently on patched clients. Rather than guess again at
            what a plate looks like, this prints what is actually there.
        ]]
        local children = { WorldFrame:GetChildren() }
        local anonymous, detected = 0, 0
        local unmatched = {}

        for _, frame in ipairs(children) do
            if not frame:GetName() then
                anonymous = anonymous + 1

                if IsNamePlate(frame) then
                    detected = detected + 1

                    if detected <= 3 then
                        DEFAULT_CHAT_FRAME:AddMessage("   plate: |cffffffff"
                            .. tostring(PlateName(frame)) .. "|r")
                    end
                elseif #unmatched < 2 then
                    local textures = {}

                    for index, region in ipairs({ frame:GetRegions() }) do
                        if region.GetObjectType and region:GetObjectType() == "Texture"
                            and region.GetTexture then
                            table.insert(textures, index .. "=" .. tostring(region:GetTexture()))
                        end
                    end

                    table.insert(unmatched, string.format("kids=%d %s",
                        frame:GetNumChildren(), table.concat(textures, " ")))
                end
            end
        end

        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Voice debug|r")
        DEFAULT_CHAT_FRAME:AddMessage(string.format(
            "   %d WorldFrame children, %d anonymous, |cffffffff%d detected as plates|r.",
            #children, anonymous, detected))

        for _, line in ipairs(unmatched) do
            DEFAULT_CHAT_FRAME:AddMessage("   unmatched: " .. line)
        end

        local talking = {}
        for name in pairs(speakingUntil) do
            table.insert(talking, name)
        end

        DEFAULT_CHAT_FRAME:AddMessage("   speaking: |cffffffff"
            .. (#talking > 0 and table.concat(talking, ", ") or "nobody") .. "|r")

        local held, heard = {}, {}
        for name in pairs(combatUntil) do table.insert(held, name) end
        for name in pairs(visibleUntil) do table.insert(heard, name) end

        DEFAULT_CHAT_FRAME:AddMessage("   held for combat: |cffffffff"
            .. (#held > 0 and table.concat(held, ", ") or "nobody") .. "|r")
        DEFAULT_CHAT_FRAME:AddMessage("   recently heard: |cffffffff"
            .. (#heard > 0 and table.concat(heard, ", ") or "nobody") .. "|r")
        DEFAULT_CHAT_FRAME:AddMessage(string.format(
            "   forcing=%s borrowed=%s (filtering needs both)",
            tostring(plateOverride.active), tostring(plateOverride.borrowed)))
    elseif command == "range" then
        if argument == "" then
            -- Reporting rather than complaining: this is where you ask how far you carry.
            DEFAULT_CHAT_FRAME:AddMessage(
                "|cff00ff96Voice:|r your voice carries " .. state.range
                .. " yards. Use the buttons on the panel, or |cffffffff/svoice range <yards>|r.")
            return
        end

        -- The world server is the only thing allowed to change this; the new
        -- value arrives back as a CFG update.
        SendServerCommand("voice range " .. argument)
    else
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Voice:|r unknown option. Try /svoice help.")
    end
end
