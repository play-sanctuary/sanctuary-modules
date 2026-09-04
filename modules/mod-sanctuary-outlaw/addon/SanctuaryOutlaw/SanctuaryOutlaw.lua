--[[
    Sanctuary Outlaw

    One button, on the minimap. Pressed, it declares you open to violence: anyone may raise a
    hand to you, and you to them. Pressed again it asks to stand down, which takes a minute
    and is refused outright while you are fighting.

    The button never sets its own state. Everything it shows is the last thing the server
    said, because the server can refuse - you are in combat, you are a game master, the
    realm has it switched off - and a button that lies about that is worse than no button.

    There was a panel too, carrying a status line and a toggle of its own. It said nothing
    the button does not: the icon already carries the state as colour, the tooltip carries
    the detail and the countdown, and both sent the identical request. Two controls for one
    decision is one too many, so the panel is gone and the button is the whole addon.
]]

local ADDON_PREFIX = "SOUTLAW"

local playerName
local state = { on = false, releasing = false, left = 0 }
local haveState = false

--------------------------------------------------------------------------
-- Talking to the server
--------------------------------------------------------------------------

local function RequestSync()
    SendAddonMessage(ADDON_PREFIX, "SYNC", "WHISPER", playerName or UnitName("player"))
end

-- Commands go out over AzerothCore's addon command channel rather than as a chat line
-- starting with a dot. An unrecognised dot command falls through and is published to
-- /say, which would announce "outlaw on" to the room; this channel just goes nowhere.
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
    local on = string.match(body, "on=(%d)")
    local releasing = string.match(body, "releasing=(%d)")
    local left = string.match(body, "left=(%d+)")

    state.on = (on == "1")
    state.releasing = (releasing == "1")
    state.left = tonumber(left) or 0

    haveState = true

    -- Defined further down, with the button itself. Safe unguarded: nothing reaches here
    -- until the server answers, which is long after the file has finished loading.
    SanctuaryOutlaw_RefreshButton()
end

--------------------------------------------------------------------------
-- Input
--------------------------------------------------------------------------

SLASH_SANCTUARYOUTLAW1 = "/outlaw"
SlashCmdList["SANCTUARYOUTLAW"] = function(input)
    input = string.lower(string.trim and string.trim(input) or input or "")

    -- show/hide used to open and close the panel. They govern the minimap button now, which
    -- is the only thing left to show or hide - and worth keeping, because a skull sitting on
    -- the minimap is not something everybody wants there.
    if input == "show" then
        SanctuaryOutlawButton:Show()
        SanctuaryOutlawDB.hidden = false
    elseif input == "hide" then
        SanctuaryOutlawButton:Hide()
        SanctuaryOutlawDB.hidden = true
    else
        SendServerCommand("outlaw " .. (input ~= "" and input or "status"))
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
        SanctuaryOutlawDB = SanctuaryOutlawDB or {}

        -- The button was placed at load from the default; now that the saved angle has
        -- loaded, put it back where the player left it.
        if SanctuaryOutlawDB.buttonAngle and SanctuaryOutlaw_PlaceButton then
            SanctuaryOutlaw_PlaceButton(SanctuaryOutlawDB.buttonAngle)
        end
        return
    end

    if event == "CHAT_MSG_ADDON" then
        if arg1 ~= ADDON_PREFIX then return end

        local verb, body = string.match(arg2, "^(%u+)%s*(.*)$")
        if verb == "STATE" then
            HandleState(body)
        end
        return
    end

    -- PLAYER_ENTERING_WORLD
    playerName = UnitName("player")

    SanctuaryOutlawDB = SanctuaryOutlawDB or {}

    if SanctuaryOutlawDB.hidden then SanctuaryOutlawButton:Hide() end

    SanctuaryOutlaw_RefreshButton()
end)

--[[
    The server pushes state on login, but that push can land while the client is still on
    the loading screen, and after a /reload this addon knows nothing at all. So it asks,
    and keeps asking until answered - slowly once it has an answer, since the countdown
    while standing down is worth keeping honest.
]]
local elapsed = 0
listener:SetScript("OnUpdate", function(self, delta)
    elapsed = elapsed + delta

    local interval = haveState and (state.releasing and 5 or 15) or 2
    if elapsed < interval then return end

    elapsed = 0
    RequestSync()
end)

--------------------------------------------------------------------------
-- The button
--------------------------------------------------------------------------

--[[
    Within reach of the minimap, and never opening anything.

    Being outlaw is dangerous enough that it should be visible without a window in the way,
    and reversible without hunting for one. The icon carries the state as colour and the
    tooltip carries the rest, which between them is everything the panel used to say.
]]

