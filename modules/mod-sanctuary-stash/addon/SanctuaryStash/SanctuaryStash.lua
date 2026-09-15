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
-- Picking up what the server just handed over
--------------------------------------------------------------------------

--[[
    Waits for an item to appear in a bag slot, then puts it on the cursor.

    The client will not hand a strongbox item to the cursor - pickup is its own code and
    only containers it owns are exposed - so the server takes the item out first and says
    where it landed. The catch is that "where it landed" arrives before the item does.

    A frame watcher rather than BAG_UPDATE, because BAG_UPDATE fires for the bag as a whole
    and would have to be filtered anyway; polling one slot for a few frames is smaller and
    says what it is waiting for.
]]
-- Named so the harness can drive it: the timing this guards cannot be tested any other
-- way, because it is entirely about which of two packets arrives first.
local cursorWatcher = CreateFrame("Frame", "SanctuaryStashCursorWatcher")
local cursorWait = nil

local CURSOR_TIMEOUT = 1.5

cursorWatcher:Hide()

cursorWatcher:SetScript("OnUpdate", function(self, elapsed)
    if not cursorWait then
        self:Hide()
        return
    end

    cursorWait.waited = cursorWait.waited + (elapsed or 0)

    local texture = GetContainerItemInfo(cursorWait.bag, cursorWait.slot)

    if texture then
        -- Not if something is already held: the player picked something else up while we
        -- were waiting, and replacing it would drop whatever that was.
        if not CursorHasItem() then
            PickupContainerItem(cursorWait.bag, cursorWait.slot)
        end

        cursorWait = nil
        self:Hide()
        return
    end

    if cursorWait.waited > CURSOR_TIMEOUT then
        cursorWait = nil
        self:Hide()
    end
end)

function WaitForItem(bag, slot)
    if not bag or not slot then return end

    cursorWait = { bag = bag, slot = slot, waited = 0 }
    cursorWatcher:Show()
end

--------------------------------------------------------------------------
-- The window
--------------------------------------------------------------------------

--[[
    Which strongbox slot the window is holding, if any.

    The bank puts the item on the cursor and lets you drop it in another bank slot. The
    client will not do that for a strongbox - pickup is exposed only for containers it owns
    - so the window holds it instead: click to lift, click again to place. The item never
    moves until the second click, and never leaves the box at all.

    This is the answer to needing a free bag slot to rearrange a full box, which was the
    thing that made taking-it-out-and-putting-it-back useless precisely when it mattered.
]]
local heldStashSlot = nil

-- Set by the window so the bag-click override can see it without reaching backwards
-- through the file. Returned rather than exposed, because nothing outside should set it.
local function HeldSlot() return heldStashSlot end

local function SetHeld(slot)
    heldStashSlot = slot

    for index = 0, slotCount - 1 do
        local button = buttons[index + 1]

        if button then
            -- Faded rather than emptied: it has not moved yet, and showing it gone would
            -- be a lie until the server says so.
            SetItemButtonDesaturated(button, slot ~= nil and index == slot)
        end
    end
end

--[[
    Splitting a stack, using the game's own dialog.

    OpenStackSplitFrame hands the frame it is given back through SplitStack when the player
    accepts, so a button of ours can raise the real dialog rather than imitating one - the
    slider, the arrows, Enter and Escape all behave as they do for a bag, because they ARE
    the ones from a bag.

    The split happens inside the box. Doing it by hand would mean taking the stack out,
    splitting it in the bags and putting both halves back, which needs two free bag slots
    and is refused on a full one.
]]
local function SlotSplitAccepted(self, count)
    if count and count > 0 then
        Send(string.format("SPLIT %d %d", self:GetID(), count))
    end
end

