/*
 * mod-sanctuary-profile
 *
 * A glance at somebody. Four short lines - what they look like, what is wrong with them,
 * how they carry themselves, and one detail worth noticing - shown when you look closer at
 * a character stood in front of you.
 *
 * Deliberately small. The fields are capped short enough that nobody can write a biography
 * into them, because the point of the realm is that the rest is found out in character.
 *
 * Served by the world server rather than exchanged between clients, for two reasons. The
 * heading has to go through SanctuaryIdentity::LabelFor or a profile window would print a
 * stranger's real name straight past the disguise; and a client-to-client protocol would let
 * anyone harvest every profile on the realm, which on an anonymity realm is exactly the
 * thing not to build. You are answered only about somebody you are stood next to.
 */

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "SanctuaryIdentity.h"
#include "StringFormat.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace
{
    // Not an "s_" prefix: winsock2.h defines s_host and friends as macros on in_addr, and a
    // file-scope name colliding with one expands into nonsense before the compiler sees it.
    bool g_enabled = true;
    std::string g_prefix = "SPRO";
    uint32 g_maxQueriesPerSecond = 10;
    float g_lookRange = 40.0f;
    uint32 g_maxFieldLength = 160;

    /// The four things a profile holds. Order is the wire format; do not renumber.
    constexpr std::size_t FieldCount = 4;

    /// Column order here is the wire format and the table's column order; do not renumber.
    struct Profile
    {
        std::array<std::string, FieldCount> Fields;
    };

    /// Loaded at login and written through on edit, so a look costs no query.
    std::unordered_map<ObjectGuid::LowType, Profile> g_profiles;

    struct QueryBudget
    {
        time_t Window = 0;
        uint32 Used = 0;
    };

    std::unordered_map<ObjectGuid::LowType, QueryBudget> g_budgets;

    void Send(Player* to, std::string const& body)
    {
        if (!to || !to->GetSession())
            return;

        std::string payload = g_prefix + "\t" + body;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, to, to, payload);
        to->GetSession()->SendPacket(&data);
    }

    void Notify(Player* to, std::string const& text)
    {
        Send(to, "M:" + text);
    }

    /*
     * A guid in the form the client itself produces.
     *
     * UnitGUID returns "0x" followed by sixteen uppercase hex digits, and the addon strips
     * the prefix before sending it. Anything the server generates has to match that exactly
     * or the addon cannot pair a reply with the window it opened.
     */
    std::string GuidKey(ObjectGuid guid)
    {
        return Acore::StringFormat("{:016X}", guid.GetRawValue());
    }

    /*
     * Strips what a profile must never be able to smuggle into another player's chat frame.
     *
     * A pipe is the client's escape character: left in, a profile could colour text, forge
     * an item link, or open a hyperlink in somebody else's window. Control characters would
     * break the addon message apart, since the wire format is line and colon delimited.
     */
    std::string Sanitise(std::string const& raw)
    {
        std::string out;
        out.reserve(raw.size());

        for (char c : raw)
        {
            if (c == '|')
                continue;

            // Everything below space, tab and newline included: the addon channel carries
            // one record per message and a newline would split it.
            if (static_cast<unsigned char>(c) < 0x20)
                continue;

            out += c;
        }

        std::size_t const first = out.find_first_not_of(' ');
        if (first == std::string::npos)
            return {};

        std::size_t const last = out.find_last_not_of(' ');
        out = out.substr(first, last - first + 1);

        if (out.size() > g_maxFieldLength)
        {
            // Cut on a space where there is one nearby, so a truncated line does not end
            // mid-word.
            std::size_t cut = g_maxFieldLength;
            std::size_t const space = out.rfind(' ', cut);

            if (space != std::string::npos && space > g_maxFieldLength - 20)
                cut = space;

            out = out.substr(0, cut);
        }

        return out;
    }

    bool WithinBudget(Player* player)
    {
        QueryBudget& budget = g_budgets[player->GetGUID().GetCounter()];
        time_t const now = time(nullptr);

        if (budget.Window != now)
        {
            budget.Window = now;
            budget.Used = 0;
        }

        return ++budget.Used <= g_maxQueriesPerSecond;
    }

    void Load(Player* player)
    {
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

        Profile& profile = g_profiles[guid];
        profile = Profile();

        QueryResult result = CharacterDatabase.Query(
            "SELECT `appearance`, `injuries`, `manner`, `detail` FROM `character_profiles` WHERE `guid` = {}",
            guid);

        if (!result)
            return;

        Field* fields = result->Fetch();

        for (std::size_t i = 0; i < FieldCount; ++i)
            profile.Fields[i] = fields[uint32(i)].Get<std::string>();
    }

    void Persist(ObjectGuid::LowType guid, Profile const& profile)
    {
        std::array<std::string, FieldCount> escaped = profile.Fields;

        // EscapeString mutates in place and returns void, so it cannot be called inline.
        for (std::string& field : escaped)
            CharacterDatabase.EscapeString(field);

        CharacterDatabase.Execute(
            "REPLACE INTO `character_profiles` (`guid`, `appearance`, `injuries`, `manner`, `detail`, `updated_at`) "
            "VALUES ({}, '{}', '{}', '{}', '{}', {})",
            guid, escaped[0], escaped[1], escaped[2], escaped[3],
            uint32(GameTime::GetGameTime().count()));
    }

    /// Whether the looker is close enough, and able, to be looking at all.
    bool CanSee(Player* looker, Player* subject)
    {
        if (!looker || !subject || !subject->IsInWorld())
            return false;

        if (looker == subject)
            return true;

        if (!looker->IsWithinDistInMap(subject, g_lookRange))
            return false;

        // Phasing matters here as much as distance: two characters in different phases are
        // standing in the same place and cannot see each other at all.
        return looker->CanSeeOrDetect(subject);
    }

    /*
     * Sends one profile to one viewer.
     *
     * One addon message per field: a 3.3.5a addon message caps at 255 bytes, so a batched
     * profile would be truncated with no way for the addon to tell.
     */
    void SendProfile(Player* viewer, Player* subject)
    {
        std::string const key = GuidKey(subject->GetGUID());

        // The heading is the one place a real name could escape, so it goes through the
        // identity module rather than GetName(). A stranger stays "Hooded Orc" here.
        Send(viewer, "H:" + key + ":" + SanctuaryIdentity::LabelFor(viewer, subject));

        auto itr = g_profiles.find(subject->GetGUID().GetCounter());
        bool any = false;

        if (itr != g_profiles.end())
        {
            for (std::size_t i = 0; i < FieldCount; ++i)
            {
                std::string const& field = itr->second.Fields[i];
                if (field.empty())
                    continue;

                Send(viewer, "F:" + key + ":" + std::to_string(i) + ":" + field);
                any = true;
            }
        }

        Send(viewer, "E:" + key + ":" + (any ? "1" : "0"));
    }

    void HandleView(Player* viewer, std::string const& rawGuid)
    {
        uint64 value = 0;

        try
        {
            value = std::stoull(rawGuid, nullptr, 16);
        }
        catch (std::exception const&)
        {
            return; // Not a guid. Nothing sane to do with it.
        }

        ObjectGuid const guid(value);
        if (!guid.IsPlayer())
            return;

        Player* subject = ObjectAccessor::FindConnectedPlayer(guid);

        if (!CanSee(viewer, subject))
        {
            // Deliberately the same answer whether they are out of range, phased away or
            // not on the realm at all: distinguishing them would make this a way to check
            // whether a given character is online and where.
            Notify(viewer, "There is nobody like that in front of you.");
            return;
        }

        SendProfile(viewer, subject);
    }

    void HandleSet(Player* player, std::string const& argument)
    {
        // "<slot>:<text>", where text may itself contain colons.
        if (argument.size() < 2 || argument[1] != ':')
            return;

        std::size_t const slot = std::size_t(argument[0] - '0');
        if (slot >= FieldCount)
            return;

        Profile& profile = g_profiles[player->GetGUID().GetCounter()];
        profile.Fields[slot] = Sanitise(argument.substr(2));

        Persist(player->GetGUID().GetCounter(), profile);

        // Echoed back so the editor shows exactly what was stored, including any trimming.
        Send(player, "F:" + GuidKey(player->GetGUID()) + ":" + std::to_string(slot) + ":" + profile.Fields[slot]);
    }

    void HandleClear(Player* player)
    {
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

        g_profiles[guid] = Profile();
        CharacterDatabase.Execute("DELETE FROM `character_profiles` WHERE `guid` = {}", guid);

        Notify(player, "Your description has been cleared.");
        SendProfile(player, player);
    }
}

