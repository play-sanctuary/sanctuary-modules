/*
 * mod-sanctuary-faction
 *
 * Player factions: an organisation a character belongs to, with a ladder of ranks, where
 * the rank is what grants things - powers the other modules already check for, and spells.
 *
 * Three decisions shape everything below, and each is a decision NOT to do the obvious
 * thing:
 *
 * **Not the guild system.** Guilds already have ranks, a roster and a client window, which
 * makes them tempting. But a character can only be in one guild, and on this realm people
 * will want a guild and a faction - a merchant house, the watch, the Defias. So: our own
 * tables and our own window, and the core's guild tables are untouched.
 *
 * **Not Faction.dbc.** The word collides. FactionTemplate.dbc decides who may attack whom,
 * needs both halves shipped (server table AND patch-enUS-4.MPQ) and therefore cannot be
 * added to at runtime. Nothing here touches it. Everything here is server rows, which is
 * exactly why a faction can be created mid-session with no restart and no client patch.
 * If hostility is ever wanted, the lever is UnitScript::IfNormalReaction - see the note in
 * mod-sanctuary-outlaw, which already proves it works for two players - and it can be
 * added later without disturbing anything in this file.
 *
 * **Spells, and nothing else.** A rank grants a spell or it grants nothing. An earlier
 * draft also had a mask of "capabilities" - bits letting a rank shackle without irons, or
 * open any strongbox - and it was cut on purpose. Two kinds of reward is twice as much to
 * balance and explain, and an invisible boolean that quietly changes what a door does is
 * worse for the player than a spell they can see in their book. The one cost is the
 * spellbook-placement trap documented in sanctuary_faction_rank_spell.sql.
 */

#include "SanctuaryFactionInternal.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

#include <algorithm>
#include <cctype>

namespace
{
    bool g_enabled = true;

    std::unordered_map<uint32, SanctuaryFactionStore::Faction> g_factions;

    /*
     * Every member on the realm, online or not.
     *
     * Loaded whole at startup rather than built at login, unlike mod-sanctuary-outlaw's
     * set. The difference is what the two are for: an outlaw flag only means anything
     * while its owner is standing there, whereas a roster has to be able to name people
     * who are not logged in - which is most of a faction, most of the time.
     */
    std::unordered_map<ObjectGuid::LowType, SanctuaryFactionStore::Member> g_members;

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        return text;
    }

    /// The rank a member is standing on, or nothing when the ladder has a hole in it.
    SanctuaryFactionStore::Rank const* RankRow(SanctuaryFactionStore::Member const* member)
    {
        if (!member)
            return nullptr;

        auto faction = g_factions.find(member->faction);

        if (faction == g_factions.end())
            return nullptr;

        auto rank = faction->second.ranks.find(member->rank);

        return rank == faction->second.ranks.end() ? nullptr : &rank->second;
    }

    SanctuaryFactionStore::Member const* MemberFor(Player const* player)
    {
        if (!g_enabled || !player)
            return nullptr;

        return SanctuaryFactionStore::MemberOf(player->GetGUID().GetCounter());
    }
}

// ============================ the store =====================================

namespace SanctuaryFactionStore
{
    Faction const* Find(uint32 id)
    {
        auto it = g_factions.find(id);
        return it == g_factions.end() ? nullptr : &it->second;
    }

    Faction const* FindByName(std::string const& name)
    {
        std::string const wanted = Lower(name);

        for (auto const& [id, faction] : g_factions)
            if (Lower(faction.name) == wanted)
                return &faction;

        return nullptr;
    }

    Member const* MemberOf(ObjectGuid::LowType guid)
    {
        auto it = g_members.find(guid);
        return it == g_members.end() ? nullptr : &it->second;
    }

