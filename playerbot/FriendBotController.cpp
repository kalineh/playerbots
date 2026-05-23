#include "playerbot/playerbot.h"
#include "FriendBotController.h"

#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "ServerFacade.h"
#include "TravelMgr.h"
#include "strategy/Action.h"
#include "strategy/AiObjectContext.h"
#include "strategy/Strategy.h"
#include "strategy/actions/MovementActions.h"
#include "strategy/values/BudgetValues.h"
#include "strategy/values/ItemUsageValue.h"
#include "strategy/values/LootValues.h"
#include "strategy/values/PossibleAttackTargetsValue.h"
#include "strategy/values/ThreatValues.h"
#include "strategy/values/TravelValues.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <sstream>

using namespace ai;

namespace
{
    const char* FRIEND_BOT_VERSION = "v41";
    const uint8 FRIEND_MANA_BUFF_COMFORT = 75;
    const uint8 FRIEND_MANA_DAMAGE_CONSERVE = 85;
    const float FRIEND_RECOVER_HOSTILE_DISTANCE = 22.0f;
    const float FRIEND_RECOVER_COMFORT_DISTANCE = 24.0f;
    const int32 FRIEND_DOWNTIME_BUFF_MIN_DURATION = 5 * 60 * IN_MILLISECONDS;
    const uint8 FRIEND_REST_DONE_HEALTH = 95;
    const uint8 FRIEND_REST_DONE_MANA = 90;
    const uint32 FRIEND_VENDOR_TRAVEL_PURPOSE = static_cast<uint32>(TravelDestinationPurpose::Vendor);
    const uint32 FRIEND_REPAIR_TRAVEL_PURPOSE = static_cast<uint32>(TravelDestinationPurpose::Repair);
    const uint32 FRIEND_GRIND_TRAVEL_PURPOSE = static_cast<uint32>(TravelDestinationPurpose::Grind);
    const uint8 FRIEND_RTI_MOON = 4;
    const uint8 FRIEND_RTI_SKULL = 7;
    const float FRIEND_CC_MELEE_ENGAGED_DISTANCE = 5.0f;
    const float FRIEND_RANGED_SPACING_MIN = 7.0f;
    const float FRIEND_RANGED_SPACING_WORLD = 13.0f;
    const float FRIEND_RANGED_SPACING_DUNGEON = 10.0f;
    const float FRIEND_RANGED_SPACING_HOSTILE_BUFFER = 14.0f;
    const float FRIEND_RANGED_SPACING_PARTY_BUFFER = 3.0f;
    const uint8 FRIEND_HEAL_TOP_OFF_HEALTH = 92;
    const int32 FRIEND_HEALTH_DROP_NOTICE = -5;
    const int32 FRIEND_HEALTH_DROP_DANGER = -10;
    const uint32 FRIEND_SOFT_LEVEL_CATCHUP_COOLDOWN = 5 * MINUTE;
    const uint32 FRIEND_SOFT_TRAINING_COOLDOWN = 3 * MINUTE;
    const uint32 FRIEND_SOFT_BAG_UPGRADE_COOLDOWN = 5 * MINUTE;
    const uint32 FRIEND_SOFT_GEAR_UPGRADE_COOLDOWN = 10 * MINUTE;
    const uint32 FRIEND_EQUIP_UPGRADE_CHECK_COOLDOWN = 2 * MINUTE;
    const uint32 FRIEND_PROPOSAL_COOLDOWN = 5 * MINUTE;
    const uint32 FRIEND_PROPOSAL_REJECT_COOLDOWN = 10 * MINUTE;
    const uint32 FRIEND_RESUPPLY_RETRY_COOLDOWN = 0;
    const uint32 FRIEND_HEADING_SAMPLE_SECONDS = 3;
    const float FRIEND_HEADING_MIN_STEP = 2.5f;
    const float FRIEND_HEADING_MAX_STEP = 70.0f;
    const uint8 FRIEND_HEADING_MIN_CONFIDENCE = 35;
    const float FRIEND_IDLE_MOVE_HOSTILE_BUFFER = 12.0f;
    const int32 FRIEND_ABILITY_TOP_ROLL_WINDOW = 25;
    const int32 FRIEND_INTENT_FAILURE_DECAY_PER_SECOND = 6;
    const int32 FRIEND_INTENT_FAILURE_MAX_PENALTY = 240;
    const uint32 FRIEND_TRADE_TIMEOUT_SECONDS = 45;
    const float FRIEND_TRADE_MAX_APPROACH_DISTANCE = 80.0f;

    uint32 FriendVendorNpcFlags()
    {
        return UNIT_NPC_FLAG_VENDOR |
            UNIT_NPC_FLAG_VENDOR_AMMO |
            UNIT_NPC_FLAG_VENDOR_FOOD |
            UNIT_NPC_FLAG_VENDOR_POISON |
            UNIT_NPC_FLAG_VENDOR_REAGENT;
    }

    void AddActions(std::vector<std::string>& actions, std::initializer_list<const char*> names)
    {
        for (const char* name : names)
            actions.push_back(name);
    }

    bool IsServiceTravelPurpose(uint32 purpose)
    {
        return purpose == FRIEND_VENDOR_TRAVEL_PURPOSE ||
            purpose == FRIEND_REPAIR_TRAVEL_PURPOSE;
    }

    void ReadFriendTravelSnapshot(Player* bot, TravelTarget* travelTarget, FriendSituation& situation)
    {
        if (!bot || !travelTarget)
            return;

        const TravelStatus status = travelTarget->GetStatus();
        situation.travelTargetStatus = static_cast<uint8>(status);
        situation.travelTargetActive = travelTarget->IsActive();
        situation.travelTargetPreparing = status == TravelStatus::TRAVEL_STATUS_PREPARE;
        situation.travelTargetTraveling = status == TravelStatus::TRAVEL_STATUS_READY ||
            status == TravelStatus::TRAVEL_STATUS_TRAVEL;
        situation.travelTargetWorking = status == TravelStatus::TRAVEL_STATUS_WORK;

        if (travelTarget->GetDestination())
        {
            situation.travelTargetPurpose = static_cast<uint32>(travelTarget->GetDestination()->GetPurpose());
            situation.travelTargetName = travelTarget->GetDestination()->GetTitle();
        }

        WorldPosition* position = travelTarget->GetPosition();
        if (!position)
            return;

        situation.travelTargetSameMap = position->getMapId() == bot->GetMapId();
        if (!situation.travelTargetSameMap)
            return;

        situation.travelTargetDistanceKnown = true;
        situation.travelTargetDistance = static_cast<uint32>(position->distance(bot));
        if (situation.travelTargetName.empty())
            situation.travelTargetName = position->getAreaName(true, true);
    }

    template <typename Candidate>
    void OrderWeightedTopCandidates(std::vector<Candidate>& candidates)
    {
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
        {
            return left.score > right.score;
        });

        if (candidates.size() < 2 || candidates.front().score <= 0)
            return;

        const int32 cutoff = std::max<int32>(1, candidates.front().score - FRIEND_ABILITY_TOP_ROLL_WINDOW);
        uint32 topCount = 0;
        uint32 totalWeight = 0;
        for (const Candidate& candidate : candidates)
        {
            if (candidate.score < cutoff)
                break;

            ++topCount;
            totalWeight += static_cast<uint32>(std::max<int32>(1, candidate.score));
        }

        if (topCount < 2 || !totalWeight)
            return;

        uint32 roll = urand(1, totalWeight);
        uint32 selected = 0;
        for (; selected < topCount; ++selected)
        {
            uint32 weight = static_cast<uint32>(std::max<int32>(1, candidates[selected].score));
            if (roll <= weight)
                break;

            roll -= weight;
        }

        if (selected > 0 && selected < candidates.size())
            std::rotate(candidates.begin(), candidates.begin() + selected, candidates.begin() + selected + 1);
    }

    class FriendPointMovementAction : public MovementAction
    {
    public:
        explicit FriendPointMovementAction(PlayerbotAI* ai) : MovementAction(ai, "friend move") {}

        bool Move(float x, float y, float z)
        {
            return bot && MoveTo(bot->GetMapId(), x, y, z, false, false);
        }

        bool Execute(Event& event) override
        {
            (void)event;
            return false;
        }
    };

    bool IsEliteTarget(PlayerbotAI* ai, Unit* target)
    {
        if (!ai || !target)
            return false;

        Creature* creature = ai->GetCreature(target->GetObjectGuid());
        if (!creature || !creature->GetCreatureInfo())
            return false;

        switch (creature->GetCreatureInfo()->Rank)
        {
            case CREATURE_ELITE_RARE:
            case CREATURE_ELITE_ELITE:
            case CREATURE_ELITE_RAREELITE:
            case CREATURE_ELITE_WORLDBOSS:
                return true;
            default:
                return false;
        }
    }

    bool IsUsableUnit(PlayerbotAI* ai, Unit* unit)
    {
        return ai && ai->GetBot() && unit && unit->IsInWorld() &&
            unit->GetMapId() == ai->GetBot()->GetMapId() &&
            !sServerFacade.UnitIsDead(unit) &&
            ai->IsSafe(unit);
    }

    bool IsHostileTarget(PlayerbotAI* ai, Unit* unit)
    {
        return IsUsableUnit(ai, unit) && sServerFacade.IsHostileTo(ai->GetBot(), unit);
    }

    bool IsFriendlyTarget(PlayerbotAI* ai, Unit* unit)
    {
        return IsUsableUnit(ai, unit) && sServerFacade.IsFriendlyTo(ai->GetBot(), unit);
    }

    uint8 HealthPercent(PlayerbotAI* ai, Unit* unit)
    {
        return IsUsableUnit(ai, unit) ? ai->GetHealthPercent(*unit) : 100;
    }

    template <typename T>
    T GetContextValue(AiObjectContext* context, const std::string& name, T fallback)
    {
        if (!context)
            return fallback;

        auto value = context->GetValue<T>(name);
        return value ? value->Get() : fallback;
    }

    bool IsMovingForAction(PlayerbotAI* ai, const std::string& lastAction, const std::string& action)
    {
        return ai && ai->GetBot() && lastAction == action && sServerFacade.isMoving(ai->GetBot());
    }

    bool StartsWith(const std::string& value, const std::string& prefix)
    {
        return value.length() >= prefix.length() && value.compare(0, prefix.length(), prefix) == 0;
    }

    bool Contains(const std::string& value, const std::string& needle)
    {
        return value.find(needle) != std::string::npos;
    }

    bool IsSelfOnlyCoreBuffName(const std::string& name)
    {
        return Contains(name, "inner fire") ||
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

    uint32 CountInventoryItems(Player* bot)
    {
        if (!bot)
            return 0;

        uint32 count = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                count += item->GetCount();
        }

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            const Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
            if (!pBag)
                continue;

            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                if (Item* item = bot->GetItemByPos(bag, slot))
                    count += item->GetCount();
            }
        }

        return count;
    }

    std::list<Item*> FriendBagItems(Player* bot)
    {
        std::list<Item*> result;
        if (!bot)
            return result;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                result.push_back(item);
        }

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
            if (!pBag)
                continue;

            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                if (Item* item = bot->GetItemByPos(bag, slot))
                    result.push_back(item);
            }
        }

        return result;
    }

    bool ShouldFriendSellItem(PlayerbotAI* ai, Item* item, bool aggressive)
    {
        if (!ai || !ai->GetAiObjectContext() || !item || item->IsInTrade() ||
            !item->GetProto() || !item->GetProto()->SellPrice)
            return false;

        AiObjectContext* context = ai->GetAiObjectContext();
        ItemUsage usage = AI_VALUE2_LAZY(ItemUsage, "item usage", ItemQualifier(item).GetQualifier());
        switch (usage)
        {
            case ItemUsage::ITEM_USAGE_NONE:
            case ItemUsage::ITEM_USAGE_BAD_EQUIP:
            case ItemUsage::ITEM_USAGE_BROKEN_EQUIP:
            case ItemUsage::ITEM_USAGE_AH:
            case ItemUsage::ITEM_USAGE_BROKEN_AH:
            case ItemUsage::ITEM_USAGE_VENDOR:
            case ItemUsage::ITEM_USAGE_FORCE_GREED:
                return true;
            case ItemUsage::ITEM_USAGE_EQUIP:
            case ItemUsage::ITEM_USAGE_SKILL:
            case ItemUsage::ITEM_USAGE_GUILD_TASK:
            case ItemUsage::ITEM_USAGE_DISENCHANT:
                return aggressive;
            default:
                return false;
        }
    }

    std::list<Item*> FriendSellItems(PlayerbotAI* ai, bool aggressive)
    {
        std::list<Item*> result;
        if (!ai || !ai->GetBot())
            return result;

        for (Item* item : FriendBagItems(ai->GetBot()))
        {
            if (ShouldFriendSellItem(ai, item, aggressive))
                result.push_back(item);
        }

        return result;
    }

    void ClearFriendInventoryValues(PlayerbotAI* ai)
    {
        if (!ai || !ai->GetAiObjectContext())
            return;

        AiObjectContext* context = ai->GetAiObjectContext();
        context->ClearValues("bag space");
        context->ClearValues("inventory items");
        context->ClearValues("item count");
        context->ClearValues("item usage");
    }

    bool StoreFriendDebugItem(Player* bot, uint32 itemId, uint32 count, InventoryResult* failure = nullptr)
    {
        if (failure)
            *failure = EQUIP_ERR_OK;

        if (!bot || !itemId || !count || !sObjectMgr.GetItemPrototype(itemId))
        {
            if (failure)
                *failure = EQUIP_ERR_ITEM_NOT_FOUND;
            return false;
        }

        ItemPosCountVec dest;
        InventoryResult result = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count);
        if (result != EQUIP_ERR_OK)
        {
            if (failure)
                *failure = result;
            return false;
        }

        Item* item = bot->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId));
        if (!item)
        {
            if (failure)
                *failure = EQUIP_ERR_INT_BAG_ERROR;
            return false;
        }

        bot->SendNewItem(item, count, true, false);
        return true;
    }

    uint32 CountFriendSellItems(PlayerbotAI* ai, bool aggressive)
    {
        uint32 count = 0;
        for (Item* item : FriendSellItems(ai, aggressive))
        {
            if (item && item->GetProto() && item->GetProto()->SellPrice)
                count += item->GetCount();
        }

        return count;
    }

    void ReportServiceTransaction(PlayerbotAI* ai, const std::string& action, Creature* npc,
        uint32 moneyBefore, uint32 moneyAfter, uint32 itemsBefore, uint32 itemsAfter)
    {
        if (!ai || !ai->GetMaster())
            return;

        std::ostringstream out;
        if (action == "sell" && (moneyAfter > moneyBefore || itemsBefore > itemsAfter))
        {
            out << "Sold";
            if (itemsBefore > itemsAfter)
                out << " " << (itemsBefore - itemsAfter) << " item(s)";
            else
                out << " items";

            if (moneyAfter > moneyBefore)
                out << " for " << ChatHelper::formatMoney(moneyAfter - moneyBefore);
        }
        else if (action == "buy" && (moneyBefore > moneyAfter || itemsAfter > itemsBefore))
        {
            out << "Bought";
            if (itemsAfter > itemsBefore)
                out << " " << (itemsAfter - itemsBefore) << " item(s)";
            else
                out << " supplies";

            if (moneyBefore > moneyAfter)
                out << " for " << ChatHelper::formatMoney(moneyBefore - moneyAfter);
        }
        else if (action == "repair" && moneyBefore > moneyAfter)
        {
            out << "Repaired gear for " << ChatHelper::formatMoney(moneyBefore - moneyAfter);
        }
        else
        {
            return;
        }

        if (npc)
            out << " at " << npc->GetName();

        ai->TellPlayerNoFacing(ai->GetMaster(), out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    }

    const char* TravelStateName(const FriendSituation& situation)
    {
        switch (static_cast<TravelStatus>(situation.travelTargetStatus))
        {
            case TravelStatus::TRAVEL_STATUS_PREPARE: return "prep";
            case TravelStatus::TRAVEL_STATUS_READY: return "ready";
            case TravelStatus::TRAVEL_STATUS_TRAVEL: return "move";
            case TravelStatus::TRAVEL_STATUS_WORK: return "work";
            case TravelStatus::TRAVEL_STATUS_COOLDOWN: return "cooldown";
            case TravelStatus::TRAVEL_STATUS_EXPIRED: return "expired";
            case TravelStatus::TRAVEL_STATUS_NONE: return "none";
            case TravelStatus::MAX_TRAVEL_STATUS: return "unknown";
        }

        return "active";
    }

    void AppendTravelSummary(std::ostringstream& out, const FriendSituation& situation)
    {
        if (!situation.travelTargetActive && !situation.travelTargetPreparing &&
            !situation.travelTargetTraveling && !situation.travelTargetWorking)
            return;

        out << ", travel=" << TravelStateName(situation);
        if (!situation.travelTargetName.empty())
            out << ":" << situation.travelTargetName;
        if (situation.travelTargetDistanceKnown)
            out << "@" << situation.travelTargetDistance << "y";
        else if (situation.travelTargetPurpose)
            out << " purpose=" << situation.travelTargetPurpose;
    }

    bool IsFriendMovementAction(const std::string& action)
    {
        return action == "move near leader" ||
            action == "come" ||
            action == "stay close" ||
            action == "idle orbit" ||
            action == "idle loiter" ||
            action == "recover position" ||
            action == "move to travel target" ||
            action == "stand near travel target" ||
            action == "move to loot" ||
            action == "move to melee" ||
            action == "move for trade" ||
            action == "ranged spacing" ||
            StartsWith(action, "move for spell:");
    }

    bool IsRelaxedFriendMovementAction(const std::string& action)
    {
        return action == "move near leader" ||
            action == "stay close" ||
            action == "idle orbit" ||
            action == "idle loiter";
    }

    bool IsActiveTask(FriendTaskType task)
    {
        return task == FriendTaskType::Resupply ||
            task == FriendTaskType::GatherNearby ||
            task == FriendTaskType::GrindNearby ||
            task == FriendTaskType::TravelToGrind ||
            task == FriendTaskType::TravelToGather ||
            task == FriendTaskType::ExploreNearby;
    }

    FriendIntent IntentForTask(FriendTaskType task)
    {
        switch (task)
        {
            case FriendTaskType::Resupply: return FriendIntent::Resupply;
            case FriendTaskType::GatherNearby:
            case FriendTaskType::TravelToGather:
                return FriendIntent::Gather;
            case FriendTaskType::GrindNearby:
            case FriendTaskType::TravelToGrind:
                return FriendIntent::Grind;
            case FriendTaskType::ExploreNearby:
                return FriendIntent::Explore;
            case FriendTaskType::HangOut:
            case FriendTaskType::OrbitLeader:
                return FriendIntent::HangOut;
            case FriendTaskType::None:
                return FriendIntent::FollowOrIdle;
        }

        return FriendIntent::FollowOrIdle;
    }

    std::string Trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());

        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();

        return value;
    }

    bool Normalize2d(float& x, float& y)
    {
        float length = std::sqrt(x * x + y * y);
        if (length < 0.1f)
            return false;

        x /= length;
        y /= length;
        return true;
    }
}

FriendBotController::FriendBotController(PlayerbotAI* ai) : ai(ai)
{
}

void FriendBotController::Reset()
{
    mode = FriendMode::Party;
    command = FriendCommand::None;
    verbosity = FriendVerbosity::Silent;
    lastIntent = FriendIntent::FollowOrIdle;
    lastResult = FriendExecutionResult::None;
    lastSituation = FriendSituation();
    lastAction.clear();
    lastHealth = ai && ai->GetBot() ? ai->GetBot()->GetHealthPercent() : 100;
    lastMana = 100;
    if (ai && ai->GetBot() && ai->GetBot()->GetMaxPower(POWER_MANA) > 0)
        lastMana = ai->GetManaPercent();
    lastLowestPartyHealth = 100;
    lastStatusLine.clear();
    lastWeightsLine.clear();
    lastTargetReason.clear();
    pendingProposal = FriendProposal::None;
    manualAttackUntil = 0;
    manualHealUntil = 0;
    manualBuffUntil = 0;
    ClearIntentFailurePenalties();
    manualHealGuid = ObjectGuid();
    executionTask = FriendTaskType::None;
    executionTaskUntil = 0;
    executionNextActionAt = 0;
    taskTravelRequested = false;
    taskTravelPurpose = 0;
    proposalExpiresAt = 0;
    nextProposalAt = 0;
    nextSoftLevelCatchupAt = 0;
    nextSoftTrainingAt = 0;
    nextSoftBagUpgradeAt = 0;
    nextSoftGearUpgradeAt = 0;
    nextEquipUpgradeAt = 0;
    nextResupplyAttemptAt = 0;
    lastPlanningBusyAt = time(nullptr);
    lastActivityBark.clear();
    nextActivityBarkAt = 0;
    pendingTradeRequesterGuid = ObjectGuid();
    pendingTradeFragment.clear();
    pendingTradeUntil = 0;
    nextPendingTradeAt = 0;
    lastLeaderHeadingGuid = ObjectGuid();
    lastLeaderHeadingMap = 0;
    lastLeaderHeadingAt = 0;
    lastLeaderHeadingX = 0.0f;
    lastLeaderHeadingY = 0.0f;
    partyHeadingX = 0.0f;
    partyHeadingY = 0.0f;
    partyHeadingConfidence = 0;
    abilityCatalog.Reset();
}

void FriendBotController::OnFriendModeEnabled()
{
    Reset();
}

void FriendBotController::OnFriendModeDisabled()
{
    Reset();
}

void FriendBotController::RunTick(bool minimal)
{
    if (!ai || !ai->GetBot())
        return;

    if (!sServerFacade.IsAlive(ai->GetBot()))
    {
        SetResult(FriendIntent::FollowOrIdle, "", FriendExecutionResult::IntentionalIdle);
        ai->SetActionDuration(minimal ? sPlayerbotAIConfig.reactDelay : sPlayerbotAIConfig.globalCoolDown);
        return;
    }

    abilityCatalog.Refresh(ai);

    FriendSituation situation = BuildSituation();
    lastSituation = situation;
    ResetTemporaryCommandIfSatisfied(situation);

    if (TryAutoLootRoll(situation))
    {
        MaybeSayStatus(situation);
        MaybeProposeTownChores(situation);
        return;
    }

    if (TryPendingTrade(situation))
    {
        MaybeSayStatus(situation);
        MaybeProposeTownChores(situation);
        return;
    }

    if (TryEquipUpgrades(situation))
    {
        MaybeSayStatus(situation);
        MaybeProposeTownChores(situation);
        return;
    }

    FriendIntent intent = SelectIntent(situation);
    lastTargetReason.clear();

    if (!ExecuteIntent(intent, situation))
    {
        SetResult(intent, "", FriendExecutionResult::IntentionalIdle);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    }

    MaybeSayStatus(situation);
    MaybeProposeTownChores(situation);
}

bool FriendBotController::HandleCommand(const std::string& rawCommand, Player* requester, std::string& response)
{
    const std::string trimmedRawCommand = Trim(rawCommand);
    std::string cmd = trimmedRawCommand;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto clearTemporaryState = [&]()
    {
        this->command = FriendCommand::None;
        manualAttackUntil = 0;
        manualHealUntil = 0;
        manualBuffUntil = 0;
        ClearIntentFailurePenalties();
        manualHealGuid = ObjectGuid();
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        nextResupplyAttemptAt = 0;
        ClearPendingTrade();
        ClearProposal();
    };

    if (cmd == "ok" || cmd == "yes")
    {
        if (pendingProposal == FriendProposal::Resupply && proposalExpiresAt >= time(nullptr))
        {
            command = FriendCommand::Shop;
            executionTask = FriendTaskType::Resupply;
            executionTaskUntil = time(nullptr) + 180;
            executionNextActionAt = 0;
            taskTravelRequested = false;
            taskTravelPurpose = 0;
            ClearProposal();
            response = "Okay, I'll handle supplies.";
            return true;
        }

        ClearProposal();
        response = "Nothing pending.";
        return true;
    }

    if (cmd == "no")
    {
        ClearProposal();
        nextProposalAt = time(nullptr) + FRIEND_PROPOSAL_REJECT_COOLDOWN;
        response = "Okay, I'll wait.";
        return true;
    }

    if (cmd == "forcelevelsync" || cmd == "force level sync" || cmd == "level sync")
        return ForceLevelSync(requester, response);

    if (cmd == "forcegearsync" || cmd == "force gear sync" || cmd == "gear sync")
        return ForceGearSync(requester, response);

    if (cmd == "forcegearempty" || cmd == "force gear empty" || cmd == "gear empty")
        return ForceGearEmpty(requester, response);

    if (cmd == "forceitemclear" || cmd == "force itemclear" || cmd == "force item clear" ||
        cmd == "item clear" || cmd == "clearitems" || cmd == "foreceitemclear")
        return ForceItemClear(requester, response);

    if (cmd == "forceitemjunk" || cmd == "force itemjunk" || cmd == "force item junk" ||
        cmd == "item junk" || cmd == "junkitems" || cmd == "foreceitemjunk")
        return ForceItemJunk(requester, response);

    if (StartsWith(cmd, "forcelevel ") || StartsWith(cmd, "force level "))
    {
        std::string levelText = StartsWith(cmd, "forcelevel ") ? Trim(cmd.substr(11)) : Trim(cmd.substr(12));
        uint32 level = static_cast<uint32>(std::atoi(levelText.c_str()));
        return ForceLevel(level, requester, response);
    }

    if (cmd == "party" || cmd == "normal" || cmd == "reset" || cmd == "act normal")
    {
        mode = FriendMode::Party;
        clearTemporaryState();
        response = "Party mode.";
        return true;
    }

    if (cmd == "dungeon")
    {
        mode = FriendMode::Dungeon;
        clearTemporaryState();
        response = "Dungeon mode.";
        return true;
    }

    if (cmd == "solo")
    {
        mode = FriendMode::Solo;
        clearTemporaryState();
        response = "Solo mode.";
        return true;
    }

    if (cmd == "stop" || cmd == "hold" || cmd == "dont move" || cmd == "don't move")
    {
        command = FriendCommand::HoldPosition;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        if (ai)
            ai->StopMoving();
        response = "Holding position.";
        return true;
    }

    if (cmd == "come" || cmd == "come here" || cmd == "return")
    {
        command = FriendCommand::ReturnToParty;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        response = "Coming back.";
        return true;
    }

    if (cmd == "stay close" || cmd == "close")
    {
        command = FriendCommand::StayClose;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        response = "Staying close.";
        return true;
    }

    if (cmd == "recover" || cmd == "drink" || cmd == "rest" || cmd == "rest up")
    {
        command = FriendCommand::Recover;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        response = "Resting up.";
        return true;
    }

    if (cmd == "shop" || cmd == "town" || cmd == "resupply" || cmd == "vendor")
    {
        command = FriendCommand::Shop;
        executionTask = FriendTaskType::Resupply;
        executionTaskUntil = time(nullptr) + 180;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        nextResupplyAttemptAt = 0;
        ClearProposal();
        MaybeSayActivity(lastSituation, "resupply-command", {
            "I'm going to sell junk and restock.",
            "I'll take care of supplies."
        }, 60, 30);
        response = "I'll resupply.";
        return true;
    }

    if (cmd == "help" || cmd == "?")
    {
        PrintHelp(requester);
        return true;
    }

    if (cmd == "version")
    {
        response = std::string("Friend bots ") + FRIEND_BOT_VERSION;
        return true;
    }

    if (cmd == "trade" || StartsWith(cmd, "trade "))
    {
        std::string fragment = cmd == "trade" ? "" : Trim(trimmedRawCommand.substr(6));
        return TradeMatchingItem(requester, fragment, response);
    }

    if (cmd == "items" || StartsWith(cmd, "items "))
    {
        std::string filter = cmd == "items" ? "" : Trim(cmd.substr(6));
        if (!PrintInventory(requester, filter))
            response = "I can't list items right now.";
        return true;
    }

    if (cmd == "equip" || StartsWith(cmd, "equip "))
    {
        std::string slot = cmd == "equip" ? "" : Trim(cmd.substr(6));
        if (!PrintEquipment(requester, slot))
            response = "I can't list equipment right now.";
        return true;
    }

    if (cmd == "attack")
    {
        command = FriendCommand::None;
        manualAttackUntil = time(nullptr) + 20;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        if (requester && requester->GetSelectionGuid() && ai && ai->GetAiObjectContext())
            ai->GetAiObjectContext()->GetValue<ObjectGuid>("attack target")->Set(requester->GetSelectionGuid());
        response = "Attacking with you.";
        return true;
    }

    if (cmd == "heal" || cmd == "heal me")
    {
        command = FriendCommand::None;
        manualHealUntil = time(nullptr) + 20;
        manualHealGuid = requester ? requester->GetObjectGuid() : ObjectGuid();
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        response = "Healing.";
        return true;
    }

    if (cmd == "buff" || cmd == "buff me")
    {
        command = FriendCommand::None;
        manualBuffUntil = time(nullptr) + 30;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        response = "Buffing.";
        return true;
    }

    if (cmd == "summon")
    {
        if (ai && requester && ai->DoSpecificAction("summon", Event("friend command", "", requester), true))
            response = "Summoning.";
        else
            response = "I can't summon right now.";
        return true;
    }

    if (cmd == "report")
    {
        Report(requester);
        return true;
    }

    if (cmd == "verbose" || cmd == "intent")
    {
        verbosity = FriendVerbosity::Intent;
        response = "Intent reporting enabled.";
        return true;
    }

    if (cmd == "debug")
    {
        verbosity = FriendVerbosity::Debug;
        response = "Debug reporting enabled.";
        return true;
    }

    if (cmd == "weights")
    {
        verbosity = FriendVerbosity::Weights;
        response = "Weight reporting enabled.";
        return true;
    }

    if (cmd == "silent")
    {
        verbosity = FriendVerbosity::Silent;
        response = "Debug reporting disabled.";
        return true;
    }

    return false;
}

