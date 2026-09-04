--[[
    Sanctuary Strongbox

    The window for a shared strongbox. Pick an item up out of your bags and click a slot to
    put it in; click a full slot to take it out.

    The window never decides anything. It shows the last thing the server said, and every
    click is a request the server is free to refuse - the box may be locked, you may have
    walked away from it, somebody else may have taken that item a second before you clicked.
    A window that updated itself optimistically would show people items that are not there.
]]

local ADDON_PREFIX = "SSTASH"

local COLS = 8
local SLOT_SIZE = 37
local PADDING = 5

local playerName
local frame
local buttons = {}
local slotCount = 0

-- Declared up here rather than beside the panel it belongs to. A local is invisible to code
-- written above it, so with the declaration further down, Handle() was assigning a GLOBAL of
-- the same name while the slash command read this one - and the panel never opened for
-- anybody.
local isGM = false

-- The locksmith's bench, declared here rather than beside its own code. Handle() opens it,
-- and Handle() is written above it - a local is invisible to code higher up the file, so
-- leaving these where they belong would have made Handle talk to two nil globals of the same
-- name. Exactly the way isGM above failed once already.
local bench
local benchPrice = 0
local OpenBench

-- Where the item on the cursor was picked up from. The cursor itself will not say - it
-- knows what it is holding but not where it came from - so the only way to tell the server
-- which stack to move is to watch it leave.
local heldBag, heldSlot

--------------------------------------------------------------------------
-- Talking to the server
--------------------------------------------------------------------------

StaticPopupDialogs["SANCTUARY_STASH_PUT_MONEY"] = {
    text = "How much do you want to put in?",
    button1 = ACCEPT,
    button2 = CANCEL,
    hasMoneyInputFrame = 1,
    timeout = 0,
    hideOnEscape = 1,
    OnAccept = function(self)
        local copper = MoneyInputFrame_GetCopper(self.moneyInputFrame)
        if copper and copper > 0 then
            SendAddonMessage("SSTASH", "PUTMONEY " .. copper, "WHISPER", UnitName("player"))
        end
    end,
}

StaticPopupDialogs["SANCTUARY_STASH_TAKE_MONEY"] = {
    text = "How much do you want to take out?",
    button1 = ACCEPT,
    button2 = CANCEL,
    hasMoneyInputFrame = 1,
    timeout = 0,
    hideOnEscape = 1,
    OnAccept = function(self)
        local copper = MoneyInputFrame_GetCopper(self.moneyInputFrame)
        if copper and copper > 0 then
            SendAddonMessage("SSTASH", "TAKEMONEY " .. copper, "WHISPER", UnitName("player"))
        end
    end,
}

local function Send(message)
    SendAddonMessage(ADDON_PREFIX, message, "WHISPER", playerName or UnitName("player"))
end

--------------------------------------------------------------------------
-- The window
--------------------------------------------------------------------------

local function SlotClicked(self)
    local slot = self:GetID()

    if CursorHasItem() then
        if heldBag and heldSlot then
            Send(string.format("PUT %d %d %d", heldBag, heldSlot, slot))
        end

        ClearCursor()
        heldBag, heldSlot = nil, nil
        return
    end

    if self.entry then
        Send("TAKE " .. slot)
    end
end

local function SlotEnter(self)
    if not self.entry then return end

    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetHyperlink("item:" .. self.entry .. ":0:0:0:0:0:0:0")
    GameTooltip:Show()
end

