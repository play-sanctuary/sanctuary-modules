/*
 * mod-proximity-voice - language knowledge
 *
 * Mirrors the rules the text chat handler applies, so a character's voice obeys
 * exactly the same barrier their /say does: you speak what you have learned, and
 * you understand what you have learned.
 */

#include "ProximityVoice.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include <algorithm>
#include <array>
#include <cctype>

namespace ProximityVoice
{
    namespace
    {
        struct LanguageName
        {
            uint32 language;
            char const* key;   // lowercase, accepted by .voice lang
            char const* label; // shown to players
        };

        // Only the languages a player character can actually hold a skill in, plus
        // the two pseudo-languages the mixer treats specially.
        constexpr std::array<LanguageName, 13> LanguageNames
        {{
            { LANG_UNIVERSAL,   "universal",  "Universal"  },
            { LANG_ORCISH,      "orcish",     "Orcish"     },
            { LANG_DARNASSIAN,  "darnassian", "Darnassian" },
            { LANG_TAURAHE,     "taurahe",    "Taurahe"    },
            { LANG_DWARVISH,    "dwarvish",   "Dwarvish"   },
            { LANG_COMMON,      "common",     "Common"     },
            { LANG_DEMONIC,     "demonic",    "Demonic"    },
            { LANG_TITAN,       "titan",      "Titan"      },
            { LANG_THALASSIAN,  "thalassian", "Thalassian" },
            { LANG_DRACONIC,    "draconic",   "Draconic"   },
            { LANG_KALIMAG,     "kalimag",    "Kalimag"    },
            { LANG_GNOMISH,     "gnomish",    "Gnomish"    },
            { LANG_TROLL,       "troll",      "Troll"      }
        }};

        // Gutterspeak and Draenei sit outside the contiguous block above.
        constexpr std::array<LanguageName, 2> ExtraLanguageNames
        {{
            { LANG_GUTTERSPEAK, "gutterspeak", "Gutterspeak" },
            { LANG_DRAENEI,     "draenei",     "Draenei"     }
        }};

        std::string ToLower(std::string const& value)
        {
            std::string out = value;
            std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char c) { return char(std::tolower(c)); });
            return out;
        }
    }

    uint32 GetRacialLanguage(uint8 race)
    {
        switch (race)
        {
            case RACE_HUMAN:         return LANG_COMMON;
            case RACE_ORC:           return LANG_ORCISH;
            case RACE_DWARF:         return LANG_DWARVISH;
            case RACE_NIGHTELF:      return LANG_DARNASSIAN;
            case RACE_UNDEAD_PLAYER: return LANG_GUTTERSPEAK;
            case RACE_TAUREN:        return LANG_TAURAHE;
            case RACE_GNOME:         return LANG_GNOMISH;
            case RACE_TROLL:         return LANG_TROLL;
            case RACE_BLOODELF:      return LANG_THALASSIAN;
            case RACE_DRAENEI:       return LANG_DRAENEI;
            default:                 return LANG_UNIVERSAL;
        }
    }

    std::vector<uint32> AllLanguages()
    {
        std::vector<uint32> out;
        out.reserve(LanguageNames.size() + ExtraLanguageNames.size());

        for (auto const& entry : LanguageNames)
            out.push_back(entry.language);
        for (auto const& entry : ExtraLanguageNames)
            out.push_back(entry.language);

        return out;
    }

    std::string GetLanguageName(uint32 language)
    {
        for (auto const& entry : LanguageNames)
            if (entry.language == language)
                return entry.label;
        for (auto const& entry : ExtraLanguageNames)
            if (entry.language == language)
                return entry.label;

        return "Unknown(" + std::to_string(language) + ")";
    }

    bool ParseLanguage(std::string const& input, uint32& language)
    {
        if (input.empty())
            return false;

        std::string const needle = ToLower(input);

        for (auto const& entry : LanguageNames)
        {
            if (needle == entry.key)
            {
                language = entry.language;
                return true;
            }
        }

        for (auto const& entry : ExtraLanguageNames)
        {
            if (needle == entry.key)
            {
                language = entry.language;
                return true;
            }
        }

        // Also accept a raw language id so tooling does not need the name table.
        if (std::all_of(needle.begin(), needle.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        {
            uint32 const parsed = uint32(std::strtoul(needle.c_str(), nullptr, 10));
            for (uint32 candidate : AllLanguages())
            {
                if (candidate == parsed)
                {
                    language = parsed;
                    return true;
                }
            }
        }

        return false;
    }

    bool KnowsLanguage(Player* player, uint32 language)
    {
        if (!player)
            return false;

        // Universal is the "everyone hears this" channel - GM speech and emotes.
        if (language == LANG_UNIVERSAL)
            return true;

        if (player->IsGameMaster())
            return true;

        LanguageDesc const* desc = GetLanguageDescByID(language);
        if (!desc)
            return false;

        // A language with no skill behind it is not gated at all.
        if (desc->skill_id == 0)
            return true;

        if (player->HasSkill(desc->skill_id))
            return true;

        // Comprehend Language effects (e.g. the Kalytha's Haunting Vision line)
        // temporarily lift the barrier for one language.
        for (auto const& auraEffect : player->GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE))
            if (auraEffect->GetMiscValue() == int32(language))
                return true;

        return false;
    }

    uint8 LanguageProficiency(Player* player, uint32 language)
    {
        if (!player)
            return 0;

        if (language == LANG_UNIVERSAL || player->IsGameMaster())
            return 100;

        LanguageDesc const* desc = GetLanguageDescByID(language);
        if (!desc)
            return 0;

        if (desc->skill_id == 0)
            return 100;

        for (auto const& auraEffect : player->GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE))
            if (auraEffect->GetMiscValue() == int32(language))
                return 100;

        if (!player->HasSkill(desc->skill_id))
            return 0;

        /*
         * The skill value is the measure. The game learns every language at 300 of 300,
         * so nothing changes for anyone until a game master sets one lower - .voice
         * teach, or .setskill on the language's skill - at which point the voice server
         * lets that share of the words through and the rest arrive as a voice through a
         * wall. Text chat has no half measure and keeps treating any skill as fluent.
         */
        uint32 const value = player->GetSkillValue(desc->skill_id);
        return uint8(std::min<uint32>(100, value * 100 / 300));
    }

    bool IsLanguageSkill(uint32 skillId)
    {
        if (!skillId)
            return false;

        for (LanguageDesc const& desc : lang_description)
            if (desc.skill_id == skillId)
                return true;

        return false;
    }

    std::vector<KnownLanguage> CollectKnownLanguages(Player* player)
    {
        std::vector<KnownLanguage> known;
        if (!player)
            return known;

        for (uint32 language : AllLanguages())
        {
            if (language == LANG_UNIVERSAL)
                continue;

            if (KnowsLanguage(player, language))
                known.push_back({ language, LanguageProficiency(player, language) });
        }

        // A game master is understood by, and understands, everyone.
        if (player->IsGameMaster())
            known.push_back({ LANG_UNIVERSAL, 100 });

        return known;
    }
}
