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
#include "strategy/values/BudgetValues.h"
#include "strategy/values/PossibleAttackTargetsValue.h"
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
    const uint32 FRIEND_EXPLORE_TRAVEL_PURPOSE = static_cast<uint32>(TravelDestinationPurpose::Explore);
    const uint8 FRIEND_RTI_MOON = 4;
    const uint8 FRIEND_RTI_SKULL = 7;
    const float FRIEND_CC_MELEE_ENGAGED_DISTANCE = 5.0f;
    const float FRIEND_RANGED_SPACING_MIN = 7.0f;
    const float FRIEND_RANGED_SPACING_WORLD = 13.0f;
    const float FRIEND_RANGED_SPACING_DUNGEON = 10.0f;
    const float FRIEND_RANGED_SPACING_HOSTILE_BUFFER = 14.0f;
    const float FRIEND_RANGED_SPACING_PARTY_BUFFER = 3.0f;
    const uint32 FRIEND_SOFT_LEVEL_CATCHUP_COOLDOWN = 5 * MINUTE;
    const uint32 FRIEND_SOFT_TRAINING_COOLDOWN = 3 * MINUTE;
    const uint32 FRIEND_SOFT_BAG_UPGRADE_COOLDOWN = 5 * MINUTE;
    const uint32 FRIEND_SOFT_GEAR_UPGRADE_COOLDOWN = 10 * MINUTE;
    const uint32 FRIEND_PROPOSAL_COOLDOWN = 5 * MINUTE;
    const uint32 FRIEND_PROPOSAL_REJECT_COOLDOWN = 10 * MINUTE;

    void AddActions(std::vector<std::string>& actions, std::initializer_list<const char*> names)
    {
        for (const char* name : names)
            actions.push_back(name);
    }

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
        return ai && ai->GetBot() && ai->GetBot()->GetMotionMaster() &&
            lastAction == action &&
            ai->GetBot()->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;
    }

    bool StartsWith(const std::string& value, const std::string& prefix)
    {
        return value.length() >= prefix.length() && value.compare(0, prefix.length(), prefix) == 0;
    }

    std::string Trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());

        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();

        return value;
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
    lastTargetReason.clear();
    pendingProposal = FriendProposal::None;
    manualAttackUntil = 0;
    manualHealUntil = 0;
    manualBuffUntil = 0;
    manualHealGuid = ObjectGuid();
    idleGoal = FriendIdleGoal::None;
    idleGoalUntil = 0;
    idleNextActionAt = 0;
    resupplyTravelRequested = false;
    idleTravelRequested = false;
    idleTravelPurpose = 0;
    proposalExpiresAt = 0;
    nextProposalAt = 0;
    nextSoftLevelCatchupAt = 0;
    nextSoftTrainingAt = 0;
    nextSoftBagUpgradeAt = 0;
    nextSoftGearUpgradeAt = 0;
    lastPlanningBusyAt = time(nullptr);
    lastActivityBark.clear();
    nextActivityBarkAt = 0;
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

    FriendIntent intent = SelectIntent(situation);
    lastTargetReason.clear();

    if (!ExecuteIntent(intent, situation))
    {
        SetResult(intent, "", FriendExecutionResult::IntentionalIdle);
        ai->SetActionDuration(minimal ? sPlayerbotAIConfig.reactDelay : sPlayerbotAIConfig.globalCoolDown);
    }

    MaybeSayStatus(situation);
    MaybeProposeTownChores(situation);
}

