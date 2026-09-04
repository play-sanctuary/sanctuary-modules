--[[
    Sanctuary Notice Board - the window

    Gossip could show a notice board's index but never a notice: a gossip line is one line,
    and a bill is up to 255 characters. Reading anything meant clicking into it, and writing
    anything meant a single-line prompt. This is the same board with room to read it.

    The gossip menu is still there and still works. It is what the board offers anybody who
    has not installed this, which was the whole reason the module was built on gossip - so
    this window is an alternative rather than a replacement, and the server is told at login
    which one to draw.

    The window decides nothing. Every notice it shows and every refusal it reports came from
    the server, which re-checks the length limit, the per-character cap and who may take a
    notice down on every request.
]]

local ADDON_PREFIX = "SBOARD"

local CATEGORIES = {
    [0] = { name = "Goods & Services", colour = "|cff7fd06b" },
    [1] = { name = "Notice",           colour = "|cffe6d6a8" },
    [2] = { name = "Wanted",           colour = "|cffff7d6b" },
    [3] = { name = "Event",            colour = "|cff8fbce6" },
}

local ROWS = 7
local ROW_HEIGHT = 46

local playerName
local frame
local rows = {}
local posts = {}          -- id -> { id, category, mine, postedAt, body }
local order = {}          -- ids, newest first, as the server sent them
local offset = 0
local filter = nil        -- nil is everything
local myPosts, myLimit = 0, 3
local filterButtons = {}

--------------------------------------------------------------------------

local function Send(message)
    SendAddonMessage(ADDON_PREFIX, message, "WHISPER", playerName or UnitName("player"))
end

local function Ago(postedAt)
    local seconds = time() - postedAt

    if seconds < 3600 then
        return math.max(1, math.floor(seconds / 60)) .. "m ago"
    elseif seconds < 86400 then
        return math.floor(seconds / 3600) .. "h ago"
    end

    return math.floor(seconds / 86400) .. "d ago"
end

local function Visible()
    local list = {}

    for _, id in ipairs(order) do
        local post = posts[id]
        if post and (not filter or post.category == filter) then
            table.insert(list, post)
        end
    end

    return list
end

--------------------------------------------------------------------------
-- Writing one
--------------------------------------------------------------------------

--[[
    Writing a notice.

    This was a StaticPopup with hasEditBox, which is a one-line text bar: 255 characters
    scrolling sideways through a slot about forty wide, so you could not see the notice you
    were writing. A bill is a paragraph and wants a paragraph-shaped box.

    Enter pins it up. That costs nothing here because a notice is one block of text either
    way - the server replaces newlines with spaces before it stores anything, so the newline
    Enter would otherwise insert was never going to survive to the board.
]]

local MAX_LENGTH = 255

local compose
local composeCategory = 1

local function ComposeSubmit()
    if not compose then return end

    -- Trimmed here as well as on the server, only so that a box holding nothing but spaces
    -- disables the button rather than being sent and refused.
    local text = string.gsub(compose.box:GetText() or "", "%s+", " ")
    text = string.match(text, "^%s*(.-)%s*$") or ""

    if text == "" then return end

    Send("POST " .. composeCategory .. " " .. text)
    compose:Hide()
end

