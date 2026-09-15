/*
 * mod-sanctuary-faction - `.faction`
 *
 * Two audiences in one tree, split by RBAC:
 *
 *   game master  create, disband, rank, spell, set, reload - the shape of a faction
 *   player       info, roster, leave                       - your own membership
 *
 * The split is the whole security model of this module and it is worth being explicit
 * about why it lives HERE rather than in the addon. Sanctuary's addons talk to the server
 * over an addon prefix, and anybody can send those messages from their own Lua - so an
 * addon that greys out a button has made the UI tidy and nothing else. Every rule that
 * matters is a check on this side of the wire.
 *
 * The addon window, when it is written, will send these same commands. That is deliberate:
 * one implementation, one set of checks, and a button that cannot do anything a player
 * could not have typed.
 */

#include "SanctuaryFactionInternal.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SanctuaryIdentity.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>
#include <sstream>

namespace
{
    // 100001 voice, 100002 outlaw, 100003 lawman, 100004 carry, 100005 disguise,
    // 100006 carry staging. See the module's db-auth migration.
    constexpr uint32 RBAC_PERM_COMMAND_FACTION = 100007;

    /*
     * Who has been asked to join what, until they answer.
     *
     * In memory and not persisted, deliberately: an invite is a conversation, and one
     * that survived a server restart would be a faction somebody joins a week later
     * having forgotten who asked. Cleared when it is answered; a stale entry costs one
     * lookup and is overwritten by the next invite.
     */
    std::unordered_map<ObjectGuid::LowType, uint32> g_invites;
}

using namespace Acore::ChatCommands;

class sanctuary_faction_commandscript : public CommandScript
{
public:
    sanctuary_faction_commandscript() : CommandScript("sanctuary_faction_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable factionTable =
        {
            // The shape of a faction. Game masters only.
            { "create",  HandleCreate,  rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "disband", HandleDisband, rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "rank",    HandleRank,    rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "spell",   HandleSpell,   rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "set",     HandleSet,     rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "reload",  HandleReload,  rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "list",    HandleList,    rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "dummy",   HandleDummy,   rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },

            /*
             * Your own membership, and running the faction you are in.
             *
             * All of these carry the SAME rbac permission every account holds, because
             * rbac is the wrong instrument for the question they ask. Whether you may kick
             * somebody depends on your rank in your faction and on theirs, which is a fact
             * about the game world rather than about your account - so each handler asks
             * SanctuaryFaction::May() and the rank table decides.
             *
             * The LADDER is not here. Defining ranks and what they grant stays a game
             * master command: those are balance decisions, and a leader who could write
             * the ladder could write themselves CAP_OPEN_ANY_LOCK on the first evening.
             */
            { "roster",  HandleRoster,  RBAC_PERM_COMMAND_FACTION, Console::No },
            { "leave",   HandleLeave,   RBAC_PERM_COMMAND_FACTION, Console::No },
            { "invite",  HandleInvite,  RBAC_PERM_COMMAND_FACTION, Console::No },
            { "rename",  HandleRename,  RBAC_PERM_COMMAND_FACTION, Console::No },
            { "accept",  HandleAccept,  RBAC_PERM_COMMAND_FACTION, Console::No },
            { "kick",    HandleKick,    RBAC_PERM_COMMAND_FACTION, Console::No },
            { "promote", HandlePromote, RBAC_PERM_COMMAND_FACTION, Console::No },
            { "demote",  HandleDemote,  RBAC_PERM_COMMAND_FACTION, Console::No },

            // Bare `.faction` reports rather than doing anything, the same way `.lawman`
            // and `.outlaw` do: a standing you cannot see is one you forget you have.
            { "",        HandleInfo,    RBAC_PERM_COMMAND_FACTION, Console::No }
        };

        static ChatCommandTable commandTable = { { "faction", factionTable } };
        return commandTable;
    }

private:
    /*
     * Says something to whoever ran the command, in their chat window.
     *
     * Lifted from mod-sanctuary-stash, which found this the hard way: a command that
     * arrives over the addon channel gets an AddonChannelCommandHandler, and its
     * SendSysMessage returns the text to the ADDON rather than putting it on screen. Every
     * command that reports rather than acts looks broken through that path. A handler built
     * from the session lands in the chat frame whichever way the command arrived.
     */
    static void Say(ChatHandler* handler, std::string const& line)
    {
        if (Player* player = handler->GetPlayer())
            ChatHandler(player->GetSession()).PSendSysMessage("{}", line);
        else
            handler->PSendSysMessage("{}", line);          // the console has no session
    }

