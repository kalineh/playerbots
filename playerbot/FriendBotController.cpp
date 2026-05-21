#include "playerbot/playerbot.h"
#include "FriendBotController.h"

#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "ServerFacade.h"
#include "strategy/Action.h"
#include "strategy/AiObjectContext.h"
#include "strategy/Strategy.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <sstream>

using namespace ai;

namespace
{
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
}

FriendBotController::FriendBotController(PlayerbotAI* ai) : ai(ai)
{
}

void FriendBotController::Reset()
{
    assignment = FriendAssignment::ParticipateWithParty;
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
    manualAttackUntil = 0;
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
    ResetTemporaryAssignmentIfSatisfied(situation);

    FriendIntent intent = SelectIntent(situation);

    if (!ExecuteIntent(intent, situation))
    {
        SetResult(intent, "", FriendExecutionResult::IntentionalIdle);
        ai->SetActionDuration(minimal ? sPlayerbotAIConfig.reactDelay : sPlayerbotAIConfig.globalCoolDown);
    }

    MaybeSayStatus(situation);
}

bool FriendBotController::HandleCommand(const std::string& command, Player* requester, std::string& response)
{
    std::string cmd = command;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (cmd == "stop" || cmd == "hold" || cmd == "dont move" || cmd == "don't move")
    {
        assignment = FriendAssignment::HoldPosition;
        if (ai)
            ai->StopMoving();
        response = "Holding position.";
        return true;
    }

    if (cmd == "come" || cmd == "come here" || cmd == "return")
    {
        assignment = FriendAssignment::ReturnToParty;
        response = "Coming back.";
        return true;
    }

    if (cmd == "normal" || cmd == "reset" || cmd == "act normal")
    {
        assignment = FriendAssignment::ParticipateWithParty;
        verbosity = FriendVerbosity::Silent;
        manualAttackUntil = 0;
        response = "Acting normal.";
        return true;
    }

    if (cmd == "stay close" || cmd == "close")
    {
        assignment = FriendAssignment::StayClose;
        response = "Staying close.";
        return true;
    }

    if (cmd == "recover" || cmd == "drink")
    {
        assignment = FriendAssignment::Recover;
        response = "Recovering.";
        return true;
    }

    if (cmd == "attack")
    {
        assignment = FriendAssignment::ParticipateWithParty;
        manualAttackUntil = time(nullptr) + 20;
        if (requester && requester->GetSelectionGuid() && ai && ai->GetAiObjectContext())
            ai->GetAiObjectContext()->GetValue<ObjectGuid>("attack target")->Set(requester->GetSelectionGuid());
        response = "Attacking with you.";
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
    out << "friend: assignment=" << AssignmentName(assignment);
    out << ", verbosity=" << VerbosityName(verbosity);
    out << ", intent=" << IntentName(lastIntent);
    out << ", action=" << (lastAction.empty() ? "none" : lastAction);
    out << ", result=" << ResultName(lastResult);
    out << ", hp=" << static_cast<uint32>(lastSituation.botHealth) << "%";
    out << ", mana=" << static_cast<uint32>(lastSituation.botMana) << "%";
    out << ", partyHp=" << static_cast<uint32>(lastSituation.lowestPartyHealth) << "%";
    out << ", balance=" << BalanceName(lastSituation.balance);
    out << ", leaderDist=" << static_cast<uint32>(lastSituation.leaderDistance);
    out << ", targets=" << static_cast<uint32>(lastSituation.possibleTargetsCount);
    out << ", abilities=" << static_cast<uint32>(abilityCatalog.GetAbilities().size());
    if (lastSituation.hasCreatureLoot)
        out << ", loot";
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
        situation.leaderInCombat = sServerFacade.IsInCombat(leader);
        situation.partyInCombat = situation.partyInCombat || situation.leaderInCombat;
    }

    return situation;
}

