#pragma once

#include "Entities/ObjectGuid.h"
#include <ctime>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class Unit;

namespace ai
{
    enum class FriendAssignment : uint8
    {
        ParticipateWithParty,
        StayClose,
        ReturnToParty,
        HoldPosition,
        Recover
    };

    enum class FriendVerbosity : uint8
    {
        Silent,
        Intent,
        Debug
    };

    enum class FriendIntent : uint8
    {
        FollowOrIdle,
        ReturnToParty,
        HoldPosition,
        RecoverResources,
        SaveSelf,
        SavePartyMember,
        DealDamage
    };

    enum class FriendExecutionResult : uint8
    {
        None,
        Done,
        IntentionalIdle,
        BlockedNoAction,
        BlockedNotUseful,
        BlockedNotPossible,
        Failed
    };

    struct FriendSituation
    {
        bool inCombat = false;
        bool inDungeon = false;
        bool hasAttackers = false;
        uint8 botHealth = 100;
        uint8 botMana = 100;
        int32 botHealthDelta = 0;
        uint8 lowestPartyHealth = 100;
        uint8 damagedPartyMembers = 0;
        float leaderDistance = 0.0f;
        ObjectGuid leaderGuid;
    };

    class FriendBotController
    {
    public:
        explicit FriendBotController(PlayerbotAI* ai);

        void Reset();
        void OnFriendModeEnabled();
        void OnFriendModeDisabled();

        void RunTick(bool minimal);
        bool HandleCommand(const std::string& command, Player* requester, std::string& response);
        void Report(Player* requester) const;
        std::string FormatReport() const;

        FriendAssignment GetAssignment() const { return assignment; }
        FriendVerbosity GetVerbosity() const { return verbosity; }

    private:
        FriendSituation BuildSituation();
        FriendIntent SelectIntent(const FriendSituation& situation) const;
        bool ExecuteIntent(FriendIntent intent, const FriendSituation& situation);
        bool TryActions(const std::vector<std::string>& names, const std::string& source);
        FriendExecutionResult TryAction(const std::string& name, const std::string& source);
        void SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result);
        void MaybeSayIntent(FriendIntent intent, const std::string& action);

        static std::string AssignmentName(FriendAssignment value);
        static std::string VerbosityName(FriendVerbosity value);
        static std::string IntentName(FriendIntent value);
        static std::string ResultName(FriendExecutionResult value);

    private:
        PlayerbotAI* ai;
        FriendAssignment assignment = FriendAssignment::ParticipateWithParty;
        FriendVerbosity verbosity = FriendVerbosity::Silent;
        FriendIntent lastIntent = FriendIntent::FollowOrIdle;
        FriendExecutionResult lastResult = FriendExecutionResult::None;
        std::string lastAction;
        uint8 lastHealth = 100;
        time_t lastBarkTime = 0;
    };
}
