/*
 * mod-sanctuary-lawman - Iron Shackles
 *
 * Used on a player: a ten second cast that disarms them and tethers them within ten yards
 * of whoever put them on. An Iron Shackle Key undoes it, over five seconds of its own.
 *
 * Both are deliberately slow, and the irons are the slower of the two. Each is done to
 * somebody who has not agreed to it, in front of whoever else is standing there, and the
 * length of the cast is most of what gives those people a chance to intervene. The key is
 * the shorter half because a rescue under way is meant to be able to finish.
 *
 * **Authority here is possession, not office.** Nothing in this file asks whether anyone
 * is a lawman. That is deliberate: the shackles and the key are tradeable, and a criminal
 * who obtains a pair from a corrupt guard is meant to be able to use them. Every use is
 * logged instead.
 *
 * Three things are worth knowing before changing any of it.
 *
 * **The cast is real, and that is the whole trick.** The Writ of Accusation suppresses its
 * spell by returning true from OnUse; this returns *false*, so Player::CastItemUseSpell
 * builds a genuine Spell. That buys the cast bar, movement interruption, range, line of
 * sight and target validation from the core for free - including a second CheckCast when
 * the bar completes, so walking out of range while it runs fails the cast. The key works
 * the same way now; it used to suppress its spell, which made a rescue instant and silent.
 *
 * **Nothing is persisted.** A shackle dies when either party leaves the world, so it lives
 * only in memory. That is a decision, not an oversight: it means there is no way to end up
 * with a prisoner still on a chain whose other end outlived the person holding it.
 *
 * **The chain drags, it does not hold.** Straying past the tether hauls the prisoner back
 * toward their captor rather than freezing them where they stand - they can still run, and
 * still be dragged for it, which is both easier to look at than a root and harder to
 * mistake for the game having broken. Every way it ends goes through Release().
 */

#include "SanctuaryLawman.h"

#include "SanctuaryIdentity.h"
#include "SanctuaryOutlaw.h"

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GlobalScript.h"
#include "Item.h"
#include "ItemScript.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MovementTypedefs.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "AllSpellScript.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <cmath>
#include <unordered_map>

namespace
{
    bool g_enabled = true;

    /*
     * The base spell is SELF-cast, and that is the fix for "Invalid Target".
     *
     * The obvious choice is a spell you aim at the prisoner, and it does not work. Every
     * candidate with a usable range has an empty `Targets` mask in the client's own DBC,
     * and the ones the server would accept are *positive* spells - so target selection
     * runs IsValidAssistTarget, which refuses a hostile unit. An outlaw carries faction 14
     * and is hostile to everybody, so the shackles failed on precisely the person they
     * exist for, while working fine on an innocent.
     *
     * Casting on ourselves sidesteps all of it. The spell has no target to validate, on
     * either side of the wire, so nothing can refuse it for who is being pointed at - and
     * the prisoner is read from the caster's own selection instead. Range and line of
     * sight then have to be checked here, since a self-cast enforces neither.
     *
     * 81001 is ours, and it is ours because of the NAME. The cast bar shows the spell's
     * name, the client reads that name from its own files, and no server setting can
     * change the text - so borrowing a spell means borrowing whatever it is called. The
     * best retail fit was 62646 "Shackle"; the word wanted was "Shackling", and owning the
     * row is the only way to have it.
     *
     * Owning it also means the row is simply correct to begin with: five seconds in the
     * cast time column, not channelled, an inert dummy aimed at the caster, and interrupt
     * flags for movement and damage. The load-time corrections below now find a spell that
     * already agrees with them. They are kept anyway - they cost nothing here, and they are
     * what makes it safe to point CastSpell back at a retail spell.
     *
     * Its visual is the blacksmith's crafting animation, which is what fitting irons to
     * somebody ought to look like.
     */
    uint32 g_shackleSpell = 81001;

    /*
     * SpellCastTimes.dbc index 7 == 10000 ms.
     *
     * This is forced onto the SpellInfo at load and therefore BEATS the spell_dbc row, so
     * the two have to be changed together - a migration alone is silently overridden here.
     * Ten seconds because putting somebody in irons is done to an unwilling person, and
     * the length of it is most of what gives anyone watching a chance to stop it.
     */
    uint32 g_castTimeIndex = 7;

    /*
     * The key's own cast, five seconds of it, and a real cast now.
     *
     * It used to be instant and suppressed - the item script answered OnUse and returned
     * true - which made freeing a prisoner silent, unstoppable, and over before anybody
     * standing there could react. Its cast time is in spell_dbc rather than forced here,
     * because unlike Shackling this row is ours rather than a borrowed retail spell.
     */
    uint32 g_keySpell = 81008;

    /// 6608 "Dropped Weapon" - SPELL_AURA_MOD_DISARM, with a debuff name that reads right.
    uint32 g_disarmSpell = 6608;

    uint32 g_shacklesEntry = 990001;
    uint32 g_keyEntry = 990002;

