--[[
    Sanctuary Profile - a glance at somebody

    Target a character and a small window says what you can see: their appearance, any
    injuries, how they carry themselves, and one detail worth noticing. Somebody who has
    written nothing still opens a window, which says so.

    That is all it is meant to say. The fields are short by design and the server enforces
    the cap - everything else about a person is supposed to come out in character.

    /profile writes your own.

    Protocol, on the SPRO prefix, carried as a whisper to yourself:
        out  V:<hex guid>            let me look at this character
        out  S:<slot>:<text>         set one line of my own description
        out  C:0                     clear my description
        out  R:0                     send me my own copy
        in   H:<hex guid>:<label>    heading; the label is the name or the alias
        in   F:<hex guid>:<slot>:<text>
        in   E:<hex guid>:<0|1>      end of profile, 1 if it had anything in it
        in   M:<text>                something to tell the player

    The heading arrives already resolved by the server. A stranger is "Hooded Orc" here for
    the same reason they are everywhere else: the addon is never told their real name.
]]

local ADDON_PREFIX = "SPRO"

local FIELDS = {
    [0] = { key = "appearance", label = "Appearance",   hint = "What someone sees at a glance." },
    [1] = { key = "injuries",   label = "Injuries",     hint = "Visible wounds, ailments, how badly." },
    [2] = { key = "manner",     label = "Manner",       hint = "How they hold themselves." },
    [3] = { key = "detail",     label = "Detail",       hint = "One thing worth noticing." },
}

local FIELD_ORDER = { 0, 1, 2, 3 }

local MAX_LENGTH = 160

-- The profile currently being assembled or shown, keyed by the guid the server answered
-- about. Replies arrive as separate messages, so they have to be collected.
local incoming = {}

-- Set while a reply about ourselves is meant to fill the editor rather than open the
-- reading window. Both arrive as the same messages about the same guid, so there is no way
-- to tell them apart from the wire alone - Preview is exactly the case that breaks
-- otherwise, since it asks to be shown a profile while the editor is still open.
local awaitingEditor = false

local playerName
local playerKey

local function Send(body)
    SendAddonMessage(ADDON_PREFIX, body, "WHISPER", playerName or UnitName("player"))
end

--- The guid as the server spells it: hex, uppercase, without the 0x the client prefixes.
local function GuidKey(unit)
    local guid = UnitGUID(unit)
    if not guid then
        return nil
    end

    return string.upper(string.sub(guid, 3))
end

--------------------------------------------------------------------------
-- The window you read
--------------------------------------------------------------------------

local BACKDROP = {
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true,
    tileSize = 32,
    edgeSize = 16,
    insets = { left = 5, right = 5, top = 5, bottom = 5 },
}

--[[
    A solid sheet behind the panel contents.

    The dialog backdrop alone leaves the world showing through, which is tolerable on a
    label and not on a form you are typing into. In 3.3.5a a texture is filled with a flat
    colour by passing SetTexture four numbers; SetColorTexture does not exist yet.
]]
local function AddSolidBacking(frame)
    local sheet = frame:CreateTexture(nil, "BACKGROUND")
    sheet:SetPoint("TOPLEFT", frame, "TOPLEFT", 5, -5)
    sheet:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -5, 5)
    sheet:SetTexture(0.055, 0.045, 0.035, 0.97)
    return sheet
end

local viewer = CreateFrame("Frame", "SanctuaryProfileFrame", UIParent)
viewer:SetWidth(320)
viewer:SetHeight(240)
viewer:SetPoint("CENTER", UIParent, "CENTER", 240, 0)
viewer:SetBackdrop(BACKDROP)
viewer:SetBackdropColor(0, 0, 0, 0.9)
viewer:SetMovable(true)
viewer:EnableMouse(true)
viewer:RegisterForDrag("LeftButton")
viewer:SetScript("OnDragStart", viewer.StartMoving)
viewer:SetScript("OnDragStop", viewer.StopMovingOrSizing)
viewer:SetFrameStrata("HIGH")
viewer:Hide()

AddSolidBacking(viewer)

