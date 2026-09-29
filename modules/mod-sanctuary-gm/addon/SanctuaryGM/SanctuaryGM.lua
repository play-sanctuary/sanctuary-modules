--[[
    Sanctuary GM - a panel instead of typing

    Search for a creature or an object and click to place it. Teach or take back a spell
    without knowing its id. Morph yourself or your target from a list you can actually
    read. Audition a sound, then play it to a chosen radius. Undo the last thing you put
    down.

    Nothing here is trusted. Every button sends a request the server re-checks against
    the account's security before acting, so this addon in the hands of a player is an
    inert window. It is a convenience for people who already have the rights, not a way
    to acquire them.

    /gm to open.
]]

-- The Timekeeper class-colour/icon registrations and the talent/glyph refusals that went with
-- it were removed on 2026-09-16 along with the class itself.

local ADDON_PREFIX = "SGM"

local state = {
    -- Set only by the server's answer to HELLO. The panel never opens without it, so a
    -- player who is not a game master never sees it, and nothing here decides that
    -- locally - the server is asked, every time.
    allowed = false,
    pendingOpen = false,
    ready = false,
    security = 0,
    searchLimit = 25,
    mode = "creature",     -- creature | object | spell | morph | sound
    permanent = false,
    onTarget = false,
    rows = {},

    -- Spells tab. A row is selected rather than acted on directly: teaching on a
    -- misclick permanently alters somebody's character, which spawning a rat does not.
    spellSelected = nil,
    spellOnTarget = false,
    spellAllRanks = false,

    -- Sounds the voice relay offers, as of the last time the panel asked.
    librarySounds = {},
    librarySound = nil,
}

local playerName

local function Send(body)
    SendAddonMessage(ADDON_PREFIX, body, "WHISPER", playerName or UnitName("player"))
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

local panel = CreateFrame("Frame", "SanctuaryGMPanel", UIParent)
panel:SetWidth(420)
-- The Sounds tab is the busiest: two play mechanisms, two sliders and their explanations
-- come to about 420px of content, and with the 82px header and a 14px margin that needs
-- 518. Below that the voice library runs off the bottom again, so this is close to the
-- floor rather than a round number picked for looks.
panel:SetHeight(540)
panel:SetPoint("CENTER")
panel:SetBackdrop(BACKDROP)
panel:SetBackdropColor(0, 0, 0, 0.92)
panel:SetMovable(true)
panel:EnableMouse(true)
panel:RegisterForDrag("LeftButton")
panel:SetScript("OnDragStart", panel.StartMoving)
panel:SetScript("OnDragStop", panel.StopMovingOrSizing)
panel:SetFrameStrata("DIALOG")
panel:Hide()

tinsert(UISpecialFrames, "SanctuaryGMPanel")

local title = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", panel, "TOP", 0, -14)
title:SetText("Sanctuary Game Master")

local subtitle = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
subtitle:SetPoint("TOP", title, "BOTTOM", 0, -4)
subtitle:SetText("Connecting...")

