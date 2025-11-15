#include "playerbot/playerbot.h"
#include "FriendStrategy.h"
#include "playerbot/strategy/generic/ClassStrategy.h"

using namespace ai;

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
}

void FriendStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new FriendModeMultiplier(ai));
}

void FriendStrategy::InitNonCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new FriendModeMultiplier(ai));
}

void FriendStrategy::OnStrategyAdded(BotState)
{
    ai->FriendStrategyAdded();
}

void FriendStrategy::OnStrategyRemoved(BotState)
{
    ai->FriendStrategyRemoved();
}
