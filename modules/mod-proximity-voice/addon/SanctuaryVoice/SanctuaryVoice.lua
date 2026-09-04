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
hud:SetHeight(54)
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

local function PlateIcon(plate)
    if plateIcons[plate] then
        return plateIcons[plate]
    end

    -- A frame rather than loose textures, so the bars show and hide as one thing.
    local holder = CreateFrame("Frame", nil, plate)
    holder:SetWidth((BAR_COUNT * 6) - 2)
    holder:SetHeight(16)

    -- Anchored off the health bar where there is one. The plate *frame* is taller than the
    -- art inside it, so anchoring to the frame puts the bars somewhere above the part of
    -- the plate the player is actually looking at.
    local healthBar = plate:GetChildren()

    if healthBar then
        holder:SetPoint("BOTTOM", healthBar, "TOP", 0, 12)
    else
        holder:SetPoint("BOTTOM", plate, "TOP", 0, 2)
    end

    holder.bars = {}

    for i = 1, BAR_COUNT do
        local bar = holder:CreateTexture(nil, "OVERLAY")
        bar:SetTexture("Interface\\Buttons\\WHITE8X8")
        bar:SetWidth(4)
        bar:SetHeight(8)
        bar:SetPoint("BOTTOMLEFT", holder, "BOTTOMLEFT", (i - 1) * 6, 0)
        -- Bright, not subtle. This has to be legible over whatever is behind it.
        bar:SetVertexColor(1.0, 0.82, 0.35)
        holder.bars[i] = bar
    end

    holder:Hide()

    plateIcons[plate] = holder
    return holder
end

--- Phase of the bar animation, advanced by the scan tick that is already running.
local barPhase = 0

--- Gives the bars a life of their own, so a speaking plate reads as active at a glance.
local function AnimateBars(holder)
    for i = 1, BAR_COUNT do
        -- Each bar runs a little behind the last, so the movement travels across them.
        local wave = math.sin(barPhase + (i * 2.0))
        holder.bars[i]:SetHeight(6 + math.floor((wave + 1) * 4.5))
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
local function ClassifyNewPlates()
    if not ShouldFilter() then
        return
    end

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

    if AnyoneSpeaking() then
        plateOverride.quietFor = 0
        ForceNameplatesOn()
    elseif plateOverride.active then
        plateOverride.quietFor = plateOverride.quietFor + slice

        if plateOverride.quietFor >= PLATE_LINGER then
            RestoreNameplates()
        end
    end

    for _, frame in ipairs({ WorldFrame:GetChildren() }) do
        if IsNamePlate(frame) then
            local name = PlateName(frame)
            local speaking = IsSpeakingName(name)
            local icon = PlateIcon(frame)

            if speaking and frame:IsShown() then
                AnimateBars(icon)
                icon:Show()
            else
                icon:Hide()
            end

            -- Read whether it is shown *after* the filter has had its say, or a plate we
            -- hid a moment ago would be treated as absent.
            ApplyPlateFilter(frame, speaking)
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
    else
        local previous = speakers[guid]

        if previous then
            -- Lingers rather than clearing. This message arrives after 300ms of silence,
            -- which is every pause for breath, not the end of the conversation.
            speakingUntil[previous.name] = GetTime() + SPEAKER_LINGER
        end

        speakers[guid] = nil
    end

    Refresh()
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
        haveConfig = false
        sinceSync = 0
        RequestSync()
        return
    end

    if event == "CHAT_MSG_ADDON" and arg1 == ADDON_PREFIX then
        local verb, body = string.match(arg2, "^(%u+)%s*(.*)$")
        if verb == "CFG" then
            haveConfig = true
            HandleConfig(body)
        elseif verb == "SPK" then
            HandleSpeaking(body)
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