    // --- game master ---------------------------------------------------------

    static bool HandleCreate(ChatHandler* handler, Tail name)
    {
        std::string const wanted(name);

        if (wanted.empty())
            return Refuse(handler, "Name it: .faction create <name>");

        /*
         * The two ways this can fail are asked about separately.
         *
         * Create returns 0 for both "the name is taken" and "the row could not be read
         * back", and reporting the first for the second is how a database race came to be
         * announced as a name collision for a name nobody had used. Whatever goes wrong
         * here, what the player is told is now true.
         */
        if (SanctuaryFactionStore::FindByName(wanted))
            return Refuse(handler, "There is already a faction by that name.");

        ObjectGuid::LowType const founder =
            handler->GetPlayer() ? handler->GetPlayer()->GetGUID().GetCounter() : 0;

        uint32 const id = SanctuaryFactionStore::Create(wanted, "", founder);

        if (!id)
            return Refuse(handler, "The realm could not store that faction. Nothing was made.");

        /*
         * A whole ladder and a founder, rather than an empty shell.
         *
         * The first draft created the faction and one nameless rung, and that was a mistake
         * worth recording: creating a faction left you not in it, with no rank able to
         * invite anybody, so the thing you had just founded did nothing and looked broken.
         * Every other "create" in the game - a guild above all - makes the creator its head.
         *
         * Four rungs: somebody just arrived, somebody settled, somebody who may recruit,
         * and somebody who may do all of it. The two lower ones carry no powers at all and
         * exist to be renamed - a faction wants to call its own people something, and the
         * difference between an Initiate and a Member is the faction's to define.
         *
         * A game master rewrites what each rung may DO with `.faction rank`; the faction
         * renames them itself from the window.
         */
        // Named once, because the creator is put on it below and the two drifted apart the
        // last time the ladder changed: the rungs grew from three to four and the founder
        // was left standing on rung 2, which had become Officer. They were then the leader
        // of a faction they could not rename, and the Ranks panel - which PERM_EDIT is what
        // opens - simply did not appear.
        uint8 const leaderRank = 3;

        SanctuaryFactionStore::SetRank(id, 0, "Member",        SanctuaryFaction::PERM_NONE);
        SanctuaryFactionStore::SetRank(id, 1, "Lower Officer", SanctuaryFaction::PERM_NONE);

        SanctuaryFactionStore::SetRank(id, 2, "Higher Officer",
            SanctuaryFaction::PERM_INVITE | SanctuaryFaction::PERM_KICK);

        SanctuaryFactionStore::SetRank(id, leaderRank, "Leader",
            SanctuaryFaction::PERM_INVITE | SanctuaryFaction::PERM_KICK |
            SanctuaryFaction::PERM_PROMOTE | SanctuaryFaction::PERM_DEMOTE |
            SanctuaryFaction::PERM_EDIT);

        /*
         * The creator is put at the top, when there is one.
         *
         * Not from the console, which has no player - and a game master founding a faction
         * for somebody else steps out with `.faction set 0`, which is one command against
         * the four it would otherwise take to build this by hand.
         */
        if (Player* player = handler->GetPlayer())
        {
            SanctuaryFactionStore::SetMembership(player, id, leaderRank);

            // No instructions on how to undo it: the window has a Leave button, and a line
            // of chat telling somebody to type a command is the thing the window exists to
            // replace.
            Say(handler, Acore::StringFormat(
                "Founded {} (id {}). You are its Leader. Ranks: Member, Lower Officer, "
                "Higher Officer, Leader.", wanted, id));
        }
        else
        {
            Say(handler, Acore::StringFormat(
                "Founded {} (id {}), with ranks Member, Lower Officer, Higher Officer "
                "and Leader.", wanted, id));
        }

        return true;
    }

