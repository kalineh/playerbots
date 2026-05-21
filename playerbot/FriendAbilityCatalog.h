#pragma once

#include "Common.h"

#include <string>
#include <vector>

class PlayerbotAI;

namespace ai
{
    enum FriendAbilityFlag : uint32
    {
        FRIEND_ABILITY_DAMAGE = 0x00000001,
        FRIEND_ABILITY_DIRECT_DAMAGE = 0x00000002,
        FRIEND_ABILITY_DOT = 0x00000004,
        FRIEND_ABILITY_HEAL = 0x00000008,
        FRIEND_ABILITY_HOT = 0x00000010,
        FRIEND_ABILITY_BUFF = 0x00000020,
        FRIEND_ABILITY_BUFF_CORE = 0x00000040,
        FRIEND_ABILITY_BUFF_SITUATIONAL = 0x00000080,
        FRIEND_ABILITY_DEFENSIVE = 0x00000100,
        FRIEND_ABILITY_SHIELD = 0x00000200,
        FRIEND_ABILITY_CURE = 0x00000400,
        FRIEND_ABILITY_CC = 0x00000800,
        FRIEND_ABILITY_FEAR = 0x00001000,
        FRIEND_ABILITY_INTERRUPT = 0x00002000,
        FRIEND_ABILITY_AOE = 0x00004000,
        FRIEND_ABILITY_MELEE = 0x00008000,
        FRIEND_ABILITY_RANGED = 0x00010000,
        FRIEND_ABILITY_SUMMON = 0x00020000,
        FRIEND_ABILITY_AURA = 0x00040000,
        FRIEND_ABILITY_THREAT = 0x00080000,
        FRIEND_ABILITY_MOVEMENT = 0x00100000,
        FRIEND_ABILITY_DAMAGE_COOLDOWN = 0x00200000,
        FRIEND_ABILITY_ROOT = 0x00400000
    };

    struct FriendAbility
    {
        uint32 spellId = 0;
        std::string name;
        std::string lowerName;
        uint32 flags = 0;
        uint32 dispelType = 0;
        float minRange = 0.0f;
        float maxRange = 0.0f;
        int32 duration = 0;
        uint32 powerType = 0;
        uint32 manaCost = 0;
        uint32 manaCostPercent = 0;

        bool Has(uint32 flag) const { return (flags & flag) != 0; }
        bool UsesMana() const { return powerType == POWER_MANA && (manaCost > 0 || manaCostPercent > 0); }
    };

    class FriendAbilityCatalog
    {
    public:
        void Reset();
        void Refresh(PlayerbotAI* ai);

        const std::vector<FriendAbility>& GetAbilities() const { return abilities; }

    private:
        uint32 BuildSignature(PlayerbotAI* ai) const;

    private:
        uint32 lastSignature = 0;
        std::vector<FriendAbility> abilities;
    };
}
