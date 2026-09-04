/*
 * mod-sanctuary-emote
 *
 * /do - an emote for the room rather than for a person.
 *
 *     /do The fire crackles warm with old tinder.
 *
 * Everyone nearby reads it, and it carries no name. That is the whole point: /me says what
 * your character does, /do says what the world is doing, and an attributed line would make
 * the second read like the first.
 *
 * It has to be the server that says it. A client can only produce a line with its own name
 * welded to the front - SendChatMessage on SAY or EMOTE both prefix the speaker - so an
 * unattributed message to the people around you is not something an addon can do alone.
 *
 * The author is recorded in the server log and, optionally, shown to game masters. On a
 * realm where nobody knows who anybody is, an anonymous broadcast to everyone in earshot
 * needs to be something a game master can trace.
 */

#include "Cell.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <list>
#include <string>
#include <unordered_map>

namespace
{
    // Not an "s_" prefix: winsock2.h defines s_host and friends as macros on in_addr, and a
    // file-scope name colliding with one expands into nonsense before the compiler sees it.
    bool g_enabled = true;
    std::string g_prefix = "SEMO";
    float g_range = 40.0f;
    uint32 g_maxLength = 255;
    uint32 g_cooldownSeconds = 3;
    std::string g_colour = "b9a2d1";
    bool g_showAuthorToGameMasters = true;

    /// guid -> when they last managed a /do, for the cooldown.
    std::unordered_map<ObjectGuid::LowType, uint32> g_lastUsed;

    uint32 Now()
    {
        return uint32(GameTime::GetGameTime().count());
    }

    /*
     * Strips what an unattributed, room-wide message must never be able to carry.
     *
     * This lands in the chat frame of everyone nearby with no name attached, which makes it
     * the most attractive thing on the realm to abuse. A pipe is the client's escape
     * character: left in, a /do could forge an item link, recolour itself to look like a
     * system message, or impersonate another player's say line.
     */
    std::string Sanitise(std::string const& raw)
    {
        std::string out;
        out.reserve(raw.size());

        for (char c : raw)
        {
            if (c == '|')
                continue;

            if (static_cast<unsigned char>(c) < 0x20)
                continue;

            out += c;
        }

        std::size_t const first = out.find_first_not_of(' ');
        if (first == std::string::npos)
            return {};

        std::size_t const last = out.find_last_not_of(' ');
        out = out.substr(first, last - first + 1);

        if (out.size() > g_maxLength)
            out = out.substr(0, g_maxLength);

        return out;
    }

    void Broadcast(Player* author, std::string const& text)
    {
        // Built once for players and once for game masters, rather than per listener: the
        // only difference is the trailing attribution.
        std::string const line = "|cff" + g_colour + text + "|r";
        std::string const traced = line + " |cff6f6f6f(" + author->GetName() + ")|r";

        std::list<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(author, g_range, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(author, nearby, check);
        Cell::VisitObjects(author, searcher, g_range);

        for (Player* listener : nearby)
        {
            if (!listener || !listener->GetSession())
                continue;

            bool const trace = g_showAuthorToGameMasters && listener->IsGameMaster() && listener != author;

            ChatHandler(listener->GetSession()).SendSysMessage((trace ? traced : line).c_str());
        }

        // Always logged, whether or not a game master happened to be standing there.
        LOG_INFO("module.sanctuaryemote", "[/do] {} ({}): {}",
            author->GetName(), author->GetGUID().ToString(), text);
    }

    void HandleDo(Player* player, std::string const& raw)
    {
        std::string const text = Sanitise(raw);

        if (text.empty())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Describe something: /do The fire gutters low.");
            return;
        }

        if (g_cooldownSeconds > 0)
        {
            ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
            auto itr = g_lastUsed.find(guid);

            if (itr != g_lastUsed.end() && Now() - itr->second < g_cooldownSeconds && !player->IsGameMaster())
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "Let the last one settle first.");
                return;
            }

            g_lastUsed[guid] = Now();
        }

        Broadcast(player, text);
    }
}

class sanctuary_emote_playerscript : public PlayerScript
{
public:
    sanctuary_emote_playerscript() : PlayerScript("sanctuary_emote_playerscript") { }

    void OnPlayerLogout(Player* player) override
    {
        g_lastUsed.erase(player->GetGUID().GetCounter());
    }

    //[[ Addon traffic arrives as a whisper the player sends to themselves. ]]
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !g_enabled)
            return true;

        std::string const marker = g_prefix + "\t";
        if (msg.rfind(marker, 0) != 0)
            return true;

        std::string body = msg.substr(marker.size());

        if (body.size() >= 2 && body[0] == 'D' && body[1] == ':')
            HandleDo(player, body.substr(2));

        // Never let our own traffic surface in the chat window.
        return false;
    }
};

class sanctuary_emote_worldscript : public WorldScript
{
public:
    sanctuary_emote_worldscript() : WorldScript("sanctuary_emote_worldscript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryEmote.Enable", true);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryEmote.Addon.Prefix", "SEMO");
        g_range = sConfigMgr->GetOption<float>("SanctuaryEmote.Range", 40.0f);
        g_maxLength = std::clamp(sConfigMgr->GetOption<uint32>("SanctuaryEmote.MaxLength", 255u), 32u, 255u);
        g_cooldownSeconds = sConfigMgr->GetOption<uint32>("SanctuaryEmote.CooldownSeconds", 3);
        g_colour = sConfigMgr->GetOption<std::string>("SanctuaryEmote.Colour", "b9a2d1");
        g_showAuthorToGameMasters = sConfigMgr->GetOption<bool>("SanctuaryEmote.ShowAuthorToGameMasters", true);

        LOG_INFO("module.sanctuaryemote", "Sanctuary /do {}: carries {} yards.",
            g_enabled ? "enabled" : "disabled", g_range);
    }
};

void AddSC_sanctuary_emote_scripts()
{
    new sanctuary_emote_playerscript();
    new sanctuary_emote_worldscript();
}