    static bool HandleDisband(ChatHandler* handler, Tail name)
    {
        SanctuaryFactionStore::Faction const* faction =
            SanctuaryFactionStore::FindByName(std::string(name));

        if (!faction)
            return Refuse(handler, "No faction by that name.");

        std::string const label = faction->name;
        uint32 const id = faction->id;

        SanctuaryFactionStore::Disband(id);

        Say(handler, Acore::StringFormat(
            "{} is disbanded. Anything it was lending its members has been taken back.", label));

        return true;
    }

    /*
     * `.faction rank <id> <rank> <permissions> <name>`
     *
     * The mask is a number rather than words, and the name comes last so it can hold
     * spaces. The bits are documented in SanctuaryFaction.h; `.faction rank` with no
     * arguments prints them rather than making anybody go and read the header.
     */
    static bool HandleRank(ChatHandler* handler, Optional<uint32> faction, Optional<uint8> rank,
                           Optional<uint32> permissions, Tail name)
    {
        if (!faction || !rank || !permissions || std::string(name).empty())
        {
            Say(handler, "Usage: .faction rank <faction id> <rank> <permissions> <name>");
            Say(handler, "  permissions: 1 invite  2 kick  4 promote  8 demote  16 edit  32 disband");
            Say(handler, "  16 and 32 are stored but unused: the ladder is a game master's to write.");
            Say(handler, "  rank 0 is the LOWEST; higher is senior, and grants are cumulative upward.");
            Say(handler, "  what a rank gives its members is spells - see .faction spell.");
            return true;
        }

        if (!SanctuaryFactionStore::SetRank(*faction, *rank, std::string(name), *permissions))
            return Refuse(handler, "No faction with that id. Try .faction list.");

        Say(handler, Acore::StringFormat("Rank {} of faction {} is now \"{}\".",
            uint32(*rank), *faction, std::string(name)));

        return true;
    }

    /*
     * `.faction spell <add|remove> <faction id> <rank> <spell id>`
     *
     * Deliberately does NOT validate the spell against the client. It cannot: the server
     * knows every spell in its own store, and whether the CLIENT will draw it in the
     * spellbook is a property of that spell's SkillLineAbility row which no check here can
     * settle. The way to find out is `.learn <id>` on a non-game-master character of the
     * wrong class, which runs the identical path this grant does. That warning is printed
     * rather than buried, because the failure is silent and looks like this module's fault.
     */
    static bool HandleSpell(ChatHandler* handler, Optional<std::string> action,
                            Optional<uint32> faction, Optional<uint8> rank, Optional<uint32> spell)
    {
        if (!action || !faction || !rank || !spell)
        {
            Say(handler, "Usage: .faction spell <add|remove> <faction id> <rank> <spell id>");
            return true;
        }

        bool const adding = (*action == "add");

        if (!adding && *action != "remove")
            return Refuse(handler, "Say add or remove.");

        if (!sSpellMgr->GetSpellInfo(*spell))
            return Refuse(handler, "The server has no such spell.");

        bool const done = adding
            ? SanctuaryFactionStore::AddRankSpell(*faction, *rank, *spell)
            : SanctuaryFactionStore::RemoveRankSpell(*faction, *rank, *spell);

        if (!done)
            return Refuse(handler, "No faction with that id. Try .faction list.");

        Say(handler, Acore::StringFormat("Spell {} {} rank {} of faction {}. Members online have been settled.",
            *spell, adding ? "added to" : "removed from", uint32(*rank), *faction));

        if (adding)
            Say(handler, "|cffffcc00Check it appears in the spellbook|r - .learn it on an ordinary "
                         "character of the wrong class first. A spell can be known and castable "
                         "and still never be drawn.");

        return true;
    }