local function BuildFrame()
    if frame then return end

    frame = CreateFrame("Frame", "SanctuaryStashFrame", UIParent)
    frame:SetFrameStrata("HIGH")
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:SetPoint("CENTER", UIParent, "CENTER", 200, 0)
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    frame:Hide()

    frame.title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    frame.title:SetPoint("TOP", frame, "TOP", 10, -16)

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -8, -8)
    close:SetScript("OnClick", function()
        Send("CLOSE")
        frame:Hide()
    end)

    -- The lock, for whoever is carrying the key. Hidden entirely for anyone who is not:
    -- a button that only ever refuses is worse than no button.
    local lock = CreateFrame("Button", nil, frame)
    lock:SetWidth(26)
    lock:SetHeight(26)
    lock:SetPoint("TOPLEFT", frame, "TOPLEFT", 14, -12)
    lock:Hide()

    lock.icon = lock:CreateTexture(nil, "ARTWORK")
    lock.icon:SetAllPoints()
    lock.icon:SetTexture("Interface\\Icons\\INV_Misc_Key_03")

    lock:SetScript("OnClick", function() Send("LOCK") end)
    lock:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText(frame.locked and "Locked" or "Unlocked")
        GameTooltip:AddLine(frame.locked
            and "Only somebody carrying a key can open it. Click to leave it open to anyone."
            or "Anyone may open it. Click to lock it again.", 1, 1, 1, true)
        GameTooltip:Show()
    end)
    lock:SetScript("OnLeave", function() GameTooltip:Hide() end)

    frame.lock = lock

    frame.money = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    frame.money:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 18, 18)
    frame.money:SetText(GetCoinTextureString(0))

    local put = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    put:SetWidth(70)
    put:SetHeight(20)
    put:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -16, 14)
    put:SetText("Put in")
    put:SetScript("OnClick", function() StaticPopup_Show("SANCTUARY_STASH_PUT_MONEY") end)

    local take = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    take:SetWidth(70)
    take:SetHeight(20)
    take:SetPoint("RIGHT", put, "LEFT", -4, 0)
    take:SetText("Take out")
    take:SetScript("OnClick", function() StaticPopup_Show("SANCTUARY_STASH_TAKE_MONEY") end)

    -- Walking away has to close it, because the server stops answering at that distance and
    -- a window that still looks open is a window that lies. The server decides how far is
    -- too far and replies SHUT; this only asks, once a second, and does as it is told.
    frame:SetScript("OnUpdate", function(self, elapsed)
        self.since = (self.since or 0) + elapsed
        if self.since < 1 then return end
        self.since = 0
        Send("SYNC")
    end)
end

local function EnsureSlots(count)
    BuildFrame()

    for index = 1, count do
        if not buttons[index] then
            local button = CreateFrame("Button", "SanctuaryStashSlot" .. index, frame, "ItemButtonTemplate")
            button:SetID(index - 1)                      -- the server counts from zero
            button:SetScript("OnClick", SlotClicked)
            button:SetScript("OnEnter", SlotEnter)
            button:SetScript("OnLeave", function() GameTooltip:Hide() end)
            button:SetScript("OnReceiveDrag", SlotClicked)
            buttons[index] = button
        end

        local button = buttons[index]
        local col = (index - 1) % COLS
        local row = math.floor((index - 1) / COLS)

        button:ClearAllPoints()
        button:SetPoint("TOPLEFT", frame, "TOPLEFT",
            16 + col * (SLOT_SIZE + PADDING),
            -40 - row * (SLOT_SIZE + PADDING))
        button:Show()
    end

    for index = count + 1, #buttons do
        buttons[index]:Hide()
    end

    local rows = math.ceil(count / COLS)
    frame:SetWidth(32 + COLS * (SLOT_SIZE + PADDING))
    frame:SetHeight(90 + rows * (SLOT_SIZE + PADDING))    -- 34 of that is the coin row
end

local function SetSlot(index, entry, count)
    local button = buttons[index + 1]
    if not button then return end

    button.entry = entry

    if not entry then
        SetItemButtonTexture(button, nil)
        SetItemButtonCount(button, 0)
        return
    end

    local _, _, _, _, _, _, _, _, _, texture = GetItemInfo(entry)
    SetItemButtonTexture(button, texture or "Interface\\Icons\\INV_Misc_QuestionMark")
    SetItemButtonCount(button, count or 1)
end

local function ClearAll()
    for index = 0, slotCount - 1 do
        SetSlot(index, nil, 0)
    end
