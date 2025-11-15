#pragma once

#include "playerbot/strategy/Strategy.h"

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

    class FriendStrategy : public Strategy
    {
    public:
        FriendStrategy(PlayerbotAI* ai) : Strategy(ai) {}

        std::string getName() override { return "friend"; }

    protected:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new FriendModeMultiplier(ai));
        }

        void InitNonCombatMultipliers(std::list<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new FriendModeMultiplier(ai));
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
}