bool FriendBotController::HandleCommand(const std::string& rawCommand, Player* requester, std::string& response)
{
    std::string cmd = rawCommand;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto clearTemporaryState = [&]()
    {
        this->command = FriendCommand::None;
        manualAttackUntil = 0;
        manualHealUntil = 0;
        manualBuffUntil = 0;
        manualHealGuid = ObjectGuid();
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        ClearProposal();
    };

    if (cmd == "ok" || cmd == "yes")
    {
        if (pendingProposal == FriendProposal::Resupply && proposalExpiresAt >= time(nullptr))
        {
            command = FriendCommand::Shop;
            idleGoal = FriendIdleGoal::Resupply;
            idleGoalUntil = time(nullptr) + 180;
            idleNextActionAt = 0;
            resupplyTravelRequested = false;
            idleTravelRequested = false;
            idleTravelPurpose = 0;
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

    if (StartsWith(cmd, "forcelevel ") || StartsWith(cmd, "force level "))
    {
        std::string levelText = StartsWith(cmd, "forcelevel ") ? Trim(cmd.substr(11)) : Trim(cmd.substr(12));
        uint32 level = static_cast<uint32>(std::atoi(levelText.c_str()));
        return ForceLevel(level, requester, response);
    }

    if (cmd == "party" || cmd == "normal" || cmd == "reset" || cmd == "act normal")
    {
        mode = FriendMode::Party;
        verbosity = FriendVerbosity::Silent;
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
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        if (ai)
            ai->StopMoving();
        response = "Holding position.";
        return true;
    }

    if (cmd == "come" || cmd == "come here" || cmd == "return")
    {
        command = FriendCommand::ReturnToParty;
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        response = "Coming back.";
        return true;
    }

    if (cmd == "stay close" || cmd == "close")
    {
        command = FriendCommand::StayClose;
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        response = "Staying close.";
        return true;
    }

    if (cmd == "recover" || cmd == "drink" || cmd == "rest" || cmd == "rest up")
    {
        command = FriendCommand::Recover;
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        response = "Resting up.";
        return true;
    }

    if (cmd == "shop" || cmd == "town" || cmd == "resupply" || cmd == "vendor")
    {
        command = FriendCommand::Shop;
        idleGoal = FriendIdleGoal::Resupply;
        idleGoalUntil = time(nullptr) + 180;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
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
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
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
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        response = "Healing.";
        return true;
    }

    if (cmd == "buff" || cmd == "buff me")
    {
        command = FriendCommand::None;
        manualBuffUntil = time(nullptr) + 30;
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
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
    out << "friend: mode=" << ModeName(mode);
    out << ", command=" << CommandName(command);
    out << ", idle=" << IdleGoalName(idleGoal);
    out << ", proposal=" << ProposalName(pendingProposal);
    out << ", verbosity=" << VerbosityName(verbosity);
    out << ", intent=" << IntentName(lastIntent);
    out << ", action=" << (lastAction.empty() ? "none" : lastAction);
    out << ", result=" << ResultName(lastResult);
    out << ", style=" << CombatStyleName(GetCombatStyle(lastSituation));
    out << ", target=" << (lastTargetReason.empty() ? "none" : lastTargetReason);
    out << ", hp=" << static_cast<uint32>(lastSituation.botHealth) << "%";
    out << ", mana=" << static_cast<uint32>(lastSituation.botMana) << "%";
    out << ", level=" << static_cast<uint32>(lastSituation.botLevel);
    if (lastSituation.leaderLevel)
        out << "/" << static_cast<uint32>(lastSituation.leaderLevel);
    out << ", bag=" << static_cast<uint32>(lastSituation.bagSpace) << "%";
    out << ", dur=" << static_cast<uint32>(lastSituation.durability) << "%";
    out << ", calm=" << lastSituation.calmDowntimeSeconds << "s";
    out << ", partyHp=" << static_cast<uint32>(lastSituation.lowestPartyHealth) << "%";
    out << ", balance=" << BalanceName(lastSituation.balance);
    if (lastSituation.botHasThreat)
        out << ", threat=self";
    else if (lastSituation.healerPartyHasThreat)
        out << ", threat=healer";
    else if (lastSituation.vulnerablePartyHasThreat)
        out << ", threat=party";
    out << ", leaderDist=" << static_cast<uint32>(lastSituation.leaderDistance);
    out << ", targets=" << static_cast<uint32>(lastSituation.possibleTargetsCount);
    if (lastSituation.travelTargetActive || lastSituation.travelTargetPreparing)
        out << ", travel=" << (lastSituation.travelTargetWorking ? "work" : (lastSituation.travelTargetTraveling ? "move" : "prep"));
    if (lastSituation.nearestHostileGuid)
        out << ", nearHostile=" << static_cast<uint32>(lastSituation.nearestHostileDistance);
    out << ", abilities=" << static_cast<uint32>(abilityCatalog.GetAbilities().size());
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
    situation.inDungeon = bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid());
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
        TravelTarget* travelTarget = context->GetValue<TravelTarget*>("travel target")->Get();
        if (travelTarget)
        {
            const TravelStatus travelStatus = travelTarget->GetStatus();
            situation.travelTargetActive = travelTarget->IsActive();
            situation.travelTargetPreparing = travelStatus == TravelStatus::TRAVEL_STATUS_PREPARE;
            situation.travelTargetTraveling = travelStatus == TravelStatus::TRAVEL_STATUS_READY ||
                travelStatus == TravelStatus::TRAVEL_STATUS_TRAVEL;
            situation.travelTargetWorking = travelStatus == TravelStatus::TRAVEL_STATUS_WORK;
            if (travelTarget->GetDestination())
                situation.travelTargetPurpose = static_cast<uint32>(travelTarget->GetDestination()->GetPurpose());
        }

        uint32 vendorItems = context->GetValue<uint32>("item count", "vendor")->Get();
        uint32 trashItems = context->GetValue<uint32>("item count", "gray")->Get();
        uint32 minRepairCost = context->GetValue<uint32>("min repair cost")->Get();
        uint32 repairMoney = context->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::repair))->Get();
        situation.trainCost = context->GetValue<uint32>("train cost", TRAINER_TYPE_CLASS)->Get();
        situation.gearBudget = context->GetValue<uint32>("free money for", static_cast<uint32>(NeedMoneyFor::gear))->Get();
        situation.shouldSell = situation.bagSpace > 80 || vendorItems > 0 || trashItems > 0;
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

        Unit* target = GetContextValue<Unit*>(context, "current target", nullptr);
        if (!target)
            target = GetContextValue<Unit*>(context, "dps target", nullptr);
        if (target && target->IsInWorld() && target->GetMapId() == bot->GetMapId() && !sServerFacade.UnitIsDead(target))
        {
            situation.hasTarget = true;
            situation.targetDistance = sServerFacade.GetDistance2d(bot, target);
            situation.targetIsElite = IsEliteTarget(ai, target);
        }

        Unit* rtiTarget = GetContextValue<Unit*>(context, "rti target", nullptr);
        if (IsHostileTarget(ai, rtiTarget))
            situation.rtiTargetGuid = rtiTarget->GetObjectGuid();

        Unit* rtiCcTarget = GetContextValue<Unit*>(context, "rti cc target", nullptr);
        if (IsHostileTarget(ai, rtiCcTarget))
            situation.rtiCcTargetGuid = rtiCcTarget->GetObjectGuid();

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
                continue;
            }

            Player* playerVictim = dynamic_cast<Player*>(victim);
            if (playerVictim && ai->IsHeal(playerVictim))
            {
                situation.healerPartyHasThreat = true;
                if (!situation.vulnerablePartyAttackerGuid)
                    situation.vulnerablePartyAttackerGuid = unit->GetObjectGuid();
            }

            if (HealthPercent(ai, victim) < sPlayerbotAIConfig.mediumHealth)
            {
                situation.vulnerablePartyHasThreat = true;
                if (!situation.vulnerablePartyAttackerGuid ||
                    HealthPercent(ai, unit) < HealthPercent(ai, ai->GetUnit(situation.vulnerablePartyAttackerGuid)))
                    situation.vulnerablePartyAttackerGuid = unit->GetObjectGuid();
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
            if (bot->GetNPCIfCanInteractWith(*itr, UNIT_NPC_FLAG_VENDOR))
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

    const time_t now = time(nullptr);
    const bool busyPlanningState = situation.inCombat || situation.partyInCombat || situation.hasAttackers ||
        situation.travelTargetPreparing || situation.travelTargetTraveling ||
        (situation.travelTargetActive && !situation.travelTargetWorking) ||
        sServerFacade.isMoving(bot);
    if (busyPlanningState || !lastPlanningBusyAt)
        lastPlanningBusyAt = now;

    situation.calmDowntimeSeconds = static_cast<uint32>(std::max<time_t>(0, now - lastPlanningBusyAt));
    return situation;
}

