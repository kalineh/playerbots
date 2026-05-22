#include "playerbot/playerbot.h"
#include "FriendAbilityCatalog.h"

#include "PlayerbotAI.h"
#include "ServerFacade.h"

#include <algorithm>
#include <cctype>
#include <map>

using namespace ai;

namespace
{
    std::string ToLower(const std::string& value)
    {
        std::string out = value;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    bool Contains(const std::string& value, const char* text)
    {
        return value.find(text) != std::string::npos;
    }

    bool IsAreaAura(uint32 effect)
    {
        return effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY ||
            effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID ||
            effect == SPELL_EFFECT_APPLY_AREA_AURA_PET ||
            effect == SPELL_EFFECT_APPLY_AREA_AURA_FRIEND ||
            effect == SPELL_EFFECT_APPLY_AREA_AURA_ENEMY;
    }

    bool IsDamageEffect(uint32 effect)
    {
        return effect == SPELL_EFFECT_SCHOOL_DAMAGE ||
            effect == SPELL_EFFECT_WEAPON_DAMAGE ||
            effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL ||
            effect == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE ||
            effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG ||
            effect == SPELL_EFFECT_HEALTH_LEECH ||
            effect == SPELL_EFFECT_POWER_BURN;
    }

    bool IsHealEffect(uint32 effect)
    {
        return effect == SPELL_EFFECT_HEAL ||
            effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
            effect == SPELL_EFFECT_HEAL_PCT;
    }

    bool IsAuraApplication(uint32 effect)
    {
        return effect == SPELL_EFFECT_APPLY_AURA || IsAreaAura(effect);
    }

    bool IsCoreBuffName(const std::string& name)
    {
        return Contains(name, "fortitude") ||
            Contains(name, "arcane intellect") ||
            Contains(name, "arcane brilliance") ||
            Contains(name, "divine spirit") ||
            Contains(name, "prayer of spirit") ||
            Contains(name, "mark of the wild") ||
            Contains(name, "gift of the wild") ||
            Contains(name, "blessing of") ||
            Contains(name, "greater blessing") ||
            Contains(name, "thorns") ||
            Contains(name, "battle shout") ||
            Contains(name, "commanding shout") ||
            Contains(name, "horn of winter") ||
            Contains(name, "trueshot aura") ||
            Contains(name, "inner fire") ||
            Contains(name, "mage armor") ||
            Contains(name, "ice armor") ||
            Contains(name, "frost armor") ||
            Contains(name, "molten armor") ||
            Contains(name, "demon armor") ||
            Contains(name, "demon skin") ||
            Contains(name, "fel armor") ||
            Contains(name, "aspect of the hawk") ||
            Contains(name, "aspect of the viper") ||
            Contains(name, "lightning shield") ||
            Contains(name, "water shield");
    }

    bool IsSituationalBuffName(const std::string& name)
    {
        return Contains(name, "fear ward") ||
            Contains(name, "shadow protection") ||
            Contains(name, "fire resistance") ||
            Contains(name, "frost resistance") ||
            Contains(name, "nature resistance") ||
            Contains(name, "water breathing") ||
            Contains(name, "water walking") ||
            Contains(name, "levitate") ||
            Contains(name, "detect invisibility") ||
            Contains(name, "unending breath");
    }

    bool IsDefensiveName(const std::string& name)
    {
        return Contains(name, "shield") ||
            Contains(name, "barrier") ||
            Contains(name, "ice block") ||
            Contains(name, "divine protection") ||
            Contains(name, "divine shield") ||
            Contains(name, "evasion") ||
            Contains(name, "deterrence") ||
            Contains(name, "barkskin") ||
            Contains(name, "survival instincts") ||
            Contains(name, "last stand") ||
            Contains(name, "shield wall") ||
            Contains(name, "fade") ||
            Contains(name, "feign death") ||
            Contains(name, "vanish");
    }

    bool IsDamageCooldownName(const std::string& name)
    {
        return Contains(name, "recklessness") ||
            Contains(name, "retaliation") ||
            Contains(name, "avenging wrath") ||
            Contains(name, "rapid fire") ||
            Contains(name, "bestial wrath") ||
            Contains(name, "arcane power") ||
            Contains(name, "icy veins") ||
            Contains(name, "combustion") ||
            Contains(name, "shadowfiend") ||
            Contains(name, "starfall") ||
            Contains(name, "berserk") ||
            Contains(name, "bloodlust") ||
            Contains(name, "heroism");
    }

    bool IsComboPointSpenderName(const std::string& name)
    {
        return Contains(name, "eviscerate") ||
            Contains(name, "rupture") ||
            Contains(name, "slice and dice") ||
            Contains(name, "kidney shot") ||
            Contains(name, "rip") ||
            Contains(name, "ferocious bite") ||
            Contains(name, "maim") ||
            Contains(name, "savage roar");
    }

    bool IsComboPointBuilderName(const std::string& name)
    {
        return Contains(name, "sinister strike") ||
            Contains(name, "backstab") ||
            Contains(name, "mutilate") ||
            Contains(name, "hemorrhage") ||
            Contains(name, "shred") ||
            Contains(name, "mangle (cat)") ||
            Contains(name, "rake") ||
            Contains(name, "claw");
    }

    void AddDispelType(uint32 type, uint32& primaryType, uint32& mask)
    {
        if (!type || type >= 32)
            return;

        if (!primaryType)
            primaryType = type;
        mask |= 1u << type;
    }

    bool IsCureName(const std::string& name, uint32& dispelType, uint32& dispelMask)
    {
        const uint32 originalMask = dispelMask;

        if (Contains(name, "dispel magic"))
            AddDispelType(DISPEL_MAGIC, dispelType, dispelMask);

        if (Contains(name, "remove curse") ||
            Contains(name, "remove lesser curse") ||
            Contains(name, "remove corruption"))
            AddDispelType(DISPEL_CURSE, dispelType, dispelMask);

        if (Contains(name, "cure disease") ||
            Contains(name, "abolish disease"))
            AddDispelType(DISPEL_DISEASE, dispelType, dispelMask);

        if (Contains(name, "cure poison") ||
            Contains(name, "abolish poison") ||
            Contains(name, "remove corruption"))
            AddDispelType(DISPEL_POISON, dispelType, dispelMask);

        if (Contains(name, "purify"))
        {
            AddDispelType(DISPEL_DISEASE, dispelType, dispelMask);
            AddDispelType(DISPEL_POISON, dispelType, dispelMask);
        }

        if (Contains(name, "cleanse spirit"))
        {
            AddDispelType(DISPEL_POISON, dispelType, dispelMask);
            AddDispelType(DISPEL_DISEASE, dispelType, dispelMask);
            AddDispelType(DISPEL_CURSE, dispelType, dispelMask);
        }
        else if (Contains(name, "cleanse"))
        {
            AddDispelType(DISPEL_POISON, dispelType, dispelMask);
            AddDispelType(DISPEL_DISEASE, dispelType, dispelMask);
            AddDispelType(DISPEL_MAGIC, dispelType, dispelMask);
        }

        return dispelMask != originalMask;
    }

    uint32 ClassifyAbility(const SpellEntry* spellInfo, const std::string& lowerName, uint32& dispelType, uint32& dispelMask)
    {
        uint32 flags = 0;
        const bool positive = IsPositiveSpell(spellInfo);

        if (positive)
            flags |= FRIEND_ABILITY_BUFF;

        if (PlayerbotAI::IsHealSpell(spellInfo))
            flags |= FRIEND_ABILITY_HEAL;

        if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
            flags |= FRIEND_ABILITY_AOE;

        if (Contains(lowerName, "fear") || Contains(lowerName, "psychic scream") || Contains(lowerName, "intimidating shout"))
            flags |= FRIEND_ABILITY_FEAR | FRIEND_ABILITY_CC;

        if (IsCoreBuffName(lowerName))
            flags |= FRIEND_ABILITY_BUFF | FRIEND_ABILITY_BUFF_CORE;

        if (IsSituationalBuffName(lowerName))
            flags |= FRIEND_ABILITY_BUFF | FRIEND_ABILITY_BUFF_SITUATIONAL;

        if (IsDefensiveName(lowerName))
            flags |= FRIEND_ABILITY_DEFENSIVE;

        if (IsDamageCooldownName(lowerName))
            flags |= FRIEND_ABILITY_DAMAGE_COOLDOWN;

        if (IsCureName(lowerName, dispelType, dispelMask))
            flags |= FRIEND_ABILITY_CURE;

        if (IsComboPointBuilderName(lowerName))
            flags |= FRIEND_ABILITY_COMBO_BUILDER;

        if (IsComboPointSpenderName(lowerName))
            flags |= FRIEND_ABILITY_COMBO_SPENDER;

        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            const uint32 effect = spellInfo->Effect[i];
            const uint32 aura = spellInfo->EffectApplyAuraName[i];

            if (IsDamageEffect(effect))
                flags |= FRIEND_ABILITY_DAMAGE | FRIEND_ABILITY_DIRECT_DAMAGE;

            if (IsHealEffect(effect))
                flags |= FRIEND_ABILITY_HEAL;

            if (effect == SPELL_EFFECT_INTERRUPT_CAST)
                flags |= FRIEND_ABILITY_INTERRUPT | FRIEND_ABILITY_CC;

            if (effect == SPELL_EFFECT_DISPEL || effect == SPELL_EFFECT_DISPEL_MECHANIC)
            {
                flags |= FRIEND_ABILITY_CURE;
                AddDispelType(spellInfo->EffectMiscValue[i], dispelType, dispelMask);
            }

            if (effect == SPELL_EFFECT_THREAT || effect == SPELL_EFFECT_THREAT_ALL || effect == SPELL_EFFECT_ATTACK_ME)
                flags |= FRIEND_ABILITY_THREAT;

            if (effect == SPELL_EFFECT_SUMMON || effect == SPELL_EFFECT_SUMMON_PET || effect == SPELL_EFFECT_SUMMON_OBJECT_WILD ||
                effect == SPELL_EFFECT_SUMMON_OBJECT_SLOT1 || effect == SPELL_EFFECT_SUMMON_OBJECT_SLOT2 ||
                effect == SPELL_EFFECT_SUMMON_OBJECT_SLOT3 || effect == SPELL_EFFECT_SUMMON_OBJECT_SLOT4)
                flags |= FRIEND_ABILITY_SUMMON;

            if (effect == SPELL_EFFECT_CHARGE || effect == SPELL_EFFECT_JUMP || effect == SPELL_EFFECT_JUMP_DEST ||
                effect == SPELL_EFFECT_LEAP || effect == SPELL_EFFECT_LEAP_BACK)
                flags |= FRIEND_ABILITY_MOVEMENT;

            if (IsAuraApplication(effect))
            {
                flags |= FRIEND_ABILITY_AURA;
                if (IsAreaAura(effect))
                    flags |= FRIEND_ABILITY_AOE;

                switch (aura)
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    case SPELL_AURA_PERIODIC_LEECH:
                    case SPELL_AURA_PERIODIC_MANA_LEECH:
                        flags |= FRIEND_ABILITY_DAMAGE | FRIEND_ABILITY_DOT;
                        break;
                    case SPELL_AURA_PERIODIC_HEAL:
                        flags |= FRIEND_ABILITY_HEAL | FRIEND_ABILITY_HOT;
                        break;
                    case SPELL_AURA_SCHOOL_ABSORB:
                    case SPELL_AURA_MANA_SHIELD:
                    case SPELL_AURA_DAMAGE_SHIELD:
                        flags |= FRIEND_ABILITY_DEFENSIVE | FRIEND_ABILITY_SHIELD;
                        break;
                    case SPELL_AURA_MOD_FEAR:
                        flags |= FRIEND_ABILITY_CC | FRIEND_ABILITY_FEAR;
                        break;
                    case SPELL_AURA_MOD_CONFUSE:
                    case SPELL_AURA_MOD_CHARM:
                    case SPELL_AURA_MOD_STUN:
                    case SPELL_AURA_MOD_ROOT:
                        flags |= FRIEND_ABILITY_ROOT;
                        [[fallthrough]];
                    case SPELL_AURA_MOD_PACIFY:
                    case SPELL_AURA_MOD_SILENCE:
                    case SPELL_AURA_MOD_PACIFY_SILENCE:
                    case SPELL_AURA_MOD_DECREASE_SPEED:
                    case SPELL_AURA_MOD_DISARM:
                        flags |= FRIEND_ABILITY_CC;
                        if (aura == SPELL_AURA_MOD_SILENCE || aura == SPELL_AURA_MOD_PACIFY_SILENCE)
                            flags |= FRIEND_ABILITY_INTERRUPT;
                        break;
                    case SPELL_AURA_MOD_TAUNT:
                        flags |= FRIEND_ABILITY_THREAT;
                        break;
                    default:
                        break;
                }
            }
        }