FriendIntent FriendBotController::SelectIntent(const FriendSituation& situation) const
{
    if (assignment == FriendAssignment::HoldPosition)
        return FriendIntent::HoldPosition;

    if (assignment == FriendAssignment::ReturnToParty)
        return FriendIntent::ReturnToParty;

    if (assignment == FriendAssignment::Recover && !situation.inCombat)
        return FriendIntent::RecoverResources;

    float softLeash = SoftLeashDistance(situation);
    float hardLeash = HardLeashDistance(situation);
    if (situation.leaderSafe && situation.leaderDistance > hardLeash &&
        (!situation.inCombat || situation.inDungeon || !situation.hasAttackers))
        return FriendIntent::ReturnToParty;

    if (situation.botHealth < sPlayerbotAIConfig.lowHealth ||
        situation.botHealthDelta <= -12 ||
        (situation.botHealth < sPlayerbotAIConfig.mediumHealth && situation.botHealthDelta < -5))
        return FriendIntent::SaveSelf;

    if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        situation.lowestPartyHealthDelta <= -12 ||
        (situation.lowestPartyHealth < sPlayerbotAIConfig.mediumHealth && situation.damagedPartyMembers))
        return FriendIntent::SavePartyMember;

    if (situation.healerish && situation.damagedPartyMembers > 0 &&
        situation.lowestPartyHealth < sPlayerbotAIConfig.almostFullHealth &&
        (situation.inCombat || situation.partyInCombat))
        return FriendIntent::SavePartyMember;

    if (situation.inCombat && (((situation.ranged || situation.healerish) && situation.hasAttackers) ||
        (situation.ranged && situation.targetDistance > 0.0f && situation.targetDistance < 8.0f)))
        return FriendIntent::ImprovePosition;

    if (!situation.inCombat && situation.botMana < sPlayerbotAIConfig.lowMana && !situation.leaderInCombat)
        return FriendIntent::RecoverResources;

    if (situation.inCombat && situation.inDungeon && situation.possibleTargetsCount > 1 && !situation.tankish)
        return FriendIntent::CrowdControl;

    if (!situation.inCombat && !situation.partyInCombat && situation.damagedPartyMembers == 0)
        return FriendIntent::BuffOrCureParty;

    if (!situation.inCombat && !situation.partyInCombat && situation.hasCreatureLoot &&
        situation.leaderSafe && situation.leaderDistance <= SoftLeashDistance(situation))
        return FriendIntent::LootNearby;

    if (!situation.inDungeon && !situation.inCombat && !situation.partyInCombat &&
        situation.leaderSafe && situation.leaderDistance <= sPlayerbotAIConfig.reactDistance &&
        situation.nearbyPartyMembers >= 2 && situation.possibleTargetsCount > 0 && situation.possibleTargetsCount <= 2 &&
        situation.botHealth >= sPlayerbotAIConfig.mediumHealth && situation.botMana >= sPlayerbotAIConfig.mediumMana)
        return FriendIntent::PullWithParty;

    if (situation.inCombat || situation.partyInCombat || situation.hasAttackers || situation.hasTarget || time(nullptr) < manualAttackUntil)
        return FriendIntent::DealDamage;

    if (situation.leaderSafe && situation.leaderDistance > softLeash)
        return FriendIntent::ReturnToParty;

    if (!situation.inCombat && !situation.partyInCombat && situation.leaderSafe &&
        situation.leaderDistance > PreferredLeaderDistance(situation))
        return FriendIntent::ReturnToParty;

    return FriendIntent::FollowOrIdle;
}