void FriendBotController::Report(Player* requester) const
{
    if (!ai || !requester)
        return;

    ai->TellPlayerNoFacing(requester, FormatReport(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
}

std::string FriendBotController::FormatReport() const
{
    std::ostringstream out;
    out << "friend " << FRIEND_BOT_VERSION << ": mode=" << ModeName(mode);
    out << ", command=" << CommandName(command);
    if (pendingTradeRequesterGuid)
        out << ", trade=pending";
    out << ", task=" << TaskName(executionTask);
    out << ", proposal=" << ProposalName(pendingProposal);
    out << ", verbosity=" << VerbosityName(verbosity);
    out << ", intent=" << IntentName(lastIntent);
    out << ", action=" << (lastAction.empty() ? "none" : lastAction);
    out << ", result=" << ResultName(lastResult);
    out << ", style=" << CombatStyleName(GetCombatStyle(lastSituation));
    out << ", target=" << (lastTargetReason.empty() ? "none" : lastTargetReason);
    if (!lastSituation.targetName.empty())
        out << ":" << lastSituation.targetName;
    out << ", hp=" << static_cast<uint32>(lastSituation.botHealth) << "%";
    out << ", mana=" << static_cast<uint32>(lastSituation.botMana) << "%";
    out << ", level=" << static_cast<uint32>(lastSituation.botLevel);
    if (lastSituation.leaderLevel)
        out << "/" << static_cast<uint32>(lastSituation.leaderLevel);
    out << ", bag=" << static_cast<uint32>(lastSituation.bagSpace) << "%";
    out << "/" << lastSituation.sellableItems << " sell";
    out << ", dur=" << static_cast<uint32>(lastSituation.durability) << "%";
    out << ", calm=" << lastSituation.calmDowntimeSeconds << "s";
    out << ", town=" << (lastSituation.inTown ? "y" : "n");
    out << ", vendor=" << (lastSituation.nearbyVendor ? "y" : "n");
    out << ", repairNpc=" << (lastSituation.nearbyRepair ? "y" : "n");
    out << ", partyHp=" << static_cast<uint32>(lastSituation.lowestPartyHealth) << "%";
    out << ", balance=" << BalanceName(lastSituation.balance);
    if (lastSituation.botHasThreat)
    {
        out << ", threat=self";
        out << ", danger=" << SelfThreatDangerScore(lastSituation);
        if (ShouldFightToSurvive(lastSituation))
            out << ":fight";
    }
    else if (lastSituation.healerPartyHasThreat)
        out << ", threat=healer";
    else if (lastSituation.vulnerablePartyHasThreat)
        out << ", threat=party";
    out << ", leaderDist=" << static_cast<uint32>(lastSituation.leaderDistance);
    if (lastSituation.partyHeadingActive)
        out << ", heading=" << static_cast<uint32>(lastSituation.partyHeadingConfidence) << "%";
    out << ", targets=" << static_cast<uint32>(lastSituation.possibleTargetsCount);
    AppendTravelSummary(out, lastSituation);
    if (nextResupplyAttemptAt > time(nullptr))
        out << ", resupplyCd=" << static_cast<uint32>(nextResupplyAttemptAt - time(nullptr)) << "s";
    const time_t now = time(nullptr);
    FriendIntent penalizedIntent = FriendIntent::FollowOrIdle;
    int32 topPenalty = 0;
    for (uint8 i = 0; i < static_cast<uint8>(FriendIntent::Max); ++i)
    {
        FriendIntent intent = static_cast<FriendIntent>(i);
        int32 penalty = IntentFailurePenalty(intent, now);
        if (penalty > topPenalty)
        {
            topPenalty = penalty;
            penalizedIntent = intent;
        }
    }
    if (topPenalty > 0)
        out << ", penalty=" << IntentName(penalizedIntent) << ":-" << topPenalty;
    if (lastSituation.nearestHostileGuid)
        out << ", nearHostile=" << static_cast<uint32>(lastSituation.nearestHostileDistance);
    out << ", abilities=" << static_cast<uint32>(abilityCatalog.GetAbilities().size());
    if (!lastWeightsLine.empty())
        out << ", " << lastWeightsLine;
    if (lastSituation.hasCreatureLoot)
        out << ", loot";
    if (NeedsTownChores(lastSituation))
        out << ", chores";
    if (lastSituation.targetIsElite)
        out << ", elite";
    return out.str();
}

FriendSituation FriendBotController::BuildSituation()
{
    FriendSituation situation;
    Player* bot = ai->GetBot();
    AiObjectContext* context = ai->GetAiObjectContext();

    situation.inCombat = sServerFacade.IsInCombat(bot);
    situation.healerish = ai->ContainsStrategy(STRATEGY_TYPE_HEAL);
    situation.tankish = ai->ContainsStrategy(STRATEGY_TYPE_TANK);
    situation.ranged = ai->ContainsStrategy(STRATEGY_TYPE_RANGED) || ai->IsRanged(bot, false);
    situation.botHealth = bot->GetHealthPercent();
    situation.botMana = bot->GetMaxPower(POWER_MANA) > 0 ? ai->GetManaPercent() : 100;
    situation.botLevel = bot->GetLevel();
    situation.money = bot->GetMoney();
    situation.inTown = WorldPosition(bot).HasAreaFlag(AREA_FLAG_CAPITAL);
    situation.botHealthDelta = static_cast<int32>(situation.botHealth) - static_cast<int32>(lastHealth);
    situation.botManaDelta = static_cast<int32>(situation.botMana) - static_cast<int32>(lastMana);
    lastHealth = situation.botHealth;
    lastMana = situation.botMana;

    if (context)
    {
        situation.hasAttackers = GetContextValue<bool>(context, "has attackers", false);
        situation.hasPossibleTargets = GetContextValue<bool>(context, "has possible attack targets", false);
        situation.attackersCount = GetContextValue<uint8>(context, "attackers count", 0);
        situation.possibleTargetsCount = GetContextValue<uint8>(context, "possible attack targets count", 0);
        situation.balance = GetContextValue<uint8>(context, "balance", 100);
        situation.bagSpace = GetContextValue<uint8>(context, "bag space", 0);
        situation.durability = GetContextValue<uint8>(context, "durability inventory", 100);
        ReadFriendTravelSnapshot(bot, context->GetValue<TravelTarget*>("travel target")->Get(), situation);

        const bool aggressiveSell = command == FriendCommand::Shop || situation.bagSpace >= 90;
        uint32 sellItems = CountFriendSellItems(ai, aggressiveSell);
        uint32 minRepairCost = context->GetValue<uint32>("min repair cost")->Get();
        uint32 repairMoney = context->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::repair))->Get();
        situation.trainCost = context->GetValue<uint32>("train cost", TRAINER_TYPE_CLASS)->Get();
        situation.gearBudget = context->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::gear))->Get();
        situation.sellableItems = sellItems;
        situation.shouldSell = sellItems > 0 || situation.bagSpace >= 95;
        situation.shouldRepair = situation.durability < 95 && minRepairCost > 0 && minRepairCost <= repairMoney;
        situation.lowFood = context->GetValue<uint32>("item count", "food")->Get() < 5;
        situation.lowWater = bot->GetMaxPower(POWER_MANA) > 0 && context->GetValue<uint32>("item count", "water")->Get() < 5;
        situation.lowAmmo = bot->getClass() == CLASS_HUNTER && context->GetValue<uint32>("item count", "ammo")->Get() < 200;
        situation.shouldBuy = situation.bagSpace < 90 && (situation.lowFood || situation.lowWater || situation.lowAmmo);
        situation.shouldTrain = situation.trainCost > 0 && situation.trainCost <=
            context->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::spells))->Get();
        situation.shouldUpgradeBags = situation.bagSpace > 70 && EquippedBagSlots() < 4 && situation.gearBudget >= 500;
        uint32 gearCost = std::max<uint32>(1000, (bot->GetLevel() * bot->GetLevel() * bot->GetLevel()) / 2);
        situation.shouldUpgradeGear = situation.gearBudget >= gearCost;

        float closestAttackerTargetingMeDistance = 0.0f;
        std::list<ObjectGuid> attackersTargetingMe = context->GetValue<std::list<ObjectGuid> >("attackers targeting me")->Get();
        for (std::list<ObjectGuid>::const_iterator itr = attackersTargetingMe.begin(); itr != attackersTargetingMe.end(); ++itr)
        {
            Unit* unit = ai->GetUnit(*itr);
            if (!IsHostileTarget(ai, unit))
                continue;

            ++situation.attackersTargetingMeCount;
            situation.botHasThreat = true;

            float distance = sServerFacade.GetDistance2d(bot, unit);
            if (!situation.closestAttackerTargetingMeGuid || distance < closestAttackerTargetingMeDistance)
            {
                situation.closestAttackerTargetingMeGuid = unit->GetObjectGuid();
                closestAttackerTargetingMeDistance = distance;
            }
        }

        LootObjectStack* availableLoot = GetContextValue<LootObjectStack*>(context, "available loot", nullptr);
        if (availableLoot)
        {
            for (uint8 i = 0; i < 10; ++i)
            {
                LootObject loot = availableLoot->GetLoot(sPlayerbotAIConfig.lootDistance);
                if (loot.IsEmpty())
                    break;

                if (loot.guid.IsCreature())
                {
                    situation.hasCreatureLoot = true;
                    break;
                }

                availableLoot->Remove(loot.guid);
            }
        }

        Unit* currentTarget = GetContextValue<Unit*>(context, "current target", nullptr);
        if (currentTarget && !IsValidFriendDamageTarget(currentTarget, true))
        {
            context->GetValue<Unit*>("current target")->Set(nullptr);
            currentTarget = nullptr;
        }

        Unit* dpsTarget = GetContextValue<Unit*>(context, "dps target", nullptr);
        if (dpsTarget && !IsValidFriendDamageTarget(dpsTarget, true))
            dpsTarget = nullptr;

        ObjectGuid attackTargetGuid = GetContextValue<ObjectGuid>(context, "attack target", ObjectGuid());
        if (attackTargetGuid)
        {
            Unit* attackTarget = ai->GetUnit(attackTargetGuid);
            if (!IsValidFriendDamageTarget(attackTarget, true))
                context->GetValue<ObjectGuid>("attack target")->Set(ObjectGuid());
        }

        Unit* target = currentTarget ? currentTarget : dpsTarget;
        if (target)
        {
            situation.hasTarget = true;
            situation.targetDistance = sServerFacade.GetDistance2d(bot, target);
            situation.targetIsElite = IsEliteTarget(ai, target);
            situation.targetName = target->GetName();
        }

        Unit* rtiTarget = GetContextValue<Unit*>(context, "rti target", nullptr);
        if (IsHostileTarget(ai, rtiTarget))
            situation.rtiTargetGuid = rtiTarget->GetObjectGuid();

        Unit* rtiCcTarget = GetContextValue<Unit*>(context, "rti cc target", nullptr);
        if (IsHostileTarget(ai, rtiCcTarget))
            situation.rtiCcTargetGuid = rtiCcTarget->GetObjectGuid();

        int32 bestPartyThreatScore = 0;
        std::list<ObjectGuid> possibleTargets = context->GetValue<std::list<ObjectGuid> >("possible attack targets")->Get();
        for (std::list<ObjectGuid>::const_iterator itr = possibleTargets.begin(); itr != possibleTargets.end(); ++itr)
        {
            Unit* unit = ai->GetUnit(*itr);
            if (!IsHostileTarget(ai, unit))
                continue;

            if (ShouldAvoidBreakingCrowdControl(unit))
                ++situation.crowdControlledTargets;

            Unit* victim = unit->GetVictim();
            if (!victim || !IsFriendlyTarget(ai, victim))
                continue;

            if (victim == bot)
            {
                situation.botHasThreat = true;
                if (!situation.attackersTargetingMeCount)
                    situation.attackersTargetingMeCount = 1;

                float distance = sServerFacade.GetDistance2d(bot, unit);
                if (!situation.closestAttackerTargetingMeGuid || distance < closestAttackerTargetingMeDistance)
                {
                    situation.closestAttackerTargetingMeGuid = unit->GetObjectGuid();
                    closestAttackerTargetingMeDistance = distance;
                }

                continue;
            }

            Player* playerVictim = dynamic_cast<Player*>(victim);
            if (playerVictim && ai->IsHeal(playerVictim))
                situation.healerPartyHasThreat = true;

            int32 partyThreatScore = PartyThreatScore(victim);
            if (HealthPercent(ai, unit) < 35)
                partyThreatScore += 10;

            if (partyThreatScore > 0)
            {
                situation.vulnerablePartyHasThreat = true;
                if (!situation.vulnerablePartyAttackerGuid || partyThreatScore > bestPartyThreatScore ||
                    (partyThreatScore == bestPartyThreatScore &&
                        HealthPercent(ai, unit) < HealthPercent(ai, ai->GetUnit(situation.vulnerablePartyAttackerGuid))))
                {
                    situation.vulnerablePartyAttackerGuid = unit->GetObjectGuid();
                    bestPartyThreatScore = partyThreatScore;
                }
            }
        }

        std::list<ObjectGuid> nearbyNpcs = context->GetValue<std::list<ObjectGuid> >("nearest npcs no los")->Get();
        for (std::list<ObjectGuid>::const_iterator itr = nearbyNpcs.begin(); itr != nearbyNpcs.end(); ++itr)
        {
            Unit* unit = ai->GetUnit(*itr);
            if (!IsHostileTarget(ai, unit))
                continue;

            float distance = sServerFacade.GetDistance2d(bot, unit);
            if (!situation.nearestHostileGuid || distance < situation.nearestHostileDistance)
            {
                situation.nearestHostileGuid = unit->GetObjectGuid();
                situation.nearestHostileDistance = distance;
            }
        }

        std::list<ObjectGuid> interactNpcs = context->GetValue<std::list<ObjectGuid> >("nearest npcs")->Get();
        for (std::list<ObjectGuid>::const_iterator itr = interactNpcs.begin(); itr != interactNpcs.end(); ++itr)
        {
            if (bot->GetNPCIfCanInteractWith(*itr, FriendVendorNpcFlags()))
                situation.nearbyVendor = true;
            if (bot->GetNPCIfCanInteractWith(*itr, UNIT_NPC_FLAG_REPAIR))
                situation.nearbyRepair = true;
        }
    }

    auto visitMember = [&](Player* member)
    {
        if (!member || !ai->IsSafe(member) || !member->IsAlive())
            return;

        if (sServerFacade.GetDistance2d(bot, member) <= sPlayerbotAIConfig.sightDistance)
        {
            ++situation.nearbyPartyMembers;
            uint8 health = member->GetHealthPercent();
            situation.lowestPartyHealth = std::min(situation.lowestPartyHealth, health);
            if (health < sPlayerbotAIConfig.almostFullHealth)
                ++situation.damagedPartyMembers;
            if (member != bot && sServerFacade.IsInCombat(member))
                situation.partyInCombat = true;
        }
    };

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            visitMember(ref->getSource());
    }
    else
    {
        visitMember(bot);
    }

    situation.lowestPartyHealthDelta = static_cast<int32>(situation.lowestPartyHealth) - static_cast<int32>(lastLowestPartyHealth);
    lastLowestPartyHealth = situation.lowestPartyHealth;

    Player* leader = ai->GetGroupMaster();
    if (leader && ai->IsSafe(leader))
    {
        situation.leaderSafe = true;
        situation.leaderGuid = leader->GetObjectGuid();
        situation.leaderDistance = sServerFacade.GetDistance2d(bot, leader);
        situation.leaderLevel = leader->GetLevel();
        situation.leaderInCombat = sServerFacade.IsInCombat(leader);
        situation.partyInCombat = situation.partyInCombat || situation.leaderInCombat;

        Unit* leaderTarget = leader->GetTarget();
        if (IsHostileTarget(ai, leaderTarget))
            situation.leaderTargetGuid = leaderTarget->GetObjectGuid();
    }

    UpdatePartyHeading(situation, leader);

    const time_t now = time(nullptr);
    const bool relaxedFriendMovement = sServerFacade.isMoving(bot) && IsRelaxedFriendMovementAction(lastAction);
    const bool busyPlanningState = situation.inCombat || situation.partyInCombat || situation.hasAttackers ||
        situation.travelTargetPreparing || situation.travelTargetTraveling ||
        (situation.travelTargetActive && !situation.travelTargetWorking) ||
        (sServerFacade.isMoving(bot) && !relaxedFriendMovement);
    if (busyPlanningState || !lastPlanningBusyAt)
        lastPlanningBusyAt = now;

    situation.calmDowntimeSeconds = static_cast<uint32>(std::max<time_t>(0, now - lastPlanningBusyAt));
    return situation;
}

FriendIntent FriendBotController::SelectIntent(const FriendSituation& situation) const
{
    const time_t now = time(nullptr);
    lastWeightsLine.clear();

    if (command == FriendCommand::HoldPosition)
        return FriendIntent::HoldPosition;

    if (command == FriendCommand::ReturnToParty)
        return FriendIntent::ReturnToParty;

    if (command == FriendCommand::Recover && !situation.inCombat)
        return FriendIntent::RecoverResources;

    const bool soloFree = mode == FriendMode::Solo && command == FriendCommand::None;
    const bool partyNearby = !soloFree || !situation.leaderSafe || situation.leaderDistance <= SoftLeashDistance(situation);
    const bool localPartyInCombat = situation.partyInCombat && partyNearby;
    const bool activeTask = executionTask != FriendTaskType::None && executionTaskUntil > now;
    const bool activeResupplyTask = activeTask && IntentForTask(executionTask) == FriendIntent::Resupply;

    float softLeash = SoftLeashDistance(situation);
    float hardLeash = HardLeashDistance(situation);
    if (!activeResupplyTask && !soloFree && situation.leaderSafe && situation.leaderDistance > hardLeash &&
        (!situation.inCombat || mode == FriendMode::Dungeon || !situation.hasAttackers))
        return FriendIntent::ReturnToParty;

    const bool fragileThreat = situation.botHasThreat && !situation.tankish &&
        (situation.ranged || situation.healerish || CanClassHeal()) &&
        SelfThreatDangerScore(situation) >= 60;
    const bool fightSelfThreat = ShouldFightToSurvive(situation);

    const bool selfHealthPressure = situation.botHealth < sPlayerbotAIConfig.lowHealth ||
        situation.botHealthDelta <= FRIEND_HEALTH_DROP_DANGER ||
        (situation.botHealth < sPlayerbotAIConfig.mediumHealth && situation.botHealthDelta <= FRIEND_HEALTH_DROP_NOTICE);
    const bool selfActivelyThreatened = situation.botHasThreat || situation.hasAttackers ||
        situation.attackersTargetingMeCount > 0;
    if (selfHealthPressure)
    {
        if (fightSelfThreat && SelfThreatDangerScore(situation) < 80)
            return FriendIntent::DealDamage;

        if (CanClassHeal() && !selfActivelyThreatened)
            return FriendIntent::SavePartyMember;

        return FriendIntent::SaveSelf;
    }

    if (fragileThreat)
        return FriendIntent::SaveSelf;

    if (manualHealUntil > now)
        return FriendIntent::SavePartyMember;

    if (partyNearby && (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_DANGER ||
        (situation.lowestPartyHealth < sPlayerbotAIConfig.mediumHealth && situation.damagedPartyMembers)))
        return FriendIntent::SavePartyMember;

    if (partyNearby && ShouldOpportunisticHeal(situation))
        return FriendIntent::SavePartyMember;

    if (situation.inCombat && situation.attackersTargetingMeCount > 0 && fightSelfThreat)
        return FriendIntent::DealDamage;

    if (situation.inCombat && PrefersSelfDefenseTarget(situation) && situation.attackersTargetingMeCount > 0 &&
        SelfThreatDangerScore(situation) >= 45)
        return FriendIntent::CrowdControl;

    if (situation.inCombat && (((situation.ranged || situation.healerish) && situation.hasAttackers) ||
        (situation.ranged && situation.targetDistance > 0.0f && situation.targetDistance < 8.0f)))
        return FriendIntent::ImprovePosition;

    if (!situation.inCombat && situation.botMana < sPlayerbotAIConfig.lowMana &&
        (!situation.leaderInCombat || !partyNearby))
        return FriendIntent::RecoverResources;

    if (manualBuffUntil > now && !situation.inCombat && !localPartyInCombat)
        return FriendIntent::BuffOrCureParty;

    if (situation.inCombat && mode == FriendMode::Dungeon && situation.possibleTargetsCount > 1 && !situation.tankish)
        return FriendIntent::CrowdControl;

    if (ShouldLootNow(situation, localPartyInCombat))
        return FriendIntent::LootNearby;

    if (activeTask && !situation.inCombat && !localPartyInCombat)
        return IntentForTask(executionTask);

    const bool serviceTravelTarget = IsServiceTravelPurpose(taskTravelPurpose) ||
        IsServiceTravelPurpose(situation.travelTargetPurpose);
    const bool resupplyInProgress = serviceTravelTarget &&
        (taskTravelRequested || situation.travelTargetPreparing || situation.travelTargetTraveling ||
         (situation.travelTargetActive && !situation.travelTargetWorking));
    const bool townAccessNearby = situation.inTown || situation.nearbyVendor || situation.nearbyRepair;
    const bool relaxedOutOfCombat = !situation.inCombat && !localPartyInCombat &&
        (!partyNearby || situation.lowestPartyHealth >= FRIEND_HEAL_TOP_OFF_HEALTH);
    const bool resupplyAllowed = command == FriendCommand::Shop || now >= nextResupplyAttemptAt;

    if (!soloFree && command == FriendCommand::None && relaxedOutOfCombat &&
        situation.leaderSafe && situation.leaderDistance > softLeash)
        return FriendIntent::ReturnToParty;

    if (relaxedOutOfCombat && IsSafeForTaskActivity(situation) &&
        (mode == FriendMode::Solo || mode == FriendMode::Party))
    {
        struct WeightedIntent
        {
            FriendIntent intent;
            int32 score;
        };

        std::vector<WeightedIntent> candidates;
        auto add = [&](FriendIntent intent, int32 score)
        {
            if (score > 0)
            {
                const bool explicitCommandIntent = command == FriendCommand::Shop && intent == FriendIntent::Resupply;
                const int32 penalty = IntentFailurePenalty(intent, now);
                if (penalty > 0)
                {
                    score -= explicitCommandIntent ? penalty / 4 : penalty;
                    if (score <= 0)
                        return;
                }

                if (intent == FriendIntent::PullWithParty && command == FriendCommand::None)
                    score = std::max<int32>(1, score - IntentFailurePenalty(FriendIntent::Grind, now) / 2);

                if (score <= 0)
                    return;

                for (WeightedIntent& candidate : candidates)
                {
                    if (candidate.intent == intent)
                    {
                        candidate.score += score;
                        return;
                    }
                }

                candidates.push_back({ intent, score });
            }
        };

        const uint32 personality = ai && ai->GetBot() ? ai->GetBot()->GetObjectGuid().GetCounter() : 0;
        const int32 boredom = static_cast<int32>(std::min<uint32>(360, situation.calmDowntimeSeconds * 6));
        const int32 socialBias = static_cast<int32>(personality % 25);
        const int32 gatherBias = static_cast<int32>((personality / 7) % 30);
        const int32 grindBias = static_cast<int32>((personality / 13) % 30);
        const int32 forwardBias = (mode != FriendMode::Dungeon && situation.partyHeadingActive) ?
            static_cast<int32>(situation.partyHeadingConfidence) : 0;
        const bool partyComfortable = situation.leaderSafe &&
            situation.leaderDistance <= SoftLeashDistance(situation) &&
            situation.botHealth >= FRIEND_HEAL_TOP_OFF_HEALTH &&
            situation.lowestPartyHealth >= FRIEND_HEAL_TOP_OFF_HEALTH;
        const bool urgentTownChores = command == FriendCommand::Shop ||
            situation.shouldRepair || situation.shouldSell || situation.shouldBuy;
        const bool safePullOpportunity = command == FriendCommand::None &&
            mode != FriendMode::Dungeon &&
            situation.leaderSafe &&
            situation.leaderDistance <= sPlayerbotAIConfig.reactDistance &&
            situation.nearbyPartyMembers >= 1 &&
            situation.possibleTargetsCount > 0 &&
            situation.possibleTargetsCount <= 2 &&
            situation.balance >= 100 &&
            situation.botHealth >= sPlayerbotAIConfig.mediumHealth &&
            situation.botMana >= sPlayerbotAIConfig.lowMana &&
            !situation.damagedPartyMembers;

        if (activeTask)
            add(IntentForTask(executionTask), 80);

        if (command == FriendCommand::Shop && IsSafeForTownChores(situation))
            add(FriendIntent::Resupply, 1000);

        if (NeedsTownChores(situation) && IsSafeForTownChores(situation) && resupplyAllowed &&
            (command == FriendCommand::Shop || townAccessNearby || resupplyInProgress || mode == FriendMode::Solo))
            add(FriendIntent::Resupply, urgentTownChores ? 160 : 70);

        if (mode != FriendMode::Dungeon)
        {
            if (HasGatherSkill() && situation.botMana >= sPlayerbotAIConfig.lowMana)
                add(FriendIntent::Gather, mode == FriendMode::Solo ?
                    38 + gatherBias + boredom / 4 + forwardBias / 5 :
                    18 + gatherBias + boredom / 5 + forwardBias / 10);

            if (situation.possibleTargetsCount > 0 && situation.botHealth >= FRIEND_HEAL_TOP_OFF_HEALTH &&
                situation.botMana >= sPlayerbotAIConfig.lowMana)
                add(FriendIntent::Grind, mode == FriendMode::Solo ?
                    115 + grindBias + boredom + forwardBias / 4 :
                    (partyComfortable ? 70 + grindBias + boredom * 2 + forwardBias / 6 : 0));

            if (safePullOpportunity)
                add(FriendIntent::PullWithParty, partyComfortable ?
                    70 + grindBias / 2 + boredom + forwardBias / 8 : 30);

            if (!urgentTownChores && situation.botHealth >= FRIEND_HEAL_TOP_OFF_HEALTH &&
                situation.botMana >= sPlayerbotAIConfig.lowMana)
                add(FriendIntent::Explore, mode == FriendMode::Solo ?
                    34 + boredom / 3 + forwardBias / 4 :
                    (partyComfortable ? 16 + boredom / 4 + forwardBias / 6 : 0));
        }

        add(FriendIntent::BuffOrCureParty, mode == FriendMode::Solo ? 8 : 18);
        add(FriendIntent::HangOut, mode == FriendMode::Solo ? 28 + socialBias / 2 : 30 + socialBias / 2);

        if (candidates.empty())
            return FriendIntent::FollowOrIdle;

        OrderWeightedTopCandidates(candidates);

        if (verbosity == FriendVerbosity::Weights)
        {
            std::ostringstream out;
            out << "weights:";
            uint8 printed = 0;
            for (const WeightedIntent& candidate : candidates)
            {
                if (printed++ >= 6)
                    break;
                out << " " << IntentName(candidate.intent) << "=" << candidate.score;
            }
            lastWeightsLine = out.str();
        }

        return candidates.front().intent;
    }

    if (relaxedOutOfCombat)
        return FriendIntent::BuffOrCureParty;

    if (situation.inCombat || localPartyInCombat || situation.hasAttackers || situation.hasTarget || now < manualAttackUntil)
        return FriendIntent::DealDamage;

    if (!soloFree && situation.leaderSafe && situation.leaderDistance > softLeash)
        return FriendIntent::ReturnToParty;

    if (!soloFree && !situation.inCombat && !localPartyInCombat && situation.leaderSafe &&
        situation.leaderDistance > PreferredLeaderDistance(situation))
        return FriendIntent::ReturnToParty;

    return FriendIntent::FollowOrIdle;
}