end

--------------------------------------------------------------------------
-- What the server says
--------------------------------------------------------------------------

local function Handle(message)
    local verb, rest = string.match(message, "^(%S+)%s*(.*)$")

    if verb == "OPEN" then
        local _, slots, name = string.match(rest, "^(%d+)%s+(%d+)%s+(.*)$")
        slotCount = tonumber(slots) or 28
        EnsureSlots(slotCount)
        ClearAll()
        frame.title:SetText(name ~= "" and name or "Strongbox")
        frame:Show()

    elseif verb == "SLOT" then
        local slot, entry, count = string.match(rest, "^(%d+)%s+(%d+)%s+(%d+)$")
        if slot then
            SetSlot(tonumber(slot), tonumber(entry), tonumber(count))
        end

    elseif verb == "COPY" then
        OpenBench(tonumber(rest))

    elseif verb == "COPIED" then
        -- Cut. The bench is cleared rather than closed, so a second key can go straight on.
        if bench then
            bench.bag, bench.slot = nil, nil
            SetItemButtonTexture(bench.slot_button, nil)
            bench.copy:Disable()
        end

    elseif verb == "COPYSHUT" then
        if bench then bench:Hide() end

    elseif verb == "LOCKED" then
        local locked, mine = string.match(rest, "^(%d)%s+(%d)$")

        if locked and frame then
            frame.locked = locked == "1"

            -- Desaturated when the box is standing open, so the state reads at a glance
            -- rather than only on hover.
            frame.lock.icon:SetDesaturated(not frame.locked)

            if mine == "1" then
                frame.lock:Show()
            else
                frame.lock:Hide()
            end
        end

    elseif verb == "MONEY" then
        local copper = tonumber(rest)
        if copper and frame then
            frame.money:SetText(GetCoinTextureString(copper))
        end

    elseif verb == "TAKEN" then
        local slot = tonumber(rest)
        if slot then SetSlot(slot, nil, 0) end

    elseif verb == "SHUT" then
        if frame then frame:Hide() end

    elseif verb == "GMOK" then
        -- The server only sends this to a game master, so it is the whole of the client's
        -- permission check. The commands behind the buttons are gated again server-side.
        isGM = true
    end
end



--------------------------------------------------------------------------
-- The locksmith's bench
--------------------------------------------------------------------------

--[[
    Put a key on the bench and he cuts you another. The window is only an interface: it
    reports which bag slot the key is sitting in and nothing else. Whether the thing there is
    really a key, whether it opens anything, whether you can pay, and whether you are still
    standing at the locksmith are all the server's to decide.
]]