local function SlotClicked(self, button)
    local slot = self:GetID()

    -- Shift-click splits, the way it does everywhere else, and NEVER does anything else.
    -- Returning unconditionally matters: without it a shift-click on a single item fell
    -- through to the lift below, so the slot was quietly picked up by a gesture that was
    -- asking to divide it - and stayed held, aimed at whatever was clicked next.
    if IsShiftKeyDown() and button ~= "RightButton" then
        if self.entry and (self._count or 1) > 1 then
            self.SplitStack = SlotSplitAccepted
            OpenStackSplitFrame(self._count, self, "BOTTOMLEFT", "TOPLEFT")
        end

        return
    end

    if CursorHasItem() then
        if heldBag and heldSlot then
            Send(string.format("PUT %d %d %d", heldBag, heldSlot, slot))
        end

        ClearCursor()
        heldBag, heldSlot = nil, nil
        return
    end

    -- Placing what the window is holding. Empty slot or occupied, the server swaps them.
    if heldStashSlot ~= nil and button ~= "RightButton" then
        local from = heldStashSlot
        SetHeld(nil)

        if from ~= slot then
            Send(string.format("MOVE %d %d", from, slot))
        end

        return
    end

    --[[
        Left click asks for the item ON THE CURSOR, the way it works in a bag.

        Which cannot be done here without the item leaving the box first - the client only
        picks up out of containers it owns - so this is a request rather than an action,
        and the server answers it one of two ways. Room in the bags: the item comes out and
        CURSOR says where it landed. No room: NOROOM comes back and the item is lifted
        INSIDE the box instead, which is the one way of holding it that costs no bag space.

        The fallback is not a lesser version of the gesture. It is the arrangement that
        made rearranging a full box possible in the first place, and a full box is exactly
        when somebody is rearranging one.
    ]]
    if self.entry and button ~= "RightButton" then
        Send("TAKE " .. slot .. " 1")
        return
    end

    if self.entry then
        --[[
            Right click takes it out, and that is ALL it does.

            It used to hand the item to the cursor as well, which was left over from before
            left click could lift anything: back then the cursor was the only way to put an
            item somewhere chosen, so the one gesture that existed had to do both jobs. Now
            that left click lifts, right click is the plain one - the same bargain the bank
            and the bags strike, where right click means "just put it away" and does not
            leave you holding something you then have to find a home for.

            The server can still deliver to the cursor; it is the trailing flag, and the
            CURSOR reply and its watcher above stay for it. Nothing asks for it today.

            OnReceiveDrag arrives with no button argument, so anything that is not
            explicitly RightButton is treated as the left one.
        ]]
        Send("TAKE " .. slot .. " 0")
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

            -- Both buttons, so a slot answers a right-click the same way it answers a
            -- left one. A frame fires OnClick for the left button alone until told
            -- otherwise, which is why right-clicking a slot previously did nothing at all
            -- - SlotClicked never looked at which button was pressed, so the handler was
            -- always willing; it simply was never called.
            --
            -- This is the other half of right-clicking a bag item to put it in: the same
            -- gesture now moves an item either way across the window.
            button:RegisterForClicks("LeftButtonUp", "RightButtonUp")

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

--[[
    An icon the client does not have YET.

    GetItemInfo answers out of the client's own item cache, and that cache only holds what
    this character has already been shown. A strongbox is full of things that have never
    been in a bag, so the first look inside one after logging in - which is every look, on
    the evening of a restart - draws a wall of question marks.

    The server now volunteers that data as it opens a box, so this should never fire. It
    stays as the recovery path for anything the server missed, and because the first
    version of it was wrong in a way worth writing down:

    GetItemInfo does NOT go and fetch. On this client it reads the cache and returns nil if
    the answer is not there, and nothing about the call makes the client go looking - so
    asking again, and again, gets nil forever. What sends CMSG_ITEM_QUERY_SINGLE is a
    TOOLTIP being told to show the item, which is why the icons appeared the moment the
    player hovered one and not a second before. So the hover happens here, on a tooltip
    that is never shown, and the cache is then polled for the answer landing. There is no
    event for that on 3.3.5 - GET_ITEM_INFO_RECEIVED is a later client.
]]
-- Owned by UIParent and anchored nowhere: it exists to make the client speak, not to be
-- looked at.
local scanner = CreateFrame("GameTooltip", "SanctuaryStashItemScanner", nil,
    "GameTooltipTemplate")

