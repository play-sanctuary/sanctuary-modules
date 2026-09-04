/*
 * mod-sanctuary-gm
 *
 * The server half of the Sanctuary game master panel.
 *
 * The addon is a convenience, never the authority. Every request that arrives here is
 * re-checked against the sender's account security before anything happens, because the
 * addon channel is just chat: a player with no addon at all can send exactly the same
 * message, and must get exactly nothing for it.
 *
 * Requests arrive as "SGM\t<VERB> <arg> <arg>" on a whisper to self, and replies go back
 * the same way. Searches are answered from the world tables so the panel can browse
 * creatures, objects and models without shipping a copy of them to every client.
 */

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Transport.h"
#include "Log.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
// Sibling module. Every module's src is on the include path and they all link into one
// modules.lib, so the panel can drive the voice bridge through its owner rather than
// opening a second link of its own.
#include "ProximityVoice.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    bool g_enabled = true;
    std::string g_prefix = "SGM";
    uint32 g_requiredSecurity = SEC_GAMEMASTER;
    uint32 g_searchLimit = 25;
    uint32 g_maxSpawnsPerMinute = 60;
    float g_soundRadius = 40.0f;

    /// Objects this session placed, so the panel's undo has something to remove.
    std::unordered_map<ObjectGuid, std::vector<ObjectGuid::LowType>> g_placed;

    struct Budget
    {
        time_t Window = 0;
        uint32 Used = 0;
    };

    std::unordered_map<ObjectGuid, Budget> g_budgets;

    /// Seconds a re-scan may take before the panel is told it went unanswered.
    time_t constexpr SoundReloadTimeout = 10;

    struct PendingReload
    {
        uint32 Revision = 0;
        time_t Asked = 0;
    };

    /*
     * Game masters waiting on the relay to re-scan its sound folder.
     *
     * The reply cannot be sent from the request: the relay answers over the voice bridge
     * some unknown number of ticks later. So the revision the library was at is recorded
     * here, and the world tick sends the list once it moves.
     */
    std::unordered_map<ObjectGuid, PendingReload> g_soundReloads;

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
        Send(to, "MSG " + text);
    }

    /*
     * The Sounds tab's whole picture: the library, and the folder it came from.
     *
     * Shared by the panel's own request and by the deferred answer to a re-scan, so the
     * two cannot drift into showing different things.
     */
    void SendSoundList(Player* to)
    {
        std::vector<std::string> const& sounds = sProximityVoice->AvailableSounds();

        Send(to, "RSOUNDS " + std::to_string(sounds.size()));

        for (std::string const& name : sounds)
            Send(to, "RSOUND " + name);

        // Sent whether or not there are any sounds: an empty library is exactly when
        // knowing which folder was read matters most.
        std::string const& directory = sProximityVoice->SoundDirectory();

        if (!directory.empty())
            Send(to, "RSOUNDDIR " + directory);

        if (sounds.empty())
            Notify(to, sProximityVoice->IsBridgeConnected()
                ? "The voice server has no sounds in its library."
                : "The voice server is not connected.");
    }

    /*
     * The only thing standing between this module and a player spawning boss creatures.
     *
     * Checked on every single request rather than once at panel-open time: the addon
     * cannot be trusted to ask first, and a demoted account must stop working immediately
     * rather than at its next login.
     */
    bool IsAuthorised(Player* player)
    {
        if (!player || !player->GetSession())
            return false;

        return player->GetSession()->GetSecurity() >= AccountTypes(g_requiredSecurity);
    }

    bool WithinBudget(Player* player)
    {
        Budget& budget = g_budgets[player->GetGUID()];
        time_t const now = time(nullptr) / 60;

        if (budget.Window != now)
        {
            budget.Window = now;
            budget.Used = 0;
        }

        return ++budget.Used <= g_maxSpawnsPerMinute;
    }

    /// Strips anything that would break the tab/space delimited reply format.
    std::string Clean(std::string const& value)
    {
        std::string out;
        out.reserve(value.size());

        for (char c : value)
        {
            if (c == '\t' || c == '\n' || c == '\r' || c == '|')
                continue;

            out += c;
        }

        return out;
    }

    // --- search -----------------------------------------------------------

    void SearchCreatures(Player* player, std::string const& needle)
    {
        // EscapeString mutates in place and returns void, so it cannot be called inline.
        std::string escaped = needle;
        WorldDatabase.EscapeString(escaped);

        QueryResult result = WorldDatabase.Query(
            "SELECT `entry`, `name`, `minlevel`, `maxlevel` FROM `creature_template` "
            "WHERE `name` LIKE '%{}%' OR `entry` = '{}' ORDER BY `entry` LIMIT {}",
            escaped, escaped, g_searchLimit);

        if (!result)
        {
            Send(player, "CRESULT 0");
            return;
        }

        std::ostringstream rows;
        uint32 count = 0;

        do
        {
            Field* fields = result->Fetch();

            rows << "CROW " << fields[0].Get<uint32>()
                 << " " << fields[2].Get<uint16>()
                 << " " << fields[3].Get<uint16>()
                 << " " << Clean(fields[1].Get<std::string>()) << "\n";

            ++count;
        } while (result->NextRow());

        Send(player, "CRESULT " + std::to_string(count));

        // One addon message per row: a 3.3.5a addon message is capped at 255 bytes, so a
        // batched reply would be truncated mid-name with no way to tell.
        std::istringstream stream(rows.str());
        std::string line;
        while (std::getline(stream, line))
            Send(player, line);
    }

    void SearchGameObjects(Player* player, std::string const& needle)
    {
        std::string escaped = needle;
        WorldDatabase.EscapeString(escaped);

        QueryResult result = WorldDatabase.Query(
            "SELECT `entry`, `name`, `type` FROM `gameobject_template` "
            "WHERE `name` LIKE '%{}%' OR `entry` = '{}' ORDER BY `entry` LIMIT {}",
            escaped, escaped, g_searchLimit);

        if (!result)
        {
            Send(player, "GRESULT 0");
            return;
        }

        std::ostringstream rows;
        uint32 count = 0;

        do
        {
            Field* fields = result->Fetch();

            rows << "GROW " << fields[0].Get<uint32>()
                 << " " << uint32(fields[2].Get<uint8>())
                 << " " << Clean(fields[1].Get<std::string>()) << "\n";

            ++count;
        } while (result->NextRow());

        Send(player, "GRESULT " + std::to_string(count));

        std::istringstream stream(rows.str());
        std::string line;
        while (std::getline(stream, line))
            Send(player, line);
    }

    /*
     * Spells have no name index anywhere, so this walks the whole spell store the way the
     * core's own .lookup spell does (cs_lookup.cpp). It is roughly eighty thousand entries,
     * which is why it stops at the first full page rather than counting the rest, and why
     * it is behind the same throttle as spawning.
     *
     * Only valid, named spells are offered. Unfiltered, most of Spell.dbc is internal
     * triggers and test entries, and a search for something as common as "fireball" buries
     * the six teachable ranks under a hundred things that cannot be learned at all.
     */
    void SearchSpells(Player* player, std::string const& needle)
    {
        std::wstring wideNeedle;
        if (!Utf8toWStr(needle, wideNeedle))
        {
            Send(player, "SRESULT 0");
            return;
        }

        wstrToLower(wideNeedle);

        // Known-state is reported against whoever the panel would act on, so the list can
        // grey out what that character already has.
        Player* subject = player->GetSelectedPlayer();
        if (!subject)
            subject = player;

        int const locale = player->GetSession()->GetSessionDbcLocale();

        std::ostringstream rows;
        uint32 count = 0;

        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize() && count < g_searchLimit; ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !SpellMgr::IsSpellValid(info))
                continue;

            char const* rawName = info->SpellName[locale];
            if (!rawName || !*rawName)
                continue;

            if (!Utf8FitTo(rawName, wideNeedle))
                continue;

            // The rank is what separates six identically named Fireballs from each other.
            char const* rawRank = info->Rank[locale];
            std::string label = Clean(rawName);

            if (rawRank && *rawRank)
                label += " (" + Clean(rawRank) + ")";

            rows << "SROW " << id
                 << " " << (subject->HasSpell(id) ? 1 : 0)
                 << " " << label << "\n";

            ++count;
        }

        Send(player, "SRESULT " + std::to_string(count));

        std::istringstream stream(rows.str());
        std::string line;
        while (std::getline(stream, line))
            Send(player, line);
    }

    /// Resolves who a spell action applies to. Spells are a player-only concept here.
    Player* SpellSubject(Player* player, bool onTarget)
    {
        if (!onTarget)
            return player;

        return player->GetSelectedPlayer();
    }

    /*
     * Learning and forgetting, reimplemented from Acore::PlayerCommand
     * (scripts/Commands/PlayerCommand.cpp). Not included: src/server/scripts/Commands is
     * not on the module include path, and it is only a dozen lines.
     */
    void LearnSpell(Player* player, uint32 spellId, bool onTarget, bool allRanks)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !SpellMgr::IsSpellValid(info))
        {
            Notify(player, "Spell " + std::to_string(spellId) + " is not one that can be taught.");
            return;
        }

        Player* subject = SpellSubject(player, onTarget);
        if (!subject)
        {
            Notify(player, "Select a player first.");
            return;
        }

        if (!allRanks && subject->HasSpell(spellId))
        {
            Notify(player, subject->GetName() + " already knows that.");
            return;
        }

        subject->learnSpell(spellId, false);

        if (allRanks)
        {
            uint32 next = spellId;
            while ((next = sSpellMgr->GetNextSpellInChain(next)))
                subject->learnSpell(next, false);
        }

        // Without this the talent frame keeps showing the old state until a relog.
        if (GetTalentSpellCost(info->GetFirstRankSpell()->Id))
            subject->SendTalentsInfoData(false);

        Notify(player, Acore::StringFormat("{} learned spell {}.", subject->GetName(), spellId));
    }

    void ForgetSpell(Player* player, uint32 spellId, bool onTarget, bool allRanks)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
        {
            Notify(player, "No spell with id " + std::to_string(spellId) + ".");
            return;
        }

        Player* subject = SpellSubject(player, onTarget);
        if (!subject)
        {
            Notify(player, "Select a player first.");
            return;
        }

        uint32 const target = allRanks ? sSpellMgr->GetFirstSpellInChain(spellId) : spellId;

        if (!subject->HasSpell(target))
        {
            Notify(player, subject->GetName() + " does not know that.");
            return;
        }

        subject->removeSpell(target, SPEC_MASK_ALL, false);

        if (GetTalentSpellCost(info->GetFirstRankSpell()->Id))
            subject->SendTalentsInfoData(false);

        Notify(player, Acore::StringFormat("{} forgot spell {}.", subject->GetName(), target));
    }

    // --- actions ----------------------------------------------------------

    void SpawnCreature(Player* player, uint32 entry, bool permanent)
    {
        if (!sObjectMgr->GetCreatureTemplate(entry))
        {
            Notify(player, "No creature with entry " + std::to_string(entry) + ".");
            return;
        }

        float x, y, z;
        player->GetClosePoint(x, y, z, player->GetObjectSize());

        if (!permanent)
        {
            player->SummonCreature(entry, x, y, z, player->GetOrientation(),
                TEMPSUMMON_MANUAL_DESPAWN, 0);

            Notify(player, "Summoned " + std::to_string(entry) + " (temporary).");
            return;
        }

        Map* map = player->GetMap();

        Creature* creature = new Creature();
        if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, player->GetPhaseMaskForSpawn(),
                entry, 0, x, y, z, player->GetOrientation()))
        {
            delete creature;
            Notify(player, "Could not create that creature here.");
            return;
        }

        creature->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), player->GetPhaseMaskForSpawn());

        ObjectGuid::LowType const spawnId = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;

        // Reloaded from the database rather than reusing the object above, so what stands
        // in the world is exactly what a server restart would produce.
        creature = new Creature();
        if (!creature->LoadCreatureFromDB(spawnId, map))
        {
            delete creature;
            Notify(player, "Saved, but could not be loaded back.");
            return;
        }

        sObjectMgr->AddCreatureToGrid(spawnId, sObjectMgr->GetCreatureData(spawnId));
        Notify(player, "Spawned " + std::to_string(entry) + " permanently.");
    }

    void SpawnGameObject(Player* player, uint32 entry, bool permanent)
    {
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
        if (!info)
        {
            Notify(player, "No object with entry " + std::to_string(entry) + ".");
            return;
        }

        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = player->GetPositionZ();
        float const o = player->GetOrientation();

        // Same construction the core's own .gobject add uses, so a placed object is
        // oriented identically to one made by hand.
        G3D::Quat rotation = G3D::Quat::fromAxisAngleRotation(G3D::Vector3::unitZ(), o);

        if (!permanent)
        {
            player->SummonGameObject(entry, x, y, z, o,
                rotation.x, rotation.y, rotation.z, rotation.w, 300);

            Notify(player, "Placed " + std::to_string(entry) + " (temporary).");
            return;
        }

        Map* map = player->GetMap();

        GameObject* object = sObjectMgr->IsGameObjectStaticTransport(entry)
            ? new StaticTransport()
            : new GameObject();

        if (!object->Create(map->GenerateLowGuid<HighGuid::GameObject>(), entry, map,
                player->GetPhaseMaskForSpawn(), x, y, z, o, rotation, 0, GO_STATE_READY))
        {
            delete object;
            Notify(player, "Could not create that object here.");
            return;
        }

        object->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), player->GetPhaseMaskForSpawn());

        ObjectGuid::LowType const spawnId = object->GetSpawnId();

        // Deleted and reloaded from the database rather than reused, exactly as the core's
        // own command does: reusing the instance leaks and misbehaves in instances.
        delete object;

        object = sObjectMgr->IsGameObjectStaticTransport(entry) ? new StaticTransport() : new GameObject();

        if (!object->LoadGameObjectFromDB(spawnId, map, true))
        {
            delete object;
            Notify(player, "Saved, but could not be loaded back.");
            return;
        }

        sObjectMgr->AddGameobjectToGrid(spawnId, sObjectMgr->GetGameObjectData(spawnId));

        g_placed[player->GetGUID()].push_back(spawnId);

        Notify(player, "Placed " + std::to_string(entry) + " permanently.");
        Send(player, "PLACED " + std::to_string(spawnId));
    }

    /// Removes the last object this session placed.
    void UndoPlacement(Player* player)
    {
        std::vector<ObjectGuid::LowType>& placed = g_placed[player->GetGUID()];

        if (placed.empty())
        {
            Notify(player, "Nothing left to undo.");
            return;
        }

        ObjectGuid::LowType const spawnId = placed.back();
        placed.pop_back();

        if (GameObjectData const* data = sObjectMgr->GetGameObjectData(spawnId))
        {
            if (GameObject* object = ObjectAccessor::GetGameObject(*player,
                    ObjectGuid::Create<HighGuid::GameObject>(data->id, spawnId)))
            {
                object->SetRespawnTime(0);   // do not save a respawn time for something being removed
                object->Delete();
                object->DeleteFromDB();
            }

            Notify(player, "Removed the last object you placed.");
            return;
        }

        Notify(player, "That object is already gone.");
    }

    void Morph(Player* player, uint32 displayId, bool onTarget)
    {
        Unit* subject = player;

        if (onTarget)
        {
            if (Unit* target = player->GetSelectedUnit())
                subject = target;
            else
            {
                Notify(player, "Select something first.");
                return;
            }
        }

        if (displayId == 0)
        {
            subject->DeMorph();
            Notify(player, "Model reverted.");
            return;
        }

        if (!sCreatureDisplayInfoStore.LookupEntry(displayId))
        {
            Notify(player, "No display with id " + std::to_string(displayId) + ".");
            return;
        }

        subject->SetDisplayId(displayId);
        Notify(player, "Model set to " + std::to_string(displayId) + ".");
    }

    void Scale(Player* player, float scale, bool onTarget)
    {
        Unit* subject = player;

        if (onTarget)
        {
            if (Unit* target = player->GetSelectedUnit())
                subject = target;
            else
            {
                Notify(player, "Select something first.");
                return;
            }
        }

        // Past these the model either vanishes into the floor or fills the screen and
        // cannot be clicked to fix.
        scale = std::clamp(scale, 0.1f, 10.0f);

        subject->SetObjectScale(scale);
        Notify(player, Acore::StringFormat("Scale set to {:.2f}.", scale));
    }

    void PlaySound(Player* player, uint32 soundId, std::string const& scope, float radius, bool positional)
    {
        if (!sSoundEntriesStore.LookupEntry(soundId))
        {
            Notify(player, "No sound with id " + std::to_string(soundId) + ".");
            return;
        }

        if (scope == "self")
        {
            player->PlayDirectSound(soundId, player);
        }
        else if (scope == "target")
        {
            if (Player* target = player->GetSelectedPlayer())
                target->PlayDirectSound(soundId, target);
            else
            {
                Notify(player, "Select a player first.");
                return;
            }
        }
        else
        {
            /*
             * Everyone within a chosen radius, which is what makes this useful for setting
             * a scene in one room rather than across a whole town square.
             *
             * The clamp to the map's visibility range is load bearing twice over.
             * SendMessageToSetInRange delivers through MessageDistDeliverer, which only
             * visits players who are already visible - 100 yards on continents by default -
             * so a larger radius would silently reach no further and the slider would be
             * promising something it cannot do. It also guarantees every recipient can see
             * the sender, which is what makes the positional form safe: PlayObjectSound is
             * attached to a GUID and is ignored by a client that does not know that object.
             */
            float const limit = player->GetMap()->GetVisibilityRange();
            radius = std::clamp(radius > 0.0f ? radius : g_soundRadius, 1.0f, limit);

            // Both packets are named locals, not temporaries. Write() hands back a pointer
            // to a member of the packet object, so building one inside the argument list
            // of an earlier statement would leave that pointer dangling by the time it is
            // sent - which is why the core always writes these inline.
            WorldPackets::Misc::PlayObjectSound fromHere(player->GetGUID(), soundId);
            WorldPackets::Misc::Playsound everywhere(soundId);

            player->SendMessageToSetInRange(
                positional ? fromHere.Write() : everywhere.Write(), radius, true);

            Notify(player, Acore::StringFormat("Played sound {} to {:.0f} yards.", soundId, radius));
            return;
        }

        Notify(player, "Played sound " + std::to_string(soundId) + ".");
    }

    // --- dispatch ---------------------------------------------------------

    void Handle(Player* player, std::string const& body)
    {
        std::istringstream stream(body);
        std::string verb;
        stream >> verb;

        if (verb == "HELLO")
        {
            uint32 const security = uint32(player->GetSession()->GetSecurity());

            /*
             * Whether the panel may open at all is decided here, not by the client.
             *
             * The addon is installed for everyone - the launcher publishes one list to
             * every player - so an ordinary player used to get READY, a panel full of
             * controls, and a refusal on each one they pressed. Answering DENY instead
             * keeps them from ever seeing it, and costs nothing in security either way:
             * every action re-checks on its own, so a client that ignored this answer
             * would still be able to do nothing.
             */
            if (security < g_requiredSecurity)
            {
                Send(player, "DENY");
                return;
            }

            // The panel asks on open; the answer is what unlocks its controls.
            Send(player, Acore::StringFormat("READY {} {}", security, g_searchLimit));
            return;
        }

        if (verb == "CSEARCH" || verb == "GSEARCH" || verb == "SSEARCH")
        {
            std::string needle;
            std::getline(stream, needle);

            if (!needle.empty() && needle.front() == ' ')
                needle.erase(0, 1);

            if (needle.size() < 2)
            {
                Notify(player, "Type at least two characters to search.");
                return;
            }

            if (verb == "CSEARCH")
            {
                SearchCreatures(player, needle);
            }
            else if (verb == "GSEARCH")
            {
                SearchGameObjects(player, needle);
            }
            else
            {
                // Behind the same throttle as spawning: scanning the spell store is by a
                // wide margin the most expensive thing this module does.
                if (!WithinBudget(player))
                {
                    Notify(player, "Slow down - too many requests this minute.");
                    return;
                }

                SearchSpells(player, needle);
            }

            return;
        }

        if (verb == "SLEARN" || verb == "SFORGET")
        {
            uint32 spellId = 0;
            uint32 onTarget = 0;
            uint32 allRanks = 0;
            stream >> spellId >> onTarget >> allRanks;

            if (spellId == 0)
                return;

            if (verb == "SLEARN")
                LearnSpell(player, spellId, onTarget != 0, allRanks != 0);
            else
                ForgetSpell(player, spellId, onTarget != 0, allRanks != 0);

            return;
        }

        if (verb == "CSPAWN" || verb == "GSPAWN")
        {
            uint32 entry = 0;
            uint32 permanent = 0;
            stream >> entry >> permanent;

            if (entry == 0)
                return;

            if (!WithinBudget(player))
            {
                Notify(player, "Slow down - too many spawns this minute.");
                return;
            }

            if (verb == "CSPAWN")
                SpawnCreature(player, entry, permanent != 0);
            else
                SpawnGameObject(player, entry, permanent != 0);

            return;
        }

        if (verb == "UNDO")
        {
            UndoPlacement(player);
            return;
        }

        if (verb == "MORPH")
        {
            uint32 displayId = 0;
            uint32 onTarget = 0;
            stream >> displayId >> onTarget;
            Morph(player, displayId, onTarget != 0);
            return;
        }

        if (verb == "SCALE")
        {
            float scale = 1.0f;
            uint32 onTarget = 0;
            stream >> scale >> onTarget;
            Scale(player, scale, onTarget != 0);
            return;
        }

        if (verb == "RSOUNDS")
        {
            // Deliberately the cached list rather than a re-scan: opening the panel should
            // not spin a disk. RELOAD is the verb that goes and looks.
            SendSoundList(player);
            return;
        }

        if (verb == "RELOAD")
        {
            if (!sProximityVoice->IsBridgeConnected())
            {
                Notify(player, "The voice server is not connected.");
                SendSoundList(player);
                return;
            }

            // Read before asking: the reply is what moves the revision, so this is the
            // value it has to differ from.
            g_soundReloads[player->GetGUID()] = { sProximityVoice->SoundsRevision(), time(nullptr) };

            sProximityVoice->RequestSoundReload();
            return;
        }

        if (verb == "RPLAY")
        {
            float range = 40.0f;
            std::string name;

            stream >> range;
            std::getline(stream, name);

            if (!name.empty() && name.front() == ' ')
                name.erase(0, 1);

            if (name.empty())
                return;

            if (!sProximityVoice->PlayRelaySound(player, name, range))
                Notify(player, "Could not play that: the voice server may be down, or you may not be in the world yet.");

            return;
        }

        if (verb == "RSTOP")
        {
            sProximityVoice->StopSounds(player);
            Notify(player, "Stopped your sounds.");
            return;
        }

        if (verb == "SOUND")
        {
            uint32 soundId = 0;
            std::string scope = "area";
            float radius = 0.0f;          // 0 means "use the configured default"
            uint32 positional = 1;
            stream >> soundId >> scope >> radius >> positional;

            PlaySound(player, soundId, scope, radius, positional != 0);
            return;
        }
    }
}