local function BuildBench()
    if bench then return end

    bench = CreateFrame("Frame", "SanctuaryStashBenchFrame", UIParent)
    bench:SetWidth(250)
    bench:SetHeight(150)
    bench:SetPoint("CENTER", UIParent, "CENTER", 0, 60)
    bench:SetFrameStrata("DIALOG")
    bench:SetMovable(true)
    bench:EnableMouse(true)
    bench:RegisterForDrag("LeftButton")
    bench:SetScript("OnDragStart", bench.StartMoving)
    bench:SetScript("OnDragStop", bench.StopMovingOrSizing)
    bench:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    bench:Hide()

    local title = bench:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", bench, "TOP", 0, -16)
    title:SetText("Copy a key")

    local close = CreateFrame("Button", nil, bench, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", bench, "TOPRIGHT", -8, -8)
    close:SetScript("OnClick", function()
        Send("COPYSHUT")
        bench:Hide()
    end)

    -- The slot the key goes on. Cursor-based rather than drag-and-drop, for the same reason
    -- the strongbox slots are: the cursor knows what it is holding but not where it came
    -- from, and the server needs the bag and slot to know WHICH key is being copied.
    local slot = CreateFrame("Button", "SanctuaryStashBenchSlot", bench, "ItemButtonTemplate")
    slot:SetPoint("TOPLEFT", bench, "TOPLEFT", 24, -44)

    local function PlaceKey()
        if not CursorHasItem() then return end

        if heldBag and heldSlot then
            bench.bag, bench.slot = heldBag, heldSlot

            local texture = GetContainerItemInfo(heldBag, heldSlot)
            SetItemButtonTexture(slot, texture or "Interface\\Icons\\INV_Misc_Key_03")
            bench.copy:Enable()
        end

        ClearCursor()
        heldBag, heldSlot = nil, nil
    end

    slot:SetScript("OnClick", PlaceKey)
    slot:SetScript("OnReceiveDrag", PlaceKey)
    slot:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText("The key to copy")
        GameTooltip:AddLine("Pick a key up out of your bags and click here.", 1, 1, 1, true)
        GameTooltip:Show()
    end)
    slot:SetScript("OnLeave", function() GameTooltip:Hide() end)
    bench.slot_button = slot

    bench.cost = bench:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    bench.cost:SetPoint("LEFT", slot, "RIGHT", 16, 0)

    -- Walking away from the trainer has to close the bench, for the same reason the
    -- strongbox closes when you leave it: the server stops answering at that distance, and
    -- a window that still looks like a locksmith is a window that lies. The server decides
    -- how far is too far and replies COPYSHUT. OnUpdate does not run on a hidden frame, so
    -- the asking stops on its own when the bench is put away.
    bench:SetScript("OnUpdate", function(self, elapsed)
        self.since = (self.since or 0) + elapsed
        if self.since < 1 then return end
        self.since = 0
        Send("ATSMITH")
    end)

    local copy = CreateFrame("Button", nil, bench, "UIPanelButtonTemplate")
    copy:SetWidth(110)
    copy:SetHeight(22)
    copy:SetPoint("BOTTOM", bench, "BOTTOM", 0, 18)
    copy:SetText("Cut a copy")
    copy:Disable()
    copy:SetScript("OnClick", function()
        if bench.bag and bench.slot then
            Send(string.format("COPYKEY %d %d", bench.bag, bench.slot))
        end
    end)
    bench.copy = copy
end

OpenBench = function(price)
    BuildBench()

    benchPrice = price or 0
    bench.bag, bench.slot = nil, nil
    bench.cost:SetText(GetCoinTextureString(benchPrice))
    SetItemButtonTexture(bench.slot_button, nil)
    bench.copy:Disable()
    bench:Show()
end

--------------------------------------------------------------------------
-- The key on the minimap
--------------------------------------------------------------------------

--[[
    A key that turns the lock of whichever strongbox you are standing at, if you are carrying
    its key. It lives on the minimap rather than on the strongbox window for a plain reason:
    a locked box you hold the key to has no window open yet, and that is precisely the moment
    you want to unlock it.

    The button decides nothing. It sends LOCK and the server works out which box you are at,
    whether you hold its key, and which way the lock should turn.
]]

local minimapButton

local function PlaceOnRing(angle)
    -- 80 is the radius the stock minimap buttons sit at. cos/sin in this client take degrees.
    minimapButton:SetPoint("CENTER", Minimap, "CENTER",
        80 * cos(angle), 80 * sin(angle))
end