local function AskClientAbout(entry)
    if not scanner.SetHyperlink then return end      -- no tooltip, no query to send

    scanner:SetOwner(UIParent, "ANCHOR_NONE")
    scanner:SetHyperlink("item:" .. entry)
    scanner:Hide()
end
-- Named for the same reason as the cursor watcher: what it guards is a race, and a race
-- cannot be tested except by driving the frame that waits on it.
local iconWatcher = CreateFrame("Frame", "SanctuaryStashIconWatcher")
local iconWait = {}          -- [button] = the entry that button is still waiting for
local iconWaited = 0
local iconTick = 0

local ICON_TIMEOUT = 8       -- an answer that has not arrived by now is not coming
local ICON_INTERVAL = 0.2    -- each retry re-asks the server, so not every frame

iconWatcher:Hide()

iconWatcher:SetScript("OnUpdate", function(self, elapsed)
    iconWaited = iconWaited + (elapsed or 0)
    iconTick = iconTick + (elapsed or 0)

    if iconTick < ICON_INTERVAL then return end
    iconTick = 0

    local outstanding = false

    for button, entry in pairs(iconWait) do
        if button.entry ~= entry then
            -- The slot was refilled while we waited; whatever is in it now asked for
            -- itself, and answering with the old item's icon would be worse than nothing.
            iconWait[button] = nil
        else
            local _, _, _, _, _, _, _, _, _, texture = GetItemInfo(entry)

            if texture then
                SetItemButtonTexture(button, texture)
                iconWait[button] = nil
            else
                -- Asked again rather than merely checked: a query can go unanswered, and a
                -- check on its own would wait out the whole timeout for a reply nobody is
                -- sending.
                AskClientAbout(entry)
                outstanding = true
            end
        end
    end

    if not outstanding or iconWaited > ICON_TIMEOUT then
        iconWait = {}
        self:Hide()
    end
end)

local function DrawIcon(button, entry)
    local _, _, _, _, _, _, _, _, _, texture = GetItemInfo(entry)

    if texture then
        iconWait[button] = nil
        SetItemButtonTexture(button, texture)
        return
    end

    -- A placeholder rather than an empty slot, because an empty slot reads as "nothing
    -- here" and something is very much here.
    SetItemButtonTexture(button, "Interface\\Icons\\INV_Misc_QuestionMark")

    AskClientAbout(entry)

    iconWait[button] = entry
    iconWaited = 0
    iconTick = 0
    iconWatcher:Show()
end

local function SetSlot(index, entry, count)
    local button = buttons[index + 1]
    if not button then return end

    button.entry = entry

    -- Kept because the split dialog is opened with the stack's size and there is no other
    -- way to ask a button what it is showing.
    button._count = count

    if not entry then
        iconWait[button] = nil
        SetItemButtonTexture(button, nil)
        SetItemButtonCount(button, 0)
        return
    end

    DrawIcon(button, entry)
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

    elseif verb == "NOROOM" then
        -- The bags are full, so the left click becomes a lift inside the box. Said out
        -- loud: the item staying put looks like the click was ignored, and the fade alone
        -- does not explain why this one behaved differently from the last one.
        local slot = tonumber(rest)

        if slot then
            SetHeld(slot)
            DEFAULT_CHAT_FRAME:AddMessage(
                "|cff00ff96Strongbox:|r your bags are full, so it is being moved inside the box.")
        end

    elseif verb == "CURSOR" then
        --[[
            Where the server put it - but not yet, because the item is not there yet.

            This arrives as a whisper, sent the moment the server has stored the item. The
            item's ARRIVAL travels separately, in the next object update block, and reaches
            the client after this does. Picking up straight away therefore picks up an
            empty slot and the item simply stays in the bag, which is exactly what it did.

            So the slot is watched instead, and the pickup happens on the frame the item
            actually appears. WaitForItem gives up after a moment rather than watching for
            ever: if it never arrives the item is in a bag somewhere and no worse off.
        ]]
        local bag, slot = string.match(rest, "^(%d+) (%d+)$")

        if bag then
            WaitForItem(tonumber(bag), tonumber(slot))
        end

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
    gmPanel:SetHeight(318)
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

    -- The lockpick sits apart from the palette above, and deliberately.
    --
    -- Those buttons lock the box you are standing at WITH that key. A pick cannot be a box's
    -- key: it is cut for nothing, and a lock its own kind opens is not a lock. This one only
    -- puts a pick in your pack, to see what one does to somebody else's box.
    local pickRows = math.ceil(#KEYS / KEY_COLS)

    local pick = CreateFrame("Button", "SanctuaryStashPick", gmPanel)
    pick:SetWidth(28)
    pick:SetHeight(28)
    pick:SetPoint("TOPLEFT", keyLabel, "BOTTOMLEFT", 0, -12 - pickRows * 32)

    local pickIcon = pick:CreateTexture(nil, "ARTWORK")
    pickIcon:SetAllPoints()
    pickIcon:SetTexture("Interface\\Icons\\INV_Misc_EngGizmos_SwissArmy")

    local pickLabel = gmPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    pickLabel:SetPoint("LEFT", pick, "RIGHT", 8, 0)
    pickLabel:SetText("Fragile Lockpick")

    pick:SetScript("OnClick", function() SendServerCommand("stash pick") end)
    pick:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText("Fragile Lockpick")
        GameTooltip:AddLine("Puts one in your pack. It opens any strongbox once and breaks doing it, "
            .. "and is only spent when nothing you carry fits the lock.", 1, 1, 1, true)
        GameTooltip:Show()
    end)
    pick:SetScript("OnLeave", function() GameTooltip:Hide() end)

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