-- Escape closes it, like every other panel in the game.
tinsert(UISpecialFrames, "SanctuaryProfileFrame")

local viewerName = viewer:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
viewerName:SetPoint("TOPLEFT", viewer, "TOPLEFT", 16, -16)
viewerName:SetWidth(260)
viewerName:SetJustifyH("LEFT")

local viewerClose = CreateFrame("Button", nil, viewer, "UIPanelCloseButton")
viewerClose:SetPoint("TOPRIGHT", viewer, "TOPRIGHT", -4, -4)

local divider = viewer:CreateTexture(nil, "ARTWORK")
divider:SetTexture("Interface\\Tooltips\\UI-Tooltip-Background")
divider:SetVertexColor(0.85, 0.71, 0.42, 0.5)
divider:SetHeight(1)
divider:SetPoint("TOPLEFT", viewerName, "BOTTOMLEFT", 0, -8)
divider:SetPoint("TOPRIGHT", viewer, "TOPRIGHT", -16, 0)

-- One block per field: a gold heading and the text under it.
local blocks = {}

for index, slot in ipairs(FIELD_ORDER) do
    local block = {}

    block.heading = viewer:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    block.heading:SetJustifyH("LEFT")
    block.heading:SetText(FIELDS[slot].label)

    block.body = viewer:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    block.body:SetJustifyH("LEFT")
    block.body:SetWidth(280)
    block.body:SetPoint("TOPLEFT", block.heading, "BOTTOMLEFT", 0, -2)

    if index == 1 then
        block.heading:SetPoint("TOPLEFT", divider, "BOTTOMLEFT", 0, -10)
    else
        block.heading:SetPoint("TOPLEFT", blocks[index - 1].body, "BOTTOMLEFT", 0, -10)
    end

    blocks[index] = block
end

local emptyNote = viewer:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
emptyNote:SetPoint("TOPLEFT", divider, "BOTTOMLEFT", 0, -14)
emptyNote:SetWidth(280)
emptyNote:SetJustifyH("LEFT")
emptyNote:SetText("Nothing stands out about them.")
emptyNote:Hide()

--- Lays the blocks out for whatever is filled in, and sizes the window to match.
local function RenderViewer(profile)
    local shown = 0
    local height = 0

    for index, slot in ipairs(FIELD_ORDER) do
        local block = blocks[index]
        local text = profile.fields and profile.fields[slot]

        if text and text ~= "" then
            block.heading:Show()
            block.body:SetText(text)
            block.body:Show()

            shown = shown + 1
            -- The body wraps, so its real height is only known once the text is set.
            height = height + 10 + block.heading:GetHeight() + 2 + block.body:GetStringHeight()
        else
            block.heading:Hide()
            block.body:SetText("")
            block.body:Hide()
        end
    end

    if shown == 0 then
        emptyNote:Show()
        height = 30
    else
        emptyNote:Hide()
    end

    viewerName:SetText(profile.label or "Someone")
    viewer:SetHeight(16 + viewerName:GetHeight() + 9 + height + 20)
    viewer:Show()
end

--------------------------------------------------------------------------
-- The window you write
--------------------------------------------------------------------------

local editor = CreateFrame("Frame", "SanctuaryProfileEditor", UIParent)
editor:SetWidth(430)
editor:SetHeight(420)
editor:SetPoint("CENTER")
editor:SetBackdrop(BACKDROP)
editor:SetBackdropColor(0, 0, 0, 0.94)
editor:SetMovable(true)
editor:EnableMouse(true)
editor:RegisterForDrag("LeftButton")
editor:SetScript("OnDragStart", editor.StartMoving)
editor:SetScript("OnDragStop", editor.StopMovingOrSizing)
editor:SetFrameStrata("DIALOG")
editor:Hide()

AddSolidBacking(editor)

tinsert(UISpecialFrames, "SanctuaryProfileEditor")

local editorTitle = editor:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
editorTitle:SetPoint("TOP", editor, "TOP", 0, -16)
editorTitle:SetText("What others see")

local editorHelp = editor:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
editorHelp:SetPoint("TOPLEFT", editor, "TOPLEFT", 18, -42)
editorHelp:SetWidth(390)
editorHelp:SetJustifyH("LEFT")
editorHelp:SetText("A glance, not a history. Leave anything blank that does not need saying.")

