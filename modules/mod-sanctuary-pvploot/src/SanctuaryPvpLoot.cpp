/*
 * mod-sanctuary-pvploot
 *
 * A player killed in open-world PvP drops their belongings: a slice of the coin they were
 * carrying, and occasionally one item. A sack falls where they died and only their killer
 * can open it.
 *
 * The victim's corpse is never touched, so they release, run back and resurrect exactly as
 * they always would. That is the whole reason this is a sack and not the body itself: the
 * core requires a corpse to be CORPSE_BONES before it can be looted, and converting it is
 * what destroys the resurrectable corpse. The two cannot both be true.
 *
 * The sack is a creature corpse, not a gameobject. That is not a stylistic choice:
 * WorldSession::HandleLootOpcode opens with
 *
 *     if (!player->IsAlive() || !guid.IsCreatureOrVehicle())
 *         return;
 *
 * so CMSG_LOOT is only ever accepted for creatures, and a gameobject can never be the
 * subject of a client-initiated loot. Every gameobject form of this was proven by trace to
 * send a perfectly good loot window that the client then discarded, because the client had
 * never asked for one.
 *
 * A corpse also gives away the access control for free. Player::SendLoot resolves a
 * creature's loot recipient to OWNER_PERMISSION for that player and NONE_PERMISSION for
 * everyone else, and Unit's per-viewer dynamic-flag filter hides the lootable flag from
 * everyone else too - so the killer-only rule needs no script at all.
 *
 * The loot is written onto the corpse before anyone opens it. With a lootid of 0 the core's
 * own fill in Player::SendLoot is skipped, so our contents survive untouched.
 */