FriendIntent FriendBotController::SelectIntent(const FriendSituation& situation) const
{
    const time_t now = time(nullptr);

    if (command == FriendCommand::HoldPosition)
        return FriendIntent::HoldPosition;

    if (command == FriendCommand::ReturnToParty)
        return FriendIntent::ReturnToParty;

    if (command == FriendCommand::Recover && !situation.inCombat)
        return FriendIntent::RecoverResources;

    if (command == FriendCommand::Shop && IsSafeForTownChores(situation))
        return FriendIntent::Resupply;

    const bool soloFree = mode == FriendMode::Solo && !situation.inDungeon && command == FriendCommand::None;
    const bool partyNearby = !soloFree || !situation.leaderSafe || situation.leaderDistance <= SoftLeashDistance(situation);
    const bool localPartyInCombat = situation.partyInCombat && partyNearby;

    float softLeash = SoftLeashDistance(situation);
    float hardLeash = HardLeashDistance(situation);
    if (!soloFree && situation.leaderSafe && situation.leaderDistance > hardLeash &&
        (!situation.inCombat || situation.inDungeon || !situation.hasAttackers))
        return FriendIntent::ReturnToParty;

    if (situation.botHealth < sPlayerbotAIConfig.lowHealth ||
        situation.botHealthDelta <= -12 ||
        (situation.botHealth < sPlayerbotAIConfig.mediumHealth && situation.botHealthDelta < -5))
        return FriendIntent::SaveSelf;

    if (manualHealUntil > now)
        return FriendIntent::SavePartyMember;

    if (partyNearby && (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealthDelta <= -12 ||
        (situation.lowestPartyHealth < sPlayerbotAIConfig.mediumHealth && situation.damagedPartyMembers)))
        return FriendIntent::SavePartyMember;

    if (partyNearby && situation.healerish && situation.damagedPartyMembers > 0 &&
        situation.lowestPartyHealth < sPlayerbotAIConfig.almostFullHealth &&
        (situation.inCombat || situation.partyInCombat))
        return FriendIntent::SavePartyMember;

    if (situation.inCombat && PrefersSelfDefenseTarget(situation) && situation.attackersTargetingMeCount > 0)
        return FriendIntent::CrowdControl;

    if (situation.inCombat && (((situation.ranged || situation.healerish) && situation.hasAttackers) ||
        (situation.ranged && situation.targetDistance > 0.0f && situation.targetDistance < 8.0f)))
        return FriendIntent::ImprovePosition;

    if (!situation.inCombat && situation.botMana < sPlayerbotAIConfig.lowMana &&
        (!situation.leaderInCombat || !partyNearby))
        return FriendIntent::RecoverResources;

    if (manualBuffUntil > now && !situation.inCombat && !localPartyInCombat)
        return FriendIntent::BuffOrCureParty;

    if (situation.inCombat && situation.inDungeon && situation.possibleTargetsCount > 1 && !situation.tankish)
        return FriendIntent::CrowdControl;

    if (!situation.inCombat && !localPartyInCombat && (situation.damagedPartyMembers == 0 || !partyNearby) &&
        NeedsTownChores(situation) && IsSafeForTownChores(situation) &&
        (mode == FriendMode::Solo || situation.inTown || situation.nearbyVendor || situation.nearbyRepair))
        return FriendIntent::Resupply;

    if (!situation.inCombat && !localPartyInCombat && (situation.damagedPartyMembers == 0 || !partyNearby))
        return FriendIntent::BuffOrCureParty;

    if (!situation.inCombat && !localPartyInCombat && situation.hasCreatureLoot &&
        situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation))
        return FriendIntent::LootNearby;

    if (mode != FriendMode::Dungeon && !situation.inDungeon && !situation.inCombat && !localPartyInCombat &&
        situation.leaderSafe && situation.leaderDistance <= sPlayerbotAIConfig.reactDistance &&
        situation.nearbyPartyMembers >= 2 && situation.possibleTargetsCount > 0 && situation.possibleTargetsCount <= 2 &&
        situation.botHealth >= sPlayerbotAIConfig.mediumHealth && situation.botMana >= sPlayerbotAIConfig.mediumMana)
        return FriendIntent::PullWithParty;

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
    const bool keepFriendMovement = intent == FriendIntent::ReturnToParty ||
        intent == FriendIntent::HoldPosition ||
        (intent == FriendIntent::FollowOrIdle && command == FriendCommand::StayClose);
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
            return TryActions(DamageActions(situation), "friend damage");

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
            if (TryActions({ "mana gem", "mana potion", "dark rune", "life tap", "dark pact" }, "friend combat recover"))
                return true;
            if (ShouldConserveDamageMana(situation) && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend damage");

        case FriendIntent::Resupply:
            return ExecuteResupply(situation);

        case FriendIntent::SaveSelf:
            if (TryActions(SelfPreservationActions(situation), "friend self"))
            {
                if (situation.botHealth < sPlayerbotAIConfig.lowHealth || situation.botHealthDelta <= -12)
                    MaybeSayActivity(situation, "self-trouble", {
                        "I'm in trouble.",
                        "Need a second here."
                    }, 40, 60);
                return true;
            }
            if (!situation.hasAttackers && (situation.inCombat || situation.partyInCombat || situation.hasTarget))
                return TryFallbackCombat(situation, "friend self fallback");
            return false;

        case FriendIntent::SavePartyMember:
            if (TryCatalogHeal(situation, "friend heal"))
            {
                if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth || situation.lowestPartyHealthDelta <= -12)
                    MaybeSayActivity(situation, "heal-pressure", {
                        "Hold on, healing.",
                        "I've got heals."
                    }, 30, 45);
                return true;
            }
            if (TryActions(HealActions(situation), "friend heal"))
            {
                if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth || situation.lowestPartyHealthDelta <= -12)
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
            if (ShouldUseLegacySupportActions(situation) && TryActions(BuffOrCureActions(situation), "friend support"))
                return true;
            if (situation.hasCreatureLoot && ExecuteLoot(situation))
                return true;
            if (TryActions(PullActions(situation), "friend pull"))
                return true;
            if (ExecuteIdleGoal(situation))
                return true;
            if (situation.leaderSafe && situation.leaderDistance > PreferredLeaderDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            return false;

        case FriendIntent::CrowdControl:
            if (TryCatalogCrowdControl(situation, "friend cc"))
                return true;
            if (TryActions(CrowdControlActions(situation), "friend cc"))
                return true;
            if (ShouldConserveDamageMana(situation) && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend damage");

        case FriendIntent::PullWithParty:
            if (TryActions(PullActions(situation), "friend pull"))
            {
                MaybeSayActivity(situation, "pull", {
                    "I'll pull one back.",
                    "Pulling one over."
                }, 35, 45);
                return true;
            }
            return false;

        case FriendIntent::LootNearby:
            return ExecuteLoot(situation);

        case FriendIntent::DealDamage:
            if (TryImproveRangedCombatSpacing(situation, "ranged spacing"))
                return true;
            if (GetCombatStyle(situation) == FriendCombatStyle::Dry && TryFreeDamage(situation, "friend free damage"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            if (PrefersMeleeDamage(situation) && MoveToDamageTarget(situation, "move to melee"))
                return true;
            if (TryActions(DamageActions(situation), "friend damage"))
                return true;
            if (situation.partyInCombat && situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            return false;

        case FriendIntent::FollowOrIdle:
            if (command == FriendCommand::StayClose && situation.leaderGuid)
                return MoveInLeaderOrbit(situation, "stay close", false);
            if (ExecuteIdleGoal(situation))
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

    Action* action = ai->GetAiObjectContext()->GetAction(name);
    if (!action)
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNoAction);
        return FriendExecutionResult::BlockedNoAction;
    }

    action->SetReaction(false);
    action->setRelevance(ACTION_NORMAL);
    action->MakeVerbose(false);

    if (!action->isUseful())
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNotUseful);
        return FriendExecutionResult::BlockedNotUseful;
    }

    if (TryPrerequisites(action, source, depth, owner) && !action->isPossible())
        return FriendExecutionResult::Done;

    if (!action->isPossible())
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNotPossible);
        return FriendExecutionResult::BlockedNotPossible;
    }

    Event event(source, "", owner ? owner : ai->GetMaster());
    bool executed = action->Execute(event);
    FriendExecutionResult result = executed ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
    if (executed)
        ai->SetActionDuration(action);

    SetResult(lastIntent, name, result);
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
    if (!ai || !ai->GetBot() || !IsHostileTarget(ai, target))
        return false;

    if (!PossibleAttackTargetsValue::IsValid(target, ai->GetBot(), sPlayerbotAIConfig.sightDistance, true, false))
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

    const bool hostile = IsHostileTarget(ai, target);
    const bool friendly = IsFriendlyTarget(ai, target);
    if (!hostile && !friendly)
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    SpellCastResult checkResult = SPELL_CAST_OK;
    bool canEventuallyCast = ai->CanCastSpell(ability.spellId, target, 0, true, nullptr, true, false, false, &checkResult);
    if (!canEventuallyCast && hostile && ability.Has(FRIEND_ABILITY_DAMAGE) && ability.maxRange <= 0.0f)
        canEventuallyCast = true;

    if (!canEventuallyCast)
        return false;

    if (hostile)
    {
        context->GetValue<Unit*>("current target")->Set(target);
        bot->SetSelectionGuid(target->GetObjectGuid());
    }

    float desiredDistance = sPlayerbotAIConfig.spellDistance;
    if (hostile && (ability.Has(FRIEND_ABILITY_MELEE) || ability.maxRange <= 0.0f))
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
    if (TryDruidCombatForm(situation, source))
        return true;

    if (ai && ai->GetBot() && ai->GetBot()->getClass() == CLASS_WARRIOR && GetDamageTarget(situation, true))
    {
        if (TryActions({ "charge", "intercept" }, source))
            return true;
    }

    if (PrefersMeleeDamage(situation) && MoveToDamageTarget(situation, "move to melee"))
        return true;

    return TryActions({ "shoot", "melee", "attack" }, source);
}