local editorClose = CreateFrame("Button", nil, editor, "UIPanelCloseButton")
editorClose:SetPoint("TOPRIGHT", editor, "TOPRIGHT", -4, -4)

local editBoxes = {}

for index, slot in ipairs(FIELD_ORDER) do
    local field = FIELDS[slot]

    local heading = editor:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    heading:SetText(field.label)

    if index == 1 then
        heading:SetPoint("TOPLEFT", editorHelp, "BOTTOMLEFT", 0, -14)
    else
        heading:SetPoint("TOPLEFT", editBoxes[index - 1].hint, "BOTTOMLEFT", -6, -14)
    end

    local counter = editor:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    counter:SetPoint("TOPRIGHT", editor, "TOPRIGHT", -20, 0)
    counter:SetPoint("TOP", heading, "TOP", 0, 0)

    --[[
        Drawn rather than templated.

        InputBoxTemplate builds its background from three pieces - a left cap, a right cap
        and a middle stretched between them - and the middle anchors to the caps by the
        names "$parentLeft" and "$parentRight". Those only resolve when the EditBox itself
        has a name, and these were created with nil. The caps still draw, because they
        anchor to the box; the middle collapses to nothing. That is the field rendering as
        two brackets with a gap where the bar should be.

        Naming the boxes would fix it, but a backdrop of our own is one fewer thing to be at
        the mercy of, and it matches the rest of the panel.
    ]]
    local box = CreateFrame("EditBox", nil, editor)
    box:SetWidth(376)
    box:SetHeight(24)
    box:SetPoint("TOPLEFT", heading, "BOTTOMLEFT", 6, -4)
    box:SetAutoFocus(false)
    box:SetMaxLetters(MAX_LENGTH)
    box:SetFontObject("ChatFontNormal")
    box:SetTextColor(0.91, 0.86, 0.78)
    -- Insets keep the caret and the text off the border at both ends.
    box:SetTextInsets(7, 7, 0, 0)

    box:SetBackdrop({
        bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 16,
        edgeSize = 12,
        insets = { left = 3, right = 3, top = 3, bottom = 3 },
    })
    box:SetBackdropColor(0, 0, 0, 0.75)
    box:SetBackdropBorderColor(0.45, 0.38, 0.27, 1)

    -- A visible focus state, since without the template there is no highlight of its own.
    box:SetScript("OnEditFocusGained", function(self)
        self:SetBackdropBorderColor(0.85, 0.71, 0.42, 1)
    end)

    box:SetScript("OnEditFocusLost", function(self)
        self:SetBackdropBorderColor(0.45, 0.38, 0.27, 1)
    end)

    local hint = editor:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    hint:SetPoint("TOPLEFT", box, "BOTTOMLEFT", 0, -2)
    hint:SetText(field.hint)

    box:SetScript("OnTextChanged", function(self)
        counter:SetText(string.format("%d/%d", string.len(self:GetText()), MAX_LENGTH))
    end)

    box:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    -- Tab moves to the next line, which is what anyone filling in a form expects.
    box:SetScript("OnTabPressed", function(self)
        local next = editBoxes[index + 1] or editBoxes[1]
        next.box:SetFocus()
    end)

    editBoxes[index] = { box = box, hint = hint, counter = counter, slot = slot }
end

local function SaveEditor()
    for _, entry in ipairs(editBoxes) do
        -- Sent one line at a time. The server echoes each back, so the boxes end up showing
        -- exactly what was stored rather than what was typed.
        Send(string.format("S:%d:%s", entry.slot, entry.box:GetText()))
    end

    DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r your description has been saved.")
end

local save = CreateFrame("Button", nil, editor, "UIPanelButtonTemplate")
save:SetWidth(100)
save:SetHeight(24)
save:SetPoint("BOTTOMRIGHT", editor, "BOTTOMRIGHT", -18, 18)
save:SetText("Save")
save:SetScript("OnClick", function()
    SaveEditor()
    editor:Hide()
end)