local function BuildMinimapButton()
    if minimapButton then return end

    minimapButton = CreateFrame("Button", "SanctuaryStashMinimapButton", Minimap)
    minimapButton:SetWidth(31)
    minimapButton:SetHeight(31)
    minimapButton:SetFrameStrata("MEDIUM")
    minimapButton:SetFrameLevel(8)
    minimapButton:RegisterForClicks("LeftButtonUp")
    minimapButton:RegisterForDrag("LeftButton")
    minimapButton:SetMovable(true)

    local icon = minimapButton:CreateTexture(nil, "BACKGROUND")
    icon:SetWidth(20)
    icon:SetHeight(20)
    icon:SetTexture("Interface\\Icons\\INV_Misc_Key_03")
    icon:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 7, -5)
    minimapButton.icon = icon

    local border = minimapButton:CreateTexture(nil, "OVERLAY")
    border:SetWidth(53)
    border:SetHeight(53)
    border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    border:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 0, 0)

    minimapButton:SetScript("OnClick", function() Send("LOCK") end)

    minimapButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_LEFT")
        GameTooltip:SetText("Strongbox key")
        GameTooltip:AddLine("Turns the lock of the strongbox you are standing at, if you are "
            .. "carrying its key. Drag to move it around the minimap.", 1, 1, 1, true)
        GameTooltip:Show()
    end)
    minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

    -- Dragged around the ring rather than freely, so it behaves like every other minimap
    -- button and cannot be lost behind the world frame.
    minimapButton:SetScript("OnDragStart", function(self)
        self:SetScript("OnUpdate", function()
            local mx, my = Minimap:GetCenter()
            local cx, cy = GetCursorPosition()
            local scale = UIParent:GetEffectiveScale()

            cx, cy = cx / scale, cy / scale

            SanctuaryStashDB = SanctuaryStashDB or {}
            SanctuaryStashDB.minimapAngle = math.deg(math.atan2(cy - my, cx - mx))

            PlaceOnRing(SanctuaryStashDB.minimapAngle)
        end)
    end)

    minimapButton:SetScript("OnDragStop", function(self)
        self:SetScript("OnUpdate", nil)
    end)

    SanctuaryStashDB = SanctuaryStashDB or {}
    -- Measured off a live minimap, not guessed. The four Sanctuary buttons share one arc:
    -- outlaw -113.04, stash -132.70, disguise -151.64, profile -171.25.
    PlaceOnRing(SanctuaryStashDB.minimapAngle or -132.70)
end

--------------------------------------------------------------------------
-- The game master's side
--------------------------------------------------------------------------

--[[
    Placing and keying boxes was SQL by hand and a restart. This is the same work as
    buttons, and it does nothing the chat commands cannot: every click sends a `.stash`
    line, and every one of those is RBAC-gated on the server. Editing this file to show the
    panel therefore gets an unauthorised player a panel that refuses everything.

    The panel only appears at all if the server answers GMHELLO, which it does for
    SEC_GAMEMASTER and above.
]]

local gmPanel

-- Commands go out over AzerothCore's addon command channel rather than as a chat line
-- starting with a dot. An unrecognised dot command falls through and is published to /say,
-- which would announce the realm's strongbox layout to the room.
local commandCounter = 0

local function SendServerCommand(command)
    commandCounter = (commandCounter + 1) % 10000
    SendAddonMessage("AzerothCore",
        string.format("i%04d%s", commandCounter, command),
        "WHISPER",
        playerName or UnitName("player"))
end

-- entry, icon, name. One per INV_Misc_Key the client carries; they are a fixed palette
-- because item entries cannot be created while the server runs.
--
-- The names must match item_template, and they are named after what the icon actually
-- shows rather than its number - the two drifted apart once already, which is how a
-- purple key came to be called Brass. If a name changes there, change it here too:
-- this table is what the game master panel labels its buttons with.
local KEYS = {
    { 990020, "INV_Misc_Key_01", "Ruby Key" },
    { 990021, "INV_Misc_Key_02", "Amethyst Key" },
    { 990022, "INV_Misc_Key_03", "Quartz Key" },
    { 990023, "INV_Misc_Key_04", "Topaz Key" },
    { 990024, "INV_Misc_Key_05", "Brass Key" },
    { 990025, "INV_Misc_Key_06", "Iron Key" },
    { 990026, "INV_Misc_Key_07", "Fine Key" },
    { 990027, "INV_Misc_Key_08", "Frostbound Key" },
    { 990028, "INV_Misc_Key_09", "Curved Key" },
    { 990029, "INV_Misc_Key_10", "Shell Key" },
    { 990030, "INV_Misc_Key_11", "Skeleton Key" },
    { 990031, "INV_Misc_Key_12", "Strongbox Key" },
    { 990032, "INV_Misc_Key_13", "Grim Key" },
    { 990033, "INV_Misc_Key_14", "Silver Key" },
    { 990034, "INV_Misc_Key_15", "Rune-cut Key" },
}

