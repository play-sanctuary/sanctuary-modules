--[[
    Sanctuary Identity - introducing yourself

    Everyone is a stranger until they say otherwise. A stranger reads as an alias - "Hooded
    Orc" - on their nameplate, in their tooltip, on your target frame, in chat, and over
    their head.

    None of that happens here. The server answers the client's name query with the alias, so
    the real name never arrives in the first place. That is the only thing that reaches the
    name drawn above a character's head: the client draws it itself, unconditionally, from
    its own name cache, and no addon can touch it.

    This addon is therefore only what the server cannot do on its own: offer you a way to
    hand your name over.

    Protocol, on the SVID prefix, carried as a whisper to yourself:
        out  A:<hex guid>     where do I stand with this character?
        out  G:<hex guid>     introduce myself to them
        out  I:<name>         introduce myself by name, for someone already known to me
        in   S:<hex guid>:<i know them>:<they know me>

    Note what the replies do not contain: a name. The addon channel is readable by the
    client, so answering with a stranger's real name would reopen the leak the server-side
    disguise exists to close.
]]

local ADDON_PREFIX = "SVID"

-- hex guid -> { theyKnowMe = bool, iKnowThem = bool, asked = time }
local standing = {}

local playerName

local function Send(body)
    SendAddonMessage(ADDON_PREFIX, body, "WHISPER", playerName or UnitName("player"))
end

-- Dot commands go over AzerothCore's addon command channel rather than as a chat line. An
-- unrecognised dot command falls through and is published to /say, which would announce
-- "disguise" to the room -- the exact opposite of the point.
local commandCounter = 0

local function SendServerCommand(command)
    commandCounter = (commandCounter + 1) % 10000
    SendAddonMessage("AzerothCore",
        string.format("i%04d%s", commandCounter, command),
        "WHISPER",
        playerName or UnitName("player"))
end

--- The guid as the server wants it: hex, without the 0x the client prefixes.
local function GuidKey(unit)
    local guid = UnitGUID(unit)
    return guid and string.sub(guid, 3) or nil
end

local function Ask(unit)
    local key = GuidKey(unit)
    if not key then
        return nil
    end

    local entry = standing[key]
    local now = GetTime()

    -- Re-asked rather than cached forever: an introduction can happen at any moment, and a
    -- stale "they do not know you" would have you introducing yourself twice.
    if not entry or (now - entry.asked) > 10 then
        standing[key] = { asked = now }
        Send("A:" .. key)
    end

    return standing[key]
end

--------------------------------------------------------------------------
-- Introducing
--------------------------------------------------------------------------

local function IntroduceTo(unit)
    if not unit or not UnitExists(unit) or not UnitIsPlayer(unit) then
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r select a player first.")
        return
    end

    if UnitIsUnit(unit, "player") then
        return
    end

    local key = GuidKey(unit)
    if not key then
        return
    end

    -- The guid, not the name. Once names are disguised the client genuinely does not know
    -- who it is looking at, and the server cannot find "Hooded Orc" either. This grants
    -- nothing: the row it writes reveals the sender and only the sender.
    Send("G:" .. key)

    local entry = standing[key]
    if entry and entry.theyKnowMe then
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r they already know your name.")
    else
        DEFAULT_CHAT_FRAME:AddMessage(
            "|cffd8b46aSanctuary:|r you introduce yourself to " .. (UnitName(unit) or "them") .. ".")
    end

    -- Whatever we thought we knew is now out of date.
    standing[key] = nil
end

-- Right-click menu on a player.
UnitPopupButtons["SANCTUARY_INTRODUCE"] = {
    text = "Introduce yourself",
    dist = 0,
}

