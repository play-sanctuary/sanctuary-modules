--[[
    SanctuaryFaction - the faction you belong to, in a window.

    Three things, because they are the three the client cannot work out on its own: which
    faction you are in and at what rank, who else is in it, and what your rank has taught
    you. All three arrive from the server over the SFACTION prefix.

    WHAT THIS ADDON DOES NOT DO IS DECIDE ANYTHING. Every button below sends a `.faction`
    command over AzerothCore's addon command channel and waits to be told what happened.
    That is not laziness - an addon is a program the player owns and can edit, so a button
    that acted directly would be a button with no rules on it. The rank checks live on the
    server and this file is a way of typing.

    The same goes for the buttons being greyed out: that is politeness, not security. A
    recruit who edits this file to un-grey the promote button gets the same refusal from
    the server that they would have got from typing the command.
]]

local ADDON_PREFIX = "SFACTION"

local playerName

--------------------------------------------------------------------------
-- Talking to the server
--------------------------------------------------------------------------

local function Send(message)
    SendAddonMessage(ADDON_PREFIX, message, "WHISPER", playerName or UnitName("player"))
end

-- Commands go out over AzerothCore's addon command channel rather than as a chat line
-- starting with a dot. An unrecognised dot command falls through and is published to /say,
-- which would announce your faction's business to the room.
local commandCounter = 0

local function SendServerCommand(command)
    commandCounter = (commandCounter + 1) % 10000
    SendAddonMessage("AzerothCore",
        string.format("i%04d%s", commandCounter, command),
        "WHISPER",
        playerName or UnitName("player"))
end

--------------------------------------------------------------------------
-- What the server last said
--------------------------------------------------------------------------

-- Permission bits, matching SanctuaryFaction.h. Only the four that are enforced are named
-- here: EDIT and DISBAND exist in the mask but the ladder is a game master's to write, so
-- there is no button for them and naming them would suggest otherwise.
local PERM_INVITE  = 1
local PERM_KICK    = 2
local PERM_PROMOTE = 4
local PERM_DEMOTE  = 8
local PERM_EDIT    = 16   -- renaming rungs; what they GRANT stays a game master's

local state = {
    faction = 0,
    rank = 0,
    permissions = 0,
    mayCreate = false,   -- whether the SERVER says this account may found one
    factionName = "",
    ranks = {},       -- [rankId] = name
    spells = {},      -- array of spell ids
    roster = {},      -- array of { rank = n, label = "..." }
    absent = 0,
}

-- The 3.3.5 client is Lua 5.1 with no bitwise operators and no bit32. Integer division and
-- modulo test one bit, and are exact for masks this small.
local function HasPermission(bit)
    return (math.floor(state.permissions / bit) % 2) == 1
end

local function RankName(rankId)
    return state.ranks[rankId] or ("Rank " .. tostring(rankId))
end

--[[
    WHERE a rung sits, as opposed to what the faction has chosen to call it.

    The Ranks panel is where somebody is deciding on the names, so it is the one place the
    names cannot double as labels: a row reading "Kingpin" beside a box reading "Kingpin"
    says nothing about which rung is being renamed. Counted down from the top for the same
    reason the server counts down - the top is the rung that does not move.

    These four words are also what the server's confirmation says; changing them here means
    changing RungDescription in SanctuaryFactionCommands.cpp.
]]
local RUNG_PLACES = { "Leader", "Higher Officer", "Lower Officer", "Member" }

local function RungPlace(position, rankId)
    return RUNG_PLACES[position] or ("Rank " .. tostring(rankId))
end

--------------------------------------------------------------------------
-- The window
--------------------------------------------------------------------------

local frame
local rosterRows = {}

--[[
    The MEMBER ITSELF is what is selected, not an index.

    An index was right when the list could only scroll: it stopped being right the moment
    the list could also be sorted and filtered, because both of those move a person to a
    different row without anybody clicking anything. Holding the entry means the highlight
    follows the person you picked through a re-sort, and typing in the search box cannot
    quietly re-aim Kick at somebody else.
]]
local selectedMember

-- How far down the SORTED AND FILTERED list the visible window starts. 0 is the top.
local rosterOffset = 0

-- The list actually on screen: state.roster, sorted and filtered. Rebuilt on every draw,
-- because every input that changes it also asks for a draw.
local view = {}