    std::vector<ObjectGuid::LowType> Roster(uint32 faction)
    {
        std::vector<std::pair<uint8, ObjectGuid::LowType>> ranked;

        for (auto const& [guid, member] : g_members)
            if (member.faction == faction)
                ranked.emplace_back(member.rank, guid);

        // Rank carried alongside rather than looked up in the comparator: std::map's
        // operator[] inserts, and a comparator that quietly creates members would be a
        // roster that grows every time it is drawn.
        std::sort(ranked.begin(), ranked.end());

        std::vector<ObjectGuid::LowType> out;
        out.reserve(ranked.size());

        for (auto const& [rank, guid] : ranked)
            out.push_back(guid);

        return out;
    }

    void LoadAll()
    {
        g_factions.clear();
        g_members.clear();

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT `id`, `name`, `description`, `founded`, `founder` FROM `sanctuary_faction`"))
        {
            do
            {
                Field* field = rows->Fetch();

                Faction faction;
                faction.id          = field[0].Get<uint32>();
                faction.name        = field[1].Get<std::string>();
                faction.description = field[2].Get<std::string>();
                faction.founded     = field[3].Get<uint32>();
                faction.founder     = field[4].Get<uint32>();

                g_factions[faction.id] = std::move(faction);
            }
            while (rows->NextRow());
        }

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT `faction`, `rank_id`, `name`, `permissions` "
                "FROM `sanctuary_faction_rank`"))
        {
            do
            {
                Field* field = rows->Fetch();
                uint32 const id = field[0].Get<uint32>();

                auto faction = g_factions.find(id);

                // A rank whose faction is gone. The foreign key makes this impossible in a
                // healthy database; it is checked anyway because the alternative is
                // default-constructing a nameless faction and serving it to players.
                if (faction == g_factions.end())
                    continue;

                Rank rank;
                rank.name        = field[2].Get<std::string>();
                rank.permissions = field[3].Get<uint32>();

                faction->second.ranks[field[1].Get<uint8>()] = std::move(rank);
            }
            while (rows->NextRow());
        }

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT `faction`, `rank_id`, `spell` FROM `sanctuary_faction_rank_spell`"))
        {
            do
            {
                Field* field = rows->Fetch();

                auto faction = g_factions.find(field[0].Get<uint32>());

                if (faction == g_factions.end())
                    continue;

                // Spells may name a rank that has no row of its own. Left alone rather than
                // dropped: the rung can be written later and the spells are still on it.
                faction->second.ranks[field[1].Get<uint8>()].spells.insert(field[2].Get<uint32>());
            }
            while (rows->NextRow());
        }

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT `guid`, `faction`, `rank_id`, `joined` FROM `sanctuary_faction_member`"))
        {
            do
            {
                Field* field = rows->Fetch();

                Member member;
                member.faction = field[1].Get<uint32>();
                member.rank    = field[2].Get<uint8>();
                member.joined  = field[3].Get<uint32>();

                if (!g_factions.count(member.faction))
                    continue;

                g_members[field[0].Get<uint32>()] = member;
            }
            while (rows->NextRow());
        }

        LOG_INFO("module.sanctuaryfaction", ">> Loaded {} faction(s) and {} member(s).",
            g_factions.size(), g_members.size());
    }

    // --- mutations ----------------------------------------------------------

    /*
     * Escaped, because every name in this module is typed by a person.
     *
     * The queries below are built by formatting rather than by prepared statement, so an
     * apostrophe in "Bral's Company" would end the string literal and take the statement
     * with it. Harmless-looking until the first faction named after somebody.
     */
    std::string Escaped(std::string text)
    {
        CharacterDatabase.EscapeString(text);
        return text;
    }

    uint32 Create(std::string const& name, std::string const& description,
                  ObjectGuid::LowType founder)
    {
        if (name.empty() || FindByName(name))
            return 0;

        uint32 const now = uint32(GameTime::GetGameTime().count());

        /*
         * DirectExecute, not Execute, and the difference is the whole bug this once had.
         *
         * DatabaseWorkerPool::Execute ENQUEUES the statement on the async worker
         * (DatabaseWorkerPool.cpp:515) while Query runs synchronously on a connection of
         * its own. Written with Execute, the SELECT below raced the INSERT and usually lost:
         * Create returned 0, the caller reported the name as taken, and the INSERT then
         * landed anyway - so the faction existed, with no ranks and no members, and the
         * person who made it was told it had not been made. Four of them accumulated on the
         * dev realm before anyone worked out why.
         *
         * DirectExecute takes a connection and runs the statement before returning
         * (DatabaseWorkerPool.cpp:532), so the row is committed by the time it is read back.
         */
        CharacterDatabase.DirectExecute(
            "INSERT INTO `sanctuary_faction` (`name`, `description`, `founded`, `founder`) "
            "VALUES ('{}', '{}', {}, {})",
            Escaped(name), Escaped(description), now, founder);

        // Read back rather than guess. AUTO_INCREMENT is the database's to allocate, and a
        // cached id that disagreed with the stored one would be a faction nobody could join.
        QueryResult row = CharacterDatabase.Query(
            "SELECT `id` FROM `sanctuary_faction` WHERE `name` = '{}'", Escaped(name));

        if (!row)
            return 0;

        Faction faction;
        faction.id          = row->Fetch()[0].Get<uint32>();
        faction.name        = name;
        faction.description = description;
        faction.founded     = now;
        faction.founder     = founder;

        uint32 const id = faction.id;
        g_factions[id] = std::move(faction);

        return id;
    }

    bool Disband(uint32 faction)
    {
        if (!g_factions.count(faction))
            return false;

        // Gathered before the rows go, and reconciled after, so that anybody standing in
        // the world loses the lent spells now rather than at their next login.
        std::vector<ObjectGuid::LowType> const members = Roster(faction);

        // The foreign keys take the ladder and the roster with the faction. The receipts in
        // sanctuary_faction_granted are NOT keyed to it and must not be: they say what a
        // character was lent, and that has to survive long enough to be taken back.
        CharacterDatabase.Execute("DELETE FROM `sanctuary_faction` WHERE `id` = {}", faction);

        g_factions.erase(faction);

        for (ObjectGuid::LowType guid : members)
            g_members.erase(guid);

        for (ObjectGuid::LowType guid : members)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, guid)))
                Reconcile(player);

        return true;
    }

    bool SetRank(uint32 faction, uint8 rank, std::string const& name, uint32 permissions)
    {
        auto it = g_factions.find(faction);

        if (it == g_factions.end())
            return false;

        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_faction_rank` "
            "(`faction`, `rank_id`, `name`, `permissions`) "
            "VALUES ({}, {}, '{}', {})",
            faction, uint32(rank), Escaped(name), permissions);

        Rank& row = it->second.ranks[rank];
        row.name        = name;
        row.permissions = permissions;

        // The spell list is deliberately untouched. Rewriting a rung's powers should not
        // silently empty what it teaches, and the two are edited by different commands.
        return true;
    }

    bool SetRankName(uint32 faction, uint8 rank, std::string const& name)
    {
        auto it = g_factions.find(faction);

        if (it == g_factions.end() || name.empty())
            return false;

        auto row = it->second.ranks.find(rank);

        if (row == it->second.ranks.end())
            return false;

        CharacterDatabase.Execute(
            "UPDATE `sanctuary_faction_rank` SET `name` = '{}' "
            "WHERE `faction` = {} AND `rank_id` = {}",
            Escaped(name), faction, uint32(rank));

        row->second.name = name;
        return true;
    }

    bool AddRankSpell(uint32 faction, uint8 rank, uint32 spell)
    {
        auto it = g_factions.find(faction);

        if (it == g_factions.end() || !spell)
            return false;

        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_faction_rank_spell` (`faction`, `rank_id`, `spell`) "
            "VALUES ({}, {}, {})", faction, uint32(rank), spell);

        it->second.ranks[rank].spells.insert(spell);

        ReconcileFaction(faction);
        return true;
    }

    bool RemoveRankSpell(uint32 faction, uint8 rank, uint32 spell)
    {
        auto it = g_factions.find(faction);

        if (it == g_factions.end())
            return false;

        CharacterDatabase.Execute(
            "DELETE FROM `sanctuary_faction_rank_spell` "
            "WHERE `faction` = {} AND `rank_id` = {} AND `spell` = {}",
            faction, uint32(rank), spell);

        auto rankRow = it->second.ranks.find(rank);

        if (rankRow != it->second.ranks.end())
            rankRow->second.spells.erase(spell);

        ReconcileFaction(faction);
        return true;
    }

    bool SetMembershipOffline(ObjectGuid::LowType guid, uint32 faction, uint8 rank)
    {
        if (!g_factions.count(faction))
            return false;

        uint32 const now = uint32(GameTime::GetGameTime().count());

        // REPLACE rather than INSERT: the primary key on `guid` is what enforces one
        // faction per character, and this is the statement that has to respect it.
        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_faction_member` (`guid`, `faction`, `rank_id`, `joined`) "
            "VALUES ({}, {}, {}, {})", guid, faction, uint32(rank), now);

        Member& member = g_members[guid];

        // Joined stays put when somebody is only being promoted; it is when they arrived,
        // not when they were last touched.
        if (member.faction != faction)
            member.joined = now;

        member.faction = faction;
        member.rank    = rank;

        return true;
    }

    bool SetMembership(Player* player, uint32 faction, uint8 rank)
    {
        if (!player || !SetMembershipOffline(player->GetGUID().GetCounter(), faction, rank))
            return false;

        Reconcile(player);
        return true;
    }

    bool ClearMembershipOffline(ObjectGuid::LowType guid)
    {
        CharacterDatabase.Execute("DELETE FROM `sanctuary_faction_member` WHERE `guid` = {}", guid);

        return g_members.erase(guid) > 0;
    }

    bool ClearMembership(Player* player)
    {
        if (!player)
            return false;

        bool const was = ClearMembershipOffline(player->GetGUID().GetCounter());

        // Reconciled whether or not they were in anything: an interrupted leave could have
        // left receipts behind, and settling to "no faction, no lent spells" is the point.
        Reconcile(player);

        return was;
    }

    /*
     * Settles what this character should know against what this module has lent them.
     *
     * Reads the RECEIPT table, never the rank table, when deciding what to take back. The
     * reasons are written out in sanctuary_faction_granted.sql and both failure modes are
     * silent, so they are worth repeating in one line each: deriving the revoke set from
     * the ladder unlearns the wrong spells after any rank edit, and strips spells the
     * player earned elsewhere.
     *
     * Idempotent on purpose. It runs at login, at every promotion, at every rank edit and
     * at disband, and running it twice must cost nothing.
     */
    void Reconcile(Player* player)
    {
        if (!g_enabled || !player)
            return;

        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

        // What the ladder says they should have: their rank and every rung below it.
        std::unordered_set<uint32> entitled;

        if (Member const* member = MemberOf(guid))
            if (Faction const* faction = Find(member->faction))
                for (auto const& [rankId, rank] : faction->ranks)
                    if (rankId <= member->rank)
                        entitled.insert(rank.spells.begin(), rank.spells.end());

        // What this module has actually handed over.
        std::unordered_set<uint32> lent;

        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT `spell` FROM `sanctuary_faction_granted` WHERE `guid` = {}", guid))
        {
            do
            {
                lent.insert(rows->Fetch()[0].Get<uint32>());
            }
            while (rows->NextRow());
        }

        for (uint32 spell : lent)
        {
            if (entitled.count(spell))
                continue;

            player->removeSpell(spell, SPEC_MASK_ALL, false);

            // Direct, for the reason Create is: this function reads the receipt table at
            // the top, and a promotion followed quickly by a demotion would otherwise let
            // the second pass read what the first had not finished writing.
            CharacterDatabase.DirectExecute(
                "DELETE FROM `sanctuary_faction_granted` WHERE `guid` = {} AND `spell` = {}",
                guid, spell);
        }

        for (uint32 spell : entitled)
        {
            if (lent.count(spell))
                continue;

            /*
             * Theirs already, by some other road - a class ability, a trainer, a drop.
             *
             * Nothing is learned and, more importantly, NO RECEIPT IS WRITTEN. Leaving the
             * faction will therefore not take it, which is the whole difference between a
             * faction lending a spell and a faction confiscating one.
             */
            if (player->HasSpell(spell))
                continue;

            player->learnSpell(spell);

            CharacterDatabase.DirectExecute(
                "INSERT IGNORE INTO `sanctuary_faction_granted` (`guid`, `spell`) VALUES ({}, {})",
                guid, spell);
        }

        // Last, so the window is told about the world after it has changed rather than
        // during. Everything above may have learned or unlearned a spell the window lists.
        PushState(player);
    }

    bool NextRankUp(uint32 faction, uint8 from, uint8& next)
    {
        Faction const* row = Find(faction);

        if (!row)
            return false;

        auto it = row->ranks.upper_bound(from);

        if (it == row->ranks.end())
            return false;

        next = it->first;
        return true;
    }

    bool NextRankDown(uint32 faction, uint8 from, uint8& next)
    {
        Faction const* row = Find(faction);

        if (!row)
            return false;

        auto it = row->ranks.lower_bound(from);

        if (it == row->ranks.begin())
            return false;                              // already on the bottom rung

        next = (--it)->first;
        return true;
    }

    void ReconcileFaction(uint32 faction)
    {
        for (ObjectGuid::LowType guid : Roster(faction))
            if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, guid)))
                Reconcile(player);
    }
}

// ============================ the public face ===============================

namespace SanctuaryFaction
{
    bool IsEnabled() { return g_enabled; }

    uint32 FactionOf(Player const* player)
    {
        SanctuaryFactionStore::Member const* member = MemberFor(player);
        return member ? member->faction : 0;
    }

    std::string NameOf(uint32 faction)
    {
        SanctuaryFactionStore::Faction const* row = SanctuaryFactionStore::Find(faction);
        return row ? row->name : std::string();
    }

    uint8 RankOf(Player const* player)
    {
        SanctuaryFactionStore::Member const* member = MemberFor(player);
        return member ? member->rank : uint8(0);
    }

    std::string RankNameOf(Player const* player)
    {
        SanctuaryFactionStore::Rank const* rank = RankRow(MemberFor(player));
        return rank ? rank->name : std::string();
    }

    bool May(Player const* player, Permission permission)
    {
        SanctuaryFactionStore::Rank const* rank = RankRow(MemberFor(player));
        return rank && (rank->permissions & permission) != 0;
    }
}

// ============================ hooks =========================================

class sanctuary_faction_worldscript : public WorldScript
{
public:
    sanctuary_faction_worldscript() : WorldScript("sanctuary_faction_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryFaction.Enable", true);
    }

    void OnStartup() override
    {
        if (!g_enabled)
            return;

        SanctuaryFactionStore::LoadAll();
    }
};

class sanctuary_faction_playerscript : public PlayerScript
{
public:
    sanctuary_faction_playerscript() : PlayerScript("sanctuary_faction_playerscript",
        { PLAYERHOOK_ON_LOGIN }) { }

    /*
     * The only place an offline rank change can be made good.
     *
     * A game master promotes somebody who is not logged in, or disbands a faction
     * overnight, and no hook fires for the absent player. Settling at login rather than
     * trying to reach them is what makes those operations safe to perform at all.
     */
    void OnPlayerLogin(Player* player) override
    {
        SanctuaryFactionStore::Reconcile(player);
    }
};

void AddSC_sanctuary_faction_scripts()
{
    new sanctuary_faction_worldscript();
    new sanctuary_faction_playerscript();
}
