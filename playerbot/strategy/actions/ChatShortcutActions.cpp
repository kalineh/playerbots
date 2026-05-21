
#include "playerbot/playerbot.h"
#include "ChatShortcutActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/strategy/values/Formations.h"

#include <algorithm>
#include <cctype>

using namespace ai;

namespace
{
    std::string NormalizeFriendCommand(std::string text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.erase(text.begin());

        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();

        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
}

void ReturnPositionResetAction::ResetPosition(std::string posName)
{
    ai::PositionMap& posMap = context->GetValue<ai::PositionMap&>("position")->Get();
    ai::PositionEntry pos = posMap[posName];
    pos.Reset();
    posMap[posName] = pos;
}

void ReturnPositionResetAction::SetPosition(WorldPosition wPos, std::string posName)
{
    ai::PositionMap& posMap = context->GetValue<ai::PositionMap&>("position")->Get();
    ai::PositionEntry pos = posMap[posName];
    pos.Set(wPos);
    posMap[posName] = pos;
}

void ReturnPositionResetAction::PrintStrategies(PlayerbotAI* ai, Event& event)
{
    if (event.getParam() == "?")
    {
        Player* requester = event.getOwner() ? event.getOwner() : ai->GetMaster();
        ai->PrintStrategies(requester, BotState::BOT_STATE_ALL);
    }
}

bool FollowChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+follow,-passive,-stay", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("-stay,-guard", BotState::BOT_STATE_COMBAT);

    if(ai->HasStrategy("passive", BotState::BOT_STATE_COMBAT)) //Remove flee
        ai->ChangeStrategy("-passive,-follow", BotState::BOT_STATE_COMBAT);

    ai::PositionMap& posMap = context->GetValue<ai::PositionMap&>("position")->Get();
    ai::PositionEntry pos = posMap["return"];
    pos.Reset();
    posMap["return"] = pos;

    ReturnPositionResetAction::PrintStrategies(ai, event);

    Formation* formation = AI_VALUE(Formation*, "formation");
    MEM_AI_VALUE(WorldPosition, "master position")->Reset();

    if (formation->getName() == "custom") //If in custom formation set relative position to current position.
    {
        ai::PositionEntry pos = posMap["follow"];

        WorldPosition relPos(bot);

        if (!ai->IsSafe(requester) || sServerFacade.GetDistance2d(bot, requester) > sPlayerbotAIConfig.reactDistance) //Use default formation location.
        {
            relPos = WorldPosition(bot->GetMapId(), cos(GetFollowAngle()) * ai->GetRange("follow"), sin(GetFollowAngle()) * ai->GetRange("follow"), 0);
        }
        else //Use relative location.
        {
            relPos -= WorldPosition(ai->GetMaster());
            relPos.rotateXY(-1 * ai->GetMaster()->GetOrientation());
        }

        pos.Set(relPos.getX(), relPos.getY(), relPos.getZ(), relPos.getMapId());
        posMap["follow"] = pos;
    }

    if (sServerFacade.IsInCombat(bot))
    {     
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || loc.mapid == -1)
            return false;

        if (MoveTo(loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, false, false))
        {
            ai->TellPlayerNoFacing(requester, BOT_TEXT("following"));
            return true;
        }
    }

    ai->TellPlayerNoFacing(requester, BOT_TEXT("following"));
    return true;
}

bool StayChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+stay,-follow,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+stay,-follow,-passive", BotState::BOT_STATE_COMBAT);

    SetPosition(bot);
    SetPosition(bot, "stay");
    MEM_AI_VALUE(WorldPosition, "master position")->Reset();

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("staying"));
    return true;
}

bool GuardChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+guard,-follow,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+guard,-follow,-passive", BotState::BOT_STATE_COMBAT);

    SetPosition(bot);
    SetPosition(bot, "guard");
    MEM_AI_VALUE(WorldPosition, "master position")->Reset();  

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("guarding"));
    return true;
}

bool FreeChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+free,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+free,-passive", BotState::BOT_STATE_COMBAT);

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("free_moving"));
    return true;
}

bool FleeChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+follow,+passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+follow,+passive", BotState::BOT_STATE_COMBAT);
    ResetPosition();

    PrintStrategies(ai, event);

    if (bot->GetMapId() != requester->GetMapId() || sServerFacade.GetDistance2d(bot, requester) > sPlayerbotAIConfig.sightDistance)
    {
        ai->TellPlayerNoFacing(requester, BOT_TEXT("fleeing_far"));
        return true;
    }
    ai->TellPlayerNoFacing(requester, BOT_TEXT("fleeing"));
    return true;
}

bool GoawayChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+runaway", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+runaway", BotState::BOT_STATE_COMBAT);
    ResetPosition();

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, "Running away");
    return true;
}

bool GrindChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+grind,-passive,-follow,-follow jump", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+grind,-passive,-follow,-follow jump", BotState::BOT_STATE_COMBAT);
    ResetPosition();
    ai->TellPlayerNoFacing(requester, BOT_TEXT("grinding"));
    return true;
}

bool SoloChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+rpg,+travel,+free,-follow,-follow jump,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+grind,+free,-follow,-follow jump,-passive", BotState::BOT_STATE_COMBAT);
    ResetPosition();
    ai->TellPlayerNoFacing(requester, "Going solo.");
    return true;
}

bool TankAttackChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    if (!ai->IsTank(bot))
        return false;

    ai->Reset();
    ai->ChangeStrategy("-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("-passive", BotState::BOT_STATE_COMBAT);
    ResetPosition();
    ai->TellPlayerNoFacing(requester, BOT_TEXT("attacking"));
    return true;
}

bool MaxDpsChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    if (!ai->ContainsStrategy(STRATEGY_TYPE_DPS))
        return false;

    ai->Reset();
    ai->ChangeStrategy("-threat,-conserve mana,-cast time,+dps debuff,+boost", BotState::BOT_STATE_COMBAT);
    ai->TellPlayerNoFacing(requester, "Max DPS!");
    return true;
}

bool FriendModeChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    const std::string param = NormalizeFriendCommand(event.getParam());
    const bool wantReport = param.find('?') != std::string::npos;

    ai->EnableFriendMode();
    std::string response;
    if (!param.empty() && param != "?")
    {
        ai->HandleFriendCommand(param, requester, response);
    }

<<<<<<< HEAD
    ai->ChangeStrategy("+friend", BotState::BOT_STATE_ALL);
    ai->TellPlayerNoFacing(requester, "Friend mode activated. (v3)");
    if (wasFriendMode && wantReport)
=======
    ai->TellPlayerNoFacing(requester, response.empty() ? "Friend mode activated." : response);
    if (wantReport)
>>>>>>> 8170c8e09ffe08c56c84d4e21c4563d53dfa3675
    {
        ai->ReportFriendModeStatus(requester);
    }
    return true;
}

bool FriendCommandChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    if (!ai->IsFriendMode())
        ai->EnableFriendMode();

    std::string response;
    const bool handled = ai->HandleFriendCommand(command, requester, response);
    if (!response.empty())
        ai->TellPlayerNoFacing(requester, response);

    return handled;
}

bool StrictModeChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->DisableFriendMode();
    ai->TellPlayerNoFacing(requester, "Strict mode restored.");
    return true;
}