local sortKey = "rank"       -- "rank" or "name"
local sortDescending = true  -- rank, highest first: a roster reads leader downwards
local searchText = ""

local ROW_HEIGHT = 16
-- Ten, not twelve. Twelve rows reached 298 pixels down a 330 pixel window and the button
-- row starts 40 up from the bottom, so the last two rows and the buttons were drawn on top
-- of each other. The window is taller now and the list is shorter than the space it has.
local MAX_ROWS = 10

local function UpdateButtons()
    if not frame then return end

    local chosen = selectedMember

    -- A row has to be picked before anything can be done to it, and the server will refuse
    -- anybody of equal or higher rank anyway - so the local test is the same one, said
    -- earlier, to save a round trip and a refusal in the chat frame.
    local actionable = chosen ~= nil and chosen.rank < state.rank

    frame.invite:Enable()
    if not HasPermission(PERM_INVITE) then frame.invite:Disable() end

    local function set(button, permission)
        if actionable and HasPermission(permission) then
            button:Enable()
        else
            button:Disable()
        end
    end

    set(frame.promote, PERM_PROMOTE)
    set(frame.demote, PERM_DEMOTE)
    set(frame.kick, PERM_KICK)

    local none = (state.faction == 0)

    -- Creating is offered only to somebody the server has said may do it, and only when
    -- they are not already in one. Hidden rather than greyed: an ordinary player has no
    -- business knowing the box exists.
    if none and state.mayCreate then
        frame.createBox:Show()
        frame.createGo:Show()
        if frame.createBox:GetText() == "" then frame.createHint:Show() end
    else
        frame.createBox:Hide()
        frame.createGo:Hide()
        frame.createHint:Hide()
    end

    if none then frame.leave:Hide() else frame.leave:Show() end

    if not none and HasPermission(PERM_EDIT) then
        frame.ranksButton:Show()
    else
        frame.ranksButton:Hide()
        if frame.ranksPanel then frame.ranksPanel:Hide() end
    end
end

--[[
    Renaming the rungs.

    A panel over the list rather than a window of its own, because it is the same subject
    seen a different way and a second floating frame is one more thing to move out of the
    way. Rows are built from state.ranks, which the server sends with every refresh, so the
    panel never has to ask for anything.

    Only the NAME is editable here. What a rank grants is a balance decision and stays with
    a game master's `.faction rank`; this window cannot add a rung, remove one, or move a
    single permission.
]]
local MAX_RANK_ROWS = 8
local rankRows = {}

local function RedrawRanks()
    if not frame or not frame.ranksPanel then return end

    -- state.ranks is keyed by rank id and may have holes, so it is walked into a list
    -- first. ipairs would stop at the first gap.
    local ladder = {}

    for id, name in pairs(state.ranks) do
        table.insert(ladder, { id = id, name = name })
    end

    table.sort(ladder, function(a, b) return a.id > b.id end)   -- senior first, as the roster

    for i = 1, MAX_RANK_ROWS do
        local row = rankRows[i]
        local rung = ladder[i]

        if rung then
            row.id = rung.id
            row.number:SetText(RungPlace(i, rung.id))

            --[[
                A box is only refilled while it still holds what WE last put in it.

                Focus was the first guess and it is not enough: renaming one rung makes the
                server push fresh state to everybody in the faction, which redraws every
                box - so typing new names into three rungs and pressing Set on the first
                wiped the other two, which were not focused and never had been.

                Comparing against what was last written distinguishes "unchanged" from
                "edited" without having to notice the editing, which OnTextChanged in this
                client cannot tell apart from a programmatic SetText anyway.
            ]]
            if row.box:GetText() == (row.shown or "") then
                row.box:SetText(rung.name)
                row.shown = rung.name
            end

            row:Show()
        else
            row.id = nil
            row:Hide()
        end
    end
end

--[[
    Sorted and filtered, into `view`.

    Rank is the default because a roster is a hierarchy before it is a list of names, and
    highest first because that is the order every guild frame in the game shows and the one
    people read without being told.

    Ties within a rank fall back to the name, so a faction of thirty Members is alphabetical
    rather than in whatever order the server happened to walk its map - which is neither
    stable nor meaningful, and looks like a bug the moment two draws disagree.
]]
local function RebuildView()
    view = {}

    local needle = string.lower(searchText)

    for _, member in ipairs(state.roster) do
        if needle == "" or string.find(string.lower(member.label), needle, 1, true) then
            table.insert(view, member)
        end
    end

    table.sort(view, function(a, b)
        if sortKey == "name" then
            if a.label ~= b.label then
                if sortDescending then return a.label > b.label end
                return a.label < b.label
            end

            return a.rank > b.rank
        end

        if a.rank ~= b.rank then
            if sortDescending then return a.rank > b.rank end
            return a.rank < b.rank
        end

        return a.label < b.label
    end)