        if (flags & FRIEND_ABILITY_HEAL)
            flags &= ~FRIEND_ABILITY_DAMAGE;

        if (flags & FRIEND_ABILITY_CURE)
            flags &= ~(FRIEND_ABILITY_BUFF_CORE | FRIEND_ABILITY_BUFF_SITUATIONAL);

        return flags;
    }

    void FillRange(const SpellEntry* spellInfo, FriendAbility& ability)
    {
        const SpellRangeEntry* range = sServerFacade.LookupSpellRangeEntry(spellInfo->rangeIndex);
        if (!range)
            return;

        ability.minRange = range->minRange;
        ability.maxRange = range->maxRange;

        if ((range->Flags & SPELL_RANGE_FLAG_MELEE) || (ability.maxRange > 0.0f && ability.maxRange <= 5.0f))
            ability.flags |= FRIEND_ABILITY_MELEE;
        else if (ability.maxRange > 5.0f)
            ability.flags |= FRIEND_ABILITY_RANGED;
    }
}

void FriendAbilityCatalog::Reset()
{
    lastSignature = 0;
    abilities.clear();
}

uint32 FriendAbilityCatalog::BuildSignature(PlayerbotAI* ai) const
{
    if (!ai || !ai->GetBot())
        return 0;

    uint32 signature = 2166136261u;
    for (PlayerSpellMap::const_iterator itr = ai->GetBot()->GetSpellMap().begin(); itr != ai->GetBot()->GetSpellMap().end(); ++itr)
    {
        signature ^= itr->first;
        signature *= 16777619u;
        signature ^= static_cast<uint32>(itr->second.state);
        signature *= 16777619u;
        signature ^= itr->second.disabled ? 1u : 0u;
        signature *= 16777619u;
    }

    if (Pet* pet = ai->GetBot()->GetPet())
    {
        for (PetSpellMap::const_iterator itr = pet->m_spells.begin(); itr != pet->m_spells.end(); ++itr)
        {
            signature ^= itr->first;
            signature *= 16777619u;
            signature ^= static_cast<uint32>(itr->second.state);
            signature *= 16777619u;
        }
    }

    return signature;
}