bool FriendBotController::TryFallbackCombat(const FriendSituation& situation, const std::string& source)
{
    if (TryFreeDamage(situation, source))
        return true;

    if (TryCatalogDamage(situation, "friend damage"))
        return true;

    if (TryActions(DamageActions(situation), "friend damage"))
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

    if (situation.healerish && situation.damagedPartyMembers > 0)
        return false;

    if (situation.botMana < sPlayerbotAIConfig.lowMana)
        return false;

    if (situation.tankish || situation.hasAttackers || situation.botHealth < sPlayerbotAIConfig.almostFullHealth)
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
    if (!IsHostileTarget(ai, target) || target->GetVictim() == bot)
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
    const float desiredDistance = situation.inDungeon ? FRIEND_RANGED_SPACING_DUNGEON : FRIEND_RANGED_SPACING_WORLD;

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
    if (!IsHostileTarget(ai, target))
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

    if (IsMovingForAction(ai, lastAction, action))
    {
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

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

    float x = target->GetPositionX() + dx * desiredDistance;
    float y = target->GetPositionY() + dy * desiredDistance;
    float z = target->GetPositionZ();
    const bool normalized = NormalizeFriendMovePosition(x, y, z);

    WorldPosition from(bot);
    WorldPosition to(bot->GetMapId(), x, y, z);
    if (!normalized || !from.canPathTo(to, bot) || !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
    {
        bot->GetMotionMaster()->MoveChase(target, desiredDistance, bot->GetAngle(target));
    }
    else
    {
        if (!MoveFriendPoint(x, y, z))
            return false;
    }

    SetResult(lastIntent, action, FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
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

FriendCombatStyle FriendBotController::GetCombatStyle(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->HasMana())
        return FriendCombatStyle::Normal;

    const bool urgent = situation.botHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        situation.botHealthDelta <= -12 ||
        situation.lowestPartyHealthDelta <= -12 ||
        situation.vulnerablePartyHasThreat ||
        situation.healerPartyHasThreat;
    if (urgent)
        return FriendCombatStyle::Burn;

    const bool hardFight = situation.inDungeon ||
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
    return !situation.inDungeon &&
        !situation.targetIsElite &&
        situation.possibleTargetsCount <= 1 &&
        situation.attackersCount <= 1 &&
        situation.balance >= 90 &&
        situation.botHealth >= sPlayerbotAIConfig.mediumHealth &&
        situation.lowestPartyHealth >= sPlayerbotAIConfig.almostFullHealth &&
        situation.botHealthDelta > -8 &&
        situation.lowestPartyHealthDelta > -8;
}

int32 FriendBotController::ManaSpendScorePenalty(const FriendSituation& situation, const FriendAbility& ability) const
{
    if (!ability.UsesMana() || ability.Has(FRIEND_ABILITY_INTERRUPT))
        return 0;

    switch (GetCombatStyle(situation))
    {
        case FriendCombatStyle::Burn:
            return 0;
        case FriendCombatStyle::Normal:
            return situation.botMana < sPlayerbotAIConfig.mediumMana ? 20 : 0;
        case FriendCombatStyle::Conserve:
            return ability.Has(FRIEND_ABILITY_DOT) ? 25 : 55;
        case FriendCombatStyle::Dry:
            return ability.Has(FRIEND_ABILITY_DOT) ? 60 : 120;
    }

    return 0;
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
    if (duplicateAuraSensitive && ai->HasAura(ability.spellId, target, ability.Has(FRIEND_ABILITY_DOT) || ability.Has(FRIEND_ABILITY_CC)))
        return false;

    if (IsHostileTarget(ai, target) && ability.Has(FRIEND_ABILITY_MELEE))
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

bool FriendBotController::TryCatalogDamage(const FriendSituation& situation, const std::string& source)
{
    if (PreferFreeDamage(situation))
        return false;

    Unit* target = GetDamageTarget(situation, true);
    if (!IsHostileTarget(ai, target))
        return false;

    struct Candidate
    {
        const FriendAbility* ability;
        int32 score;
    };

    std::vector<Candidate> candidates;
    const uint8 targetHealth = HealthPercent(ai, target);
    const bool targetCasting = target->IsNonMeleeSpellCasted(false);

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_DAMAGE) && !ability.Has(FRIEND_ABILITY_INTERRUPT) &&
            !ability.Has(FRIEND_ABILITY_DAMAGE_COOLDOWN) && !ability.Has(FRIEND_ABILITY_THREAT))
            continue;

        if (ability.Has(FRIEND_ABILITY_HEAL) || ability.Has(FRIEND_ABILITY_BUFF_CORE) || ability.Has(FRIEND_ABILITY_BUFF_SITUATIONAL))
            continue;

        if (situation.inDungeon && ability.Has(FRIEND_ABILITY_FEAR))
            continue;

        if (ability.Has(FRIEND_ABILITY_CC) && !ability.Has(FRIEND_ABILITY_DAMAGE) && !ability.Has(FRIEND_ABILITY_INTERRUPT))
            continue;

        if (ability.Has(FRIEND_ABILITY_DOT) && targetHealth < 35)
            continue;

        if (ability.Has(FRIEND_ABILITY_AOE) && situation.inDungeon && situation.possibleTargetsCount < 3)
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
        if (ability.Has(FRIEND_ABILITY_THREAT) && situation.tankish)
            score += 25;
        if (ability.Has(FRIEND_ABILITY_AOE) && situation.possibleTargetsCount > 1)
            score += 12;
        if (ability.Has(FRIEND_ABILITY_DAMAGE_COOLDOWN))
            score += 20;
        score -= ManaSpendScorePenalty(situation, ability);

        if (score > 0)
            candidates.push_back({ &ability, score });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
    {
        return left.score > right.score;
    });

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

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_HEAL) && !ability.Has(FRIEND_ABILITY_SHIELD))
            continue;

        if (ability.Has(FRIEND_ABILITY_DAMAGE) || ability.Has(FRIEND_ABILITY_CURE))
            continue;

        int32 score = 20;
        if (ability.Has(FRIEND_ABILITY_SHIELD))
            score += targetHealth < sPlayerbotAIConfig.mediumHealth || situation.lowestPartyHealthDelta <= -8 ? 35 : 10;
        if (ability.Has(FRIEND_ABILITY_HOT))
            score += targetHealth > sPlayerbotAIConfig.lowHealth ? 35 : 5;
        if (ability.Has(FRIEND_ABILITY_HEAL) && !ability.Has(FRIEND_ABILITY_HOT))
            score += targetHealth < sPlayerbotAIConfig.mediumHealth ? 45 : 8;
        if (targetHealth < sPlayerbotAIConfig.lowHealth)
            score += 40;
        if (situation.lowestPartyHealthDelta <= -8)
            score += 25;
        if (targetHealth > sPlayerbotAIConfig.almostFullHealth && !ability.Has(FRIEND_ABILITY_HOT) && !ability.Has(FRIEND_ABILITY_SHIELD))
            score -= 35;

        if (score > 0)
            candidates.push_back({ &ability, score });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
    {
        return left.score > right.score;
    });

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
        if (!ability.Has(FRIEND_ABILITY_CURE) || !ability.dispelType)
            continue;

        for (Unit* target : party)
        {
            if (ai->HasAuraToDispel(target, ability.dispelType) && TryCastAbility(ability, target, source))
                return true;
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

        for (Unit* target : party)
        {
            if (!ai->HasAura(ability.spellId, target) && TryCastAbility(ability, target, source))
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

        if (situation.inDungeon && ability.Has(FRIEND_ABILITY_FEAR))
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
            score -= situation.inDungeon ? 100 : 10;
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

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
    {
        return left.score > right.score;
    });

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
    if (lootWorldObject && sServerFacade.GetDistance2d(ai->GetBot(), lootWorldObject) <= INTERACTION_DISTANCE)
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

    if (situation.nearbyRepair && situation.shouldRepair &&
        TryAction("repair", "friend resupply") == FriendExecutionResult::Done)
    {
        MaybeSayActivity(situation, "resupply-repair", {
            "Repairing my gear.",
            "Getting repairs done."
        }, command == FriendCommand::Shop ? 70 : 25, 60);
        return true;
    }

    if (situation.nearbyVendor)
    {
        if (situation.shouldSell)
        {
            if (TryActionWithParam("sell", "vendor", "friend resupply") == FriendExecutionResult::Done)
            {
                MaybeSayActivity(situation, "resupply-sell", {
                    "Selling junk while I'm here.",
                    "Clearing out my bags."
                }, command == FriendCommand::Shop ? 70 : 25, 60);
                return true;
            }
            if (TryActionWithParam("sell", "gray", "friend resupply") == FriendExecutionResult::Done)
            {
                MaybeSayActivity(situation, "resupply-sell", {
                    "Selling junk while I'm here.",
                    "Clearing out my bags."
                }, command == FriendCommand::Shop ? 70 : 25, 60);
                return true;
            }
        }

        if (situation.shouldBuy &&
            TryActionWithParam("buy", "vendor", "friend resupply") == FriendExecutionResult::Done)
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

    if (NeedsTownChores(situation) && TryTravelForResupply(situation))
        return true;

    if (command == FriendCommand::Shop)
    {
        const bool blocked = NeedsTownChores(situation);
        if (blocked)
        {
            SetResult(lastIntent, "shop blocked", FriendExecutionResult::BlockedNotUseful);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;
        }

        command = FriendCommand::None;
        ClearIdleState();
        SetResult(lastIntent, "shop done", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    return false;
}

bool FriendBotController::TryTravelForResupply(const FriendSituation& situation)
{
    if (mode == FriendMode::Dungeon || situation.inDungeon)
        return false;

    if (command != FriendCommand::Shop && mode != FriendMode::Solo)
        return false;

    if (situation.travelTargetPreparing && !resupplyTravelRequested)
    {
        SetResult(lastIntent, "shop blocked:travel busy", FriendExecutionResult::BlockedNotUseful);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return false;
    }

    if ((situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetActive) &&
        !resupplyTravelRequested)
    {
        if (TryAction("reset travel target", "friend resupply", 0, ai->GetBot()) == FriendExecutionResult::Done)
            return true;
    }

    if (situation.travelTargetPreparing && resupplyTravelRequested)
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

    if ((situation.travelTargetTraveling || situation.travelTargetActive) && resupplyTravelRequested)
    {
        if (MoveToFriendTravelTarget(situation))
            return true;
        if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) &&
            !ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+travel once", BotState::BOT_STATE_NON_COMBAT);
        return TryAction("move to travel target", "friend resupply", 0, ai->GetBot()) == FriendExecutionResult::Done;
    }

    uint32 purpose = (situation.shouldRepair && !situation.nearbyRepair) ?
        FRIEND_REPAIR_TRAVEL_PURPOSE : FRIEND_VENDOR_TRAVEL_PURPOSE;
    if (TryAction("request travel target::" + std::to_string(purpose), "friend resupply", 0, ai->GetBot()) == FriendExecutionResult::Done)
    {
        resupplyTravelRequested = true;
        MaybeSayActivity(situation, "resupply-travel", {
            "I'm going to find a vendor.",
            "Going to handle supplies."
        }, command == FriendCommand::Shop ? 100 : 50, 60);
        return true;
    }

    return false;
}

bool FriendBotController::MoveToFriendTravelTarget(const FriendSituation& situation)
{
    (void)situation;
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || !ai->CanMove())
        return false;

    Player* bot = ai->GetBot();
    if (!bot->GetMotionMaster())
        return false;

    TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (!target || !target->GetPosition() || !target->GetDestination())
        return false;

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
        target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL && target->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK)
        return false;

    WorldPosition destination = *target->GetPosition();
    if (destination.getMapId() != bot->GetMapId())
        return false;

    WorldPosition from(bot);
    if (from.distance(destination) <= INTERACTION_DISTANCE)
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
    if (!NormalizeFriendMovePosition(x, y, z))
        return false;

    WorldPosition to(bot->GetMapId(), x, y, z);
    if (!from.canPathTo(to, bot) || !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
        return false;

    ClearFriendMovement(false);
    if (!MoveFriendPoint(x, y, z))
        return false;

    SetResult(lastIntent, "move to travel target", FriendExecutionResult::Done);
    ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
    return true;
}

bool FriendBotController::NormalizeFriendMovePosition(float& x, float& y, float& z) const
{
    if (!ai || !ai->GetBot())
        return false;

    Player* bot = ai->GetBot();
    if (!bot->IsFlying() && !bot->IsFreeFlying() && !bot->IsSwimming())
        bot->UpdateAllowedPositionZ(x, y, z);

    return WorldPosition(bot->GetMapId(), x, y, z).isValid();
}

bool FriendBotController::MoveFriendPoint(float x, float y, float z)
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->GetMotionMaster())
        return false;

    Player* bot = ai->GetBot();
    if (!NormalizeFriendMovePosition(x, y, z))
        return false;

    bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN, true);
    return true;
}