    uint32 g_minutes = 10;
    float g_tether = 5.0f;
    float g_pullSpeed = 20.0f;
    uint32 g_pullIntervalMs = 1000;
    uint32 g_chainKit = 1224;      ///> SpellVisualKit: the Elven Manacles chain

    /*
     * 81000 "Iron Shackles" - the chain that stays on, and the chain strung between the
     * two of them. Ours, and it had to be.
     *
     * SendPlaySpellVisual plays a kit once and it is gone. That is right for the moment the
     * irons close and useless for everything after it, and everything after it is the part
     * that matters: a prisoner nobody can identify on sight is not really in custody. A
     * chain that lasts is a *state* kit, and a state kit is drawn only while an aura sits
     * on the unit - so the lasting chain has to be an aura, not a packet.
     *
     * Which is where the name came in. An aura is labelled, in letters, above the head of
     * everyone who looks at the prisoner, and the client reads that label out of its own
     * Spell.dbc - never from the server. Borrowing 45630 for the artwork borrowed its name
     * with it, and no correction applied here could have touched the text, because the
     * text never travels. So the patch adds a spell row of our own instead, carrying the
     * same artwork under a name worth reading: state kit 1224 worn by the prisoner, and
     * channel kit 8634 drawn between captor and prisoner.
     *
     * Server-side the row is added by the module's world SQL, which AzerothCore overlays
     * on top of Spell.dbc. Both halves must exist or the aura simply never applies.
     */
    uint32 g_chainAura = 81000;
    float g_keyRange = 0.5f;
    float g_castRange = 0.5f;
    /*
     * Off, because the cast spell now carries an animation of its own.
     *
     * This existed when the cast was a borrowed spell with no animation worth playing,
     * and it works - but a state emote is a looping pose and it wins over a cast
     * animation, so leaving it on would hide the very thing it was standing in for.
     * Set it to an emote id if the spell's own animation ever stops playing.
     */
    uint32 g_workEmote = 0;

    struct Shackle
    {
        ObjectGuid captor;
        time_t expires = 0;
        uint32 sinceCheckMs = 0;
        uint32 sincePullMs = 0;   ///> spacing between tugs; see the tether check
    };

    /// Keyed by the prisoner. Only ever holds people who are online.
    std::unordered_map<ObjectGuid::LowType, Shackle> g_shackled;

    time_t Now() { return GameTime::GetGameTime().count(); }

    Shackle* Find(Player const* player)
    {
        if (!g_enabled || !player || g_shackled.empty())
            return nullptr;

        auto it = g_shackled.find(player->GetGUID().GetCounter());
        return it == g_shackled.end() ? nullptr : &it->second;
    }

    /*
     * The crafting pose, held for as long as the irons are being fitted.
     *
     * A state emote rather than a one-shot: one-shots fire once and are over, and this has
     * to last the whole five seconds. Nothing here tracks whether a cast ended, and nothing
     * needs to - the caller passes what is true this tick and the field is corrected to
     * match, so an interrupted cast is tidied up by the next tick whatever interrupted it.
     *
     * It only ever clears a value it set. A player standing in some other emote - an RP
     * addon, a /sit - keeps it.
     */
    void WorkTheIrons(Player* player, bool working)
    {
        uint32 const now = player->GetUInt32Value(UNIT_NPC_EMOTESTATE);

        if (working)
        {
            if (now != g_workEmote)
                player->SetUInt32Value(UNIT_NPC_EMOTESTATE, g_workEmote);
        }
        else if (g_workEmote && now == g_workEmote)
            player->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
    }

    /*
     * The animation a captor plays as the chain goes taut.
     *
     * It has to match what they are holding, which is why the haul was going unseen. An
     * unarmed attack emote sent to somebody with a sword drawn asks the client for an
     * animation that does not exist in that weapon state, and the client's answer is to
     * play nothing at all - no error, no fallback, just a chain that yanks in silence.
     */
    uint32 HaulEmote(Player* captor)
    {
        // Weapons stowed: there is nothing in their hands to swing, so the bare-handed
        // pull is both the honest animation and the one that will actually play.
        if (captor->GetSheath() != SHEATH_STATE_MELEE)
            return EMOTE_ONESHOT_ATTACK_UNARMED;

        Item* weapon = captor->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);

        if (!weapon || !weapon->GetTemplate())
            return EMOTE_ONESHOT_ATTACK_UNARMED;