bool FriendBotController::ExecuteIntent(FriendIntent intent, const FriendSituation& situation)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    const bool movingForFriendAction = bot && sServerFacade.isMoving(bot) && IsFriendMovementAction(lastAction);
    const bool continuingFriendMovement =
        movingForFriendAction &&
        (intent == FriendIntent::Resupply ||
         intent == FriendIntent::Gather ||
         intent == FriendIntent::Grind ||
         intent == FriendIntent::Explore ||
         intent == FriendIntent::HangOut ||
         intent == lastIntent);
    const bool keepFriendMovement = intent == FriendIntent::ReturnToParty ||
        intent == FriendIntent::HoldPosition ||
        (intent == FriendIntent::FollowOrIdle && command == FriendCommand::StayClose) ||
        continuingFriendMovement;
    if (!keepFriendMovement)
        ClearFriendMovement(true);

    lastIntent = intent;

    switch (intent)
    {
        case FriendIntent::HoldPosition:
            ai->StopMoving();
            SetResult(intent, "hold", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
            return true;

        case FriendIntent::ReturnToParty:
            return MoveNearLeader(situation,
                command == FriendCommand::ReturnToParty ? "come" : "move near leader",
                command == FriendCommand::ReturnToParty);

        case FriendIntent::ImprovePosition:
            if (TryImproveRangedCombatSpacing(situation, "ranged spacing"))
                return true;
            if (TryActions(PositionActions(situation), "friend position"))
                return true;
            if (situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            if (ShouldConserveDamageMana(situation) && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend fallback damage");

        case FriendIntent::RecoverResources:
            if (!situation.inCombat)
            {
                if (MoveToRecoverPosition(situation))
                    return true;

                if (TryActions({ "drink", "food" }, "friend recover"))
                {
                    MaybeSayActivity(situation, "recover", {
                        "Resting for a moment.",
                        "Drinking for a moment."
                    }, 30, 60);
                    return true;
                }

                if (TryAction("sit", "friend recover") == FriendExecutionResult::Done)
                {
                    MaybeSayActivity(situation, "recover", {
                        "Resting for a moment.",
                        "Taking a breather."
                    }, 20, 60);
                    ai->SetActionDuration(std::max(sPlayerbotAIConfig.globalCoolDown, sPlayerbotAIConfig.reactDelay));
                    return true;
                }

                return false;
            }
            if (TryActions({ "mana gem", "mana potion", "dark rune", "life tap", "dark pact" }, "friend fallback combat recover"))
                return true;
            if (ShouldConserveDamageMana(situation) && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend fallback damage");

        case FriendIntent::Resupply:
            return ExecuteTaskIntent(intent, situation);

        case FriendIntent::SaveSelf:
            if (ShouldFightToSurvive(situation))
            {
                if (TryCatalogDamage(situation, "friend fight back"))
                    return true;
                if (TryFreeDamage(situation, "friend fight back"))
                    return true;
            }
            if (TryActions(SelfPreservationActions(situation), "friend fallback self"))
            {
                if (situation.botHealth < sPlayerbotAIConfig.lowHealth ||
                    situation.botHealthDelta <= FRIEND_HEALTH_DROP_DANGER ||
                    (situation.botHasThreat && !situation.tankish &&
                     (situation.ranged || situation.healerish || CanClassHeal())))
                    MaybeSayActivity(situation, "self-trouble", {
                        "I'm in trouble.",
                        "Need a second here.",
                        "I've got aggro."
                    }, 40, 60);
                return true;
            }
            if (TryDruidCombatForm(situation, "friend self form"))
                return true;
            if (situation.botHasThreat && !situation.tankish &&
                (situation.ranged || situation.healerish || CanClassHeal()))
            {
                MaybeSayActivity(situation, "self-aggro", {
                    "I need help.",
                    "I've got aggro."
                }, 35, 45);
            }
            if (!situation.hasAttackers && (situation.inCombat || situation.partyInCombat || situation.hasTarget))
                return TryFallbackCombat(situation, "friend self fallback");
            return false;

        case FriendIntent::SavePartyMember:
            if (TryCatalogHeal(situation, "friend heal"))
            {
                if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
                    situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_DANGER)
                    MaybeSayActivity(situation, "heal-pressure", {
                        "Hold on, healing.",
                        "I've got heals."
                    }, 30, 45);
                return true;
            }
            if (TryActions(HealActions(situation), "friend fallback heal"))
            {
                if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
                    situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_DANGER)
                    MaybeSayActivity(situation, "heal-pressure", {
                        "Hold on, healing.",
                        "I've got heals."
                    }, 30, 45);
                return true;
            }
            if (situation.inCombat || situation.partyInCombat)
                return TryFallbackCombat(situation, "friend party fallback");
            return false;

        case FriendIntent::BuffOrCureParty:
            if (TryCatalogSupport(situation, "friend support"))
                return true;
            if (ShouldUseLegacySupportActions(situation) && TryActions(BuffOrCureActions(situation), "friend fallback support"))
                return true;
            if (situation.hasCreatureLoot && ExecuteLoot(situation))
                return true;
            if (situation.leaderSafe && situation.leaderDistance > PreferredLeaderDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            return false;

        case FriendIntent::CrowdControl:
            if (TryCatalogCrowdControl(situation, "friend cc"))
                return true;
            if (TryActions(CrowdControlActions(situation), "friend fallback cc"))
                return true;
            if (ShouldConserveDamageMana(situation) && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend fallback damage");

        case FriendIntent::PullWithParty:
            if (TryActions(PullActions(situation), "friend pull"))
            {
                MaybeSayActivity(situation, "pull", {
                    "I'll pull one back.",
                    "Pulling one over."
                }, 35, 45);
                return true;
            }
            if (GetDamageTarget(situation, true))
            {
                if (TryFreeDamage(situation, "friend pull"))
                    return true;
                if (TryCatalogDamage(situation, "friend pull"))
                    return true;
                if (PrefersMeleeDamage(situation) && MoveToDamageTarget(situation, "pull engage"))
                    return true;
                if (TryActions(DamageActions(situation), "friend pull fallback"))
                    return true;
            }
            if (ExecuteTaskIntent(FriendIntent::Grind, situation))
                return true;
            AddIntentFailurePenalty(FriendIntent::PullWithParty, 80);
            return false;

        case FriendIntent::LootNearby:
            if (ExecuteLoot(situation))
            {
                MaybeSayActivity(situation, "loot", {
                    "I'll grab the loot.",
                    "Looting quick."
                }, mode == FriendMode::Solo ? 35 : 18, 45);
                return true;
            }
            return false;

        case FriendIntent::Gather:
        case FriendIntent::Grind:
        case FriendIntent::Explore:
        case FriendIntent::HangOut:
            return ExecuteTaskIntent(intent, situation);

        case FriendIntent::DealDamage:
            GetDamageTarget(situation, true);
            if (ShouldOpportunisticHeal(situation) && TryCatalogHeal(situation, "friend top off"))
                return true;
            if (TryImproveRangedCombatSpacing(situation, "ranged spacing"))
                return true;
            if (GetCombatStyle(situation) == FriendCombatStyle::Dry && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (Unit* threatTarget = GetDamageTarget(situation, true))
            {
                if (ThreatCautionScore(situation, threatTarget) >= 55 &&
                    TryFreeDamage(situation, "friend low threat"))
                    return true;
            }
            if (ai && ai->GetBot() && ai->GetBot()->getClass() == CLASS_DRUID &&
                (ShouldConserveDamageMana(situation) || situation.botHasThreat ||
                 situation.botMana < sPlayerbotAIConfig.mediumMana) &&
                TryDruidCombatForm(situation, "friend druid form"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            if (TryFreeDamage(situation, "friend basic damage"))
                return true;
            if (TryActions(DamageActions(situation), "friend fallback damage"))
                return true;
            if (MoveToDamageTarget(situation, "move to attack"))
                return true;
            if (situation.partyInCombat && situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            return false;

        case FriendIntent::FollowOrIdle:
            if (command == FriendCommand::StayClose && situation.leaderGuid)
                return MoveInLeaderOrbit(situation, "stay close", false);
            if (ExecuteTaskIntent(FriendIntent::FollowOrIdle, situation))
                return true;
            ClearFriendMovement(false);
            SetResult(intent, "", FriendExecutionResult::IntentionalIdle);
            return false;
    }

    return false;
}

bool FriendBotController::TryActions(const std::vector<std::string>& names, const std::string& source)
{
    for (const std::string& name : names)
    {
        FriendExecutionResult result = TryAction(name, source);
        if (result == FriendExecutionResult::Done && !IsTargetSetupAction(name))
            return true;
    }

    return false;
}

FriendExecutionResult FriendBotController::TryAction(const std::string& name, const std::string& source, uint8 depth, Player* owner)
{
    if (!ai || !ai->GetAiObjectContext())
        return FriendExecutionResult::BlockedNoAction;

    const bool fallbackAction = source.find("fallback") != std::string::npos &&
        !IsFriendMovementAction(name) && !StartsWith(name, "reach ");
    const std::string displayName = fallbackAction ? "fallback:" + name : name;

    Action* action = ai->GetAiObjectContext()->GetAction(name);
    if (!action)
    {
        SetResult(lastIntent, displayName, FriendExecutionResult::BlockedNoAction);
        return FriendExecutionResult::BlockedNoAction;
    }

    action->SetReaction(false);
    action->setRelevance(ACTION_NORMAL);
    action->MakeVerbose(false);

    if (!action->isUseful())
    {
        SetResult(lastIntent, displayName, FriendExecutionResult::BlockedNotUseful);
        return FriendExecutionResult::BlockedNotUseful;
    }

    if (TryPrerequisites(action, source, depth, owner) && !action->isPossible())
        return FriendExecutionResult::Done;

    if (!action->isPossible())
    {
        SetResult(lastIntent, displayName, FriendExecutionResult::BlockedNotPossible);
        return FriendExecutionResult::BlockedNotPossible;
    }

    Event event(source, "", owner ? owner : ai->GetMaster());
    bool executed = action->Execute(event);
    FriendExecutionResult result = executed ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
    if (executed)
        ai->SetActionDuration(action);

    SetResult(lastIntent, displayName, result);
    return result;
}

FriendExecutionResult FriendBotController::TryActionWithParam(const std::string& name, const std::string& param, const std::string& source)
{
    if (!ai || !ai->GetAiObjectContext())
        return FriendExecutionResult::BlockedNoAction;

    Action* action = ai->GetAiObjectContext()->GetAction(name);
    if (!action)
    {
        SetResult(lastIntent, name + ":" + param, FriendExecutionResult::BlockedNoAction);
        return FriendExecutionResult::BlockedNoAction;
    }

    action->SetReaction(false);
    action->setRelevance(ACTION_NORMAL);
    action->MakeVerbose(false);

    if (!action->isUseful())
    {
        SetResult(lastIntent, name + ":" + param, FriendExecutionResult::BlockedNotUseful);
        return FriendExecutionResult::BlockedNotUseful;
    }

    if (!action->isPossible())
    {
        SetResult(lastIntent, name + ":" + param, FriendExecutionResult::BlockedNotPossible);
        return FriendExecutionResult::BlockedNotPossible;
    }

    Event event(source, param, ai->GetMaster());
    bool executed = action->Execute(event);
    FriendExecutionResult result = executed ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
    if (executed)
        ai->SetActionDuration(action);

    SetResult(lastIntent, name + ":" + param, result);
    return result;
}

FriendExecutionResult FriendBotController::TryRequestTravelTarget(uint32 purpose)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext())
        return FriendExecutionResult::BlockedNoAction;

    Player* bot = ai->GetBot();
    AiObjectContext* context = ai->GetAiObjectContext();
    const std::string actionName = "request travel target::" + std::to_string(purpose);
    Action* action = context->GetAction(actionName);
    if (!action)
    {
        SetResult(lastIntent, actionName, FriendExecutionResult::BlockedNoAction);
        return FriendExecutionResult::BlockedNoAction;
    }

    TravelTarget* travelTarget = context->GetValue<TravelTarget*>("travel target")->Get();
    if (bot->InBattleGround() || !ai->AllowActivity(TRAVEL_ACTIVITY) ||
        !GetContextValue<bool>(context, "can move around", false) ||
        !travelTarget || travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE ||
        travelTarget->IsActive() ||
        context->GetValue<bool>("no active travel destinations", std::to_string(purpose))->Get())
    {
        SetResult(lastIntent, actionName, FriendExecutionResult::BlockedNotUseful);
        return FriendExecutionResult::BlockedNotUseful;
    }

    action->SetReaction(false);
    action->setRelevance(ACTION_NORMAL);
    action->MakeVerbose(false);

    if (!action->isPossible())
    {
        SetResult(lastIntent, actionName, FriendExecutionResult::BlockedNotPossible);
        return FriendExecutionResult::BlockedNotPossible;
    }

    Event event("", "", bot);
    bool executed = action->Execute(event);
    FriendExecutionResult result = executed ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
    if (executed)
        ai->SetActionDuration(action);

    SetResult(lastIntent, actionName, result);
    return result;
}

Creature* FriendBotController::GetNearbyServiceNpc(uint32 npcFlags) const
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext())
        return nullptr;

    Player* bot = ai->GetBot();
    std::list<ObjectGuid> npcs = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("nearest npcs")->Get();
    for (std::list<ObjectGuid>::const_iterator itr = npcs.begin(); itr != npcs.end(); ++itr)
    {
        Creature* npc = bot->GetNPCIfCanInteractWith(*itr, npcFlags);
        if (npc)
            return npc;
    }

    return nullptr;
}

FriendExecutionResult FriendBotController::TryServiceAction(const std::string& name, const std::string& param, uint32 npcFlags)
{
    Creature* npc = GetNearbyServiceNpc(npcFlags);
    const std::string displayName = param.empty() ? name : name + ":" + param;
    if (!npc)
    {
        SetResult(lastIntent, displayName + ":no npc", FriendExecutionResult::BlockedNotUseful);
        return FriendExecutionResult::BlockedNotUseful;
    }

    Player* bot = ai->GetBot();
    const uint32 moneyBefore = bot->GetMoney();
    const uint32 itemsBefore = CountInventoryItems(bot);
    bot->SetSelectionGuid(npc->GetObjectGuid());
    sServerFacade.SetFacingTo(bot, npc);

    FriendExecutionResult result = FriendExecutionResult::None;
    if (name == "sell" && !param.empty())
    {
        result = TryDirectSellItems(npc, param) ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
        if (result != FriendExecutionResult::Done && param != "friend")
            result = TryActionWithParam(name, param, "rpg action");

        SetResult(lastIntent, displayName, result);
    }
    else if (param.empty())
        result = TryAction(name, "rpg action");
    else
        result = TryActionWithParam(name, param, "rpg action");

    if (result == FriendExecutionResult::Done)
        ReportServiceTransaction(ai, name, npc, moneyBefore, bot->GetMoney(), itemsBefore, CountInventoryItems(bot));

    return result;
}

bool FriendBotController::TryDirectSellItems(Creature* npc, const std::string& qualifier)
{
    if (!ai || !ai->GetBot() || !npc)
        return false;

    Player* bot = ai->GetBot();
    if (!bot->GetNPCIfCanInteractWith(npc->GetObjectGuid(), FriendVendorNpcFlags()))
        return false;

    const bool aggressiveSell = command == FriendCommand::Shop || lastSituation.bagSpace >= 90;
    std::list<Item*> items = qualifier == "friend" ?
        FriendSellItems(ai, aggressiveSell) : ai->InventoryParseItems(qualifier, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    if (items.empty())
        return false;

    uint32 soldItems = 0;
    uint32 soldMoney = 0;
    uint32 failedItems = 0;
    for (Item* item : items)
    {
        if (!item || item->IsInTrade() || !item->GetProto() || !item->GetProto()->SellPrice)
            continue;

        std::string itemName = item->GetProto()->Name1 ? item->GetProto()->Name1 : "";
        uint32 itemId = item->GetProto()->ItemId;
        uint32 count = item->GetCount();
        uint32 bag = item->GetBagSlot();
        uint32 slot = item->GetSlot();
        uint32 value = item->GetProto()->SellPrice * count;
        ObjectGuid itemGuid = item->GetObjectGuid();
        uint32 moneyBefore = bot->GetMoney();
        bool sold = false;
        uint32 moneyGained = 0;

        WorldPacket packet;
        packet << npc->GetObjectGuid() << itemGuid << count;
        bot->GetSession()->HandleSellItemOpcode(packet);

        Item* remaining = bot->GetItemByPos(bag, slot);
        if (!remaining || remaining->GetObjectGuid() != itemGuid)
        {
            sold = true;
            moneyGained = bot->GetMoney() > moneyBefore ? bot->GetMoney() - moneyBefore : value;
        }
        else
        {
            uint32 moneyBeforeFallback = bot->GetMoney();
            bot->DestroyItem(bag, slot, true);
            remaining = bot->GetItemByPos(bag, slot);
            if (!remaining || remaining->GetObjectGuid() != itemGuid)
            {
                sold = true;
                bot->ModifyMoney(static_cast<int32>(value));
                moneyGained = value;
                if (bot->GetMoney() > moneyBeforeFallback)
                    moneyGained = bot->GetMoney() - moneyBeforeFallback;
            }
        }

        if (!sold)
        {
            ++failedItems;
            continue;
        }

        soldItems += count;
        soldMoney += moneyGained;
        sPlayerbotAIConfig.logEvent(ai, "FriendSellAction", itemName, std::to_string(itemId));
    }

    if (soldItems > 0)
        SetResult(lastIntent, "direct sell:" + qualifier + ":" + std::to_string(soldItems) + " items:" +
            ChatHelper::formatMoney(soldMoney), FriendExecutionResult::Done);
    else if (failedItems > 0)
        SetResult(lastIntent, "direct sell failed:" + std::to_string(failedItems), FriendExecutionResult::Failed);

    if (soldItems > 0)
        ClearFriendInventoryValues(ai);

    return soldItems > 0;
}

void FriendBotController::ClearFriendTravelTarget()
{
    if (!ai || !ai->GetAiObjectContext())
        return;

    AiObjectContext* context = ai->GetAiObjectContext();
    TravelTarget* target = context->GetValue<TravelTarget*>("travel target")->Get();
    sTravelMgr.SetNullTravelTarget(target);
    context->ClearValues("no active travel destinations");
    context->GetValue<GuidPosition>("rpg target")->Set(GuidPosition());
    context->GetValue<ObjectGuid>("attack target")->Set(ObjectGuid());
}

bool FriendBotController::TryPrerequisites(Action* action, const std::string& source, uint8 depth, Player* owner)
{
    if (!action || depth >= 3)
        return false;

    NextAction** prerequisites = action->getPrerequisites();
    if (!prerequisites)
        return false;

    bool executed = false;
    for (uint8 i = 0; prerequisites[i]; ++i)
    {
        const std::string prerequisiteName = prerequisites[i]->getName();
        if (TryAction(prerequisiteName, source, depth + 1, owner) == FriendExecutionResult::Done)
        {
            if (prerequisiteName.find("reach ") == 0 && ai && ai->GetBot() &&
                !sServerFacade.isMoving(ai->GetBot()) && !ai->GetBot()->IsNonMeleeSpellCasted(true))
            {
                continue;
            }

            executed = true;
            break;
        }
    }

    NextAction::destroy(prerequisites);
    return executed;
}

Unit* FriendBotController::GetDamageTarget(const FriendSituation& situation, bool prepare)
{
    (void)prepare;

    std::string reason;
    Unit* target = SelectDamageTarget(situation, false, reason);
    if (!target)
        target = SelectDamageTarget(situation, true, reason);

    if (!target)
        return nullptr;

    SetCurrentDamageTarget(target, reason);
    return target;
}

Unit* FriendBotController::SelectDamageTarget(const FriendSituation& situation, bool allowCrowdControlFallback, std::string& reason)
{
    reason = "none";
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext())
        return nullptr;

    AiObjectContext* context = ai->GetAiObjectContext();
    Unit* crowdControlFallback = nullptr;
    std::string crowdControlReason;

    auto consider = [&](Unit* candidate, const std::string& candidateReason) -> Unit*
    {
        if (!IsValidFriendDamageTarget(candidate, true))
            return nullptr;

        if (ShouldAvoidBreakingCrowdControl(candidate))
        {
            if (!crowdControlFallback)
            {
                crowdControlFallback = candidate;
                crowdControlReason = candidateReason + ":cc";
            }

            return nullptr;
        }

        reason = candidateReason;
        return candidate;
    };

    Unit* target = nullptr;
    if (!situation.tankish && situation.closestAttackerTargetingMeGuid &&
        (ShouldFightToSurvive(situation) || SelfThreatDangerScore(situation) >= 45))
    {
        target = ai->GetUnit(situation.closestAttackerTargetingMeGuid);
        if (Unit* selected = consider(target, "self-threat"))
            return selected;
    }

    if (CanProtectPartyWithThreat(situation) && (situation.vulnerablePartyHasThreat || situation.healerPartyHasThreat))
    {
        target = ai->GetUnit(situation.vulnerablePartyAttackerGuid);
        if (Unit* selected = consider(target, situation.healerPartyHasThreat ? "protect-healer" : "protect-party"))
            return selected;
    }

    if (ai->GetBot()->GetGroup())
    {
        target = GetRaidIconTarget(FRIEND_RTI_SKULL);
        if (Unit* selected = consider(target, "skull"))
            return selected;
    }

    target = GetContextValue<Unit*>(context, "rti target", nullptr);
    if (Unit* selected = consider(target, IsSkullTarget(target) ? "skull" : "rti"))
        return selected;

    target = ai->GetUnit(situation.leaderTargetGuid);
    if (Unit* selected = consider(target, "leader"))
        return selected;

    if (PrefersSelfDefenseTarget(situation))
    {
        target = ai->GetUnit(situation.closestAttackerTargetingMeGuid);
        if (Unit* selected = consider(target, "self-threat"))
            return selected;
    }

    target = ai->GetUnit(situation.vulnerablePartyAttackerGuid);
    if (Unit* selected = consider(target, situation.healerPartyHasThreat ? "healer-threat" : "party-threat"))
        return selected;

    struct AssistTarget
    {
        Unit* target;
        int32 score;
        uint32 health;
        std::string reason;
    };

    std::vector<AssistTarget> assistTargets;
    auto addAssistTarget = [&](Unit* candidate, const std::string& candidateReason, int32 score)
    {
        if (!IsValidFriendDamageTarget(candidate, true))
            return;

        if (ShouldAvoidBreakingCrowdControl(candidate))
        {
            if (!crowdControlFallback)
            {
                crowdControlFallback = candidate;
                crowdControlReason = candidateReason + ":cc";
            }
            return;
        }

        score += (100 - HealthPercent(ai, candidate)) / 4;
        if (Unit* victim = candidate->GetVictim())
        {
            if (victim == ai->GetBot())
                score += 40;
            else if (IsFriendlyTarget(ai, victim))
                score += std::max<int32>(0, PartyThreatScore(victim));
        }

        for (AssistTarget& assistTarget : assistTargets)
        {
            if (assistTarget.target->GetObjectGuid() != candidate->GetObjectGuid())
                continue;

            assistTarget.score += score;
            assistTarget.health = std::min(assistTarget.health, candidate->GetHealth());
            if (candidateReason == "party-victim")
                assistTarget.reason = candidateReason;
            return;
        }

        assistTargets.push_back({ candidate, score, candidate->GetHealth(), candidateReason });
    };

    if (Group* group = ai->GetBot()->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == ai->GetBot() || !member->IsAlive() ||
                member->GetMapId() != ai->GetBot()->GetMapId() || !ai->IsSafe(member))
                continue;

            addAssistTarget(member->GetVictim(), "party-victim", 35);

            Unit* selected = member->GetSelectionGuid() ? ai->GetUnit(member->GetSelectionGuid()) : nullptr;
            if (selected && (sServerFacade.IsInCombat(member) || selected->IsInCombat() ||
                (selected->GetVictim() && IsFriendlyTarget(ai, selected->GetVictim()))))
                addAssistTarget(selected, "party-target", 20);
        }
    }

    Unit* bestAssistTarget = nullptr;
    std::string bestAssistReason;
    int32 bestAssistScore = 0;
    uint32 bestAssistHealth = 0;
    for (const AssistTarget& assistTarget : assistTargets)
    {
        if (!bestAssistTarget || assistTarget.score > bestAssistScore ||
            (assistTarget.score == bestAssistScore && assistTarget.health < bestAssistHealth))
        {
            bestAssistTarget = assistTarget.target;
            bestAssistReason = assistTarget.reason;
            bestAssistScore = assistTarget.score;
            bestAssistHealth = assistTarget.health;
        }
    }

    if (bestAssistTarget)
    {
        reason = bestAssistReason;
        return bestAssistTarget;
    }

    Unit* bestLowestHealth = nullptr;
    uint32 bestHealth = 0;
    std::list<ObjectGuid> possibleTargets = context->GetValue<std::list<ObjectGuid> >("possible attack targets")->Get();
    for (std::list<ObjectGuid>::const_iterator itr = possibleTargets.begin(); itr != possibleTargets.end(); ++itr)
    {
        Unit* candidate = ai->GetUnit(*itr);
        if (!IsValidFriendDamageTarget(candidate, true))
            continue;

        if (ShouldAvoidBreakingCrowdControl(candidate))
        {
            if (!crowdControlFallback)
            {
                crowdControlFallback = candidate;
                crowdControlReason = "lowest-hp:cc";
            }
            continue;
        }

        if (!bestLowestHealth || candidate->GetHealth() < bestHealth)
        {
            bestLowestHealth = candidate;
            bestHealth = candidate->GetHealth();
        }
    }

    if (bestLowestHealth)
    {
        reason = "lowest-hp";
        return bestLowestHealth;
    }

    target = GetContextValue<Unit*>(context, "current target", nullptr);
    if (Unit* selected = consider(target, "current"))
        return selected;

    target = GetContextValue<Unit*>(context, "dps target", nullptr);
    if (Unit* selected = consider(target, "dps"))
        return selected;

    target = GetContextValue<Unit*>(context, "least hp target", nullptr);
    if (Unit* selected = consider(target, "least-hp"))
        return selected;

    Unit* nearestTarget = nullptr;
    float nearestDistance = 0.0f;
    std::list<ObjectGuid> nearbyNpcs = context->GetValue<std::list<ObjectGuid> >("nearest npcs no los")->Get();
    for (std::list<ObjectGuid>::const_iterator itr = nearbyNpcs.begin(); itr != nearbyNpcs.end(); ++itr)
    {
        Unit* candidate = ai->GetUnit(*itr);
        if (!IsValidFriendDamageTarget(candidate, true))
            continue;

        const float distance = sServerFacade.GetDistance2d(ai->GetBot(), candidate);
        if (!nearestTarget || distance < nearestDistance)
        {
            nearestTarget = candidate;
            nearestDistance = distance;
        }
    }

    if (Unit* selected = consider(nearestTarget, "nearest"))
        return selected;

    if (allowCrowdControlFallback && crowdControlFallback)
    {
        reason = crowdControlReason.empty() ? "cc-fallback" : crowdControlReason;
        return crowdControlFallback;
    }

    return nullptr;
}

Unit* FriendBotController::GetCrowdControlTarget(const FriendSituation& situation, const FriendAbility& ability, Unit* currentDamageTarget) const
{
    if (!ai || !ai->GetAiObjectContext())
        return nullptr;

    AiObjectContext* context = ai->GetAiObjectContext();
    auto accept = [&](Unit* candidate) -> Unit*
    {
        return IsCrowdControlTargetWorthwhile(situation, ability, candidate, currentDamageTarget) ? candidate : nullptr;
    };

    const bool currentTargetCasting = currentDamageTarget && currentDamageTarget->IsNonMeleeSpellCasted(false);
    if (ability.Has(FRIEND_ABILITY_INTERRUPT) && currentTargetCasting)
        return currentDamageTarget;

    Unit* target = GetRaidIconTarget(FRIEND_RTI_MOON);
    if (Unit* selected = accept(target))
        return selected;

    target = GetContextValue<Unit*>(context, "rti cc target", nullptr);
    if (Unit* selected = accept(target))
        return selected;

    target = ai->GetUnit(situation.rtiCcTargetGuid);
    if (Unit* selected = accept(target))
        return selected;

    target = ai->GetUnit(situation.closestAttackerTargetingMeGuid);
    if (Unit* selected = accept(target))
        return selected;

    target = ai->GetUnit(situation.vulnerablePartyAttackerGuid);
    if (Unit* selected = accept(target))
        return selected;

    target = context->GetValue<Unit*>("cc target", ability.name)->Get();
    if (Unit* selected = accept(target))
        return selected;

    return nullptr;
}

bool FriendBotController::IsValidFriendDamageTarget(Unit* target, bool allowCrowdControlFallback) const
{
    if (!ai || !ai->GetBot() || !IsUsableUnit(ai, target))
        return false;

    Player* bot = ai->GetBot();
    if (sServerFacade.IsFriendlyTo(bot, target))
        return false;

    if (!PossibleAttackTargetsValue::IsPossibleTarget(target, bot, sPlayerbotAIConfig.sightDistance, true))
        return false;

    const ObjectGuid targetGuid = target->GetObjectGuid();
    bool explicitlyChosen = IsSkullTarget(target);
    bool fightingParty = false;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (context)
    {
        explicitlyChosen = explicitlyChosen ||
            targetGuid == GetContextValue<ObjectGuid>(context, "attack target", ObjectGuid());

        Unit* currentTarget = GetContextValue<Unit*>(context, "current target", nullptr);
        Unit* dpsTarget = GetContextValue<Unit*>(context, "dps target", nullptr);
        Unit* leastHpTarget = GetContextValue<Unit*>(context, "least hp target", nullptr);
        Unit* rtiTarget = GetContextValue<Unit*>(context, "rti target", nullptr);
        explicitlyChosen = explicitlyChosen ||
            (currentTarget && currentTarget->GetObjectGuid() == targetGuid) ||
            (dpsTarget && dpsTarget->GetObjectGuid() == targetGuid) ||
            (leastHpTarget && leastHpTarget->GetObjectGuid() == targetGuid) ||
            (rtiTarget && rtiTarget->GetObjectGuid() == targetGuid);
    }

    Player* leader = ai->GetGroupMaster();
    if (leader && ai->IsSafe(leader) && leader->GetSelectionGuid() == targetGuid)
        explicitlyChosen = true;

    Unit* victim = target->GetVictim();
    if (IsFriendlyTarget(ai, victim))
        fightingParty = true;

    ObjectGuid unitTargetGuid = target->GetGuidValue(UNIT_FIELD_TARGET);
    if (unitTargetGuid)
    {
        Unit* unitTarget = ai->GetUnit(unitTargetGuid);
        if (IsFriendlyTarget(ai, unitTarget))
            fightingParty = true;
    }

    auto checkPartyMember = [&](Player* member)
    {
        if (!member || !member->IsAlive() || member->GetMapId() != bot->GetMapId() || !ai->IsSafe(member))
            return;

        if (member->GetVictim() == target || member->GetSelectionGuid() == targetGuid)
            fightingParty = true;

        if (victim == member || target->GetGuidValue(UNIT_FIELD_TARGET) == member->GetObjectGuid())
            fightingParty = true;
    };

    checkPartyMember(bot);
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            checkPartyMember(ref->getSource());
    }

    const bool personallyHostile = sServerFacade.IsHostileTo(bot, target);
    if (!personallyHostile && !explicitlyChosen && !fightingParty)
        return false;

    return allowCrowdControlFallback || !ShouldAvoidBreakingCrowdControl(target);
}

bool FriendBotController::ShouldAvoidBreakingCrowdControl(Unit* target) const
{
    if (!ai || !ai->GetBot() || !target || IsSkullTarget(target))
        return false;

    if (IsMoonTarget(target))
        return true;

    AiObjectContext* context = ai->GetAiObjectContext();
    Unit* rtiCcTarget = GetContextValue<Unit*>(context, "rti cc target", nullptr);
    if (rtiCcTarget && rtiCcTarget == target)
        return true;

    return PossibleAttackTargetsValue::HasBreakableCC(target, ai->GetBot()) ||
        PossibleAttackTargetsValue::HasUnBreakableCC(target, ai->GetBot());
}

bool FriendBotController::IsCrowdControlTargetWorthwhile(const FriendSituation& situation, const FriendAbility& ability,
    Unit* target, Unit* currentDamageTarget) const
{
    if (!ai || !ai->GetBot() || !IsHostileTarget(ai, target))
        return false;

    if (ability.Has(FRIEND_ABILITY_INTERRUPT))
        return target->IsNonMeleeSpellCasted(false);

    if (IsSkullTarget(target))
        return false;

    if (IsMoonTarget(target))
        return true;

    const ObjectGuid targetGuid = target->GetObjectGuid();
    const bool selfPeel = PrefersSelfDefenseTarget(situation) &&
        situation.closestAttackerTargetingMeGuid == targetGuid;
    const bool partyPeel = situation.vulnerablePartyAttackerGuid == targetGuid &&
        (situation.vulnerablePartyHasThreat || situation.healerPartyHasThreat);

    if (selfPeel)
        return true;

    if (partyPeel && currentDamageTarget != target && !IsPartyMeleeEngagedWith(target))
        return true;

    if (currentDamageTarget == target ||
        situation.leaderTargetGuid == targetGuid ||
        situation.rtiTargetGuid == targetGuid ||
        IsPartyMeleeEngagedWith(target))
        return false;

    if (situation.possibleTargetsCount <= 1)
        return false;

    return true;
}

