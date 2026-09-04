--[[
    Sanctuary Lawman

    One button, for going on and off duty. It only ever appears for somebody who actually
    holds the office: the panel stays hidden until the server says otherwise, and the
    server is asked rather than the client deciding for itself.

    Like the outlaw panel, the button never sets its own state -- everything it shows is
    the last thing the server said, because the server can refuse.
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
-- Frame
--------------------------------------------------------------------------

local BACKDROP = {
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true,
    tileSize = 32,
    edgeSize = 16,
    insets = { left = 5, right = 5, top = 5, bottom = 5 },
}

local panel = CreateFrame("Frame", "SanctuaryLawmanPanel", UIParent)
panel:SetWidth(146)
panel:SetHeight(58)
panel:SetPoint("CENTER", UIParent, "CENTER", 300, -30)
panel:SetBackdrop(BACKDROP)
panel:SetBackdropColor(0, 0, 0, 0.65)
panel:SetMovable(true)
panel:EnableMouse(true)
panel:RegisterForDrag("LeftButton")
panel:SetScript("OnDragStart", panel.StartMoving)
panel:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    if SanctuaryLawmanDB then
        local point, _, relativePoint, x, y = self:GetPoint()
        SanctuaryLawmanDB.point = point
        SanctuaryLawmanDB.relativePoint = relativePoint
        SanctuaryLawmanDB.x = x
        SanctuaryLawmanDB.y = y
    end
end)

-- Hidden until the server confirms the office. Somebody who holds none never sees it and
-- is never told there was anything to see.
panel:Hide()

local statusLine = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
statusLine:SetPoint("TOPLEFT", panel, "TOPLEFT", 12, -10)
statusLine:SetWidth(122)
statusLine:SetJustifyH("LEFT")

local toggle = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
toggle:SetWidth(122)
toggle:SetHeight(20)
toggle:SetPoint("BOTTOMLEFT", panel, "BOTTOMLEFT", 12, 9)
toggle:SetText("Go on duty")

--------------------------------------------------------------------------
-- Drawing whatever the server last said
--------------------------------------------------------------------------

local function Refresh()
    if not state.lawman then
        panel:Hide()
        return
    end

    if SanctuaryLawmanDB and SanctuaryLawmanDB.hidden then
        panel:Hide()
    else
        panel:Show()
    end

    if not haveState then
        statusLine:SetText("|cff808080Asking...|r")
        -- Enable/Disable, not SetEnabled: the latter does not exist in 3.3.5a and would
        -- throw here, taking the panel down with it.
        toggle:Disable()
        return
    end

    toggle:Enable()

    if state.duty then
        statusLine:SetText("|cff7fb069" .. state.title .. "|r on duty")
        toggle:SetText("Stand down")
        toggle:LockHighlight()
    else
        statusLine:SetText(state.title .. ", off duty")
        toggle:SetText("Go on duty")
        toggle:UnlockHighlight()
    end
end

local function HandleState(body)
    local lawman = string.match(body, "lawman=(%d)")
    local duty = string.match(body, "duty=(%d)")
    local title = string.match(body, "title=(%S+)")

    state.lawman = (lawman == "1")
    state.duty = (duty == "1")
    state.title = (title ~= "-" and title) or "Lawman"

    haveState = true
    Refresh()
end

--------------------------------------------------------------------------
-- Input
--------------------------------------------------------------------------

toggle:SetScript("OnClick", function()
    -- Only ever a request; Refresh draws whatever the server decided.
    SendServerCommand(state.duty and "lawman off" or "lawman on")
end)

toggle:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_TOP")
    if state.duty then
        GameTooltip:SetText("Stand down")
        GameTooltip:AddLine("Takes off the tabard. The writ stays in your pack.", 1, 1, 1, true)
    else
        GameTooltip:SetText("Go on duty")
        GameTooltip:AddLine("Wear the tabard and carry the writ. Guards stay friendly to you.", 1, 1, 1, true)
    end
    GameTooltip:Show()
end)

toggle:SetScript("OnLeave", function() GameTooltip:Hide() end)

SLASH_SANCTUARYLAWMAN1 = "/lawman"
SlashCmdList["SANCTUARYLAWMAN"] = function(input)
    input = string.lower(string.trim and string.trim(input) or input or "")

    if input == "show" then
        SanctuaryLawmanDB.hidden = false
        Refresh()
    elseif input == "hide" then
        SanctuaryLawmanDB.hidden = true
        panel:Hide()
    else
        SendServerCommand("lawman " .. (input ~= "" and input or "status"))
    end
end

--------------------------------------------------------------------------
-- Events
--------------------------------------------------------------------------

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")

listener:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "CHAT_MSG_ADDON" then
        if arg1 ~= ADDON_PREFIX then return end

        local verb, body = string.match(arg2, "^(%u+)%s*(.*)$")
        if verb == "STATE" then
            HandleState(body)
        end
        return
    end

    playerName = UnitName("player")

    SanctuaryLawmanDB = SanctuaryLawmanDB or {}

    if SanctuaryLawmanDB.point then
        panel:ClearAllPoints()
        panel:SetPoint(SanctuaryLawmanDB.point, UIParent, SanctuaryLawmanDB.relativePoint,
                       SanctuaryLawmanDB.x, SanctuaryLawmanDB.y)
    end

    Refresh()
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