bool FriendBotController::IsSafeForTownChores(const FriendSituation& situation) const
{
    if (command == FriendCommand::HoldPosition || command == FriendCommand::ReturnToParty ||
        command == FriendCommand::Recover)
        return false;

    const bool remoteSoloPartyCombat = mode == FriendMode::Solo && !situation.inDungeon &&
        situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation);

    if (situation.inCombat || situation.hasAttackers ||
        (situation.partyInCombat && !remoteSoloPartyCombat))
        return false;

    if (situation.botHealth < sPlayerbotAIConfig.almostFullHealth)
        return false;

    if (!remoteSoloPartyCombat &&
        (situation.lowestPartyHealth < sPlayerbotAIConfig.almostFullHealth ||
         situation.damagedPartyMembers > 0))
        return false;

    if (mode == FriendMode::Dungeon || situation.inDungeon)
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

    return command == FriendCommand::Shop && resupplyTravelRequested &&
        (situation.travelTargetPreparing || situation.travelTargetTraveling ||
         (situation.travelTargetActive && !situation.nearbyVendor && !situation.nearbyRepair));
}

bool FriendBotController::WantsTownProgression(const FriendSituation& situation) const
{
    if (mode == FriendMode::Dungeon || situation.inDungeon)
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
        situation.inDungeon || situation.inCombat || situation.hasAttackers || situation.partyInCombat)
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