bool FriendBotController::IsPartyMeleeEngagedWith(Unit* target) const
{
    if (!ai || !ai->GetBot() || !target || !target->IsInWorld())
        return false;

    Group* group = ai->GetBot()->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == ai->GetBot() || !member->IsAlive() ||
            member->GetMapId() != target->GetMapId() || !ai->IsSafe(member))
            continue;

        if (sServerFacade.GetDistance2d(member, target) > FRIEND_CC_MELEE_ENGAGED_DISTANCE)
            continue;

        if (member->GetSelectionGuid() == target->GetObjectGuid() || member->GetVictim() == target)
            return true;
    }

    return false;
}

Unit* FriendBotController::GetRaidIconTarget(uint8 icon) const
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->GetGroup())
        return nullptr;

    ObjectGuid guid = ObjectGuid(ai->GetBot()->GetGroup()->GetTargetIcon(icon));
    return guid ? ai->GetUnit(guid) : nullptr;
}

bool FriendBotController::IsSkullTarget(Unit* target) const
{
    Unit* iconTarget = GetRaidIconTarget(FRIEND_RTI_SKULL);
    return target && iconTarget && iconTarget->GetObjectGuid() == target->GetObjectGuid();
}

bool FriendBotController::IsMoonTarget(Unit* target) const
{
    Unit* iconTarget = GetRaidIconTarget(FRIEND_RTI_MOON);
    return target && iconTarget && iconTarget->GetObjectGuid() == target->GetObjectGuid();
}

void FriendBotController::SetCurrentDamageTarget(Unit* target, const std::string& reason)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !target)
        return;

    ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
    ai->GetBot()->SetSelectionGuid(target->GetObjectGuid());
    lastTargetReason = reason;
    lastSituation.hasTarget = true;
    lastSituation.targetName = target->GetName();
    lastSituation.targetDistance = sServerFacade.GetDistance2d(ai->GetBot(), target);
    lastSituation.targetIsElite = IsEliteTarget(ai, target);
}

std::vector<Unit*> FriendBotController::GetPartyTargets() const
{
    std::vector<Unit*> targets;
    if (!ai || !ai->GetBot())
        return targets;

    Player* bot = ai->GetBot();
    auto addTarget = [&](Player* member)
    {
        if (!member || !member->IsAlive() || !ai->IsSafe(member))
            return;

        if (std::find(targets.begin(), targets.end(), member) == targets.end())
            targets.push_back(member);
    };

    addTarget(bot);
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            addTarget(ref->getSource());
    }

    return targets;
}

bool FriendBotController::ShouldLootNow(const FriendSituation& situation, bool localPartyInCombat) const
{
    if (!situation.hasCreatureLoot)
        return false;

    if (command != FriendCommand::None && command != FriendCommand::StayClose)
        return false;

    bool closeCreatureLoot = false;
    if (ai && ai->GetBot() && ai->GetAiObjectContext())
    {
        LootObjectStack* availableLoot = GetContextValue<LootObjectStack*>(ai->GetAiObjectContext(), "available loot", nullptr);
        if (availableLoot)
        {
            LootObject closeLoot = availableLoot->GetLoot(INTERACTION_DISTANCE + 2.0f);
            closeCreatureLoot = !closeLoot.IsEmpty() && closeLoot.guid.IsCreature();
        }
    }

    if (situation.inCombat || situation.hasAttackers || localPartyInCombat)
    {
        if (!closeCreatureLoot || situation.inCombat || situation.hasAttackers)
            return false;
    }

    if (situation.botHealth < sPlayerbotAIConfig.mediumHealth)
        return false;

    const bool partyNearby = situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation);
    if (partyNearby && situation.lowestPartyHealth < sPlayerbotAIConfig.mediumHealth)
        return false;

    if (mode == FriendMode::Solo && (!situation.leaderSafe || situation.leaderDistance > SoftLeashDistance(situation)))
        return true;

    if (!situation.leaderSafe)
        return false;

    if (mode == FriendMode::Dungeon)
        return situation.leaderDistance <= PreferredLeaderDistance(situation);

    return situation.leaderDistance <= SoftLeashDistance(situation);
}

bool FriendBotController::TryAutoLootRoll(const FriendSituation& situation)
{
    (void)situation;
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !ai->GetBot()->GetGroup())
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (GetContextValue<LootRollMap>(context, "active rolls", LootRollMap()).empty())
        return false;

    lastIntent = FriendIntent::LootNearby;
    FriendExecutionResult result = TryAction("auto loot roll", "friend loot roll");
    if (result != FriendExecutionResult::Done)
        return false;

    MaybeSayActivity(situation, "loot-roll", {
        "I'll roll on that.",
        "Rolling for loot."
    }, 25, 60);
    return true;
}

Unit* FriendBotController::GetHealTarget(const FriendSituation& situation) const
{
    if (!ai || !ai->GetAiObjectContext())
        return nullptr;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (manualHealUntil > time(nullptr) && manualHealGuid)
    {
        Unit* requested = ai->GetUnit(manualHealGuid);
        if (IsFriendlyTarget(ai, requested) && HealthPercent(ai, requested) < 99)
            return requested;
    }

    Unit* target = GetContextValue<Unit*>(context, "party member to heal", nullptr);
    if (IsFriendlyTarget(ai, target))
        return target;

    Unit* best = nullptr;
    uint8 bestHealth = 100;
    for (Unit* member : GetPartyTargets())
    {
        const uint8 health = HealthPercent(ai, member);
        if (health < bestHealth)
        {
            best = member;
            bestHealth = health;
        }
    }

    if (!best)
        return nullptr;

    if (bestHealth < sPlayerbotAIConfig.almostFullHealth ||
        situation.lowestPartyHealthDelta <= -5 ||
        ((situation.inCombat || situation.partyInCombat) && bestHealth < 98))
        return best;

    return nullptr;
}

bool FriendBotController::TryReachAbilityTarget(const FriendAbility& ability, Unit* target, const std::string& source)
{
    (void)source;

    if (!ai || !ai->GetAiObjectContext() || !target || !ai->CanMove())
        return false;

    Player* bot = ai->GetBot();
    if (!bot || !IsUsableUnit(ai, target) || !bot->GetMotionMaster())
        return false;

    if (!ShouldMoveForAbilityTarget(ability, target))
        return false;

    const bool damageTarget = IsValidFriendDamageTarget(target, true);
    const bool friendly = IsFriendlyTarget(ai, target);
    if (!damageTarget && !friendly)
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    SpellCastResult checkResult = SPELL_CAST_OK;
    bool canEventuallyCast = ai->CanCastSpell(ability.spellId, target, 0, true, nullptr, true, false, false, &checkResult);
    if (!canEventuallyCast && damageTarget && ability.Has(FRIEND_ABILITY_DAMAGE) && ability.maxRange <= 0.0f)
        canEventuallyCast = true;

    if (!canEventuallyCast)
        return false;

    if (damageTarget)
    {
        context->GetValue<Unit*>("current target")->Set(target);
        bot->SetSelectionGuid(target->GetObjectGuid());
    }

    float desiredDistance = sPlayerbotAIConfig.spellDistance;
    if (damageTarget && (ability.Has(FRIEND_ABILITY_MELEE) || ability.maxRange <= 0.0f))
    {
        desiredDistance = std::max(sPlayerbotAIConfig.meleeDistance, sPlayerbotAIConfig.contactDistance);
    }
    else if (ability.maxRange > 0.0f)
    {
        desiredDistance = std::max(sPlayerbotAIConfig.contactDistance, ability.maxRange - sPlayerbotAIConfig.contactDistance);
    }
    else
    {
        desiredDistance = friendly ? ai->GetRange("follow") : std::max(sPlayerbotAIConfig.meleeDistance, sPlayerbotAIConfig.contactDistance);
    }

    return MoveToUnitRange(target, desiredDistance, "move for spell:" + ability.name);
}

bool FriendBotController::TryFreeDamage(const FriendSituation& situation, const std::string& source)
{
    Unit* target = GetDamageTarget(situation, true);
    if (!IsValidFriendDamageTarget(target, true))
        return false;

    if (TryDruidCombatForm(situation, source))
        return true;

    if (ai && ai->GetBot() && ai->GetBot()->getClass() == CLASS_WARRIOR && GetDamageTarget(situation, true))
    {
        if (TryActions({ "charge", "intercept" }, source))
            return true;
    }

    if (PrefersMeleeDamage(situation))
    {
        if (MoveToDamageTarget(situation, "move to melee"))
            return true;

        if (TryActions({ "melee", "attack" }, source))
            return true;

        return TryAction("shoot", source) == FriendExecutionResult::Done;
    }

    if (TryAction("shoot", source) == FriendExecutionResult::Done)
        return true;

    if (MoveToDamageTarget(situation, "move to melee"))
        return true;

    return TryActions({ "melee", "attack" }, source);
}

bool FriendBotController::TryFallbackCombat(const FriendSituation& situation, const std::string& source)
{
    if (TryFreeDamage(situation, source))
        return true;

    if (TryCatalogDamage(situation, "friend damage"))
        return true;

    if (TryActions(DamageActions(situation), "friend fallback damage"))
        return true;

    if (situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
        return MoveNearLeader(situation, "move near leader", false);

    return false;
}

bool FriendBotController::TryDruidCombatForm(const FriendSituation& situation, const std::string& source)
{
    if (!ai || !ai->GetBot() || ai->GetBot()->getClass() != CLASS_DRUID)
        return false;

    Player* bot = ai->GetBot();
    if (ai->HasAnyAuraOf(bot, "cat form", "bear form", "dire bear form", NULL))
        return false;

    const bool selfHot = ai->HasAnyAuraOf(bot, "regrowth", "rejuvenation", NULL);
    const bool selfHotReady = selfHot && situation.botHealth < sPlayerbotAIConfig.almostFullHealth &&
        situation.lowestPartyHealth >= sPlayerbotAIConfig.mediumHealth;
    if (situation.healerish && situation.damagedPartyMembers > 0 &&
        !situation.botHasThreat && !selfHotReady)
        return false;

    if (situation.tankish || situation.hasAttackers || situation.botHasThreat ||
        situation.botHealth < sPlayerbotAIConfig.almostFullHealth ||
        situation.botHealthDelta <= FRIEND_HEALTH_DROP_NOTICE)
    {
        if (TryAction("dire bear form", source) == FriendExecutionResult::Done)
            return true;
        return TryAction("bear form", source) == FriendExecutionResult::Done;
    }

    if (TryAction("cat form", source) == FriendExecutionResult::Done)
        return true;
    if (TryAction("dire bear form", source) == FriendExecutionResult::Done)
        return true;
    return TryAction("bear form", source) == FriendExecutionResult::Done;
}

bool FriendBotController::TryImproveRangedCombatSpacing(const FriendSituation& situation, const std::string& action)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !ai->CanMove())
        return false;

    Player* bot = ai->GetBot();
    if (!bot->GetMotionMaster() || bot->IsNonMeleeSpellCasted(true))
        return false;

    if ((!situation.ranged && !situation.healerish) || situation.tankish ||
        situation.hasAttackers || situation.botHasThreat || situation.attackersTargetingMeCount > 0)
        return false;

    Unit* target = GetDamageTarget(situation, true);
    if (!IsValidFriendDamageTarget(target, true) || target->GetVictim() == bot)
        return false;

    const float distance = sServerFacade.GetDistance2d(bot, target);
    if (distance >= FRIEND_RANGED_SPACING_MIN && bot->IsWithinLOSInMap(target, true))
        return false;

    if (IsMovingForAction(ai, lastAction, action))
    {
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!FindRangedCombatPosition(target, situation, x, y, z))
        return false;

    if (!MoveFriendPoint(x, y, z))
        return false;

    SetResult(lastIntent, action, FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool FriendBotController::FindRangedCombatPosition(Unit* target, const FriendSituation& situation, float& x, float& y, float& z) const
{
    if (!ai || !ai->GetBot() || !target)
        return false;

    Player* bot = ai->GetBot();
    const float desiredDistance = mode == FriendMode::Dungeon ? FRIEND_RANGED_SPACING_DUNGEON : FRIEND_RANGED_SPACING_WORLD;

    float dx = bot->GetPositionX() - target->GetPositionX();
    float dy = bot->GetPositionY() - target->GetPositionY();

    Unit* nearestHostile = ai->GetUnit(situation.nearestHostileGuid);
    if (nearestHostile && nearestHostile != target && IsHostileTarget(ai, nearestHostile))
    {
        dx += (target->GetPositionX() - nearestHostile->GetPositionX()) * 1.5f;
        dy += (target->GetPositionY() - nearestHostile->GetPositionY()) * 1.5f;
    }

    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f)
    {
        const float angle = target->GetAngle(bot);
        dx = std::cos(angle);
        dy = std::sin(angle);
    }
    else
    {
        dx /= length;
        dy /= length;
    }

    const float baseAngle = std::atan2(dy, dx);
    const float randomOffset = static_cast<float>(urand(0, 600)) / 1000.0f - 0.3f;
    const float offsets[] = { 0.0f, 0.45f, -0.45f, 0.9f, -0.9f, 1.35f, -1.35f };
    const float radii[] = { desiredDistance, desiredDistance + 2.0f, std::max(FRIEND_RANGED_SPACING_MIN, desiredDistance - 2.0f) };

    for (float radius : radii)
    {
        for (float offset : offsets)
        {
            const float angle = baseAngle + offset + randomOffset;
            x = target->GetPositionX() + std::cos(angle) * radius;
            y = target->GetPositionY() + std::sin(angle) * radius;
            z = target->GetPositionZ();
            if (!NormalizeFriendMovePosition(x, y, z))
                continue;

            if (IsRangedCombatPositionSafe(target, situation, x, y, z))
                return true;
        }
    }

    return false;
}

bool FriendBotController::IsRangedCombatPositionSafe(Unit* target, const FriendSituation& situation, float x, float y, float z) const
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !target)
        return false;

    Player* bot = ai->GetBot();
    WorldPosition from(bot);
    WorldPosition to(target->GetMapId(), x, y, z);
    if (!from.canPathTo(to, bot) ||
        !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true) ||
        !target->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
        return false;

    if (situation.leaderSafe && (mode != FriendMode::Solo || situation.leaderDistance <= SoftLeashDistance(situation)))
    {
        Unit* leader = ai->GetUnit(situation.leaderGuid);
        if (leader && leader->GetMapId() == target->GetMapId())
        {
            const float dx = leader->GetPositionX() - x;
            const float dy = leader->GetPositionY() - y;
            if (std::sqrt(dx * dx + dy * dy) > SoftLeashDistance(situation))
                return false;
        }
    }

    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> nearbyNpcs = context->GetValue<std::list<ObjectGuid> >("nearest npcs no los")->Get();
    for (std::list<ObjectGuid>::const_iterator itr = nearbyNpcs.begin(); itr != nearbyNpcs.end(); ++itr)
    {
        Unit* hostile = ai->GetUnit(*itr);
        if (!IsHostileTarget(ai, hostile) || hostile == target)
            continue;

        const float candidateDx = hostile->GetPositionX() - x;
        const float candidateDy = hostile->GetPositionY() - y;
        const float candidateDistance = std::sqrt(candidateDx * candidateDx + candidateDy * candidateDy);
        if (candidateDistance < FRIEND_RANGED_SPACING_HOSTILE_BUFFER)
            return false;

        const float currentDistance = sServerFacade.GetDistance2d(bot, hostile);
        if (currentDistance < sPlayerbotAIConfig.sightDistance &&
            candidateDistance + 2.0f < currentDistance)
            return false;
    }

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMapId() != target->GetMapId() ||
                !ai->IsSafe(member))
                continue;

            const float dx = member->GetPositionX() - x;
            const float dy = member->GetPositionY() - y;
            if (std::sqrt(dx * dx + dy * dy) < FRIEND_RANGED_SPACING_PARTY_BUFFER)
                return false;
        }
    }

    return true;
}

bool FriendBotController::MoveToDamageTarget(const FriendSituation& situation, const std::string& action)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !ai->CanMove())
        return false;

    Unit* target = GetDamageTarget(situation, true);
    if (!IsValidFriendDamageTarget(target, true))
        return false;

    Player* bot = ai->GetBot();
    if (!bot->GetMotionMaster())
        return false;

    const float desiredDistance = std::max(sPlayerbotAIConfig.meleeDistance, sPlayerbotAIConfig.contactDistance);
    const float distance = sServerFacade.GetDistance2d(bot, target);
    if (distance <= desiredDistance && bot->IsWithinLOSInMap(target, true))
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    context->GetValue<Unit*>("current target")->Set(target);
    bot->SetSelectionGuid(target->GetObjectGuid());
    return MoveToUnitRange(target, desiredDistance, action);
}

bool FriendBotController::MoveToUnitRange(Unit* target, float desiredDistance, const std::string& action)
{
    if (!ai || !ai->GetBot() || !target || !ai->CanMove())
        return false;

    Player* bot = ai->GetBot();
    if (!bot->GetMotionMaster() || !IsUsableUnit(ai, target))
        return false;

    desiredDistance = std::max(sPlayerbotAIConfig.contactDistance, desiredDistance);
    const float distance = sServerFacade.GetDistance2d(bot, target);
    if (distance <= desiredDistance && bot->IsWithinLOSInMap(target, true))
        desiredDistance = std::max(sPlayerbotAIConfig.contactDistance, desiredDistance * 0.5f);
    else if (distance <= desiredDistance)
        desiredDistance = std::max(sPlayerbotAIConfig.contactDistance, distance * 0.5f);

    float dx = bot->GetPositionX() - target->GetPositionX();
    float dy = bot->GetPositionY() - target->GetPositionY();
    float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.1f)
    {
        const float angle = target->GetAngle(bot);
        dx = std::cos(angle);
        dy = std::sin(angle);
    }
    else
    {
        dx /= length;
        dy /= length;
    }

    const float baseAngle = std::atan2(dy, dx);
    const float offsets[] = { 0.0f, 0.45f, -0.45f, 0.9f, -0.9f, 1.35f, -1.35f, 3.14159f };
    const float radii[] = {
        desiredDistance,
        desiredDistance + sPlayerbotAIConfig.contactDistance,
        std::max(sPlayerbotAIConfig.contactDistance, desiredDistance * 0.5f)
    };

    for (float radius : radii)
    {
        for (float offset : offsets)
        {
            float x = target->GetPositionX() + std::cos(baseAngle + offset) * radius;
            float y = target->GetPositionY() + std::sin(baseAngle + offset) * radius;
            float z = target->GetPositionZ();
            if (!NormalizeFriendMovePosition(x, y, z))
                continue;

            ClearFriendMovement(false);
            if (MoveFriendPoint(x, y, z))
            {
                SetResult(lastIntent, action, FriendExecutionResult::Done);
                ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
                return true;
            }
        }
    }

    if (IsHostileTarget(ai, target))
    {
        ClearFriendMovement(false);
        bot->GetMotionMaster()->MoveChase(target, desiredDistance, bot->GetAngle(target));
        if (sServerFacade.isMoving(bot))
        {
            SetResult(lastIntent, action, FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
            return true;
        }
    }

    SetResult(lastIntent, action + ":blocked", FriendExecutionResult::BlockedNotPossible);
    return false;
}

bool FriendBotController::PrefersMeleeDamage(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot())
        return false;

    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_PALADIN:
            return true;
        case CLASS_DRUID:
            return ShouldConserveDamageMana(situation) || !situation.ranged || situation.tankish;
        case CLASS_SHAMAN:
            return !situation.ranged || situation.tankish;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            return true;
#endif
        default:
            return false;
    }
}

bool FriendBotController::PrefersSelfDefenseTarget(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot() || !situation.closestAttackerTargetingMeGuid)
        return false;

    if (situation.tankish)
        return false;

    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_ROGUE:
            return false;
        default:
            return situation.healerish || situation.ranged || situation.botHealth < sPlayerbotAIConfig.mediumHealth;
    }
}

int32 FriendBotController::SelfThreatDangerScore(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot() || !situation.botHasThreat)
        return 0;

    Player* bot = ai->GetBot();
    Unit* threat = ai->GetUnit(situation.closestAttackerTargetingMeGuid);
    int32 score = 20;

    if (situation.botHealth < sPlayerbotAIConfig.lowHealth)
        score += 70;
    else if (situation.botHealth < sPlayerbotAIConfig.mediumHealth)
        score += 35;
    else if (situation.botHealth < sPlayerbotAIConfig.almostFullHealth)
        score += 10;

    if (situation.botHealthDelta <= FRIEND_HEALTH_DROP_DANGER)
        score += 55;
    else if (situation.botHealthDelta <= FRIEND_HEALTH_DROP_NOTICE)
        score += 20;

    if (situation.attackersTargetingMeCount > 2)
        score += 75;
    else if (situation.attackersTargetingMeCount > 1)
        score += 35;

    if (situation.possibleTargetsCount > 2)
        score += 20;

    if (situation.balance < 70)
        score += 25;
    else if (situation.balance < 85)
        score += 10;

    if (threat)
    {
        const int32 levelDelta = static_cast<int32>(threat->GetLevel()) - static_cast<int32>(bot->GetLevel());
        if (levelDelta >= 3)
            score += 45;
        else if (levelDelta >= 1)
            score += 20;
        else if (levelDelta <= -3)
            score -= 25;
        else if (levelDelta <= -1)
            score -= 10;

        if (IsEliteTarget(ai, threat))
            score += 45;

        const uint8 threatHealth = HealthPercent(ai, threat);
        if (threatHealth < 20)
            score -= 25;
        else if (threatHealth < 40)
            score -= 15;
        else if (threatHealth > 80)
            score += 10;

        const uint32 botHealth = bot->GetHealth();
        const uint32 threatHealthValue = threat->GetHealth();
        if (botHealth && threatHealthValue)
        {
            if (static_cast<uint64>(threatHealthValue) * 100 < static_cast<uint64>(botHealth) * 70)
                score -= 20;
            else if (static_cast<uint64>(threatHealthValue) > static_cast<uint64>(botHealth) * 130 / 100)
                score += 25;
        }

        const float distance = sServerFacade.GetDistance2d(bot, threat);
        if (distance <= sPlayerbotAIConfig.meleeDistance)
            score += 15;
        else if (distance < 8.0f)
            score += 8;
    }

    if (situation.nearbyPartyMembers > 1)
        score -= 10;
    else if (situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
        score += 15;

    return std::max<int32>(0, std::min<int32>(200, score));
}

bool FriendBotController::ShouldFightToSurvive(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot() || !situation.botHasThreat || !situation.closestAttackerTargetingMeGuid)
        return false;

    Player* bot = ai->GetBot();
    Unit* threat = ai->GetUnit(situation.closestAttackerTargetingMeGuid);
    if (!IsValidFriendDamageTarget(threat, false))
        return false;

    const uint8 threatHealth = HealthPercent(ai, threat);
    const int32 danger = SelfThreatDangerScore(situation);
    const int32 levelDelta = static_cast<int32>(threat->GetLevel()) - static_cast<int32>(bot->GetLevel());
    const bool targetWeak = threatHealth < 35 || levelDelta <= -2;
    const bool oneOnOne = situation.attackersTargetingMeCount <= 1 && situation.possibleTargetsCount <= 2;
    const bool stableEnough = situation.botHealth >= sPlayerbotAIConfig.mediumHealth &&
        situation.botHealthDelta > FRIEND_HEALTH_DROP_DANGER;
    const bool enemyAlreadyLosing = bot->GetHealth() && threat->GetHealth() &&
        static_cast<uint64>(threat->GetHealth()) * 100 < static_cast<uint64>(bot->GetHealth()) * 90;

    if (targetWeak && situation.botHealth >= sPlayerbotAIConfig.lowHealth &&
        situation.attackersTargetingMeCount <= 2 && situation.possibleTargetsCount <= 3)
        return true;

    if (!oneOnOne || IsEliteTarget(ai, threat))
        return false;

    if (levelDelta >= 3 && threatHealth > 30)
        return false;

    if (situation.botHealth < sPlayerbotAIConfig.lowHealth && threatHealth > 20)
        return false;

    return danger < 70 || (stableEnough && enemyAlreadyLosing);
}

bool FriendBotController::CanProtectPartyWithThreat(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot())
        return false;

    if (situation.tankish)
        return true;

    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DRUID:
            return true;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            return true;
#endif
        default:
            return false;
    }
}

int32 FriendBotController::PartyThreatScore(Unit* victim) const
{
    if (!ai || !ai->GetBot() || !victim || victim == ai->GetBot() || !IsFriendlyTarget(ai, victim))
        return 0;

    Player* bot = ai->GetBot();
    int32 score = 0;

    Player* playerVictim = dynamic_cast<Player*>(victim);
    if (playerVictim && ai->IsHeal(playerVictim))
        score += 90;

    const uint8 victimHealthPct = HealthPercent(ai, victim);
    if (victimHealthPct < sPlayerbotAIConfig.lowHealth)
        score += 90;
    else if (victimHealthPct < sPlayerbotAIConfig.mediumHealth)
        score += 65;

    if (!playerVictim)
        return score;

    const uint32 botMaxHealth = bot->GetMaxHealth();
    const uint32 victimMaxHealth = victim->GetMaxHealth();
    if (botMaxHealth && victimMaxHealth)
    {
        if (static_cast<uint64>(victimMaxHealth) * 100 < static_cast<uint64>(botMaxHealth) * 80)
            score += 45;
        else if (static_cast<uint64>(victimMaxHealth) * 100 < static_cast<uint64>(botMaxHealth) * 95)
            score += 15;
    }

    const uint32 botHealth = bot->GetHealth();
    const uint32 victimHealth = victim->GetHealth();
    if (botHealth && victimHealth &&
        static_cast<uint64>(victimHealth) * 100 < static_cast<uint64>(botHealth) * 75)
        score += 30;

    return score;
}

bool FriendBotController::CanClassHeal() const
{
    if (!ai || !ai->GetBot())
        return false;

    switch (ai->GetBot()->getClass())
    {
        case CLASS_PRIEST:
        case CLASS_DRUID:
        case CLASS_PALADIN:
        case CLASS_SHAMAN:
            return true;
        default:
            return false;
    }
}

bool FriendBotController::ShouldOpportunisticHeal(const FriendSituation& situation) const
{
    if (!CanClassHeal() || situation.damagedPartyMembers == 0 ||
        (!situation.inCombat && !situation.partyInCombat))
        return false;

    if (situation.lowestPartyHealth < sPlayerbotAIConfig.mediumHealth)
        return true;

    if (situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_NOTICE)
        return true;

    return situation.healerish && situation.lowestPartyHealth < FRIEND_HEAL_TOP_OFF_HEALTH;
}

FriendCombatStyle FriendBotController::GetCombatStyle(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->HasMana())
        return FriendCombatStyle::Normal;

    const bool urgent = situation.botHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        situation.botHealthDelta <= FRIEND_HEALTH_DROP_DANGER ||
        situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_DANGER ||
        situation.vulnerablePartyHasThreat ||
        situation.healerPartyHasThreat;
    if (urgent)
        return FriendCombatStyle::Burn;

    const bool hardFight = mode == FriendMode::Dungeon ||
        situation.targetIsElite ||
        situation.possibleTargetsCount > 1 ||
        situation.attackersCount > 1 ||
        situation.balance < 85;
    if (hardFight)
        return situation.botMana < sPlayerbotAIConfig.lowMana ? FriendCombatStyle::Conserve : FriendCombatStyle::Normal;

    if (situation.botMana < sPlayerbotAIConfig.lowMana)
        return FriendCombatStyle::Dry;

    if (IsLowPressureFight(situation) && situation.botMana < FRIEND_MANA_DAMAGE_CONSERVE)
        return FriendCombatStyle::Conserve;

    return FriendCombatStyle::Normal;
}

bool FriendBotController::ShouldConserveDamageMana(const FriendSituation& situation) const
{
    FriendCombatStyle style = GetCombatStyle(situation);
    return style == FriendCombatStyle::Conserve || style == FriendCombatStyle::Dry;
}

bool FriendBotController::IsLowPressureFight(const FriendSituation& situation) const
{
    return mode != FriendMode::Dungeon &&
        !situation.targetIsElite &&
        situation.possibleTargetsCount <= 1 &&
        situation.attackersCount <= 1 &&
        situation.balance >= 90 &&
        situation.botHealth >= sPlayerbotAIConfig.mediumHealth &&
        situation.lowestPartyHealth >= sPlayerbotAIConfig.almostFullHealth &&
        situation.botHealthDelta > FRIEND_HEALTH_DROP_NOTICE &&
        situation.lowestPartyHealthDelta > FRIEND_HEALTH_DROP_NOTICE;
}

int32 FriendBotController::ManaSpendScorePenalty(const FriendSituation& situation, const FriendAbility& ability) const
{
    if (!ability.UsesMana() || ability.Has(FRIEND_ABILITY_INTERRUPT))
        return 0;

    int32 extraPenalty = 0;
    if (ai && ai->GetBot() && ai->GetBot()->getClass() == CLASS_PRIEST &&
        IsLowPressureFight(situation) && situation.botMana < 95 &&
        situation.lowestPartyHealth >= FRIEND_HEAL_TOP_OFF_HEALTH)
    {
        extraPenalty += ability.Has(FRIEND_ABILITY_DOT) ? 10 : 30;
    }

    switch (GetCombatStyle(situation))
    {
        case FriendCombatStyle::Burn:
            return extraPenalty;
        case FriendCombatStyle::Normal:
            return (situation.botMana < sPlayerbotAIConfig.mediumMana ? 20 : 0) + extraPenalty;
        case FriendCombatStyle::Conserve:
            return (ability.Has(FRIEND_ABILITY_DOT) ? 25 : 55) + extraPenalty;
        case FriendCombatStyle::Dry:
            return (ability.Has(FRIEND_ABILITY_DOT) ? 60 : 120) + extraPenalty;
    }

    return extraPenalty;
}