class sanctuary_profile_playerscript : public PlayerScript
{
public:
    sanctuary_profile_playerscript() : PlayerScript("sanctuary_profile_playerscript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (g_enabled)
            Load(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        g_profiles.erase(player->GetGUID().GetCounter());
        g_budgets.erase(player->GetGUID().GetCounter());
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
        if (body.size() < 2 || body[1] != ':')
            return false; // Ours, but malformed. Swallow it rather than let it show as a whisper.

        char const kind = body[0];
        std::string const argument = body.substr(2);

        if (kind == 'V')
        {
            if (WithinBudget(player))
                HandleView(player, argument);
        }
        else if (kind == 'S')
        {
            HandleSet(player, argument);
        }
        else if (kind == 'C')
        {
            HandleClear(player);
        }
        else if (kind == 'R')
        {
            // The editor asking for its own copy, on open or after a reload.
            SendProfile(player, player);
        }

        // Never let our own traffic surface in the chat window.
        return false;
    }
};

class sanctuary_profile_worldscript : public WorldScript
{
public:
    sanctuary_profile_worldscript() : WorldScript("sanctuary_profile_worldscript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryProfile.Enable", true);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryProfile.Addon.Prefix", "SPRO");
        g_maxQueriesPerSecond = sConfigMgr->GetOption<uint32>("SanctuaryProfile.MaxQueriesPerSecond", 10);
        g_lookRange = sConfigMgr->GetOption<float>("SanctuaryProfile.LookRange", 40.0f);
        g_maxFieldLength = std::clamp(sConfigMgr->GetOption<uint32>("SanctuaryProfile.MaxFieldLength", 160u), 40u, 200u);

        LOG_INFO("module.sanctuaryprofile", "Sanctuary profiles {}: {} characters per line, visible within {} yards.",
            g_enabled ? "enabled" : "disabled", g_maxFieldLength, g_lookRange);
    }
};

void AddSC_sanctuary_profile_scripts()
{
    new sanctuary_profile_playerscript();
    new sanctuary_profile_worldscript();
}