#include "Chat.h"
#include "Containers.h"
#include "Config.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{
    // Not an "s_" prefix: winsock2.h defines s_host and friends as macros on in_addr, and a
    // file-scope name colliding with one expands into nonsense before the compiler sees it.
    bool g_enabled = true;
    bool g_announce = true;
    bool g_explainToGameMasters = true;

    float g_goldPercent = 10.0f;
    float g_itemChance = 3.0f;
    uint32 g_maxItems = 1;

    bool g_allowInBattleground = false;
    bool g_allowInArena = false;
    bool g_allowInDuel = false;
    bool g_allowInSanctuary = false;
    bool g_allowGameMasters = false;
    bool g_allowSameAccount = false;

    uint32 g_minLevel = 10;
    uint32 g_maxLevelGap = 0;             ///< 0 disables the check entirely.
    uint32 g_repeatCooldownSeconds = 900;
    uint32 g_lootableSeconds = 300;
    uint32 g_spoilsEntry = 990001;

    bool g_takeSoulbound = false;
    bool g_takeEquipped = false;
    uint32 g_maxItemQuality = ITEM_QUALITY_EPIC;

    /*
     * Refuses to run unless the spoils template is one that can actually be looted.
     *
     * This module takes coin and items from the victim before the killer opens anything, so
     * a sack that cannot be opened is not a cosmetic fault - it destroys property. That is
     * not hypothetical: entry 990001 was silently overwritten by another module's
     * scaffolding with a type-2 prop carrying no script, and GameObject::Create happily
     * succeeded on it, so every kill quietly confiscated belongings into an unopenable
     * object. Disabling is the only safe response to any of these being wrong.
     */
    void ValidateSpoilsTemplate()
    {
        if (!g_enabled)
            return;

        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(g_spoilsEntry);

        if (!info)
        {
            LOG_ERROR("module.sanctuarypvploot",
                "Spoils creature {} is missing from creature_template. PvP loot is disabled - "
                "apply the module's SQL, or point SanctuaryPvpLoot.SpoilsEntry at the right entry.",
                g_spoilsEntry);

            g_enabled = false;
            return;
        }

        // A loot template would make Player::SendLoot fill the corpse from it and discard
        // the victim's belongings.
        if (info->lootid != 0)
        {
            LOG_ERROR("module.sanctuarypvploot",
                "Spoils creature {} has lootid {}, expected 0. PvP loot is disabled - the core "
                "would overwrite the victim's belongings with that loot template's contents.",
                g_spoilsEntry, info->lootid);

            g_enabled = false;
            return;
        }

        LOG_INFO("module.sanctuarypvploot", "Spoils creature {} verified.", g_spoilsEntry);
    }

    /// One item lifted from a victim, on its way into the sack.
    struct TakenItem
    {
        uint32 ItemId = 0;
        uint32 Count = 0;
        int32 RandomPropertyId = 0;
        uint32 RandomSuffix = 0;
    };

    /// killer+victim -> when they were last farmed, to blunt repeat killing.
    std::unordered_map<uint64, uint32> g_recentKills;

    uint64 PairKey(ObjectGuid killer, ObjectGuid victim)
    {
        return (uint64(killer.GetCounter()) << 32) ^ uint64(victim.GetCounter());
    }

    uint32 Now()
    {
        return uint32(GameTime::GetGameTime().count());
    }

    /*
     * Why this kill produces nothing, or nullptr if it counts.
     *
     * A reason rather than a bare bool because from in game the gates are indistinguishable
     * from an unlucky roll: a realm whose test characters were all below MinLevel looked
     * exactly like one where the 3% simply never hit, and stayed that way for days.
     */
    char const* DeclineReason(Player* killer, Player* victim)
    {
        if (!g_enabled)
            return "the module is disabled";

        if (!killer || !victim || killer == victim)
            return "not a kill between two players";

        // The hook fires from Unit::Kill for every player-on-player death, with no
        // filtering of its own, so all of this has to be checked here.
        if (!g_allowInDuel && (killer->duel || victim->duel))
            return "a duel";

        if (!g_allowInArena && (killer->InArena() || victim->InArena()))
            return "an arena";

        // Battlegrounds already have the real insignia mechanic; running both would mean
        // two things competing over the same death.
        if (!g_allowInBattleground && (killer->InBattleground() || victim->InBattleground()))
            return "a battleground";

        if (!g_allowInSanctuary && (killer->IsInSanctuary() || victim->IsInSanctuary()))
            return "a sanctuary";

        if (!g_allowGameMasters && (killer->IsGameMaster() || victim->IsGameMaster()))
            return "a game master is involved (.gm off to test)";

        if (!g_allowSameAccount && killer->GetSession() && victim->GetSession() &&
            killer->GetSession()->GetAccountId() == victim->GetSession()->GetAccountId())
            return "both characters are on one account";

        if (victim->GetLevel() < g_minLevel)
            return "the victim is below MinLevel";

        if (g_maxLevelGap > 0)
        {
            uint32 const gap = killer->GetLevel() > victim->GetLevel()
                ? killer->GetLevel() - victim->GetLevel()
                : victim->GetLevel() - killer->GetLevel();

            if (gap > g_maxLevelGap)
                return "the level gap is too wide";
        }

        if (g_repeatCooldownSeconds > 0)
        {
            uint64 const key = PairKey(killer->GetGUID(), victim->GetGUID());
            auto itr = g_recentKills.find(key);

            if (itr != g_recentKills.end() && Now() - itr->second < g_repeatCooldownSeconds)
                return "this pair is still on cooldown";
        }

        return nullptr;
    }

    /*
     * Reports a kill that produced nothing.
     *
     * Told to game masters by account security rather than by IsGameMaster(), because
     * testing is done with GM mode off - it is itself one of the reasons - and an account
     * that can see this is one that could read the same thing out of the log anyway.
     */
    void ReportDecline(Player* killer, Player* victim, char const* reason)
    {
        LOG_DEBUG("module.sanctuarypvploot", "No spoils, {} killing {}: {}",
            killer ? killer->GetName() : "?", victim ? victim->GetName() : "?", reason);

        if (!g_explainToGameMasters || !killer || !killer->GetSession())
            return;

        if (killer->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            return;

        ChatHandler(killer->GetSession()).PSendSysMessage("|cff9c9081No spoils:|r {}", reason);
    }

    /// Whether this particular item may be taken from a corpse.
    bool ItemIsEligible(Player* victim, Item* item, bool equipped)
    {
        if (!item)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return false;

        if (!g_takeEquipped && equipped)
            return false;

        if (!g_takeSoulbound && item->IsSoulBound())
            return false;

        if (proto->Quality > g_maxItemQuality)
            return false;

        // Quest items and conjured goods are either unsellable or would strand a quest.
        if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM)
            return false;

        if (proto->HasFlag(ITEM_FLAG_CONJURED))
            return false;

        // A bag with things in it would take its contents with it.
        if (proto->Class == ITEM_CLASS_CONTAINER && item->IsNotEmptyBag())
            return false;

        // Never take what the victim is standing in: their own corpse-run gear aside,
        // an item currently being traded or locked is not safely removable here.
        if (item->IsInTrade())
            return false;

        return true;
    }

    /// Everything on the victim that could be dropped, paired with where it lives.
    void CollectEligibleItems(Player* victim, std::vector<Item*>& into)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* item = victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                if (ItemIsEligible(victim, item, true))
                    into.push_back(item);
            }
        }

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (Item* item = victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                if (ItemIsEligible(victim, item, false))
                    into.push_back(item);
            }
        }

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* container = victim->GetBagByPos(bag);
            if (!container)
                continue;

            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
            {
                if (Item* item = victim->GetItemByPos(bag, uint8(slot)))
                {
                    if (ItemIsEligible(victim, item, false))
                        into.push_back(item);
                }
            }
        }
    }

    void TellPlayers(Player* killer, Player* victim, uint32 gold, std::size_t items)
    {
        if (!g_announce)
            return;

        if (gold == 0 && items == 0)
            return;

        std::string const coin = gold > 0
            ? Acore::StringFormat("{}g {}s {}c", gold / 10000, (gold % 10000) / 100, gold % 100)
            : "";

        if (victim->GetSession())
        {
            ChatHandler handler(victim->GetSession());

            if (gold > 0 && items > 0)
                handler.PSendSysMessage("|cffff4444You have been robbed:|r {} and {} item(s) fell from your pack.", coin, items);
            else if (gold > 0)
                handler.PSendSysMessage("|cffff4444You have been robbed:|r {} fell from your pack.", coin);
            else
                handler.PSendSysMessage("|cffff4444You have been robbed:|r {} item(s) fell from your pack.", items);
        }

        if (killer->GetSession())
            ChatHandler(killer->GetSession()).PSendSysMessage(
                "|cffd8b46aTheir belongings spilled where they fell.|r");
    }

    /*
     * Drops the sack.
     *
     * Built by hand rather than through Player::SummonGameObject: that helper calls
     * ToUnit()->AddGameObject on a player summoner, which ties the object's lifetime to
     * them - the sack would vanish the next time the killer died, which on a PvP realm is
     * exactly when it matters.
     */
    bool DropSpoils(Player* killer, Player* victim, uint32 gold, std::vector<TakenItem> const& items)
    {
        if (!victim->GetMap())
            return false;

        float const x = victim->GetPositionX();
        float const y = victim->GetPositionY();
        float const z = victim->GetPositionZ();
        float const o = victim->GetOrientation();

        // A creature summon's lifetime is its own, so unlike a summoned gameobject the sack
        // does not vanish when the killer next dies - which on a PvP realm is exactly when
        // it matters.
        TempSummon* sack = victim->SummonCreature(g_spoilsEntry, x, y, z, o,
            TEMPSUMMON_TIMED_DESPAWN, g_lootableSeconds * IN_MILLISECONDS);

        if (!sack)
        {
            LOG_ERROR("module.sanctuarypvploot",
                "Could not summon spoils creature {} - is its creature_template row present?",
                g_spoilsEntry);
            return false;
        }

        /*
         * Order is load-bearing here, and the core says so itself.
         *
         * Unit::Kill carries the comment "must be after setDeathState which resets dynamic
         * flags" above its own SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE), so the flag has to go
         * on after the corpse exists or it is wiped immediately.
         *
         * Nobody ever damages this creature, so IsDamageEnoughForLootingAndReward would
         * normally be false and Player::isAllowedToLoot would refuse on that alone - the
         * client is then never shown the corpse as lootable and never asks to loot it.
         *
         * That is handled declaratively by CREATURE_FLAG_EXTRA_NO_PLAYER_DAMAGE_REQ on the
         * template, which short-circuits the check. Emphatically NOT by
         * ResetPlayerDamageReq(), whose name reads like it clears the requirement while it
         * actually arms one: it sets _playerDamageReq to half of current health and clears
         * _damagedByPlayer, which is the precise combination that makes a corpse unlootable.
         */
        sack->setDeathState(DeathState::JustDied);
        sack->SetHealth(0);

        Loot& loot = sack->loot;
        loot.clear();
        loot.gold = gold;

        for (TakenItem const& taken : items)
        {
            LootItem entry;
            entry.itemid = taken.ItemId;
            entry.count = uint8(std::min<uint32>(taken.Count, 255));
            entry.randomPropertyId = taken.RandomPropertyId;
            entry.randomSuffix = taken.RandomSuffix;
            entry.is_looted = false;
            entry.is_blocked = false;
            entry.is_counted = false;
            entry.freeforall = false;
            entry.needs_quest = false;
            // Under the group threshold and outside the loot rules, so a killer in a raid
            // does not have their spoils put up for a roll.
            entry.is_underthreshold = true;
            entry.follow_loot_rules = false;

            loot.items.push_back(entry);
        }

        loot.unlootedCount = uint8(loot.items.size());

        // Non-NONE, or Player::SendLoot refills the corpse from the creature's own loot
        // template and the victim's belongings are replaced by whatever that holds.
        loot.loot_type = LOOT_CORPSE;

        // The entire killer-only restriction. SendLoot resolves this to OWNER_PERMISSION
        // for the recipient and NONE_PERMISSION for anybody else, and Unit's per-viewer
        // dynamic-flag filter hides the lootable flag from everyone else as well.
        sack->SetLootRecipient(killer);

        sack->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE);

        LOG_DEBUG("module.sanctuarypvploot",
            "Spoils of {} dropped for {}: {} copper, {} item(s). "
            "corpse={} dead={} lootable={} recipient={} inWorld={} visible={}",
            victim->GetName(), killer->GetName(), gold, items.size(),
            sack->GetGUID().ToString(), sack->isDead(),
            sack->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE),
            sack->GetLootRecipient() ? sack->GetLootRecipient()->GetName() : "none",
            sack->IsInWorld(), killer->CanSeeOrDetect(sack, false, true));

        return true;
    }
}