int32 FriendBotController::ThreatCautionScore(const FriendSituation& situation, Unit* target) const
{
    if (!ai || !ai->GetBot() || !target || situation.botHasThreat || situation.tankish)
        return 0;

    Player* bot = ai->GetBot();
    Unit* victim = target->GetVictim();
    Player* victimPlayer = dynamic_cast<Player*>(victim);
    if (!victimPlayer || victimPlayer == bot || !IsFriendlyTarget(ai, victimPlayer))
        return 0;

    if (situation.vulnerablePartyHasThreat || situation.healerPartyHasThreat)
        return 0;

    const uint8 targetHealth = HealthPercent(ai, target);
    if (targetHealth < 20)
        return 0;

    const uint8 victimHealthPct = HealthPercent(ai, victimPlayer);
    if (victimHealthPct < sPlayerbotAIConfig.mediumHealth)
        return 0;

    const uint32 botHealth = bot->GetHealth();
    const uint32 botMaxHealth = bot->GetMaxHealth();
    const uint32 victimHealth = victimPlayer->GetHealth();
    const uint32 victimMaxHealth = victimPlayer->GetMaxHealth();
    const bool victimSturdier =
        (botHealth && victimHealth && static_cast<uint64>(victimHealth) * 100 >= static_cast<uint64>(botHealth) * 115) ||
        (botMaxHealth && victimMaxHealth && static_cast<uint64>(victimMaxHealth) * 100 >= static_cast<uint64>(botMaxHealth) * 120);
    if (!victimSturdier)
        return 0;

    const float botThreat = ThreatValue::GetThreat(bot, target);
    const float victimThreat = ThreatValue::GetThreat(victimPlayer, target);
    if (botThreat <= 0.0f || victimThreat <= 0.0f)
        return 0;

    int32 score = 0;
    const float percent = botThreat * 100.0f / victimThreat;
    if (percent >= 90.0f)
        score += 70;
    else if (percent >= 70.0f)
        score += 45;
    else if (percent >= 50.0f)
        score += 20;

    if (situation.ranged || situation.healerish)
        score += 10;
    if (situation.botHealth < sPlayerbotAIConfig.mediumHealth)
        score += 15;
    if (targetHealth > 60)
        score += 10;

    return std::max<int32>(0, std::min<int32>(100, score));
}

bool FriendBotController::ShouldUseLegacySupportActions(const FriendSituation& situation) const
{
    return !situation.inCombat && !situation.partyInCombat &&
        (situation.botMana >= FRIEND_MANA_BUFF_COMFORT || manualBuffUntil > time(nullptr));
}

bool FriendBotController::TryCastAbility(const FriendAbility& ability, Unit* target, const std::string& source)
{
    if (!ai || !ai->GetBot() || !IsUsableUnit(ai, target))
        return false;

    const bool duplicateAuraSensitive = ability.Has(FRIEND_ABILITY_DOT) ||
        ability.Has(FRIEND_ABILITY_HOT) ||
        ability.Has(FRIEND_ABILITY_SHIELD) ||
        ability.Has(FRIEND_ABILITY_BUFF_CORE) ||
        ability.Has(FRIEND_ABILITY_BUFF_SITUATIONAL) ||
        (ability.Has(FRIEND_ABILITY_BUFF) && !ability.Has(FRIEND_ABILITY_DAMAGE) && !ability.Has(FRIEND_ABILITY_DIRECT_DAMAGE)) ||
        (ability.Has(FRIEND_ABILITY_CC) && !ability.Has(FRIEND_ABILITY_DAMAGE) && !ability.Has(FRIEND_ABILITY_DIRECT_DAMAGE));
    if (duplicateAuraSensitive && HasEquivalentAura(ability, target))
        return false;

    Player* bot = ai->GetBot();
    if (bot->getClass() == CLASS_DRUID &&
        (ability.Has(FRIEND_ABILITY_HEAL) || ability.Has(FRIEND_ABILITY_CURE) ||
         ability.Has(FRIEND_ABILITY_BUFF_CORE) || ability.Has(FRIEND_ABILITY_BUFF_SITUATIONAL)) &&
        ai->HasAnyAuraOf(bot, "cat form", "bear form", "dire bear form", "travel form", "aquatic form",
            "flight form", "swift flight form", "moonkin form", "tree of life", NULL))
    {
        if (TryAction("caster form", source) != FriendExecutionResult::Done)
            return false;
    }

    if (IsValidFriendDamageTarget(target, true) && ability.Has(FRIEND_ABILITY_MELEE))
    {
        const float desiredDistance = std::max(sPlayerbotAIConfig.meleeDistance, sPlayerbotAIConfig.contactDistance);
        if (sServerFacade.GetDistance2d(ai->GetBot(), target) > desiredDistance || !ai->GetBot()->IsWithinLOSInMap(target, true))
            return TryReachAbilityTarget(ability, target, source);
    }

    SpellCastResult checkResult = SPELL_CAST_OK;
    if (!ai->CanCastSpell(ability.spellId, target, 0, true, nullptr, false, false, false, &checkResult))
    {
        if (checkResult == SPELL_FAILED_OUT_OF_RANGE || checkResult == SPELL_FAILED_LINE_OF_SIGHT)
            return TryReachAbilityTarget(ability, target, source);

        return false;
    }

    uint32 spellDuration = sPlayerbotAIConfig.globalCoolDown;
    if (!ai->CastSpell(ability.spellId, target, nullptr, false, &spellDuration))
    {
        SetResult(lastIntent, "spell:" + ability.name, FriendExecutionResult::Failed);
        return false;
    }

    SetResult(lastIntent, "spell:" + ability.name, FriendExecutionResult::Done);
    ai->SetActionDuration(spellDuration);
    return true;
}

bool FriendBotController::HasEquivalentAura(const FriendAbility& ability, Unit* target) const
{
    if (!ai || !target)
        return false;

    const bool ownedAura = ability.Has(FRIEND_ABILITY_DOT) || ability.Has(FRIEND_ABILITY_CC);
    if (ai->HasAura(ability.spellId, target, ownedAura))
        return true;

    if (ability.Has(FRIEND_ABILITY_BUFF) || ability.Has(FRIEND_ABILITY_HOT) || ability.Has(FRIEND_ABILITY_SHIELD))
    {
        if (ai->HasAura(ability.name, target, false, ownedAura))
            return true;
    }

    Player* bot = ai->GetBot();
    if (!bot || target != bot)
        return false;

    if (Contains(ability.lowerName, "mage armor") ||
        Contains(ability.lowerName, "ice armor") ||
        Contains(ability.lowerName, "frost armor") ||
        Contains(ability.lowerName, "molten armor"))
        return ai->HasAnyAuraOf(target, "mage armor", "ice armor", "frost armor", "molten armor", NULL);

    if (Contains(ability.lowerName, "demon skin") ||
        Contains(ability.lowerName, "demon armor") ||
        Contains(ability.lowerName, "fel armor"))
        return ai->HasAnyAuraOf(target, "demon skin", "demon armor", "fel armor", NULL);

    if (Contains(ability.lowerName, "aspect of the hawk") ||
        Contains(ability.lowerName, "aspect of the viper"))
        return ai->HasAnyAuraOf(target, "aspect of the hawk", "aspect of the viper", NULL);

    if (Contains(ability.lowerName, "lightning shield") ||
        Contains(ability.lowerName, "water shield"))
        return ai->HasAnyAuraOf(target, "lightning shield", "water shield", NULL);

    return false;
}

bool FriendBotController::ShouldMoveForAbilityTarget(const FriendAbility& ability, Unit* target) const
{
    if (!ai || !ai->GetBot() || !target || target == ai->GetBot())
        return false;

    if (ability.Has(FRIEND_ABILITY_MOVEMENT))
        return false;

    const bool damageTarget = IsValidFriendDamageTarget(target, true);
    const bool friendly = IsFriendlyTarget(ai, target);
    if (!damageTarget && !friendly)
        return false;

    const bool pointBlank = ability.maxRange <= sPlayerbotAIConfig.contactDistance;
    if (friendly && pointBlank)
        return false;

    const SpellEntry* spellInfo = sServerFacade.LookupSpellInfo(ability.spellId);
    bool explicitUnitTarget = spellInfo && (spellInfo->Targets & TARGET_FLAG_UNIT);
    bool destinationTarget = spellInfo && (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION);
    if (spellInfo)
    {
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            const uint32 targetA = spellInfo->EffectImplicitTargetA[i];
            const uint32 targetB = spellInfo->EffectImplicitTargetB[i];
            if (targetA == TARGET_UNIT || targetA == TARGET_UNIT_ENEMY ||
                targetB == TARGET_UNIT || targetB == TARGET_UNIT_ENEMY)
                explicitUnitTarget = true;

            if (targetA == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC ||
                targetB == TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC)
                destinationTarget = true;
        }
    }

    if (damageTarget && pointBlank && ability.Has(FRIEND_ABILITY_AOE) && !explicitUnitTarget)
        return false;

    if (pointBlank && !explicitUnitTarget && !destinationTarget && !ability.Has(FRIEND_ABILITY_MELEE))
        return false;

    return true;
}

bool FriendBotController::TryCatalogDamage(const FriendSituation& situation, const std::string& source)
{
    if (PreferFreeDamage(situation))
        return false;

    Unit* target = GetDamageTarget(situation, true);
    if (!IsValidFriendDamageTarget(target, true))
        return false;

    struct Candidate
    {
        const FriendAbility* ability;
        int32 score;
    };

    std::vector<Candidate> candidates;
    const uint8 targetHealth = HealthPercent(ai, target);
    const bool targetCasting = target->IsNonMeleeSpellCasted(false);
    const bool partyPeelTarget = target->GetObjectGuid() == situation.vulnerablePartyAttackerGuid &&
        (situation.vulnerablePartyHasThreat || situation.healerPartyHasThreat);
    const bool threatPeel = partyPeelTarget &&
        CanProtectPartyWithThreat(situation) &&
        target->GetVictim() &&
        target->GetVictim() != ai->GetBot();
    const bool usesComboPoints = ai && ai->GetBot() &&
        (ai->GetBot()->getClass() == CLASS_ROGUE || ai->GetBot()->getClass() == CLASS_DRUID);
    const uint8 comboPoints = usesComboPoints && target->GetObjectGuid() == ai->GetBot()->GetComboTargetGuid() ?
        ai->GetBot()->GetComboPoints() : 0;
    const int32 threatCaution = ThreatCautionScore(situation, target);

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_DAMAGE) && !ability.Has(FRIEND_ABILITY_INTERRUPT) &&
            !ability.Has(FRIEND_ABILITY_DAMAGE_COOLDOWN) && !ability.Has(FRIEND_ABILITY_THREAT))
            continue;

        if (ability.Has(FRIEND_ABILITY_HEAL) || ability.Has(FRIEND_ABILITY_BUFF_CORE) || ability.Has(FRIEND_ABILITY_BUFF_SITUATIONAL))
            continue;

        if (mode == FriendMode::Dungeon && ability.Has(FRIEND_ABILITY_FEAR))
            continue;

        if (ability.Has(FRIEND_ABILITY_CC) && !ability.Has(FRIEND_ABILITY_DAMAGE) && !ability.Has(FRIEND_ABILITY_INTERRUPT))
            continue;

        if (ability.Has(FRIEND_ABILITY_DOT) && targetHealth < 35)
            continue;

        if (ability.Has(FRIEND_ABILITY_AOE) && mode == FriendMode::Dungeon && situation.possibleTargetsCount < 3)
            continue;

        if (ability.Has(FRIEND_ABILITY_DAMAGE_COOLDOWN) && !situation.targetIsElite && situation.balance >= 90)
            continue;

        int32 score = 20;
        if (ability.Has(FRIEND_ABILITY_INTERRUPT))
            score += targetCasting ? 80 : -15;
        if (ability.Has(FRIEND_ABILITY_DOT))
            score += targetHealth > 55 ? 35 : 5;
        if (ability.Has(FRIEND_ABILITY_DIRECT_DAMAGE))
            score += 25;
        if (ability.Has(FRIEND_ABILITY_RANGED) && (situation.ranged || situation.healerish))
            score += 12;
        if (ability.Has(FRIEND_ABILITY_MELEE) && !situation.ranged)
            score += 12;
        if (ability.Has(FRIEND_ABILITY_THREAT))
        {
            if (situation.tankish || CanProtectPartyWithThreat(situation))
                score += 25;
            if (threatPeel)
                score += situation.healerPartyHasThreat ? 140 : 110;
        }
        if (threatPeel && ability.Has(FRIEND_ABILITY_DIRECT_DAMAGE))
            score += 15;
        if (ability.Has(FRIEND_ABILITY_AOE) && situation.possibleTargetsCount > 1)
            score += threatPeel ? 25 : 12;
        if (ability.Has(FRIEND_ABILITY_DAMAGE_COOLDOWN))
            score += 20;
        if (threatPeel && ability.Has(FRIEND_ABILITY_DOT) && !ability.Has(FRIEND_ABILITY_THREAT))
            score -= 25;
        if (threatCaution > 0 && !ability.Has(FRIEND_ABILITY_INTERRUPT) && !threatPeel)
        {
            if (ability.Has(FRIEND_ABILITY_THREAT))
                score -= threatCaution + 50;
            if (ability.Has(FRIEND_ABILITY_DAMAGE_COOLDOWN))
                score -= threatCaution + 25;
            if (ability.Has(FRIEND_ABILITY_AOE))
                score -= threatCaution / 2;
            if (ability.Has(FRIEND_ABILITY_DIRECT_DAMAGE))
                score -= threatCaution / 2;
            if (ability.Has(FRIEND_ABILITY_DOT))
                score -= threatCaution / 3;
        }
        if (usesComboPoints)
        {
            const bool comboSpender = ability.Has(FRIEND_ABILITY_COMBO_SPENDER);
            const bool comboBuilder = ability.Has(FRIEND_ABILITY_COMBO_BUILDER);
            if (comboSpender)
            {
                if (!comboPoints)
                    continue;

                score += static_cast<int32>(comboPoints) * 14;
                if (comboPoints < 3 && targetHealth > 30)
                    score -= 45;
                if (comboPoints >= 4 || targetHealth < 30)
                    score += 20;
            }
            else if (comboBuilder)
            {
                score += comboPoints < 5 ? 10 : -55;
            }
        }
        score -= ManaSpendScorePenalty(situation, ability);

        if (score > 0)
            candidates.push_back({ &ability, score });
    }

    OrderWeightedTopCandidates(candidates);

    for (const Candidate& candidate : candidates)
    {
        if (TryCastAbility(*candidate.ability, target, source))
            return true;
    }

    return false;
}

bool FriendBotController::TryCatalogHeal(const FriendSituation& situation, const std::string& source)
{
    Unit* target = GetHealTarget(situation);
    if (!IsFriendlyTarget(ai, target))
        return false;

    struct Candidate
    {
        const FriendAbility* ability;
        int32 score;
    };

    std::vector<Candidate> candidates;
    const uint8 targetHealth = HealthPercent(ai, target);
    if (targetHealth >= 99 && situation.lowestPartyHealthDelta >= 0)
        return false;

    const bool danger = targetHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_DANGER;
    const bool urgent = danger ||
        (targetHealth < sPlayerbotAIConfig.mediumHealth && situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_NOTICE);
    const bool topOff = targetHealth >= sPlayerbotAIConfig.mediumHealth &&
        situation.lowestPartyHealthDelta >= FRIEND_HEALTH_DROP_NOTICE;

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_HEAL) && !ability.Has(FRIEND_ABILITY_SHIELD))
            continue;

        if (ability.Has(FRIEND_ABILITY_DAMAGE) || ability.Has(FRIEND_ABILITY_CURE))
            continue;

        int32 score = 20;
        const bool instantHeal = ability.castTime == 0;
        const bool fastHeal = ability.castTime > 0 && ability.castTime <= 1600;
        const bool longHeal = ability.castTime >= 2200;
        const bool mediumHeal = !instantHeal && !fastHeal && !longHeal;

        if (ability.Has(FRIEND_ABILITY_SHIELD))
            score += urgent ? 45 : (topOff ? 10 : 25);
        if (ability.Has(FRIEND_ABILITY_HOT))
            score += topOff ? 55 : (urgent ? 8 : 30);
        if (ability.Has(FRIEND_ABILITY_HEAL) && !ability.Has(FRIEND_ABILITY_HOT))
            score += urgent ? 35 : (topOff ? 5 : 25);
        if (danger)
            score += 40;
        if (situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_NOTICE)
            score += 25;

        if (instantHeal)
            score += danger ? 25 : (urgent ? 20 : (topOff ? 10 : 15));
        if (fastHeal)
            score += danger ? 45 : (urgent ? 45 : -20);
        if (longHeal)
            score += danger ? -10 : (urgent ? 5 : (topOff ? -15 : 35));
        if (mediumHeal)
            score += topOff ? 10 : (urgent ? 15 : 25);
        if (targetHealth > sPlayerbotAIConfig.almostFullHealth && !ability.Has(FRIEND_ABILITY_HOT) && !ability.Has(FRIEND_ABILITY_SHIELD))
            score -= 35;

        if (score > 0)
            candidates.push_back({ &ability, score });
    }

    OrderWeightedTopCandidates(candidates);

    for (const Candidate& candidate : candidates)
    {
        if (TryCastAbility(*candidate.ability, target, source))
            return true;
    }

    return false;
}

bool FriendBotController::TryCatalogSupport(const FriendSituation& situation, const std::string& source)
{
    std::vector<Unit*> party = GetPartyTargets();
    if (party.empty())
        return false;

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_CURE) || (!ability.dispelType && !ability.dispelMask))
            continue;

        for (uint32 dispelType = 1; dispelType < 32; ++dispelType)
        {
            if (!(ability.dispelMask & (1u << dispelType)) && ability.dispelType != dispelType)
                continue;

            for (Unit* target : party)
            {
                if (ai->HasAuraToDispel(target, dispelType) && TryCastAbility(ability, target, source))
                    return true;
            }
        }
    }

    const bool manualBuff = manualBuffUntil > time(nullptr);
    if (situation.inCombat || situation.partyInCombat ||
        (situation.botMana < FRIEND_MANA_BUFF_COMFORT && !manualBuff))
        return false;

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_BUFF_CORE) || ability.Has(FRIEND_ABILITY_BUFF_SITUATIONAL) ||
            ability.Has(FRIEND_ABILITY_HEAL) || ability.Has(FRIEND_ABILITY_DAMAGE) || ability.Has(FRIEND_ABILITY_CURE))
            continue;

        if (ability.duration == 0 || (ability.duration > 0 && ability.duration < FRIEND_DOWNTIME_BUFF_MIN_DURATION))
            continue;

        if (ability.powerType != POWER_MANA && (ability.manaCost > 0 || ability.manaCostPercent > 0))
            continue;

        Player* bot = ai->GetBot();
        const bool selfOnlyBuff = IsSelfOnlyCoreBuffName(ability.lowerName);
        for (Unit* target : party)
        {
            if (selfOnlyBuff && target != bot)
                continue;

            if (!HasEquivalentAura(ability, target) && TryCastAbility(ability, target, source))
                return true;
        }
    }

    return false;
}

bool FriendBotController::TryCatalogCrowdControl(const FriendSituation& situation, const std::string& source)
{
    if (!ai || !ai->GetAiObjectContext())
        return false;

    Unit* damageTarget = GetDamageTarget(situation, false);

    struct Candidate
    {
        const FriendAbility* ability;
        Unit* target;
        std::string reason;
        int32 score;
    };

    std::vector<Candidate> candidates;
    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_CC) && !ability.Has(FRIEND_ABILITY_INTERRUPT))
            continue;

        if (mode == FriendMode::Dungeon && ability.Has(FRIEND_ABILITY_FEAR))
            continue;

        Unit* target = GetCrowdControlTarget(situation, ability, damageTarget);
        if (!IsHostileTarget(ai, target))
            continue;

        int32 score = 20;
        std::string reason = "cc";
        if (target->GetObjectGuid() == situation.closestAttackerTargetingMeGuid)
        {
            score += 45;
            reason = "cc:self-threat";
        }
        else if (target->GetObjectGuid() == situation.rtiCcTargetGuid || IsMoonTarget(target))
        {
            score += 30;
            reason = "cc:mark";
        }

        if (ability.Has(FRIEND_ABILITY_INTERRUPT) && target == damageTarget && damageTarget && damageTarget->IsNonMeleeSpellCasted(false))
        {
            score += 80;
            reason = "interrupt";
        }
        if (ability.Has(FRIEND_ABILITY_FEAR))
            score -= mode == FriendMode::Dungeon ? 100 : 10;
        if (ability.Has(FRIEND_ABILITY_AOE) && situation.possibleTargetsCount < 3)
            score -= 20;
        if (ability.Has(FRIEND_ABILITY_ROOT))
        {
            if (sServerFacade.GetDistance2d(ai->GetBot(), target) < 8.0f && !situation.healerish)
                score -= 70;
            if (PrefersMeleeDamage(situation) && situation.possibleTargetsCount <= 1)
                score -= 50;
        }

        if (score > 0)
            candidates.push_back({ &ability, target, reason, score });
    }

    OrderWeightedTopCandidates(candidates);

    for (const Candidate& candidate : candidates)
    {
        lastTargetReason = candidate.reason;
        if (TryCastAbility(*candidate.ability, candidate.target, source))
            return true;
    }

    return false;
}

bool FriendBotController::IsTargetSetupAction(const std::string& name) const
{
    return name == "dps assist" ||
        name == "tank assist" ||
        name == "attack least hp target" ||
        name == "attack rti target";
}

