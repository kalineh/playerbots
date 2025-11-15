#pragma once

#include "playerbot/strategy/Strategy.h"

namespace ai
{
    class FriendStrategy : public Strategy
    {
    public:
        FriendStrategy(PlayerbotAI* ai) : Strategy(ai) {}

        std::string getName() override { return "friend"; }

    protected:
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
        void InitNonCombatMultipliers(std::list<Multiplier*>& multipliers) override;
        void OnStrategyAdded(BotState state) override;
        void OnStrategyRemoved(BotState state) override;
    };
}