local close = CreateFrame("Button", nil, panel, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", panel, "TOPRIGHT", -6, -6)

--[[
    Closing the panel tells the server, which takes back the placement spell.

    On OnHide rather than on the close button, because there are four ways out - the button,
    Escape (the panel is in UISpecialFrames), /gm again, and a DENY - and the spell should
    not be left in the spellbook by any of them. The guard is for the Hide() on the line
    above, which runs while the addon is still loading and has nobody to talk to.
]]
panel:SetScript("OnHide", function()
    if state.ready then
        Send("BYE")
    end
end)

--------------------------------------------------------------------------
-- Tabs
--------------------------------------------------------------------------

local TABS = {
    { key = "creature", label = "Creatures" },
    { key = "object",   label = "Objects" },
    { key = "spell",    label = "Spells" },
    { key = "morph",    label = "Morph" },
    { key = "sound",    label = "Sounds" },
}

local tabButtons = {}
local pages = {}

-- The tabs that put things down, and so the only ones with a placement to undo.
local PLACING = { creature = true, object = true }

-- The tabs with something to look at before committing to it.
local PREVIEWING = { creature = true, object = true, morph = true }

-- Built at the end of the file, but ShowMode has to reach it; see "Undo" below.
local undo

-- The preview flyout, built below the tabs and reached by the three tabs above.
local preview
local PreviewObject, PreviewCreature, PreviewCreatureEntry, PreviewNothing

local function ShowMode(mode)
    state.mode = mode

    for _, tab in ipairs(TABS) do
        pages[tab.key]:Hide()

        if tab.key == mode then
            tabButtons[tab.key]:LockHighlight()
        else
            tabButtons[tab.key]:UnlockHighlight()
        end
    end

    pages[mode]:Show()

    if undo then
        if PLACING[mode] then undo:Show() else undo:Hide() end
    end

    if preview then
        if PREVIEWING[mode] then preview:Show() else preview:Hide() end

        -- Whatever was being looked at belongs to the tab that was left.
        PreviewNothing()
    end
end

for index, tab in ipairs(TABS) do
    local button = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    button:SetWidth(78)
    button:SetHeight(22)
    button:SetPoint("TOPLEFT", panel, "TOPLEFT", 14 + ((index - 1) * 80), -52)
    button:SetText(tab.label)
    button:SetScript("OnClick", function() ShowMode(tab.key) end)

    tabButtons[tab.key] = button

    local page = CreateFrame("Frame", nil, panel)
    page:SetPoint("TOPLEFT", panel, "TOPLEFT", 14, -82)
    page:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", -14, 14)
    page:Hide()

    pages[tab.key] = page
end

--------------------------------------------------------------------------
-- Preview, for the Objects and Morph tabs
--------------------------------------------------------------------------

--[[
    Seeing the thing before committing to it: an object before it is put in the world, a
    display id before somebody is turned into it.

    It hangs off the side of the panel rather than living on the pages. The panel is 420
    wide and the result rows are 378 of that, so there is no room beside a list - and a
    preview that pushed the list narrower would cost every tab something to serve two.

    THE TWO HALVES ARE NOT ALIKE.

    A creature display needs nothing from the server: a Model frame takes a display id
    directly through SetCreature, and the Morph tab is given one by whoever is typing.
    (SetDisplayInfo, which later clients use for this, does not exist in 3.3.5 - the string
    is not in the executable. SetCreature is what this client has.)

    An object has no such call. Its model is a FILE, named in GameObjectDisplayInfo.dbc,
    which an addon cannot read - so the server is asked for the path and SetModel is handed
    the answer. The file itself is already in the client; only its name crosses the wire.
]]
do
    preview = CreateFrame("Frame", "SanctuaryGMPreview", panel)
    preview:SetWidth(210)
    preview:SetHeight(260)
    preview:SetPoint("TOPLEFT", panel, "TOPRIGHT", 6, -52)
    preview:SetBackdrop(BACKDROP)
    preview:SetBackdropColor(0, 0, 0, 0.92)
    preview:Hide()

    local title = preview:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    title:SetPoint("TOPLEFT", preview, "TOPLEFT", 14, -14)
    title:SetText("|cffd8b46aPreview|r")

    local hint = preview:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("TOPRIGHT", preview, "TOPRIGHT", -14, -14)
    hint:SetText("drag to turn, scroll to zoom")

    local caption = preview:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    caption:SetPoint("BOTTOMLEFT", preview, "BOTTOMLEFT", 14, 12)
    caption:SetWidth(182)
    caption:SetJustifyH("LEFT")

    local model = CreateFrame("PlayerModel", nil, preview)
    model:SetPoint("TOPLEFT", preview, "TOPLEFT", 12, -32)
    model:SetPoint("BOTTOMRIGHT", preview, "BOTTOMRIGHT", -12, 30)

    -- Turning slowly on its own, because one fixed angle hides as much as it shows - and
    -- draggable, because the angle you want is never the one it stopped at.
    local facing = 0
    local dragging, dragFrom = false, 0

    model:EnableMouse(true)

    model:SetScript("OnMouseDown", function()
        dragging = true
        dragFrom = ({ GetCursorPosition() })[1]
    end)

    model:SetScript("OnMouseUp", function() dragging = false end)

    model:SetScript("OnUpdate", function(self, elapsed)
        if dragging then
            local x = ({ GetCursorPosition() })[1]
            facing = facing + ((x - dragFrom) * 0.02)
            dragFrom = x
        else
            facing = facing + ((elapsed or 0) * 0.5)
        end

        self:SetRotation(facing)
    end)

    --[[
        Zooming, on the wheel.

        The client frames a model for itself and its idea of the right distance is built
        for a humanoid, so a kobold is a speck and a boat fills the frame before you can
        tell which boat. SetModelScale is the zoom this client has - there is no
        SetCameraDistance in 3.3.5 - and it is re-applied after every draw, because setting
        a new model resets it.

        The bounds are wide on purpose: 0.1 is what a gunship needs and 10 is what a rat
        does. It is kept across subjects rather than reset, so walking a list at one zoom
        level compares like with like.
    ]]
    local ZOOM_MIN, ZOOM_MAX = 0.1, 10.0
    local zoom = 1.0

    local function ApplyZoom()
        pcall(model.SetModelScale, model, zoom)
    end

    model:EnableMouseWheel(true)

    model:SetScript("OnMouseWheel", function(_, delta)
        -- A step in proportion to where it is, so the wheel feels the same at either end.
        zoom = zoom * ((delta or 0) > 0 and 1.2 or (1 / 1.2))
        zoom = math.max(ZOOM_MIN, math.min(ZOOM_MAX, zoom))
        ApplyZoom()
    end)

    --[[
        What the panel is pointed at, and what the server has told us about it.

        `wanted` is a string rather than a number because three kinds of thing can be shown
        and they are not interchangeable: display 328 and creature 328 are different
        previews. An answer is painted only if it still matches, since it arrives a round
        trip after the cursor has usually moved on.

        Both caches keep a `false` for "asked, and there is nothing to draw", so a bad entry
        is asked about once rather than on every pass of the mouse.
    ]]
    local wanted
    local objectPaths = {}
    local creatureDisplays = {}

    local function Draw(setter, argument, description)
        model:ClearModel()

        -- Guarded because these are numbers somebody typed and paths somebody else owns.
        -- A model the client cannot draw leaves the frame empty rather than erroring.
        local ok = pcall(setter, model, argument)
        ApplyZoom()

        caption:SetText(ok and description or ("|cffff8800" .. description .. " cannot be drawn.|r"))
    end

    --[[
        Hovering asks after a moment, not instantly.

        Running a mouse down a list of twelve rows on the way to the thirteenth should not
        send twelve requests, so a row asks only once the cursor has settled on it. The
        delay is on the preview rather than the rows because there is one of these and
        twelve of those - twice over, now that creatures have them as well.
    ]]
    local HOVER_DELAY = 0.25
    local hoverKind, hoverEntry, hoverFor

    preview:SetScript("OnUpdate", function(_, elapsed)
        if not hoverEntry then
            return
        end

        hoverFor = hoverFor + (elapsed or 0)

        if hoverFor >= HOVER_DELAY then
            local kind, entry = hoverKind, hoverEntry
            hoverKind, hoverEntry = nil, nil

            if kind == "creature" then
                PreviewCreatureEntry(entry)
            else
                PreviewObject(entry)
            end
        end
    end)

    function PreviewNothing()
        wanted, hoverKind, hoverEntry = nil, nil, nil
        model:ClearModel()
        caption:SetText("")
    end

    -- A display id straight from the Morph box: the client needs no help with these.
    function PreviewCreature(displayId)
        hoverKind, hoverEntry = nil, nil

        if not displayId or displayId <= 0 then
            PreviewNothing()
            return
        end

        wanted = "display " .. displayId
        Draw(model.SetCreature, displayId, "Display " .. displayId)
    end

    -- A creature entry from a search row, which only the server can turn into a display.
    function PreviewCreatureEntry(entry)
        if not entry or entry <= 0 then
            PreviewNothing()
            return
        end

        wanted = "creature " .. entry
        local displayId = creatureDisplays[entry]

        if displayId == nil then
            caption:SetText("Creature " .. entry .. "...")
            Send("CMODEL " .. entry)
            return
        end

        if displayId == false then
            model:ClearModel()
            caption:SetText("|cffff8800Creature " .. entry .. " has no model to show.|r")
            return
        end

        Draw(model.SetCreature, displayId,
            "Creature " .. entry .. " |cff9c9081(display " .. displayId .. ")|r")
    end

    function PreviewObject(entry)
        if not entry or entry <= 0 then
            PreviewNothing()
            return
        end

        wanted = "object " .. entry
        local path = objectPaths[entry]

        if path == nil then
            caption:SetText("Object " .. entry .. "...")
            Send("GMODEL " .. entry)
            return
        end

        if path == false then
            model:ClearModel()
            caption:SetText("|cffff8800Object " .. entry .. " has no model to show.|r")
            return
        end

        Draw(model.SetModel, path, "Object " .. entry)
    end

    --[[ The server's answers. Painted only if they are still what is wanted. ]]
    function preview.OnModelPath(entry, path)
        objectPaths[entry] = path or false

        if wanted == "object " .. entry then
            PreviewObject(entry)
        end
    end

    function preview.OnCreatureDisplay(entry, displayId)
        creatureDisplays[entry] = (displayId and displayId > 0) and displayId or false

        if wanted == "creature " .. entry then
            PreviewCreatureEntry(entry)
        end
    end

    function preview.HoverObject(entry)
        hoverKind, hoverEntry, hoverFor = "object", entry, 0
    end

    function preview.HoverCreature(entry)
        hoverKind, hoverEntry, hoverFor = "creature", entry, 0
    end

    function preview.EndHover()
        hoverKind, hoverEntry = nil, nil
    end
end

--------------------------------------------------------------------------
-- Shared results list, used by the creature and object tabs
--------------------------------------------------------------------------

local RESULT_ROWS = 12
local resultButtons = {}

local function BuildSearchPage(page, placeholder, searchVerb, spawnVerb)
    -- Named: InputBoxTemplate's middle texture anchors to $parentLeft and
    -- $parentRight, which resolve to nothing on an anonymous frame - leaving
    -- the box drawn as two end caps with a gap.
    local box = CreateFrame("EditBox", "SanctuaryGMInputBox1", page, "InputBoxTemplate")
    box:SetWidth(250)
    box:SetHeight(22)
    box:SetPoint("TOPLEFT", page, "TOPLEFT", 6, 0)
    box:SetAutoFocus(false)

    local hint = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", box, "LEFT", 4, 0)
    hint:SetText(placeholder)

    box:SetScript("OnTextChanged", function(self)
        if self:GetText() == "" then hint:Show() else hint:Hide() end
    end)

    local function DoSearch()
        local text = box:GetText()
        if string.len(text) < 2 then
            subtitle:SetText("|cffff8800Type at least two characters.|r")
            return
        end

        state.rows = {}
        Send(searchVerb .. " " .. text)
    end

    box:SetScript("OnEnterPressed", function(self) DoSearch() self:ClearFocus() end)
    box:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    local search = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    search:SetWidth(70)
    search:SetHeight(22)
    search:SetPoint("LEFT", box, "RIGHT", 8, 0)
    search:SetText("Search")
    search:SetScript("OnClick", DoSearch)

    local permanent = CreateFrame("CheckButton", nil, page, "UICheckButtonTemplate")
    permanent:SetWidth(22)
    permanent:SetHeight(22)
    permanent:SetPoint("TOPLEFT", box, "BOTTOMLEFT", -2, -6)
    permanent:SetScript("OnClick", function(self)
        state.permanent = self:GetChecked() and true or false
    end)

    local permanentLabel = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    permanentLabel:SetPoint("LEFT", permanent, "RIGHT", 2, 0)
    permanentLabel:SetText("Save to the database (permanent)")

    --[[
        Removing what you are standing at.

        NEAREST IS THE SELECTION, because there is no other. The client cannot target a
        gameobject - there is no unit frame for a door - so the core's own `.gobject delete`
        works from a guid remembered by an earlier `.gobject target`, which is two commands
        and something to keep track of. Standing next to the thing is the gesture people
        actually use.

        Only on the object page, and only for objects: a stray click here should not be able
        to remove a creature.
    ]]
    if spawnVerb == "GSPAWN" then
        local remove = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
        remove:SetWidth(150)
        remove:SetHeight(22)
        remove:SetPoint("BOTTOMLEFT", page, "BOTTOMLEFT", 6, 38)

        --[[
            Removing by pointing at it, which is the other half of placing by pointing.

            The same reticle and the same spell: the server is told beforehand whether the
            next cast is putting something down or taking something away. A second ground
            targeted spell would have meant a second client patch for a difference the
            server already knows.

            The mode stays on afterwards, so clearing several objects is one press and then
            a click each.
        ]]
        --[[
            One press, not two.

            This button both tells the server the next cast is a removal AND starts the
            reticle, which is why it is a secure button of its own rather than something
            that arms the Place button. PreClick is the piece that makes it possible: it
            runs BEFORE the secure action, is not itself protected, and so can send the
            addon message that sets the mode while the cast that follows opens the reticle.

            There is no race. The message goes out first, and the cast does not LAND until
            the ground is clicked - which is a person's reaction time later, not a packet's.
        ]]
        local removeAt = CreateFrame("Button", "SanctuaryGMDeleteAtButton", page,
            "SecureActionButtonTemplate,UIPanelButtonTemplate")
        removeAt:SetWidth(150)
        removeAt:SetHeight(22)
        removeAt:SetPoint("BOTTOMLEFT", remove, "BOTTOMRIGHT", 8, 0)
        removeAt:SetText("Delete where I click")
        removeAt:RegisterForClicks("LeftButtonUp")
        removeAt:SetAttribute("type", "spell")
        removeAt:SetAttribute("spell", "Place Object")
        removeAt:SetScript("PreClick", function() Send("GPENDING 0 0 1") end)

        removeAt:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_LEFT")
            GameTooltip:SetText("Delete where I click")
            GameTooltip:AddLine("Press this, then click an object. It stays on, so several "
                .. "can be cleared one after another - press it again for each.", 1, 1, 1, true)
            GameTooltip:Show()
        end)

        removeAt:SetScript("OnLeave", function() GameTooltip:Hide() end)
        remove:SetText("Delete nearest")
        remove:SetScript("OnClick", function() Send("GDELETE") end)

        remove:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_LEFT")
            GameTooltip:SetText("Delete the nearest object")
            GameTooltip:AddLine("Within ten yards. A saved object is removed from the "
                .. "database too, so it stays gone after a restart.", 1, 1, 1, true)
            GameTooltip:Show()
        end)

        remove:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    local rows = {}

    for i = 1, RESULT_ROWS do
        --[[
            A result row IS the placement gesture: click it and the reticle opens.

            It has to be a secure button, because casting is protected and a reticle can
            only be opened by casting. PreClick does the rest - it runs before the secure
            action and is not itself protected, so it can tell the server WHAT is being
            placed while the cast that follows asks the player WHERE.

            The choice sticks afterwards, so the same thing can be put down again and
            again. Right-clicking a row is how you stop.
        ]]
        local row = CreateFrame("Button", nil, page, "SecureActionButtonTemplate")
        row:SetWidth(378)
        row:SetHeight(18)
        row:SetPoint("TOPLEFT", permanent, "BOTTOMLEFT", 2, -6 - ((i - 1) * 19))
        row:RegisterForClicks("LeftButtonUp", "RightButtonUp")
        row:SetAttribute("type", "spell")
        row:SetAttribute("spell", "Place Object")

        row:SetScript("PreClick", function(self, button)
            if not self.entry then
                return
            end

            -- Right-click gives this one up. The attribute is cleared for the duration of
            -- the click so the secure half casts nothing, and restored in PostClick.
            if button == "RightButton" then
                self:SetAttribute("spell", nil)
                Send("GPENDING 0 0 0 0")
                return
            end

            Send(string.format("GPENDING %d %d 0 %d", self.entry,
                state.permanent and 1 or 0, spawnVerb == "CSPAWN" and 1 or 0))
        end)

        row:SetScript("PostClick", function(self, button)
            if button == "RightButton" then
                self:SetAttribute("spell", "Place Object")
                SpellStopTargeting()
            end
        end)

        row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.text:SetPoint("LEFT", row, "LEFT", 2, 0)
        row.text:SetJustifyH("LEFT")
        row.text:SetWidth(374)

        row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")

        row:Hide()
        rows[i] = row
    end

    return rows
end

resultButtons.creature = BuildSearchPage(pages.creature, "Creature name or id", "CSEARCH", "CSPAWN")
resultButtons.object = BuildSearchPage(pages.object, "Object name or id", "GSEARCH", "GSPAWN")

--[[
    Hovering a row shows what it is.

    Hovering, rather than clicking: a click on these rows spawns or places the thing, so
    looking would otherwise mean committing first. Attached here rather than inside
    BuildSearchPage because the two lists ask different questions - an object is drawn from
    a file path, a creature from a display id - and the row only knows its entry.
]]
for kind, ask in pairs({ creature = preview.HoverCreature, object = preview.HoverObject }) do
    for _, row in ipairs(resultButtons[kind]) do
        row:SetScript("OnEnter", function(self)
            if self.entry then
                ask(self.entry)
            end
        end)

        row:SetScript("OnLeave", function() preview.EndHover() end)
    end
end

--------------------------------------------------------------------------
-- Spells
--------------------------------------------------------------------------

--[[
    Deliberately not BuildSearchPage. Clicking a creature row spawns a rat you can walk
    away from; clicking a spell row would permanently alter somebody's character. So a
    click only selects, and Learn and Forget are separate, explicit buttons.
]]

local spellSelectedLabel

-- Declared before the rows that call it, so the click handlers close over this local
-- rather than reaching for a global that would not exist yet.
local RenderRows

do
    local page = pages.spell

    -- Named: InputBoxTemplate's middle texture anchors to $parentLeft and
    -- $parentRight, which resolve to nothing on an anonymous frame - leaving
    -- the box drawn as two end caps with a gap.
    local box = CreateFrame("EditBox", "SanctuaryGMInputBox2", page, "InputBoxTemplate")
    box:SetWidth(250)
    box:SetHeight(22)
    box:SetPoint("TOPLEFT", page, "TOPLEFT", 6, 0)
    box:SetAutoFocus(false)

    local hint = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", box, "LEFT", 4, 0)
    hint:SetText("Spell name")

    box:SetScript("OnTextChanged", function(self)
        if self:GetText() == "" then hint:Show() else hint:Hide() end
    end)

    local function DoSearch()
        local text = box:GetText()
        if string.len(text) < 2 then
            subtitle:SetText("|cffff8800Type at least two characters.|r")
            return
        end

        state.rows = {}
        state.spellSelected = nil
        Send("SSEARCH " .. text)
    end

    box:SetScript("OnEnterPressed", function(self) DoSearch() self:ClearFocus() end)
    box:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    local search = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    search:SetWidth(70)
    search:SetHeight(22)
    search:SetPoint("LEFT", box, "RIGHT", 8, 0)
    search:SetText("Search")
    search:SetScript("OnClick", DoSearch)

    local onTarget = CreateFrame("CheckButton", nil, page, "UICheckButtonTemplate")
    onTarget:SetWidth(22)
    onTarget:SetHeight(22)
    onTarget:SetPoint("TOPLEFT", box, "BOTTOMLEFT", -2, -4)
    onTarget:SetScript("OnClick", function(self)
        state.spellOnTarget = self:GetChecked() and true or false
    end)

    local onTargetLabel = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    onTargetLabel:SetPoint("LEFT", onTarget, "RIGHT", 2, 0)
    onTargetLabel:SetText("Apply to my target instead of me")

    local allRanks = CreateFrame("CheckButton", nil, page, "UICheckButtonTemplate")
    allRanks:SetWidth(22)
    allRanks:SetHeight(22)
    allRanks:SetPoint("LEFT", onTargetLabel, "RIGHT", 12, 0)
    allRanks:SetScript("OnClick", function(self)
        state.spellAllRanks = self:GetChecked() and true or false
    end)

    local allRanksLabel = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    allRanksLabel:SetPoint("LEFT", allRanks, "RIGHT", 2, 0)
    allRanksLabel:SetText("All ranks")

    local rows = {}

    for i = 1, RESULT_ROWS do
        local row = CreateFrame("Button", nil, page)
        row:SetWidth(378)
        row:SetHeight(18)
        row:SetPoint("TOPLEFT", onTarget, "BOTTOMLEFT", 2, -6 - ((i - 1) * 19))

        --[[
            The spell's own icon, and its own tooltip.

            Both are read from the client rather than sent over the wire: GetSpellInfo takes
            the icon out of the client's Spell.dbc, and SetHyperlink("spell:id") shows the
            same tooltip the spellbook does - description, range, cost, cast time. Sending
            any of that from the server would be a second copy to keep in step, and it would
            be the copy nobody looks at that drifted.
        ]]
        row.icon = row:CreateTexture(nil, "ARTWORK")
        row.icon:SetWidth(16)
        row.icon:SetHeight(16)
        row.icon:SetPoint("LEFT", row, "LEFT", 2, 0)

        row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.text:SetPoint("LEFT", row.icon, "RIGHT", 4, 0)
        row.text:SetJustifyH("LEFT")
        row.text:SetWidth(354)

        row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")

        row:SetScript("OnEnter", function(self)
            if not self.entry then
                return
            end

            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetHyperlink("spell:" .. self.entry)
            GameTooltip:Show()
        end)

        row:SetScript("OnLeave", function() GameTooltip:Hide() end)

        row:SetScript("OnClick", function(self)
            if not self.entry then
                return
            end

            state.spellSelected = self.entry
            RenderRows()
        end)

        row:Hide()
        rows[i] = row
    end

    resultButtons.spell = rows

    spellSelectedLabel = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    spellSelectedLabel:SetPoint("BOTTOMLEFT", page, "BOTTOMLEFT", 6, 30)
    spellSelectedLabel:SetJustifyH("LEFT")
    spellSelectedLabel:SetWidth(374)
    spellSelectedLabel:SetText("|cff9c9081Nothing selected.|r")

    local function Act(verb)
        if not state.spellSelected then
            subtitle:SetText("|cffff8800Pick a spell from the list first.|r")
            return
        end

        Send(string.format("%s %d %d %d", verb, state.spellSelected,
            state.spellOnTarget and 1 or 0, state.spellAllRanks and 1 or 0))
    end

    local learn = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    learn:SetWidth(100)
    learn:SetHeight(22)
    learn:SetPoint("BOTTOMLEFT", page, "BOTTOMLEFT", 6, 4)
    learn:SetText("Learn")
    learn:SetScript("OnClick", function() Act("SLEARN") end)

    local forget = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    forget:SetWidth(100)
    forget:SetHeight(22)
    forget:SetPoint("LEFT", learn, "RIGHT", 6, 0)
    forget:SetText("Forget")
    forget:SetScript("OnClick", function() Act("SFORGET") end)
end

RenderRows = function()
    local rows = resultButtons[state.mode]
    if not rows then
        return
    end

    for i = 1, RESULT_ROWS do
        local data = state.rows[i]

        if data then
            rows[i].entry = data.entry
            rows[i].text:SetText(data.label)

            -- Only the spell rows carry an icon; the others have none and skip this.
            if rows[i].icon then
                local _, _, icon = GetSpellInfo(data.entry)

                -- A spell the client has never heard of still gets a row: the server knows
                -- it, and a question mark is a clearer symptom than a missing line.
                rows[i].icon:SetTexture(icon or "Interface\\Icons\\INV_Misc_QuestionMark")
            end

            rows[i]:Show()

            -- Only the Spells tab has a persistent selection; the others act on click.
            if state.mode == "spell" then
                if data.entry == state.spellSelected then
                    rows[i]:LockHighlight()
                else
                    rows[i]:UnlockHighlight()
                end
            end
        else
            rows[i].entry = nil
            rows[i]:Hide()
        end
    end

    if state.mode == "spell" and spellSelectedLabel then
        local selected

        for _, data in ipairs(state.rows) do
            if data.entry == state.spellSelected then
                selected = data
                break
            end
        end

        spellSelectedLabel:SetText(selected
            and ("Selected: " .. selected.label)
            or "|cff9c9081Nothing selected.|r")
    end
end

--------------------------------------------------------------------------
-- Morph
--------------------------------------------------------------------------

do
    local page = pages.morph

    -- InputBoxTemplate's border art does not shrink to a narrow box: set it to 120 and a
    -- second box-shaped patch of border is left sitting beside the field. The search tabs
    -- never showed it because theirs are 250 wide, so these match them, with the caption
    -- inside the field the way those do.
    -- Named: InputBoxTemplate's middle texture anchors to $parentLeft and
    -- $parentRight, which resolve to nothing on an anonymous frame - leaving
    -- the box drawn as two end caps with a gap.
    local box = CreateFrame("EditBox", "SanctuaryGMInputBox3", page, "InputBoxTemplate")
    box:SetWidth(250)
    box:SetHeight(22)
    box:SetPoint("TOPLEFT", page, "TOPLEFT", 6, 0)
    box:SetAutoFocus(false)
    box:SetNumeric(true)

    local hint = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", box, "LEFT", 4, 0)
    hint:SetText("Display id")

    box:SetScript("OnTextChanged", function(self)
        if self:GetText() == "" then hint:Show() else hint:Hide() end

        -- Previewed as it is typed. The box is numeric, so this is a number or nothing,
        -- and a half-typed id simply draws whatever that id happens to be.
        PreviewCreature(self:GetNumber())
    end)

    local onTarget = CreateFrame("CheckButton", nil, page, "UICheckButtonTemplate")
    onTarget:SetWidth(22)
    onTarget:SetHeight(22)
    onTarget:SetPoint("TOPLEFT", box, "BOTTOMLEFT", -2, -6)
    onTarget:SetScript("OnClick", function(self)
        state.onTarget = self:GetChecked() and true or false
    end)

    local onTargetLabel = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    onTargetLabel:SetPoint("LEFT", onTarget, "RIGHT", 2, 0)
    onTargetLabel:SetText("Apply to my target instead of me")

    local apply = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    apply:SetWidth(90)
    apply:SetHeight(22)
    apply:SetPoint("TOPLEFT", onTarget, "BOTTOMLEFT", 2, -8)
    apply:SetText("Apply")
    apply:SetScript("OnClick", function()
        Send(string.format("MORPH %d %d", box:GetNumber() or 0, state.onTarget and 1 or 0))
    end)

    local revert = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    revert:SetWidth(90)
    revert:SetHeight(22)
    revert:SetPoint("LEFT", apply, "RIGHT", 8, 0)
    revert:SetText("Revert")
    revert:SetScript("OnClick", function()
        Send(string.format("MORPH 0 %d", state.onTarget and 1 or 0))
    end)

    -- Scale
    local scaleLabel = page:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    scaleLabel:SetPoint("TOPLEFT", apply, "BOTTOMLEFT", 0, -20)
    scaleLabel:SetText("Scale")

    local scaleValue = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    scaleValue:SetPoint("LEFT", scaleLabel, "RIGHT", 10, 0)
    scaleValue:SetText("1.00")

    local scale = CreateFrame("Slider", "SanctuaryGMScale", page, "OptionsSliderTemplate")
    scale:SetWidth(240)
    scale:SetPoint("TOPLEFT", scaleLabel, "BOTTOMLEFT", 4, -12)
    scale:SetMinMaxValues(0.1, 5.0)
    scale:SetValueStep(0.05)
    scale:SetValue(1.0)
    _G["SanctuaryGMScaleLow"]:SetText("0.1")
    _G["SanctuaryGMScaleHigh"]:SetText("5.0")
    _G["SanctuaryGMScaleText"]:SetText("")

    scale:SetScript("OnValueChanged", function(self, value)
        scaleValue:SetText(string.format("%.2f", value))
    end)

    -- Applied on release rather than on every tick: dragging the slider would otherwise
    -- be one addon message per frame.
    scale:SetScript("OnMouseUp", function(self)
        Send(string.format("SCALE %.2f %d", self:GetValue(), state.onTarget and 1 or 0))
    end)

    local resetScale = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    resetScale:SetWidth(90)
    resetScale:SetHeight(22)
    resetScale:SetPoint("TOPLEFT", scale, "BOTTOMLEFT", -4, -14)
    resetScale:SetText("Reset scale")
    resetScale:SetScript("OnClick", function()
        scale:SetValue(1.0)
        Send(string.format("SCALE 1.00 %d", state.onTarget and 1 or 0))
    end)

    local note = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    note:SetPoint("TOPLEFT", resetScale, "BOTTOMLEFT", 0, -16)
    note:SetWidth(370)
    note:SetJustifyH("LEFT")
    note:SetText("Display ids come from CreatureDisplayInfo.dbc. The server refuses any id "
        .. "that does not exist, so a wrong number is harmless.")
end

--------------------------------------------------------------------------
-- Sounds
--------------------------------------------------------------------------

do
    local page = pages.sound

    -- InputBoxTemplate's border art does not shrink to a narrow box: set it to 120 and a
    -- second box-shaped patch of border is left sitting beside the field. The search tabs
    -- never showed it because theirs are 250 wide, so these match them, with the caption
    -- inside the field the way those do.
    -- Named: InputBoxTemplate's middle texture anchors to $parentLeft and
    -- $parentRight, which resolve to nothing on an anonymous frame - leaving
    -- the box drawn as two end caps with a gap.
    local box = CreateFrame("EditBox", "SanctuaryGMInputBox4", page, "InputBoxTemplate")
    box:SetWidth(250)
    box:SetHeight(22)
    box:SetPoint("TOPLEFT", page, "TOPLEFT", 6, 0)
    box:SetAutoFocus(false)
    box:SetNumeric(true)

    local hint = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("LEFT", box, "LEFT", 4, 0)
    hint:SetText("Sound id")

    box:SetScript("OnTextChanged", function(self)
        if self:GetText() == "" then hint:Show() else hint:Hide() end
    end)

    local scope = "area"

    local scopes = {
        { key = "self",   label = "Only me" },
        { key = "target", label = "My target" },
        { key = "area",   label = "Everyone nearby" },
    }

    local scopeButtons = {}

    for index, entry in ipairs(scopes) do
        local button = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
        button:SetWidth(118)
        button:SetHeight(22)
        button:SetPoint("TOPLEFT", box, "BOTTOMLEFT", -2 + ((index - 1) * 122), -8)
        button:SetText(entry.label)

        button:SetScript("OnClick", function()
            scope = entry.key
            for _, other in ipairs(scopeButtons) do
                other:UnlockHighlight()
            end
            button:LockHighlight()
        end)

        scopeButtons[index] = button
    end

    -- How far "nearby" reaches, and whether the sound has a direction.
    local radiusLabel = page:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    radiusLabel:SetPoint("TOPLEFT", scopeButtons[1], "BOTTOMLEFT", 2, -14)
    radiusLabel:SetText("Radius")

    local radiusValue = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    radiusValue:SetPoint("LEFT", radiusLabel, "RIGHT", 10, 0)
    radiusValue:SetText("40 yd")

    local radius = CreateFrame("Slider", "SanctuaryGMRadius", page, "OptionsSliderTemplate")
    radius:SetWidth(240)
    radius:SetPoint("TOPLEFT", radiusLabel, "BOTTOMLEFT", 4, -12)
    radius:SetMinMaxValues(5, 100)
    radius:SetValueStep(5)
    radius:SetValue(40)
    _G["SanctuaryGMRadiusLow"]:SetText("5")
    _G["SanctuaryGMRadiusHigh"]:SetText("100")
    _G["SanctuaryGMRadiusText"]:SetText("")

    radius:SetScript("OnValueChanged", function(self, value)
        radiusValue:SetText(string.format("%d yd", value))
    end)

    local positional = CreateFrame("CheckButton", nil, page, "UICheckButtonTemplate")
    positional:SetWidth(22)
    positional:SetHeight(22)
    positional:SetPoint("TOPLEFT", radius, "BOTTOMLEFT", -6, -8)
    positional:SetChecked(true)

    local positionalLabel = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    positionalLabel:SetPoint("LEFT", positional, "RIGHT", 2, 0)
    positionalLabel:SetText("Comes from where I am standing")

    -- The radius only means anything for the area scope.
    local function UpdateScopeControls()
        local area = scope == "area"

        radiusLabel:SetAlpha(area and 1 or 0.35)
        radiusValue:SetAlpha(area and 1 or 0.35)
        positionalLabel:SetAlpha(area and 1 or 0.35)

        if area then
            radius:Enable()
            positional:Enable()
        else
            radius:Disable()
            positional:Disable()
        end
    end

    for index, entry in ipairs(scopes) do
        scopeButtons[index]:SetScript("OnClick", function()
            scope = entry.key
            for _, other in ipairs(scopeButtons) do
                other:UnlockHighlight()
            end
            scopeButtons[index]:LockHighlight()
            UpdateScopeControls()
        end)
    end

    scopeButtons[3]:LockHighlight()
    UpdateScopeControls()

    -- Audition locally first. The client can play any sound id on its own, so a game
    -- master can check what something is before playing it to a whole zone.
    local audition = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    audition:SetWidth(118)
    audition:SetHeight(22)
    audition:SetPoint("TOPLEFT", positional, "BOTTOMLEFT", 6, -12)
    audition:SetText("Preview")
    audition:SetScript("OnClick", function()
        local id = box:GetNumber() or 0
        if id > 0 then
            PlaySound(id)
        end
    end)

    local play = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    play:SetWidth(118)
    play:SetHeight(22)
    play:SetPoint("LEFT", audition, "RIGHT", 4, 0)
    play:SetText("Play")
    play:SetScript("OnClick", function()
        Send(string.format("SOUND %d %s %d %d",
            box:GetNumber() or 0, scope, radius:GetValue(),
            positional:GetChecked() and 1 or 0))
    end)

    local note = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    note:SetPoint("TOPLEFT", audition, "BOTTOMLEFT", 0, -18)
    note:SetWidth(370)
    note:SetJustifyH("LEFT")
    note:SetText("Preview plays on your machine only and needs no permission. Play goes "
        .. "through the server and is checked there. The radius is capped at the realm's "
        .. "visibility distance, because a sound cannot reach someone the server is not "
        .. "already showing you.")

    ----------------------------------------------------------------------
    -- Voice library
    ----------------------------------------------------------------------

    --[[
        The other way to play something, and the one that actually fades with distance.

        A game sound above is a sound id the client plays at whatever volume its own DBC
        row says, because no 3.3.5a sound packet carries a volume at all. A library sound
        is routed through the voice relay as though somebody were speaking from where you
        stand, so it gets the same attenuation, stereo placement and range cutoff as a
        voice - at the cost of only reaching players who have voice connected.
    ]]

    local libraryTitle = page:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    libraryTitle:SetPoint("TOPLEFT", note, "BOTTOMLEFT", 0, -16)
    libraryTitle:SetText("Voice library |cffd8b46a(fades with distance)|r")

    local libraryNote = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    libraryNote:SetPoint("TOPLEFT", libraryTitle, "BOTTOMLEFT", 0, -4)
    libraryNote:SetWidth(370)
    libraryNote:SetJustifyH("LEFT")
    libraryNote:SetText("Played through the voice server, so it fades exactly as a voice "
        .. "does. Only players with voice connected can hear it.")

    local libraryDrop = CreateFrame("Frame", "SanctuaryGMLibrary", page, "UIDropDownMenuTemplate")
    libraryDrop:SetPoint("TOPLEFT", libraryNote, "BOTTOMLEFT", -16, -6)

    local libraryRangeLabel = page:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    libraryRangeLabel:SetPoint("TOPLEFT", libraryDrop, "BOTTOMLEFT", 20, -8)
    libraryRangeLabel:SetText("Radius")

    local libraryRangeValue = page:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    libraryRangeValue:SetPoint("LEFT", libraryRangeLabel, "RIGHT", 10, 0)
    libraryRangeValue:SetText("40 yd")

    local libraryRange = CreateFrame("Slider", "SanctuaryGMLibraryRange", page, "OptionsSliderTemplate")
    libraryRange:SetWidth(240)
    libraryRange:SetPoint("TOPLEFT", libraryRangeLabel, "BOTTOMLEFT", 4, -12)
    libraryRange:SetMinMaxValues(5, 200)
    libraryRange:SetValueStep(5)
    libraryRange:SetValue(40)
    _G["SanctuaryGMLibraryRangeLow"]:SetText("5")
    _G["SanctuaryGMLibraryRangeHigh"]:SetText("200")
    _G["SanctuaryGMLibraryRangeText"]:SetText("")

    libraryRange:SetScript("OnValueChanged", function(self, value)
        libraryRangeValue:SetText(string.format("%d yd", value))
    end)

    local function SelectSound(name)
        state.librarySound = name
        UIDropDownMenu_SetText(libraryDrop, name or "No sounds available")
    end

    -- Rebuilt from whatever the server last told us, so a relay restart with a different
    -- folder is reflected the next time the panel is opened.
    state.RefreshLibrary = function()
        UIDropDownMenu_Initialize(libraryDrop, function()
            for _, name in ipairs(state.librarySounds) do
                local entry = UIDropDownMenu_CreateInfo()
                entry.text = name
                entry.checked = (name == state.librarySound)
                entry.func = function() SelectSound(name) end
                UIDropDownMenu_AddButton(entry)
            end
        end)

        UIDropDownMenu_SetWidth(libraryDrop, 150)

        if state.librarySound and not tContains(state.librarySounds, state.librarySound) then
            state.librarySound = nil
        end

        SelectSound(state.librarySound or state.librarySounds[1])
    end

    state.RefreshLibrary()

    local libraryPlay = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    libraryPlay:SetWidth(118)
    libraryPlay:SetHeight(22)
    libraryPlay:SetPoint("TOPLEFT", libraryRange, "BOTTOMLEFT", -4, -14)
    libraryPlay:SetText("Play")
    libraryPlay:SetScript("OnClick", function()
        if not state.librarySound then
            subtitle:SetText("|cffff8800The voice server has no sounds.|r")
            return
        end

        -- Range first: the name is taken to the end of the line, so it may contain spaces.
        Send(string.format("RPLAY %d %s", libraryRange:GetValue(), state.librarySound))
    end)

    local libraryStop = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    libraryStop:SetWidth(118)
    libraryStop:SetHeight(22)
    libraryStop:SetPoint("LEFT", libraryPlay, "RIGHT", 4, 0)
    libraryStop:SetText("Stop")
    libraryStop:SetScript("OnClick", function() Send("RSTOP") end)

    -- Asks the voice server to look at its folder again, rather than re-reading the list
    -- it sent when it connected. Dropping a file in and pressing this is the whole point:
    -- without it a new sound needs the relay restarted, which drops everyone's voice.
    local libraryRefresh = CreateFrame("Button", nil, page, "UIPanelButtonTemplate")
    libraryRefresh:SetWidth(118)
    libraryRefresh:SetHeight(22)
    libraryRefresh:SetPoint("LEFT", libraryStop, "RIGHT", 4, 0)
    libraryRefresh:SetText("Refresh")
    libraryRefresh:SetScript("OnClick", function()
        subtitle:SetText("Re-scanning the sound folder...")
        Send("RELOAD")
    end)

    libraryRefresh:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine("Refresh")
        GameTooltip:AddLine("Re-reads the voice server's sound folder, so a file added "
            .. "since it started can be played without restarting it.", 1, 1, 1, true)
        GameTooltip:Show()
    end)
    libraryRefresh:SetScript("OnLeave", function() GameTooltip:Hide() end)

    -- Filled in by the server, because only it knows where it looked. Players never see
    -- this tab, so a server-side path here reaches nobody who should not have it.
    local libraryFolder = page:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    libraryFolder:SetPoint("TOPLEFT", libraryPlay, "BOTTOMLEFT", 4, -10)
    libraryFolder:SetWidth(370)
    libraryFolder:SetJustifyH("LEFT")
    libraryFolder:SetText("")

    state.SetLibraryFolder = function(path)
        if path and path ~= "" then
            libraryFolder:SetText("|cff9c9081Folder:|r " .. path)
        else
            libraryFolder:SetText("")
        end
    end