bool FriendBotController::ExecuteIdleGoal(const FriendSituation& situation)
{
    if (!ai || !ai->GetBot())
        return false;

    if (!IsSafeForIdleActivity(situation))
    {
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;

        if (situation.leaderSafe && situation.leaderDistance > PreferredLeaderDistance(situation))
            return MoveInLeaderOrbit(situation, "move near leader", false);

        return false;
    }

    FriendIdleGoal selected = SelectIdleGoal(situation);
    if (selected == FriendIdleGoal::None)
        return false;

    return ExecuteCurrentIdleGoal(situation);
}

bool FriendBotController::IsSafeForIdleActivity(const FriendSituation& situation) const
{
    if (command == FriendCommand::HoldPosition || command == FriendCommand::ReturnToParty ||
        command == FriendCommand::Recover)
        return false;

    const bool remoteSoloPartyCombat = mode == FriendMode::Solo && !situation.inDungeon &&
        situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation);

    if (situation.inCombat || situation.hasAttackers ||
        (situation.partyInCombat && !remoteSoloPartyCombat))
        return false;

    if (situation.botHealth < sPlayerbotAIConfig.almostFullHealth)
        return false;

    if (!remoteSoloPartyCombat &&
        (situation.lowestPartyHealth < sPlayerbotAIConfig.almostFullHealth ||
         situation.damagedPartyMembers > 0))
        return false;

    if (mode == FriendMode::Dungeon)
        return situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation);

    if (mode == FriendMode::Party)
        return situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation);

    return true;
}

FriendIdleGoal FriendBotController::SelectIdleGoal(const FriendSituation& situation)
{
    const time_t now = time(nullptr);
    if (idleGoal != FriendIdleGoal::None && idleGoalUntil > now)
    {
        if (mode != FriendMode::Dungeon ||
            idleGoal == FriendIdleGoal::OrbitLeader ||
            idleGoal == FriendIdleGoal::Loiter)
            return idleGoal;
    }

    idleGoal = FriendIdleGoal::None;
    idleGoalUntil = 0;
    idleNextActionAt = 0;

    struct WeightedGoal
    {
        FriendIdleGoal goal;
        uint32 weight;
    };

    std::vector<WeightedGoal> goals;
    uint32 personality = ai && ai->GetBot() ? ai->GetBot()->GetObjectGuid().GetCounter() : 0;
    uint32 socialBias = personality % 25;
    uint32 gatherBias = (personality / 7) % 30;
    uint32 grindBias = (personality / 13) % 30;
    const bool canGather = HasGatherSkill();

    if (mode == FriendMode::Dungeon)
    {
        goals.push_back({ FriendIdleGoal::OrbitLeader, 75 + socialBias });
        goals.push_back({ FriendIdleGoal::Loiter, 25 });
    }
    else if (mode == FriendMode::Solo)
    {
        goals.push_back({ FriendIdleGoal::OrbitLeader, 10 + socialBias / 3 });
        goals.push_back({ FriendIdleGoal::Loiter, 8 });

        if (NeedsTownChores(situation) && IsSafeForTownChores(situation))
            goals.push_back({ FriendIdleGoal::Resupply, 70 });

        if (situation.possibleTargetsCount > 0 && situation.botMana >= sPlayerbotAIConfig.lowMana)
            goals.push_back({ FriendIdleGoal::GrindNearby, 65 + grindBias });
        else if (!NeedsTownChores(situation) && situation.botHealth >= sPlayerbotAIConfig.almostFullHealth &&
            situation.botMana >= sPlayerbotAIConfig.lowMana)
            goals.push_back({ FriendIdleGoal::TravelToGrind, 55 + grindBias });

        if (canGather)
        {
            goals.push_back({ FriendIdleGoal::GatherNearby, 25 + gatherBias });
            if (!NeedsTownChores(situation))
                goals.push_back({ FriendIdleGoal::TravelToGather, 25 + gatherBias / 2 });
        }

        if (!NeedsTownChores(situation) && !situation.possibleTargetsCount)
            goals.push_back({ FriendIdleGoal::ExploreNearby, 15 });
    }
    else
    {
        goals.push_back({ FriendIdleGoal::OrbitLeader, 45 + socialBias });
        goals.push_back({ FriendIdleGoal::Loiter, 30 });

        if (NeedsTownChores(situation) && IsSafeForTownChores(situation))
            goals.push_back({ FriendIdleGoal::Resupply, 45 });

        if (canGather && situation.botMana >= sPlayerbotAIConfig.mediumMana)
            goals.push_back({ FriendIdleGoal::GatherNearby, 12 + gatherBias });

        if (situation.possibleTargetsCount > 0 && situation.botMana >= sPlayerbotAIConfig.mediumMana)
            goals.push_back({ FriendIdleGoal::GrindNearby, 18 + grindBias });
    }

    uint32 total = 0;
    for (const WeightedGoal& goal : goals)
        total += goal.weight;

    if (!total)
        return FriendIdleGoal::None;

    uint32 roll = urand(1, total);
    for (const WeightedGoal& goal : goals)
    {
        if (roll <= goal.weight)
        {
            idleGoal = goal.goal;
            break;
        }
        roll -= goal.weight;
    }

    uint32 lease = 20;
    if (mode == FriendMode::Dungeon)
        lease = urand(8, 20);
    else if (mode == FriendMode::Solo)
        lease = urand(45, 150);
    else
        lease = urand(18, 55);

    idleGoalUntil = now + lease;
    return idleGoal;
}

