#pragma once

#include "Entities/ObjectGuid.h"
#include "FriendAbilityCatalog.h"
#include <ctime>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class Unit;

namespace ai
{
    class Action;

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
        ImprovePosition,
        RecoverResources,
        SaveSelf,
        SavePartyMember,
        BuffOrCureParty,
        CrowdControl,
        PullWithParty,
        LootNearby,
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
        bool hasPossibleTargets = false;
        bool hasTarget = false;
        bool targetIsElite = false;
        bool hasCreatureLoot = false;
        bool partyInCombat = false;
        bool leaderSafe = false;
        bool leaderInCombat = false;
        bool healerish = false;
        bool tankish = false;
        bool ranged = false;
        uint8 botHealth = 100;
        uint8 botMana = 100;
        int32 botHealthDelta = 0;
        int32 botManaDelta = 0;
        uint8 lowestPartyHealth = 100;
        int32 lowestPartyHealthDelta = 0;
        uint8 damagedPartyMembers = 0;
        uint8 nearbyPartyMembers = 0;
        uint8 attackersCount = 0;
        uint8 possibleTargetsCount = 0;
        uint8 balance = 100;
        float leaderDistance = 0.0f;
        float targetDistance = 0.0f;
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
        FriendExecutionResult TryAction(const std::string& name, const std::string& source, uint8 depth = 0);
        bool TryPrerequisites(Action* action, const std::string& source, uint8 depth);
        bool TryCatalogDamage(const FriendSituation& situation, const std::string& source);
        bool TryCatalogHeal(const FriendSituation& situation, const std::string& source);
        bool TryCatalogSupport(const FriendSituation& situation, const std::string& source);
        bool TryCatalogCrowdControl(const FriendSituation& situation, const std::string& source);
        bool TryCastAbility(const FriendAbility& ability, Unit* target, const std::string& source);
        bool TryReachAbilityTarget(const FriendAbility& ability, Unit* target, const std::string& source);
        bool MoveToDamageTarget(const FriendSituation& situation, const std::string& action);
        bool PrefersMeleeDamage(const FriendSituation& situation) const;
        Unit* GetDamageTarget(const FriendSituation& situation, bool prepare);
        Unit* GetHealTarget(const FriendSituation& situation) const;
        std::vector<Unit*> GetPartyTargets() const;
        bool ExecuteLoot(const FriendSituation& situation);
        bool PreferFreeDamage(const FriendSituation& situation) const;
        bool IsTargetSetupAction(const std::string& name) const;
        float PreferredLeaderDistance(const FriendSituation& situation) const;
        float SoftLeashDistance(const FriendSituation& situation) const;
        float HardLeashDistance(const FriendSituation& situation) const;
        bool MoveNearLeader(const FriendSituation& situation, const std::string& action, bool urgent);
        void ClearFriendMovement(bool includePointMove);
        void SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result);
        void MaybeSayStatus(const FriendSituation& situation);
        void ResetTemporaryAssignmentIfSatisfied(const FriendSituation& situation);

        std::vector<std::string> PositionActions(const FriendSituation& situation) const;
        std::vector<std::string> SelfPreservationActions(const FriendSituation& situation) const;
        std::vector<std::string> HealActions(const FriendSituation& situation) const;
        std::vector<std::string> BuffOrCureActions(const FriendSituation& situation) const;
        std::vector<std::string> CrowdControlActions(const FriendSituation& situation) const;
        std::vector<std::string> PullActions(const FriendSituation& situation) const;
        std::vector<std::string> DamageActions(const FriendSituation& situation) const;

        static std::string AssignmentName(FriendAssignment value);
        static std::string VerbosityName(FriendVerbosity value);
        static std::string IntentName(FriendIntent value);
        static std::string ResultName(FriendExecutionResult value);
        static std::string BalanceName(uint8 balance);

    private:
        PlayerbotAI* ai;
        FriendAssignment assignment = FriendAssignment::ParticipateWithParty;
        FriendVerbosity verbosity = FriendVerbosity::Silent;
        FriendIntent lastIntent = FriendIntent::FollowOrIdle;
        FriendExecutionResult lastResult = FriendExecutionResult::None;
        FriendSituation lastSituation;
        std::string lastAction;
        uint8 lastHealth = 100;
        uint8 lastMana = 100;
        uint8 lastLowestPartyHealth = 100;
        std::string lastStatusLine;
        time_t manualAttackUntil = 0;
        FriendAbilityCatalog abilityCatalog;
    };
}
