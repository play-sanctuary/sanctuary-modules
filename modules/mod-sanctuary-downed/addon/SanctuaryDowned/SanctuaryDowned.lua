--[[
    SanctuaryDowned - the button you get while you are lying on the floor.

    A downed player is alive, rooted and silenced, with ten minutes on the clock. If nobody
    is coming, waiting it out is the only option they have, and `.bleedout` is not something
    to expect anyone to remember while face down in the dirt. So it becomes a button.

    The trigger is the "Bleeding Out" debuff rather than a message from the server. That
    aura already has to exist - it is the countdown the player watches - so using it as the
    signal means there is no addon channel to register, no packet to keep in step, and no
    way for the button and the timer to disagree about whether you are down.
]]

-- Commands go out over AzerothCore's addon command channel rather than as a chat line
-- starting with a dot. Two reasons, and the second is the one that matters now: an
-- unrecognised dot command falls through and is published to /say, and the realm refuses
-- typed commands from ordinary players - the windows are the way in, and a button that
-- typed would be refused alongside the typing.
--
-- No leading dot. AddonChannelCommandHandler::ParseCommands passes the text straight
-- through, so ".carry drop" would be looked up as a command named ".carry".
local commandCounter = 0

local function SendServerCommand(command)
    commandCounter = (commandCounter + 1) % 10000
    SendAddonMessage("AzerothCore",
        string.format("i%04d%s", commandCounter, command),
        "WHISPER",
        UnitName("player"))
end

local BLEED_OUT_SPELL = 81003
local IMMUNE_SPELL = 81002
local CARRIED_SPELL = 81007

local frame = CreateFrame("Frame", "SanctuaryDownedFrame", UIParent)
frame:SetWidth(260)
frame:SetHeight(96)
frame:SetPoint("CENTER", UIParent, "CENTER", 0, -140)
frame:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
frame:SetFrameStrata("DIALOG")
frame:Hide()

local title = frame:CreateFontString(nil, "ARTWORK", "GameFontNormal")
title:SetPoint("TOP", frame, "TOP", 0, -18)
title:SetText("You are down")

local timer = frame:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
timer:SetPoint("TOP", title, "BOTTOM", 0, -6)

local button = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
button:SetWidth(150)
button:SetHeight(22)
button:SetPoint("BOTTOM", frame, "BOTTOM", 0, 16)
button:SetText("Stop holding on")

button:SetScript("OnClick", function()
    SendServerCommand("bleedout")
end)

button:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText("Bleed out now", 1, 1, 1)
    GameTooltip:AddLine("Stop waiting. This kills you, and cannot be undone.", nil, nil, nil, true)
    GameTooltip:Show()
end)
button:SetScript("OnLeave", function() GameTooltip:Hide() end)

--[[
    Finds one of our auras on the player.

    Scanned by spell id rather than by name: the names are ours and could be changed in the
    patch at any time, while the ids are what the module actually applies. UnitAura returns
    the id eleventh in 3.3.5.
]]
local function findAura(spellId)
    for i = 1, 40 do
        local name, _, _, _, _, duration, expires, _, _, _, id = UnitAura("player", i, "HARMFUL")
        if not name then
            return nil
        end
        if id == spellId then
            return duration, expires
        end
    end
end

local function refresh()
    local _, expires = findAura(BLEED_OUT_SPELL)

    if not expires then
        frame:Hide()
        return
    end

    frame:Show()

    -- The button is withheld during the immunity. Those few seconds exist so a rescue is
    -- possible at all, and offering the give-up button first is the wrong thing to put in
    -- front of somebody who has just hit the floor.
    local immune = findAura(IMMUNE_SPELL)
    if immune then button:Disable() else button:Enable() end
end

frame:SetScript("OnEvent", function(_, _, unit)
    if unit == nil or unit == "player" then
        refresh()
    end
end)
frame:RegisterEvent("UNIT_AURA")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")

frame:SetScript("OnUpdate", function(self, elapsed)
    self.since = (self.since or 0) + elapsed

    if self.since < 0.2 then
        return
    end
    self.since = 0

    local _, expires = findAura(BLEED_OUT_SPELL)

    if not expires then
        self:Hide()
        return
    end

    local left = expires - GetTime()

    if left < 0 then
        left = 0
    end

    -- Floored explicitly rather than left to %d: Lua 5.1 truncates a float there, but
    -- saying so costs nothing and does not depend on that staying true.
    timer:SetText(string.format("%d:%02d before it is over",
        math.floor(left / 60), math.floor(left % 60)))

    -- Re-checked here as well as on UNIT_AURA: the immunity ending is an aura EXPIRING, and
    -- an expiry does not always fire the event on the client.
    if findAura(IMMUNE_SPELL) then button:Disable() else button:Enable() end
end)


