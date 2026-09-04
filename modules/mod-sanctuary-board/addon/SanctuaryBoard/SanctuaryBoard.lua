--[[
    Sanctuary Board — two corrections to the client's own notice-writing dialog.

    The board itself is server-side gossip and works with no addon at all. This only fixes
    the popup the client puts up when you choose a category, which is stock FrameXML and
    wrong in two ways.

    Both are in Interface\FrameXML\StaticPopup.lua, StaticPopupDialogs["GOSSIP_ENTER_CODE"].
]]

local dialog = StaticPopupDialogs and StaticPopupDialogs["GOSSIP_ENTER_CODE"]

if not dialog then
    -- A patch moved it. Better to do nothing than to error on every login.
    return
end

--[[
    1. The prompt.

    The dialog's text is the global ENTER_CODE, "Please enter code:", which is aimed at
    guild-bank passwords rather than at writing a notice. The BoxMessage the server sends
    alongside each gossip option is never shown by this dialog, so there is no server-side
    wording to change — this is the only place it can be done.

    This is realm-wide rather than board-specific: the dialog is shared by every coded
    gossip option. The notice board is the only thing on Sanctuary that uses one.
]]
dialog.text = "Write your message"

--[[
    2. Enter should post.

    Enter was already wired up, and already broken. Compare the two handlers as they ship:

        OnAccept              = SelectGossipOption(data, self.editBox:GetText(), true)
        EditBoxOnEnterPressed = SelectGossipOption(data, parent.editBox:GetText())

    The third argument is the "coded" flag. Clicking Accept sets it and the server receives
    CMSG_GOSSIP_SELECT_OPTION carrying the typed text, which is what reaches
    OnGossipSelectCode. Pressing Enter omits it, so the server gets an ordinary gossip
    select with no text at all and the notice is silently dropped — the message looks like
    it was sent and simply never appears.

    Restoring the flag is the whole fix.
]]
dialog.EditBoxOnEnterPressed = function(self, data)
    local parent = self:GetParent()
    SelectGossipOption(data, parent.editBox:GetText(), true)
    parent:Hide()
end