--------------------------------------------------------------------------
-- Right-clicking a bag item puts it in the open box
--------------------------------------------------------------------------

--[[
    The same gesture the bank, the mailbox and the trade window all use.

    Picking an item up and dropping it on a slot still works and always will; this is the
    shortcut for emptying a bag into a box, where doing it a drag at a time is tedious.

    **Why this stands ON the bag buttons rather than in front of Blizzard's handler.**

    The first version replaced ContainerFrameItemButton_OnClick and handed every click it
    did not want back to the original, believing that kept the taint contained. It is the
    opposite: a call made from addon code runs Blizzard's handler tainted, and the use
    inside its right-click is protected - so every ordinary right-click on a bag became
    "SanctuaryStash has been blocked from an action only available to the Blizzard UI".
    Hooking afterwards is no better; by then the potion is drunk.

    So Blizzard's handler is not touched at all. While a box is open, a transparent button
    stands on each bag slot and owns the click. Right-click deposits. Left-click does what
    Blizzard's does - PickupContainerItem, which is not protected - or, with a box item
    held, swaps. With a modifier key down the catchers step aside entirely, so shift-split,
    shift-link and ctrl-dress-up reach Blizzard's own button untainted. Close the box and
    they are gone: nothing about the bags is different when no box is open.
]]

--- The first slot with nothing in it, numbered as the server numbers them, or nil if full.
local function FirstEmptySlot()
    for index = 1, #buttons do
        local button = buttons[index]

        -- Shown matters: buttons past the box's size are kept for reuse but hidden, and
        -- depositing into one would name a slot the server does not believe exists.
        if button:IsShown() and not button.entry then
            return button:GetID()
        end
    end
end

-- bag button -> the catcher standing on it. Created once per button; plates and bag
-- buttons alike are pooled by the client and reused forever.
local catchers = {}

local function BagSlotOf(catcher)
    local bagButton = catcher:GetParent()
    return bagButton:GetParent():GetID(), bagButton:GetID()
end

