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

    enum class FriendCommand : uint8
    {
        None,
        StayClose,
        ReturnToParty,
        HoldPosition,
        Recover,
        Shop
    };

    enum class FriendMode : uint8
    {
        Party,
        Dungeon,
        Solo
    };

    enum class FriendIdleGoal : uint8
    {
        None,
        Loiter,
        OrbitLeader,
        Resupply,
        GatherNearby,
        GrindNearby
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
        Resupply,
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

    enum class FriendCombatStyle : uint8
    {
        Burn,
        Normal,
        Conserve,
        Dry
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
        bool inTown = false;
        bool nearbyVendor = false;
        bool nearbyRepair = false;
        bool shouldSell = false;
        bool shouldRepair = false;
        bool shouldBuy = false;
        bool lowFood = false;
        bool lowWater = false;
        bool lowAmmo = false;
        bool travelTargetActive = false;
        bool travelTargetPreparing = false;
        bool travelTargetTraveling = false;
        bool healerish = false;
        bool tankish = false;
        bool ranged = false;
        bool botHasThreat = false;
        bool vulnerablePartyHasThreat = false;
        bool healerPartyHasThreat = false;
        uint8 botHealth = 100;
        uint8 botMana = 100;
        uint8 bagSpace = 0;
        uint8 durability = 100;
        int32 botHealthDelta = 0;
        int32 botManaDelta = 0;
        uint8 lowestPartyHealth = 100;
        int32 lowestPartyHealthDelta = 0;
        uint8 damagedPartyMembers = 0;
        uint8 nearbyPartyMembers = 0;
        uint8 attackersCount = 0;
        uint8 attackersTargetingMeCount = 0;
        uint8 possibleTargetsCount = 0;
        uint8 crowdControlledTargets = 0;
        uint8 balance = 100;
        float leaderDistance = 0.0f;
        float targetDistance = 0.0f;
        float nearestHostileDistance = 0.0f;
        ObjectGuid leaderGuid;
        ObjectGuid nearestHostileGuid;
        ObjectGuid closestAttackerTargetingMeGuid;
        ObjectGuid vulnerablePartyAttackerGuid;
        ObjectGuid leaderTargetGuid;
        ObjectGuid rtiTargetGuid;
        ObjectGuid rtiCcTargetGuid;
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

        FriendCommand GetCommand() const { return command; }
        FriendVerbosity GetVerbosity() const { return verbosity; }

    private:
        FriendSituation BuildSituation();
        FriendIntent SelectIntent(const FriendSituation& situation) const;
        bool ExecuteIntent(FriendIntent intent, const FriendSituation& situation);
        bool TryActions(const std::vector<std::string>& names, const std::string& source);
        FriendExecutionResult TryAction(const std::string& name, const std::string& source, uint8 depth = 0);
        FriendExecutionResult TryActionWithParam(const std::string& name, const std::string& param, const std::string& source);
        bool TryPrerequisites(Action* action, const std::string& source, uint8 depth);
        bool TryCatalogDamage(const FriendSituation& situation, const std::string& source);
        bool TryCatalogHeal(const FriendSituation& situation, const std::string& source);
        bool TryCatalogSupport(const FriendSituation& situation, const std::string& source);
        bool TryCatalogCrowdControl(const FriendSituation& situation, const std::string& source);
        bool TryCastAbility(const FriendAbility& ability, Unit* target, const std::string& source);
        bool TryReachAbilityTarget(const FriendAbility& ability, Unit* target, const std::string& source);
        bool TryFreeDamage(const FriendSituation& situation, const std::string& source);
        bool TryFallbackCombat(const FriendSituation& situation, const std::string& source);
        bool TryDruidCombatForm(const FriendSituation& situation, const std::string& source);
        bool MoveToDamageTarget(const FriendSituation& situation, const std::string& action);
        bool MoveToUnitRange(Unit* target, float desiredDistance, const std::string& action);
        bool PrefersMeleeDamage(const FriendSituation& situation) const;
        bool PrefersSelfDefenseTarget(const FriendSituation& situation) const;
        FriendCombatStyle GetCombatStyle(const FriendSituation& situation) const;
        bool ShouldConserveDamageMana(const FriendSituation& situation) const;
        bool IsLowPressureFight(const FriendSituation& situation) const;
        int32 ManaSpendScorePenalty(const FriendSituation& situation, const FriendAbility& ability) const;
        bool ShouldUseLegacySupportActions(const FriendSituation& situation) const;
        Unit* GetDamageTarget(const FriendSituation& situation, bool prepare);
        Unit* SelectDamageTarget(const FriendSituation& situation, bool allowCrowdControlFallback, std::string& reason);
        Unit* GetCrowdControlTarget(const FriendSituation& situation, const FriendAbility& ability, Unit* currentDamageTarget) const;
        bool IsValidFriendDamageTarget(Unit* target, bool allowCrowdControlFallback) const;
        bool ShouldAvoidBreakingCrowdControl(Unit* target) const;
        bool IsSkullTarget(Unit* target) const;
        bool IsMoonTarget(Unit* target) const;
        void SetCurrentDamageTarget(Unit* target, const std::string& reason);
        Unit* GetHealTarget(const FriendSituation& situation) const;
        std::vector<Unit*> GetPartyTargets() const;
        bool ExecuteLoot(const FriendSituation& situation, bool allowObjects = false);
        bool MoveToRecoverPosition(const FriendSituation& situation);
        bool ExecuteResupply(const FriendSituation& situation);
        bool TryTravelForResupply(const FriendSituation& situation);
        bool IsSafeForTownChores(const FriendSituation& situation) const;
        bool NeedsTownChores(const FriendSituation& situation) const;
        bool ExecuteIdleGoal(const FriendSituation& situation);
        bool IsSafeForIdleActivity(const FriendSituation& situation) const;
        FriendIdleGoal SelectIdleGoal(const FriendSituation& situation);
        bool ExecuteCurrentIdleGoal(const FriendSituation& situation);
        bool MoveInLeaderOrbit(const FriendSituation& situation, const std::string& action, bool urgent);
        bool PreferFreeDamage(const FriendSituation& situation) const;
        bool IsTargetSetupAction(const std::string& name) const;
        float PreferredLeaderDistance(const FriendSituation& situation) const;
        float SoftLeashDistance(const FriendSituation& situation) const;
        float HardLeashDistance(const FriendSituation& situation) const;
        bool MoveNearLeader(const FriendSituation& situation, const std::string& action, bool urgent);
        void ClearFriendMovement(bool includePointMove);
        void SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result);
        void MaybeSayStatus(const FriendSituation& situation);
        void ResetTemporaryCommandIfSatisfied(const FriendSituation& situation);
        void ClearIdleState();

        std::vector<std::string> PositionActions(const FriendSituation& situation) const;
        std::vector<std::string> SelfPreservationActions(const FriendSituation& situation) const;
        std::vector<std::string> HealActions(const FriendSituation& situation) const;
        std::vector<std::string> BuffOrCureActions(const FriendSituation& situation) const;
        std::vector<std::string> CrowdControlActions(const FriendSituation& situation) const;
        std::vector<std::string> PullActions(const FriendSituation& situation) const;
        std::vector<std::string> DamageActions(const FriendSituation& situation) const;

        static std::string CommandName(FriendCommand value);
        static std::string ModeName(FriendMode value);
        static std::string IdleGoalName(FriendIdleGoal value);
        static std::string VerbosityName(FriendVerbosity value);
        static std::string IntentName(FriendIntent value);
        static std::string ResultName(FriendExecutionResult value);
        static std::string CombatStyleName(FriendCombatStyle value);
        static std::string BalanceName(uint8 balance);

    private:
        PlayerbotAI* ai;
        FriendMode mode = FriendMode::Party;
        FriendCommand command = FriendCommand::None;
        FriendVerbosity verbosity = FriendVerbosity::Silent;
        FriendIntent lastIntent = FriendIntent::FollowOrIdle;
        FriendExecutionResult lastResult = FriendExecutionResult::None;
        FriendSituation lastSituation;
        std::string lastAction;
        uint8 lastHealth = 100;
        uint8 lastMana = 100;
        uint8 lastLowestPartyHealth = 100;
        std::string lastStatusLine;
        std::string lastTargetReason;
        time_t manualAttackUntil = 0;
        time_t manualHealUntil = 0;
        time_t manualBuffUntil = 0;
        ObjectGuid manualHealGuid;
        FriendIdleGoal idleGoal = FriendIdleGoal::None;
        time_t idleGoalUntil = 0;
        time_t idleNextActionAt = 0;
        bool resupplyTravelRequested = false;
        FriendAbilityCatalog abilityCatalog;
    };
}