bool FriendBotController::ExecuteCurrentIdleGoal(const FriendSituation& situation)
{
    const time_t now = time(nullptr);
    if (idleNextActionAt > now)
    {
        SetResult(lastIntent, "idle " + IdleGoalName(idleGoal), FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    switch (idleGoal)
    {
        case FriendIdleGoal::Resupply:
            if (ExecuteResupply(situation))
            {
                idleNextActionAt = now + urand(2, 6);
                return true;
            }
            break;

        case FriendIdleGoal::GatherNearby:
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
                        idleNextActionAt = now + urand(4, 10);
                        return true;
                    }

                    idleNextActionAt = now + urand(4, 10);
                    return true;
                }
                if (ExecuteLoot(situation, true))
                {
                    idleNextActionAt = now + urand(4, 10);
                    return true;
                }
            }
            break;

        case FriendIdleGoal::GrindNearby:
            if (mode != FriendMode::Dungeon && situation.possibleTargetsCount > 0 &&
                TryAction("attack anything", "friend idle") == FriendExecutionResult::Done)
            {
                MaybeSayActivity(situation, "grind", {
                    "I'll clear a few nearby.",
                    "Going to fight something nearby."
                }, mode == FriendMode::Solo ? 45 : 20, 75);
                idleNextActionAt = now + urand(5, 12);
                return true;
            }
            break;

        case FriendIdleGoal::TravelToGrind:
            if (ExecuteIdleTravelGoal(situation, FRIEND_GRIND_TRAVEL_PURPOSE, FriendIdleGoal::GrindNearby,
                "travel to grind", {
                    "I'm going to find something useful to fight.",
                    "I'll look for a good place to grind."
                }))
            {
                idleNextActionAt = now + urand(3, 8);
                return true;
            }
            break;

        case FriendIdleGoal::TravelToGather:
        {
            uint32 gatherPurpose = SelectGatherTravelPurpose();
            if (gatherPurpose && ExecuteIdleTravelGoal(situation, gatherPurpose, FriendIdleGoal::GatherNearby,
                "travel to gather", {
                    "I'm going to look for materials.",
                    "I'll go gather for a bit."
                }))
            {
                idleNextActionAt = now + urand(3, 8);
                return true;
            }
            break;
        }

        case FriendIdleGoal::ExploreNearby:
            if (ExecuteIdleTravelGoal(situation, FRIEND_EXPLORE_TRAVEL_PURPOSE, FriendIdleGoal::Loiter,
                "travel to explore", {
                    "I'm going to look around a bit.",
                    "I'll scout around nearby."
                }))
            {
                idleNextActionAt = now + urand(6, 14);
                return true;
            }
            break;

        case FriendIdleGoal::OrbitLeader:
            if (MoveInLeaderOrbit(situation, "idle orbit", false))
            {
                idleNextActionAt = now + urand(8, 18);
                return true;
            }
            break;

        case FriendIdleGoal::Loiter:
            idleNextActionAt = now + urand(6, 18);
            SetResult(lastIntent, "hang out", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
            return true;

        case FriendIdleGoal::None:
            break;
    }

    idleGoal = FriendIdleGoal::None;
    idleGoalUntil = 0;
    idleNextActionAt = now + urand(4, 10);

    if (situation.leaderSafe)
        return MoveInLeaderOrbit(situation, "idle orbit", false);

    return false;
}

bool FriendBotController::ExecuteIdleTravelGoal(const FriendSituation& situation, uint32 purpose, FriendIdleGoal workGoal,
    const std::string& action, std::initializer_list<const char*> lines)
{
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext() || mode != FriendMode::Solo || situation.inDungeon || purpose == 0)
        return false;

    if (situation.travelTargetWorking && situation.travelTargetPurpose == purpose)
    {
        idleGoal = workGoal;
        idleGoalUntil = time(nullptr) + urand(45, 120);
        idleTravelRequested = false;
        idleTravelPurpose = 0;
        SetResult(lastIntent, "arrived " + IdleGoalName(workGoal), FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if (idleTravelRequested && idleTravelPurpose && idleTravelPurpose != purpose)
        return false;

    if ((situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetActive) &&
        (!idleTravelRequested || idleTravelPurpose != purpose))
    {
        if (TryAction("reset travel target", "friend idle", 0, ai->GetBot()) == FriendExecutionResult::Done)
            return true;
        return false;
    }

    if (situation.travelTargetPreparing && idleTravelRequested)
    {
        FriendExecutionResult result = TryAction("choose travel target", "friend idle", 0, ai->GetBot());
        if (result == FriendExecutionResult::Done)
            return true;

        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!target || target->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
            return false;

        SetResult(lastIntent, action + ":preparing", FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    if ((situation.travelTargetTraveling || situation.travelTargetActive) && idleTravelRequested)
    {
        if (MoveToFriendTravelTarget(situation))
            return true;

        if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) &&
            !ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+travel once", BotState::BOT_STATE_NON_COMBAT);

        return TryAction("move to travel target", "friend idle", 0, ai->GetBot()) == FriendExecutionResult::Done;
    }

    if (TryAction("request travel target::" + std::to_string(purpose), "friend idle", 0, ai->GetBot()) == FriendExecutionResult::Done)
    {
        idleTravelRequested = true;
        idleTravelPurpose = purpose;
        MaybeSayActivity(situation, action, lines, 70, 90);
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

bool FriendBotController::PreferFreeDamage(const FriendSituation& situation) const
{
    return GetCombatStyle(situation) == FriendCombatStyle::Dry;
}

float FriendBotController::PreferredLeaderDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (mode == FriendMode::Dungeon)
        return std::max(followRange, 6.0f);
    if (mode == FriendMode::Solo && !situation.inDungeon)
        return std::max(followRange * 2.5f, 28.0f);

    if (situation.inDungeon)
        return std::max(followRange, 8.0f);

    if (situation.inTown)
        return std::max(followRange * 1.75f, 18.0f);

    return std::max(followRange * 1.25f, 12.0f);
}

float FriendBotController::SoftLeashDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (mode == FriendMode::Dungeon)
        return std::max(followRange * 1.25f, 12.0f);
    if (mode == FriendMode::Solo && !situation.inDungeon)
        return std::max(followRange * 4.0f, 60.0f);

    if (situation.inDungeon)
        return std::max(followRange * 1.5f, 16.0f);

    if (situation.inTown)
        return std::max(followRange * 3.0f, 45.0f);

    return std::max(followRange * 2.0f, 24.0f);
}

float FriendBotController::HardLeashDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (mode == FriendMode::Solo && !situation.inDungeon)
        return std::max(followRange * 6.0f, 120.0f);

    if (situation.inDungeon)
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
        action != "idle orbit")
    {
        ClearFriendMovement(true);
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    const float distance = minDistance + static_cast<float>(urand(0, 1000)) / 1000.0f * (maxDistance - minDistance);
    const float angle = static_cast<float>(urand(0, 6283)) / 1000.0f;
    float x = leader->GetPositionX() + std::cos(angle) * distance;
    float y = leader->GetPositionY() + std::sin(angle) * distance;
    float z = leader->GetPositionZ();
    if (!NormalizeFriendMovePosition(x, y, z))
        return false;

    WorldPosition from(bot);
    WorldPosition to(bot->GetMapId(), x, y, z);
    if (!from.canPathTo(to, bot) || !bot->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), true))
    {
        x = leader->GetPositionX();
        y = leader->GetPositionY();
        z = leader->GetPositionZ();
        if (!NormalizeFriendMovePosition(x, y, z))
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
    if (movementType == FOLLOW_MOTION_TYPE ||
        (includePointMove && movementType == POINT_MOTION_TYPE &&
            (lastAction == "move near leader" || lastAction == "come" ||
             lastAction == "stay close" || lastAction == "idle orbit" ||
             lastAction == "recover position")))
    {
        ai->StopMoving();
    }
}