local function BuildCompose()
    if compose then return end

    compose = CreateFrame("Frame", "SanctuaryBoardComposeFrame", UIParent)
    compose:SetWidth(420)
    compose:SetHeight(224)
    compose:SetPoint("CENTER", UIParent, "CENTER", 0, 40)
    compose:SetFrameStrata("DIALOG")           -- above the board window, which is HIGH
    compose:SetMovable(true)
    compose:EnableMouse(true)
    compose:RegisterForDrag("LeftButton")
    compose:SetScript("OnDragStart", compose.StartMoving)
    compose:SetScript("OnDragStop", compose.StopMovingOrSizing)
    compose:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    compose:Hide()

    compose.title = compose:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    compose.title:SetPoint("TOP", compose, "TOP", 0, -16)

    local close = CreateFrame("Button", nil, compose, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", compose, "TOPRIGHT", -8, -8)
    close:SetScript("OnClick", function() compose:Hide() end)

    -- The sunken area the text sits in, so it reads as a box rather than as loose text.
    local well = CreateFrame("Frame", nil, compose)
    well:SetWidth(376)
    well:SetHeight(116)
    well:SetPoint("TOPLEFT", compose, "TOPLEFT", 22, -42)
    well:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 14,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    well:SetBackdropColor(0, 0, 0, 0.75)
    well:EnableMouse(true)

    local box = CreateFrame("EditBox", "SanctuaryBoardComposeBox", well)
    box:SetPoint("TOPLEFT", well, "TOPLEFT", 8, -8)
    box:SetPoint("BOTTOMRIGHT", well, "BOTTOMRIGHT", -8, 8)
    box:SetMultiLine(true)
    box:SetMaxLetters(MAX_LENGTH)
    box:SetAutoFocus(false)
    box:SetFontObject(ChatFontNormal)
    box:SetJustifyH("LEFT")
    box:SetJustifyV("TOP")
    box:SetTextInsets(0, 0, 0, 0)
    box:SetScript("OnEnterPressed", ComposeSubmit)
    box:SetScript("OnEscapePressed", function() compose:Hide() end)
    compose.box = box

    -- Clicking anywhere in the box puts the cursor in it, not just on the line of text.
    well:SetScript("OnMouseDown", function() box:SetFocus() end)

    compose.left = compose:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    compose.left:SetPoint("BOTTOMLEFT", compose, "BOTTOMLEFT", 22, 20)

    local pin = CreateFrame("Button", nil, compose, "UIPanelButtonTemplate")
    pin:SetWidth(90)
    pin:SetHeight(22)
    pin:SetPoint("BOTTOMRIGHT", compose, "BOTTOMRIGHT", -22, 16)
    pin:SetText("Pin it up")
    pin:SetScript("OnClick", ComposeSubmit)
    pin:Disable()
    compose.pin = pin

    local cancel = CreateFrame("Button", nil, compose, "UIPanelButtonTemplate")
    cancel:SetWidth(80)
    cancel:SetHeight(22)
    cancel:SetPoint("RIGHT", pin, "LEFT", -6, 0)
    cancel:SetText(CANCEL)
    cancel:SetScript("OnClick", function() compose:Hide() end)

    local function Typed()
        local text = box:GetText() or ""
        local trimmed = string.match(text, "^%s*(.-)%s*$") or ""

        compose.left:SetText(string.format("%d / %d   -   Enter pins it up",
            string.len(text), MAX_LENGTH))

        if trimmed == "" then compose.pin:Disable() else compose.pin:Enable() end
    end

    box:SetScript("OnTextChanged", Typed)
    compose.Typed = Typed

    tinsert(UISpecialFrames, "SanctuaryBoardComposeFrame")   -- Escape closes it
end

local function OpenCompose(category)
    BuildCompose()

    composeCategory = category
    compose.title:SetText(string.format("Pin up a %s notice",
        (CATEGORIES[category] or CATEGORIES[1]).name))
    compose.box:SetText("")
    compose.Typed()
    compose:Show()
    compose.box:SetFocus()
end

StaticPopupDialogs["SANCTUARY_BOARD_TAKE_DOWN"] = {
    text = "Take your notice down?",
    button1 = YES,
    button2 = NO,
    timeout = 0,
    hideOnEscape = 1,
    OnAccept = function(self)
        SendAddonMessage(ADDON_PREFIX, "REMOVE " .. (self.postId or 0),
            "WHISPER", UnitName("player"))
    end,
}

--------------------------------------------------------------------------
-- The window
--------------------------------------------------------------------------

local Refresh

local function BuildRow(index)
    local row = CreateFrame("Frame", nil, frame)
    row:SetWidth(392)
    row:SetHeight(ROW_HEIGHT)
    row:SetPoint("TOPLEFT", frame, "TOPLEFT", 18, -70 - (index - 1) * ROW_HEIGHT)

    row.head = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    row.head:SetPoint("TOPLEFT", row, "TOPLEFT", 0, 0)

    -- The whole notice, wrapped. This is the thing gossip could not do.
    row.body = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    row.body:SetPoint("TOPLEFT", row, "TOPLEFT", 0, -14)
    row.body:SetWidth(346)
    row.body:SetJustifyH("LEFT")
    row.body:SetJustifyV("TOP")

    -- Moderators are told who wrote each notice. The head line carries the name and the
    -- tooltip the rest, because guid and account are what a ban needs and neither is worth
    -- the width on every row.
    row:EnableMouse(true)
    row:SetScript("OnEnter", function(self)
        if not self.author then return end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText("Written by")
        GameTooltip:AddLine(self.author, 1, 1, 1, true)
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function() GameTooltip:Hide() end)

    row.take = CreateFrame("Button", nil, row, "UIPanelCloseButton")
    row.take:SetWidth(22)
    row.take:SetHeight(22)
    row.take:SetPoint("TOPRIGHT", row, "TOPRIGHT", 4, 4)
    row.take:Hide()

    return row
end

local function BuildFrame()
    if frame then return end

    frame = CreateFrame("Frame", "SanctuaryBoardFrame", UIParent)
    frame:SetWidth(430)
    frame:SetHeight(70 + ROWS * ROW_HEIGHT + 48)
    frame:SetPoint("CENTER")
    frame:SetFrameStrata("HIGH")
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    frame:Hide()

    frame.title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    frame.title:SetPoint("TOP", frame, "TOP", 0, -16)
    frame.title:SetText("Notice Board")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -8, -8)
    close:SetScript("OnClick", function() frame:Hide() end)

    -- Category filters. "All" first, then one per category.
    --
    -- The chosen one stays lit. A filter you cannot see the state of is worse than no
    -- filter: an empty board and a board filtered down to nothing look identical.
    local function Choose(value)
        filter = value
        offset = 0

        for key, button in pairs(filterButtons) do
            if key == (value == nil and "all" or value) then
                button:LockHighlight()
            else
                button:UnlockHighlight()
            end
        end

        Refresh()
    end

    local all = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    all:SetWidth(44)
    all:SetHeight(18)
    all:SetPoint("TOPLEFT", frame, "TOPLEFT", 18, -44)
    all:SetText("All")
    all:SetScript("OnClick", function() Choose(nil) end)
    filterButtons.all = all

    local previous = all

    for index = 0, 3 do
        local button = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
        button:SetWidth(84)
        button:SetHeight(18)
        button:SetPoint("LEFT", previous, "RIGHT", 3, 0)
        button:SetText(CATEGORIES[index].name:gsub(" & Services", ""))
        button:SetScript("OnClick", function() Choose(index) end)
        filterButtons[index] = button
        previous = button
    end

    all:LockHighlight()

    for index = 1, ROWS do
        rows[index] = BuildRow(index)
    end

    local scrollUp = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    scrollUp:SetWidth(24)
    scrollUp:SetHeight(20)
    scrollUp:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 18, 14)
    scrollUp:SetText("^")
    scrollUp:SetScript("OnClick", function()
        offset = math.max(0, offset - ROWS)
        Refresh()
    end)

    local scrollDown = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    scrollDown:SetWidth(24)
    scrollDown:SetHeight(20)
    scrollDown:SetPoint("LEFT", scrollUp, "RIGHT", 4, 0)
    scrollDown:SetText("v")
    scrollDown:SetScript("OnClick", function()
        if offset + ROWS < #Visible() then
            offset = offset + ROWS
            Refresh()
        end
    end)

    -- Anchored between the scroll buttons and the writing button rather than centred on the
    -- frame, where "yours: n of n" ran underneath "Pin up a notice".
    frame.count = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    frame.count:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 78, 20)
    frame.count:SetWidth(206)
    frame.count:SetJustifyH("LEFT")

    local write = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    write:SetWidth(120)
    write:SetHeight(20)
    write:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -18, 14)
    write:SetText("Pin up a notice")
    -- Whatever category is being looked at, or Notice when looking at everything.
    write:SetScript("OnClick", function() OpenCompose(filter or 1) end)

    --[[
        Standing at the board is a condition, not a moment.

        The server binds the window to the board it was opened on and refuses everything
        once you are out of its reach - so a window left open while its owner wanders off
        would sit there looking usable and answering nothing. This asks once a second
        whether we are still there; the server replies only when the answer is no.

        OnUpdate does not run on a hidden frame, so the asking stops on its own.
    ]]
    frame:SetScript("OnUpdate", function(self, elapsed)
        self.since = (self.since or 0) + elapsed
        if self.since < 1 then return end
        self.since = 0
        Send("HERE")
    end)