end

local function RedrawRoster()
    if not frame then return end

    RebuildView()

    -- Clamped every draw rather than only when scrolling: the list shortens under it when
    -- somebody is kicked, logs out, or is typed out of the search, and an offset left past
    -- the end shows an empty window with a roster that is plainly not empty.
    local maximum = math.max(0, #view - MAX_ROWS)

    if rosterOffset > maximum then rosterOffset = maximum end
    if rosterOffset < 0 then rosterOffset = 0 end

    for i = 1, MAX_ROWS do
        local row = rosterRows[i]
        local member = view[i + rosterOffset]

        if member then
            row.label:SetText(member.label)
            row.rank:SetText(RankName(member.rank))
            row.member = member
            row:Show()

            if selectedMember == member then
                row.highlight:Show()
            else
                row.highlight:Hide()
            end
        else
            row.member = nil
            row:Hide()
        end
    end

    -- The bar, sized to what is left over rather than to the list: a scrollbar whose range
    -- is the row count would let you scroll ten rows past the end.
    if maximum > 0 then
        frame.scroll:SetMinMaxValues(0, maximum)
        frame.scroll:SetValue(rosterOffset)
        frame.scroll:Show()
    else
        frame.scroll:Hide()
    end

    -- Says how many there are and where in them you are looking. Without it a list that
    -- scrolls looks exactly like a list that is ten long. The unfiltered total is kept in
    -- view when a search is narrowing things, or the count looks like people have left.
    local total = #state.roster

    -- Labelled, because a bare number in the corner of a window is a number in the corner
    -- of a window: on a one-member faction it read "1", which could have been anything.
    if #view < total then
        frame.count:SetText(string.format("|cff808080Members: %d of %d|r", #view, total))
    elseif total > MAX_ROWS then
        frame.count:SetText(string.format("|cff808080Members: %d-%d of %d|r",
            rosterOffset + 1, math.min(rosterOffset + MAX_ROWS, total), total))
    elseif total > 0 then
        frame.count:SetText(string.format("|cff808080Members: %d|r", total))
    else
        frame.count:SetText("")
    end

    --[[
        Which one is in use, said in colour rather than with an arrow.

        These read as instructions now - "Sort by Name" - and an arrow hung off the end of
        an instruction is a puzzle: it looks like part of what the button will do rather
        than a report of what it has already done. Lighting the active one says the same
        thing without adding a symbol to decode.
    ]]
    frame.sortName.label:SetText(sortKey == "name"
        and "|cffffd100Sort by Name|r" or "Sort by Name")

    frame.sortRank.label:SetText(sortKey == "rank"
        and "|cffffd100Sort by Rank|r" or "Sort by Rank")

    RedrawRanks()

    if state.absent > 0 then
        -- Counted, not named. The server will not say who they are: LabelFor needs both
        -- people present to decide between a real name and an alias, so an absent member
        -- has no honest label yet.
        frame.absent:SetText(string.format("|cff808080and %d not logged in|r", state.absent))
        frame.absent:Show()
    else
        frame.absent:Hide()
    end

    UpdateButtons()
end

local function RedrawState()
    if not frame then return end

    if state.faction == 0 then
        frame.title:SetText("No faction")
        frame.rank:SetText("")
        frame.spells:SetText(state.mayCreate
            and "You belong to no faction. Name one below to create it."
            or "You belong to no faction. Somebody in one can invite you.")
        state.roster = {}
        state.absent = 0
        selectedMember = nil
        RedrawRoster()
        return
    end

    frame.title:SetText(state.factionName)
    frame.rank:SetText("|cffd8b46a" .. RankName(state.rank) .. "|r")

    if #state.spells == 0 then
        frame.spells:SetText("|cff808080Your rank teaches nothing yet.|r")
    else
        local names = {}

        for _, id in ipairs(state.spells) do
            -- The name comes out of the client's own Spell.dbc rather than off the wire,
            -- so the window cannot disagree with the spellbook about what a spell is
            -- called. An id the client does not know is shown as an id, which is a visible
            -- symptom rather than a blank line.
            local name = GetSpellInfo(id)
            table.insert(names, name or ("spell " .. tostring(id)))
        end

        frame.spells:SetText("Taught: " .. table.concat(names, ", "))
    end

    RedrawRoster()
end

local function CreateWindow()
    frame = CreateFrame("Frame", "SanctuaryFactionFrame", UIParent)
    frame:SetWidth(340)
    frame:SetHeight(400)
    frame:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 }
    })
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:Hide()

    -- Escape closes it, the way every other window in the game does.
    table.insert(UISpecialFrames, "SanctuaryFactionFrame")

    frame.title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    frame.title:SetPoint("TOP", frame, "TOP", 0, -18)

    frame.rank = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    frame.rank:SetPoint("TOP", frame.title, "BOTTOM", 0, -4)

    frame.spells = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    frame.spells:SetPoint("TOPLEFT", frame, "TOPLEFT", 20, -66)
    frame.spells:SetWidth(300)
    frame.spells:SetJustifyH("LEFT")

    -- Searching. Filters what is already here rather than asking the server again: the
    -- whole roster is in hand, and a round trip per keystroke would be both slower and a
    -- way to make the window flicker.
    --[[
        NAMED, and it has to be.

        InputBoxTemplate draws itself from three textures - two end caps and a middle - and
        the middle anchors itself with relativeTo="$parentLeft" and "$parentRight". There is
        nothing to substitute for $parent on an anonymous frame, so those anchors resolve to
        nothing, the middle collapses to no width, and what is left on screen is two small
        caps with a gap between them. Which is exactly what it looked like.
    ]]
    frame.search = CreateFrame("EditBox", "SanctuaryFactionSearchBox", frame, "InputBoxTemplate")
    frame.search:SetWidth(150)
    frame.search:SetHeight(20)
    frame.search:SetPoint("TOPLEFT", frame, "TOPLEFT", 26, -96)
    frame.search:SetAutoFocus(false)
    frame.search:SetMaxLetters(48)

    local searchHint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    searchHint:SetPoint("LEFT", frame.search, "LEFT", 4, 0)
    searchHint:SetText("Search")

    frame.search:SetScript("OnTextChanged", function(self)
        searchText = self:GetText() or ""

        if searchText == "" then searchHint:Show() else searchHint:Hide() end

        -- Back to the top: the first match of a new search should be on screen, not
        -- wherever the last one happened to leave the offset.
        rosterOffset = 0
        RedrawRoster()
    end)

    frame.search:SetScript("OnEscapePressed", function(self)
        self:SetText("")
        self:ClearFocus()
    end)

    frame.search:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)

    -- Column headers that sort. Clicking the one already in use turns it around, which is
    -- the behaviour of every sortable list in the game and needs no explaining.
    local function Header(text, point, x, key)
        local button = CreateFrame("Button", nil, frame)
        button:SetHeight(14)
        button:SetWidth(80)          -- "Sort by Name" needs more room than "Name" did
        button:SetPoint(point, frame, point, x, -120)

        local label = button:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        label:SetAllPoints()
        label:SetJustifyH(point == "TOPLEFT" and "LEFT" or "RIGHT")
        label:SetText(text)

        button:SetScript("OnClick", function()
            if sortKey == key then
                sortDescending = not sortDescending
            else
                sortKey = key
                -- Rank opens highest first, name opens A to Z. Both are what somebody
                -- means when they ask for that column without saying which way.
                sortDescending = (key == "rank")
            end

            rosterOffset = 0
            RedrawRoster()
        end)

        button.label = label
        return button
    end

    frame.sortName = Header("Sort by Name", "TOPLEFT", 22, "name")
    frame.sortRank = Header("Sort by Rank", "TOPRIGHT", -42, "rank")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -6, -6)

    for i = 1, MAX_ROWS do
        local row = CreateFrame("Button", "SanctuaryFactionRow" .. i, frame)
        row:SetWidth(276)                      -- narrower: the scrollbar has the right edge
        row:SetHeight(ROW_HEIGHT)
        row:SetPoint("TOPLEFT", frame, "TOPLEFT", 20, -138 - (i - 1) * ROW_HEIGHT)

        row.highlight = row:CreateTexture(nil, "BACKGROUND")
        row.highlight:SetAllPoints()
        row.highlight:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
        row.highlight:Hide()

        row.label = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.label:SetPoint("LEFT", row, "LEFT", 2, 0)

        row.rank = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        row.rank:SetPoint("RIGHT", row, "RIGHT", -2, 0)

        row:SetScript("OnClick", function(self)
            -- The member the row is currently showing, set when it was drawn.
            selectedMember = self.member
            RedrawRoster()
        end)

        row:Hide()
        rosterRows[i] = row
    end

    --[[
        The wheel scrolls the list.

        A FauxScrollFrame would give a draggable bar as well, and is what the game's own
        lists use, but it wants its rows parented into a scroll child and its OnVerticalScroll
        wired to an update function - a lot of machinery for ten rows that are redrawn from a
        table anyway. The wheel over a plain offset does the same job here.

        EnableMouseWheel on the window rather than on each row, so the wheel works over the
        whole panel including the gaps between rows.
    ]]
    frame:EnableMouseWheel(true)
    frame:SetScript("OnMouseWheel", function(self, delta)
        if #view <= MAX_ROWS then return end

        rosterOffset = rosterOffset - delta      -- wheel up is +1, and up means earlier
        RedrawRoster()
    end)

    frame.count = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    frame.count:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -34, -100)

    --[[
        The bar.

        A plain Slider on UIPanelScrollBarTemplate rather than a FauxScrollFrame. The faux
        frame is what the game's own lists use, but it wants the rows parented into a scroll
        child and its OnVerticalScroll wired back to an update function - machinery for rows
        that are already redrawn from a table on every change. A slider carrying the offset
        does the same job and the wheel and the bar stay one number.
    ]]
    frame.scroll = CreateFrame("Slider", "SanctuaryFactionScroll", frame, "UIPanelScrollBarTemplate")
    frame.scroll:SetWidth(16)
    frame.scroll:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -22, -156)
    frame.scroll:SetPoint("BOTTOMRIGHT", frame, "TOPRIGHT", -22, -292)
    frame.scroll:SetMinMaxValues(0, 0)
    frame.scroll:SetValueStep(1)
    frame.scroll:SetValue(0)

    frame.scroll:SetScript("OnValueChanged", function(self, value)
        value = math.floor(value + 0.5)

        -- Guarded, or SetValue inside RedrawRoster re-enters this handler and the two call
        -- each other until the client gives up.
        if value == rosterOffset then return end

        rosterOffset = value
        RedrawRoster()
    end)

    frame.absent = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    frame.absent:SetPoint("TOPLEFT", frame, "TOPLEFT", 20, -138 - MAX_ROWS * ROW_HEIGHT - 4)
    frame.absent:Hide()

    local function Button(label, x, handler)
        local button = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
        button:SetWidth(72)
        button:SetHeight(22)
        button:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", x, 18)
        button:SetText(label)
        button:SetScript("OnClick", handler)
        return button
    end

    -- Invite acts on the game target, not on a roster row: the person being asked is by
    -- definition not in the faction yet, so they cannot be in the list.
    frame.invite = Button("Invite", 18, function()
        SendServerCommand("faction invite")
        Send("ROSTER")
    end)

    local function Act(command)
        local chosen = selectedMember

        if not chosen then return end

        -- By guid, not by label: a label is an alias for anybody this character has not
        -- been introduced to, and an alias resolves to no one. The server rank-checks it
        -- exactly as it would a typed command.
        SendServerCommand(command .. " " .. chosen.guid)
        Send("ROSTER")
    end

    frame.promote = Button("Promote", 96, function() Act("faction promote") end)
    frame.demote = Button("Demote", 174, function() Act("faction demote") end)
    frame.kick = Button("Kick", 252, function() Act("faction kick") end)

    --[[
        Creating one: a box and a button, in the window.

        This was a StaticPopup with hasEditBox and it did nothing when clicked. The reason
        is a signature: 3.3.5 does not hand OnAccept the dialog frame the way later clients
        do, and it has no `dialog.editBox` member either, so the handler read the name out
        of a nil and gave up before it sent anything - silently, because a Lua error inside
        a StaticPopup handler goes nowhere the player can see.

        SanctuaryBoard abandoned hasEditBox for a box of its own for a different reason and
        the same conclusion: a text field we build is a text field we can rely on. Nothing
        here depends on the client's popup conventions.
    ]]
    -- Named, for the reason the search box is. See there.
    frame.createBox = CreateFrame("EditBox", "SanctuaryFactionCreateBox", frame, "InputBoxTemplate")
    frame.createBox:SetWidth(186)
    frame.createBox:SetHeight(22)
    frame.createBox:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 24, 46)
    frame.createBox:SetAutoFocus(false)      -- or opening the window swallows every keypress
    frame.createBox:SetMaxLetters(48)        -- `name` is VARCHAR(48)

    local hint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", frame.createBox, "LEFT", 4, 0)
    hint:SetText("Name a faction to create")

    frame.createBox:SetScript("OnTextChanged", function(self)
        if self:GetText() == "" then hint:Show() else hint:Hide() end
    end)

    local function Create()
        local name = frame.createBox:GetText()

        -- Trimmed here only so a box holding spaces is refused before it is sent; the
        -- server checks the name properly and owns the answer.
        name = string.gsub(name, "^%s*(.-)%s*$", "%1")

        if name == "" then return end

        SendServerCommand("faction create " .. name)
        frame.createBox:SetText("")
        frame.createBox:ClearFocus()

        -- Asked again rather than assumed: creating puts the creator in at the top rank,
        -- and the window has no way of learning the new id otherwise.
        Send("HELLO")
        Send("ROSTER")
    end

    frame.createBox:SetScript("OnEnterPressed", Create)
    frame.createBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    frame.createGo = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    frame.createGo:SetWidth(80)
    frame.createGo:SetHeight(22)
    frame.createGo:SetPoint("BOTTOMLEFT", frame.createBox, "BOTTOMRIGHT", 8, 0)
    frame.createGo:SetText("Create")
    frame.createGo:SetScript("OnClick", Create)

    frame.createHint = hint

    -- Shares the row with the creation box, because the two are never both useful: you are
    -- either in a faction or you are not.
    frame.leave = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    frame.leave:SetWidth(80)
    frame.leave:SetHeight(22)
    frame.leave:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -18, 46)
    frame.leave:SetText("Leave")
    frame.leave:SetScript("OnClick", function()
        SendServerCommand("faction leave")
        Send("HELLO")
        Send("ROSTER")
    end)

    frame.ranksButton = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    frame.ranksButton:SetWidth(80)
    frame.ranksButton:SetHeight(22)
    frame.ranksButton:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 18, 46)
    frame.ranksButton:SetText("Ranks")
    frame.ranksButton:Hide()

    -- Sits over the roster, so the two never fight for the same pixels.
    local panel = CreateFrame("Frame", nil, frame)
    panel:SetPoint("TOPLEFT", frame, "TOPLEFT", 16, -114)
    panel:SetPoint("BOTTOMRIGHT", frame, "TOPRIGHT", -16, -302)
    --[[
        A SOLID background, not the dialog one.

        UI-DialogBox-Background carries alpha of its own, so tinting it with
        SetBackdropColor darkens it without making it opaque - the roster showed through and
        the rank names were being read against a column of member names. A flat white
        texture tinted almost black is the only way to get a genuinely opaque panel out of a
        backdrop, because the opacity has to come from the texture rather than from the
        colour applied over it.
    ]]
    panel:SetBackdrop({
        bgFile = "Interface\\Buttons\\WHITE8X8",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = false, edgeSize = 12,
        insets = { left = 3, right = 3, top = 3, bottom = 3 }
    })

    panel:SetBackdropColor(0.04, 0.04, 0.05, 1)
    panel:SetBackdropBorderColor(0.5, 0.45, 0.35, 1)

    -- Above the list, not merely in front of it: a frame drawn at the same level can still
    -- have the roster's own text render over it.
    panel:SetFrameLevel(frame:GetFrameLevel() + 10)
    panel:EnableMouse(true)          -- or clicks fall through to the roster underneath
    panel:Hide()

    local heading = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    heading:SetPoint("TOPLEFT", panel, "TOPLEFT", 10, -8)
    heading:SetText("Rename ranks")

    -- The Ranks button toggles this, but a panel that covers the roster needs the way out
    -- to be visible ON it: reaching past it to the button that opened it is not obvious
    -- when what you can see is a panel with no corner.
    local close = CreateFrame("Button", nil, panel, "UIPanelCloseButton")
    close:SetWidth(24)
    close:SetHeight(24)
    close:SetPoint("TOPRIGHT", panel, "TOPRIGHT", -2, -2)
    close:SetScript("OnClick", function() panel:Hide() end)

    for i = 1, MAX_RANK_ROWS do
        local row = CreateFrame("Frame", nil, panel)
        row:SetWidth(276)
        row:SetHeight(22)
        row:SetPoint("TOPLEFT", panel, "TOPLEFT", 10, -24 - (i - 1) * 22)

        row.number = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        row.number:SetPoint("LEFT", row, "LEFT", 2, 0)
        row.number:SetWidth(84)          -- "Higher Officer" is the longest of them
        row.number:SetJustifyH("LEFT")

        local box = CreateFrame("EditBox", "SanctuaryFactionRankBox" .. i, row, "InputBoxTemplate")
        box:SetWidth(104)
        box:SetHeight(20)
        box:SetPoint("LEFT", row, "LEFT", 92, 0)
        box:SetAutoFocus(false)
        box:SetMaxLetters(32)        -- `name` is VARCHAR(32)

        local function Apply()
            local text = box:GetText()
            text = string.gsub(text, "^%s*(.-)%s*$", "%1")

            if not row.id or text == "" then return end

            SendServerCommand("faction rename " .. row.id .. " " .. text)
            box:ClearFocus()

            -- What was just asked for becomes the value a redraw may overwrite, so the
            -- server's confirmation lands cleanly instead of being treated as an edit.
            row.shown = text

            -- The server pushes a fresh STATE to everybody in the faction after a rename,
            -- so there is nothing to ask for here; the panel redraws when it arrives.
        end

        box:SetScript("OnEnterPressed", Apply)
        box:SetScript("OnEscapePressed", function(self)
            self:ClearFocus()
            RedrawRanks()            -- put the real name back
        end)

        local set = CreateFrame("Button", nil, row, "UIPanelButtonTemplate")
        set:SetWidth(56)
        set:SetHeight(20)
        set:SetPoint("LEFT", box, "RIGHT", 8, 0)
        set:SetText("Set")
        set:SetScript("OnClick", Apply)

        row.box = box
        row:Hide()
        rankRows[i] = row
    end

    frame.ranksPanel = panel

    frame.ranksButton:SetScript("OnClick", function()
        if panel:IsShown() then
            panel:Hide()
        else
            RedrawRanks()
            panel:Show()
        end
    end)