    /// `.faction set <faction id> <rank>` on the selected player. 0 takes them out.
    static bool HandleSet(ChatHandler* handler, Optional<uint32> faction, Optional<uint8> rank)
    {
        Player* target = handler->getSelectedPlayer();

        if (!target)
            return Refuse(handler, "Select somebody first.");

        if (!faction)
        {
            Say(handler, "Usage: .faction set <faction id> <rank>, or .faction set 0 to remove them.");
            return true;
        }

        if (!*faction)
        {
            SanctuaryFactionStore::ClearMembership(target);
            Say(handler, Acore::StringFormat("{} belongs to no faction now.", target->GetName()));
            return true;
        }

        if (!SanctuaryFactionStore::SetMembership(target, *faction, rank ? *rank : uint8(0)))
            return Refuse(handler, "No faction with that id. Try .faction list.");

        Say(handler, Acore::StringFormat("{} is now rank {} of {}.",
            target->GetName(), uint32(rank ? *rank : 0), SanctuaryFaction::NameOf(*faction)));

        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        bool any = false;

        for (uint32 id = 1; id < 100000; ++id)
        {
            SanctuaryFactionStore::Faction const* faction = SanctuaryFactionStore::Find(id);

            if (!faction)
                continue;

            any = true;

            Say(handler, Acore::StringFormat("  {} - {} ({} rank(s), {} member(s))",
                faction->id, faction->name, faction->ranks.size(),
                SanctuaryFactionStore::Roster(faction->id).size()));
        }

        if (!any)
            Say(handler, "No factions yet. .faction create <name> makes one.");

        return true;
    }

    /*
     * `.faction dummy <count>` - pads YOUR OWN roster window with invented members.
     *
     * For looking at the window with a full list in it, which is otherwise impossible
     * without thirty people logged in at once. Nothing is written: the rows are generated
     * as the roster is sent and exist only in that message, so they vanish on `.faction
     * dummy 0`, on logout, and on a restart.
     */
    static bool HandleDummy(ChatHandler* handler, Optional<uint32> count)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        uint32 const rows = count ? std::min<uint32>(*count, 200) : 0;

        SanctuaryFactionStore::SetDummyRoster(player->GetGUID().GetCounter(), rows);