        return weapon->GetTemplate()->InventoryType == INVTYPE_2HWEAPON
            ? EMOTE_ONESHOT_ATTACK2HTIGHT
            : EMOTE_ONESHOT_ATTACK1H;
    }

    /*
     * The chain drawn between the two of them.
     *
     * There is no packet for "draw a chain from A to B". There is, however, one thing the
     * client already does exactly that for: a channelled spell, which it strings from the
     * caster to whatever UNIT_FIELD_CHANNEL_OBJECT names. Both fields are public, so
     * setting them by hand buys the visual with no spell behind it at all - the core
     * plays the same trick for the battleground spirit healers.
     */
    void HoldTheChain(Player* captor, Player const* prisoner)
    {
        if (!captor || !prisoner || !g_chainAura)
            return;

        // Never over the top of a channel of the captor's own. A lawman who stops to
        // drain somebody keeps their own spell's visual; the chain is re-hung when it
        // ends, which is what the re-assert on every tether check is for.
        uint32 const channelling = captor->GetUInt32Value(UNIT_CHANNEL_SPELL);

        if (channelling && channelling != g_chainAura)
            return;

        captor->SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, prisoner->GetGUID());
        captor->SetUInt32Value(UNIT_CHANNEL_SPELL, g_chainAura);
    }

    /// Only ever takes down our own chain, never a real channel that happens to be running.
    void DropTheChain(Player* captor)
    {
        if (!captor || !g_chainAura || captor->GetUInt32Value(UNIT_CHANNEL_SPELL) != g_chainAura)
            return;

        captor->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
        captor->SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, ObjectGuid::Empty);
    }

    /*
     * The single exit. Every way a shackle can end comes through here, because every one of
     * them has to leave the prisoner able to walk away and the disarm lifted.
     *
     * What it deliberately does NOT do is give the accusation back.
     *
     * Apply() revokes outlaw status because being in custody is the opposite of being at
     * large. Release() leaves it revoked, whatever ended the shackle - a key, a pardon, the
     * timer running out, or the captor being killed. Time in irons is time served, and it
     * is served the moment the irons close rather than by the clock.
     *
     * The consequence is worth stating rather than leaving to be rediscovered: accuse
     * somebody, shackle them, then kill the guard, and they walk away free AND no longer an
     * outlaw - better off than if the watch had never touched them. That is accepted. A
     * rescue that costs an accomplice a fight is allowed to be worth something, and the
     * alternative - an accusation that outlives every arrest - makes the writ the real
     * punishment and the irons a formality.
     */
    void Release(Player* prisoner, char const* why)
    {
        if (!prisoner)
            return;

        ObjectGuid::LowType guid = prisoner->GetGUID().GetCounter();
        auto it = g_shackled.find(guid);

        if (it == g_shackled.end())
            return;

        // Read the captor off the record before it goes, or there is nothing left to
        // unhook the far end of the chain from.
        DropTheChain(ObjectAccessor::FindConnectedPlayer(it->second.captor));

        g_shackled.erase(guid);

        prisoner->RemoveAurasDueToSpell(g_disarmSpell);

        if (g_chainAura)
            prisoner->RemoveAurasDueToSpell(g_chainAura);

        // Back on the guards' books the moment the irons come off, however that happened.
        prisoner->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC);

        if (why && prisoner->GetSession())
            ChatHandler(prisoner->GetSession()).PSendSysMessage("{}", why);
    }

    /// Whoever is holding this prisoner, if they are still here.
    Player* CaptorOf(Shackle const& shackle)
    {
        return ObjectAccessor::FindConnectedPlayer(shackle.captor);
    }

    /*
     * Why these two cannot be shackled together, or nullptr if they can.
     *
     * Checked twice - once when the item is used and again when the bar fills - because a
     * self-cast spell validates nothing about the prisoner, not even that they are still
     * standing there. Everything a targeted cast would have given us has to be done here.
     */
    char const* Refusal(Player* captor, Player* prisoner)
    {
        if (!prisoner)
            return "Select the person you mean to shackle.";

        if (prisoner == captor)
            return "You cannot shackle yourself.";

        if (prisoner->IsGameMaster())
            return "That one is beyond your reach.";

        if (!prisoner->IsAlive())
            return "There is no point shackling the dead.";

        if (SanctuaryLawman::IsShackled(prisoner))
            return "They are already in irons.";

        if (!captor->IsWithinDistInMap(prisoner, g_castRange))
            return "Too far away to reach them.";

        if (!captor->IsWithinLOSInMap(prisoner))
            return "You cannot see them.";

        // One prisoner at a time: the tether has one anchor, and two would fight.
        for (auto const& [guid, shackle] : g_shackled)
            if (shackle.captor == captor->GetGUID())
                return "You are already leading someone.";

        return nullptr;
    }

    /*
     * Why this lock will not open, or nullptr if it will.
     *
     * Checked twice for the same reason the shackles are: the key's spell is self-cast, so
     * it validates nothing about the prisoner, and five seconds is long enough to walk
     * apart, lose sight of each other, or for somebody else to free them first.
     *
     * A key is a key throughout - it does not ask who fitted the irons, which is what lets
     * one that has found its way into the wrong hands free the wrong person.
     */
    char const* KeyRefusal(Player* freer, Player* prisoner)
    {
        if (!prisoner)
            return "Select the person you mean to free.";

        if (!SanctuaryLawman::IsShackled(prisoner))
            return "They are not in irons.";

        if (!freer->IsWithinDistInMap(prisoner, g_keyRange))
            return "Too far away to reach the lock.";

        if (!freer->IsWithinLOSInMap(prisoner))
            return "You cannot see them.";

        return nullptr;
    }
}