local KEY_COLS = 8

local function BuildGMPanel()
    if gmPanel then return end

    gmPanel = CreateFrame("Frame", "SanctuaryStashGMFrame", UIParent)
    -- Sized for what is in it rather than by eye. Four buttons were previously anchored
    -- three-from-the-left and one-from-the-right in a 360 wide panel, and Remove sat on top
    -- of Reload by 42 pixels - which is the sort of thing that is obvious in the game and
    -- invisible in the source.
    gmPanel:SetWidth(360)
    gmPanel:SetHeight(282)
    gmPanel:SetPoint("CENTER", UIParent, "CENTER", -260, 0)
    gmPanel:SetFrameStrata("HIGH")
    gmPanel:SetMovable(true)
    gmPanel:EnableMouse(true)
    gmPanel:RegisterForDrag("LeftButton")
    gmPanel:SetScript("OnDragStart", gmPanel.StartMoving)
    gmPanel:SetScript("OnDragStop", gmPanel.StopMovingOrSizing)
    gmPanel:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    gmPanel:Hide()

    local title = gmPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", gmPanel, "TOP", 0, -16)
    title:SetText("Strongbox tools")

    local close = CreateFrame("Button", nil, gmPanel, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", gmPanel, "TOPRIGHT", -8, -8)
    close:SetScript("OnClick", function() gmPanel:Hide() end)

    local nameLabel = gmPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    nameLabel:SetPoint("TOPLEFT", gmPanel, "TOPLEFT", 20, -44)
    nameLabel:SetText("Name")

    local nameBox = CreateFrame("EditBox", "SanctuaryStashGMName", gmPanel, "InputBoxTemplate")
    nameBox:SetWidth(220)
    nameBox:SetHeight(20)
    nameBox:SetPoint("TOPLEFT", nameLabel, "BOTTOMLEFT", 6, -4)
    nameBox:SetAutoFocus(false)
    nameBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
    gmPanel.nameBox = nameBox

    -- A name with no text is not an error: `.stash place` falls back to "Strongbox".
    local place = CreateFrame("Button", nil, gmPanel, "UIPanelButtonTemplate")
    place:SetWidth(150)
    place:SetHeight(22)
    place:SetPoint("TOPLEFT", nameBox, "BOTTOMLEFT", -6, -8)
    place:SetText("Place one here")
    place:SetScript("OnClick", function()
        SendServerCommand("stash place " .. (nameBox:GetText() or ""))
    end)

    local rename = CreateFrame("Button", nil, gmPanel, "UIPanelButtonTemplate")
    rename:SetWidth(90)
    rename:SetHeight(22)
    rename:SetPoint("LEFT", place, "RIGHT", 6, 0)
    rename:SetText("Rename")
    rename:SetScript("OnClick", function()
        local text = nameBox:GetText()
        if text and text ~= "" then
            SendServerCommand("stash name " .. text)
        end
    end)

    local keyLabel = gmPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    keyLabel:SetPoint("TOPLEFT", place, "BOTTOMLEFT", 6, -12)
    keyLabel:SetText("Lock the box you are standing at:")

    for index, key in ipairs(KEYS) do
        local entry, icon, label = key[1], key[2], key[3]

        local button = CreateFrame("Button", "SanctuaryStashKey" .. index, gmPanel)
        button:SetWidth(28)
        button:SetHeight(28)

        local col = (index - 1) % KEY_COLS
        local row = math.floor((index - 1) / KEY_COLS)
        button:SetPoint("TOPLEFT", keyLabel, "BOTTOMLEFT", col * 32, -6 - row * 32)

        local texture = button:CreateTexture(nil, "ARTWORK")
        texture:SetAllPoints()
        texture:SetTexture("Interface\\Icons\\" .. icon)

        button:SetScript("OnClick", function()
            SendServerCommand("stash key " .. entry)
        end)
        button:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetText(label)
            GameTooltip:AddLine("Locks the strongbox you are standing at, and puts a copy in your pack.", 1, 1, 1, true)
            GameTooltip:Show()
        end)
        button:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    -- Upper row: the three harmless ones, left to right. 20 + 78 + 6 + 64 + 6 + 78 = 252,
    -- inside the 360 the panel is wide.
    local unlock = CreateFrame("Button", nil, gmPanel, "UIPanelButtonTemplate")
    unlock:SetWidth(92)
    unlock:SetHeight(22)
    unlock:SetPoint("BOTTOMLEFT", gmPanel, "BOTTOMLEFT", 20, 46)
    unlock:SetText("Lock/Unlock")
    -- No argument: the server toggles, putting back whatever key the box last had. Naming a
    -- key is what the icons below are for.
    unlock:SetScript("OnClick", function() SendServerCommand("stash key") end)

    local list = CreateFrame("Button", nil, gmPanel, "UIPanelButtonTemplate")
    list:SetWidth(64)
    list:SetHeight(22)
    list:SetPoint("LEFT", unlock, "RIGHT", 6, 0)
    list:SetText("List")
    list:SetScript("OnClick", function() SendServerCommand("stash list") end)

    local reload = CreateFrame("Button", nil, gmPanel, "UIPanelButtonTemplate")
    reload:SetWidth(78)
    reload:SetHeight(22)
    reload:SetPoint("LEFT", list, "RIGHT", 6, 0)
    reload:SetText("Reload")
    reload:SetScript("OnClick", function() SendServerCommand("stash reload") end)

    -- No confirmation, because there is nothing to confirm: the server refuses outright
    -- while the box holds anything, so this button either removes an empty box or is
    -- politely declined.
    local remove = CreateFrame("Button", nil, gmPanel, "UIPanelButtonTemplate")
    remove:SetWidth(120)
    remove:SetHeight(22)
    remove:SetPoint("BOTTOMLEFT", gmPanel, "BOTTOMLEFT", 20, 16)
    remove:SetText("Remove box")
    remove:SetScript("OnClick", function() SendServerCommand("stash remove") end)
    remove:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText("Remove the strongbox you are standing at")
        GameTooltip:AddLine("Refused while it still holds items or coin - empty it first.",
            1, 1, 1, true)
        GameTooltip:Show()
    end)
    remove:SetScript("OnLeave", function() GameTooltip:Hide() end)