bool FriendBotController::ExecuteIntent(FriendIntent intent, const FriendSituation& situation)
{
    const bool keepFriendMovement = intent == FriendIntent::ReturnToParty ||
        intent == FriendIntent::HoldPosition ||
        (intent == FriendIntent::FollowOrIdle && assignment == FriendAssignment::StayClose);
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
                assignment == FriendAssignment::ReturnToParty ? "come" : "move near leader",
                assignment == FriendAssignment::ReturnToParty);

        case FriendIntent::ImprovePosition:
            if (TryActions(PositionActions(situation), "friend position"))
                return true;
            if (situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend damage");

        case FriendIntent::RecoverResources:
            if (!situation.inCombat)
            {
                if (TryActions({ "drink", "food" }, "friend recover"))
                    return true;

                if (TryAction("sit", "friend recover") == FriendExecutionResult::Done)
                {
                    ai->SetActionDuration(std::max(sPlayerbotAIConfig.globalCoolDown, sPlayerbotAIConfig.reactDelay));
                    return true;
                }

                return false;
            }
            if (TryActions({ "mana gem", "mana potion", "dark rune", "life tap", "dark pact" }, "friend combat recover"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend damage");

        case FriendIntent::SaveSelf:
            return TryActions(SelfPreservationActions(situation), "friend self");

        case FriendIntent::SavePartyMember:
            if (TryCatalogHeal(situation, "friend heal"))
                return true;
            if (TryActions(HealActions(situation), "friend heal"))
                return true;
            if (situation.inCombat || situation.partyInCombat)
            {
                if (TryCatalogDamage(situation, "friend damage"))
                    return true;
                if (TryActions(DamageActions(situation), "friend damage"))
                    return true;
                if (situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
                    return MoveNearLeader(situation, "move near leader", false);
            }
            return false;

        case FriendIntent::BuffOrCureParty:
            if (TryCatalogSupport(situation, "friend support"))
                return true;
            if (TryActions(BuffOrCureActions(situation), "friend support"))
                return true;
            if (situation.hasCreatureLoot && ExecuteLoot(situation))
                return true;
            if (TryActions(PullActions(situation), "friend pull"))
                return true;
            if (situation.leaderSafe && situation.leaderDistance > PreferredLeaderDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            return false;

        case FriendIntent::CrowdControl:
            if (TryCatalogCrowdControl(situation, "friend cc"))
                return true;
            if (TryActions(CrowdControlActions(situation), "friend cc"))
                return true;
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            return TryActions(DamageActions(situation), "friend damage");

        case FriendIntent::PullWithParty:
            return TryActions(PullActions(situation), "friend pull");

        case FriendIntent::LootNearby:
            return ExecuteLoot(situation);

        case FriendIntent::DealDamage:
            if (TryCatalogDamage(situation, "friend damage"))
                return true;
            if (TryActions(DamageActions(situation), "friend damage"))
                return true;
            if (situation.partyInCombat && situation.leaderSafe && situation.leaderDistance > SoftLeashDistance(situation))
                return MoveNearLeader(situation, "move near leader", false);
            return false;

        case FriendIntent::FollowOrIdle:
            if (assignment == FriendAssignment::StayClose && situation.leaderGuid)
                return MoveNearLeader(situation, "move near leader", false);
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

FriendExecutionResult FriendBotController::TryAction(const std::string& name, const std::string& source, uint8 depth)
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

    if (TryPrerequisites(action, source, depth) && !action->isPossible())
        return FriendExecutionResult::Done;

    if (!action->isPossible())
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNotPossible);
        return FriendExecutionResult::BlockedNotPossible;
    }

    Event event(source, "", ai->GetMaster());
    bool executed = action->Execute(event);
    FriendExecutionResult result = executed ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
    if (executed)
        ai->SetActionDuration(action);

    SetResult(lastIntent, name, result);
    return result;
}

bool FriendBotController::TryPrerequisites(Action* action, const std::string& source, uint8 depth)
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
        if (TryAction(prerequisiteName, source, depth + 1) == FriendExecutionResult::Done)
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
    (void)situation;
    if (!ai || !ai->GetAiObjectContext())
        return nullptr;

    AiObjectContext* context = ai->GetAiObjectContext();
    Unit* target = GetContextValue<Unit*>(context, "current target", nullptr);
    if (!IsHostileTarget(ai, target))
        target = GetContextValue<Unit*>(context, "rti target", nullptr);
    if (!IsHostileTarget(ai, target))
        target = GetContextValue<Unit*>(context, "dps target", nullptr);
    if (!IsHostileTarget(ai, target))
        target = GetContextValue<Unit*>(context, "least hp target", nullptr);

    if (IsHostileTarget(ai, target))
    {
        context->GetValue<Unit*>("current target")->Set(target);
        ai->GetBot()->SetSelectionGuid(target->GetObjectGuid());
        return target;
    }

    if (!prepare)
        return nullptr;

    if (GetContextValue<Unit*>(context, "rti target", nullptr))
        TryAction("attack rti target", "friend target");
    TryAction("dps assist", "friend target");
    TryAction("attack least hp target", "friend target");

    return GetDamageTarget(situation, false);
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
    if (!ai || !ai->GetAiObjectContext() || !target || !ai->CanMove())
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    SpellCastResult checkResult = SPELL_CAST_OK;
    if (!ai->CanCastSpell(ability.spellId, target, 0, true, nullptr, true, false, false, &checkResult))
        return false;

    std::string reachAction;
    if (IsHostileTarget(ai, target))
    {
        context->GetValue<Unit*>("current target")->Set(target);
        ai->GetBot()->SetSelectionGuid(target->GetObjectGuid());
        reachAction = ability.Has(FRIEND_ABILITY_MELEE) ? "reach melee" : "reach spell::" + ability.name + "::current target";
    }
    else
    {
        Unit* partyHealTarget = GetContextValue<Unit*>(context, "party member to heal", nullptr);
        if (partyHealTarget != target)
            return false;

        reachAction = "reach spell::" + ability.name + "::party member to heal";
    }

    return TryAction(reachAction, source) == FriendExecutionResult::Done;
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
        if (situation.botMana < sPlayerbotAIConfig.lowMana && ability.Has(FRIEND_ABILITY_RANGED))
            score -= 20;

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

    if (situation.inCombat || situation.partyInCombat || situation.botMana < sPlayerbotAIConfig.mediumMana)
        return false;

    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_BUFF_CORE) || ability.Has(FRIEND_ABILITY_BUFF_SITUATIONAL) ||
            ability.Has(FRIEND_ABILITY_HEAL) || ability.Has(FRIEND_ABILITY_DAMAGE) || ability.Has(FRIEND_ABILITY_CURE))
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

    AiObjectContext* context = ai->GetAiObjectContext();
    Unit* damageTarget = GetDamageTarget(situation, false);
    const bool targetCasting = damageTarget && damageTarget->IsNonMeleeSpellCasted(false);

    struct Candidate
    {
        const FriendAbility* ability;
        Unit* target;
        int32 score;
    };

    std::vector<Candidate> candidates;
    for (const FriendAbility& ability : abilityCatalog.GetAbilities())
    {
        if (!ability.Has(FRIEND_ABILITY_CC) && !ability.Has(FRIEND_ABILITY_INTERRUPT))
            continue;

        if (situation.inDungeon && ability.Has(FRIEND_ABILITY_FEAR))
            continue;

        Unit* target = nullptr;
        if (ability.Has(FRIEND_ABILITY_INTERRUPT) && targetCasting)
            target = damageTarget;

        if (!target)
            target = context->GetValue<Unit*>("cc target", ability.name)->Get();

        if (!IsHostileTarget(ai, target))
            continue;

        int32 score = 20;
        if (ability.Has(FRIEND_ABILITY_INTERRUPT) && target == damageTarget && targetCasting)
            score += 80;
        if (ability.Has(FRIEND_ABILITY_FEAR))
            score -= situation.inDungeon ? 100 : 10;
        if (ability.Has(FRIEND_ABILITY_AOE) && situation.possibleTargetsCount < 3)
            score -= 20;

        if (score > 0)
            candidates.push_back({ &ability, target, score });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
    {
        return left.score > right.score;
    });

    for (const Candidate& candidate : candidates)
    {
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

bool FriendBotController::ExecuteLoot(const FriendSituation& situation)
{
    (void)situation;
    if (!ai || !ai->GetBot() || !ai->GetAiObjectContext())
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();
    LootObjectStack* availableLoot = GetContextValue<LootObjectStack*>(context, "available loot", nullptr);
    LootObject lootTarget = GetContextValue<LootObject>(context, "loot target", LootObject());

    if (!lootTarget.IsEmpty() && !lootTarget.guid.IsCreature())
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

            if (loot.guid.IsCreature())
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

bool FriendBotController::PreferFreeDamage(const FriendSituation& situation) const
{
    if (!ai || !ai->GetBot() || !ai->GetBot()->HasMana())
        return false;

    if (situation.targetIsElite || situation.inDungeon || situation.possibleTargetsCount > 1 ||
        situation.botMana > sPlayerbotAIConfig.mediumMana)
        return false;

    if (situation.lowestPartyHealth < sPlayerbotAIConfig.almostFullHealth ||
        situation.damagedPartyMembers > 0 || situation.balance < 80)
        return false;

    return true;
}

float FriendBotController::PreferredLeaderDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (situation.inDungeon)
        return std::max(followRange, 8.0f);

    return std::max(followRange * 1.25f, 12.0f);
}

float FriendBotController::SoftLeashDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (situation.inDungeon)
        return std::max(followRange * 1.5f, 16.0f);

    return std::max(followRange * 2.0f, 24.0f);
}