for _, menu in ipairs({ "PLAYER", "PARTY", "RAID_PLAYER", "FRIEND" }) do
    if UnitPopupMenus[menu] then
        table.insert(UnitPopupMenus[menu], #UnitPopupMenus[menu], "SANCTUARY_INTRODUCE")
    end
end

hooksecurefunc("UnitPopup_OnClick", function(self)
    if self.value ~= "SANCTUARY_INTRODUCE" then
        return
    end

    local dropdown = UIDROPDOWNMENU_INIT_MENU

    if dropdown.unit then
        IntroduceTo(dropdown.unit)
    elseif dropdown.name then
        -- A menu opened from a chat name has no unit. This only resolves for somebody
        -- already known to us, which is the only case where a name is meaningful.
        Send("I:" .. dropdown.name)
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r you introduce yourself to " .. dropdown.name .. ".")
    end
end)

--------------------------------------------------------------------------
-- Incoming
--------------------------------------------------------------------------

local function HandleServerMessage(body)
    local kind = string.sub(body, 1, 1)

    if kind == "D" then
        local on = string.match(body, "^D:([01])$")
        if on and SanctuaryIdentity_SetDisguiseState then
            SanctuaryIdentity_SetDisguiseState(on == "1")
        end
        return
    end

    if kind ~= "S" then
        return
    end

    local key, iKnowThem, theyKnowMe = string.match(body, "^S:([^:]+):([01]):([01])$")
    if not key then
        return
    end

    standing[key] = {
        asked = GetTime(),
        iKnowThem = iKnowThem == "1",
        theyKnowMe = theyKnowMe == "1",
    }
end

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("VARIABLES_LOADED")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")
listener:RegisterEvent("PLAYER_TARGET_CHANGED")

listener:SetScript("OnEvent", function(self, event, prefix, message)
    if event == "CHAT_MSG_ADDON" then
        if prefix == ADDON_PREFIX then
            HandleServerMessage(message)
        end
        return
    end

    if event == "PLAYER_TARGET_CHANGED" then
        -- Asked on target rather than on demand, so the answer has arrived by the time the
        -- player opens the right-click menu.
        if UnitExists("target") and UnitIsPlayer("target") and not UnitIsUnit("target", "player") then
            Ask("target")
        end
        return
    end

    if event == "VARIABLES_LOADED" then
        SanctuaryIdentityDB = SanctuaryIdentityDB or {}

        -- The button was placed at load from whatever default was available; now that the
        -- saved angle exists, put it where the player actually left it.
        if SanctuaryIdentityDB.buttonAngle and SanctuaryIdentity_PlaceButton then
            SanctuaryIdentity_PlaceButton(SanctuaryIdentityDB.buttonAngle)
        end
        return
    end

    playerName = UnitName("player")
    standing = {}
end)

--------------------------------------------------------------------------
-- Slash commands
--------------------------------------------------------------------------

SLASH_SANCTUARYIDENTITY1 = "/introduce"
SLASH_SANCTUARYIDENTITY2 = "/sid"

SlashCmdList["SANCTUARYIDENTITY"] = function(input)
    input = string.gsub(input or "", "^%s*(.-)%s*$", "%1")

    if input == "" then
        IntroduceTo("target")
        return
    end

    if input == "help" then
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary Identity|r")
        DEFAULT_CHAT_FRAME:AddMessage("  /introduce - introduce yourself to your target")
        DEFAULT_CHAT_FRAME:AddMessage("  /introduce <name> - introduce yourself to someone you already know")
        DEFAULT_CHAT_FRAME:AddMessage("Right-clicking a player offers the same thing.")
        DEFAULT_CHAT_FRAME:AddMessage("Strangers are shown to you under an alias until they introduce themselves.")
        return
    end

    Send("I:" .. input)
    DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r you introduce yourself to " .. input .. ".")
end

--------------------------------------------------------------------------
-- Minimap button
--------------------------------------------------------------------------

--[[
    A hood you can reach without typing.

    The button never decides anything. It shows the last thing the server said, and a click
    is only a request -- the realm can have disguises switched off, and a button that
    reported "disguised" when the server disagreed would be worse than no button at all.
]]

local disguised = false
local haveDisguiseState = false

local minimapButton = CreateFrame("Button", "SanctuaryDisguiseButton", Minimap)
minimapButton:SetWidth(31)
minimapButton:SetHeight(31)
minimapButton:SetFrameStrata("MEDIUM")
minimapButton:SetFrameLevel(8)
minimapButton:RegisterForClicks("LeftButtonUp")
minimapButton:RegisterForDrag("LeftButton")
minimapButton:SetMovable(true)

local disguiseIcon = minimapButton:CreateTexture(nil, "BACKGROUND")
disguiseIcon:SetWidth(20)
disguiseIcon:SetHeight(20)
disguiseIcon:SetPoint("TOPLEFT", 7, -6)
disguiseIcon:SetTexture("Interface\\Icons\\Ability_Stealth")

local disguiseBorder = minimapButton:CreateTexture(nil, "OVERLAY")
disguiseBorder:SetWidth(53)
disguiseBorder:SetHeight(53)
disguiseBorder:SetPoint("TOPLEFT", 0, 0)
disguiseBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

--- Places the button on the minimap's edge at the given angle, in degrees.
local function PlaceDisguiseButton(angle)
    -- 80 clears the default minimap and its border on a stock UI. math.rad because the
    -- global cos/sin the WoW API adds take degrees and these do not.
    local x = 80 * math.cos(math.rad(angle))
    local y = 80 * math.sin(math.rad(angle))
    minimapButton:SetPoint("CENTER", Minimap, "CENTER", x, y)
end

local function SavedDisguiseAngle()
    if SanctuaryIdentityDB and SanctuaryIdentityDB.buttonAngle then
        return SanctuaryIdentityDB.buttonAngle
    end
    -- The arc the four Sanctuary buttons sit in, measured off a live minimap rather than
    -- guessed: outlaw -113.04, stash -132.70, disguise -151.64, profile -171.25. Roughly 19
    -- degrees apart, which is about 27px at this radius, so they sit against each other
    -- without touching the stock tracking and clock buttons.
    return -151.64
end

local function DragDisguiseButton(self)
    local mx, my = Minimap:GetCenter()
    local cx, cy = GetCursorPosition()
    local scale = UIParent:GetScale()

    local angle = math.deg(math.atan2((cy / scale) - my, (cx / scale) - mx))

    PlaceDisguiseButton(angle)

    if SanctuaryIdentityDB then
        SanctuaryIdentityDB.buttonAngle = angle
    end
end

minimapButton:SetScript("OnDragStart", function(self)
    self:SetScript("OnUpdate", DragDisguiseButton)
end)

minimapButton:SetScript("OnDragStop", function(self)
    self:SetScript("OnUpdate", nil)
end)

local function RefreshDisguiseButton()
    if not haveDisguiseState then
        disguiseIcon:SetVertexColor(0.5, 0.5, 0.5)
        return
    end

    -- Gold while worn, plain while not: readable at a glance without a second texture.
    if disguised then
        disguiseIcon:SetVertexColor(1.0, 0.82, 0.3)
    else
        disguiseIcon:SetVertexColor(1.0, 1.0, 1.0)
    end
end

minimapButton:SetScript("OnClick", function()
    -- A request, not a decision. Refresh draws whatever the server says came back.
    SendServerCommand("disguise")
end)

minimapButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")

    if not haveDisguiseState then
        GameTooltip:SetText("Disguise")
        GameTooltip:AddLine("Asking the server...", 1, 1, 1, true)
    elseif disguised then
        GameTooltip:SetText("|cffd8b46aDisguised|r")
        GameTooltip:AddLine("Nobody knows you, including people you have introduced yourself to.", 1, 1, 1, true)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine("Click to lower your hood.", 0.6, 0.6, 0.6, true)
    else
        GameTooltip:SetText("Disguise")
        GameTooltip:AddLine("Go unrecognised, even to those who know your name.", 1, 1, 1, true)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine("Click to draw up your hood.", 0.6, 0.6, 0.6, true)
        GameTooltip:AddLine("Drag to move.", 0.6, 0.6, 0.6, true)
    end

    GameTooltip:Show()
end)

minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

--- Called from the addon message handler when the server reports the disguise state.
function SanctuaryIdentity_SetDisguiseState(on)
    disguised = on
    haveDisguiseState = true
    RefreshDisguiseButton()
end

--- Asked repeatedly until answered: a push at login arrives during the loading screen.
local disguiseSync = CreateFrame("Frame")
local sinceDisguiseAsk = 0

disguiseSync:SetScript("OnUpdate", function(self, elapsed)
    if haveDisguiseState then
        self:SetScript("OnUpdate", nil)
        return
    end

    sinceDisguiseAsk = sinceDisguiseAsk + elapsed
    if sinceDisguiseAsk < 2 then
        return
    end

    sinceDisguiseAsk = 0
    Send("D:?")
end)

--- Exposed so VARIABLES_LOADED can reposition once the saved angle is available.
function SanctuaryIdentity_PlaceButton(angle)
    PlaceDisguiseButton(angle)
end

PlaceDisguiseButton(SavedDisguiseAngle())
RefreshDisguiseButton()