void FriendAbilityCatalog::Refresh(PlayerbotAI* ai)
{
    const uint32 signature = BuildSignature(ai);
    if (signature == lastSignature)
        return;

    lastSignature = signature;
    abilities.clear();

    if (!ai || !ai->GetBot())
        return;

    std::map<std::string, FriendAbility> byName;
    int loc = ai->GetBot()->GetSession() ? ai->GetBot()->GetSession()->GetSessionDbcLocale() : 0;

    auto addSpell = [&](uint32 spellId)
    {
        const SpellEntry* spellInfo = sServerFacade.LookupSpellInfo(spellId);
        if (!spellInfo || spellInfo->Effect[0] == SPELL_EFFECT_LEARN_SPELL)
            return;

        const char* rawName = spellInfo->SpellName[loc] && spellInfo->SpellName[loc][0] ? spellInfo->SpellName[loc] : spellInfo->SpellName[0];
        if (!rawName || !rawName[0])
            return;

        FriendAbility ability;
        ability.spellId = spellId;
        ability.name = rawName;
        ability.lowerName = ToLower(ability.name);
        ability.duration = GetSpellDuration(spellInfo);
        ability.powerType = spellInfo->powerType;
        ability.manaCost = spellInfo->manaCost;
        ability.manaCostPercent = spellInfo->ManaCostPercentage;
        ability.castTime = GetSpellCastTime(spellInfo, ai->GetBot());
        ability.flags = ClassifyAbility(spellInfo, ability.lowerName, ability.dispelType, ability.dispelMask);
        FillRange(spellInfo, ability);

        if (!ability.flags)
            return;

        std::map<std::string, FriendAbility>::iterator existing = byName.find(ability.lowerName);
        if (existing == byName.end() || existing->second.spellId < ability.spellId)
            byName[ability.lowerName] = ability;
    };

    for (PlayerSpellMap::const_iterator itr = ai->GetBot()->GetSpellMap().begin(); itr != ai->GetBot()->GetSpellMap().end(); ++itr)
    {
        const uint32 spellId = itr->first;
        if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled || IsPassiveSpell(spellId))
            continue;

        addSpell(spellId);
    }

    if (Pet* pet = ai->GetBot()->GetPet())
    {
        for (PetSpellMap::const_iterator itr = pet->m_spells.begin(); itr != pet->m_spells.end(); ++itr)
        {
            const uint32 spellId = itr->first;
            if (itr->second.state == PETSPELL_REMOVED || IsPassiveSpell(spellId))
                continue;

            addSpell(spellId);
        }
    }

    abilities.reserve(byName.size());
    for (std::map<std::string, FriendAbility>::const_iterator itr = byName.begin(); itr != byName.end(); ++itr)
        abilities.push_back(itr->second);
}