end

Refresh = function()
    if not frame then return end

    local list = Visible()

    for index = 1, ROWS do
        local row = rows[index]
        local post = list[offset + index]

        if not post then
            row:Hide()
        else
            local category = CATEGORIES[post.category] or CATEGORIES[1]

            -- "by Name" only; the guid and account go in the tooltip.
            local by = ""

            if post.author then
                by = "  |cffff8080by " ..
                    (string.match(post.author, "^(.-) %(guid") or post.author) .. "|r"
            end

            row.author = post.author

            row.head:SetText(string.format("%s[%s]|r  |cff808080%s|r%s",
                category.colour, category.name, Ago(post.postedAt), by))
            row.body:SetText(post.body or "")

            if post.mine then
                row.take:Show()
                row.take:SetScript("OnClick", function()
                    local dialog = StaticPopup_Show("SANCTUARY_BOARD_TAKE_DOWN")
                    if dialog then dialog.postId = post.id end
                end)
            else
                row.take:Hide()
            end

            row:Show()
        end
    end

    frame.count:SetText(string.format("%d notice(s)   -   yours: %d of %d",
        #list, myPosts, myLimit))
end

--------------------------------------------------------------------------
-- What the server says
--------------------------------------------------------------------------

local function Handle(message)
    local verb, rest = string.match(message, "^(%S+)%s*(.*)$")

    if verb == "OPEN" then
        local _, limit, mine = string.match(rest, "^(%d+)%s+(%d+)%s+(%d+)$")
        myLimit = tonumber(limit) or 3
        myPosts = tonumber(mine) or 0

        posts, order, offset = {}, {}, 0
        BuildFrame()

    elseif verb == "POST" then
        local id, category, mine, at = string.match(rest, "^(%d+)%s+(%d+)%s+(%d)%s+(%d+)$")

        if id then
            id = tonumber(id)
            posts[id] = { id = id, category = tonumber(category),
                          mine = mine == "1", postedAt = tonumber(at), body = "" }
            table.insert(order, id)
        end

    elseif verb == "BODY" then
        -- Bodies arrive in pieces because one addon message cannot hold 255 characters plus
        -- everything else on the line. Appended in the order they came.
        local id, chunk = string.match(rest, "^(%d+)%s(.*)$")
        local post = id and posts[tonumber(id)]

        if post then
            post.body = post.body .. chunk
        end

    elseif verb == "WHO" then
        -- Sent only to moderators, and only ever after the POST line it belongs to.
        local id, author = string.match(rest, "^(%d+)%s(.*)$")
        local post = id and posts[tonumber(id)]

        if post then
            post.author = author
        end

    elseif verb == "SHUT" then
        -- We have walked away from the board. The server has already stopped answering;
        -- this is the window agreeing to stop looking like it works.
        if frame then frame:Hide() end
        if compose then compose:Hide() end

    elseif verb == "DONE" then
        BuildFrame()
        Refresh()
        frame:Show()

    elseif verb == "HI" then
        -- The server knows we are here; it will send the window instead of a gossip menu.
    end
end

--------------------------------------------------------------------------

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")

listener:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "PLAYER_ENTERING_WORLD" then
        playerName = UnitName("player")

        -- Announced at login rather than when a board is first clicked: the server has to
        -- know which interface to draw BEFORE it draws one, or the first board of a session
        -- would always be gossip.
        Send("HELLO")
        return
    end

    if event == "CHAT_MSG_ADDON" and arg1 == ADDON_PREFIX then
        Handle(arg2)
    end
end)