local outlawButton = CreateFrame("Button", "SanctuaryOutlawButton", Minimap)
outlawButton:SetWidth(31)
outlawButton:SetHeight(31)
outlawButton:SetFrameStrata("MEDIUM")
outlawButton:SetFrameLevel(8)
outlawButton:RegisterForClicks("LeftButtonUp")
outlawButton:RegisterForDrag("LeftButton")
outlawButton:SetMovable(true)

local outlawIcon = outlawButton:CreateTexture(nil, "BACKGROUND")
outlawIcon:SetWidth(20)
outlawIcon:SetHeight(20)
outlawIcon:SetPoint("TOPLEFT", 7, -6)
outlawIcon:SetTexture("Interface\\Icons\\INV_Misc_Bone_HumanSkull_01")

local outlawBorder = outlawButton:CreateTexture(nil, "OVERLAY")
outlawBorder:SetWidth(53)
outlawBorder:SetHeight(53)
outlawBorder:SetPoint("TOPLEFT", 0, 0)
outlawBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

--- Places the button on the minimap's edge at the given angle, in degrees.
local function PlaceOutlawButton(angle)
    -- math.rad because the global cos/sin the WoW API adds take degrees and these do not.
    local x = 80 * math.cos(math.rad(angle))
    local y = 80 * math.sin(math.rad(angle))
    outlawButton:SetPoint("CENTER", Minimap, "CENTER", x, y)
end

local function SavedOutlawAngle()
    if SanctuaryOutlawDB and SanctuaryOutlawDB.buttonAngle then
        return SanctuaryOutlawDB.buttonAngle
    end
    -- The arc the four Sanctuary buttons sit in, measured off a live minimap rather than
    -- guessed: outlaw -113.04, stash -132.70, disguise -151.64, profile -171.25. Roughly 19
    -- degrees apart, which is about 27px at this radius, so they sit against each other
    -- without touching the stock tracking and clock buttons.
    return -113.04
end

local function DragOutlawButton(self)
    local mx, my = Minimap:GetCenter()
    local cx, cy = GetCursorPosition()
    local scale = UIParent:GetScale()

    local angle = math.deg(math.atan2((cy / scale) - my, (cx / scale) - mx))

    PlaceOutlawButton(angle)

    if SanctuaryOutlawDB then
        SanctuaryOutlawDB.buttonAngle = angle
    end
end

outlawButton:SetScript("OnDragStart", function(self)
    self:SetScript("OnUpdate", DragOutlawButton)
end)

outlawButton:SetScript("OnDragStop", function(self)
    self:SetScript("OnUpdate", nil)
end)

--- The whole of the addon's drawing: the state as the icon's colour.
function SanctuaryOutlaw_RefreshButton()
    if not haveState then
        outlawIcon:SetVertexColor(0.5, 0.5, 0.5)
    elseif state.releasing then
        -- Standing down: amber, because you are still fair game until it completes.
        outlawIcon:SetVertexColor(1.0, 0.8, 0.2)
    elseif state.on then
        outlawIcon:SetVertexColor(1.0, 0.25, 0.25)
    else
        outlawIcon:SetVertexColor(1.0, 1.0, 1.0)
    end
end

outlawButton:SetScript("OnClick", function()
    -- Only ever a request. The server decides and the refresh draws whatever it decided,
    -- so a refusal in combat visibly does not take, rather than the button lying.
    if state.on and not state.releasing then
        SendServerCommand("outlaw off")
    else
        SendServerCommand("outlaw on")
    end
end)

outlawButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")

    if not haveState then
        GameTooltip:SetText("Outlaw")
        GameTooltip:AddLine("Asking the server...", 1, 1, 1, true)
    elseif state.releasing then
        GameTooltip:SetText("|cffffcc00Standing down|r")
        GameTooltip:AddLine(string.format("%d seconds left. You are still fair game until it finishes.",
            state.left or 0), 1, 1, 1, true)
    elseif state.on then
        GameTooltip:SetText("|cffff4040OUTLAW|r")
        GameTooltip:AddLine("Anyone may raise a hand to you, guards included.", 1, 1, 1, true)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine("Click to stand down. Takes a minute, and is refused while fighting.", 0.6, 0.6, 0.6, true)
    else
        GameTooltip:SetText("Lawful")
        GameTooltip:AddLine("Nobody of your own faction may strike you.", 1, 1, 1, true)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine("Click to declare yourself open to violence.", 0.6, 0.6, 0.6, true)
        GameTooltip:AddLine("Drag to move.", 0.6, 0.6, 0.6, true)
    end

    GameTooltip:Show()
end)

outlawButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

--- Exposed so VARIABLES_LOADED can reposition once the saved angle is available.
function SanctuaryOutlaw_PlaceButton(angle)
    PlaceOutlawButton(angle)
end

PlaceOutlawButton(SavedOutlawAngle())
SanctuaryOutlaw_RefreshButton()
