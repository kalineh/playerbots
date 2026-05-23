#pragma once

#include "Entities/ObjectGuid.h"
#include "FriendAbilityCatalog.h"
#include <ctime>
#include <initializer_list>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class Creature;
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

    enum class FriendTaskType : uint8
    {
        None,
        HangOut,
        OrbitLeader,
        Resupply,
        GatherNearby,
        GrindNearby,
        TravelToGrind,
        TravelToGather,
        ExploreNearby
    };

    enum class FriendProposal : uint8
    {
        None,
        Resupply
    };

    enum class FriendVerbosity : uint8
    {
        Silent,
        Intent,
        Debug,
        Weights
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
        Gather,
        Grind,
        Explore,
        HangOut,
        DealDamage,
        Max
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
        uint32 sellableItems = 0;
        bool shouldRepair = false;
        bool shouldBuy = false;
        bool lowFood = false;
        bool lowWater = false;
        bool lowAmmo = false;
        bool shouldTrain = false;
        bool shouldUpgradeGear = false;
        bool shouldUpgradeBags = false;
        bool travelTargetActive = false;
        bool travelTargetPreparing = false;
        bool travelTargetTraveling = false;
        bool travelTargetWorking = false;
        bool travelTargetSameMap = false;
        uint8 travelTargetStatus = 0;
        uint32 travelTargetPurpose = 0;
        bool travelTargetDistanceKnown = false;
        uint32 travelTargetDistance = 0;
        std::string travelTargetName;
        std::string targetName;
        bool healerish = false;
        bool tankish = false;
        bool ranged = false;
        bool botHasThreat = false;
        bool vulnerablePartyHasThreat = false;
        bool healerPartyHasThreat = false;
        uint8 botHealth = 100;
        uint8 botMana = 100;
        uint8 botLevel = 1;
        uint8 leaderLevel = 0;
        uint8 bagSpace = 0;
        uint8 durability = 100;
        int32 botHealthDelta = 0;
        int32 botManaDelta = 0;
        uint32 money = 0;
        uint32 trainCost = 0;
        uint32 gearBudget = 0;
        uint32 calmDowntimeSeconds = 0;
        uint8 lowestPartyHealth = 100;
        int32 lowestPartyHealthDelta = 0;
        uint8 damagedPartyMembers = 0;
        uint8 nearbyPartyMembers = 0;
        uint8 attackersCount = 0;
        uint8 attackersTargetingMeCount = 0;
        uint8 possibleTargetsCount = 0;
        uint8 nearbyFightTargetsCount = 0;
        uint8 crowdControlledTargets = 0;
        uint8 balance = 100;
        float leaderDistance = 0.0f;
        float targetDistance = 0.0f;
        float nearestHostileDistance = 0.0f;
        float nearbyFightTargetDistance = 0.0f;
        bool partyHeadingActive = false;
        uint8 partyHeadingConfidence = 0;
        float partyHeadingX = 0.0f;
        float partyHeadingY = 0.0f;
        ObjectGuid leaderGuid;
        ObjectGuid nearestHostileGuid;
        ObjectGuid nearbyFightTargetGuid;
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
        FriendExecutionResult TryAction(const std::string& name, const std::string& source, uint8 depth = 0, Player* owner = nullptr);
        FriendExecutionResult TryActionWithParam(const std::string& name, const std::string& param, const std::string& source);
        FriendExecutionResult TryRequestTravelTarget(uint32 purpose);
        Creature* GetNearbyServiceNpc(uint32 npcFlags) const;
        FriendExecutionResult TryServiceAction(const std::string& name, const std::string& param, uint32 npcFlags);
        bool TryDirectSellItems(Creature* npc, const std::string& qualifier);
        bool TryDirectBuySupplies(Creature*& npc, const FriendSituation& situation);
        void ClearFriendTravelTarget();
        bool TryPrerequisites(Action* action, const std::string& source, uint8 depth, Player* owner);
        bool TryCatalogDamage(const FriendSituation& situation, const std::string& source);
        bool TryCatalogHeal(const FriendSituation& situation, const std::string& source);
        bool TryCatalogSupport(const FriendSituation& situation, const std::string& source);
        bool TryCatalogCrowdControl(const FriendSituation& situation, const std::string& source);
        bool TryCastAbility(const FriendAbility& ability, Unit* target, const std::string& source);
        bool HasEquivalentAura(const FriendAbility& ability, Unit* target) const;
        bool ShouldMoveForAbilityTarget(const FriendAbility& ability, Unit* target) const;
        bool TryReachAbilityTarget(const FriendAbility& ability, Unit* target, const std::string& source);
        bool TryFreeDamage(const FriendSituation& situation, const std::string& source);
        bool TryFallbackCombat(const FriendSituation& situation, const std::string& source);
        bool TryDruidCombatForm(const FriendSituation& situation, const std::string& source);
        bool TryImproveRangedCombatSpacing(const FriendSituation& situation, const std::string& action);
        bool FindRangedCombatPosition(Unit* target, const FriendSituation& situation, float& x, float& y, float& z) const;
        bool IsRangedCombatPositionSafe(Unit* target, const FriendSituation& situation, float x, float y, float z,
            bool avoidPartyStacking) const;
        bool MoveToDamageTarget(const FriendSituation& situation, const std::string& action);
        bool MoveToUnitRange(Unit* target, float desiredDistance, const std::string& action);
        bool PrefersMeleeDamage(const FriendSituation& situation) const;
        bool PrefersSelfDefenseTarget(const FriendSituation& situation) const;
        int32 SelfThreatDangerScore(const FriendSituation& situation) const;
        bool ShouldFightToSurvive(const FriendSituation& situation) const;
        bool CanProtectPartyWithThreat(const FriendSituation& situation) const;
        int32 PartyThreatScore(Unit* victim) const;
        bool CanClassHeal() const;
        bool HasUsableCrowdControlAbility(const FriendSituation& situation) const;
        bool ShouldOpportunisticHeal(const FriendSituation& situation) const;
        FriendCombatStyle GetCombatStyle(const FriendSituation& situation) const;
        bool ShouldConserveDamageMana(const FriendSituation& situation) const;
        bool IsLowPressureFight(const FriendSituation& situation) const;
        int32 ManaSpendScorePenalty(const FriendSituation& situation, const FriendAbility& ability) const;
        int32 ThreatCautionScore(const FriendSituation& situation, Unit* target) const;
        bool ShouldUseLegacySupportActions(const FriendSituation& situation) const;
        Unit* GetDamageTarget(const FriendSituation& situation, bool prepare);
        Unit* SelectDamageTarget(const FriendSituation& situation, bool allowCrowdControlFallback, std::string& reason);
        Unit* GetNearbyFightTarget(const FriendSituation& situation) const;
        bool PrepareNearbyFightTarget(const FriendSituation& situation, const std::string& reason);
        Unit* GetCrowdControlTarget(const FriendSituation& situation, const FriendAbility& ability, Unit* currentDamageTarget) const;
        bool IsValidFriendDamageTarget(Unit* target, bool allowCrowdControlFallback) const;
        bool IsValidNearbyFightTarget(Unit* target, const FriendSituation& situation) const;
        bool ShouldAvoidBreakingCrowdControl(Unit* target) const;
        bool IsCrowdControlTargetWorthwhile(const FriendSituation& situation, const FriendAbility& ability,
            Unit* target, Unit* currentDamageTarget) const;
        bool IsPartyMeleeEngagedWith(Unit* target) const;
        Unit* GetRaidIconTarget(uint8 icon) const;
        bool IsSkullTarget(Unit* target) const;
        bool IsMoonTarget(Unit* target) const;
        void SetCurrentDamageTarget(Unit* target, const std::string& reason);
        Unit* GetHealTarget(const FriendSituation& situation) const;
        std::vector<Unit*> GetPartyTargets() const;
        bool ShouldLootNow(const FriendSituation& situation, bool localPartyInCombat) const;
        bool TryAutoLootRoll(const FriendSituation& situation);
        bool ExecuteLoot(const FriendSituation& situation, bool allowObjects = false);
        bool MoveToRecoverPosition(const FriendSituation& situation);
        bool ExecuteResupply(const FriendSituation& situation);
        bool TryTravelForResupply(const FriendSituation& situation);
        bool MoveToFriendTravelTarget(const FriendSituation& situation);
        bool IsSafeForTownChores(const FriendSituation& situation) const;
        bool NeedsTownChores(const FriendSituation& situation) const;
        bool WantsTownProgression(const FriendSituation& situation) const;
        bool TrySoftTownProgression(const FriendSituation& situation);
        bool TryEquipUpgrades(const FriendSituation& situation, bool force = false);
        bool TrySoftLevelCatchup(const FriendSituation& situation);
        bool TrySoftTraining(const FriendSituation& situation);
        bool TrySoftBagUpgrade(const FriendSituation& situation);
        bool TrySoftGearUpgrade(const FriendSituation& situation);
        bool ForceLevel(uint32 level, Player* requester, std::string& response);
        bool ForceLevelSync(Player* requester, std::string& response);
        bool ForceGearSync(Player* requester, std::string& response);
        bool ForceGearEmpty(Player* requester, std::string& response);
        bool ForceItemClear(Player* requester, std::string& response);
        bool ForceItemJunk(Player* requester, std::string& response);
        bool ApplyFriendLevel(uint32 level);
        uint8 EquippedBagSlots() const;
        Item* FindBestTradeItem(const std::string& fragment) const;
        bool TradeMatchingItem(Player* requester, const std::string& fragment, std::string& response);
        bool TryPendingTrade(const FriendSituation& situation);
        bool PutItemInTrade(Item* item);
        void ClearPendingTrade();
        void MaybeProposeTownChores(const FriendSituation& situation);
        void ClearProposal();
        bool ExecuteTaskIntent(FriendIntent intent, const FriendSituation& situation);
        bool IsSafeForTaskActivity(const FriendSituation& situation) const;
        bool CanContinueTaskActivity(const FriendSituation& situation) const;
        std::string TaskInterruptReason(const FriendSituation& situation) const;
        FriendIntent GetIdleTaskInterruptIntent(const FriendSituation& situation) const;
        FriendTaskType SelectTaskForIntent(FriendIntent intent, const FriendSituation& situation);
        bool ExecuteCurrentTask(const FriendSituation& situation);
        bool ExecuteTaskTravelGoal(const FriendSituation& situation, uint32 purpose, FriendTaskType workTask,
            const std::string& action, std::initializer_list<const char*> lines);
        uint32 SelectGatherTravelPurpose() const;
        bool HasGatherSkill() const;
        void UpdatePartyHeading(FriendSituation& situation, Unit* leader);
        bool IsIdleMovePositionSafe(const FriendSituation& situation, Unit* leader, float x, float y, float z) const;
        bool NormalizeFriendMovePosition(float& x, float& y, float& z) const;
        bool MoveFriendPoint(float x, float y, float z);
        bool MoveToExplorePoint(const FriendSituation& situation);
        bool MoveInLeaderOrbit(const FriendSituation& situation, const std::string& action, bool urgent);
        bool PreferFreeDamage(const FriendSituation& situation) const;
        bool TryStartMeleeAttack(Unit* target, const std::string& source);
        bool IsTargetSetupAction(const std::string& name) const;
        float PreferredLeaderDistance(const FriendSituation& situation) const;
        float SoftLeashDistance(const FriendSituation& situation) const;
        float HardLeashDistance(const FriendSituation& situation) const;
        bool MoveNearLeader(const FriendSituation& situation, const std::string& action, bool urgent);
        void ClearFriendMovement(bool includePointMove);
        void SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result);
        void MaybeSayStatus(const FriendSituation& situation);
        void MaybeSayActivity(const FriendSituation& situation, const std::string& key,
            std::initializer_list<const char*> lines, uint32 chancePercent = 100, uint32 cooldownSeconds = 30);
        void ResetTemporaryCommandIfSatisfied(const FriendSituation& situation);
        void ClearExecutionState();
        int32 IntentFailurePenalty(FriendIntent intent, time_t now) const;
        void AddIntentFailurePenalty(FriendIntent intent, int32 amount);
        void ClearIntentFailurePenalty(FriendIntent intent);
        void ClearIntentFailurePenalties();
        bool PrintInventory(Player* requester, const std::string& filter);
        bool PrintEquipment(Player* requester, const std::string& slotName);
        void PrintHelp(Player* requester) const;

        std::vector<std::string> PositionActions(const FriendSituation& situation) const;
        std::vector<std::string> SelfPreservationActions(const FriendSituation& situation) const;
        std::vector<std::string> HealActions(const FriendSituation& situation) const;
        std::vector<std::string> BuffOrCureActions(const FriendSituation& situation) const;
        std::vector<std::string> CrowdControlActions(const FriendSituation& situation) const;
        std::vector<std::string> PullActions(const FriendSituation& situation) const;
        std::vector<std::string> DamageActions(const FriendSituation& situation) const;

        static std::string CommandName(FriendCommand value);
        static std::string ModeName(FriendMode value);
        static std::string TaskName(FriendTaskType value);
        static std::string ProposalName(FriendProposal value);
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
        FriendProposal pendingProposal = FriendProposal::None;
        FriendSituation lastSituation;
        std::string lastAction;
        uint8 lastHealth = 100;
        uint8 lastMana = 100;
        uint8 lastLowestPartyHealth = 100;
        std::string lastStatusLine;
        mutable std::string lastWeightsLine;
        std::string lastTargetReason;
        time_t manualAttackUntil = 0;
        time_t manualHealUntil = 0;
        time_t manualBuffUntil = 0;
        ObjectGuid manualHealGuid;
        int32 intentFailurePenalty[static_cast<uint8>(FriendIntent::Max)] = {};
        time_t intentFailurePenaltyAt[static_cast<uint8>(FriendIntent::Max)] = {};
        FriendTaskType executionTask = FriendTaskType::None;
        time_t executionTaskUntil = 0;
        time_t executionNextActionAt = 0;
        bool resupplyEquipAttempted = false;
        bool taskTravelRequested = false;
        uint32 taskTravelPurpose = 0;
        time_t proposalExpiresAt = 0;
        time_t nextProposalAt = 0;
        time_t nextSoftLevelCatchupAt = 0;
        time_t nextSoftTrainingAt = 0;
        time_t nextSoftBagUpgradeAt = 0;
        time_t nextSoftGearUpgradeAt = 0;
        time_t nextEquipUpgradeAt = 0;
        time_t nextResupplyAttemptAt = 0;
        time_t lastPlanningBusyAt = 0;
        std::string lastActivityBark;
        time_t nextActivityBarkAt = 0;
        ObjectGuid pendingTradeRequesterGuid;
        std::string pendingTradeFragment;
        time_t pendingTradeUntil = 0;
        time_t nextPendingTradeAt = 0;
        ObjectGuid lastLeaderHeadingGuid;
        uint32 lastLeaderHeadingMap = 0;
        time_t lastLeaderHeadingAt = 0;
        float lastLeaderHeadingX = 0.0f;
        float lastLeaderHeadingY = 0.0f;
        float partyHeadingX = 0.0f;
        float partyHeadingY = 0.0f;
        uint8 partyHeadingConfidence = 0;
        FriendAbilityCatalog abilityCatalog;
    };
}