namespace SanctuaryLawman
{
    bool IsShackled(Player const* player) { return Find(player) != nullptr; }
}

/*
 * Rewrites the base spell's cast time at load.
 *
 * This hook is the reason no SQL is involved. spell_dbc is loaded with SELECT * and
 * replaces the whole row, so a partial insert would zero every column it omitted; and a
 * row in spellcasttimes_dbc would change that index for every spell in the game using it.
 * OnLoadSpellCustomAttr hands over a non-const SpellInfo* and asks for nothing else.
 */
class sanctuary_lawman_globalscript : public GlobalScript
{
public:
    sanctuary_lawman_globalscript() : GlobalScript("sanctuary_lawman_globalscript",
        { GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR }) { }

    void OnLoadSpellCustomAttr(SpellInfo* spell) override
    {
        if (!spell)
            return;

        if (g_chainAura && spell->Id == g_chainAura)
        {
            Neuter(spell);
            return;
        }

        if (spell->Id != g_shackleSpell)
            return;

        if (SpellCastTimesEntry const* cast = sSpellCastTimesStore.LookupEntry(g_castTimeIndex))
            spell->CastTimeEntry = cast;

        /*
         * Aimed at the caster and made to do nothing.
         *
         * The borrowed spell applies an aura to whatever it hits; cast at a prisoner it
         * would be refused for being hostile, and cast at ourselves it would land on the
         * wrong person. A dummy effect on the caster is inert in both directions, which is
         * all we want from it - the shackle itself is applied by this module.
         */
        spell->Effects[EFFECT_0].Effect = SPELL_EFFECT_DUMMY;
        spell->Effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
        spell->Effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(0);

        /*
         * And it is not a channel.
         *
         * The borrowed spell is flagged channelled, which is what left the caster stuck in
         * a casting animation that never ended: a channel takes its length from a duration
         * rather than a cast time, and with the effect turned into a dummy there was
         * nothing to end it. SpellInfo::IsChanneled reads these two bits directly, so
         * clearing them is enough to make it an ordinary five second cast.
         */
        spell->AttributesEx &= ~(SPELL_ATTR1_IS_CHANNELED | SPELL_ATTR1_IS_SELF_CHANNELED);

        /*
         * And it can be interrupted, which it could not be before.
         *
         * The borrowed spell has no interrupt flags at all, so the core never cancelled it:
         * walking stopped the bar on the client, which predicts the cancel locally, while
         * the server carried on and applied the shackle five seconds later anyway. The
         * movement check at Spell::update only runs for spells that ask for it.
         */
        spell->InterruptFlags |= SPELL_INTERRUPT_FLAG_MOVEMENT | SPELL_INTERRUPT_FLAG_ABORT_ON_DMG;
    }

private:
    /*
     * Strips the borrowed chain spell down to its artwork.
     *
     * The art is the only thing wanted from it. Whatever it did in the encounter it came
     * out of - a charm, a snare, a teleport - would otherwise land on the prisoner along
     * with the chain, so all three effects are replaced by one inert dummy aura.
     *
     * The duration becomes infinite because this module decides when the irons come off,
     * and the aura is forced negative and uncancellable so a prisoner cannot simply
     * right-click the chain away: the cancel handler refuses both a debuff and anything
     * carrying SPELL_ATTR0_NO_AURA_CANCEL, and there is no reason not to have both.
     */
    static void Neuter(SpellInfo* spell)
    {
        spell->Effects[EFFECT_0].Effect = SPELL_EFFECT_APPLY_AURA;
        spell->Effects[EFFECT_0].ApplyAuraName = SPELL_AURA_DUMMY;
        spell->Effects[EFFECT_0].TargetA = SpellImplicitTargetInfo(TARGET_UNIT_TARGET_ANY);
        spell->Effects[EFFECT_0].TargetB = SpellImplicitTargetInfo(0);
        spell->Effects[EFFECT_0].BasePoints = 0;

        spell->Effects[EFFECT_1].Effect = 0;   // SpellEffects has no enumerator for none
        spell->Effects[EFFECT_2].Effect = 0;

        // Index 21 is the -1 row: no duration at all, which is to say forever.
        if (SpellDurationEntry const* forever = sSpellDurationStore.LookupEntry(21))
            spell->DurationEntry = forever;

        spell->Attributes |= uint32(SPELL_ATTR0_AURA_IS_DEBUFF) | uint32(SPELL_ATTR0_NO_AURA_CANCEL);
        spell->AttributesEx &= ~(SPELL_ATTR1_IS_CHANNELED | SPELL_ATTR1_IS_SELF_CHANNELED);
    }
};