local preview = CreateFrame("Button", nil, editor, "UIPanelButtonTemplate")
preview:SetWidth(100)
preview:SetHeight(24)
preview:SetPoint("RIGHT", save, "LEFT", -8, 0)
preview:SetText("Preview")
preview:SetScript("OnClick", function()
    SaveEditor()
    awaitingEditor = false
    Send("V:" .. (playerKey or ""))
end)

local clear = CreateFrame("Button", nil, editor, "UIPanelButtonTemplate")
clear:SetWidth(100)
clear:SetHeight(24)
clear:SetPoint("BOTTOMLEFT", editor, "BOTTOMLEFT", 18, 18)
clear:SetText("Clear all")
clear:SetScript("OnClick", function()
    for _, entry in ipairs(editBoxes) do
        entry.box:SetText("")
    end

    Send("C:0")
end)

local function OpenEditor()
    -- Always refetched rather than trusting what the boxes hold: the character may have
    -- changed, or this may be the first open since a reload.
    awaitingEditor = true
    Send("R:0")
    editor:Show()
end

local function ToggleEditor()
    if editor:IsShown() then
        editor:Hide()
    else
        OpenEditor()
    end
end

--------------------------------------------------------------------------
-- The button
--------------------------------------------------------------------------

--[[
    A button on the minimap, so writing your description is something you can find rather
    than something you have to be told a slash command for.

    Positioned by an angle around the minimap rather than a fixed offset: that is what keeps
    it on the ring while being dragged, and lets the position survive as one saved number.
    3.3.5a's cos and sin take degrees, not radians.
]]

local MINIMAP_RADIUS = 80
-- Measured off a live minimap, not guessed. The four Sanctuary buttons share one arc:
-- outlaw -113.04, stash -132.70, disguise -151.64, profile -171.25.
local DEFAULT_ANGLE = -171.25

local minimapButton = CreateFrame("Button", "SanctuaryProfileMinimapButton", Minimap)
minimapButton:SetWidth(31)
minimapButton:SetHeight(31)
minimapButton:SetFrameStrata("MEDIUM")
minimapButton:SetFrameLevel(8)
minimapButton:RegisterForClicks("LeftButtonUp", "RightButtonUp")
minimapButton:RegisterForDrag("LeftButton")

local minimapIcon = minimapButton:CreateTexture(nil, "BACKGROUND")
minimapIcon:SetWidth(20)
minimapIcon:SetHeight(20)
minimapIcon:SetTexture("Interface\\Icons\\INV_Misc_Note_01")
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

local function RestoreMinimapButton()
    local angle = (SanctuaryProfileDB and SanctuaryProfileDB.minimapAngle) or DEFAULT_ANGLE
    PlaceMinimapButton(angle)
end

minimapButton:SetScript("OnDragStart", function(self)
    self.dragging = true
end)

minimapButton:SetScript("OnDragStop", function(self)
    self.dragging = false

    if SanctuaryProfileDB then
        SanctuaryProfileDB.minimapAngle = self.angle or DEFAULT_ANGLE
    end
end)

minimapButton:SetScript("OnUpdate", function(self)
    if not self.dragging then
        return
    end

    local mx, my = Minimap:GetCenter()
    local cx, cy = GetCursorPosition()
    local scale = Minimap:GetEffectiveScale()

    -- Converted to degrees, because PlaceMinimapButton works in them.
    self.angle = math.deg(math.atan2((cy / scale) - my, (cx / scale) - mx))
    PlaceMinimapButton(self.angle)
end)

minimapButton:SetScript("OnClick", function(self, button)
    if button == "RightButton" then
        SanctuaryProfileDB = SanctuaryProfileDB or {}
        SanctuaryProfileDB.autoOpen = (SanctuaryProfileDB.autoOpen == false)

        DEFAULT_CHAT_FRAME:AddMessage(SanctuaryProfileDB.autoOpen
            and "|cffd8b46aSanctuary:|r descriptions will open when you target someone."
            or "|cffd8b46aSanctuary:|r descriptions will only open when you type /look.")
        return
    end

    ToggleEditor()
end)

minimapButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:SetText("Sanctuary Profile")
    GameTooltip:AddLine("Click to write what others see when they look at you.", 1, 1, 1, true)
    GameTooltip:AddLine("Right-click to stop or start descriptions opening on target.", 1, 1, 1, true)
    GameTooltip:Show()
end)

minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

PlaceMinimapButton(DEFAULT_ANGLE)

--------------------------------------------------------------------------
-- Looking at somebody
--------------------------------------------------------------------------

--- `automatic` only decides whether a miss is worth a chat line: targeting a creature
--- should say nothing, whereas typing /look at one should explain itself.
local function Look(unit, automatic)
    if not unit or not UnitExists(unit) or not UnitIsPlayer(unit) then
        if not automatic then
            DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r target a player first.")
        end
        return
    end

    local key = GuidKey(unit)
    if not key then
        return
    end

    awaitingEditor = false
    Send("V:" .. key)
end

--------------------------------------------------------------------------
-- Opening on target
--------------------------------------------------------------------------

--[[
    The window follows your target.

    Asking is delayed very slightly, so tab-targeting through a crowd sends one request for
    whoever you land on rather than one per character passed over. That matters because the
    server rate-limits looks to ten a second and silently drops the excess, so an
    unthrottled version would fail exactly when a room is busy. The delay is kept just under
    that ceiling rather than comfortably below it: the window should feel like it belongs to
    the target, not like it is catching up.

    Every target opens a window, including one with nothing written in it. Hiding the
    empty case looks identical to the feature not working, which is exactly how it was
    first read.
]]

local TARGET_DELAY = 0.15

local pendingUnit = nil
local pendingAt = 0

-- Parented, sized and anchored on purpose. OnUpdate only runs on a frame the client
-- considers visible, and a parentless frame with no size and no anchor is the one shape
-- where that is genuinely in doubt - and it would fail silently.
local watcher = CreateFrame("Frame", nil, UIParent)
watcher:SetWidth(1)
watcher:SetHeight(1)
watcher:SetPoint("TOPLEFT", UIParent, "TOPLEFT", 0, 0)
watcher:Show()

watcher:SetScript("OnUpdate", function(self, elapsed)
    if not pendingUnit then
        return
    end

    if (GetTime() - pendingAt) < TARGET_DELAY then
        return
    end

    local unit = pendingUnit
    pendingUnit = nil

    -- Re-checked after the delay: the target may have been dropped or swapped since.
    if UnitExists(unit) and UnitIsPlayer(unit) and not UnitIsUnit(unit, "player") then
        Look(unit, true)
    end
end)

--[[
    Whether the window follows your target.

    Written as "on unless explicitly switched off" rather than "on if the saved setting says
    so", and the difference is the whole reason this did nothing at first. SanctuaryProfileDB
    does not exist until ADDON_LOADED has run and written it, and any character whose saved
    variables have never been written starts with it missing. Testing the value for truth
    treats both of those as "switched off", so a feature that is meant to default to on
    silently never fires - with no error, and nothing to see.
]]
local function AutoOpenEnabled()
    return not SanctuaryProfileDB or SanctuaryProfileDB.autoOpen ~= false
end

local function OnTargetChanged()
    pendingUnit = nil

    if not AutoOpenEnabled() then
        return
    end

    if not UnitExists("target") or not UnitIsPlayer("target") or UnitIsUnit("target", "player") then
        -- Dropping a target closes the window, so it never describes somebody you have
        -- stopped looking at.
        viewer:Hide()
        return
    end

    viewer:Hide()
    pendingUnit = "target"
    pendingAt = GetTime()
end

--------------------------------------------------------------------------
-- The button on the unit frame
--------------------------------------------------------------------------

--[[
    A small button on the target and focus frames that opens the description of whoever is
    in them.

    Anchored to the portrait rather than to the frame itself. The portrait is a fixed,
    named region, so its outer edge is a reliable place to sit; offsets measured from the
    frame get covered by the level text, the elite dragon border, or the buffs that grow
    underneath, depending on who is targeted.

    Only shown for other players, since nobody else has a description to read.
]]

local lookButtons = {}