float FriendBotController::HardLeashDistance(const FriendSituation& situation) const
{
    float followRange = ai ? ai->GetRange("follow") : 10.0f;
    if (situation.inDungeon)
        return std::max(followRange * 2.5f, 30.0f);

    return std::max(followRange * 4.0f, 70.0f);
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

    const float stopDistance = urgent ? std::max(ai->GetRange("follow") * 0.75f, 6.0f) : PreferredLeaderDistance(situation);
    if (situation.leaderDistance <= stopDistance)
    {
        ClearFriendMovement(true);
        SetResult(lastIntent, action, FriendExecutionResult::Done);
        ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
        return true;
    }

    ClearFriendMovement(false);
    bot->GetMotionMaster()->MovePoint(leader->GetMapId(), leader->GetPositionX(),
        leader->GetPositionY(), leader->GetPositionZ(), FORCED_MOVEMENT_RUN, true);
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
        (includePointMove && movementType == POINT_MOTION_TYPE && (lastAction == "move near leader" || lastAction == "come")))
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
        out << ", hp " << static_cast<uint32>(situation.botHealth) << "%";
        out << " (" << static_cast<int32>(situation.botHealthDelta) << ")";
        out << ", mana " << static_cast<uint32>(situation.botMana) << "%";
        out << ", party " << static_cast<uint32>(situation.lowestPartyHealth) << "%";
        out << " (" << static_cast<int32>(situation.lowestPartyHealthDelta) << ")";
        out << ", combat " << (situation.inCombat ? "self" : "no");
        out << "/" << (situation.partyInCombat ? "party" : "no");
        out << ", threat " << (situation.hasAttackers ? "yes" : "no");
        out << ", leader " << static_cast<uint32>(situation.leaderDistance);
        out << ", target " << static_cast<uint32>(situation.targetDistance);
        out << ", " << BalanceName(situation.balance);
        out << ", targets " << static_cast<uint32>(situation.possibleTargetsCount) << "]";
        if (situation.hasCreatureLoot)
            out << " [loot]";
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