/*
 * The shackles.
 *
 * Only the cheap, item-specific refusals happen here; then it returns false and the core
 * runs the real cast. Range and line of sight are deliberately NOT checked here - letting
 * the cast happen is exactly what buys them, and checking twice would only let the two
 * disagree.
 */
class sanctuary_lawman_shackles : public ItemScript
{
public:
    sanctuary_lawman_shackles() : ItemScript("sanctuary_lawman_shackles") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        if (!player || !player->GetSession())
            return true;

        auto refuse = [&](std::string const& why)
        {
            // Without an error the client leaves the item greyed out and the player reads
            // it as a cooldown rather than a refusal.
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            ChatHandler(player->GetSession()).PSendSysMessage("{}", why);
            return true;
        };

        if (!g_enabled)
            return refuse("The shackles will not close.");

        /*
         * The prisoner comes from the caster's selection, not from the packet.
         *
         * The spell is cast on ourselves, so there is no target in it to read - which is
         * the entire point: a self-cast cannot be refused for who it is aimed at, and the
         * shackles work on a hostile outlaw and a peaceful bystander alike.
         */
        Player* prisoner = player->GetSelectedPlayer();

        if (char const* problem = Refusal(player, prisoner))
            return refuse(problem);

        // Only the cast itself is left to the core now, and a self-cast enforces no range
        // of its own - so the checks above are repeated when the bar finishes, in case
        // they walked apart during the five seconds.
        return false;
    }
};

/*
 * The key.
 *
 * Five seconds of work now, the same shape as the shackles: only the cheap refusals happen
 * here, then it returns false and the core runs the real cast, and the lock actually opens
 * when the bar fills.
 *
 * It was instant and suppressed its spell, which made a rescue silent and impossible to
 * interrupt - somebody could be freed out from under the person who put them there with
 * nothing to see and nothing to do about it. The five seconds are the whole point.
 */
class sanctuary_lawman_shackle_key : public ItemScript
{
public:
    sanctuary_lawman_shackle_key() : ItemScript("sanctuary_lawman_shackle_key") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& targets) override
    {
        if (!player || !player->GetSession())
            return true;

        auto refuse = [&](std::string const& why)
        {
            player->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, item, nullptr);
            ChatHandler(player->GetSession()).PSendSysMessage("{}", why);
            return true;
        };

        if (!g_enabled)
            return refuse("The key turns and nothing happens.");

        /*
         * Selection first, packet second.
         *
         * The key's spell is self-targeted so the client sends no unit with it, which made
         * "Point it at a person" the answer even when somebody was plainly selected.
         */
        Unit* unit = targets.GetUnitTarget();
        Player* prisoner = player->GetSelectedPlayer();

        if (!prisoner && unit)
            prisoner = unit->ToPlayer();

        if (char const* problem = KeyRefusal(player, prisoner))
            return refuse(problem);

        // The cast is the core's from here. Everything above is re-tested when the bar
        // finishes, because a self-cast tells the core nothing about who it was aimed at.
        return false;
    }
};

/*
 * The cast landing.
 *
 * OnSpellCast is the last line of Spell::_cast, reached only after the bar filled AND the
 * second CheckCast passed. PlayerScript::OnPlayerSpellCast is the tempting one and is
 * wrong: it fires before that check, so it would also fire for a cast about to be refused
 * for range.
 */