end

local function Toggle()
    if not frame then CreateWindow() end

    if frame:IsShown() then
        frame:Hide()
        return
    end

    -- Asked for fresh every time it opens rather than trusted from last time: ranks and
    -- rosters change while the window is shut.
    Send("HELLO")
    Send("ROSTER")
    frame:Show()
end

--------------------------------------------------------------------------
-- The minimap button
--------------------------------------------------------------------------

local MINIMAP_RADIUS = 80
-- Measured off a live minimap, not guessed. The other Sanctuary buttons share one arc,
-- about 19 degrees apart: outlaw -113.04, lawman -132.01, disguise -151.64, profile -171.25. This one is
-- deliberately not on it - it sits clear of the rest, above the minimap.
local DEFAULT_ANGLE = -17.59

local minimapButton = CreateFrame("Button", "SanctuaryFactionMinimapButton", Minimap)
minimapButton:SetWidth(31)
minimapButton:SetHeight(31)
minimapButton:SetFrameStrata("MEDIUM")
minimapButton:SetFrameLevel(8)
minimapButton:RegisterForClicks("LeftButtonUp")
minimapButton:RegisterForDrag("LeftButton")

local minimapIcon = minimapButton:CreateTexture(nil, "BACKGROUND")
minimapIcon:SetWidth(20)
minimapIcon:SetHeight(20)
minimapIcon:SetTexture("Interface\\Icons\\INV_Banner_02")
minimapIcon:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 6, -6)