        if (!rows)
            Say(handler, "Roster padding off.");
        else
            Say(handler, Acore::StringFormat(
                "Roster padded with {} invented member(s). Nothing was written; "
                ".faction dummy 0 clears it.", rows));

        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        SanctuaryFactionStore::LoadAll();
        Say(handler, "Factions reloaded from the database.");
        return true;
    }

    // --- players -------------------------------------------------------------

    static bool HandleInfo(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        uint32 const faction = SanctuaryFaction::FactionOf(player);

        if (!faction)
        {
            Say(handler, "You belong to no faction.");
            return true;
        }

        Say(handler, Acore::StringFormat("{}, {}.",
            SanctuaryFaction::NameOf(faction), SanctuaryFaction::RankNameOf(player)));

        return true;
    }

    /*
     * The roster.
     *
     * Every name here goes through mod-sanctuary-identity, and that is not a nicety: a
     * roster is the most natural place on the realm to leak a name somebody has not been
     * introduced to. It is built per viewer for exactly that reason - it cannot be one
     * payload broadcast to the whole faction, because what each member may be told differs.
     * The carry addon shipped with this bug a week ago; it is not going to ship again here.
     */
    static bool HandleRoster(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        uint32 const faction = SanctuaryFaction::FactionOf(player);

        if (!faction)
        {
            Say(handler, "You belong to no faction.");
            return true;
        }

        Say(handler, Acore::StringFormat("{}:", SanctuaryFaction::NameOf(faction)));

        for (ObjectGuid::LowType guid : SanctuaryFactionStore::Roster(faction))
        {
            SanctuaryFactionStore::Member const* member = SanctuaryFactionStore::MemberOf(guid);

            if (!member)
                continue;

            /*
             * TODO: name through SanctuaryIdentity::LabelFor once the addon window exists.
             *
             * LabelFor takes two live Players, and a roster is mostly people who are not
             * logged in - so an offline member needs an answer this module does not have
             * yet. Left as the character's stored name ONLY because this command is game
             * master facing in practice; the addon window must not copy it.
             */
            std::string name;

            if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid(HighGuid::Player, guid), name))
                continue;

            std::string rankName;

            if (SanctuaryFactionStore::Faction const* row = SanctuaryFactionStore::Find(faction))
            {
                auto rank = row->ranks.find(member->rank);
                rankName = rank == row->ranks.end() ? "?" : rank->second.name;
            }

            Say(handler, Acore::StringFormat("  {} - {}", name, rankName));
        }

        return true;
    }

    /*
     * ===================== delegated membership =============================
     *
     * A faction runs its own roster; a game master owns its ladder.
     *
     * Three rules do all the work here, and the second and third are the ones without
     * which PERM_PROMOTE quietly becomes "make yourself leader":
     *
     *   1. You may only touch somebody in YOUR faction.
     *   2. You may not touch somebody of your own rank or higher. Otherwise a junior
     *      officer holding PERM_KICK removes the leader.
     *   3. You may not promote anybody TO your own rank or higher. Otherwise promotion is
     *      self-elevation with one extra step: raise a friend to leader, have them raise
     *      you back.
     *
     * Rule 2 is also what stops somebody demoting a peer out of spite, and what makes the
     * top rung of the ladder genuinely the top.
     */

    /// The member this command is about: the selection, a name, or a guid from the addon.
    static bool Target(ChatHandler* handler, std::string const& name,
                       ObjectGuid::LowType& guid, std::string& label)
    {
        if (!name.empty())
        {
            /*
             * All digits means the addon sent it, and it is a guid rather than a name.
             *
             * The window has to address somebody it may only know by an alias - a member
             * this viewer was never introduced to - and an alias resolves to no character.
             * A guid always resolves, and giving one to the server reveals nothing the
             * viewer could not already see.
             *
             * Typed by hand it works too, and is rank-checked exactly the same way.
             */
            if (name.find_first_not_of("0123456789") == std::string::npos)
            {
                guid = ObjectGuid::LowType(strtoul(name.c_str(), nullptr, 10));

                if (!guid || !SanctuaryFactionStore::MemberOf(guid))
                    return false;

                if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid(HighGuid::Player, guid), label))
                    label = "They";

                return true;
            }

            ObjectGuid const found = sCharacterCache->GetCharacterGuidByName(name);

            if (found.IsEmpty())
                return false;

            guid = found.GetCounter();
            label = name;
            return true;
        }

        Player* selected = handler->getSelectedPlayer();

        if (!selected || selected == handler->GetPlayer())
            return false;

        guid = selected->GetGUID().GetCounter();

        /*
         * The selection is named through mod-sanctuary-identity, not GetName().
         *
         * An officer kicking somebody they have not been introduced to should be told the
         * alias they already see. Reporting the real name here would make `.faction kick`
         * a name lookup for anybody standing next to a stranger.
         */
        label = SanctuaryIdentity::LabelFor(handler->GetPlayer(), selected);
        return true;
    }

    /*
     * Everything rules 1 and 2 need, answered once.
     *
     * Returns the actor's own membership on success. `subject` is filled in with the
     * target's, which the caller needs for rule 3.
     */
    static SanctuaryFactionStore::Member const* Standing(
        ChatHandler* handler, ObjectGuid::LowType targetGuid,
        SanctuaryFaction::Permission permission,
        SanctuaryFactionStore::Member const*& subject)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return nullptr;

        SanctuaryFactionStore::Member const* self =
            SanctuaryFactionStore::MemberOf(player->GetGUID().GetCounter());

        if (!self)
        {
            Say(handler, "You belong to no faction.");
            return nullptr;
        }

        if (!SanctuaryFaction::May(player, permission))
        {
            Say(handler, "Your rank does not carry that.");
            return nullptr;
        }

        subject = SanctuaryFactionStore::MemberOf(targetGuid);

        if (!subject || subject->faction != self->faction)
        {
            Say(handler, "They are not one of yours.");
            return nullptr;
        }

        if (subject->rank >= self->rank)
        {
            Say(handler, "They are your equal or your senior. That is not yours to do.");
            return nullptr;
        }

        return self;
    }

    /*
     * `.faction invite` on a selected player.
     *
     * Offers rather than conscripts. Guilds put a dialog on the screen; this module has no
     * addon window yet, so the offer is a line of chat and `.faction accept` is the button.
     * That is not a placeholder for the sake of it - joining has to be the invitee's own
     * act, or an invite is a way to move somebody into a faction whose spells and
     * capabilities they never asked for.
     */
    static bool HandleInvite(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        SanctuaryFactionStore::Member const* self =
            SanctuaryFactionStore::MemberOf(player->GetGUID().GetCounter());

        if (!self)
            return Refuse(handler, "You belong to no faction.");

        if (!SanctuaryFaction::May(player, SanctuaryFaction::PERM_INVITE))
            return Refuse(handler, "Your rank does not carry that.");

        Player* target = handler->getSelectedPlayer();

        if (!target || target == player)
            return Refuse(handler, "Select the person you mean to ask.");

        if (SanctuaryFaction::FactionOf(target))
            return Refuse(handler, "They already belong to a faction.");

        g_invites[target->GetGUID().GetCounter()] = self->faction;

        ChatHandler(target->GetSession()).PSendSysMessage(
            "{} asks you to join {}. Type |cff00ff00.faction accept|r if you will.",
            SanctuaryIdentity::LabelFor(target, player),
            SanctuaryFaction::NameOf(self->faction));

        Say(handler, Acore::StringFormat("Asked {}.",
            SanctuaryIdentity::LabelFor(player, target)));

        return true;
    }

    static bool HandleAccept(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        auto invite = g_invites.find(player->GetGUID().GetCounter());

        if (invite == g_invites.end())
            return Refuse(handler, "Nobody has asked you.");

        uint32 const faction = invite->second;
        g_invites.erase(invite);

        if (SanctuaryFaction::FactionOf(player))
            return Refuse(handler, "You already belong to a faction.");

        // Rank 0. The bottom of the ladder is the only rung an invite may ever land on -
        // anything else would be a promotion the inviter did not have to hold PERM_PROMOTE
        // to give.
        if (!SanctuaryFactionStore::SetMembership(player, faction, 0))
            return Refuse(handler, "That faction is gone.");

        Say(handler, Acore::StringFormat("You are one of {} now.",
            SanctuaryFaction::NameOf(faction)));

        return true;
    }

    /*
     * Which rung, said in words rather than as an index.
     *
     * "Rank 2 is called Kingpin now" names neither end of what just happened: the number is
     * an index into a table nobody has seen, and the name is the thing that changed. Saying
     * WHERE the rung sits identifies it in terms that survive the rename.
     *
     * Counted down from the top, because the top is the fixed point - a game master adding
     * a rung at the bottom would otherwise renumber every rung above it. Kept in step with
     * the same four words in the addon's Ranks panel.
     */
    static std::string RungDescription(uint32 faction, uint8 rank)
    {
        SanctuaryFactionStore::Faction const* row = SanctuaryFactionStore::Find(faction);

        if (!row || row->ranks.empty() || rank > row->ranks.rbegin()->first)
            return Acore::StringFormat("Rank {}", uint32(rank));

        switch (row->ranks.rbegin()->first - rank)
        {
            case 0: return "Leader";
            case 1: return "Higher Officer";
            case 2: return "Lower Officer";
            case 3: return "Member";
            default: break;
        }

        // A ladder deeper than the four a faction is created with has no agreed word for
        // its lower rungs, and inventing one would be worse than saying which it is.
        return Acore::StringFormat("Rank {}", uint32(rank));
    }

    /*
     * `.faction rename <rank> <name>` - what a rung is CALLED, in your own faction.
     *
     * The one ladder edit a faction may make for itself, and the split is deliberate: a
     * game master decides what a rank may DO, because that is a balance decision, and the
     * faction decides what it is CALLED, because "we call our recruits Deckhands" is
     * nobody else's business. PERM_EDIT is what carries it, and a faction created from the
     * window gives it to its Leader.
     *
     * Nothing here can add a rung, remove one, or change a single permission.
     */
    static bool HandleRename(ChatHandler* handler, Optional<uint8> rank, Tail name)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        uint32 const faction = SanctuaryFaction::FactionOf(player);

        if (!faction)
            return Refuse(handler, "You belong to no faction.");

        if (!SanctuaryFaction::May(player, SanctuaryFaction::PERM_EDIT))
            return Refuse(handler, "Your rank does not carry that.");

        std::string wanted(name);

        if (!rank || wanted.empty())
        {
            Say(handler, "Usage: .faction rename <rank> <name>");
            return true;
        }

        // `name` is VARCHAR(32); truncated here so a long name is shortened rather than
        // refused by the database after the cache has already been updated.
        if (wanted.size() > 32)
            wanted.resize(32);

        if (!SanctuaryFactionStore::SetRankName(faction, *rank, wanted))
            return Refuse(handler, "Your faction has no rank with that number.");

        /*
         * Everybody's window, not just the one that asked.
         *
         * A rank name is on every roster row of everybody in the faction, so a rename that
         * only reached the person who typed it would leave the rest reading the old name
         * until they next logged in.
         */
        for (ObjectGuid::LowType guid : SanctuaryFactionStore::Roster(faction))
            if (Player* member = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, guid)))
                SanctuaryFactionStore::PushState(member);

        Say(handler, Acore::StringFormat("{} is called \"{}\" now.",
            RungDescription(faction, *rank), wanted));
        return true;
    }

    static bool HandleKick(ChatHandler* handler, Tail name)
    {
        ObjectGuid::LowType guid = 0;
        std::string label;

        if (!Target(handler, std::string(name), guid, label))
            return Refuse(handler, "Select somebody, or name them.");

        SanctuaryFactionStore::Member const* subject = nullptr;

        if (!Standing(handler, guid, SanctuaryFaction::PERM_KICK, subject))
            return true;

        // Offline as readily as online. Their spells are settled at their next login,
        // which is the whole reason Reconcile runs there as well as here.
        if (Player* online = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, guid)))
            SanctuaryFactionStore::ClearMembership(online);
        else
            SanctuaryFactionStore::ClearMembershipOffline(guid);

        Say(handler, Acore::StringFormat("{} is out.", label));
        return true;
    }

    static bool Move(ChatHandler* handler, std::string const& name, bool up)
    {
        ObjectGuid::LowType guid = 0;
        std::string label;

        if (!Target(handler, name, guid, label))
            return Refuse(handler, "Select somebody, or name them.");

        SanctuaryFactionStore::Member const* subject = nullptr;

        SanctuaryFactionStore::Member const* self = Standing(handler, guid,
            up ? SanctuaryFaction::PERM_PROMOTE : SanctuaryFaction::PERM_DEMOTE, subject);

        if (!self)
            return true;

        uint32 const faction = subject->faction;
        uint8 next = 0;

        bool const found = up
            ? SanctuaryFactionStore::NextRankUp(faction, subject->rank, next)
            : SanctuaryFactionStore::NextRankDown(faction, subject->rank, next);

        if (!found)
            return Refuse(handler, up ? "They are already at the top of the ladder."
                                      : "They are already at the bottom of the ladder.");

        /*
         * Rule 3, and the only check in this file that stops the system being pointless.
         *
         * Without it PERM_PROMOTE is transitive: raise somebody to your own rank, and they
         * hold whatever you hold - including the power to raise you again. Two players and
         * two commands, and the top of the ladder belongs to whoever thought of it first.
         */
        if (up && next >= self->rank)
            return Refuse(handler, "You cannot raise anybody to your own standing.");

        // `subject` points into the member cache, which the writes below rewrite. Nothing
        // is read through it after this line.
        Player* online = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, guid));

        if (online)
            SanctuaryFactionStore::SetMembership(online, faction, next);
        else
            SanctuaryFactionStore::SetMembershipOffline(guid, faction, next);

        std::string rankName = "?";

        if (SanctuaryFactionStore::Faction const* row = SanctuaryFactionStore::Find(faction))
        {
            auto rank = row->ranks.find(next);

            if (rank != row->ranks.end())
                rankName = rank->second.name;
        }

        Say(handler, Acore::StringFormat("{} is {} now.", label, rankName));

        if (online)
            ChatHandler(online->GetSession()).PSendSysMessage("You are {} now.", rankName);

        return true;
    }

    static bool HandlePromote(ChatHandler* handler, Tail name)
    {
        return Move(handler, std::string(name), true);
    }

    static bool HandleDemote(ChatHandler* handler, Tail name)
    {
        return Move(handler, std::string(name), false);
    }

    static bool HandleLeave(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        if (!SanctuaryFaction::FactionOf(player))
        {
            Say(handler, "You belong to no faction.");
            return true;
        }

        SanctuaryFactionStore::ClearMembership(player);
        Say(handler, "You have left. Anything the faction was lending you is gone with it.");

        return true;
    }

    /// Reports and returns true: a refusal is a complete answer, not a usage error.
    static bool Refuse(ChatHandler* handler, std::string const& why)
    {
        Say(handler, why);
        return true;
    }
};

void AddSC_sanctuary_faction_commandscript()
{
    new sanctuary_faction_commandscript();
}