class sanctuary_lawman_shackle_spell : public AllSpellScript
{
public:
    sanctuary_lawman_shackle_spell() : AllSpellScript("sanctuary_lawman_shackle_spell",
        { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!g_enabled || !spell || !spellInfo)
            return;

        // The key finishing is the opposite job and short enough to do here rather than in
        // a second script: same shape, same re-check, one lock instead of one prisoner.
        if (spellInfo->Id == g_keySpell)
        {
            OnKeyCast(spell, caster);
            return;
        }

        if (spellInfo->Id != g_shackleSpell)
            return;

        // The base spell is a real one that other things may use, so the cast only counts
        // when it came from our item.
        if (!spell->m_CastItem || spell->m_CastItem->GetEntry() != g_shacklesEntry)
            return;

        Player* captor = caster ? caster->ToPlayer() : nullptr;

        if (!captor)
            return;

        // The selection again, and re-tested from scratch: ten seconds is long enough to
        // walk out of reach, break line of sight, die, or pick a different target
        // entirely, and a self-cast tells the core none of that.
        Player* prisoner = captor->GetSelectedPlayer();

        if (char const* problem = Refusal(captor, prisoner))
        {
            if (captor->GetSession())
                ChatHandler(captor->GetSession()).PSendSysMessage("{}", problem);
            return;
        }

        /*
         * AddAura rather than CastSpell, and that is load bearing.
         *
         * The disarm spell carries MECHANIC_DISARM, and SpellInfo::CheckTarget refuses to
         * disarm a target with an empty main hand - so a cast would silently fail on
         * exactly the unarmed prisoner this is most likely to be used on. AddAura skips
         * CheckCast entirely, which is safe here because the real cast has already
         * validated range, line of sight and the target.
         */
        int32 const duration = int32(g_minutes) * MINUTE * IN_MILLISECONDS;

        if (Aura* aura = captor->AddAura(g_disarmSpell, prisoner))
        {
            // Max first, then current, or the client draws a bar longer than its own maximum.
            aura->SetMaxDuration(duration);
            aura->SetDuration(duration);
        }

        /*
         * The chain, played straight at the client.
         *
         * Two earlier attempts at this failed for different reasons, and both are worth
         * recording because each looks correct.
         *
         * SendPlaySpellVisual with a SpellVisual id drew nothing: the packet carries a
         * *SpellVisualKit* index, and SpellVisual and SpellVisualKit are different tables.
         * A triggered cast then drew nothing either, because triggering skips CheckCast but
         * NOT target selection - the borrowed spell wants TARGET_UNIT_TARGET_ENEMY, so on a
         * prisoner who is not hostile no target is ever selected and the spell quietly does
         * nothing at all.
         *
         * A kit id sidesteps both. SMSG_PLAY_SPELL_VISUAL names the unit and the kit and
         * nothing else, so there is no targeting, no faction, and no spell system involved.
         * 1224 is the chain out of Elven Manacles - iron rather than ice, which is the
         * point of it.
         */
        if (g_chainKit)
            prisoner->SendPlaySpellVisual(g_chainKit);

        /*
         * And then the chain that stays.
         *
         * The kit above is the moment the irons close; this is every moment after it. The
         * aura carries a state visual, so for as long as it sits on the prisoner every
         * player who can see them sees the chain - which is the entire point of putting
         * somebody in irons in public rather than quietly.
         */
        if (g_chainAura)
            captor->AddAura(g_chainAura, prisoner);

        /*
         * And guards stop seeing them.
         *
         * A prisoner is disarmed, chained and being walked somewhere by whoever arrested
         * them. A patrol cutting them down on the way is not a scene - it wastes the arrest,
         * and the prisoner cannot answer it, having no weapon and no way to leave.
         *
         * Only the NPC half. Players can still cut somebody loose by force, or take a
         * prisoner off the lawman walking them in, and both of those are the good parts.
         */
        prisoner->SetUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC);

        HoldTheChain(captor, prisoner);

        /*
         * An outlaw stops being one the moment the irons close.
         *
         * They are in custody now, which is the opposite of being at large - and it is also
         * what lets them be led: an outlaw carries a hostile faction, and hauling a hostile
         * player around fights every check the core makes about who may touch whom.
         */
        if (SanctuaryOutlaw::IsOutlaw(prisoner))
            SanctuaryOutlaw::Revoke(prisoner, "; you are in irons now, and no longer at large");

        Shackle& shackle = g_shackled[prisoner->GetGUID().GetCounter()];
        shackle.captor = captor->GetGUID();
        shackle.expires = Now() + time_t(g_minutes) * MINUTE;
        shackle.sincePullMs = 0;
        shackle.sinceCheckMs = 0;

        // Labels, never names, and resolved for each side separately.
        if (captor->GetSession())
            ChatHandler(captor->GetSession()).PSendSysMessage(
                "You clap the irons on |cffff4040{}|r.", SanctuaryIdentity::LabelFor(captor, prisoner));

        if (prisoner->GetSession())
            ChatHandler(prisoner->GetSession()).PSendSysMessage(
                "|cffff4040{} has put you in irons.|r You are disarmed and cannot stray far.",
                SanctuaryIdentity::LabelFor(prisoner, captor));

        LOG_INFO("module.sanctuarylawman", "{} shackled {} for {} minutes.",
            captor->GetName(), prisoner->GetName(), g_minutes);
    }

private:
    /*
     * The key's bar filling.
     *
     * Re-tested from scratch rather than trusted from OnUse: five seconds is long enough
     * to walk apart, lose sight of each other, or for somebody else to reach the lock
     * first - and a self-cast tells the core none of it.
     */
    static void OnKeyCast(Spell* spell, Unit* caster)
    {
        // The key's spell is ours alone, but it is still worth insisting the cast came
        // from the key rather than from anything else that learns to cast it later.
        if (!spell->m_CastItem || spell->m_CastItem->GetEntry() != g_keyEntry)
            return;

        Player* freer = caster ? caster->ToPlayer() : nullptr;

        if (!freer)
            return;

        Player* prisoner = freer->GetSelectedPlayer();

        if (char const* problem = KeyRefusal(freer, prisoner))
        {
            if (freer->GetSession())
                ChatHandler(freer->GetSession()).PSendSysMessage("{}", problem);
            return;
        }

        // A key is a key. It does not ask who put them on - which is what lets a key that
        // has found its way into the wrong hands free the wrong person.
        LOG_INFO("module.sanctuarylawman", "{} unlocked the shackles on {}.",
            freer->GetName(), prisoner->GetName());

        Release(prisoner, "The irons fall away.");

        if (freer->GetSession())
            ChatHandler(freer->GetSession()).PSendSysMessage(
                "You unlock the shackles on {}.", SanctuaryIdentity::LabelFor(freer, prisoner));
    }
};