local function CatcherClicked(self, button)
    -- Should have stepped aside before the click landed; if the key went down inside the
    -- same tenth of a second, doing nothing beats doing the wrong thing.
    if IsModifierKeyDown() then
        return
    end

    local bag, slot = BagSlotOf(self)

    --[[
        A left click while the window is holding a strongbox item completes a swap.

        This is the other half of clicking an item in the box: the bank lets you put what
        you are carrying straight into a bag slot, exchanging it for whatever is there, and
        this is the same gesture. It is one step on the server, so it needs no free bag
        slot - the two items change places.

        An empty bag slot is allowed and means "put it here", which is what the bank does.
        A locked slot is one already in flight and is left alone.
    ]]
    if button ~= "RightButton" and HeldSlot() ~= nil and not CursorHasItem() then
        local _, _, locked = GetContainerItemInfo(bag, slot)

        if not locked then
            local from = HeldSlot()
            SetHeld(nil)

            -- The server counts bag slots from one; the client's buttons count from one
            -- too, so these go across as they are and ToCoreSlot does the translation.
            Send(string.format("SWAP %d %d %d", from, bag, slot))
        end

        return
    end

    if button == "RightButton" and not CursorHasItem() then
        local texture, _, locked = GetContainerItemInfo(bag, slot)

        -- An empty slot, or one already in flight, is not ours to take - and with the box
        -- open there is nothing else a right-click should do, so it does nothing.
        if texture and not locked then
            local target = FirstEmptySlot()

            if not target then
                DEFAULT_CHAT_FRAME:AddMessage("|cffff4040The strongbox is full.|r")
                return
            end

            -- The same message the drag route sends, so both ways in are one path on the
            -- server and cannot come to disagree about the rules.
            Send(string.format("PUT %d %d %d", bag, slot, target))
        end

        return
    end

    -- Everything else is what Blizzard's own unmodified click is: pick the item up, or
    -- put down what the cursor holds. Not protected, so ours to call.
    PickupContainerItem(bag, slot)
end

local function CatcherFor(bagButton)
    local catcher = catchers[bagButton]

    if catcher then
        return catcher
    end

    -- A child of the bag button, so it is placed, shown and hidden with it and draws over
    -- it. Being a child of a secure frame is fine; the parent's own scripts stay secure.
    catcher = CreateFrame("Button", nil, bagButton)
    catcher:SetAllPoints(bagButton)
    catcher:SetFrameLevel(bagButton:GetFrameLevel() + 5)
    catcher:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    catcher:RegisterForDrag("LeftButton")
    catcher:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")

    catcher:SetScript("OnClick", CatcherClicked)

    -- Dragging out of a bag is a pickup too, and so is dropping the cursor onto one.
    catcher:SetScript("OnDragStart", function(self) PickupContainerItem(BagSlotOf(self)) end)
    catcher:SetScript("OnReceiveDrag", function(self) PickupContainerItem(BagSlotOf(self)) end)

    -- The tooltip the bag button would have shown. Standing on it means standing in front
    -- of its OnEnter as well.
    catcher:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self:GetParent(), "ANCHOR_RIGHT")
        GameTooltip:SetBagItem(BagSlotOf(self))
        GameTooltip:Show()
    end)
    catcher:SetScript("OnLeave", function() GameTooltip:Hide() end)

    catcher:Hide()
    catchers[bagButton] = catcher
    return catcher
end

--[[
    Puts a catcher on every visible bag slot while a box is open, and takes them all off
    otherwise. Polled rather than hooked, because bags open and close on their own schedule
    and a modifier key can go down at any moment - and a tenth of a second is well inside
    the time it takes to press shift and then click.
]]
local function RefreshCatchers()
    if not (frame and frame:IsShown()) or IsModifierKeyDown() then
        for _, catcher in pairs(catchers) do
            catcher:Hide()
        end

        return
    end

    for i = 1, (NUM_CONTAINER_FRAMES or 13) do
        local bagFrame = _G["ContainerFrame" .. i]

        if bagFrame and bagFrame:IsShown() and bagFrame.size then
            for j = 1, bagFrame.size do
                local bagButton = _G["ContainerFrame" .. i .. "Item" .. j]

                if bagButton then
                    CatcherFor(bagButton):Show()
                end
            end
        end
    end
end

-- Named so the harness can drive it: what it decides is entirely about timing.
local bagWatcher = CreateFrame("Frame", "SanctuaryStashBagWatcher")
local sinceBagScan = 0

bagWatcher:SetScript("OnUpdate", function(self, elapsed)
    sinceBagScan = sinceBagScan + (elapsed or 0)

    if sinceBagScan < 0.1 then
        return
    end

    sinceBagScan = 0
    RefreshCatchers()
end)