local function AttachLookButton(frameName, portraitName, unit)
    local frame = _G[frameName]
    if not frame then
        return
    end

    local button = CreateFrame("Button", nil, frame)
    button:SetWidth(20)
    button:SetHeight(20)

    -- Above the frame art, or it renders behind the portrait border.
    button:SetFrameLevel(frame:GetFrameLevel() + 4)

    local portrait = _G[portraitName]
    if portrait then
        button:SetPoint("BOTTOMLEFT", portrait, "BOTTOMRIGHT", 1, -1)
    else
        button:SetPoint("TOPLEFT", frame, "TOPRIGHT", -18, -30)
    end

    button:SetNormalTexture("Interface\\Icons\\INV_Misc_Note_01")
    button:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")

    -- Icons ship with a border baked in; cropping it leaves just the artwork.
    local normal = button:GetNormalTexture()
    if normal then
        normal:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    end

    local ring = button:CreateTexture(nil, "OVERLAY")
    ring:SetPoint("TOPLEFT", button, "TOPLEFT", -2, 2)
    ring:SetPoint("BOTTOMRIGHT", button, "BOTTOMRIGHT", 2, -2)
    ring:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    ring:SetTexCoord(0.08, 0.58, 0.08, 0.58)

    button:SetScript("OnClick", function()
        Look(unit, false)
    end)

    button:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText("Look closer")
        GameTooltip:AddLine("What you can see of them.", 1, 1, 1, true)
        GameTooltip:Show()
    end)

    button:SetScript("OnLeave", function() GameTooltip:Hide() end)

    button:Hide()

    table.insert(lookButtons, { button = button, unit = unit })
end

AttachLookButton("TargetFrame", "TargetFramePortrait", "target")
AttachLookButton("FocusFrame", "FocusFramePortrait", "focus")

local function UpdateLookButtons()
    for _, entry in ipairs(lookButtons) do
        local unit = entry.unit

        if UnitExists(unit) and UnitIsPlayer(unit) and not UnitIsUnit(unit, "player") then
            entry.button:Show()
        else
            entry.button:Hide()
        end
    end
end

--------------------------------------------------------------------------
-- The unit frame tooltip
--------------------------------------------------------------------------

--[[
    Hovering a player's unit frame pops the default tooltip - their name, level, guild.
    On a realm where you are supposed to learn who somebody is by talking to them, and
    where this addon already answers "what can I see about them", that tooltip is the
    wrong answer arriving first.

    Hooked rather than replaced: UnitFrame_OnEnter does more than show a tooltip, and
    clearing the script outright would take the rest with it. HookScript runs after the
    original, so the tooltip is hidden in the same frame it was shown and never paints.

    Only for other players. Creatures keep their tooltips, and so does your own frame,
    where nothing is being concealed from you.
]]
local function SuppressUnitTooltip(frame)
    if not frame then
        return
    end

    frame:HookScript("OnEnter", function(self)
        local unit = self.unit
        if unit and UnitIsPlayer(unit) and not UnitIsUnit(unit, "player") then
            GameTooltip:Hide()
        end
    end)
end

for _, name in ipairs({
    "TargetFrame",
    "TargetFrameToT",
    "FocusFrame",
    "FocusFrameToT",
    "PartyMemberFrame1",
    "PartyMemberFrame2",
    "PartyMemberFrame3",
    "PartyMemberFrame4",
}) do
    SuppressUnitTooltip(_G[name])
end

--------------------------------------------------------------------------
-- Incoming
--------------------------------------------------------------------------