class sanctuary_lawman_shackle_playerscript : public PlayerScript
{
public:
    sanctuary_lawman_shackle_playerscript() : PlayerScript("sanctuary_lawman_shackle_playerscript",
        {
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_LOGOUT
        }) { }

    /*
     * The tether, and every way a shackle ends.
     *
     * Throttled to 200ms rather than the second the tabard re-assert uses: a running
     * player covers about seven yards a second, and a one second check would let them get
     * most of the way to the next zone before the chain noticed.
     */
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!g_enabled || !player)
            return;

        WatchTheCast(player);

        if (g_shackled.empty())
            return;

        Shackle* shackle = Find(player);

        if (!shackle)
            return;

        shackle->sinceCheckMs += diff;
        if (shackle->sinceCheckMs < 200)
            return;

        shackle->sinceCheckMs = 0;

        if (shackle->expires && Now() >= shackle->expires)
        {
            Release(player, "The irons spring open.");
            return;
        }

        Player* captor = CaptorOf(*shackle);

        /*
         * Gone, dead, or somewhere else entirely. Any of them and the chain has nothing to
         * hold on to, so it comes off rather than leaving somebody rooted to a memory.
         *
         * Death counting here is the point, not an oversight. Killing the escort is the
         * sanctioned way to break somebody out of custody by force - and it takes an
         * accomplice, because the prisoner is disarmed and cannot manage it alone. It is
         * also what is meant to make a lone guard walking a prisoner across open country
         * feel like a bad idea, and two guards worth the trouble of finding.
         *
         * It fires the instant they hit zero health, before they have released spirit, so
         * no resurrection is quick enough to save the arrest.
         */
        if (!captor || !captor->IsAlive() || captor->GetMapId() != player->GetMapId())
        {
            Release(player, "Whoever held your chain is gone. The irons fall away.");
            return;
        }

        // Re-hung every check rather than once: a real channel of the captor's own clears
        // the fields when it ends, and either of them relogging clears them outright.
        HoldTheChain(captor, player);

        float const distance = player->GetDistance(captor);

        if (distance <= g_tether)
            return;

        /*
         * Dragged back rather than stopped where they stand.
         *
         * Spacing matters as much as the distance does: a tug every tick would fight the
         * client's own movement the whole way and read as rubber-banding, so once the
         * chain has pulled it is left alone long enough for the jump to land. A prisoner
         * can still run in between, which is the point - they are being dragged, not held.
         */
        shackle->sincePullMs += 200;

        if (shackle->sincePullMs < g_pullIntervalMs)
            return;

        shackle->sincePullMs = 0;

        /*
         * Back to the edge of the circle, not to the captor's feet.
         *
         * A chain stops you leaving; it does not reel you in. Hauling somebody the whole
         * way would also make walking a prisoner anywhere impossible, since every tug would
         * put them back on top of the person leading them. So the destination is the point
         * on the tether's circumference nearest to where they were heading - they keep
         * their bearing, they simply cannot get any further out.
         */
        float const angle = captor->GetAngle(player);
        float const x = captor->GetPositionX() + g_tether * std::cos(angle);
        float const y = captor->GetPositionY() + g_tether * std::sin(angle);
        float z = captor->GetPositionZ();

        // Follow the ground rather than the straight line, or the chain drops people into
        // the floor on a slope and through it on a bridge.
        player->UpdateAllowedPositionZ(x, y, z);

        float const speedZ = float(player->GetDistance(x, y, z) / g_pullSpeed * 0.5f * Movement::gravity);

        player->GetMotionMaster()->MoveJump(x, y, z + 1.0f, g_pullSpeed, speedZ);

        // Without this the anticheat sees a player moving in a way they did not ask for.
        sScriptMgr->AnticheatSetUnderACKmount(player);

        // The captor hauls on the chain, with whatever is in their hands.
        captor->HandleEmoteCommand(HaulEmote(captor));
    }

    /*
     * Cancels a shackling that has stopped making sense.
     *
     * The cast is on the caster, so the core checks nothing about the prisoner for the
     * whole ten seconds - not that they are still in reach, not that they are still
     * visible, not that they are still the person who was selected. Moving is handled by
     * the core now that the spell has interrupt flags; everything about the *other* end of
     * the chain has to be watched here.
     *
     * The key is watched the same way and for the same reason. Its five seconds are
     * shorter but no better supervised, and a cast that runs to the end only to refuse is
     * worse than one that stops the moment it stopped making sense.
     */
    static void WatchTheCast(Player* player)
    {
        Spell* casting = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        uint32 const casting_id = casting ? casting->GetSpellInfo()->Id : 0;

        bool const shackling = casting_id == g_shackleSpell;
        bool const unlocking = casting_id == g_keySpell;

        // Fitting irons is work, and it looks like it - so is picking them open. Set before
        // the early return so that finishing, cancelling and being interrupted all put the
        // pose away again.
        WorkTheIrons(player, shackling || unlocking);

        if (shackling)
        {
            if (!Refusal(player, player->GetSelectedPlayer()))
                return;

            player->InterruptNonMeleeSpells(false, g_shackleSpell);

            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage("They are out of your reach. The irons stay open.");
        }
        else if (unlocking)
        {
            if (!KeyRefusal(player, player->GetSelectedPlayer()))
                return;

            player->InterruptNonMeleeSpells(false, g_keySpell);

            if (player->GetSession())
                ChatHandler(player->GetSession()).PSendSysMessage("They are out of your reach. The lock stays shut.");
        }
    }

    /*
     * Logging out ends it, in both directions - the prisoner leaving, and the captor
     * leaving, which would otherwise strand the prisoner until the next tick noticed.
     */
    void OnPlayerLogout(Player* player) override
    {
        if (!player || g_shackled.empty())
            return;

        Release(player, nullptr);

        ObjectGuid const leaving = player->GetGUID();

        for (auto it = g_shackled.begin(); it != g_shackled.end();)
        {
            if (it->second.captor != leaving)
            {
                ++it;
                continue;
            }

            Player* prisoner = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(it->first));

            ++it;

            if (prisoner)
                Release(prisoner, "Whoever held your chain is gone. The irons fall away.");
        }
    }
};

