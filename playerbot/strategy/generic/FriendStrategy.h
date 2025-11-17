#pragma once

#include "playerbot/strategy/Strategy.h"
#include <algorithm>
#include <sstream>
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"

namespace ai
{
    class FriendModeMultiplier : public Multiplier
    {
    public:
        FriendModeMultiplier(PlayerbotAI* ai) : Multiplier(ai, "friend mode") {}

        float GetValue(Action* action) override
        {
            if (!ai->IsFriendMode())
                return 1.0f;

            float relevance = action->getRelevance();
            if (relevance > ACTION_NORMAL)
                return 1.0f;

            uint32 roll = urand(85, 115);
            return static_cast<float>(roll) / 100.0f;
        }
    };

    class FriendHealPriorityMultiplier : public Multiplier
    {
    public:
        FriendHealPriorityMultiplier(PlayerbotAI* ai) : Multiplier(ai, "friend heal priority") {}

        float GetValue(Action* action) override
        {
            if (!ai->IsFriendMode())
                return 1.0f;

            Unit* healTarget = AI_VALUE(Unit*, "party member to heal");
            const bool hasHealTarget = healTarget && healTarget->IsAlive();
            const bool needsHeal = hasHealTarget && healTarget->GetHealthPercent() < sPlayerbotAIConfig.almostFullHealth;

            const bool isHealAction = IsHealAction(action);

            const uint8 attackerCount = AI_VALUE(uint8, "possible attack targets count");
            const float threatBoost = std::min(0.4f, attackerCount * 0.1f);

            if (isHealAction)
            {
                if (!needsHeal)
                    return 1.0f;

                const float missing = 100.0f - healTarget->GetHealthPercent();
                float bonus = missing / 60.0f;
                bonus = std::clamp(bonus, 0.0f, 0.8f);
                return 1.0f + bonus + threatBoost;
            }

            float multiplier = 1.0f;

            if (needsHeal)
                multiplier *= std::min(1.0f, 0.6f + threatBoost);

            Player* bot = ai->GetBot();
            if (bot->GetPowerType() == POWER_MANA && attackerCount == 0)
            {
                const uint32 maxMana = bot->GetMaxPower(POWER_MANA);
                if (maxMana > 0)
                {
                    const float manaPct = (100.0f * bot->GetPower(POWER_MANA)) / static_cast<float>(maxMana);
                    if (manaPct < sPlayerbotAIConfig.lowMana)
                        multiplier *= 0.5f;
                    else if (manaPct < sPlayerbotAIConfig.mediumHealth)
                        multiplier *= 0.8f;
                }
            }

            return multiplier;
        }

    private:
        bool IsHealAction(Action* action) const
        {
            const uint32 spellId = ai->GetAiObjectContext()->GetValue<uint32>("spell id", action->getName())->Get();
            if (!spellId)
                return false;

            const SpellEntry* spellInfo = sServerFacade.LookupSpellInfo(spellId);
            return spellInfo && PlayerbotAI::IsHealSpell(spellInfo);
        }
    };

    class FriendStrategy : public Strategy
    {
    public:
        FriendStrategy(PlayerbotAI* ai) : Strategy(ai) {}

        std::string getName() override { return "friend"; }

    protected:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new FriendModeMultiplier(ai));
            multipliers.push_back(new FriendHealPriorityMultiplier(ai));
        }

        void InitNonCombatMultipliers(std::list<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new FriendModeMultiplier(ai));
            multipliers.push_back(new FriendHealPriorityMultiplier(ai));
        }

        void OnStrategyAdded(BotState) override
        {
            ai->FriendStrategyAdded();
        }

        void OnStrategyRemoved(BotState) override
        {
            ai->FriendStrategyRemoved();
        }
    };

    class FriendDebugStrategy : public Strategy
    {
    public:
        FriendDebugStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "friend debug"; }
    };
}