local function Handle(body)
    local kind = string.sub(body, 1, 1)

    if kind == "M" then
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r " .. string.sub(body, 3))
        return
    end

    if kind == "H" then
        local key, label = string.match(body, "^H:([^:]+):(.*)$")
        if key then
            incoming[key] = { label = label, fields = {} }
        end
        return
    end

    if kind == "F" then
        -- The text may itself contain colons, so only the first three segments are fixed.
        local key, slot, text = string.match(body, "^F:([^:]+):(%d):(.*)$")
        if not key then
            return
        end

        slot = tonumber(slot)

        -- An echo of our own edit, which arrives on its own with no heading before it.
        if key == playerKey and editor:IsShown() and not incoming[key] then
            for _, entry in ipairs(editBoxes) do
                if entry.slot == slot then
                    entry.box:SetText(text)
                end
            end
            return
        end

        if incoming[key] then
            incoming[key].fields[slot] = text
        end
        return
    end

    if kind == "E" then
        local key = string.match(body, "^E:([^:]+):[01]$")
        if not key or not incoming[key] then
            return
        end

        local profile = incoming[key]
        incoming[key] = nil

        -- Opening the editor asks for our own profile too. Only that request fills the
        -- boxes; Preview asks for the same guid and must still be shown.
        if awaitingEditor and key == playerKey then
            awaitingEditor = false

            for _, entry in ipairs(editBoxes) do
                entry.box:SetText(profile.fields[entry.slot] or "")
            end
            return
        end

        -- Always shown, even when there is nothing written. Suppressing the empty case
        -- reads as the feature being broken rather than as the character being
        -- undescribed, and there is no way to tell the two apart from the outside.

        RenderViewer(profile)
        return
    end
end

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")
listener:RegisterEvent("PLAYER_TARGET_CHANGED")
listener:RegisterEvent("PLAYER_FOCUS_CHANGED")
listener:RegisterEvent("ADDON_LOADED")

listener:SetScript("OnEvent", function(self, event, arg1, message)
    if event == "CHAT_MSG_ADDON" then
        if arg1 == ADDON_PREFIX then
            Handle(message)
        end
        return
    end

    if event == "ADDON_LOADED" then
        if arg1 == "SanctuaryProfile" then
            SanctuaryProfileDB = SanctuaryProfileDB or {}

            if SanctuaryProfileDB.autoOpen == nil then
                SanctuaryProfileDB.autoOpen = true
            end

            RestoreMinimapButton()
        end
        return
    end

    if event == "PLAYER_TARGET_CHANGED" then
        OnTargetChanged()
        UpdateLookButtons()
        return
    end

    if event == "PLAYER_FOCUS_CHANGED" then
        UpdateLookButtons()
        return
    end

    playerName = UnitName("player")
    playerKey = GuidKey("player")
    incoming = {}
    awaitingEditor = false
    pendingUnit = nil
end)

--------------------------------------------------------------------------
-- Slash commands
--------------------------------------------------------------------------

SLASH_SANCTUARYLOOK1 = "/look"
SLASH_SANCTUARYLOOK2 = "/examine"

SlashCmdList["SANCTUARYLOOK"] = function(input)
    input = string.lower(string.gsub(input or "", "^%s*(.-)%s*$", "%1"))

    if input == "auto" then
        -- Guarded: a saved-variables table is only there once ADDON_LOADED has run.
        SanctuaryProfileDB = SanctuaryProfileDB or {}
        -- Stored as an explicit boolean: AutoOpenEnabled treats only a literal false as
        -- off, so writing nil here would read as on again.
        SanctuaryProfileDB.autoOpen = (SanctuaryProfileDB.autoOpen == false)

        DEFAULT_CHAT_FRAME:AddMessage(SanctuaryProfileDB.autoOpen
            and "|cffd8b46aSanctuary:|r descriptions will open when you target someone."
            or "|cffd8b46aSanctuary:|r descriptions will only open when you type /look.")
        return
    end

    Look("target", false)
end

SLASH_SANCTUARYPROFILE1 = "/profile"
SLASH_SANCTUARYPROFILE2 = "/describe"

SlashCmdList["SANCTUARYPROFILE"] = function(input)
    input = string.gsub(input or "", "^%s*(.-)%s*$", "%1")

    if input == "help" then
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary Profile|r")
        DEFAULT_CHAT_FRAME:AddMessage("  /profile - write what others see when they look at you")
        DEFAULT_CHAT_FRAME:AddMessage("  /look - look closer at your target")
        DEFAULT_CHAT_FRAME:AddMessage("  /look auto - stop or start the window opening on target")
        DEFAULT_CHAT_FRAME:AddMessage("Targeting a character opens their description on its own.")
        return
    end

    ToggleEditor()
end