class sanctuary_lawman_shackle_worldscript : public WorldScript
{
public:
    sanctuary_lawman_shackle_worldscript() : WorldScript("sanctuary_lawman_shackle_worldscript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    /*
     * The chain spell has two halves and they are installed separately.
     *
     * The server half is this module's world SQL; the client half is a row in
     * patch-enUS-4.MPQ. Miss the SQL and the aura never applies, so there is no chain
     * at all. Miss the patch and the aura applies to a spell the client has never heard
     * of, which draws nothing and is labelled "Unknown".
     *
     * Only the server half is visible from here, and it is the half that fails silently,
     * so it is worth one line at startup rather than a report of an invisible chain
     * weeks later.
     */
    void OnStartup() override
    {
        if (!g_enabled || !g_chainAura)
            return;

        if (SpellInfo const* chain = sSpellMgr->GetSpellInfo(g_chainAura))
            LOG_INFO("module.sanctuarylawman", "Shackle chain is spell {} \"{}\".",
                g_chainAura, chain->SpellName[LOCALE_enUS]);
        else
            LOG_ERROR("module.sanctuarylawman",
                "Shackle chain spell {} does not exist - the prisoner will wear no chain. "
                "The module's world SQL has not been applied.", g_chainAura);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>("SanctuaryLawman.Shackles.Enable", true);
        g_shacklesEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.Entry", 990001);
        g_keyEntry = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.KeyEntry", 990002);
        g_shackleSpell = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.CastSpell", 81001);
        g_keySpell = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.KeySpell", 81008);
        g_disarmSpell = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.DisarmSpell", 6608);
        g_minutes = std::max(1u, sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.Minutes", 10));
        g_tether = std::clamp(sConfigMgr->GetOption<float>("SanctuaryLawman.Shackles.Tether", 5.0f), 3.0f, 60.0f);
        g_pullSpeed = std::clamp(sConfigMgr->GetOption<float>("SanctuaryLawman.Shackles.PullSpeed", 20.0f), 5.0f, 50.0f);
        g_pullIntervalMs = std::max(400u, sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.PullIntervalMs", 1000));
        g_chainKit = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.ChainVisualKit", 1224);
        g_chainAura = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.ChainAuraSpell", 81000);
        g_keyRange = std::clamp(sConfigMgr->GetOption<float>("SanctuaryLawman.Shackles.KeyRange", 0.5f), 0.25f, 30.0f);
        g_castRange = std::clamp(sConfigMgr->GetOption<float>("SanctuaryLawman.Shackles.Range", 0.5f), 0.25f, 30.0f);
        g_workEmote = sConfigMgr->GetOption<uint32>("SanctuaryLawman.Shackles.CastEmote", 0);


        LOG_INFO("module.sanctuarylawman",
                 "Sanctuary shackles {}: item {}, key {}, {} minutes, tether {:.0f} yards.",
                 g_enabled ? "enabled" : "disabled", g_shacklesEntry, g_keyEntry, g_minutes, g_tether);
    }
};

void AddSC_sanctuary_lawman_shackles()
{
    new sanctuary_lawman_globalscript();
    new sanctuary_lawman_shackles();
    new sanctuary_lawman_shackle_key();
    new sanctuary_lawman_shackle_spell();
    new sanctuary_lawman_shackle_playerscript();
    new sanctuary_lawman_shackle_worldscript();
}
