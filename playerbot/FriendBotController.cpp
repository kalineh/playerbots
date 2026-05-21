#include "playerbot/playerbot.h"
#include "FriendBotController.h"

#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "ServerFacade.h"
#include "strategy/Action.h"
#include "strategy/AiObjectContext.h"

#include <algorithm>
#include <cctype>
#include <sstream>

using namespace ai;

FriendBotController::FriendBotController(PlayerbotAI* ai) : ai(ai)
{
}

void FriendBotController::Reset()
{
    assignment = FriendAssignment::ParticipateWithParty;
    verbosity = FriendVerbosity::Silent;
    lastIntent = FriendIntent::FollowOrIdle;
    lastResult = FriendExecutionResult::None;
    lastAction.clear();
    lastHealth = ai && ai->GetBot() ? ai->GetBot()->GetHealthPercent() : 100;
    lastBarkTime = 0;
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

    FriendSituation situation = BuildSituation();
    FriendIntent intent = SelectIntent(situation);

    if (!ExecuteIntent(intent, situation))
    {
        SetResult(intent, "", FriendExecutionResult::IntentionalIdle);
        ai->SetActionDuration(minimal ? sPlayerbotAIConfig.reactDelay : sPlayerbotAIConfig.globalCoolDown);
    }
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
    return out.str();
}

FriendSituation FriendBotController::BuildSituation()
{
    FriendSituation situation;
    Player* bot = ai->GetBot();
    AiObjectContext* context = ai->GetAiObjectContext();

    situation.inCombat = sServerFacade.IsInCombat(bot);
    situation.inDungeon = bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid());
    situation.botHealth = bot->GetHealthPercent();
    situation.botMana = ai->GetManaPercent();
    situation.botHealthDelta = static_cast<int32>(situation.botHealth) - static_cast<int32>(lastHealth);
    lastHealth = situation.botHealth;

    if (context)
    {
        situation.hasAttackers = context->GetValue<bool>("has attackers")->Get();

        if (Unit* healTarget = context->GetValue<Unit*>("party member to heal")->Get())
        {
            if (healTarget->IsAlive())
            {
                situation.lowestPartyHealth = healTarget->GetHealthPercent();
                if (situation.lowestPartyHealth < sPlayerbotAIConfig.almostFullHealth)
                    situation.damagedPartyMembers = 1;
            }
        }
    }

    Player* leader = ai->GetGroupMaster();
    if (leader && ai->IsSafe(leader))
    {
        situation.leaderGuid = leader->GetObjectGuid();
        situation.leaderDistance = sServerFacade.GetDistance2d(bot, leader);
    }

    return situation;
}

FriendIntent FriendBotController::SelectIntent(const FriendSituation& situation) const
{
    if (assignment == FriendAssignment::HoldPosition)
        return FriendIntent::HoldPosition;

    if (assignment == FriendAssignment::Recover)
        return FriendIntent::RecoverResources;

    if (assignment == FriendAssignment::ReturnToParty)
        return FriendIntent::ReturnToParty;

    if (situation.botHealth < sPlayerbotAIConfig.lowHealth ||
        (situation.botHealth < sPlayerbotAIConfig.mediumHealth && situation.botHealthDelta < -10))
        return FriendIntent::SaveSelf;

    if (situation.lowestPartyHealth < sPlayerbotAIConfig.lowHealth ||
        (situation.lowestPartyHealth < sPlayerbotAIConfig.mediumHealth && situation.damagedPartyMembers))
        return FriendIntent::SavePartyMember;

    if (situation.inCombat || situation.hasAttackers)
        return FriendIntent::DealDamage;

    if (situation.botMana < sPlayerbotAIConfig.lowMana)
        return FriendIntent::RecoverResources;

    if (situation.leaderGuid && situation.leaderDistance > ai->GetRange("follow"))
        return FriendIntent::ReturnToParty;

    return FriendIntent::FollowOrIdle;
}

bool FriendBotController::ExecuteIntent(FriendIntent intent, const FriendSituation& situation)
{
    lastIntent = intent;

    switch (intent)
    {
        case FriendIntent::HoldPosition:
            ai->StopMoving();
            SetResult(intent, "hold", FriendExecutionResult::Done);
            ai->SetActionDuration(sPlayerbotAIConfig.reactDelay);
            return true;

        case FriendIntent::ReturnToParty:
            return TryActions({ "follow" }, "friend return");

        case FriendIntent::RecoverResources:
            if (!situation.inCombat)
                return TryActions({ "drink", "food", "sit" }, "friend recover");
            return false;

        case FriendIntent::SaveSelf:
            if (TryActions({ "healthstone", "healing potion", "whipper root tuber", "use bandage" }, "friend self"))
                return true;
            if (!situation.inCombat)
                return TryActions({ "food" }, "friend self");
            return false;

        case FriendIntent::SavePartyMember:
            return TryActions({
                "power word: shield on party",
                "flash heal on party",
                "lesser heal on party",
                "heal on party",
                "renew on party",
                "healing wave on party",
                "lesser healing wave on party",
                "chain heal",
                "holy light on party",
                "flash of light on party",
                "rejuvenation on party",
                "regrowth on party",
                "healing touch on party"
            }, "friend heal");

        case FriendIntent::DealDamage:
            return TryActions({
                "dps assist",
                "attack least hp target",
                "shoot",
                "melee",
                "attack"
            }, "friend damage");

        case FriendIntent::FollowOrIdle:
            if (assignment == FriendAssignment::StayClose && situation.leaderGuid)
                return TryActions({ "follow" }, "friend close");
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
        if (result == FriendExecutionResult::Done)
            return true;
    }

    return false;
}

FriendExecutionResult FriendBotController::TryAction(const std::string& name, const std::string& source)
{
    if (!ai || !ai->GetAiObjectContext())
        return FriendExecutionResult::BlockedNoAction;

    Action* action = ai->GetAiObjectContext()->GetAction(name);
    if (!action)
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNoAction);
        return FriendExecutionResult::BlockedNoAction;
    }

    if (!action->isUseful())
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNotUseful);
        return FriendExecutionResult::BlockedNotUseful;
    }

    if (!action->isPossible())
    {
        SetResult(lastIntent, name, FriendExecutionResult::BlockedNotPossible);
        return FriendExecutionResult::BlockedNotPossible;
    }

    Event event(source);
    bool executed = action->Execute(event);
    FriendExecutionResult result = executed ? FriendExecutionResult::Done : FriendExecutionResult::Failed;
    if (executed)
        ai->SetActionDuration(action);

    SetResult(lastIntent, name, result);
    if (executed)
        MaybeSayIntent(lastIntent, name);

    return result;
}

void FriendBotController::SetResult(FriendIntent intent, const std::string& action, FriendExecutionResult result)
{
    lastIntent = intent;
    lastAction = action;
    lastResult = result;
}

void FriendBotController::MaybeSayIntent(FriendIntent intent, const std::string& action)
{
    if (!ai || verbosity == FriendVerbosity::Silent)
        return;

    time_t now = time(nullptr);
    if (verbosity == FriendVerbosity::Intent && now < lastBarkTime + 10)
        return;

    lastBarkTime = now;

    Player* master = ai->GetMaster();
    if (!master)
        return;

    std::ostringstream out;
    out << "friend intent: " << IntentName(intent) << " -> " << action;
    ai->TellPlayerNoFacing(master, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
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
        case FriendIntent::RecoverResources: return "recover";
        case FriendIntent::SaveSelf: return "save self";
        case FriendIntent::SavePartyMember: return "save party";
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