end

SLASH_SANCTUARYSTASH1 = "/strongbox"
SLASH_SANCTUARYSTASH2 = "/stash"

SlashCmdList["SANCTUARYSTASH"] = function()
    if not isGM then
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff96Strongbox:|r these tools are for game masters.")
        return
    end

    BuildGMPanel()

    if gmPanel:IsShown() then
        gmPanel:Hide()
    else
        gmPanel:Show()
    end
end

--------------------------------------------------------------------------

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")

listener:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "PLAYER_ENTERING_WORLD" then
        playerName = UnitName("player")
        BuildMinimapButton()

        -- Asked at login rather than when /stash is first typed, so the keystroke is not
        -- swallowed while the answer is still in flight.
        -- Announced at login so the locksmith knows to offer the bench rather than the
        -- gossip list, before anybody walks up to one.
        Send("HELLO")
        Send("GMHELLO")
        return
    end

    if event == "CHAT_MSG_ADDON" and arg1 == ADDON_PREFIX then
        Handle(arg2)
    end
end)

-- Remembering where a picked-up item came from. PickupContainerItem is what every route out
-- of a bag ends up calling - dragging, clicking, the keybind - so one hook covers them all.
hooksecurefunc("PickupContainerItem", function(bag, slot)
    heldBag, heldSlot = bag, slot
end)