-- The ring that makes it read as a minimap button rather than a loose icon. Drawn over the
-- icon, which is why the icon sits in BACKGROUND.
local minimapBorder = minimapButton:CreateTexture(nil, "OVERLAY")
minimapBorder:SetWidth(53)
minimapBorder:SetHeight(53)
minimapBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
minimapBorder:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 0, 0)

minimapButton:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")

local function PlaceMinimapButton(angle)
    minimapButton:SetPoint("CENTER", Minimap, "CENTER",
        cos(angle) * MINIMAP_RADIUS, sin(angle) * MINIMAP_RADIUS)
end

minimapButton:SetScript("OnDragStart", function(self) self.dragging = true end)

minimapButton:SetScript("OnDragStop", function(self)
    self.dragging = false

    if SanctuaryFactionDB then
        SanctuaryFactionDB.minimapAngle = self.angle or DEFAULT_ANGLE
    end
end)

minimapButton:SetScript("OnUpdate", function(self)
    if not self.dragging then return end

    local mx, my = Minimap:GetCenter()
    local cx, cy = GetCursorPosition()
    local scale = Minimap:GetEffectiveScale()

    -- Converted to degrees, because PlaceMinimapButton works in them.
    self.angle = math.deg(math.atan2((cy / scale) - my, (cx / scale) - mx))
    PlaceMinimapButton(self.angle)
end)