bool FriendBotController::ExecuteLoot(const FriendSituation& situation, bool allowObjects)
{
    (void)situation;
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext())
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    LootObjectStack* availableLoot = GetContextValue<LootObjectStack*>(context, "available loot", nullptr);
    LootObject lootTarget = GetContextValue<LootObject>(context, "loot target", LootObject());

    if (!lootTarget.IsEmpty() && !lootTarget.guid.IsCreature() && !allowObjects)
    {
        if (availableLoot)
            availableLoot->Remove(lootTarget.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
        lootTarget = LootObject();
    }

    if (lootTarget.IsEmpty())
    {
        if (!availableLoot)
            return false;

        for (uint8 i = 0; i < 10; ++i)
        {
            LootObject loot = availableLoot->GetLoot(sPlayerbotAIConfig.lootDistance);
            if (loot.IsEmpty())
                return false;

            if (loot.guid.IsCreature() || allowObjects)
                return TryAction("loot", "friend loot") == FriendExecutionResult::Done;

            availableLoot->Remove(loot.guid);
        }

        return false;
    }

    WorldObject* lootWorldObject = lootTarget.GetWorldObject(ai->GetBot());
    if (!lootWorldObject)
    {
        if (availableLoot)
            availableLoot->Remove(lootTarget.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
        return false;
    }

    if (sServerFacade.GetDistance2d(ai->GetBot(), lootWorldObject) <= INTERACTION_DISTANCE)
    {
        if (TryAction("open loot", "friend loot") == FriendExecutionResult::Done)
            return true;
    }

    if (TryAction("move to loot", "friend loot") == FriendExecutionResult::Done)
        return true;

    if (TryAction("open loot", "friend loot") == FriendExecutionResult::Done)
        return true;

    return TryAction("release loot", "friend loot") == FriendExecutionResult::Done;
}

bool FriendBotController::MoveToRecoverPosition(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || !ai->CanMove() || !situation.nearestHostileGuid)
        return false;

    if (situation.nearestHostileDistance <= 0.0f || situation.nearestHostileDistance >= FRIEND_RECOVER_HOSTILE_DISTANCE)
        return false;

    Player* bot = ai->GetBot();
    Unit* hostile = ai->GetUnit(situation.nearestHostileGuid);
    if (!IsHostileTarget(ai, hostile) || !bot->GetMotionMaster())
        return false;

    const float away = std::atan2(bot->GetPositionY() - hostile->GetPositionY(),
        bot->GetPositionX() - hostile->GetPositionX());
    const float moveDistance = std::min(8.0f, std::max(4.0f, FRIEND_RECOVER_COMFORT_DISTANCE - situation.nearestHostileDistance));
    float x = bot->GetPositionX() + std::cos(away) * moveDistance;
    float y = bot->GetPositionY() + std::sin(away) * moveDistance;
    float z = bot->GetPositionZ();
    if (!NormalizeFriendMovePosition(x, y, z))
        return false;

    WorldPosition from(bot);
    WorldPosition to(bot->GetMapId(), x, y, z);
    if (!from.canPathTo(to, bot) || !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
        return false;

    ClearFriendMovement(false);
    if (!MoveFriendPoint(x, y, z))
        return false;

    SetResult(lastIntent, "recover position", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool FriendBotController::ExecuteResupply(const FriendSituation& situation)
{
    if (!IsSafeForTownChores(situation))
        return false;

    const time_t now = time(nullptr);
    const bool serviceTravelTarget = taskTravelRequested && situation.travelTargetWorking &&
        IsServiceTravelPurpose(situation.travelTargetPurpose);
    const bool expectedServiceNearby = situation.travelTargetPurpose == FRIEND_REPAIR_TRAVEL_PURPOSE ?
        situation.nearbyRepair : situation.nearbyVendor;
    if (serviceTravelTarget && !expectedServiceNearby)
    {
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        if (command != FriendCommand::Shop)
            nextResupplyAttemptAt = now + FRIEND_RESUPPLY_RETRY_COOLDOWN;

        ClearFriendTravelTarget();
        if (command == FriendCommand::Shop)
        {
            command = FriendCommand::None;
            ClearExecutionState();
        }

        SetResult(lastIntent, "shop blocked:no service npc", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (situation.nearbyRepair && situation.shouldRepair &&
        TryServiceAction("repair", "", UNIT_NPC_FLAG_REPAIR) == FriendExecutionResult::Done)
    {
        MaybeSayActivity(situation, "resupply-repair", {
            "Repairing my gear.",
            "Getting repairs done."
        }, command == FriendCommand::Shop ? 70 : 25, 60);
        return true;
    }

    if (situation.nearbyVendor)
    {
        if (situation.shouldSell && TryEquipUpgrades(situation, true))
            return true;

        if (situation.shouldSell)
        {
            if (TryServiceAction("sell", "friend", FriendVendorNpcFlags()) == FriendExecutionResult::Done)
            {
                MaybeSayActivity(situation, "resupply-sell", {
                    "Selling junk while I'm here.",
                    "Clearing out my bags."
                }, command == FriendCommand::Shop ? 70 : 25, 60);
                return true;
            }
        }

        if (situation.shouldBuy &&
            TryServiceAction("buy", "vendor", FriendVendorNpcFlags()) == FriendExecutionResult::Done)
        {
            MaybeSayActivity(situation, "resupply-buy", {
                "Restocking supplies.",
                "Buying supplies."
            }, command == FriendCommand::Shop ? 70 : 25, 60);
            return true;
        }
    }

    if (TrySoftTownProgression(situation))
        return true;

    if (serviceTravelTarget && expectedServiceNearby && NeedsTownChores(situation))
    {
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        if (command != FriendCommand::Shop)
            nextResupplyAttemptAt = now + FRIEND_RESUPPLY_RETRY_COOLDOWN;

        ClearFriendTravelTarget();
        if (command == FriendCommand::Shop)
        {
            command = FriendCommand::None;
            ClearExecutionState();
            SetResult(lastIntent, "shop blocked:service action failed", FriendExecutionResult::BlockedNotUseful);
        }
        else
        {
            SetResult(lastIntent, "resupply deferred:service action failed", FriendExecutionResult::BlockedNotUseful);
        }
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (serviceTravelTarget && expectedServiceNearby && !NeedsTownChores(situation))
    {
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        ClearFriendTravelTarget();
        if (command == FriendCommand::Shop)
        {
            command = FriendCommand::None;
            ClearExecutionState();
            SetResult(lastIntent, "shop done", FriendExecutionResult::Done);
        }
        else
        {
            SetResult(lastIntent, "resupply done", FriendExecutionResult::Done);
        }
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (NeedsTownChores(situation) && TryTravelForResupply(situation))
        return true;

    if (command == FriendCommand::Shop)
    {
        const bool blocked = NeedsTownChores(situation);
        if (blocked)
        {
            taskTravelRequested = false;
            taskTravelPurpose = 0;
            ClearFriendTravelTarget();
            command = FriendCommand::None;
            ClearExecutionState();
            SetResult(lastIntent, "shop blocked", FriendExecutionResult::BlockedNotUseful);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;
        }

        command = FriendCommand::None;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        ClearFriendTravelTarget();
        ClearExecutionState();
        SetResult(lastIntent, "shop done", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (NeedsTownChores(situation))
    {
        nextResupplyAttemptAt = now + FRIEND_RESUPPLY_RETRY_COOLDOWN;
        SetResult(lastIntent, "resupply deferred", FriendExecutionResult::BlockedNotUseful);
    }

    return false;
}

bool FriendBotController::TryTravelForResupply(const FriendSituation& situation)
{
    if (mode == FriendMode::Dungeon)
        return false;

    const bool canStartResupplyTravel = command == FriendCommand::Shop || mode == FriendMode::Solo;
    const bool existingServiceTarget = IsServiceTravelPurpose(situation.travelTargetPurpose);

    if (situation.travelTargetPreparing && !taskTravelRequested)
    {
        if (existingServiceTarget && NeedsTownChores(situation))
        {
            taskTravelRequested = true;
            taskTravelPurpose = situation.travelTargetPurpose;
        }
        else
        {
            ClearFriendTravelTarget();
            SetResult(lastIntent, "clear stale travel target", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;
        }
    }

    if ((situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetActive) &&
        !taskTravelRequested)
    {
        if (existingServiceTarget && NeedsTownChores(situation))
        {
            taskTravelRequested = true;
            taskTravelPurpose = situation.travelTargetPurpose;
        }
        else
        {
            ClearFriendTravelTarget();
            SetResult(lastIntent, "clear travel target", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;
        }
    }

    if (situation.travelTargetPreparing && taskTravelRequested)
    {
        FriendExecutionResult result = TryAction("choose travel target", "friend resupply", 0, ai->GetBot());
        if (result == FriendExecutionResult::Done)
            return true;

        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target && target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        {
            SetResult(lastIntent, "shop preparing", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;
        }

        return false;
    }

    if (situation.travelTargetWorking && taskTravelRequested)
    {
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        if (command != FriendCommand::Shop)
            nextResupplyAttemptAt = time(nullptr) + FRIEND_RESUPPLY_RETRY_COOLDOWN;

        ClearFriendTravelTarget();
        if (command == FriendCommand::Shop)
        {
            command = FriendCommand::None;
            ClearExecutionState();
            SetResult(lastIntent, "shop blocked:stale travel target", FriendExecutionResult::BlockedNotUseful);
        }
        else
        {
            SetResult(lastIntent, "resupply deferred:stale travel target", FriendExecutionResult::BlockedNotUseful);
        }
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if ((situation.travelTargetTraveling || situation.travelTargetActive) && taskTravelRequested)
    {
        if (MoveToFriendTravelTarget(situation))
            return true;
        if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) &&
            !ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+travel once", BotState::BOT_STATE_NON_COMBAT);
        return TryAction("move to travel target", "friend resupply", 0, ai->GetBot()) == FriendExecutionResult::Done;
    }

    if (!canStartResupplyTravel)
        return false;

    uint32 purpose = (situation.shouldRepair && !situation.nearbyRepair) ?
        FRIEND_REPAIR_TRAVEL_PURPOSE : FRIEND_VENDOR_TRAVEL_PURPOSE;
    FriendExecutionResult result = TryRequestTravelTarget(purpose);
    if (result == FriendExecutionResult::Done)
    {
        taskTravelRequested = true;
        taskTravelPurpose = purpose;
        MaybeSayActivity(situation, "resupply-travel", {
            "I'm going to find a vendor.",
            "Going to handle supplies."
        }, command == FriendCommand::Shop ? 100 : 50, 60);
        return true;
    }

    if (result != FriendExecutionResult::BlockedNoAction)
    {
        if (command != FriendCommand::Shop)
            nextResupplyAttemptAt = time(nullptr) + FRIEND_RESUPPLY_RETRY_COOLDOWN;
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    return false;
}

bool FriendBotController::MoveToFriendTravelTarget(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !ai->CanMove())
    {
        SetResult(lastIntent, "move to travel target:blocked", FriendExecutionResult::BlockedNotPossible);
        return false;
    }

    Player* bot = ai->GetBot();
    if (!bot->GetMotionMaster())
    {
        SetResult(lastIntent, "move to travel target:no motion", FriendExecutionResult::BlockedNotPossible);
        return false;
    }

    TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (!target || !target->GetPosition() || !target->GetDestination())
    {
        SetResult(lastIntent, "move to travel target:no target", FriendExecutionResult::BlockedNotUseful);
        return false;
    }

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
        target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL && target->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK)
    {
        SetResult(lastIntent, "move to travel target:not traveling", FriendExecutionResult::BlockedNotUseful);
        return false;
    }

    WorldPosition destination = *target->GetPosition();
    if (destination.getMapId() != bot->GetMapId())
    {
        SetResult(lastIntent, "move to travel target:wrong map", FriendExecutionResult::BlockedNotPossible);
        return false;
    }

    WorldPosition from(bot);
    const bool serviceTravelTarget = IsServiceTravelPurpose(situation.travelTargetPurpose);
    auto findServiceStandPoint = [&](float& outX, float& outY, float& outZ) -> bool
    {
        const float approachDistance = std::min<float>(INTERACTION_DISTANCE * 0.75f,
            std::max<float>(1.8f, target->GetDestination()->GetRadiusMin() * 0.6f));
        float baseAngle = std::atan2(bot->GetPositionY() - destination.getY(), bot->GetPositionX() - destination.getX());
        if (std::fabs(bot->GetPositionX() - destination.getX()) < 0.05f &&
            std::fabs(bot->GetPositionY() - destination.getY()) < 0.05f)
            baseAngle = static_cast<float>(urand(0, 6283)) / 1000.0f;

        bool hasFallback = false;
        float fallbackX = 0.0f;
        float fallbackY = 0.0f;
        float fallbackZ = 0.0f;

        for (uint8 i = 0; i < 8; ++i)
        {
            float angle = baseAngle + static_cast<float>(i) * 0.785398f;
            float x = destination.getX() + std::cos(angle) * approachDistance;
            float y = destination.getY() + std::sin(angle) * approachDistance;
            float z = destination.getZ();
            if (!NormalizeFriendMovePosition(x, y, z))
                continue;

            if (!hasFallback)
            {
                fallbackX = x;
                fallbackY = y;
                fallbackZ = z;
                hasFallback = true;
            }

            WorldPosition standPoint(bot->GetMapId(), x, y, z);
            if (from.canPathTo(standPoint, bot) && bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
            {
                outX = x;
                outY = y;
                outZ = z;
                return true;
            }
        }

        if (!hasFallback)
            return false;

        outX = fallbackX;
        outY = fallbackY;
        outZ = fallbackZ;
        return true;
    };

    if (serviceTravelTarget && from.distance(destination) < 1.25f && !sServerFacade.isMoving(bot))
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (findServiceStandPoint(x, y, z))
        {
            ClearFriendMovement(false);
            if (MoveFriendPoint(x, y, z))
            {
                SetResult(lastIntent, "stand near travel target", FriendExecutionResult::Done);
                ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
                return true;
            }
        }
    }

    const float destinationRadius = target->GetDestination()->GetRadiusMin();
    const float localTravelRadius = serviceTravelTarget ? INTERACTION_DISTANCE :
        std::max<float>(INTERACTION_DISTANCE, std::min<float>(destinationRadius, mode == FriendMode::Solo ? 35.0f : 22.0f));
    const bool arrived = from.distance(destination) <= localTravelRadius;
    if (arrived)
    {
        target->SetStatus(TravelStatus::TRAVEL_STATUS_WORK);
        SetResult(lastIntent, "at travel target", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (IsMovingForAction(ai, lastAction, "move to travel target"))
    {
        SetResult(lastIntent, "move to travel target", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    float x = destination.getX();
    float y = destination.getY();
    float z = destination.getZ();
    if (serviceTravelTarget)
    {
        if (!findServiceStandPoint(x, y, z))
        {
            SetResult(lastIntent, "move to travel target:no nearby point", FriendExecutionResult::BlockedNotPossible);
            return false;
        }
    }
    else
    {
        const float radius = std::min<float>(destinationRadius, mode == FriendMode::Solo ? 35.0f : 22.0f);
        if (radius > INTERACTION_DISTANCE)
        {
            const float angle = static_cast<float>(urand(0, 6283)) / 1000.0f;
            const float distance = radius * (static_cast<float>(urand(50, 100)) / 100.0f);
            x += std::cos(angle) * distance;
            y += std::sin(angle) * distance;
        }
    }

    if (!NormalizeFriendMovePosition(x, y, z))
    {
        SetResult(lastIntent, "move to travel target:bad position", FriendExecutionResult::BlockedNotPossible);
        return false;
    }

    ClearFriendMovement(false);
    if (!MoveFriendPoint(x, y, z))
    {
        SetResult(lastIntent, "move to travel target:move failed", FriendExecutionResult::Failed);
        return false;
    }

    SetResult(lastIntent, "move to travel target", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool FriendBotController::NormalizeFriendMovePosition(float& x, float& y, float& z) const
{
    if (!ai || !ai->GetBot())
        return false;

    Player* bot = ai->GetBot();
    if (!bot->IsFlying() && !bot->IsFreeFlying())
    {
        WorldPosition botPosition(bot);
        WorldPosition destination(bot->GetMapId(), x, y, z);
        const bool waterRelevant = botPosition.isInWater() || botPosition.isUnderWater() ||
            destination.isInWater() || destination.isUnderWater();

        if (waterRelevant)
        {
            if (const TerrainInfo* terrain = destination.getTerrain())
            {
                float bottom = terrain->GetHeightStatic(x, y, z);
                const float waterLevel = terrain->GetWaterOrGroundLevel(x, y, z, bottom, true);
                if (waterLevel > -200000.0f && waterLevel > bottom)
                {
                    z = waterLevel;
                    return WorldPosition(bot->GetMapId(), x, y, z).isValid();
                }
            }
        }

        WorldPosition groundPosition(bot->GetMapId(), x, y, z);
        float ground = groundPosition.getHeight(false);
        if (ground > -200000.0f)
            z = ground;
        else
            bot->UpdateAllowedPositionZ(x, y, z);
    }

    return WorldPosition(bot->GetMapId(), x, y, z).isValid();
}

bool FriendBotController::MoveFriendPoint(float x, float y, float z)
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->GetMotionMaster())
        return false;

    if (!NormalizeFriendMovePosition(x, y, z))
        return false;

    FriendPointMovementAction move(ai);
    return move.Move(x, y, z);
}

bool FriendBotController::MoveToExplorePoint(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !ai->CanMove())
        return false;

    Player* bot = ai->GetBot();
    if ((situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetActive ||
        situation.travelTargetWorking) && !IsServiceTravelPurpose(situation.travelTargetPurpose))
        ClearFriendTravelTarget();

    Unit* leader = ai->GetGroupMaster();
    if (!leader || leader->GetObjectGuid() != situation.leaderGuid)
        leader = situation.leaderGuid ? ai->GetUnit(situation.leaderGuid) : nullptr;

    const bool keepNearLeader = mode != FriendMode::Solo;
    if (keepNearLeader && (!leader || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId() || !ai->IsSafe(leader)))
        return false;

    const bool useHeading = situation.partyHeadingActive && mode != FriendMode::Dungeon;
    const float headingAngle = useHeading ? std::atan2(situation.partyHeadingY, situation.partyHeadingX) : 0.0f;
    const float minDistance = mode == FriendMode::Solo ? 18.0f : 10.0f;
    const float maxDistance = mode == FriendMode::Solo ? 42.0f : 24.0f;
    WorldPosition from(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    for (uint8 attempt = 0; attempt < 10; ++attempt)
    {
        float angle = static_cast<float>(urand(0, 6283)) / 1000.0f;
        if (useHeading && attempt < 7)
        {
            const float spread = mode == FriendMode::Solo ? 1.4f : 0.95f;
            angle = headingAngle + (static_cast<float>(urand(0, 2000)) / 1000.0f - 1.0f) * spread;
        }

        const float distance = minDistance + static_cast<float>(urand(0, 1000)) / 1000.0f * (maxDistance - minDistance);
        float x = bot->GetPositionX() + std::cos(angle) * distance;
        float y = bot->GetPositionY() + std::sin(angle) * distance;
        float z = bot->GetPositionZ();
        if (!NormalizeFriendMovePosition(x, y, z))
            continue;

        WorldPosition to(bot->GetMapId(), x, y, z);
        if (!from.canPathTo(to, bot) || !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
            continue;

        if (keepNearLeader)
        {
            const float leaderDx = leader->GetPositionX() - x;
            const float leaderDy = leader->GetPositionY() - y;
            if (std::sqrt(leaderDx * leaderDx + leaderDy * leaderDy) > SoftLeashDistance(situation))
                continue;
        }

        bool hostileSafe = true;
        std::list<ObjectGuid> nearbyNpcs = context->GetValue<std::list<ObjectGuid> >("nearest npcs no los")->Get();
        for (std::list<ObjectGuid>::const_iterator itr = nearbyNpcs.begin(); itr != nearbyNpcs.end(); ++itr)
        {
            Unit* hostile = ai->GetUnit(*itr);
            if (!IsHostileTarget(ai, hostile))
                continue;

            const float candidateDx = hostile->GetPositionX() - x;
            const float candidateDy = hostile->GetPositionY() - y;
            const float candidateDistance = std::sqrt(candidateDx * candidateDx + candidateDy * candidateDy);
            if (candidateDistance < FRIEND_IDLE_MOVE_HOSTILE_BUFFER)
            {
                hostileSafe = false;
                break;
            }
        }

        if (!hostileSafe)
            continue;

        ClearFriendMovement(false);
        if (!MoveFriendPoint(x, y, z))
            continue;

        SetResult(lastIntent, "explore nearby", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    SetResult(lastIntent, "explore blocked:no safe point", FriendExecutionResult::BlockedNotPossible);
    return false;
}

bool FriendBotController::IsSafeForTownChores(const FriendSituation& situation) const
{
    if (command == FriendCommand::HoldPosition || command == FriendCommand::ReturnToParty ||
        command == FriendCommand::Recover)
        return false;

    const bool remoteSoloPartyCombat = mode == FriendMode::Solo &&
        situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation);

    if (situation.inCombat || situation.hasAttackers ||
        (situation.partyInCombat && !remoteSoloPartyCombat))
        return false;

    const uint8 choreHealthFloor = mode == FriendMode::Solo ?
        sPlayerbotAIConfig.mediumHealth : FRIEND_HEAL_TOP_OFF_HEALTH;
    if (situation.botHealth < choreHealthFloor)
        return false;

    if (mode != FriendMode::Solo && !remoteSoloPartyCombat &&
        situation.lowestPartyHealth < FRIEND_HEAL_TOP_OFF_HEALTH)
        return false;

    if (mode == FriendMode::Dungeon)
        return false;

    if (command == FriendCommand::Shop || mode == FriendMode::Solo)
        return true;

    if (!situation.leaderSafe || situation.leaderDistance > SoftLeashDistance(situation))
        return false;

    return situation.inTown || situation.nearbyVendor || situation.nearbyRepair;
}

bool FriendBotController::NeedsTownChores(const FriendSituation& situation) const
{
    if (situation.shouldRepair || situation.shouldSell || situation.shouldBuy)
        return true;

    if (WantsTownProgression(situation))
        return true;

    return command == FriendCommand::Shop && taskTravelRequested &&
        (situation.travelTargetPreparing || situation.travelTargetTraveling ||
         (situation.travelTargetActive && !situation.nearbyVendor && !situation.nearbyRepair));
}

bool FriendBotController::WantsTownProgression(const FriendSituation& situation) const
{
    if (mode == FriendMode::Dungeon)
        return false;

    const bool alreadyTownReady = situation.inTown || situation.nearbyVendor || situation.nearbyRepair;
    const uint8 levelGap = situation.leaderLevel > situation.botLevel ?
        situation.leaderLevel - situation.botLevel : 0;
    const bool majorLevelGap = levelGap >= 3;
    const bool manualShop = command == FriendCommand::Shop;
    const bool soloAutonomy = mode == FriendMode::Solo;
    const bool planningWindow = alreadyTownReady || manualShop || situation.calmDowntimeSeconds >= 45;

    if (!manualShop && !soloAutonomy)
        return false;

    const time_t now = time(nullptr);
    if (levelGap > 0 && now >= nextSoftLevelCatchupAt && (alreadyTownReady || manualShop || (majorLevelGap && planningWindow)))
        return true;

    if (situation.shouldTrain && now >= nextSoftTrainingAt && planningWindow)
        return true;

    if (situation.shouldUpgradeBags && now >= nextSoftBagUpgradeAt && planningWindow)
        return true;

    return situation.shouldUpgradeGear && now >= nextSoftGearUpgradeAt && planningWindow;
}

bool FriendBotController::TrySoftTownProgression(const FriendSituation& situation)
{
    if (!IsSafeForTownChores(situation))
        return false;

    if (TrySoftLevelCatchup(situation))
        return true;

    if (TrySoftTraining(situation))
        return true;

    if (TrySoftBagUpgrade(situation))
        return true;

    return TrySoftGearUpgrade(situation);
}

bool FriendBotController::TryEquipUpgrades(const FriendSituation& situation, bool force)
{
    if (!ai || !ai->GetBot())
        return false;

    Player* bot = ai->GetBot();
    const time_t now = time(nullptr);
    if (!force && now < nextEquipUpgradeAt)
        return false;

    if (situation.inCombat || situation.partyInCombat || situation.hasAttackers ||
        bot->GetTrader() || bot->IsBeingTeleported() || bot->IsTaxiFlying())
        return false;

    nextEquipUpgradeAt = now + FRIEND_EQUIP_UPGRADE_CHECK_COOLDOWN;
    const uint32 scoreBefore = ai->GetEquipGearScore(bot, false, false);
    const bool equipped = ai->DoSpecificAction("equip upgrades", Event("friend equip", "", ai->GetMaster()), true);
    const uint32 scoreAfter = ai->GetEquipGearScore(bot, false, false);
    if (!equipped && scoreAfter <= scoreBefore)
        return false;

    abilityCatalog.Reset();
    ClearFriendInventoryValues(ai);
    SetResult(FriendIntent::Resupply, "equip upgrades", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
    return true;
}

bool FriendBotController::TrySoftLevelCatchup(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || (!situation.inTown && !situation.nearbyVendor))
        return false;

    const time_t now = time(nullptr);
    if (now < nextSoftLevelCatchupAt)
        return false;

    Player* bot = ai->GetBot();
    Player* leader = ai->GetGroupMaster();
    if (!leader || !ai->IsSafe(leader))
        return false;

    const uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    if (bot->GetLevel() < leader->GetLevel())
    {
        uint32 targetLevel = std::min<uint32>(bot->GetLevel() + 1, std::min<uint32>(leader->GetLevel(), maxLevel));
        if (!ApplyFriendLevel(targetLevel))
            return false;

        nextSoftLevelCatchupAt = now + FRIEND_SOFT_LEVEL_CATCHUP_COOLDOWN;
        MaybeSayActivity(situation, "level-catchup", {
            "I caught up a bit while we were in town.",
            "I spent a little time catching up."
        }, 70, 120);
        SetResult(lastIntent, "town xp catchup", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (bot->GetLevel() != leader->GetLevel() || bot->GetLevel() >= maxLevel)
        return false;

    uint32 nextLevelXp = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    if (!nextLevelXp)
        nextLevelXp = sObjectMgr.GetXPForLevel(bot->GetLevel());
    if (!nextLevelXp)
        return false;

    uint32 botXp = bot->GetUInt32Value(PLAYER_XP);
    uint32 leaderXp = leader->GetUInt32Value(PLAYER_XP);
    if (leaderXp <= botXp + nextLevelXp / 4)
        return false;

    uint32 grant = std::min<uint32>(leaderXp - botXp, std::max<uint32>(1, nextLevelXp / 5));
    uint32 newXp = std::min<uint32>(botXp + grant, nextLevelXp - 1);
    if (newXp <= botXp)
        return false;

    bot->SetUInt32Value(PLAYER_XP, newXp);
    nextSoftLevelCatchupAt = now + FRIEND_SOFT_LEVEL_CATCHUP_COOLDOWN;
    SetResult(lastIntent, "town xp catchup", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
    return true;
}

bool FriendBotController::TrySoftTraining(const FriendSituation& situation)
{
#ifdef MANGOSBOT_ONE
    (void)situation;
    return false;
#else
    if (!ai || !ai->GetBot() || (!situation.inTown && !situation.nearbyVendor) || !situation.shouldTrain)
        return false;

    const time_t now = time(nullptr);
    if (now < nextSoftTrainingAt)
        return false;

    Player* bot = ai->GetBot();
    if (!situation.trainCost || bot->GetMoney() < situation.trainCost)
        return false;

    bot->ModifyMoney(-static_cast<int32>(situation.trainCost));
    bot->learnClassLevelSpells();
    abilityCatalog.Reset();
    nextSoftTrainingAt = now + FRIEND_SOFT_TRAINING_COOLDOWN;
    MaybeSayActivity(situation, "town-train", {
        "I trained while we were here.",
        "Picked up my class training."
    }, 55, 120);
    SetResult(lastIntent, "town training", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
    return true;
#endif
}

bool FriendBotController::TrySoftBagUpgrade(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || !situation.nearbyVendor || !situation.shouldUpgradeBags)
        return false;

    const time_t now = time(nullptr);
    if (now < nextSoftBagUpgradeAt)
        return false;

    Player* bot = ai->GetBot();
    if (EquippedBagSlots() >= 4)
        return false;

    struct BagChoice
    {
        uint32 itemId;
        uint32 cost;
        uint8 minLevel;
    };

    static const BagChoice bags[] =
    {
        { 4500, 50000, 30 },
        { 4497, 10000, 15 },
        { 4498, 2500, 8 },
        { 4496, 500, 1 }
    };

    uint32 selectedItem = 0;
    uint32 selectedCost = 0;
    uint32 freeMoney = ai->GetAiObjectContext() ?
        ai->GetAiObjectContext()->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::gear))->Get() :
        bot->GetMoney();

    for (const BagChoice& bag : bags)
    {
        if (bot->GetLevel() >= bag.minLevel && freeMoney >= bag.cost && bot->GetMoney() >= bag.cost)
        {
            selectedItem = bag.itemId;
            selectedCost = bag.cost;
            break;
        }
    }

    if (!selectedItem)
        return false;

    uint8 before = EquippedBagSlots();
    bot->StoreNewItemInBestSlots(selectedItem, 1);
    if (EquippedBagSlots() <= before)
    {
        nextSoftBagUpgradeAt = now + FRIEND_SOFT_BAG_UPGRADE_COOLDOWN;
        SetResult(lastIntent, "town bag upgrade failed", FriendExecutionResult::BlockedNotPossible);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return false;
    }

    bot->ModifyMoney(-static_cast<int32>(selectedCost));
    nextSoftBagUpgradeAt = now + FRIEND_SOFT_BAG_UPGRADE_COOLDOWN;
    MaybeSayActivity(situation, "town-bag", {
        "I bought another bag.",
        "Grabbed a bag upgrade."
    }, 60, 120);
    SetResult(lastIntent, "town bag upgrade", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
    return true;
}

bool FriendBotController::TrySoftGearUpgrade(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || !situation.nearbyVendor || !situation.shouldUpgradeGear)
        return false;

    const time_t now = time(nullptr);
    if (now < nextSoftGearUpgradeAt)
        return false;

    Player* bot = ai->GetBot();
    uint32 cost = std::max<uint32>(1000, (bot->GetLevel() * bot->GetLevel() * bot->GetLevel()) / 2);
    uint32 freeMoney = ai->GetAiObjectContext() ?
        ai->GetAiObjectContext()->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::gear))->Get() :
        bot->GetMoney();

    uint32 quality = ITEM_QUALITY_NORMAL;
    if (bot->GetLevel() >= 18 && freeMoney >= cost * 8)
    {
        quality = ITEM_QUALITY_UNCOMMON;
        cost *= 2;
    }
    if (bot->GetLevel() >= 35 && freeMoney >= cost * 8)
    {
        quality = ITEM_QUALITY_RARE;
        cost *= 3;
    }

    if (freeMoney < cost || bot->GetMoney() < cost)
        return false;

    uint32 before = ai->GetEquipGearScore(bot, false, false);
    PlayerbotFactory factory(bot, bot->GetLevel(), quality);
    factory.UpgradeGear(false);
    uint32 after = ai->GetEquipGearScore(bot, false, false);
    if (after <= before)
    {
        nextSoftGearUpgradeAt = now + FRIEND_SOFT_GEAR_UPGRADE_COOLDOWN;
        SetResult(lastIntent, "town gear upgrade failed", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return false;
    }

    bot->ModifyMoney(-static_cast<int32>(cost));
    nextSoftGearUpgradeAt = now + FRIEND_SOFT_GEAR_UPGRADE_COOLDOWN;
    MaybeSayActivity(situation, "town-gear", {
        "I found a small gear upgrade.",
        "Spent some coin on better gear."
    }, 60, 120);
    SetResult(lastIntent, "town gear upgrade", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
    return true;
}

bool FriendBotController::ForceLevel(uint32 level, Player* requester, std::string& response)
{
    if (!ai || !ai->GetBot())
    {
        response = "I can't change level right now.";
        return true;
    }

    uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    if (level < 1 || level > maxLevel)
    {
        std::ostringstream out;
        out << "Use forcelevel 1-" << maxLevel << ".";
        response = out.str();
        return true;
    }

    if (!ApplyFriendLevel(level))
    {
        response = "I couldn't change level.";
        return true;
    }

    if (requester)
        Report(requester);

    response = "Level set.";
    return true;
}

bool FriendBotController::ForceLevelSync(Player* requester, std::string& response)
{
    Player* target = requester ? requester : (ai ? ai->GetMaster() : nullptr);
    if (!target)
    {
        response = "No player to sync level with.";
        return true;
    }

    return ForceLevel(target->GetLevel(), requester, response);
}

bool FriendBotController::ForceGearSync(Player* requester, std::string& response)
{
    if (!ai || !ai->GetBot())
    {
        response = "I can't change gear right now.";
        return true;
    }

    Player* bot = ai->GetBot();
    Player* master = requester ? requester : ai->GetMaster();
    uint32 targetLevel = bot->GetLevel();
    if (master)
        targetLevel = std::min<uint32>(targetLevel, master->GetLevel());

    PlayerbotFactory factory(bot, targetLevel, ITEM_QUALITY_NORMAL);
    factory.UpgradeGear(master != nullptr);
    abilityCatalog.Reset();
    response = "Gear synced.";
    return true;
}

bool FriendBotController::ForceGearEmpty(Player* requester, std::string& response)
{
    (void)requester;
    if (!ai || !ai->GetBot())
    {
        response = "I can't clear gear right now.";
        return true;
    }

    Player* bot = ai->GetBot();
    uint32 removed = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        ++removed;
    }

    abilityCatalog.Reset();
    std::ostringstream out;
    out << "Removed " << removed << " equipped items.";
    response = out.str();
    return true;
}

bool FriendBotController::ForceItemClear(Player* requester, std::string& response)
{
    (void)requester;
    if (!ai || !ai->GetBot())
    {
        response = "I can't clear items right now.";
        return true;
    }

    static const uint32 HEARTHSTONE_ITEM_ID = 6948;

    Player* bot = ai->GetBot();
    struct ItemSlot
    {
        uint8 bag;
        uint8 slot;
        uint32 count;
        ObjectGuid guid;
    };

    std::vector<ItemSlot> remove;
    auto queueRemove = [&](Item* item)
    {
        if (!item || item->IsInTrade() || !item->GetProto() || item->GetProto()->ItemId == HEARTHSTONE_ITEM_ID)
            return;

        remove.push_back({ item->GetBagSlot(), item->GetSlot(), item->GetCount(), item->GetObjectGuid() });
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        queueRemove(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (!pBag)
            continue;

        for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            queueRemove(bot->GetItemByPos(bag, slot));
    }

    uint32 removed = 0;
    uint32 failed = 0;
    for (const ItemSlot& slot : remove)
    {
        bot->DestroyItem(slot.bag, slot.slot, true);
        Item* remaining = bot->GetItemByPos(slot.bag, slot.slot);
        if (!remaining || remaining->GetObjectGuid() != slot.guid)
            removed += slot.count;
        else
            ++failed;
    }

    ClearFriendInventoryValues(ai);
    std::ostringstream out;
    out << "Cleared " << removed << " bag item(s), keeping hearthstone.";
    if (failed)
        out << " Failed to remove " << failed << " slot(s).";
    response = out.str();
    return true;
}

bool FriendBotController::ForceItemJunk(Player* requester, std::string& response)
{
    (void)requester;
    if (!ai || !ai->GetBot())
    {
        response = "I can't add junk items right now.";
        return true;
    }

    static const uint32 TEST_ITEMS[] =
    {
        // Poor quality gear.
        1376, 1377, 1378, 1379, 1380, 1411, 1412, 1413, 1414, 1415, 1416,
        // Low-level green gear.
        4561, 4562, 4563, 4564, 4565, 4566, 4567, 4568, 4569, 4570, 4571, 4575,
        3184, 3192,
        // Vendor trash.
        7073, 7074, 7096
    };

    Player* bot = ai->GetBot();
    uint32 added = 0;
    uint32 failedInRow = 0;
    InventoryResult lastFailure = EQUIP_ERR_OK;
    const uint32 itemCount = sizeof(TEST_ITEMS) / sizeof(TEST_ITEMS[0]);
    for (uint32 attempts = 0; attempts < itemCount * 8 && failedInRow < itemCount; ++attempts)
    {
        uint32 itemId = TEST_ITEMS[attempts % itemCount];
        if (StoreFriendDebugItem(bot, itemId, 1, &lastFailure))
        {
            ++added;
            failedInRow = 0;
        }
        else
            ++failedInRow;
    }

    ClearFriendInventoryValues(ai);
    std::ostringstream out;
    out << "Added " << added << " junk test item(s).";
    if (!added)
        out << " Store failed, err=" << static_cast<uint32>(lastFailure) << ".";
    response = out.str();
    return true;
}

bool FriendBotController::ApplyFriendLevel(uint32 level)
{
    if (!ai || !ai->GetBot())
        return false;

    Player* bot = ai->GetBot();
    uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    level = std::max<uint32>(1, std::min<uint32>(level, maxLevel));
    bot->SetLevel(level);
    bot->SetUInt32Value(PLAYER_XP, 0);
    bot->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr.GetXPForLevel(level));
    if (Pet* pet = bot->GetPet())
        pet->SetLevel(level);

#ifndef MANGOSBOT_ONE
    bot->learnClassLevelSpells();
#endif

    abilityCatalog.Reset();
    return true;
}

uint8 FriendBotController::EquippedBagSlots() const
{
    if (!ai || !ai->GetBot())
        return 0;

    uint8 count = 0;
    Player* bot = ai->GetBot();
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++count;
    }

    return count;
}

void FriendBotController::MaybeProposeTownChores(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot() || mode != FriendMode::Party || command != FriendCommand::None ||
        situation.inCombat || situation.hasAttackers || situation.partyInCombat)
        return;

    const time_t now = time(nullptr);
    if (pendingProposal != FriendProposal::None && proposalExpiresAt < now)
        ClearProposal();

    if (pendingProposal != FriendProposal::None || now < nextProposalAt)
        return;

    if (!NeedsTownChores(situation) || IsSafeForTownChores(situation))
        return;

    const bool urgent = situation.bagSpace >= 95 ||
        (situation.shouldRepair && situation.durability < 60) ||
        situation.lowWater || situation.lowAmmo;
    if (!urgent)
        return;

    const char* line = "I need to resupply soon. Say ok if we should head to town.";
    if (situation.bagSpace >= 95)
        line = "My bags are full. Say ok if we should find a vendor.";
    else if (situation.shouldRepair && situation.durability < 60)
        line = "My gear is getting rough. Say ok if we should repair.";
    else if (situation.lowWater || situation.lowAmmo)
        line = "I'm low on supplies. Say ok if we should resupply.";

    if (ai->GetBot()->GetGroup())
        ai->SayToParty(line);
    else
        ai->Say(line);

    pendingProposal = FriendProposal::Resupply;
    proposalExpiresAt = now + 90;
    nextProposalAt = now + FRIEND_PROPOSAL_COOLDOWN;
}

void FriendBotController::ClearProposal()
{
    pendingProposal = FriendProposal::None;
    proposalExpiresAt = 0;
}

int32 FriendBotController::IntentFailurePenalty(FriendIntent intent, time_t now) const
{
    const uint8 index = static_cast<uint8>(intent);
    if (index >= static_cast<uint8>(FriendIntent::Max))
        return 0;

    const int32 penalty = intentFailurePenalty[index];
    if (penalty <= 0)
        return 0;

    const time_t age = std::max<time_t>(0, now - intentFailurePenaltyAt[index]);
    return std::max<int32>(0, penalty - static_cast<int32>(age) * FRIEND_INTENT_FAILURE_DECAY_PER_SECOND);
}

void FriendBotController::AddIntentFailurePenalty(FriendIntent intent, int32 amount)
{
    const uint8 index = static_cast<uint8>(intent);
    if (index >= static_cast<uint8>(FriendIntent::Max) || amount <= 0)
        return;

    const time_t now = time(nullptr);
    intentFailurePenalty[index] = std::min<int32>(
        FRIEND_INTENT_FAILURE_MAX_PENALTY,
        IntentFailurePenalty(intent, now) + amount);
    intentFailurePenaltyAt[index] = now;
}

void FriendBotController::ClearIntentFailurePenalty(FriendIntent intent)
{
    const uint8 index = static_cast<uint8>(intent);
    if (index >= static_cast<uint8>(FriendIntent::Max))
        return;

    intentFailurePenalty[index] = 0;
    intentFailurePenaltyAt[index] = 0;
}

void FriendBotController::ClearIntentFailurePenalties()
{
    for (uint8 i = 0; i < static_cast<uint8>(FriendIntent::Max); ++i)
    {
        intentFailurePenalty[i] = 0;
        intentFailurePenaltyAt[i] = 0;
    }
}

bool FriendBotController::ExecuteTaskIntent(FriendIntent intent, const FriendSituation& situation)
{
    if (!ai || !ai->GetBot())
        return false;

    if (!IsSafeForTaskActivity(situation))
    {
        ClearExecutionState();

        if (situation.leaderSafe && situation.leaderDistance > PreferredLeaderDistance(situation))
            return MoveInLeaderOrbit(situation, "move near leader", false);

        return false;
    }

    FriendTaskType selected = SelectTaskForIntent(intent, situation);
    if (selected == FriendTaskType::None)
        return false;

    return ExecuteCurrentTask(situation);
}

bool FriendBotController::IsSafeForTaskActivity(const FriendSituation& situation) const
{
    if (command == FriendCommand::HoldPosition || command == FriendCommand::ReturnToParty ||
        command == FriendCommand::Recover)
        return false;

    const bool remoteSoloPartyCombat = mode == FriendMode::Solo &&
        situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation);

    if (situation.inCombat || situation.hasAttackers ||
        (situation.partyInCombat && !remoteSoloPartyCombat))
        return false;

    const uint8 idleHealthFloor = mode == FriendMode::Solo ?
        sPlayerbotAIConfig.mediumHealth : FRIEND_HEAL_TOP_OFF_HEALTH;
    if (situation.botHealth < idleHealthFloor)
        return false;

    if (mode != FriendMode::Solo && !remoteSoloPartyCombat &&
        situation.lowestPartyHealth < FRIEND_HEAL_TOP_OFF_HEALTH)
        return false;

    if (mode == FriendMode::Dungeon)
        return situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation);

    if (mode == FriendMode::Party)
        return situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation);

    return true;
}

FriendTaskType FriendBotController::SelectTaskForIntent(FriendIntent intent, const FriendSituation& situation)
{
    const time_t now = time(nullptr);
    if (executionTask != FriendTaskType::None && executionTaskUntil > now &&
        IntentForTask(executionTask) == intent)
        return executionTask;

    FriendTaskType nextTask = FriendTaskType::None;
    uint32 lease = 20;
    switch (intent)
    {
        case FriendIntent::Resupply:
            nextTask = FriendTaskType::Resupply;
            lease = command == FriendCommand::Shop ? 180 : 60;
            break;
        case FriendIntent::Gather:
            if (!HasGatherSkill() || mode == FriendMode::Dungeon)
                return FriendTaskType::None;
            nextTask = mode == FriendMode::Solo ? FriendTaskType::TravelToGather : FriendTaskType::GatherNearby;
            lease = mode == FriendMode::Solo ? urand(35, 90) : urand(15, 40);
            break;
        case FriendIntent::Grind:
            if (mode == FriendMode::Dungeon)
                return FriendTaskType::None;
            nextTask = situation.possibleTargetsCount > 0 ? FriendTaskType::GrindNearby : FriendTaskType::TravelToGrind;
            lease = mode == FriendMode::Solo ? urand(35, 90) : urand(18, 45);
            break;
        case FriendIntent::Explore:
            if (mode == FriendMode::Dungeon)
                return FriendTaskType::None;
            nextTask = FriendTaskType::ExploreNearby;
            lease = mode == FriendMode::Solo ? urand(35, 90) : urand(18, 45);
            break;
        case FriendIntent::HangOut:
        case FriendIntent::FollowOrIdle:
            nextTask = mode == FriendMode::Solo || urand(0, 1) ? FriendTaskType::HangOut : FriendTaskType::OrbitLeader;
            lease = mode == FriendMode::Dungeon ? urand(8, 20) : urand(5, 16);
            break;
        default:
            return FriendTaskType::None;
    }

    executionTask = nextTask;
    executionTaskUntil = now + lease;
    executionNextActionAt = 0;
    taskTravelRequested = false;
    taskTravelPurpose = 0;
    return executionTask;
}

bool FriendBotController::ExecuteCurrentTask(const FriendSituation& situation)
{
    const time_t now = time(nullptr);
    if (executionNextActionAt > now)
    {
        if (ai && ai->GetBot() && sServerFacade.isMoving(ai->GetBot()) && IsFriendMovementAction(lastAction))
        {
            SetResult(lastIntent, lastAction, FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
            return true;
        }

        SetResult(lastIntent, "task " + TaskName(executionTask), FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    switch (executionTask)
    {
        case FriendTaskType::Resupply:
            if (ExecuteResupply(situation))
            {
                executionNextActionAt = (IsFriendMovementAction(lastAction) || lastAction == "at travel target") ?
                    now : now + urand(2, 6);
                return true;
            }
            break;

        case FriendTaskType::GatherNearby:
            if (mode != FriendMode::Dungeon)
            {
                if (TryAction("add gathering loot", "friend idle", 0, ai->GetBot()) == FriendExecutionResult::Done)
                {
                    MaybeSayActivity(situation, "gather", {
                        "I'll gather what's nearby.",
                        "I see something worth gathering."
                    }, mode == FriendMode::Solo ? 45 : 20, 75);
                    if (ExecuteLoot(situation, true))
                    {
                        executionNextActionAt = now + urand(4, 10);
                        return true;
                    }

                    executionNextActionAt = now + urand(4, 10);
                    return true;
                }
                if (ExecuteLoot(situation, true))
                {
                    executionNextActionAt = now + urand(4, 10);
                    return true;
                }
            }
            break;

        case FriendTaskType::GrindNearby:
            if (mode != FriendMode::Dungeon && situation.possibleTargetsCount > 0)
            {
                if (GetDamageTarget(situation, true))
                {
                    if (TryFreeDamage(situation, "friend grind") ||
                        TryCatalogDamage(situation, "friend grind") ||
                        (PrefersMeleeDamage(situation) && MoveToDamageTarget(situation, "grind engage")) ||
                        TryActions(DamageActions(situation), "friend grind fallback"))
                    {
                        MaybeSayActivity(situation, "grind", {
                            "I'll clear a few nearby.",
                            "Going to fight something nearby."
                        }, mode == FriendMode::Solo ? 45 : 20, 75);
                        executionNextActionAt = now + urand(5, 12);
                        return true;
                    }
                }

                if (TryAction("attack anything", "friend idle") == FriendExecutionResult::Done)
                {
                    MaybeSayActivity(situation, "grind", {
                        "I'll clear a few nearby.",
                        "Going to fight something nearby."
                    }, mode == FriendMode::Solo ? 45 : 20, 75);
                    executionNextActionAt = now + urand(5, 12);
                    return true;
                }
            }
            break;

        case FriendTaskType::TravelToGrind:
            if (ExecuteTaskTravelGoal(situation, FRIEND_GRIND_TRAVEL_PURPOSE, FriendTaskType::GrindNearby,
                "travel to grind", {
                    "I'm going to find something useful to fight.",
                    "I'll look for a good place to grind."
                }))
            {
                executionNextActionAt = IsFriendMovementAction(lastAction) ? now : now + urand(3, 8);
                return true;
            }
            break;

        case FriendTaskType::TravelToGather:
        {
            uint32 gatherPurpose = SelectGatherTravelPurpose();
            if (gatherPurpose && ExecuteTaskTravelGoal(situation, gatherPurpose, FriendTaskType::GatherNearby,
                "travel to gather", {
                    "I'm going to look for materials.",
                    "I'll go gather for a bit."
                }))
            {
                executionNextActionAt = IsFriendMovementAction(lastAction) ? now : now + urand(3, 8);
                return true;
            }
            break;
        }

        case FriendTaskType::ExploreNearby:
            if (MoveToExplorePoint(situation))
            {
                MaybeSayActivity(situation, "explore", {
                    "I'm going to look around a bit.",
                    "I'll scout around nearby."
                }, mode == FriendMode::Solo ? 45 : 20, 90);
                executionTask = FriendTaskType::HangOut;
                executionTaskUntil = now + urand(5, 14);
                executionNextActionAt = now + urand(2, 5);
                AddIntentFailurePenalty(FriendIntent::Explore, mode == FriendMode::Solo ? 180 : 140);
                return true;
            }
            break;

        case FriendTaskType::OrbitLeader:
            if (MoveInLeaderOrbit(situation, "idle orbit", false))
            {
                executionNextActionAt = now + urand(8, 18);
                return true;
            }
            break;

        case FriendTaskType::HangOut:
            if (ai && ai->GetBot() && urand(0, 3) == 0)
            {
                switch (urand(0, 8))
                {
                    case 0: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_WAVE); break;
                    case 1: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_TALK); break;
                    case 2: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_CHEER); break;
                    case 3: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_QUESTION); break;
                    case 4: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_KNEEL); break;
                    case 5: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_DANCE); break;
                    default: ai->GetBot()->HandleEmoteCommand(EMOTE_ONESHOT_TALK); break;
                }

                MaybeSayActivity(situation, "hangout", {
                    "Hanging out a bit.",
                    "I'm still here.",
                    "Let me know when we're moving."
                }, mode == FriendMode::Solo ? 15 : 25, 90);
                executionNextActionAt = now + urand(4, 9);
                SetResult(lastIntent, "social emote", FriendExecutionResult::Done);
                ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
                return true;
            }

            if (mode != FriendMode::Solo && MoveInLeaderOrbit(situation, "idle loiter", false))
            {
                executionNextActionAt = now + urand(6, 14);
                return true;
            }

            executionNextActionAt = now + urand(2, 5);
            SetResult(lastIntent, "hang out", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;

        case FriendTaskType::None:
            break;
    }

    FriendTaskType failedGoal = executionTask;
    FriendIntent failedIntent = IntentForTask(failedGoal);
    ClearExecutionState();
    executionNextActionAt = now + urand(4, 10);
    if (IsActiveTask(failedGoal))
        AddIntentFailurePenalty(failedIntent, failedGoal == FriendTaskType::Resupply ? 120 : 80);

    if (IsActiveTask(failedGoal))
    {
        if (lastResult == FriendExecutionResult::Done || lastAction.empty())
            SetResult(lastIntent, "task " + TaskName(failedGoal) + " blocked", FriendExecutionResult::BlockedNotUseful);

        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (situation.leaderSafe)
        return MoveInLeaderOrbit(situation, "idle orbit", false);

    return false;
}

bool FriendBotController::ExecuteTaskTravelGoal(const FriendSituation& situation, uint32 purpose, FriendTaskType workGoal,
    const std::string& action, std::initializer_list<const char*> lines)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || mode == FriendMode::Dungeon || purpose == 0)
        return false;

    const bool soloTravel = mode == FriendMode::Solo;
    const bool partyBoredTravel = mode == FriendMode::Party &&
        command == FriendCommand::None &&
        situation.leaderSafe &&
        situation.leaderDistance <= SoftLeashDistance(situation) &&
        situation.botHealth >= FRIEND_HEAL_TOP_OFF_HEALTH &&
        situation.lowestPartyHealth >= FRIEND_HEAL_TOP_OFF_HEALTH &&
        situation.botMana >= sPlayerbotAIConfig.lowMana &&
        !NeedsTownChores(situation);
    if (!soloTravel && !partyBoredTravel)
    {
        SetResult(lastIntent, action + ":party not ready", FriendExecutionResult::BlockedNotUseful);
        return false;
    }

    if (taskTravelRequested && taskTravelPurpose == purpose && !IsServiceTravelPurpose(purpose) &&
        (situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetActive))
    {
        const uint32 maxTravelDistance = mode == FriendMode::Solo ? 450 : 140;
        if (!situation.travelTargetSameMap ||
            (situation.travelTargetDistanceKnown && situation.travelTargetDistance > maxTravelDistance))
        {
            taskTravelRequested = false;
            taskTravelPurpose = 0;
            ClearFriendTravelTarget();
            SetResult(lastIntent, action + ":target too far", FriendExecutionResult::BlockedNotUseful);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return false;
        }
    }

    if (situation.travelTargetWorking && situation.travelTargetPurpose == purpose)
    {
        executionTask = workGoal;
        executionTaskUntil = time(nullptr) + urand(45, 120);
        taskTravelRequested = false;
        taskTravelPurpose = 0;
        ClearFriendTravelTarget();
        SetResult(lastIntent, "arrived " + TaskName(workGoal), FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (taskTravelRequested && taskTravelPurpose && taskTravelPurpose != purpose)
    {
        SetResult(lastIntent, action + ":other travel active", FriendExecutionResult::BlockedNotUseful);
        return false;
    }

    if ((situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetActive) &&
        (!taskTravelRequested || taskTravelPurpose != purpose))
    {
        ClearFriendTravelTarget();
        SetResult(lastIntent, "clear travel target", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (situation.travelTargetPreparing && taskTravelRequested)
    {
        FriendExecutionResult result = TryAction("choose travel target", "friend idle", 0, ai->GetBot());
        if (result == FriendExecutionResult::Done)
            return true;

        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!target || target->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        {
            SetResult(lastIntent, action + ":prepare failed", FriendExecutionResult::Failed);
            return false;
        }

        SetResult(lastIntent, action + ":preparing", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if ((situation.travelTargetTraveling || situation.travelTargetActive) && taskTravelRequested)
    {
        if (MoveToFriendTravelTarget(situation))
            return true;

        if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) &&
            !ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+travel once", BotState::BOT_STATE_NON_COMBAT);

        FriendExecutionResult moveResult = TryAction("move to travel target", "friend idle", 0, ai->GetBot());
        if (moveResult == FriendExecutionResult::Done)
            return true;

        if (moveResult == FriendExecutionResult::BlockedNoAction)
            SetResult(lastIntent, action + ":move failed", FriendExecutionResult::Failed);
        return false;
    }

    FriendExecutionResult result = TryRequestTravelTarget(purpose);
    if (result == FriendExecutionResult::Done)
    {
        taskTravelRequested = true;
        taskTravelPurpose = purpose;
        MaybeSayActivity(situation, action, lines, 70, 90);
        return true;
    }

    if (result != FriendExecutionResult::BlockedNoAction)
    {
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    return false;
}

uint32 FriendBotController::SelectGatherTravelPurpose() const
{
    if (!ai || !ai->GetBot())
        return 0;

    Player* bot = ai->GetBot();
    std::vector<uint32> purposes;
    if (bot->HasSkill(SKILL_HERBALISM))
        purposes.push_back(static_cast<uint32>(TravelDestinationPurpose::GatherHerbalism));
    if (bot->HasSkill(SKILL_MINING))
        purposes.push_back(static_cast<uint32>(TravelDestinationPurpose::GatherMining));
    if (bot->HasSkill(SKILL_SKINNING))
        purposes.push_back(static_cast<uint32>(TravelDestinationPurpose::GatherSkinning));
    if (bot->HasSkill(SKILL_FISHING))
        purposes.push_back(static_cast<uint32>(TravelDestinationPurpose::GatherFishing));

    if (purposes.empty())
        return 0;

    uint32 personality = bot->GetObjectGuid().GetCounter();
    return purposes[personality % purposes.size()];
}

bool FriendBotController::HasGatherSkill() const
{
    return SelectGatherTravelPurpose() != 0;
}

void FriendBotController::UpdatePartyHeading(FriendSituation& situation, Unit* leader)
{
    auto publish = [&]()
    {
        if (partyHeadingConfidence >= FRIEND_HEADING_MIN_CONFIDENCE)
        {
            float x = partyHeadingX;
            float y = partyHeadingY;
            if (Normalize2d(x, y))
            {
                situation.partyHeadingActive = true;
                situation.partyHeadingConfidence = partyHeadingConfidence;
                situation.partyHeadingX = x;
                situation.partyHeadingY = y;
            }
        }
    };

    auto decay = [&](uint8 amount)
    {
        partyHeadingConfidence = amount >= partyHeadingConfidence ? 0 : static_cast<uint8>(partyHeadingConfidence - amount);
        if (!partyHeadingConfidence)
        {
            partyHeadingX = 0.0f;
            partyHeadingY = 0.0f;
        }

        publish();
    };

    if (!ai || !ai->GetBot())
        return;

    Player* bot = ai->GetBot();
    if (mode == FriendMode::Dungeon || !leader || !situation.leaderSafe ||
        leader->GetMapId() != bot->GetMapId() || !ai->IsSafe(leader))
    {
        decay(20);
        return;
    }

    const time_t now = time(nullptr);
    const ObjectGuid leaderGuid = leader->GetObjectGuid();
    if (!lastLeaderHeadingAt || lastLeaderHeadingGuid != leaderGuid || lastLeaderHeadingMap != leader->GetMapId())
    {
        lastLeaderHeadingGuid = leaderGuid;
        lastLeaderHeadingMap = leader->GetMapId();
        lastLeaderHeadingAt = now;
        lastLeaderHeadingX = leader->GetPositionX();
        lastLeaderHeadingY = leader->GetPositionY();
        publish();
        return;
    }

    if (now < lastLeaderHeadingAt + FRIEND_HEADING_SAMPLE_SECONDS)
    {
        publish();
        return;
    }

    float dx = leader->GetPositionX() - lastLeaderHeadingX;
    float dy = leader->GetPositionY() - lastLeaderHeadingY;
    const float step = std::sqrt(dx * dx + dy * dy);

    lastLeaderHeadingAt = now;
    lastLeaderHeadingX = leader->GetPositionX();
    lastLeaderHeadingY = leader->GetPositionY();

    if (situation.inCombat || situation.partyInCombat || situation.hasAttackers || situation.leaderInCombat)
    {
        decay(20);
        return;
    }

    if (step > FRIEND_HEADING_MAX_STEP)
    {
        partyHeadingConfidence = 0;
        partyHeadingX = 0.0f;
        partyHeadingY = 0.0f;
        publish();
        return;
    }

    if (step < FRIEND_HEADING_MIN_STEP)
    {
        decay(8);
        return;
    }

    if (!Normalize2d(dx, dy))
    {
        decay(8);
        return;
    }

    if (partyHeadingConfidence < FRIEND_HEADING_MIN_CONFIDENCE)
    {
        partyHeadingX = dx;
        partyHeadingY = dy;
    }
    else
    {
        float mixedX = partyHeadingX * 0.65f + dx * 0.35f;
        float mixedY = partyHeadingY * 0.65f + dy * 0.35f;
        if (Normalize2d(mixedX, mixedY))
        {
            partyHeadingX = mixedX;
            partyHeadingY = mixedY;
        }
        else
        {
            partyHeadingX = dx;
            partyHeadingY = dy;
        }
    }

    partyHeadingConfidence = static_cast<uint8>(std::min<uint32>(100,
        partyHeadingConfidence + (step >= 8.0f ? 25 : 15)));
    publish();
}

bool FriendBotController::IsIdleMovePositionSafe(const FriendSituation& situation, Unit* leader, float x, float y, float z) const
{
    if (!ai || !ai->GetBot() || !leader || !ai->GetAiObjectContext())
        return false;

    Player* bot = ai->GetBot();
    WorldPosition from(bot);
    WorldPosition to(bot->GetMapId(), x, y, z);
    if (!from.canPathTo(to, bot) || !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
        return false;

    const float leaderDx = leader->GetPositionX() - x;
    const float leaderDy = leader->GetPositionY() - y;
    if (std::sqrt(leaderDx * leaderDx + leaderDy * leaderDy) > SoftLeashDistance(situation))
        return false;

    if (mode == FriendMode::Dungeon)
        return true;

    AiObjectContext* context = ai->GetAiObjectContext();
    std::list<ObjectGuid> nearbyNpcs = context->GetValue<std::list<ObjectGuid> >("nearest npcs no los")->Get();
    for (std::list<ObjectGuid>::const_iterator itr = nearbyNpcs.begin(); itr != nearbyNpcs.end(); ++itr)
    {
        Unit* hostile = ai->GetUnit(*itr);
        if (!IsHostileTarget(ai, hostile))
            continue;

        const float candidateDx = hostile->GetPositionX() - x;
        const float candidateDy = hostile->GetPositionY() - y;
        const float candidateDistance = std::sqrt(candidateDx * candidateDx + candidateDy * candidateDy);
        if (candidateDistance < FRIEND_IDLE_MOVE_HOSTILE_BUFFER)
            return false;

        const float currentDistance = sServerFacade.GetDistance2d(bot, hostile);
        if (currentDistance < sPlayerbotAIConfig.sightDistance &&
            candidateDistance + 2.0f < currentDistance &&
            candidateDistance < FRIEND_IDLE_MOVE_HOSTILE_BUFFER + 6.0f)
            return false;
    }

    return true;
}

bool FriendBotController::PreferFreeDamage(const FriendSituation& situation) const
{
    return GetCombatStyle(situation) == FriendCombatStyle::Dry;
}

float FriendBotController::PreferredLeaderDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (mode == FriendMode::Dungeon)
        return std::max(followRange, 6.0f);
    if (mode == FriendMode::Solo)
        return std::max(followRange * 2.5f, 28.0f);

    if (situation.inTown)
        return std::max(followRange * 1.75f, 18.0f);

    return std::max(followRange * 1.25f, 12.0f);
}

float FriendBotController::SoftLeashDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (mode == FriendMode::Dungeon)
        return std::max(followRange * 1.25f, 12.0f);
    if (mode == FriendMode::Solo)
        return std::max(followRange * 4.0f, 60.0f);

    if (situation.inTown)
        return std::max(followRange * 3.0f, 45.0f);

    return std::max(followRange * 2.0f, 24.0f);
}

float FriendBotController::HardLeashDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (mode == FriendMode::Solo)
        return std::max(followRange * 6.0f, 120.0f);

    if (mode == FriendMode::Dungeon)
        return std::max(followRange * 2.5f, 30.0f);

    if (situation.inTown)
        return std::max(followRange * 5.0f, 90.0f);

    return std::max(followRange * 4.0f, 70.0f);
}

bool FriendBotController::MoveInLeaderOrbit(const FriendSituation& situation, const std::string& action, bool urgent)
{
    if (!ai || !ai->GetBot() || !situation.leaderGuid)
        return false;

    Player* bot = ai->GetBot();
    Unit* leader = ai->GetGroupMaster();
    if (!leader || leader->GetObjectGuid() != situation.leaderGuid)
        leader = ai->GetUnit(situation.leaderGuid);

    if (!leader || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId() ||
        !ai->IsSafe(leader) || !ai->CanMove() || !bot->GetMotionMaster())
        return false;

    if (IsMovingForAction(ai, lastAction, action))
    {
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    float minDistance = 5.0f;
    float maxDistance = 14.0f;
    if (mode == FriendMode::Dungeon || command == FriendCommand::StayClose)
    {
        minDistance = 3.0f;
        maxDistance = 8.0f;
    }
    else if (mode == FriendMode::Solo)
    {
        minDistance = 8.0f;
        maxDistance = 24.0f;
    }
    else if (situation.inTown)
    {
        minDistance = 6.0f;
        maxDistance = 20.0f;
    }

    if (urgent)
    {
        minDistance = 2.0f;
        maxDistance = std::max(ai->GetRange("follow") * 0.75f, 6.0f);
    }

    if (situation.leaderDistance >= minDistance && situation.leaderDistance <= maxDistance &&
        action != "idle orbit" && action != "idle loiter")
    {
        ClearFriendMovement(true);
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    const bool forwardOrbit = situation.partyHeadingActive && !urgent &&
        mode != FriendMode::Dungeon && command != FriendCommand::StayClose;
    const float forwardAngle = forwardOrbit ? std::atan2(situation.partyHeadingY, situation.partyHeadingX) : 0.0f;
    const float forwardSpread = mode == FriendMode::Solo ? 1.7f : 1.25f;
    float x = leader->GetPositionX();
    float y = leader->GetPositionY();
    float z = leader->GetPositionZ();
    bool found = false;

    for (uint8 attempt = 0; attempt < 8; ++attempt)
    {
        const float distance = minDistance + static_cast<float>(urand(0, 1000)) / 1000.0f * (maxDistance - minDistance);
        float angle = static_cast<float>(urand(0, 6283)) / 1000.0f;
        if (forwardOrbit && attempt < 5)
        {
            const float offset = (static_cast<float>(urand(0, 2000)) / 1000.0f - 1.0f) * forwardSpread;
            angle = forwardAngle + offset;
        }

        float candidateX = leader->GetPositionX() + std::cos(angle) * distance;
        float candidateY = leader->GetPositionY() + std::sin(angle) * distance;
        float candidateZ = leader->GetPositionZ();
        if (!NormalizeFriendMovePosition(candidateX, candidateY, candidateZ))
            continue;

        if (!IsIdleMovePositionSafe(situation, leader, candidateX, candidateY, candidateZ))
            continue;

        x = candidateX;
        y = candidateY;
        z = candidateZ;
        found = true;
        break;
    }

    if (!found)
    {
        x = leader->GetPositionX();
        y = leader->GetPositionY();
        z = leader->GetPositionZ();
        if (!NormalizeFriendMovePosition(x, y, z))
            return false;

        if (!IsIdleMovePositionSafe(situation, leader, x, y, z))
            return false;
    }

    ClearFriendMovement(false);
    if (!MoveFriendPoint(x, y, z))
        return false;

    SetResult(lastIntent, action, FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool FriendBotController::MoveNearLeader(const FriendSituation& situation, const std::string& action, bool urgent)
{
    if (!ai || !ai->GetBot() || !situation.leaderGuid)
        return false;

    Player* bot = ai->GetBot();
    Unit* leader = ai->GetGroupMaster();
    if (!leader || leader->GetObjectGuid() != situation.leaderGuid)
        leader = ai->GetUnit(situation.leaderGuid);

    if (!leader || !leader->IsInWorld() || leader->GetMapId() != bot->GetMapId() ||
        !ai->IsSafe(leader) || !ai->CanMove())
        return false;

    if (!urgent)
        return MoveInLeaderOrbit(situation, action, false);

    const float stopDistance = urgent ? std::max(ai->GetRange("follow") * 0.75f, 6.0f) : PreferredLeaderDistance(situation);
    if (situation.leaderDistance <= stopDistance)
    {
        ClearFriendMovement(true);
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    ClearFriendMovement(false);
    if (!MoveFriendPoint(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
        return false;

    SetResult(lastIntent, action, FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
}

void FriendBotController::ClearFriendMovement(bool includePointMove)
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->GetMotionMaster())
        return;

    MovementGeneratorType movementType = ai->GetBot()->GetMotionMaster()->GetCurrentMovementGeneratorType();
    const bool friendMovement = IsFriendMovementAction(lastAction);
    if (movementType == FOLLOW_MOTION_TYPE ||
        (includePointMove && friendMovement &&
            (movementType == POINT_MOTION_TYPE || movementType == CHASE_MOTION_TYPE)))
    {
        ai->StopMoving();
    }
}

void FriendBotController::SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result)
{
    lastIntent = intent;
    lastAction = action;
    lastResult = result;

    if (result == FriendExecutionResult::Done || result == FriendExecutionResult::IntentionalIdle)
        ClearIntentFailurePenalty(intent);
}

void FriendBotController::MaybeSayStatus(const FriendSituation& situation)
{
    if (!ai || verbosity == FriendVerbosity::Silent)
        return;

    std::ostringstream out;
    const std::string action = lastAction.empty() ? ResultName(lastResult) : lastAction;
    out << "[" << IntentName(lastIntent) << "] " << action;
    const bool travelAction = lastIntent == FriendIntent::Resupply ||
        lastIntent == FriendIntent::Gather ||
        lastIntent == FriendIntent::Grind ||
        lastIntent == FriendIntent::Explore ||
        lastIntent == FriendIntent::HangOut ||
        Contains(action, "travel");
    if (travelAction && (situation.travelTargetActive || situation.travelTargetPreparing ||
        situation.travelTargetTraveling || situation.travelTargetWorking))
    {
        out << " @ " << (situation.travelTargetName.empty() ? "travel target" : situation.travelTargetName);
        if (situation.travelTargetDistanceKnown)
            out << " " << situation.travelTargetDistance << "y";
    }
    else if (!situation.targetName.empty() &&
        (lastIntent == FriendIntent::DealDamage ||
         lastIntent == FriendIntent::PullWithParty ||
         lastIntent == FriendIntent::CrowdControl ||
         lastIntent == FriendIntent::SaveSelf ||
         Contains(action, "spell:")))
    {
        out << " @ " << situation.targetName;
        if (situation.targetDistance > 0.0f)
            out << " " << static_cast<uint32>(situation.targetDistance) << "y";
    }

    if (verbosity == FriendVerbosity::Debug || verbosity == FriendVerbosity::Weights)
    {
        out << " [" << ResultName(lastResult);
        out << ", mode " << ModeName(mode);
        out << ", command " << CommandName(command);
        out << ", task " << TaskName(executionTask);
        out << ", hp " << static_cast<uint32>(situation.botHealth) << "%";
        out << " (" << static_cast<int32>(situation.botHealthDelta) << ")";
        out << ", mana " << static_cast<uint32>(situation.botMana) << "%";
        out << ", lvl " << static_cast<uint32>(situation.botLevel);
        if (situation.leaderLevel)
            out << "/" << static_cast<uint32>(situation.leaderLevel);
        out << ", party " << static_cast<uint32>(situation.lowestPartyHealth) << "%";
        out << " (" << static_cast<int32>(situation.lowestPartyHealthDelta) << ")";
        out << ", combat " << (situation.inCombat ? "self" : "no");
        out << "/" << (situation.partyInCombat ? "party" : "no");
        out << ", style " << CombatStyleName(GetCombatStyle(situation));
        out << ", target " << (lastTargetReason.empty() ? "none" : lastTargetReason);
        if (!situation.targetName.empty())
            out << ":" << situation.targetName;
        out << ", threat ";
        if (situation.botHasThreat)
            out << "self";
        else if (situation.healerPartyHasThreat)
            out << "healer";
        else if (situation.vulnerablePartyHasThreat)
            out << "party";
        else
            out << "none";
        out << "(" << static_cast<uint32>(situation.attackersTargetingMeCount) << ")";
        out << ", leader " << static_cast<uint32>(situation.leaderDistance);
        if (situation.partyHeadingActive)
            out << ", heading " << static_cast<uint32>(situation.partyHeadingConfidence) << "%";
        out << ", targetDist " << static_cast<uint32>(situation.targetDistance);
        out << ", calm " << situation.calmDowntimeSeconds << "s";
        out << ", " << BalanceName(situation.balance);
        out << ", targets " << static_cast<uint32>(situation.possibleTargetsCount);
        out << ", town " << (situation.inTown ? "y" : "n");
        out << ", vendor " << (situation.nearbyVendor ? "y" : "n");
        out << ", repair " << (situation.nearbyRepair ? "y" : "n") << "]";
        if (situation.crowdControlledTargets)
            out << " [cc " << static_cast<uint32>(situation.crowdControlledTargets) << "]";
        if (situation.hasCreatureLoot)
            out << " [loot]";
        if (NeedsTownChores(situation))
            out << " [chores sell=" << (situation.shouldSell ? "y" : "n")
                << "(" << situation.sellableItems << ")"
                << " repair=" << (situation.shouldRepair ? "y" : "n")
                << " buy=" << (situation.shouldBuy ? "y" : "n")
                << " train=" << (situation.shouldTrain ? "y" : "n")
                << " bag=" << (situation.shouldUpgradeBags ? "y" : "n")
                << " gear=" << (situation.shouldUpgradeGear ? "y" : "n") << "]";
        if (nextResupplyAttemptAt > time(nullptr))
            out << " [resupply cd " << static_cast<uint32>(nextResupplyAttemptAt - time(nullptr)) << "s]";
        if (situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetWorking)
        {
            out << " [travel " << TravelStateName(situation) << " purpose=" << situation.travelTargetPurpose;
            if (!situation.travelTargetName.empty())
                out << " " << situation.travelTargetName;
            if (situation.travelTargetDistanceKnown)
                out << " " << situation.travelTargetDistance << "y";
            else if (!situation.travelTargetSameMap)
                out << " off-map";
            out << "]";
        }
        if (verbosity == FriendVerbosity::Weights && !lastWeightsLine.empty())
            out << " [" << lastWeightsLine << "]";
    }

    std::string statusLine = out.str();
    if (statusLine == lastStatusLine)
        return;

    lastStatusLine = statusLine;

    Player* master = ai->GetMaster();
    if (!master)
        return;

    ai->TellPlayerNoFacing(master, statusLine, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
}

void FriendBotController::MaybeSayActivity(const FriendSituation& situation, const std::string& key,
    std::initializer_list<const char*> lines, uint32 chancePercent, uint32 cooldownSeconds)
{
    (void)situation;
    if (!ai || !ai->GetBot() || lines.size() == 0)
        return;

    const time_t now = time(nullptr);
    if (now < nextActivityBarkAt)
        return;
    if (key == lastActivityBark && now < nextActivityBarkAt + static_cast<time_t>(cooldownSeconds))
        return;

    if (chancePercent < 100 && urand(1, 100) > chancePercent)
        return;

    uint32 index = urand(0, static_cast<uint32>(lines.size() - 1));
    const char* selected = *lines.begin();
    for (const char* line : lines)
    {
        selected = line;
        if (!index)
            break;
        --index;
    }

    if (!selected || !selected[0])
        return;

    if (ai->GetBot()->GetGroup())
        ai->SayToParty(selected);
    else
        ai->Say(selected);

    lastActivityBark = key;
    nextActivityBarkAt = now + cooldownSeconds;
}

bool FriendBotController::PrintInventory(Player* requester, const std::string& filter)
{
    if (!ai || !requester)
        return false;

    const std::string param = filter.empty() ? "inventory" : filter;
    return ai->DoSpecificAction("item count", Event("friend command", param, requester), true);
}

bool FriendBotController::PrintEquipment(Player* requester, const std::string& slotName)
{
    if (!ai || !ai->GetBot() || !requester)
        return false;

    struct SlotLine
    {
        const char* name;
        uint8 slot;
    };

    static const SlotLine slots[] =
    {
        { "head", EQUIPMENT_SLOT_HEAD },
        { "neck", EQUIPMENT_SLOT_NECK },
        { "shoulder", EQUIPMENT_SLOT_SHOULDERS },
        { "shirt", EQUIPMENT_SLOT_BODY },
        { "chest", EQUIPMENT_SLOT_CHEST },
        { "waist", EQUIPMENT_SLOT_WAIST },
        { "legs", EQUIPMENT_SLOT_LEGS },
        { "feet", EQUIPMENT_SLOT_FEET },
        { "wrist", EQUIPMENT_SLOT_WRISTS },
        { "hands", EQUIPMENT_SLOT_HANDS },
        { "finger 1", EQUIPMENT_SLOT_FINGER1 },
        { "finger 2", EQUIPMENT_SLOT_FINGER2 },
        { "trinket 1", EQUIPMENT_SLOT_TRINKET1 },
        { "trinket 2", EQUIPMENT_SLOT_TRINKET2 },
        { "back", EQUIPMENT_SLOT_BACK },
        { "main hand", EQUIPMENT_SLOT_MAINHAND },
        { "off hand", EQUIPMENT_SLOT_OFFHAND },
        { "ranged", EQUIPMENT_SLOT_RANGED },
        { "tabard", EQUIPMENT_SLOT_TABARD }
    };

    uint32 onlySlot = EQUIPMENT_SLOT_END;
    if (!slotName.empty())
    {
        onlySlot = ChatHelper::parseSlot(slotName);
        if (onlySlot >= EQUIPMENT_SLOT_END)
        {
            ai->TellPlayerNoFacing(requester, "Unknown equipment slot.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
            return true;
        }
    }

    ai->TellPlayerNoFacing(requester, "=== Equipment ===", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    for (const SlotLine& slotLine : slots)
    {
        if (onlySlot < EQUIPMENT_SLOT_END && onlySlot != slotLine.slot)
            continue;

        std::ostringstream out;
        out << slotLine.name << ": ";
        if (Item* item = ai->GetBot()->GetItemByPos(INVENTORY_SLOT_BAG_0, slotLine.slot))
            out << ChatHelper::formatItem(item);
        else
            out << "(empty)";

        ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    }

    return true;
}

Item* FriendBotController::FindBestTradeItem(const std::string& fragment) const
{
    if (!ai || !ai->GetBot())
        return nullptr;

    std::string needle = Trim(fragment);
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (needle.empty())
        return nullptr;

    Player* bot = ai->GetBot();
    Item* best = nullptr;
    int32 bestScore = -1;

    auto consider = [&](Item* item)
    {
        if (!item || item->IsInTrade() || !item->CanBeTraded() || !item->GetProto())
            return;

        std::string itemName = item->GetProto()->Name1 ? item->GetProto()->Name1 : "";
        std::string searchable = itemName + " " + ChatHelper::formatItem(item);
        std::transform(searchable.begin(), searchable.end(), searchable.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        size_t match = searchable.find(needle);
        if (match == std::string::npos)
            return;

        int32 score = 10000 - static_cast<int32>(std::min<size_t>(match, 100));
        if (StartsWith(searchable, needle))
            score += 5000;
        if (searchable == needle)
            score += 10000;
        score += static_cast<int32>(item->GetProto()->Quality) * 100;
        score += static_cast<int32>(std::min<uint32>(item->GetCount(), 20));

        if (score > bestScore)
        {
            bestScore = score;
            best = item;
        }
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (!pBag)
            continue;

        for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            consider(bot->GetItemByPos(bag, slot));
    }

    return best;
}

bool FriendBotController::TradeMatchingItem(Player* requester, const std::string& fragment, std::string& response)
{
    if (!ai || !ai->GetBot() || !requester)
    {
        response = "I can't trade right now.";
        return true;
    }

    if (Trim(fragment).empty())
    {
        response = "Trade what?";
        return true;
    }

    Item* item = FindBestTradeItem(fragment);
    if (!item || !item->GetProto())
    {
        response = "I don't have a tradable item matching that.";
        return true;
    }

    const std::string itemLink = ChatHelper::formatItem(item);
    Player* bot = ai->GetBot();
    if (bot->GetTrader() && bot->GetTrader() != requester)
    {
        response = "I'm already trading with someone else.";
        return true;
    }

    if (bot->GetMapId() != requester->GetMapId())
    {
        response = "I have " + itemLink + ", but we're not in the same place.";
        return true;
    }

    const float distance = sServerFacade.GetDistance2d(bot, requester);
    if (distance > FRIEND_TRADE_MAX_APPROACH_DISTANCE)
    {
        response = "I have " + itemLink + ", but I'm too far away to trade.";
        return true;
    }

    pendingTradeRequesterGuid = requester->GetObjectGuid();
    pendingTradeFragment = fragment;
    pendingTradeUntil = time(nullptr) + FRIEND_TRADE_TIMEOUT_SECONDS;
    nextPendingTradeAt = 0;
    ClearExecutionState();

    response = distance > INTERACTION_DISTANCE ?
        "I'll come trade " + itemLink + "." :
        "Opening trade for " + itemLink + ".";
    return true;
}

bool FriendBotController::TryPendingTrade(const FriendSituation& situation)
{
    (void)situation;
    if (!pendingTradeRequesterGuid)
        return false;

    if (!ai || !ai->GetBot())
    {
        ClearPendingTrade();
        return false;
    }

    const time_t now = time(nullptr);
    Player* bot = ai->GetBot();
    Player* requester = sObjectMgr.GetPlayer(pendingTradeRequesterGuid);
    if (!requester || !requester->IsInWorld())
    {
        SetResult(FriendIntent::FollowOrIdle, "trade blocked:player gone", FriendExecutionResult::BlockedNotPossible);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    if (now > pendingTradeUntil)
    {
        ai->TellPlayerNoFacing(requester, "Trade timed out.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        SetResult(FriendIntent::FollowOrIdle, "trade timed out", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    if (now < nextPendingTradeAt)
    {
        SetResult(FriendIntent::FollowOrIdle, "trade pending", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    Item* item = FindBestTradeItem(pendingTradeFragment);
    if (!item || !item->GetProto())
    {
        ai->TellPlayerNoFacing(requester, "I don't have that tradable item anymore.",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        SetResult(FriendIntent::FollowOrIdle, "trade blocked:no item", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    std::string itemName = item->GetProto()->Name1 ? item->GetProto()->Name1 : "item";
    if (bot->GetTrader() && bot->GetTrader() != requester)
    {
        ai->TellPlayerNoFacing(requester, "I'm already trading with someone else.",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        SetResult(FriendIntent::FollowOrIdle, "trade blocked:busy", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    if (requester->GetTrader() && requester->GetTrader() != bot)
    {
        ai->TellPlayerNoFacing(requester, "You're already trading with someone else.",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        SetResult(FriendIntent::FollowOrIdle, "trade blocked:requester busy", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    if (bot->GetMapId() != requester->GetMapId())
    {
        ai->TellPlayerNoFacing(requester, "We're not in the same place anymore.",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        SetResult(FriendIntent::FollowOrIdle, "trade blocked:wrong map", FriendExecutionResult::BlockedNotPossible);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    const float distance = sServerFacade.GetDistance2d(bot, requester);
    if (distance > FRIEND_TRADE_MAX_APPROACH_DISTANCE)
    {
        ai->TellPlayerNoFacing(requester, "You're too far away to trade.",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
        SetResult(FriendIntent::FollowOrIdle, "trade blocked:too far", FriendExecutionResult::BlockedNotPossible);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        ClearPendingTrade();
        return true;
    }

    const float desiredDistance = std::max(sPlayerbotAIConfig.contactDistance + 1.0f, INTERACTION_DISTANCE * 0.5f);
    if (distance > desiredDistance || !bot->IsWithinLOSInMap(requester, true))
    {
        lastIntent = FriendIntent::FollowOrIdle;
        if (MoveToUnitRange(requester, desiredDistance, "move for trade"))
            return true;

        SetResult(FriendIntent::FollowOrIdle, "move for trade failed", FriendExecutionResult::Failed);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        nextPendingTradeAt = now + 2;
        return true;
    }

    ClearFriendMovement(true);
    if (!bot->GetTrader())
    {
        WorldPacket packet(CMSG_INITIATE_TRADE);
        packet << requester->GetObjectGuid();
        bot->GetSession()->HandleInitiateTradeOpcode(packet);
        SetResult(FriendIntent::FollowOrIdle, "open trade:" + itemName, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        nextPendingTradeAt = now + 1;
        return true;
    }

    TradeData* trade = bot->GetTradeData();
    if (!trade)
    {
        SetResult(FriendIntent::FollowOrIdle, "trade waiting", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        nextPendingTradeAt = now + 1;
        return true;
    }

    bool alreadyInTrade = item->IsInTrade();
    for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT && !alreadyInTrade; ++slot)
        alreadyInTrade = trade->GetItem(TradeSlots(slot)) == item;

    if (!alreadyInTrade && !PutItemInTrade(item))
    {
        SetResult(FriendIntent::FollowOrIdle, "trade item failed:" + itemName, FriendExecutionResult::Failed);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        nextPendingTradeAt = now + 2;
        return true;
    }

    ai->TellPlayerNoFacing(requester, "Trading " + ChatHelper::formatItem(item) + ".",
        PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    SetResult(FriendIntent::FollowOrIdle, "trade item:" + itemName, FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
    ClearPendingTrade();
    return true;
}

bool FriendBotController::PutItemInTrade(Item* item)
{
    if (!ai || !ai->GetBot() || !item || !item->CanBeTraded())
        return false;

    TradeData* trade = ai->GetBot()->GetTradeData();
    if (!trade)
        return false;

    for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
    {
        if (trade->GetItem(TradeSlots(slot)) == item)
            return true;
    }

    int8 tradeSlot = -1;
    for (uint8 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
    {
        if (!trade->GetItem(TradeSlots(slot)))
        {
            tradeSlot = slot;
            break;
        }
    }

    if (tradeSlot < 0)
        return false;

    WorldPacket packet(CMSG_SET_TRADE_ITEM, 3);
    packet << static_cast<uint8>(tradeSlot) << static_cast<uint8>(item->GetBagSlot())
        << static_cast<uint8>(item->GetSlot());
    ai->GetBot()->GetSession()->HandleSetTradeItemOpcode(packet);
    return true;
}

void FriendBotController::ClearPendingTrade()
{
    pendingTradeRequesterGuid = ObjectGuid();
    pendingTradeFragment.clear();
    pendingTradeUntil = 0;
    nextPendingTradeAt = 0;
}

void FriendBotController::PrintHelp(Player* requester) const
{
    if (!ai || !requester)
        return;

    ai->TellPlayerNoFacing(requester, "modes: party, dungeon, solo, normal/reset, strict/friend off",
        PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    ai->TellPlayerNoFacing(requester, "commands: stop, come, stay close, attack, heal, buff, rest, shop/town, summon",
        PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    ai->TellPlayerNoFacing(requester, "progress: ok, no, forcelevel N, forcelevelsync, forcegearsync, forcegearempty",
        PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    ai->TellPlayerNoFacing(requester, "debug: report, version, intent/verbose, debug, weights, silent, items [filter], equip [slot], trade <item>, forceitemclear, forceitemjunk, help",
        PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
}

void FriendBotController::ResetTemporaryCommandIfSatisfied(const FriendSituation& situation)
{
    if (command == FriendCommand::ReturnToParty && situation.leaderSafe &&
        situation.leaderDistance <= std::max(ai->GetRange("follow") * 0.75f, 6.0f))
    {
        command = FriendCommand::None;
        executionTask = FriendTaskType::None;
        executionTaskUntil = 0;
        executionNextActionAt = 0;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
    }

    if (command == FriendCommand::Recover &&
        !situation.inCombat &&
        situation.botHealth >= FRIEND_REST_DONE_HEALTH &&
        situation.botMana >= FRIEND_REST_DONE_MANA)
    {
        command = FriendCommand::None;
        taskTravelRequested = false;
        taskTravelPurpose = 0;
    }

    if (command == FriendCommand::Shop && mode == FriendMode::Dungeon)
    {
        command = FriendCommand::None;
        ClearExecutionState();
    }
}

void FriendBotController::ClearExecutionState()
{
    executionTask = FriendTaskType::None;
    executionTaskUntil = 0;
    executionNextActionAt = 0;
    taskTravelRequested = false;
    taskTravelPurpose = 0;
}

std::vector<std::string> FriendBotController::PositionActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;

    if (situation.healerish && situation.hasAttackers)
        actions.push_back("fade");

    if (situation.ranged || situation.healerish)
    {
        actions.push_back("move out of enemy contact");
    }

    if (!situation.ranged)
        actions.push_back("set behind");

    actions.push_back("set facing");
    return actions;
}

std::vector<std::string> FriendBotController::SelfPreservationActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
            AddActions(actions, { "shield wall", "last stand", "challenging shout" });
            if (mode != FriendMode::Dungeon)
                actions.push_back("intimidating shout");
            break;
        case CLASS_PALADIN:
            AddActions(actions, { "divine shield", "divine protection", "lay on hands", "flash of light", "holy light" });
            break;
        case CLASS_HUNTER:
            AddActions(actions, { "feign death", "deterrence", "scatter shot", "wing clip", "frost trap in place" });
            break;
        case CLASS_ROGUE:
            AddActions(actions, { "evasion", "cloak of shadows", "vanish", "blind", "gouge" });
            break;
        case CLASS_PRIEST:
            AddActions(actions, { "fade", "desperate prayer", "power word: shield", "renew", "flash heal" });
            break;
        case CLASS_SHAMAN:
            AddActions(actions, { "stoneclaw totem", "lesser healing wave", "healing wave" });
            break;
        case CLASS_MAGE:
        {
            const int32 danger = SelfThreatDangerScore(situation);
            if (danger >= 95)
                AddActions(actions, { "ice block", "blink", "ice barrier", "mana shield", "frost nova" });
            else if (danger >= 55)
                AddActions(actions, { "ice barrier", "mana shield", "frost nova" });
            else
                AddActions(actions, { "ice barrier", "mana shield" });
            break;
        }
        case CLASS_WARLOCK:
            AddActions(actions, { "death coil", "sacrifice", "soulshatter", "drain life" });
            if (mode != FriendMode::Dungeon)
                actions.push_back("fear");
            break;
        case CLASS_DRUID:
            AddActions(actions, { "barkskin", "survival instincts", "frenzied regeneration", "regrowth", "rejuvenation", "nature's grasp" });
            break;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            AddActions(actions, { "icebound fortitude", "anti magic shell", "rune tap", "death pact", "vampiric blood", "death strike" });
            break;
#endif
        default:
            break;
    }

    if (situation.inCombat && situation.botMana < sPlayerbotAIConfig.mediumMana)
        AddActions(actions, { "mana gem", "mana potion", "dark rune", "life tap", "dark pact" });

    AddActions(actions, { "healthstone", "healing potion", "whipper root tuber", "use bandage" });
    if (!situation.inCombat)
        AddActions(actions, { "food", "sit" });

    return actions;
}

std::vector<std::string> FriendBotController::HealActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;

    if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealthDelta <= FRIEND_HEALTH_DROP_DANGER)
    {
        AddActions(actions, {
            "lay on hands on party", "pain suppression on party", "power word: shield on party",
            "flash heal on party", "lesser healing wave on party", "flash of light on party",
            "riptide on party", "regrowth on party", "rejuvenation on party"
        });
    }

    AddActions(actions, {
        "prayer of mending", "circle of healing", "chain heal", "holy shock on party",
        "greater heal on party", "heal on party", "lesser heal on party",
        "healing wave on party", "lesser healing wave on party",
        "holy light on party", "flash of light on party",
        "regrowth on party", "rejuvenation on party", "healing touch on party"
    });

    if (situation.damagedPartyMembers > 1)
        AddActions(actions, { "prayer of healing", "tranquility", "healing stream totem" });

    return actions;
}

std::vector<std::string> FriendBotController::BuffOrCureActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    AddActions(actions, {
        "dispel magic on party", "cure disease on party", "abolish disease on party",
        "cleanse poison on party", "cleanse disease on party", "cleanse magic on party",
        "purify poison on party", "purify disease on party",
        "cure poison on party", "abolish poison on party", "remove curse on party",
        "cleanse spirit poison on party", "cleanse spirit disease on party", "cleanse spirit curse on party"
    });

    if (!situation.inCombat)
    {
        AddActions(actions, {
            "arcane brilliance on party", "arcane intellect on party",
            "prayer of fortitude on party", "power word: fortitude on party",
            "prayer of spirit on party", "divine spirit on party",
            "pve greater blessing on party", "pve blessing on party",
            "mark of the wild on party", "gift of the wild on party", "thorns on party",
            "paladin aura", "trueshot aura", "horn of winter"
        });

        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot || !ai->HasAnyAuraOf(bot, "water shield", "lightning shield", NULL))
            AddActions(actions, { "water shield", "lightning shield" });

        if (!bot || !ai->HasAnyAuraOf(bot, "demon armor", "demon skin", "fel armor", NULL))
            AddActions(actions, { "demon armor", "demon skin", "fel armor" });

        if (!bot || !ai->HasAnyAuraOf(bot, "inner fire", NULL))
            actions.push_back("inner fire");

        if (!bot || !ai->HasAnyAuraOf(bot, "mage armor", "ice armor", "frost armor", "molten armor", NULL))
            AddActions(actions, { "mage armor", "ice armor", "molten armor" });

        if (!bot || !ai->HasAnyAuraOf(bot, "aspect of the hawk", "aspect of the viper", NULL))
            AddActions(actions, { "aspect of the hawk", "aspect of the viper" });
    }

    return actions;
}

std::vector<std::string> FriendBotController::CrowdControlActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    if (mode != FriendMode::Dungeon && situation.possibleTargetsCount < 3)
        return actions;

    AddActions(actions, {
        "polymorph", "freezing trap on cc", "entangling roots on cc", "hibernate on cc",
        "banish on cc", "shackle undead", "repentance", "blind", "sap",
        "frost nova", "earthbind totem", "hammer of justice", "bash", "scatter shot"
    });

    if (mode != FriendMode::Dungeon)
        AddActions(actions, { "fear on cc", "psychic scream", "intimidating shout" });

    return actions;
}

std::vector<std::string> FriendBotController::PullActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    if (mode == FriendMode::Dungeon || situation.inCombat || situation.partyInCombat || situation.damagedPartyMembers ||
        !situation.leaderSafe || situation.leaderDistance > sPlayerbotAIConfig.reactDistance ||
        situation.nearbyPartyMembers < 2 || situation.possibleTargetsCount == 0 || situation.possibleTargetsCount > 2)
        return actions;

    actions.push_back("attack anything");

    return actions;
}

std::vector<std::string> FriendBotController::DamageActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    const bool partyPeel = CanProtectPartyWithThreat(situation) &&
        (situation.vulnerablePartyHasThreat || situation.healerPartyHasThreat);
    if (situation.tankish || partyPeel)
        AddActions(actions, { "taunt", "hand of reckoning", "righteous defense", "growl", "dark command" });

    const bool preferFreeDamage = PreferFreeDamage(situation);
    if (preferFreeDamage)
        AddActions(actions, { "shoot", "melee", "attack" });

    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
            if (partyPeel)
                AddActions(actions, { "challenging shout", "battle shout taunt", "sunder armor", "revenge", "thunder clap" });
            AddActions(actions, {
                "pummel", "shield bash", "charge", "intercept", "bloodrage", "battle shout",
                "demoralizing shout"
            });
            break;
        case CLASS_PALADIN:
            AddActions(actions, {
                "hammer of justice", "holy shield", "seal of command", "seal of righteousness",
                "seal of wisdom", "judgement", "judgement of light", "judgement of wisdom", "consecration"
            });
            break;
        case CLASS_HUNTER:
            AddActions(actions, {
                "kill command", "hunter's mark", "auto shot", "mend pet"
            });
            break;
        case CLASS_ROGUE:
            AddActions(actions, {
                "kick", "slice and dice", "cheap shot", "kidney shot", "blade flurry", "adrenaline rush"
            });
            break;
        case CLASS_PRIEST:
            AddActions(actions, {
                "silence", "shadowfiend", "vampiric embrace"
            });
            break;
        case CLASS_SHAMAN:
            AddActions(actions, {
                "wind shear", "searing totem", "fire nova", "heroism", "bloodlust"
            });
            break;
        case CLASS_MAGE:
            AddActions(actions, {
                "counterspell", "icy veins", "combustion", "mirror image", "summon water elemental"
            });
            break;
        case CLASS_WARLOCK:
            AddActions(actions, {
                "spell lock", "curse of weakness", "death coil"
            });
            break;
        case CLASS_DRUID:
            AddActions(actions, {
                "faerie fire", "faerie fire (feral)", "demoralizing roar"
            });
            break;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            AddActions(actions, {
                "mind freeze", "death grip", "horn of winter"
            });
            break;
#endif
        default:
            break;
    }

    if (situation.targetIsElite || situation.balance < 90)
        AddActions(actions, {
            "recklessness", "retaliation", "avenging wrath", "rapid fire", "bestial wrath",
            "arcane power", "presence of mind", "summon water elemental", "shadowfiend",
            "summon inferno", "summon felguard", "starfall", "berserk", "bloodlust", "heroism",
            "army of the dead", "summon gargoyle"
        });

    if (!preferFreeDamage)
        AddActions(actions, { "shoot", "melee", "attack" });
    return actions;
}

std::string FriendBotController::CommandName(FriendCommand value)
{
    switch (value)
    {
        case FriendCommand::None: return "none";
        case FriendCommand::StayClose: return "close";
        case FriendCommand::ReturnToParty: return "return";
        case FriendCommand::HoldPosition: return "hold";
        case FriendCommand::Recover: return "recover";
        case FriendCommand::Shop: return "shop";
    }

    return "unknown";
}

std::string FriendBotController::ModeName(FriendMode value)
{
    switch (value)
    {
        case FriendMode::Party: return "party";
        case FriendMode::Dungeon: return "dungeon";
        case FriendMode::Solo: return "solo";
    }

    return "unknown";
}

std::string FriendBotController::TaskName(FriendTaskType value)
{
    switch (value)
    {
        case FriendTaskType::None: return "none";
        case FriendTaskType::HangOut: return "hang out";
        case FriendTaskType::OrbitLeader: return "orbit";
        case FriendTaskType::Resupply: return "resupply";
        case FriendTaskType::GatherNearby: return "gather";
        case FriendTaskType::GrindNearby: return "grind";
        case FriendTaskType::TravelToGrind: return "travel grind";
        case FriendTaskType::TravelToGather: return "travel gather";
        case FriendTaskType::ExploreNearby: return "explore";
    }

    return "unknown";
}

std::string FriendBotController::ProposalName(FriendProposal value)
{
    switch (value)
    {
        case FriendProposal::None: return "none";
        case FriendProposal::Resupply: return "resupply";
    }

    return "unknown";
}

std::string FriendBotController::VerbosityName(FriendVerbosity value)
{
    switch (value)
    {
        case FriendVerbosity::Silent: return "silent";
        case FriendVerbosity::Intent: return "intent";
        case FriendVerbosity::Debug: return "debug";
        case FriendVerbosity::Weights: return "weights";
    }

    return "unknown";
}

std::string FriendBotController::IntentName(FriendIntent value)
{
    switch (value)
    {
        case FriendIntent::FollowOrIdle: return "follow/idle";
        case FriendIntent::ReturnToParty: return "return";
        case FriendIntent::HoldPosition: return "hold";
        case FriendIntent::ImprovePosition: return "position";
        case FriendIntent::RecoverResources: return "recover";
        case FriendIntent::Resupply: return "resupply";
        case FriendIntent::SaveSelf: return "save self";
        case FriendIntent::SavePartyMember: return "heal";
        case FriendIntent::BuffOrCureParty: return "support";
        case FriendIntent::CrowdControl: return "cc";
        case FriendIntent::PullWithParty: return "pull";
        case FriendIntent::LootNearby: return "loot";
        case FriendIntent::Gather: return "gather";
        case FriendIntent::Grind: return "grind";
        case FriendIntent::Explore: return "explore";
        case FriendIntent::HangOut: return "hang out";
        case FriendIntent::DealDamage: return "damage";
        case FriendIntent::Max: return "max";
    }

    return "unknown";
}

std::string FriendBotController::ResultName(FriendExecutionResult value)
{
    switch (value)
    {
        case FriendExecutionResult::None: return "none";
        case FriendExecutionResult::Done: return "done";
        case FriendExecutionResult::IntentionalIdle: return "idle";
        case FriendExecutionResult::BlockedNoAction: return "no action";
        case FriendExecutionResult::BlockedNotUseful: return "not useful";
        case FriendExecutionResult::BlockedNotPossible: return "not possible";
        case FriendExecutionResult::Failed: return "failed";
    }

    return "unknown";
}

std::string FriendBotController::CombatStyleName(FriendCombatStyle value)
{
    switch (value)
    {
        case FriendCombatStyle::Burn: return "burn";
        case FriendCombatStyle::Normal: return "normal";
        case FriendCombatStyle::Conserve: return "conserve";
        case FriendCombatStyle::Dry: return "dry";
    }

    return "unknown";
}

std::string FriendBotController::BalanceName(uint8 balance)
{
    if (balance < 70)
        return "hard";
    if (balance < 100)
        return "pressured";
    if (balance < 140)
        return "steady";
    return "safe";
}