class sanctuary_pvploot_playerscript : public PlayerScript
{
public:
    sanctuary_pvploot_playerscript() : PlayerScript("sanctuary_pvploot_playerscript",
        { PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_BEFORE_SEND_LOOT }) { }

    /*
     * Fires from inside Player::SendLoot, after permission has been resolved. It is the
     * only way to tell "the client never asked for loot" from "the client asked and was
     * refused" - a distinction that cost several rounds of guesswork on the gameobject
     * version of this, where the server was sending windows nobody had requested.
     */
    void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot) override
    {
        if (!lootGuid.IsCreatureOrVehicle())
            return;

        Creature* creature = ObjectAccessor::GetCreature(*player, lootGuid);
        if (!creature || creature->GetEntry() != g_spoilsEntry)
            return;

        LOG_DEBUG("module.sanctuarypvploot",
            "Spoils opened by {}: gold={} items={} lootType={}",
            player->GetName(), loot ? loot->gold : 0,
            loot ? loot->items.size() : 0, loot ? uint32(loot->loot_type) : 0);
    }

    /*
     * Everything happens here now.
     *
     * The earlier version had to wait for the victim to release, because it needed their
     * corpse to exist before it could be turned into loot. The sack needs nothing but a
     * position, so the killer can take their spoils immediately and the victim is left to
     * run back to a corpse that was never disturbed.
     */
    void OnPlayerPVPKill(Player* killer, Player* victim) override
    {
        /*
         * Nothing to explain when the module is off.
         *
         * The reason exists to tell a game master why a kill that should have produced
         * spoils did not. Switched off, no kill should - so reporting it turned every
         * single death into a line of chat for anyone with game master security.
         */
        if (!g_enabled)
            return;

        if (char const* reason = DeclineReason(killer, victim))
        {
            ReportDecline(killer, victim, reason);
            return;
        }

        // --- coin ---------------------------------------------------------
        uint32 const purse = victim->GetMoney();
        uint32 const gold = uint32(purse * (g_goldPercent / 100.0f));

        // --- one item, rarely ---------------------------------------------
        std::vector<TakenItem> taken;

        std::vector<Item*> eligible;
        CollectEligibleItems(victim, eligible);

        // Shuffled so the roll is not biased towards whatever sits in the first bag slot.
        Acore::Containers::RandomShuffle(eligible);

        for (Item* item : eligible)
        {
            if (taken.size() >= g_maxItems)
                break;

            if (!roll_chance_f(g_itemChance))
                continue;

            TakenItem entry;
            entry.ItemId = item->GetEntry();
            entry.Count = item->GetCount();
            entry.RandomPropertyId = item->GetItemRandomPropertyId();
            entry.RandomSuffix = item->GetItemSuffixFactor();

            taken.push_back(entry);
        }

        if (gold == 0 && taken.empty())
        {
            // Distinct from the gates above, and the distinction is the whole point: this
            // one means the rules allowed it and the roll simply came up empty.
            ReportDecline(killer, victim, "nothing worth taking");
            return;
        }

        // Only taken from the victim once the sack really exists. Checking the result is
        // what makes that true - the drop can fail, and an unchecked call here charged the
        // victim for a sack that was never created.
        if (!DropSpoils(killer, victim, gold, taken))
        {
            ReportDecline(killer, victim, "the spoils sack could not be created");
            return;
        }

        if (gold > 0)
            victim->ModifyMoney(-int32(gold), false);

        for (Item* item : eligible)
        {
            for (TakenItem const& entry : taken)
            {
                if (item->GetEntry() == entry.ItemId && item->GetCount() == entry.Count)
                {
                    victim->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                    break;
                }
            }
        }

        g_recentKills[PairKey(killer->GetGUID(), victim->GetGUID())] = Now();

        TellPlayers(killer, victim, gold, taken.size());
    }
};