minimapButton:SetScript("OnClick", function() Toggle() end)

minimapButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:SetText("Sanctuary Faction")

    if state.faction == 0 then
        GameTooltip:AddLine("You belong to no faction.", 1, 1, 1, true)
    else
        GameTooltip:AddLine(state.factionName .. " - " .. RankName(state.rank), 1, 1, 1, true)
    end

    GameTooltip:AddLine("Click to open.", 1, 1, 1, true)
    GameTooltip:Show()
end)

minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

PlaceMinimapButton(DEFAULT_ANGLE)

--------------------------------------------------------------------------
-- Reading what the server says
--------------------------------------------------------------------------

local function HandleMessage(body)
    local verb, rest = string.match(body, "^(%S+)%s*(.*)$")

    if not verb then return end

    if verb == "MAYCREATE" then
        state.mayCreate = (rest == "1")
        return
    end

    if verb == "STATE" then
        local faction, rank, permissions = string.match(rest, "^(%d+) (%d+) (%d+)")

        if not faction then return end

        state.faction = tonumber(faction)
        state.rank = tonumber(rank)
        state.permissions = tonumber(permissions)

        -- Cleared here rather than on DONE: STATE is the first message of a refresh, and
        -- the lines that follow are what refill these.
        state.ranks = {}
        state.spells = {}
        return
    end

    if verb == "FNAME" then
        state.factionName = rest
        return
    end

    if verb == "RANK" then
        local id, name = string.match(rest, "^(%d+) (.*)$")

        if id then state.ranks[tonumber(id)] = name end
        return
    end

    if verb == "SPELL" then
        local id = tonumber(rest)

        if id then table.insert(state.spells, id) end
        return
    end

    if verb == "DONE" then
        RedrawState()
        return
    end

    if verb == "RSTART" then
        state.roster = {}
        state.absent = 0
        selectedMember = nil
        rosterOffset = 0
        return
    end

    if verb == "MEMBER" then
        -- guid then label, because a label can hold spaces and so has to be last.
        local rank, guid, label = string.match(rest, "^(%d+) (%d+) (.*)$")

        if rank then
            table.insert(state.roster,
                { rank = tonumber(rank), guid = guid, label = label })
        end
        return
    end

    if verb == "ABSENT" then
        state.absent = tonumber(rest) or 0
        return
    end

    if verb == "RDONE" then
        RedrawRoster()
        return
    end
end

local events = CreateFrame("Frame")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("CHAT_MSG_ADDON")

events:SetScript("OnEvent", function(self, event, prefix, message)
    if event == "PLAYER_ENTERING_WORLD" then
        playerName = UnitName("player")

        SanctuaryFactionDB = SanctuaryFactionDB or {}
        PlaceMinimapButton(SanctuaryFactionDB.minimapAngle or DEFAULT_ANGLE)

        -- Asked at login so the minimap tooltip can say what you are without the window
        -- ever having been opened.
        Send("HELLO")
        return
    end

    if event == "CHAT_MSG_ADDON" and prefix == ADDON_PREFIX then
        HandleMessage(message)
    end
end)

--------------------------------------------------------------------------

SLASH_SANCTUARYFACTION1 = "/faction"
SlashCmdList["SANCTUARYFACTION"] = function() Toggle() end