void FriendBotController::ResetTemporaryAssignmentIfSatisfied(const FriendSituation& situation)
{
    if (assignment == FriendAssignment::ReturnToParty && situation.leaderSafe &&
        situation.leaderDistance <= std::max(ai->GetRange("follow") * 0.75f, 6.0f))
    {
        assignment = FriendAssignment::ParticipateWithParty;
    }

    if (assignment == FriendAssignment::Recover &&
        !situation.inCombat &&
        situation.botHealth >= sPlayerbotAIConfig.almostFullHealth &&
        situation.botMana >= sPlayerbotAIConfig.mediumMana)
        assignment = FriendAssignment::ParticipateWithParty;
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
    if (situation.inDungeon || situation.inCombat || situation.partyInCombat || situation.damagedPartyMembers ||
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
        AddActions(actions, { "tank assist", "taunt", "hand of reckoning", "righteous defense", "growl", "dark command" });

    AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
    if (GetContextValue<Unit*>(context, "rti target", nullptr))
        actions.push_back("attack rti target");
    AddActions(actions, { "dps assist", "attack least hp target" });

    const bool preferFreeDamage = PreferFreeDamage(situation);
    if (preferFreeDamage)
        AddActions(actions, { "shoot", "melee", "attack" });

    switch (ai->GetBot()->getClass())
    {
        case CLASS_WARRIOR:
            AddActions(actions, {
                "pummel", "shield bash", "charge", "intercept", "bloodrage", "battle shout",
                "thunder clap", "demoralizing shout", "rend", "mortal strike", "bloodthirst",
                "shield slam", "revenge", "sunder armor", "heroic strike", "execute", "cleave", "whirlwind"
            });
            break;
        case CLASS_PALADIN:
            AddActions(actions, {
                "hammer of justice", "avenger's shield", "holy shield", "seal of command", "seal of righteousness",
                "seal of wisdom", "judgement", "judgement of light", "judgement of wisdom", "crusader strike",
                "divine storm", "holy shock", "exorcism", "hammer of wrath", "consecration"
            });
            break;
        case CLASS_HUNTER:
            AddActions(actions, {
                "kill command", "hunter's mark", "serpent sting", "chimera shot", "explosive shot",
                "arcane shot", "multi-shot", "steady shot", "aimed shot", "auto shot",
                "raptor strike", "wing clip", "mend pet"
            });
            break;
        case CLASS_ROGUE:
            AddActions(actions, {
                "kick", "slice and dice", "riposte", "cheap shot", "kidney shot", "rupture",
                "sinister strike", "mutilate", "hemorrhage", "backstab", "ghostly strike",
                "eviscerate", "blade flurry", "adrenaline rush"
            });
            break;
        case CLASS_PRIEST:
            AddActions(actions, {
                "silence", "shadow word: pain", "vampiric touch", "devouring plague",
                "mind blast", "mind flay", "holy fire", "smite", "shadow word: death",
                "shadowfiend", "vampiric embrace"
            });
            break;
        case CLASS_SHAMAN:
            AddActions(actions, {
                "wind shear", "flame shock", "earth shock", "frost shock", "stormstrike", "lava lash",
                "lightning bolt", "chain lightning", "searing totem", "fire nova", "heroism", "bloodlust"
            });
            break;
        case CLASS_MAGE:
            AddActions(actions, {
                "counterspell", "living bomb", "frostbolt", "fireball", "fire blast", "scorch",
                "arcane barrage", "arcane blast", "arcane missiles", "ice lance",
                "icy veins", "combustion", "mirror image"
            });
            break;
        case CLASS_WARLOCK:
            AddActions(actions, {
                "spell lock", "corruption", "curse of agony", "curse of weakness", "immolate",
                "unstable affliction", "siphon life", "shadow bolt", "incinerate", "conflagrate",
                "drain life", "shadowburn", "death coil"
            });
            break;
        case CLASS_DRUID:
            AddActions(actions, {
                "faerie fire", "faerie fire (feral)", "moonfire", "insect swarm", "wrath", "starfire",
                "mangle (bear)", "lacerate", "maul", "swipe (bear)", "demoralizing roar",
                "mangle (cat)", "rake", "claw", "shred", "rip", "ferocious bite"
            });
            break;
#ifdef MANGOSBOT_TWO
        case CLASS_DEATH_KNIGHT:
            AddActions(actions, {
                "mind freeze", "icy touch", "plague strike", "blood strike", "heart strike",
                "scourge strike", "obliterate", "frost strike", "death coil", "death strike",
                "pestilence", "blood boil", "death and decay", "horn of winter"
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

std::string FriendBotController::AssignmentName(FriendAssignment value)
{
    switch (value)
    {
        case FriendAssignment::ParticipateWithParty: return "party";
        case FriendAssignment::StayClose: return "close";
        case FriendAssignment::ReturnToParty: return "return";
        case FriendAssignment::HoldPosition: return "hold";
        case FriendAssignment::Recover: return "recover";
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