end

--------------------------------------------------------------------------
-- Undo, shared by the two placing tabs
--------------------------------------------------------------------------

undo = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
undo:SetWidth(150)
undo:SetHeight(22)
-- In the row with the other two rather than off in the panel's corner. Anchored to the
-- delete button by name because that button belongs to the object page, which is built
-- before this - and a frame's position does not depend on whether its page is showing.
--
-- Which is also why ShowMode hides it everywhere else. It belongs to the panel, not to a
-- page, so hiding a page does not hide it - and on the Spells and Sounds tabs it sat in
-- that same spot, behind Learn and Forget and behind the sound buttons.
undo:SetPoint("TOPLEFT", SanctuaryGMDeleteAtButton, "BOTTOMLEFT", -158, -6)
undo:SetText("Undo last placement")
undo:SetScript("OnClick", function() Send("UNDO") end)
if not PLACING[state.mode] then undo:Hide() end

--------------------------------------------------------------------------
-- Incoming
--------------------------------------------------------------------------

--[[
    Opening is a separate function because it happens from two places: the slash command
    when permission is already known, and the DENY/READY handler when it was not - so the
    first /gm of a session opens the panel rather than being spent finding out.
]]
local function OpenPanel()
    panel:Show()
    ShowMode(state.mode)

    -- Asked every time the panel opens rather than cached: security can change while
    -- someone is logged in, and a stale answer would leave dead buttons on screen.
    subtitle:SetText("Connecting...")
    Send("HELLO")

    -- The relay may have been restarted with a different folder since last time, so the
    -- list is re-asked rather than cached across openings.
    Send("RSOUNDS")
