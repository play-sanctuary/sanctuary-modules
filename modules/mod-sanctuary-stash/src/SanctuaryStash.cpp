/*
 * mod-sanctuary-stash
 *
 * Strongboxes: shared containers standing in the world, each opened by whoever is carrying
 * its key. A thieves' cache, the watch's evidence locker, a merchant's lockup behind the
 * counter - anything several people are meant to reach into and nobody is meant to own.
 *
 * **The guild bank cannot be borrowed for this.** Every guild-bank opcode begins with
 * `GetPlayer()->GetGuild()` and answers ERR_GUILD_PLAYER_NOT_IN_GUILD without one, and
 * AzerothCore membership is exclusive - so keying that window would mean throwing people
 * out of their real guilds to open a box. The window here is an addon's; the storage is
 * this module's.
 *
 * **A strongbox holds real items.** Not entries and counts - the actual `item_instance`
 * rows, taken out of the depositor's bags with their owner cleared, exactly as
 * Guild::BankTab does it. Entry-and-count storage would be a great deal simpler and quietly
 * destructive: an enchanted sword would come back plain, a half-charged wand full, a
 * battered shield pristine. A strongbox is not a laundry.
 *
 * **The key is an item entry, and possession is the whole authority.** Any item can be a
 * key, including a stock retail one that costs no client patch. A key can be copied, lost,
 * taken off a body or sold to a fence, which is the point - the same reasoning as the
 * writs and the shackles.
 *
 * **The addon is not trusted.** It says "take slot 4" and nothing more; every rule - that
 * the box exists, that the player is standing at it, that they hold its key, that the slot
 * holds what they think - is checked here, on the packet, every time. An addon can be
 * edited by anyone who has it.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "CommandScript.h"
#include "AllCreatureScript.h"
#include "CreatureScript.h"
#include "ItemScript.h"
#include "ScriptedGossip.h"
#include "AllSpellScript.h"
#include "SpellInfo.h"
#include "GameObjectScript.h"
#include "Map.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    bool g_enabled = true;
    float g_range = 0.5f;
    std::string g_prefix = "SSTASH";

    uint8 const MAX_SLOTS = 98;
    uint32 g_objectEntry = 990100;   ///> the gameobject_template a placed box spawns
    uint32 g_copyPrice = 10000;      ///> copper the locksmith charges for a duplicate
    uint32 g_locksmithText = 990200; ///> the npc_text he greets you with
    uint32 g_openSpell = 81011;      ///> the two second cast that works the lid open

    struct Stash
    {
        std::string name = "Strongbox";
        uint32 keyItem = 0;                          ///> 0 means it is not locked at all
        uint32 lastKey = 0;                          ///> what it was locked with, so it can be re-locked
        uint8 slots = 40;
        uint32 money = 0;                            ///> copper, as everywhere else
        bool mourned = false;                        ///> its object has gone and we have said so
        std::unordered_map<uint8, Item*> items;      ///> slot -> the real item standing in it
    };

    /// Keyed by the gameobject's SPAWN guid, so a dozen boxes can share one model and still
    /// be a dozen different strongboxes with a dozen different keys.
    std::unordered_map<ObjectGuid::LowType, Stash> g_stashes;

    /// Who currently has which box open. Client messages name no box; this does, so a
    /// forged packet cannot reach into one the player never opened.
    std::unordered_map<ObjectGuid::LowType, ObjectGuid::LowType> g_open;

    /// Who is part way through the two second cast, and which box they clicked.
    std::unordered_map<ObjectGuid::LowType, ObjectGuid::LowType> g_pending;

    /*
     * Which physical key opens which box: item_instance guid -> stash.
     *
     * Boxes used to be locked to an item ENTRY, which meant two boxes with the same entry
     * shared a key - and since entries cannot be made at runtime, the realm could hold only
     * as many distinguishable locks as the client patch shipped keys. Fifteen.
     *
     * Binding the instance instead removes the ceiling: one entry serves any number of
     * boxes, because it is this particular key that was cut for that particular lock. The
     * entry now only decides what a key looks like.
     */
    std::unordered_map<ObjectGuid::LowType, ObjectGuid::LowType> g_keys;

    /*
     * Who is running the addon, and who has a locksmith's window open.
     *
     * The gossip list is kept for anybody without the addon - it is clumsy but it works for
     * somebody who has installed nothing, which is the same reason the notice board keeps
     * its gossip. So the trainer has to know which of the two to offer, and the only honest
     * way to know is to be told at login.
     */
    std::unordered_set<ObjectGuid::LowType> g_hasAddon;
    std::unordered_map<ObjectGuid::LowType, ObjectGuid> g_atLocksmith;

    Stash* Find(ObjectGuid::LowType guid)
    {
        auto it = g_stashes.find(guid);
        return it == g_stashes.end() ? nullptr : &it->second;
    }

    void Tell(Player* player, std::string const& what)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).PSendSysMessage("{}", what);
    }

    void SendAddonPacket(Player* player, std::string const& payload)
    {
        if (!player || !player->GetSession())
            return;

        // 3.3.5a carries addon traffic as "PREFIX\tBODY" inside a whisper to self.
        std::string message = g_prefix + "\t" + payload;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
        player->GetSession()->SendPacket(&data);
    }

    /// The object a strongbox lives in, found by its spawn id.
    GameObject* FindBox(Player* player, ObjectGuid::LowType guid)
    {
        Map* map = player ? player->GetMap() : nullptr;

        if (!map)
            return nullptr;

        auto bounds = map->GetGameObjectBySpawnIdStore().equal_range(guid);

        return bounds.first != bounds.second ? bounds.first->second : nullptr;
    }

    /*
     * The lid.
     *
     * A chest model carries its own open and closed states, so this is a GoState change
     * rather than an animation played at anybody - which means everyone nearby sees it,
     * including people who cannot open the box themselves. That is the point: a strongbox
     * standing open is a thing worth noticing from across the room.
     *
     * It shuts only when the last person using it walks away. Two people can be in the same
     * box at once, and the first to leave should not close the lid in the other's face.
     */
    void SetLid(Player* player, ObjectGuid::LowType guid, bool open)
    {
        if (!open)
            for (auto const& who : g_open)
                if (who.second == guid)
                    return;                          // somebody is still in it

        if (GameObject* box = FindBox(player, guid))
            box->SetGoState(open ? GO_STATE_ACTIVE : GO_STATE_READY);
    }

    /*
     * Whether a strongbox still has an object in the world to open.
     *
     * Asked of ObjectMgr rather than of the map, because a map lookup only finds objects on
     * the asker's own continent and only while that grid is loaded - so a perfectly healthy
     * box in Orgrimmar would read as missing to somebody standing in Stormwind. GameObject::
     * DeleteFromDB calls DeleteGOData, and a grid unload does not, so this is the one test
     * that tells a deleted spawn from an unloaded one.
     */
    bool HasObject(ObjectGuid::LowType guid)
    {
        return sObjectMgr->GetGameObjectData(guid) != nullptr;
    }

    /// The box this particular key was cut for, or 0.
    ObjectGuid::LowType BoxFor(Item const* key)
    {
        if (!key)
            return 0;

        auto it = g_keys.find(key->GetGUID().GetCounter());
        return it == g_keys.end() ? 0 : it->second;
    }

    /*
     * Whether this player is carrying a key for this box.
     *
     * A scan of the bags rather than HasItemCount, because the question is no longer "do you
     * own one of these" but "is one of the things you are carrying the key to this lock".
     * Bags only - a key in the bank opens nothing, which is the point of putting it there.
     */
    Item* CarriedKeyFor(Player* player, ObjectGuid::LowType guid)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* held = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (BoxFor(held) == guid)
                    return held;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* held = bag->GetItemByPos(uint8(slot)))
                        if (BoxFor(held) == guid)
                            return held;

        return nullptr;
    }

    bool CarriesKeyFor(Player* player, ObjectGuid::LowType guid)
    {
        return CarriedKeyFor(player, guid) != nullptr;
    }

    /// Cuts a key for a box and hands it over. The entry is only its appearance.
    Item* CutKey(Player* player, ObjectGuid::LowType guid, uint32 entry)
    {
        ItemPosCountVec dest;

        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1) != EQUIP_ERR_OK)
            return nullptr;

        Item* key = player->StoreNewItem(dest, entry, true);

        if (!key)
            return nullptr;

        g_keys[key->GetGUID().GetCounter()] = guid;

        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_stash_key` (`item_guid`, `stash`) VALUES ({}, {})",
            key->GetGUID().GetCounter(), guid);

        return key;
    }

    /*
     * Everything a player must satisfy to touch a box, checked on every message rather than
     * once when it opened. A window left open while its owner walks away - or is killed,
     * or has the key taken off them - must stop working the moment any of that is true.
     */
    Stash* Reach(Player* player, char const** why)
    {
        *why = nullptr;

        auto open = g_open.find(player->GetGUID().GetCounter());

        if (open == g_open.end())
            return nullptr;                          // nothing open; say nothing

        Stash* stash = Find(open->second);

        if (!stash)
        {
            *why = "That strongbox is no longer there.";
            return nullptr;
        }

        /*
         * The box itself, found by its spawn id.
         *
         * This was written once as a GetGameObject on a guid built from the spawn id, with a
         * FindNearestGameObject fallback, and the result was assigned and never read - so
         * there was no range check at all. A window stayed open and answering from anywhere
         * in the world. The spawn-id store is the correct lookup and the distance is checked
         * against what it returns.
         */
        GameObject* box = FindBox(player, open->second);

        if (!box)
        {
            *why = "That strongbox is no longer there.";
            return nullptr;
        }

        if (!player->IsWithinDistInMap(box, g_range))
        {
            *why = "You have walked away from the strongbox.";
            return nullptr;
        }

        if (!player->IsAlive())
        {
            *why = "Not while you are dead.";
            return nullptr;
        }

        if (stash->keyItem && !CarriesKeyFor(player, open->second))
        {
            *why = "You no longer have the key.";
            return nullptr;
        }

        return stash;
    }

    /*
     * Whether the player is still standing at the locksmith whose bench they have open.
     *
     * The same shape as Reach, and for the same reason: the range was only ever tested at
     * the moment a copy was paid for, so walking off mid-job left the window sitting there
     * looking like a locksmith. It would refuse the copy - but only once you had tried.
     *
     * `complain` is what separates the two callers. Somebody who pressed the button is owed
     * a reason; somebody who simply walked away is not, and a line of chat every time you
     * leave a trainer would be noise.
     */
    bool StillAtLocksmith(Player* player, bool complain)
    {
        auto at = g_atLocksmith.find(player->GetGUID().GetCounter());

        if (at == g_atLocksmith.end())
            return false;

        Creature* locksmith = ObjectAccessor::GetCreature(*player, at->second);

        if (locksmith && player->IsWithinDistInMap(locksmith, INTERACTION_DISTANCE))
            return true;

        if (complain)
            Tell(player, "You have walked away from the locksmith.");

        g_atLocksmith.erase(at);
        SendAddonPacket(player, "COPYSHUT");
        return false;
    }

    void SendContents(Player* player, ObjectGuid::LowType guid, Stash const& stash)
    {
        std::ostringstream open;
        open << "OPEN " << guid << " " << uint32(stash.slots) << " " << stash.name;
        SendAddonPacket(player, open.str());

        for (auto const& held : stash.items)
        {
            if (!held.second)
                continue;

            std::ostringstream line;
            line << "SLOT " << uint32(held.first) << " " << held.second->GetEntry()
                 << " " << held.second->GetCount();
            SendAddonPacket(player, line.str());
        }

        std::ostringstream lock;
        lock << "LOCKED " << (stash.keyItem ? 1 : 0) << " "
             << (CarriesKeyFor(player, guid) ? 1 : 0);
        SendAddonPacket(player, lock.str());

        SendAddonPacket(player, "MONEY " + std::to_string(stash.money));
        SendAddonPacket(player, "DONE");
    }

    /*
     * Taking something out.
     *
     * The row goes first and the item second, inside one transaction: an item handed to a
     * player while its stash row survives would be in two places at once, which is the only
     * way a container like this can mint items.
     */
    void Withdraw(Player* player, ObjectGuid::LowType guid, Stash& stash, uint8 slot)
    {
        auto held = stash.items.find(slot);

        if (held == stash.items.end() || !held->second)
            return Tell(player, "There is nothing in that slot.");

        Item* item = held->second;

        ItemPosCountVec dest;
        InventoryResult space = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);

        if (space != EQUIP_ERR_OK)
        {
            player->SendEquipError(space, item, nullptr);
            return;
        }

        stash.items.erase(slot);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM `sanctuary_stash_item` WHERE `stash` = {} AND `slot` = {}",
            guid, uint32(slot));

        // StoreItem sets the owner and writes the inventory row, so the item stops being
        // ownerless in the same breath as it stops being in the box.
        player->StoreItem(dest, item, true);
        player->SaveInventoryAndGoldToDB(trans);

        CharacterDatabase.CommitTransaction(trans);

        SendAddonPacket(player, "TAKEN " + std::to_string(slot));

        LOG_INFO("module.sanctuarystash", "{} took {} x{} out of strongbox {}.",
            player->GetName(), item->GetEntry(), item->GetCount(), guid);
    }

    void SaveMoney(ObjectGuid::LowType guid, Stash const& stash)
    {
        CharacterDatabase.Execute(
            "REPLACE INTO `sanctuary_stash_money` (`stash`, `money`) VALUES ({}, {})",
            guid, stash.money);
    }

    /*
     * Coin in, coin out.
     *
     * Both directions are one step on the server - the player's purse and the box's total
     * move together or not at all - so there is no window in which the money is in both
     * places or neither. The addon proposes an amount and nothing more; whether the player
     * has it, and whether the box can hold it, is decided here.
     */
    void DepositMoney(Player* player, ObjectGuid::LowType guid, Stash& stash, uint32 amount)
    {
        if (!amount)
            return;

        if (player->GetMoney() < amount)
            return Tell(player, "You do not have that much.");

        // The core caps a purse at MAX_MONEY_AMOUNT, so a box may hold no more than a
        // player could carry out of it. Refused rather than clamped: silently taking less
        // than was offered is how coin goes missing.
        if (stash.money > uint32(MAX_MONEY_AMOUNT) - amount)
            return Tell(player, "The strongbox will not hold that much more.");

        player->ModifyMoney(-int32(amount));
        stash.money += amount;
        SaveMoney(guid, stash);

        SendAddonPacket(player, "MONEY " + std::to_string(stash.money));

        LOG_INFO("module.sanctuarystash", "{} put {} copper into strongbox {}.",
            player->GetName(), amount, guid);
    }

    void WithdrawMoney(Player* player, ObjectGuid::LowType guid, Stash& stash, uint32 amount)
    {
        if (!amount)
            return;

        if (stash.money < amount)
            return Tell(player, "There is not that much in it.");

        if (player->GetMoney() > uint32(MAX_MONEY_AMOUNT) - amount)
            return Tell(player, "You cannot carry that much.");

        stash.money -= amount;
        player->ModifyMoney(int32(amount));
        SaveMoney(guid, stash);

        SendAddonPacket(player, "MONEY " + std::to_string(stash.money));

        LOG_INFO("module.sanctuarystash", "{} took {} copper out of strongbox {}.",
            player->GetName(), amount, guid);
    }

    /*
     * From the client's numbering to the core's.
     *
     * The addon can only report what the client told it, and the two count bags and slots
     * differently:
     *
     *     client bag 0, slots 1-16     the backpack   ->  INVENTORY_SLOT_BAG_0, slots 23-38
     *     client bags 1-4, slots 1-N   worn bags      ->  bags 19-22, slots 0-N-1
     *
     * Passing the client's numbers straight through is what answered "You are not holding
     * that" to everything: "bag 0, slot 1" is a real position in both schemes and means two
     * entirely different things in each.
     */
    bool ToCoreSlot(Player* player, uint32 clientBag, uint32 clientSlot, uint8& bag, uint8& slot)
    {
        if (!clientSlot)
            return false;                            // the client counts slots from one

        if (clientBag == 0)
        {
            if (clientSlot > uint32(INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START))
                return false;

            bag = INVENTORY_SLOT_BAG_0;
            slot = uint8(INVENTORY_SLOT_ITEM_START + clientSlot - 1);
            return true;
        }

        if (clientBag > uint32(INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START))
            return false;

        bag = uint8(INVENTORY_SLOT_BAG_START + clientBag - 1);

        Bag* worn = player->GetBagByPos(bag);

        if (!worn || clientSlot > worn->GetBagSize())
            return false;

        slot = uint8(clientSlot - 1);
        return true;
    }

    /*
     * Putting something in. The mirror of the above, and the same transaction argument.
     */
    void Deposit(Player* player, ObjectGuid::LowType guid, Stash& stash, uint8 bag, uint8 bagSlot, uint8 slot)
    {
        if (slot >= stash.slots)
            return Tell(player, "That is not a slot in this strongbox.");

        if (stash.items.count(slot))
            return Tell(player, "Something is in that slot already.");

        Item* item = player->GetItemByPos(bag, bagSlot);

        if (!item)
            return Tell(player, "You are not holding that.");

        if (item->IsBag() && !((Bag*)item)->IsEmpty())
            return Tell(player, "Empty the bag first.");

        if (item->IsSoulBound())
            return Tell(player, "Soulbound things cannot be left for somebody else.");

        if (item->IsEquipped())
            return Tell(player, "Take it off first.");

        uint32 const entry = item->GetEntry();
        uint32 const count = item->GetCount();

        player->MoveItemFromInventory(bag, bagSlot, true);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        item->DeleteFromInventoryDB(trans);

        // Ownerless, and saved standing on its own - the same three lines Guild::BankTab
        // uses, and the reason a deposited item keeps its enchant, charges and durability.
        item->SetGuidValue(ITEM_FIELD_CONTAINED, ObjectGuid::Empty);
        item->SetGuidValue(ITEM_FIELD_OWNER, ObjectGuid::Empty);
        item->FSetState(ITEM_NEW);
        item->SaveToDB(trans);

        trans->Append("REPLACE INTO `sanctuary_stash_item` (`stash`, `slot`, `item_guid`) VALUES ({}, {}, {})",
            guid, uint32(slot), item->GetGUID().GetCounter());

        CharacterDatabase.CommitTransaction(trans);

        stash.items[slot] = item;

        std::ostringstream line;
        line << "SLOT " << uint32(slot) << " " << entry << " " << count;
        SendAddonPacket(player, line.str());

        LOG_INFO("module.sanctuarystash", "{} put {} x{} into strongbox {}.",
            player->GetName(), entry, count, guid);
    }
}

class sanctuary_stash : public GameObjectScript
{
public:
    sanctuary_stash() : GameObjectScript("sanctuary_stash") { }

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        if (!g_enabled || !player || !go)
            return false;

        Stash* stash = Find(go->GetSpawnId());

        if (!stash)
        {
            Tell(player, "This strongbox has no lock and no contents. It is not finished.");
            return true;
        }

        /*
         * The same reach that keeps a box open is required to open it.
         *
         * Without this the two disagree: the core lets a QUESTGIVER-type object be clicked
         * from its own interaction distance, which is further than SanctuaryStash.Range, so
         * a player could open a box from five yards and be told a second later that they had
         * walked away from it. Refusing here says the same thing at the moment it is true.
         */
        if (!player->IsWithinDistInMap(go, g_range))
        {
            Tell(player, "You are not close enough to reach into it.");
            return true;
        }

        if (stash->keyItem && !CarriesKeyFor(player, go->GetSpawnId()))
        {
            Tell(player, "|cffff4040It is locked.|r Whoever has its key can open it.");
            return true;
        }

        /*
         * Two seconds of work, rather than a window appearing the instant it is clicked.
         *
         * The spell carries the animation as well as the delay - visual 180 is what the
         * client already plays for chests and lockboxes - and it carries the interrupt, so
         * walking off cancels it without this module watching for that. Everything is
         * checked again when the cast lands, because two seconds is long enough to step
         * away, be killed, or have the key taken.
         */
        g_pending[player->GetGUID().GetCounter()] = go->GetSpawnId();
        player->CastSpell(player, g_openSpell, false);

        return true;                                  // no gossip menu; the window is the UI
    }
};

class sanctuary_stash_playerscript : public PlayerScript
{
public:
    sanctuary_stash_playerscript() : PlayerScript("sanctuary_stash_playerscript",
        {
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
        }) { }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        g_hasAddon.erase(player->GetGUID().GetCounter());
        g_atLocksmith.erase(player->GetGUID().GetCounter());

        ObjectGuid::LowType const was = g_open[player->GetGUID().GetCounter()];
        g_open.erase(player->GetGUID().GetCounter());
        SetLid(player, was, false);
    }

    /// Addon traffic arrives as a whisper the player sends to themselves.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !g_enabled)
            return true;

        std::string const marker = g_prefix + "\t";

        // Every registered PlayerScript sees this message and the first false swallows it,
        // so anything that is not ours has to be passed along untouched.
        if (msg.rfind(marker, 0) != 0)
            return true;

        std::istringstream body(msg.substr(marker.size()));
        std::string verb;
        body >> verb;

        /*
         * The GM panel asking whether it may exist.
         *
         * Answered before the "must have a box open" gate below, because the panel is asking
         * at login when nothing is open. The reply is the whole permission check as far as
         * the client is concerned - but only as far as the client: every .stash command is
         * RBAC-gated on its own, so a player who edits the addon to show itself still cannot
         * place anything.
         */
        if (verb == "HELLO")
        {
            g_hasAddon.insert(player->GetGUID().GetCounter());
            return false;
        }

        /*
         * Copying the key that was put into the locksmith's window.
         *
         * Answered before the "must have a box open" gate: this happens standing at a
         * trainer, nowhere near a strongbox, and the box in question is whichever one the
         * key was cut for rather than one underfoot.
         *
         * The window is only an interface. It says which bag slot the key is in and nothing
         * more - that the item there is really a key, that it opens something, that its owner
         * can pay and has room, and that they are still standing at the locksmith who offered
         * are all decided here.
         */
        /*
         * The bench's heartbeat, asked once a second while its window is open. Goes through
         * the same test as the purchase, so the two cannot disagree about how far is too far.
         */
        if (verb == "ATSMITH")
        {
            StillAtLocksmith(player, false);
            return false;
        }

        if (verb == "COPYKEY")
        {
            if (!StillAtLocksmith(player, true))
                return false;

            uint32 bag = 0, bagSlot = 0;
            std::istringstream where(msg.substr(marker.size() + verb.size()));
            where >> bag >> bagSlot;

            uint8 coreBag = 0, coreSlot = 0;

            if (!ToCoreSlot(player, bag, bagSlot, coreBag, coreSlot))
            {
                Tell(player, "You are not holding that.");
                return false;
            }

            Item* key = player->GetItemByPos(coreBag, coreSlot);
            ObjectGuid::LowType const box = BoxFor(key);
            Stash const* stash = Find(box);

            if (!stash)
            {
                Tell(player, "He turns it over and hands it back. It opens nothing.");
                return false;
            }

            if (player->GetMoney() < g_copyPrice)
            {
                Tell(player, "You cannot afford it.");
                return false;
            }

            if (!CutKey(player, box, key->GetEntry()))
            {
                Tell(player, "You have nowhere to put it.");
                return false;
            }

            player->ModifyMoney(-int32(g_copyPrice));

            Tell(player, Acore::StringFormat(
                "He works for a minute and hands you a second key for |cffffffff{}|r.", stash->name));

            SendAddonPacket(player, "COPIED");

            LOG_INFO("module.sanctuarystash", "{} copied a key for strongbox {} at {} copper.",
                player->GetName(), box, g_copyPrice);

            return false;
        }

        if (verb == "COPYSHUT")
        {
            g_atLocksmith.erase(player->GetGUID().GetCounter());
            return false;
        }

        if (verb == "GMHELLO")
        {
            if (player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
                SendAddonPacket(player, "GMOK");

            return false;
        }

        /*
         * Turning the key, from the minimap button.
         *
         * Answered before the "must have a box open" gate, because the button is on the
         * minimap and not on the window - somebody standing at a locked box they hold the key
         * to has nothing open yet, and that is exactly when they want to unlock it. So the
         * box is resolved by where they are standing rather than by what they have open.
         *
         * The only gate is the key in their hand. This is the first thing a player can do to
         * a box rather than to its contents: leave a cache unlocked for your crew and anyone
         * may reach in, turn the key and only keyholders can.
         *
         * Locking uses the entry of the key being turned rather than anything remembered, so
         * the lock and the key that fits it cannot drift apart and nothing has to be written
         * down to survive a restart.
         */
        if (verb == "LOCK")
        {
            for (auto& box : g_stashes)
            {
                GameObject* object = FindBox(player, box.first);

                if (!object || !player->IsWithinDistInMap(object, g_range))
                    continue;

                Item* key = CarriedKeyFor(player, box.first);

                if (!key)
                {
                    Tell(player, "You are not carrying its key.");
                    return false;
                }

                box.second.keyItem = box.second.keyItem ? 0 : key->GetEntry();

                WorldDatabase.Execute("UPDATE `sanctuary_stash` SET `keyitem` = {} WHERE `guid` = {}",
                    box.second.keyItem, box.first);

                Tell(player, box.second.keyItem
                    ? "You turn the key. It is locked."
                    : "You turn the key. It is open to anyone now.");

                std::ostringstream lock;
                lock << "LOCKED " << (box.second.keyItem ? 1 : 0) << " 1";
                SendAddonPacket(player, lock.str());

                LOG_INFO("module.sanctuarystash", "{} {} strongbox {}.",
                    player->GetName(), box.second.keyItem ? "locked" : "unlocked", box.first);

                return false;
            }

            Tell(player, "There is no strongbox within reach.");
            return false;
        }

        if (verb == "CLOSE")
        {
            ObjectGuid::LowType const was = g_open[player->GetGUID().GetCounter()];
            g_open.erase(player->GetGUID().GetCounter());
            SetLid(player, was, false);
            return false;
        }

        char const* why = nullptr;
        Stash* stash = Reach(player, &why);

        if (!stash)
        {
            if (why)
            {
                ObjectGuid::LowType const was = g_open[player->GetGUID().GetCounter()];
                Tell(player, why);
                g_open.erase(player->GetGUID().GetCounter());
                SetLid(player, was, false);
                SendAddonPacket(player, "SHUT");
            }
            return false;
        }

        ObjectGuid::LowType const guid = g_open[player->GetGUID().GetCounter()];

        if (verb == "SYNC")
        {
            SendContents(player, guid, *stash);
        }
        else if (verb == "TAKE")
        {
            uint32 slot = MAX_SLOTS;
            body >> slot;

            if (slot < stash->slots)
                Withdraw(player, guid, *stash, uint8(slot));
        }
        else if (verb == "PUTMONEY" || verb == "TAKEMONEY")
        {
            uint32 amount = 0;
            body >> amount;

            if (verb == "PUTMONEY")
                DepositMoney(player, guid, *stash, amount);
            else
                WithdrawMoney(player, guid, *stash, amount);
        }
        else if (verb == "PUT")
        {
            uint32 bag = 0, bagSlot = 0, slot = MAX_SLOTS;
            body >> bag >> bagSlot >> slot;

            uint8 coreBag = 0, coreSlot = 0;

            if (slot >= stash->slots)
                return false;

            if (!ToCoreSlot(player, bag, bagSlot, coreBag, coreSlot))
                Tell(player, "You are not holding that.");
            else
                Deposit(player, guid, *stash, coreBag, coreSlot, uint8(slot));
        }

        return false;
    }
};

class sanctuary_stash_worldscript : public WorldScript
{
    uint32 _since = 0;                               ///> throttles the objectless-box sweep

public:
    sanctuary_stash_worldscript() : WorldScript("sanctuary_stash_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    /*
     * Noticing when a strongbox loses its object.
     *
     * `.gobject delete` knows nothing about strongboxes, and there is no hook for it worth
     * using - OnGameObjectRemoveWorld fires for grid unloads and shutdowns as well, so it
     * cannot tell a deletion from a continent going quiet. What CAN tell them apart is
     * ObjectMgr: DeleteFromDB clears the spawn data and an unload does not.
     *
     * So this asks, every few seconds, rather than trying to be told. A box that loses its
     * object still holds its items and its keys still exist - it is unreachable rather than
     * gone, and `.stash remove` will not delete it while it holds anything - so the only
     * thing that must happen is that somebody finds out. It is said once per box.
     */
    void OnUpdate(uint32 diff) override
    {
        if (!g_enabled || g_stashes.empty())
            return;

        _since += diff;

        if (_since < 5000)
            return;

        _since = 0;

        for (auto& box : g_stashes)
        {
            if (HasObject(box.first))
            {
                box.second.mourned = false;          // rebuilt, so it may be said again
                continue;
            }

            if (box.second.mourned)
                continue;

            box.second.mourned = true;

            LOG_ERROR("module.sanctuarystash",
                "Strongbox {} \"{}\" has lost its object - deleted outside .stash remove. It "
                "still holds {} item(s) and {} copper, and its keys still exist. Nothing can "
                "reach it until `.stash rebuild {}` puts an object back.",
                box.first, box.second.name, uint32(box.second.items.size()),
                box.second.money, box.first);
        }
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryStash.Enable", true);
        g_range = std::clamp(sConfigMgr->GetOption<float>("SanctuaryStash.Range", 0.5f), 0.25f, 30.0f);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryStash.Addon.Prefix", "SSTASH");
        g_objectEntry = sConfigMgr->GetOption<uint32>("SanctuaryStash.ObjectEntry", 990100);
        g_copyPrice = sConfigMgr->GetOption<uint32>("SanctuaryStash.Locksmith.Price", 10000);
        g_locksmithText = sConfigMgr->GetOption<uint32>("SanctuaryStash.Locksmith.TextId", 990200);
        g_openSpell = sConfigMgr->GetOption<uint32>("SanctuaryStash.OpenSpell", 81011);
    }

    void OnStartup() override
    {
        if (!g_enabled)
            return;

        Load();
    }

    /*
     * Every strongbox and everything in one, read once at startup.
     *
     * The items are built the way the guild bank builds its own: NewItemOrBag, then
     * LoadFromDB with an empty owner. The column order of the SELECT is not free - Item::
     * LoadFromDB indexes its fields positionally, so creatorGuid through text must appear
     * exactly in that order and the first four columns here are skipped past.
     */
    static void Load()
    {
        g_stashes.clear();

        QueryResult boxes = WorldDatabase.Query(
            "SELECT `guid`, `name`, `keyitem`, `slots` FROM `sanctuary_stash`");

        if (boxes)
        {
            do
            {
                Field* fields = boxes->Fetch();

                ObjectGuid::LowType guid = fields[0].Get<uint32>();
                Stash& stash = g_stashes[guid];
                stash.name = fields[1].Get<std::string>();
                stash.keyItem = fields[2].Get<uint32>();
                stash.slots = std::clamp<uint8>(fields[3].Get<uint8>(), 1, MAX_SLOTS);
            } while (boxes->NextRow());
        }

        QueryResult purses = CharacterDatabase.Query(
            "SELECT `stash`, `money` FROM `sanctuary_stash_money`");

        if (purses)
        {
            do
            {
                Field* fields = purses->Fetch();

                if (Stash* stash = Find(fields[0].Get<uint32>()))
                    stash->money = fields[1].Get<uint32>();
            } while (purses->NextRow());
        }

        /*
         * Bindings whose key no longer exists go first. Item guids are not normally reused,
         * but a stale row inheriting a recycled one would hand somebody a key they never
         * cut, and this costs a single statement at startup.
         */
        CharacterDatabase.Execute(
            "DELETE k FROM `sanctuary_stash_key` k "
            "LEFT JOIN `item_instance` i ON i.`guid` = k.`item_guid` "
            "WHERE i.`guid` IS NULL");

        g_keys.clear();

        if (QueryResult keys = CharacterDatabase.Query(
            "SELECT `item_guid`, `stash` FROM `sanctuary_stash_key`"))
        {
            do
            {
                Field* fields = keys->Fetch();
                g_keys[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
            } while (keys->NextRow());
        }

        uint32 loaded = 0;
        uint32 orphans = 0;

        QueryResult items = CharacterDatabase.Query(
            "SELECT si.`stash`, si.`slot`, si.`item_guid`, ii.`itemEntry`, "
            "ii.`creatorGuid`, ii.`giftCreatorGuid`, ii.`count`, ii.`duration`, ii.`charges`, "
            "ii.`flags`, ii.`enchantments`, ii.`randomPropertyId`, ii.`durability`, "
            "ii.`playedTime`, ii.`text` "
            "FROM `sanctuary_stash_item` si "
            "JOIN `item_instance` ii ON ii.`guid` = si.`item_guid`");

        if (items)
        {
            do
            {
                Field* fields = items->Fetch();

                ObjectGuid::LowType box = fields[0].Get<uint32>();
                uint8 slot = fields[1].Get<uint8>();
                ObjectGuid::LowType itemGuid = fields[2].Get<uint32>();
                uint32 entry = fields[3].Get<uint32>();

                Stash* stash = Find(box);

                if (!stash || slot >= stash->slots)
                {
                    ++orphans;
                    continue;
                }

                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);

                if (!proto)
                {
                    LOG_ERROR("module.sanctuarystash",
                        "Strongbox {} slot {} holds item {}, entry {}, which no longer exists.",
                        box, uint32(slot), itemGuid, entry);
                    ++orphans;
                    continue;
                }

                Item* item = NewItemOrBag(proto);

                if (!item->LoadFromDB(itemGuid, ObjectGuid::Empty, &fields[4], entry))
                {
                    LOG_ERROR("module.sanctuarystash",
                        "Strongbox {} slot {} holds item {} which will not load.",
                        box, uint32(slot), itemGuid);
                    delete item;
                    ++orphans;
                    continue;
                }

                stash->items[slot] = item;
                ++loaded;
            } while (items->NextRow());
        }

        uint32 copper = 0;
        for (auto const& box : g_stashes)
            copper += box.second.money;

        uint32 objectless = 0;

        for (auto const& box : g_stashes)
            if (!HasObject(box.first))
                ++objectless;

        uint32 dangling = 0;

        for (auto const& key : g_keys)
            if (!Find(key.second))
                ++dangling;

        /*
         * Keys pointing at a strongbox that no longer exists.
         *
         * Reported rather than swept, deliberately. A binding is the only remaining record
         * that a particular key belonged to a particular box, and if a definition went
         * missing by accident that record is what a rebuild would be reconstructed from.
         * Deleting it would tidy away the evidence of the very thing worth investigating.
         */
        if (dangling)
            LOG_ERROR("module.sanctuarystash",
                "{} key(s) are cut for strongboxes that no longer exist. They open nothing. "
                "Their bindings are kept rather than swept, in case a definition went missing "
                "by mistake and wants restoring.", dangling);

        if (objectless)
            LOG_ERROR("module.sanctuarystash",
                "{} strongbox(es) have no object in the world. They still hold their contents "
                "and their keys still exist; `.stash list` names them and `.stash rebuild <guid>` "
                "puts an object back under one.", objectless);

        LOG_INFO("module.sanctuarystash",
            "Sanctuary strongboxes: {} box(es), {} item(s) and {} gold held, {} key(s) cut{}.",
            g_stashes.size(), loaded, copper / 10000, uint32(g_keys.size()),
            orphans ? Acore::StringFormat(", {} unreadable", orphans) : "");
    }
};



/*
 * Using a key tells you what it opens.
 *
 * A key cannot carry this itself: its name and description live in item_template, which is
 * per-ENTRY, so every copy reads identically and two boxes sharing a key could never be told
 * apart by looking at it. Minting one item per strongbox would say it, and would put a
 * permanent item_template row in the database for every box anyone ever places.
 *
 * The alternative was an addon that writes the line onto the tooltip. This is better for the
 * reason the notice board is gossip rather than an addon: it works for somebody who has just
 * arrived and installed nothing. It also cannot go stale - there is no keyring pushed at
 * login to be wrong after the key changes hands, because the answer is computed when asked.
 *
 * Only what this key opens is listed, never the rest, so a key is not a map of the realm.
 */
class sanctuary_stash_key : public ItemScript
{
public:
    sanctuary_stash_key() : ItemScript("sanctuary_stash_key") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!player || !player->GetSession())
            return true;

        /*
         * Clears the item, silently.
         *
         * Something has to answer the use or the client leaves the key greyed out and the
         * player reads it as a cooldown rather than an answer. This used to send
         * EQUIP_ERR_CANT_DO_RIGHT_NOW, which cleared it but printed "You can't do that right
         * now" over a key that had just worked perfectly well.
         *
         * EQUIP_ERR_OK is the same packet with the error byte zero: Player::SendEquipError
         * writes only the result and returns before the item guids, so the client takes it as
         * the use having concluded and prints nothing. The answer is the line below it.
         */
        player->SendEquipError(EQUIP_ERR_OK, item, nullptr);

        ChatHandler handler(player->GetSession());

        // Exact, now that a key is bound to one box rather than to a kind of box. Two keys
        // that look identical can open different strongboxes, and each says which.
        if (Stash const* stash = Find(BoxFor(item)))
            handler.PSendSysMessage("This key opens |cffffffff{}|r.", stash->name);
        else
            handler.PSendSysMessage("This key opens nothing you know of.");

        return true;
    }
};


/*
 * Rogues cut keys.
 *
 * Bring a key, pay, walk away with a second one - which is what makes a key worth owning
 * rather than an administrative fact. Anybody holding one can copy it, so lending a key is
 * a decision, and so is leaving one in a box somebody else can open. A copy is the same item
 * entry as the original and nothing distinguishes the first key from the tenth.
 *
 * This script sits on the rogue trainers WITHOUT taking their menu away. A gossip_menu_option
 * row would have been simpler and does not work: a database option reaches scripts with
 * sender 0 and action = its OptionType, so it cannot be told apart from any other plain line
 * on the same menu, and an OptionType invented to make it unique is hidden by
 * PrepareGossipMenu's `default: canTalk = false`.
 *
 * So the trainer's own menu is built here, by the same call the core would have made, and one
 * line is appended to it. Training, quests and anything else they offer arrive untouched.
 * Our line is the only one carrying our sender, and anything else is handed back to the core.
 *
 * WHY AllCreatureScript RATHER THAN CreatureScript
 *
 * A CreatureScript has to be named in creature_template.ScriptName, and a creature has exactly
 * one of those. Shenthul in Orgrimmar already has npc_shenthul - the AI behind The Shattered
 * Salute - so the migration that attached this to the rogue trainers deliberately skipped
 * him rather than break a quest, and he was the one rogue trainer in the game with no key
 * option. Taking his ScriptName would have removed his GetAI along with it.
 *
 * These hooks run for every creature and, crucially, run BEFORE the ScriptName lookup in
 * ScriptMgr::OnGossipHello - so the line can be added without owning the slot. Nothing of
 * Shenthul's is displaced: npc_shenthul has no gossip handler of its own, only OnQuestAccept
 * and an AI. It also means the module no longer writes to 33 stock creature_template rows,
 * and a future core update that scripts another trainer cannot silently drop them either.
 */
class sanctuary_stash_locksmith : public AllCreatureScript
{
public:
    sanctuary_stash_locksmith() : AllCreatureScript("sanctuary_stash_locksmith") { }

    /*
     * Who cuts keys. The same test the SQL used to run, now asked at the moment somebody
     * opens a conversation instead of being baked into a column - so it is right for any
     * trainer added later without a migration to remember.
     */
    static bool IsLocksmith(Creature* creature)
    {
        CreatureTemplate const* info = creature ? creature->GetCreatureTemplate() : nullptr;

        return info && info->SubName.find("Rogue Trainer") != std::string::npos;
    }

    /// Distinct from the 0 that every database-defined option carries.
    static uint32 constexpr SENDER = 990200;

    /// Every strongbox this player is carrying a key for. One line per box, because two
    /// identical-looking keys may open different locks and both deserve offering.
    static std::vector<ObjectGuid::LowType> KeysCarried(Player* player)
    {
        std::vector<ObjectGuid::LowType> boxes;

        for (auto const& box : g_stashes)
            if (box.second.keyItem && CarriesKeyFor(player, box.first))
                boxes.push_back(box.first);

        return boxes;
    }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!g_enabled || !IsLocksmith(creature))
            return false;

        // The CreatureScript path did this for us; on this hook it has not happened yet.
        ClearGossipMenuFor(player);

        // Their menu, built exactly as the core would have built it, then one line more.
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "I would like to copy a key.", SENDER, 1);

        player->SendPreparedGossip(creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        // Not ours: hand it back, and the core does whatever the trainer would have done.
        // This runs for every creature in the world, so it has to be the cheapest test first.
        if (sender != SENDER || !IsLocksmith(creature))
            return false;

        if (action == 1)
        {
            /*
             * The window, for anybody who has the addon: hand him a key and he copies that
             * key, which is what actually happens at a locksmith. The gossip list below is
             * the same trade described rather than done, kept for people running no addon.
             */
            if (g_hasAddon.count(player->GetGUID().GetCounter()))
            {
                CloseGossipMenuFor(player);
                g_atLocksmith[player->GetGUID().GetCounter()] = creature->GetGUID();
                SendAddonPacket(player, "COPY " + std::to_string(g_copyPrice));
                return true;
            }

            ClearGossipMenuFor(player);

            std::vector<ObjectGuid::LowType> const boxes = KeysCarried(player);

            for (ObjectGuid::LowType box : boxes)
            {
                Stash const* stash = Find(box);
                ItemTemplate const* proto = stash ? sObjectMgr->GetItemTemplate(stash->keyItem) : nullptr;

                if (!proto)
                    continue;

                // The BOX rides in the action, not the item entry: the copy has to be cut
                // for the same lock, and two keys that look alike may open different ones.
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR,
                    "Cut me another key for " + stash->name, SENDER, box,
                    "", g_copyPrice, false);
            }

            if (boxes.empty())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    "(You carry nothing he can copy.)", SENDER, 1);

            SendGossipMenuFor(player, g_locksmithText, creature->GetGUID());
            return true;
        }

        CloseGossipMenuFor(player);

        // Everything is checked again here. The menu was built a moment ago and the player
        // may have spent the money, dropped the key or filled their bags since.
        Stash const* stash = Find(action);

        if (!stash || !stash->keyItem)
            return Tell(player, "He turns it over and hands it back. It opens nothing."), true;

        if (!CarriesKeyFor(player, action))
            return Tell(player, "You are not carrying that key."), true;

        if (player->GetMoney() < g_copyPrice)
            return Tell(player, "You cannot afford it."), true;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(stash->keyItem);

        if (!proto)
            return true;

        // Cut for the same lock, not merely the same shape - which is the whole reason the
        // box rather than the item entry was carried through the menu.
        if (!CutKey(player, action, stash->keyItem))
            return Tell(player, "You have nowhere to put it."), true;

        player->ModifyMoney(-int32(g_copyPrice));

        Tell(player, "He works for a minute and hands you a second " + std::string(proto->Name1) + ".");

        LOG_INFO("module.sanctuarystash", "{} copied a key for strongbox {} at {} copper.",
            player->GetName(), action, g_copyPrice);

        return true;
    }
};


/*
 * The lid comes up when the cast finishes, not when it starts.
 *
 * Spell::cast() runs at the END of a timed cast, which is what makes this the completion
 * hook rather than the beginning of one. Every condition is tested again here: the two
 * seconds are long enough to walk away, to die, or to have the key lifted out of a pocket.
 */
class sanctuary_stash_open_spell : public AllSpellScript
{
public:
    sanctuary_stash_open_spell() : AllSpellScript("sanctuary_stash_open_spell",
        { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!g_enabled || !spellInfo || spellInfo->Id != g_openSpell)
            return;

        Player* player = caster ? caster->ToPlayer() : nullptr;

        if (!player)
            return;

        auto pending = g_pending.find(player->GetGUID().GetCounter());

        if (pending == g_pending.end())
            return;

        ObjectGuid::LowType const guid = pending->second;
        g_pending.erase(pending);

        Stash* stash = Find(guid);
        GameObject* box = stash ? FindBox(player, guid) : nullptr;

        if (!stash || !box)
            return Tell(player, "That strongbox is no longer there.");

        if (!player->IsWithinDistInMap(box, g_range))
            return Tell(player, "You are not close enough to reach into it.");

        if (stash->keyItem && !CarriesKeyFor(player, guid))
            return Tell(player, "|cffff4040It is locked.|r Whoever has its key can open it.");

        g_open[player->GetGUID().GetCounter()] = guid;
        SetLid(player, guid, true);
        SendContents(player, guid, *stash);
    }
};

using namespace Acore::ChatCommands;

/*
 * Placing and keying boxes without leaving the game.
 *
 * All of this was SQL by hand and a restart, because the module reads its definitions once
 * at startup. That is fine for the first box and miserable for the tenth, and the placement
 * instructions in the migration promised a `.stash reload` that did not exist. It does now,
 * and `place` and `key` do not even need it - they register what they create in memory as
 * they go, so a box is usable the moment it is put down.
 *
 * What still cannot be done at runtime is creating a new key ITEM. AzerothCore has 305
 * reload subcommands and item_template is not among them, so item entries are fixed at
 * startup. That is why the keys are a palette shipped in advance rather than made on
 * demand: `key` chooses one, it does not invent one.
 */
class sanctuary_stash_commandscript : public CommandScript
{
public:
    sanctuary_stash_commandscript() : CommandScript("sanctuary_stash_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable stashTable =
        {
            { "place",  HandleStashPlace,  rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "key",    HandleStashKey,    rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "name",   HandleStashName,   rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "remove", HandleStashRemove, rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "rotate", HandleStashRotate, rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "rebuild", HandleStashRebuild, rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::No  },
            { "list",   HandleStashList,   rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes },
            { "reload", HandleStashReload, rbac::RBAC_PERM_COMMAND_MODIFY_FACTION, Console::Yes }
        };

        static ChatCommandTable commandTable = { { "stash", stashTable } };
        return commandTable;
    }

    /*
     * Says something to whoever ran the command, in their chat window.
     *
     * A command issued over the addon channel gets an AddonChannelCommandHandler, whose
     * SendSysMessage returns the text to the ADDON as an `m` packet rather than putting it
     * on screen. So every button that reports rather than acts looked broken - `.stash list`
     * ran perfectly and its entire output went nowhere. A handler built from the session
     * lands in the chat frame whichever way the command arrived.
     */
    static void Say(ChatHandler* handler, std::string const& line)
    {
        if (Player* player = handler->GetPlayer())
            ChatHandler(player->GetSession()).PSendSysMessage("{}", line);
        else
            handler->PSendSysMessage("{}", line);          // the console has no session
    }

    /// The strongbox under the GM's feet, or nothing.
    static Stash* Underfoot(Player* player, ObjectGuid::LowType& guid)
    {
        GameObject* box = player->FindNearestGameObject(g_objectEntry, 20.0f);

        if (!box)
            return nullptr;

        guid = box->GetSpawnId();
        return Find(guid);
    }

    /*
     * A strongbox object where the player is standing. Returns its spawn id, or 0.
     *
     * The delete-and-reload at the end is not tidiness: SaveToDB writes the row, and the
     * object then has to be rebuilt from that row or the copy in memory and the copy in the
     * database drift apart. Lifted from .gobject add, which does the same dance for the same
     * reason.
     */
    static ObjectGuid::LowType SpawnBox(Player* player)
    {
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(g_objectEntry);

        if (!info)
            return 0;

        Map* map = player->GetMap();
        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = player->GetPositionZ();
        float const o = player->GetOrientation();

        GameObject* object = new GameObject();
        ObjectGuid::LowType spawnId = map->GenerateLowGuid<HighGuid::GameObject>();
        G3D::Quat const rotation = G3D::Quat::fromAxisAngleRotation(G3D::Vector3::unitZ(), o);

        if (!object->Create(spawnId, info->entry, map, player->GetPhaseMaskForSpawn(), x, y, z, o, rotation, 0, GO_STATE_READY))
        {
            delete object;
            return 0;
        }

        object->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), player->GetPhaseMaskForSpawn());
        spawnId = object->GetSpawnId();

        delete object;

        object = new GameObject();

        if (!object->LoadGameObjectFromDB(spawnId, map, true))
        {
            delete object;
            return 0;
        }

        sObjectMgr->AddGameobjectToGrid(spawnId, sObjectMgr->GetGameObjectData(spawnId));
        return spawnId;
    }

    /*
     * `.stash place [name]` - a strongbox where you are standing.
     *
     * The spawn half is lifted from .gobject add, including the delete-and-reload dance:
     * SaveToDB writes the row, and the object then has to be built again from that row or
     * the one in memory and the one in the database drift apart.
     */
    static bool HandleStashPlace(ChatHandler* handler, Tail name)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        ObjectGuid::LowType const spawnId = SpawnBox(player);

        if (!spawnId)
        {
            handler->SendErrorMessage("The strongbox could not be placed there.");
            return false;
        }

        std::string label(name);

        if (label.empty())
            label = "Strongbox";

        Stash& stash = g_stashes[spawnId];
        stash.name = label;
        stash.keyItem = 0;
        stash.slots = 40;
        stash.money = 0;
        stash.items.clear();

        WorldDatabase.Execute(
            "REPLACE INTO `sanctuary_stash` (`guid`, `name`, `keyitem`, `slots`) VALUES ({}, '{}', 0, {})",
            spawnId, label, uint32(stash.slots));

        Say(handler, Acore::StringFormat(
            "Strongbox |cffffffff{}|r placed, spawn {}. It is |cffff4040unlocked|r - "
            "give it a key with |cffffffff.stash key <item>|r.", label, spawnId));

        LOG_INFO("module.sanctuarystash", "{} placed strongbox {} \"{}\" on map {}.",
            player->GetName(), spawnId, label, player->GetMapId());

        return true;
    }

    /*
     * `.stash key <item>` locks the box to that item. `.stash key 0` unlocks it. `.stash key`
     * with nothing after it toggles between the two.
     *
     * The toggle exists so the panel's button can honestly say "Lock/Unlock". Re-locking
     * needs to know what to lock WITH, and the only sensible answer is whatever it was
     * locked with before, so unlocking remembers it. That memory is not written down: a box
     * unlocked when the server stops comes back unlocked with nothing to restore, and says
     * so rather than guessing.
     */
    static bool HandleStashKey(ChatHandler* handler, Optional<uint32> given)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        ObjectGuid::LowType guid = 0;
        Stash* stash = Underfoot(player, guid);

        if (!stash)
        {
            handler->SendErrorMessage("Stand at a strongbox first.");
            return false;
        }

        uint32 entry;

        if (given)
        {
            entry = *given;
        }
        else if (stash->keyItem)
        {
            entry = 0;                               // locked, so the toggle unlocks it
        }
        else if (stash->lastKey)
        {
            entry = stash->lastKey;                  // unlocked, so put back what it had
        }
        else
        {
            handler->SendErrorMessage(
                "|cffffffff{}|r is unlocked and has never had a key. Choose one first.",
                stash->name);
            return false;
        }

        if (entry && !sObjectMgr->GetItemTemplate(entry))
        {
            handler->SendErrorMessage("Item {} does not exist.", entry);
            return false;
        }

        // Remember what is being taken off, so the toggle has something to put back.
        if (!entry && stash->keyItem)
            stash->lastKey = stash->keyItem;

        stash->keyItem = entry;

        WorldDatabase.Execute("UPDATE `sanctuary_stash` SET `keyitem` = {} WHERE `guid` = {}", entry, guid);

        if (!entry)
        {
            Say(handler, Acore::StringFormat("|cffffffff{}|r is now unlocked - anyone may open it.", stash->name));
            return true;
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);

        Say(handler, Acore::StringFormat("|cffffffff{}|r now opens only for |cff00ff96{}|r ({}).",
            stash->name, proto->Name1, entry));

        // A key in the hand, cut for THIS box, so it can be tested without a second command.
        if (CutKey(player, guid, entry))
            Say(handler, "A key for it is in your pack.");
        else
            Say(handler, "No room in your pack for the key - cut one with the panel later.");

        return true;
    }

    /// `.stash name <text>` - what the window is titled.
    static bool HandleStashName(ChatHandler* handler, Tail name)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        ObjectGuid::LowType guid = 0;
        Stash* stash = Underfoot(player, guid);

        if (!stash)
        {
            handler->SendErrorMessage("Stand at a strongbox first.");
            return false;
        }

        std::string label(name);

        if (label.empty())
        {
            handler->SendErrorMessage("Give it a name.");
            return false;
        }

        stash->name = label;
        WorldDatabase.Execute("UPDATE `sanctuary_stash` SET `name` = '{}' WHERE `guid` = {}", label, guid);

        Say(handler, Acore::StringFormat("Strongbox {} is now called |cffffffff{}|r.", guid, label));
        return true;
    }


    /*
     * `.stash remove` - take the box away, once it is empty.
     *
     * It refuses while anything is inside, and there is no override. That is deliberate: the
     * items in a strongbox are real item_instance rows with no owner, and nothing else in the
     * world points at them. Deleting the box's rows without them leaves them orphaned in the
     * database forever; deleting them with it destroys somebody's belongings with no way
     * back. A command that can do the second thing will eventually do it by accident.
     *
     * Emptying a box is not hard - open it and take everything - and it puts the decision
     * where it belongs, on somebody looking at what they are removing.
     */
    static bool HandleStashRemove(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        ObjectGuid::LowType guid = 0;
        Stash* stash = Underfoot(player, guid);

        if (!stash)
        {
            handler->SendErrorMessage("Stand at a strongbox first.");
            return false;
        }

        if (!stash->items.empty() || stash->money)
        {
            handler->SendErrorMessage(
                "|cffffffff{}|r still holds {} item(s) and {}. Empty it first.",
                stash->name, uint32(stash->items.size()),
                stash->money ? std::to_string(stash->money / 10000) + " gold" : "no coin");
            return false;
        }

        // Empty, so there is nothing to delete but the box's own bookkeeping.
        CharacterDatabase.Execute("DELETE FROM `sanctuary_stash_money` WHERE `stash` = {}", guid);
        WorldDatabase.Execute("DELETE FROM `sanctuary_stash` WHERE `guid` = {}", guid);

        std::string const name = stash->name;
        g_stashes.erase(guid);

        // And the object itself, the same way .gobject delete does it.
        if (GameObject* box = player->FindNearestGameObject(g_objectEntry, 20.0f))
        {
            box->SetRespawnTime(0);
            box->Delete();
            box->DeleteFromDB();
        }

        Say(handler, Acore::StringFormat("Strongbox |cffffffff{}|r ({}) is gone.", name, guid));

        LOG_INFO("module.sanctuarystash", "{} removed strongbox {} \"{}\".",
            player->GetName(), guid, name);

        return true;
    }


    /*
     * `.stash rotate [degrees]` - turn the box you are standing at. 180 by default.
     *
     * The object is deleted and loaded again afterwards, which looks superfluous and is not:
     * the 3.3.5a client caches recently deleted objects and will resurrect one from its own
     * stale location if the same guid appears again, so the new facing has to arrive as a
     * fresh spawn. .gobject turn does exactly this for exactly that reason.
     */
    static bool HandleStashRotate(ChatHandler* handler, Optional<float> degrees)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        ObjectGuid::LowType guid = 0;
        Stash* stash = Underfoot(player, guid);

        if (!stash)
        {
            handler->SendErrorMessage("Stand at a strongbox first.");
            return false;
        }

        GameObject* box = FindBox(player, guid);

        if (!box)
        {
            handler->SendErrorMessage("Its object is missing. |cffffffff.stash rebuild {}|r puts one back.", guid);
            return false;
        }

        float const turn = degrees.value_or(180.0f) * float(M_PI) / 180.0f;
        float const facing = Position::NormalizeOrientation(box->GetOrientation() + turn);

        Map* map = box->GetMap();

        box->Relocate(box->GetPositionX(), box->GetPositionY(), box->GetPositionZ(), facing);
        box->SetWorldRotationAngles(facing, 0.0f, 0.0f);
        box->SaveToDB();
        box->Delete();

        GameObject* fresh = new GameObject();

        if (!fresh->LoadGameObjectFromDB(guid, map, true))
        {
            delete fresh;
            handler->SendErrorMessage("It turned, but would not load back. A restart will put it right.");
            return false;
        }

        Say(handler, Acore::StringFormat("|cffffffff{}|r turned {:.0f} degrees.",
            stash->name, degrees.value_or(180.0f)));

        return true;
    }

    /*
     * `.stash rebuild <guid>` - put an object back under a strongbox that lost one.
     *
     * Deleting the gameobject without `.stash remove` leaves the definition, its contents and
     * every key cut for it behind, with nothing in the world to open. The box is not gone -
     * it is unreachable, which is worse, because `.stash remove` refuses to delete a box that
     * still holds anything and there is no way left to empty it.
     *
     * This is the way out. A new object is placed at the GM's feet and everything that
     * pointed at the old spawn is moved onto it in one transaction - the definition, the
     * items, the coin and the keys - so the same keys keep working and nothing is lost.
     */
    static bool HandleStashRebuild(ChatHandler* handler, uint32 old)
    {
        Player* player = handler->GetPlayer();

        if (!player)
            return false;

        Stash* stash = Find(old);

        if (!stash)
        {
            handler->SendErrorMessage("No strongbox {} exists.", old);
            return false;
        }

        if (FindBox(player, old))
        {
            handler->SendErrorMessage("|cffffffff{}|r already has an object. Nothing to rebuild.", stash->name);
            return false;
        }

        ObjectGuid::LowType const fresh = SpawnBox(player);

        if (!fresh)
        {
            handler->SendErrorMessage("The replacement could not be placed there.");
            return false;
        }

        WorldDatabase.Execute("UPDATE `sanctuary_stash` SET `guid` = {} WHERE `guid` = {}", fresh, old);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("UPDATE `sanctuary_stash_item` SET `stash` = {} WHERE `stash` = {}", fresh, old);
        trans->Append("UPDATE `sanctuary_stash_money` SET `stash` = {} WHERE `stash` = {}", fresh, old);
        trans->Append("UPDATE `sanctuary_stash_key` SET `stash` = {} WHERE `stash` = {}", fresh, old);
        CharacterDatabase.CommitTransaction(trans);

        // And the same move in memory, so none of it waits on a restart.
        g_stashes[fresh] = *stash;
        g_stashes.erase(old);

        uint32 moved = 0;

        for (auto& key : g_keys)
            if (key.second == old)
            {
                key.second = fresh;
                ++moved;
            }

        Say(handler, Acore::StringFormat(
            "|cffffffff{}|r rebuilt as spawn {}, carrying {} item(s) and {} key(s) with it.",
            g_stashes[fresh].name, fresh, uint32(g_stashes[fresh].items.size()), moved));

        LOG_INFO("module.sanctuarystash", "{} rebuilt strongbox {} as {}.",
            player->GetName(), old, fresh);

        return true;
    }

    /// `.stash list` - every box, what locks it, and what is in it.
    static bool HandleStashList(ChatHandler* handler)
    {
        if (g_stashes.empty())
        {
            Say(handler, "No strongboxes exist.");
            return true;
        }

        Say(handler, "--- strongboxes ---");

        for (auto const& box : g_stashes)
        {
            std::string key = "|cffff4040unlocked|r";

            if (box.second.keyItem)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(box.second.keyItem);
                key = proto ? proto->Name1 : ("item " + std::to_string(box.second.keyItem));
            }

            // A definition whose object was deleted outside `.stash remove` is still here,
            // still holding things, and cannot be reached or emptied.
            bool const orphan = !HasObject(box.first);

            Say(handler, Acore::StringFormat("  {} |cffffffff{}|r - key: {} - {}/{} slots, {} gold{}",
                box.first, box.second.name, key, uint32(box.second.items.size()),
                uint32(box.second.slots), box.second.money / 10000,
                orphan ? "  |cffff4040NO OBJECT - .stash rebuild " + std::to_string(box.first) + "|r" : ""));
        }

        return true;
    }

    /// `.stash reload` - re-read every definition and every item from the database.
    static bool HandleStashReload(ChatHandler* handler)
    {
        sanctuary_stash_worldscript::Load();
        Say(handler, Acore::StringFormat("Strongboxes reloaded: {} box(es).", uint32(g_stashes.size())));
        return true;
    }
};

void AddSC_sanctuary_stash_scripts()
{
    new sanctuary_stash();
    new sanctuary_stash_commandscript();
    new sanctuary_stash_key();
    new sanctuary_stash_locksmith();
    new sanctuary_stash_open_spell();
    new sanctuary_stash_playerscript();
    new sanctuary_stash_worldscript();
}