--[[
    /downed - say what this addon can see.

    The window only appears while the "Bleeding Out" aura is on the player, so an empty
    screen is ambiguous: the addon might not be loaded, the aura might not be arriving, or
    the player might simply not be down. This answers all three without needing to die.

    `/downed show` forces the frame up regardless, to check the artwork on its own.
]]
SLASH_SANCTUARYDOWNED1 = "/downed"

SlashCmdList["SANCTUARYDOWNED"] = function(arg)
    if arg == "show" then
        frame:Show()
        button:Enable()
        timer:SetText("(forced open - not actually down)")
        DEFAULT_CHAT_FRAME:AddMessage("SanctuaryDowned: frame forced open.")
        return
    end

    DEFAULT_CHAT_FRAME:AddMessage("SanctuaryDowned: loaded.")

    local bleedDuration, bleedExpires = findAura(BLEED_OUT_SPELL)
    local immuneDuration = findAura(IMMUNE_SPELL)

    if bleedExpires then
        DEFAULT_CHAT_FRAME:AddMessage(string.format(
            "  Bleeding Out (%d): %d seconds left.", BLEED_OUT_SPELL,
            math.floor(bleedExpires - GetTime())))
    else
        DEFAULT_CHAT_FRAME:AddMessage(string.format(
            "  Bleeding Out (%d): not on you. The window only shows while it is.",
            BLEED_OUT_SPELL))
    end

    DEFAULT_CHAT_FRAME:AddMessage(string.format("  Insensible (%d): %s", IMMUNE_SPELL,
        immuneDuration and "on you, so the button stays disabled" or "not on you"))

    DEFAULT_CHAT_FRAME:AddMessage(string.format("  Carried (%d): %s", CARRIED_SPELL,
        findAura(CARRIED_SPELL) and "somebody has you over their shoulder" or "not on you"))

    -- Listing every harmful aura is what distinguishes "the server never applied it" from
    -- "it is there but the id does not match what this addon is looking for".
    local found = 0
    for i = 1, 40 do
        local name, _, _, _, _, _, _, _, _, _, id = UnitAura("player", i, "HARMFUL")
        if not name then break end
        DEFAULT_CHAT_FRAME:AddMessage(string.format("    debuff %d: %s", id or 0, name))
        found = found + 1
    end

    if found == 0 then
        DEFAULT_CHAT_FRAME:AddMessage("    no debuffs on you at all.")
    end
end


--[[
    And the button you get while somebody is carrying you.

    Being carried does not require being down - which is what makes kidnapping possible, and
    the lawman and outlaw modules rather invite it. Somebody still conscious simply climbs
    down; there is no contest to win, only a button to find. Without one they would have to
    know a chat command while the screen moves on its own, which nobody will.

    Withheld while the "Bleeding Out" aura is up, because that is the one case where getting
    down is not theirs to decide. Being unable to is what makes an unconscious person
    carryable against their will, and that is the whole point of the downed state.
]]

local carried = CreateFrame("Frame", "SanctuaryCarriedFrame", UIParent)
carried:SetWidth(220)
carried:SetHeight(74)
carried:SetPoint("CENTER", UIParent, "CENTER", 0, -140)
carried:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
carried:SetFrameStrata("DIALOG")
carried:Hide()

local carriedTitle = carried:CreateFontString(nil, "ARTWORK", "GameFontNormal")
carriedTitle:SetPoint("TOP", carried, "TOP", 0, -16)
carriedTitle:SetText("You are being carried")

local getDown = CreateFrame("Button", nil, carried, "UIPanelButtonTemplate")
getDown:SetWidth(120)
getDown:SetHeight(22)
getDown:SetPoint("BOTTOM", carried, "BOTTOM", 0, 14)
getDown:SetText("Get down")

getDown:SetScript("OnClick", function()
    SendServerCommand("getdown")
end)

getDown:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText("Get down", 1, 1, 1)
    GameTooltip:AddLine("Climb down off their shoulder.", nil, nil, nil, true)
    GameTooltip:Show()
end)
getDown:SetScript("OnLeave", function() GameTooltip:Hide() end)

--[[
    One window at a time.

    Both frames sit in the same place, and while somebody is carrying a downed player both
    auras are on them at once. The downed window wins: it has the clock on it, and getting
    down is not on offer to anybody wearing it.
]]
local function refreshCarried()
    if findAura(CARRIED_SPELL) and not findAura(BLEED_OUT_SPELL) then
        carried:Show()
    else
        carried:Hide()
    end
end