end

local function Handle(body)
    local verb, rest = string.match(body, "^(%S+)%s*(.*)$")

    if verb == "DENY" then
        state.allowed = false
        state.ready = false
        state.pendingOpen = false
        panel:Hide()
        return
    end

    if verb == "READY" then
        local security, limit = string.match(rest, "^(%d+)%s+(%d+)$")

        state.allowed = true
        state.ready = true
        state.security = tonumber(security) or 0
        state.searchLimit = tonumber(limit) or 25

        subtitle:SetText(string.format("|cff7fb069Connected.|r Security level %d.", state.security))

        if state.pendingOpen then
            state.pendingOpen = false
            OpenPanel()
        end

        return
    end

    if verb == "MSG" then
        subtitle:SetText(rest)
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aGM:|r " .. rest)
        return
    end

    if verb == "CRESULT" or verb == "GRESULT" or verb == "SRESULT" then
        state.rows = {}
        local count = tonumber(rest) or 0

        if verb == "SRESULT" then
            state.spellSelected = nil
        end

        subtitle:SetText(count > 0
            and string.format("%d result(s).", count)
            or "|cffff8800Nothing matched.|r")

        RenderRows()
        return
    end

    if verb == "SROW" then
        -- "SROW <id> <known 0|1> <name> (<rank>)"
        local id, known, name = string.match(rest, "^(%d+)%s+([01])%s+(.*)$")
        if id then
            table.insert(state.rows, {
                entry = tonumber(id),
                -- Already known is greyed rather than hidden: seeing that a rank is
                -- already learned is the whole reason to look before teaching.
                label = known == "1"
                    and string.format("|cff9c9081%s  %s  (known)|r", id, name)
                    or string.format("|cffd8b46a%s|r  %s", id, name),
            })
            RenderRows()
        end
        return
    end

    if verb == "CROW" then
        local entry, minLevel, maxLevel, name = string.match(rest, "^(%d+)%s+(%d+)%s+(%d+)%s+(.*)$")
        if entry then
            table.insert(state.rows, {
                entry = tonumber(entry),
                label = string.format("|cffd8b46a%s|r  %s  |cff9c9081(lvl %s-%s)|r", entry, name, minLevel, maxLevel),
            })
            RenderRows()
        end
        return
    end

    if verb == "GMODEL" then
        -- "GMODEL <entry> <path>" - the path is taken to the end of the line, since model
        -- paths contain spaces ("World\Generic\Human\Passive Doodads\...").
        local entry, path = string.match(rest, "^(%d+)%s+(.+)$")
        if entry then
            preview.OnModelPath(tonumber(entry), path)
        end
        return
    end

    if verb == "CMODEL" then
        -- "CMODEL <entry> <display id>"
        local entry, displayId = string.match(rest, "^(%d+)%s+(%d+)$")
        if entry then
            preview.OnCreatureDisplay(tonumber(entry), tonumber(displayId))
        end
        return
    end

    if verb == "GROW" then
        local entry, goType, name = string.match(rest, "^(%d+)%s+(%d+)%s+(.*)$")
        if entry then
            table.insert(state.rows, {
                entry = tonumber(entry),
                label = string.format("|cffd8b46a%s|r  %s  |cff9c9081(type %s)|r", entry, name, goType),
            })
            RenderRows()
        end
        return
    end

    if verb == "RSOUNDS" then
        state.librarySounds = {}

        if state.RefreshLibrary then
            state.RefreshLibrary()
        end

        -- Said on every list, not only on a refresh: it costs a line and it is the only
        -- confirmation that the button did anything when the count has not changed.
        subtitle:SetText(string.format("|cff7fb069%s sound(s) in the library.|r", rest))
        return
    end

    if verb == "RSOUNDDIR" then
        if state.SetLibraryFolder then
            state.SetLibraryFolder(rest)
        end
        return
    end

    if verb == "RSOUND" then
        table.insert(state.librarySounds, rest)

        if state.RefreshLibrary then
            state.RefreshLibrary()
        end
        return
    end

    if verb == "PLACED" then
        subtitle:SetText("|cff7fb069Placed.|r Undo is available.")
        return
    end
end

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("PLAYER_LOGIN")

listener:SetScript("OnEvent", function(self, event, prefix, message)
    if event == "CHAT_MSG_ADDON" then
        if prefix == ADDON_PREFIX then
            Handle(message)
        end
        return
    end

    playerName = UnitName("player")

    -- Asked on login rather than only on open, so the very first /gm of a session already
    -- knows the answer instead of swallowing the keystroke while it finds out.
    Send("HELLO")
end)

--------------------------------------------------------------------------
-- Slash command
--------------------------------------------------------------------------

SLASH_SANCTUARYGM1 = "/gm"
SLASH_SANCTUARYGM2 = "/sgm"

SlashCmdList["SANCTUARYGM"] = function()
    if panel:IsShown() then
        panel:Hide()
        return
    end

    -- Silent for everyone else. Telling a player they are not a game master would only
    -- advertise that there is a panel to want. The answer decides: READY opens it, DENY
    -- does nothing at all.
    if not state.allowed then
        state.pendingOpen = true
        Send("HELLO")
        return
    end

    OpenPanel()
end