void FriendBotController::SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result)
{
    lastIntent = intent;
    lastAction = action;
    lastResult = result;
}

void FriendBotController::MaybeSayStatus(const FriendSituation& situation)
{
    if (!ai || verbosity == FriendVerbosity::Silent)
        return;

    std::ostringstream out;
    out << "friend intent: " << IntentName(lastIntent);
    out << " -> " << (lastAction.empty() ? ResultName(lastResult) : lastAction);
    if (verbosity == FriendVerbosity::Debug)
    {
        out << " [" << ResultName(lastResult);
        out << ", mode " << ModeName(mode);
        out << ", command " << CommandName(command);
        out << ", idle " << IdleGoalName(idleGoal);
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
        out << ", targetDist " << static_cast<uint32>(situation.targetDistance);
        out << ", calm " << situation.calmDowntimeSeconds << "s";
        out << ", " << BalanceName(situation.balance);
        out << ", targets " << static_cast<uint32>(situation.possibleTargetsCount) << "]";
        if (situation.crowdControlledTargets)
            out << " [cc " << static_cast<uint32>(situation.crowdControlledTargets) << "]";
        if (situation.hasCreatureLoot)
            out << " [loot]";
        if (NeedsTownChores(situation))
            out << " [chores sell=" << (situation.shouldSell ? "y" : "n")
                << " repair=" << (situation.shouldRepair ? "y" : "n")
                << " buy=" << (situation.shouldBuy ? "y" : "n")
                << " train=" << (situation.shouldTrain ? "y" : "n")
                << " bag=" << (situation.shouldUpgradeBags ? "y" : "n")
                << " gear=" << (situation.shouldUpgradeGear ? "y" : "n") << "]";
        if (situation.travelTargetPreparing || situation.travelTargetTraveling || situation.travelTargetWorking)
            out << " [travel " << (situation.travelTargetWorking ? "work" : (situation.travelTargetTraveling ? "move" : "prep"))
                << " purpose=" << situation.travelTargetPurpose << "]";
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
    ai->TellPlayerNoFacing(requester, "debug: report, intent/verbose, debug, silent, items [filter], equip [slot], help",
        PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
}

void FriendBotController::ResetTemporaryCommandIfSatisfied(const FriendSituation& situation)
{
    if (command == FriendCommand::ReturnToParty && situation.leaderSafe &&
        situation.leaderDistance <= std::max(ai->GetRange("follow") * 0.75f, 6.0f))
    {
        command = FriendCommand::None;
        idleGoal = FriendIdleGoal::None;
        idleGoalUntil = 0;
        idleNextActionAt = 0;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
    }

    if (command == FriendCommand::Recover &&
        !situation.inCombat &&
        situation.botHealth >= FRIEND_REST_DONE_HEALTH &&
        situation.botMana >= FRIEND_REST_DONE_MANA)
    {
        command = FriendCommand::None;
        resupplyTravelRequested = false;
        idleTravelRequested = false;
        idleTravelPurpose = 0;
    }

    if (command == FriendCommand::Shop && (mode == FriendMode::Dungeon || situation.inDungeon))
    {
        command = FriendCommand::None;
        ClearIdleState();
    }
}

void FriendBotController::ClearIdleState()
{
    idleGoal = FriendIdleGoal::None;
    idleGoalUntil = 0;
    idleNextActionAt = 0;
    resupplyTravelRequested = false;
    idleTravelRequested = false;
    idleTravelPurpose = 0;
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
            if (!situation.inDungeon)
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
            AddActions(actions, { "ice block", "blink", "mana shield", "ice barrier", "frost nova" });
            break;
        case CLASS_WARLOCK:
            AddActions(actions, { "death coil", "sacrifice", "soulshatter", "drain life" });
            if (!situation.inDungeon)
                actions.push_back("fear");
            break;
        case CLASS_DRUID:
            AddActions(actions, { "barskin", "survival instincts", "frenzied regeneration", "regrowth", "rejuvenation", "nature's grasp" });
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

    if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth || situation.lowestPartyHealthDelta <= -12)
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
            "paladin aura", "trueshot aura", "water shield", "lightning shield",
            "demon armor", "demon skin", "fel armor", "inner fire", "mage armor", "ice armor",
            "molten armor", "aspect of the hawk", "aspect of the viper", "horn of winter"
        });
    }

    return actions;
}

std::vector<std::string> FriendBotController::CrowdControlActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    if (!situation.inDungeon && situation.possibleTargetsCount < 3)
        return actions;

    AddActions(actions, {
        "polymorph", "freezing trap on cc", "entangling roots on cc", "hibernate on cc",
        "banish on cc", "shackle undead", "repentance", "blind", "sap",
        "frost nova", "earthbind totem", "hammer of justice", "bash", "scatter shot"
    });

    if (!situation.inDungeon)
        AddActions(actions, { "fear on cc", "psychic scream", "intimidating shout" });

    return actions;
}

std::vector<std::string> FriendBotController::PullActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    if (mode == FriendMode::Dungeon || situation.inDungeon || situation.inCombat || situation.partyInCombat || situation.damagedPartyMembers ||
        !situation.leaderSafe || situation.leaderDistance > sPlayerbotAIConfig.reactDistance ||
        situation.nearbyPartyMembers < 2 || situation.possibleTargetsCount == 0 || situation.possibleTargetsCount > 2)
        return actions;

    switch (ai->GetBot()->getClass())
    {
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_DRUID:
            actions.push_back("attack anything");
            break;
        default:
            break;
    }

    return actions;
}

std::vector<std::string> FriendBotController::DamageActions(const FriendSituation& situation) const
{
    std::vector<std::string> actions;
    if (situation.tankish)
        AddActions(actions, { "taunt", "hand of reckoning", "righteous defense", "growl", "dark command" });

    const bool preferFreeDamage = PreferFreeDamage(situation);
    if (preferFreeDamage)
        AddActions(actions, { "shoot", "melee", "attack" });

    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
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

std::string FriendBotController::IdleGoalName(FriendIdleGoal value)
{
    switch (value)
    {
        case FriendIdleGoal::None: return "none";
        case FriendIdleGoal::Loiter: return "loiter";
        case FriendIdleGoal::OrbitLeader: return "orbit";
        case FriendIdleGoal::Resupply: return "resupply";
        case FriendIdleGoal::GatherNearby: return "gather";
        case FriendIdleGoal::GrindNearby: return "grind";
        case FriendIdleGoal::TravelToGrind: return "travel grind";
        case FriendIdleGoal::TravelToGather: return "travel gather";
        case FriendIdleGoal::ExploreNearby: return "explore";
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
        case FriendIntent::SavePartyMember: return "save party";
        case FriendIntent::BuffOrCureParty: return "support";
        case FriendIntent::CrowdControl: return "cc";
        case FriendIntent::PullWithParty: return "pull";
        case FriendIntent::LootNearby: return "loot";
        case FriendIntent::DealDamage: return "damage";
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