class sanctuary_gm_playerscript : public PlayerScript
{
public:
    sanctuary_gm_playerscript() : PlayerScript("sanctuary_gm_playerscript",
        {
            // Addon traffic is a whisper to self, so it is the private-chat overload.
            PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
            PLAYERHOOK_ON_LOGOUT
        }) { }

    //[[ Addon traffic arrives as a whisper the player sends to themselves. ]]
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang != LANG_ADDON || !player || !g_enabled)
            return true;

        std::string const marker = g_prefix + "\t";
        if (msg.rfind(marker, 0) != 0)
            return true;

        // Ours. Swallow it whatever happens next, so nothing surfaces as a whisper.
        if (!IsAuthorised(player))
        {
            LOG_WARN("module.sanctuarygm", "{} ({}) sent a GM panel request without the security for it.",
                player->GetName(), player->GetGUID().ToString());
            return false;
        }

        Handle(player, msg.substr(marker.size()));
        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        g_placed.erase(player->GetGUID());
        g_budgets.erase(player->GetGUID());
    }
};

class sanctuary_gm_worldscript : public WorldScript
{
public:
    sanctuary_gm_worldscript() : WorldScript("sanctuary_gm_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    /*
     * Delivers the answer to a sound folder re-scan.
     *
     * Polling a revision counter rather than having the addon wait a fixed delay and ask
     * again: how long a scan takes depends on how many files need encoding, and a delay
     * guessed short shows a stale list with nothing to say it is stale.
     */
    void OnUpdate(uint32 /*diff*/) override
    {
        if (!g_enabled || g_soundReloads.empty())
            return;

        uint32 const revision = sProximityVoice->SoundsRevision();
        time_t const now = time(nullptr);

        for (auto it = g_soundReloads.begin(); it != g_soundReloads.end(); )
        {
            Player* player = ObjectAccessor::FindPlayer(it->first);

            if (revision != it->second.Revision)
            {
                if (player)
                    SendSoundList(player);

                it = g_soundReloads.erase(it);
            }
            else if (!player || now - it->second.Asked >= SoundReloadTimeout)
            {
                if (player)
                    Notify(player, "The voice server did not answer the re-scan.");

                it = g_soundReloads.erase(it);
            }
            else
                ++it;
        }
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryGM.Enable", true);
        g_prefix = sConfigMgr->GetOption<std::string>("SanctuaryGM.Addon.Prefix", "SGM");
        g_requiredSecurity = sConfigMgr->GetOption<uint32>("SanctuaryGM.RequiredSecurity", uint32(SEC_GAMEMASTER));
        g_searchLimit = std::clamp(sConfigMgr->GetOption<uint32>("SanctuaryGM.SearchLimit", 25u), 1u, 50u);
        g_maxSpawnsPerMinute = sConfigMgr->GetOption<uint32>("SanctuaryGM.MaxSpawnsPerMinute", 60);
        g_soundRadius = sConfigMgr->GetOption<float>("SanctuaryGM.Sound.DefaultRadius", 40.0f);

        LOG_INFO("module.sanctuarygm", "Sanctuary GM panel {} (security {} and above).",
            g_enabled ? "enabled" : "disabled", g_requiredSecurity);
    }
};

void AddSC_sanctuary_gm_scripts()
{
    new sanctuary_gm_playerscript();
    new sanctuary_gm_worldscript();
}
