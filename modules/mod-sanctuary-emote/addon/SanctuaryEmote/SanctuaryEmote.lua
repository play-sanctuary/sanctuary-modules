--[[
    Sanctuary Emote - /do

    /me says what your character does. /do says what the world is doing.

        /do The fire crackles warm with old tinder.
        /do Rain finds every gap in the roof, and the floor is going dark with it.

    Everyone nearby reads it and it carries no name, which is what separates it from an
    emote. Setting a scene should not read as one person performing.

    It goes through the server because it has to: SendChatMessage welds your own name to the
    front of anything sent on SAY or EMOTE, so an unattributed line to the people around you
    is not something a client can produce on its own.

    Protocol, on the SEMO prefix, carried as a whisper to yourself:
        out  D:<text>
]]

local ADDON_PREFIX = "SEMO"

local playerName

local function Send(body)
    SendAddonMessage(ADDON_PREFIX, body, "WHISPER", playerName or UnitName("player"))
end

local listener = CreateFrame("Frame")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")

listener:SetScript("OnEvent", function()
    playerName = UnitName("player")
end)

SLASH_SANCTUARYDO1 = "/do"
SLASH_SANCTUARYDO2 = "/env"

SlashCmdList["SANCTUARYDO"] = function(input)
    input = string.gsub(input or "", "^%s*(.-)%s*$", "%1")

    if input == "" then
        DEFAULT_CHAT_FRAME:AddMessage("|cffd8b46aSanctuary:|r describe the scene, not yourself.")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffb9a2d1/do The fire crackles warm with old tinder.|r")
        return
    end

    -- Sent raw. The server strips anything that could forge a link or recolour the line,
    -- and it is the only side that can be trusted to, since the addon is not the only thing
    -- that can send on this prefix.
    Send("D:" .. input)
end