class sanctuary_pvploot_worldscript : public WorldScript
{
public:
    sanctuary_pvploot_worldscript() : WorldScript("sanctuary_pvploot_worldscript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE
        }) { }

    /*
     * Validated here rather than in OnAfterConfigLoad, which runs before
     * ObjectMgr::LoadGameObjectTemplate: the store is still empty at config time, so the
     * check would fail against a perfectly good realm.
     */
    void OnStartup() override
    {
        ValidateSpoilsTemplate();
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Enable", true);
        g_announce = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Announce", true);
        g_explainToGameMasters = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.ExplainToGameMasters", true);

        g_goldPercent = std::clamp(sConfigMgr->GetOption<float>("SanctuaryPvpLoot.Gold.Percent", 10.0f), 0.0f, 100.0f);
        g_itemChance = std::clamp(sConfigMgr->GetOption<float>("SanctuaryPvpLoot.Item.Chance", 3.0f), 0.0f, 100.0f);
        g_maxItems = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.Item.MaxPerKill", 1);

        g_allowInBattleground = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Allow.Battleground", false);
        g_allowInArena = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Allow.Arena", false);
        g_allowInDuel = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Allow.Duel", false);
        g_allowInSanctuary = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Allow.Sanctuary", false);
        g_allowGameMasters = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Allow.GameMasters", false);
        g_allowSameAccount = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Allow.SameAccount", false);

        g_minLevel = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.MinLevel", 10);
        g_maxLevelGap = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.MaxLevelGap", 0);
        g_repeatCooldownSeconds = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.RepeatCooldownSeconds", 900);
        g_lootableSeconds = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.LootableSeconds", 300);
        g_spoilsEntry = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.SpoilsEntry", 990001);

        g_takeSoulbound = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Item.TakeSoulbound", false);
        g_takeEquipped = sConfigMgr->GetOption<bool>("SanctuaryPvpLoot.Item.TakeEquipped", false);
        g_maxItemQuality = sConfigMgr->GetOption<uint32>("SanctuaryPvpLoot.Item.MaxQuality", uint32(ITEM_QUALITY_EPIC));

        LOG_INFO("module.sanctuarypvploot", "Sanctuary PvP loot {}: {}% of coin, {}% chance of an item.",
            g_enabled ? "enabled" : "disabled", g_goldPercent, g_itemChance);
    }

    /// Forgets kill cooldowns and abandoned pending loot, so neither grows without bound.
    void OnUpdate(uint32 diff) override
    {
        _sinceSweep += diff;
        if (_sinceSweep < 60000)
            return;

        _sinceSweep = 0;
        uint32 const now = Now();

        for (auto itr = g_recentKills.begin(); itr != g_recentKills.end(); )
            itr = (now - itr->second > g_repeatCooldownSeconds) ? g_recentKills.erase(itr) : std::next(itr);
    }

private:
    uint32 _sinceSweep = 0;
};

void AddSC_sanctuary_pvploot_scripts()
{
    new sanctuary_pvploot_playerscript();
    new sanctuary_pvploot_worldscript();
}