carried:SetScript("OnEvent", function(_, _, unit)
    if unit == nil or unit == "player" then
        refreshCarried()
    end
end)
carried:RegisterEvent("UNIT_AURA")
carried:RegisterEvent("PLAYER_ENTERING_WORLD")

-- Polled as well, for the same reason the timer is: an aura ending is an expiry, and an
-- expiry does not reliably fire UNIT_AURA on this client.
carried:SetScript("OnUpdate", function(self, elapsed)
    self.since = (self.since or 0) + elapsed

    if self.since < 0.5 then
        return
    end
    self.since = 0

    refreshCarried()
end)


--[[
    And the button you get while you are carrying somebody.

    This one cannot read an aura. The passenger wears "Carried" and the two frames above
    watch for it, but a carrier wears nothing at all - they are the vehicle rather than a
    passenger, and 3.3.5a gives the client no way to ask whether anything is riding it.
    UnitInVehicle("player") is false for the carrier, which is the whole difficulty.

    So the server says. It pushes on every change and answers SYNC on demand, because a
    push is missed by anyone who /reloads or who is still on the loading screen when it
    goes out - and a carrier whose button has gone has no way to put the body down.
]]

local ADDON_PREFIX = "SDOWNED"

local carrying = { on = false, name = nil }
local haveCarryState = false

local holder = CreateFrame("Frame", "SanctuaryCarryingFrame", UIParent)
holder:SetWidth(220)
holder:SetHeight(74)
holder:SetPoint("CENTER", UIParent, "CENTER", 0, -140)
holder:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
holder:SetFrameStrata("DIALOG")
holder:Hide()

local holderTitle = holder:CreateFontString(nil, "ARTWORK", "GameFontNormal")
holderTitle:SetPoint("TOP", holder, "TOP", 0, -16)
holderTitle:SetText("You are carrying someone")

local putDown = CreateFrame("Button", nil, holder, "UIPanelButtonTemplate")
putDown:SetWidth(140)
putDown:SetHeight(22)
putDown:SetPoint("BOTTOM", holder, "BOTTOM", 0, 14)
putDown:SetText("Put them down")

putDown:SetScript("OnClick", function()
    SendServerCommand("carry drop")
end)

putDown:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText("Put them down", 1, 1, 1)
    GameTooltip:AddLine("Set them back on their feet where you stand.", nil, nil, nil, true)
    GameTooltip:Show()
end)
putDown:SetScript("OnLeave", function() GameTooltip:Hide() end)

--[[
    The carried and downed windows sit in the same place as this one, and the downed one
    carries a clock. None of the three can honestly co-occur - you cannot be carried by
    somebody while carrying them - but a stale state on either side would put two frames
    on top of each other, so this one yields rather than overlaps.
]]
local function refreshCarrying()
    if carrying.on and not findAura(CARRIED_SPELL) and not findAura(BLEED_OUT_SPELL) then
        holderTitle:SetText(carrying.name
            and ("You are carrying " .. carrying.name)
            or "You are carrying someone")
        holder:Show()
    else
        holder:Hide()
    end
end

local function handleCarryState(body)
    -- "on=1 name=Someone", with the name last and free to contain spaces.
    --
    -- It is whatever the identity module says the carrier may call them, not their real
    -- name, and a stranger's label is their race - so "Night Elf" and "Blood Elf" are
    -- ordinary values here. Matching %S+ took the first word and put "Night" on the frame.
    local on = string.match(body, "on=(%d)")
    local name = string.match(body, "name=(.+)$")

    carrying.on = (on == "1")
    carrying.name = carrying.on and name or nil
    haveCarryState = true

    refreshCarrying()
end

holder:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "CHAT_MSG_ADDON" then
        if arg1 ~= ADDON_PREFIX then return end

        local verb, body = string.match(arg2, "^(%u+)%s*(.*)$")
        if verb == "CARRY" then
            handleCarryState(body)
        end
        return
    end

    -- PLAYER_ENTERING_WORLD: nothing is known yet, and the ask below starts immediately.
    haveCarryState = false
    carrying.on = false
    refreshCarrying()
end)

holder:RegisterEvent("CHAT_MSG_ADDON")
holder:RegisterEvent("PLAYER_ENTERING_WORLD")

--[[
    Ask until answered.

    Fast while nothing is known, because that is the window in which a carrier is looking
    for a button that is not there; slow afterwards, since every change is pushed and this
    is only a safety net against a push that went missing.
]]
local carrySince = 0
holder:SetScript("OnUpdate", function(self, elapsed)
    carrySince = carrySince + elapsed

    if carrySince < (haveCarryState and 15 or 2) then
        return
    end
    carrySince = 0

    SendAddonMessage(ADDON_PREFIX, "SYNC", "WHISPER", UnitName("player"))
end)
