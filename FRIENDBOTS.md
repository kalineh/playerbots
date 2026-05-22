# Friend Bots Design Notes

These are working notes for what we want friend bots to feel like and the architecture we are converging on. They are not a strict spec or permanent implementation mandate. Update them when testing shows the model is wrong, too complex, or missing an obvious case.

## Goal

Friend mode should make a small number of bots feel like fluid party members, not like raid-role automation. A friend bot should tag along, read the party state, position sensibly, heal or buff when appropriate, use items, trade useful resources, sometimes take light initiative, and recover from bad situations without needing constant commands.

The design should favor believable, useful behavior over perfect optimal play. Friend bots should not be pure DPS machines or strict role enforcers. They should feel like sensible, slightly imperfect party members who use their class kit reasonably well, avoid obvious nonsense, and stay useful enough for normal play.

Friend bots can spend more CPU than large-scale random bots, but they still need bounded scans and simple rules.

## Non-Goals

- Do not rewrite the whole playerbot system.
- Do not fork `PlayerbotAI`, packet handling, chat handling, reactions, security, or world-state plumbing.
- Do not keep expanding friend mode by enabling every normal strategy.
- Do not depend on the normal strategy engine as a fallback when friend mode is active.
- Do not attempt full MMO encounter AI in the first pass.
- Do not build a strict "tank/healer/DPS must perform perfectly" role system.
- Do not add a heavy reactive micro-action layer. Friend mode should mostly evaluate between actions; if an action is cancelled or completes, the next evaluation can choose a better path.

## Current Direction

Friend mode is now intended to be a replacement top-level controller while friend mode is active, not a modifier over the normal strategy engine. The old approach was mostly:

- `FriendStrategy` adds small random relevance changes and heal priority nudging.
- `EnableFriendMode()` applies many existing class/spec/role strategies.
- `Engine::addStrategy()` skips sibling-strategy removal while friend mode is active.

That created some variety, but it was still the same one-action-per-tick priority engine. The newer controller approach is better for friend-like behavior because it can choose an intent first, then choose an action/task for that intent.

The main design risk now is state ownership. Testing showed that behavior becomes fragile when `mode`, `command`, idle goals, travel flags, and legacy travel targets all partially own decisions. The stable model should be:

```text
mode/command/situation
  -> score top-level intents
  -> pick one intent
  -> score actions/tasks for that intent
  -> continue or update small execution state
  -> execute one concrete step
```

Avoid hidden sub-selectors like `Adventure -> idleGoal -> travel flags`. If the bot may resupply, explore, grind, gather, loot, or hang out, those should be direct intent candidates in the same weighted intent pass.

The controller still needs:

- a continuous party participation decision;
- a standing positioning plan;
- an explicit fight pressure model;
- cleaner execution state for multi-step tasks;
- clear reasons for idle, impossible, blocked, or unsafe actions.

## Top-Level Architecture

Friend mode should own the top-level action tick.

Conceptually:

```cpp
if (IsFriendMode())
{
    friendController.RunTick(minimal);
    return;
}

currentEngine->DoNextAction(...);
```

There should be no implicit fallback to normal bot behavior. If friend mode decides to idle, that is the chosen result for that tick.

Friend mode does not need a separate high-frequency reactive action system. The controller should evaluate before starting an action, after an action finishes, and after an action is cancelled or fails. That is enough for the desired behavior. Existing urgent reaction plumbing can remain separate where already required by the broader bot system.

Normal bots continue using the existing engine unchanged.

## Main Layers

Friend mode should be built as a small stateful controller with these layers:

```text
Mode / Command
  -> Situation Snapshot
  -> Positioning Plan
  -> Intent Scoring
  -> Action / Task Scoring
  -> Execution State
  -> Execution
```

Each layer should produce simple, inspectable outputs for debugging.

## Mode, Commands, And Intent Weights

Friend bots separate long-lived social policy from temporary explicit commands and current task execution state.

Modes:

- `party`: default social mode; stay useful near the human leader while allowing light nearby activity.
- `dungeon`: strict party mode; no optional wandering, pulling, or town chores.
- `solo`: looser autonomy; may travel, grind, gather, or shop more independently.

Commands:

- `friend`: enable friend mode and default to `party` mode with no temporary command.
- `normal` / `reset`: clear temporary commands and return to party mode.
- `come` / `come here`: temporarily return to the leader.
- `stay close`: keep a tighter social leash.
- `stop` / `hold` / `dont move`: hold position.
- `rest`: recover health and mana.
- `shop` / `town` / `resupply`: temporarily prioritize town chores, then clear back to the current mode.
- `attack`, `heal`, `buff`: short tactical pushes without changing mode.

Modes and commands should mostly change intent weights. They should not create a second hidden behavior system.

Examples:

- `party` boosts return, help, loot, light grind, light gather, light explore, and hang-out behavior.
- `solo` boosts grind, gather, explore, resupply, and independent travel.
- `dungeon` suppresses casual explore, gather, loose pulls, and town chores.
- `shop` gives `Resupply` a very high temporary weight.
- `attack` gives combat and pull intents a high temporary weight.
- `heal` gives healing intents a high temporary weight.
- `come` and `stop` are hard overrides, not just weights.

Non-combat activities such as `Resupply`, `Explore`, `Grind`, `Gather`, `LootNearby`, and `HangOut` are top-level intents. They should not live inside a separate `Adventure` sub-selector.

The human party leader/master should usually be the strongest social anchor. Friend bots can still care about the tank, healer, and party cluster, but when in doubt they should prefer staying useful to the human leader over optimizing around an inferred role layout.

Town chores should stay friend-owned at the decision level. Reuse direct old actions like `repair`, `sell`, `buy`, and travel target movement where useful, but do not enable the broad old RPG/vendor/maintenance strategy stack as a friend-mode fallback.

First-pass town chores:

- repair gear when a repair NPC is nearby;
- sell vendor/trash items when a vendor is nearby;
- buy useful vendor supplies such as food, water, ammo, and reagents;
- use travel targets for vendor/repair only when explicitly commanded with `shop`/`town`/`resupply` or when solo mode is allowed to act independently;
- loosen the party leash in capital/town areas, but keep dungeon mode strict.

## Party Participation

Being in a party does not always mean every bot should immediately abandon its current task for every party fight.

Friend mode should compare:

```text
current_task_value
vs
party_help_value - travel_cost - abandonment_cost
```

Examples:

- A bot executing a `Resupply` town task should ignore routine party grinding.
- The same bot should return if the master, tank, or healer is in serious danger and return is realistic.
- A bot exploring nearby should finish or escape a local 1v1 before trying to help distant party combat.
- In dungeons, the threshold to rejoin/help should be much lower and most splitting should be disallowed.

Useful inputs:

- distance and estimated travel time to master/main party;
- same map, same instance, or unreachable;
- number of party members in combat;
- lowest party health;
- party leader/master danger;
- likely tank/healer danger when inferable;
- local danger around the bot;
- current command/task interruptibility;
- time already spent away;
- dungeon/world/battleground context.

Use stickiness: selected intents/tasks should not flip every few seconds because the party tagged another mob.

## Situation Snapshot

Each tick, friend mode should build a compact snapshot from existing values plus new friend-specific summaries.

Suggested fields:

- bot health, mana, cooldown posture, movement state;
- recent bot health/mana deltas;
- current mode, command, selected intent, and execution state;
- local attackers and attackers targeting the bot;
- main party cluster and local cluster;
- party lowest HP and number of damaged members;
- party leader/master state;
- likely tank/healer state when inferable;
- recent party health deltas and projected short-term danger;
- fight pressure: none, low, medium, high, critical;
- dungeon/world context;
- current target and expected time-to-die estimate;
- reachable helpful targets;
- resource pressure: abundant, normal, conserve, exhausted;
- whether movement is allowed by command and context.

This snapshot should be the input to positioning and intent selection. Avoid spreading party-state decisions across unrelated action classes.

## Positioning Plan

Friend mode needs a continuous positioning layer. Existing movement actions and stance values are useful, but they are mostly action/prerequisite driven. Friend mode should maintain "where I should be" independently of which spell was chosen this tick.

Suggested posture states:

- `MeleeCommitted`
- `RangedCommitted`
- `CloseSupport`
- `LooseFollow`
- `ReturnToParty`
- `HoldStill`
- `Recovering`

The positioning plan should evaluate:

- anchor: master, tank, party center, current target, assigned hold point;
- leader weighting: prefer the human leader/master as the default anchor unless command, selected intent, execution state, or immediate danger says otherwise;
- preferred range band;
- maximum leash distance;
- whether the bot is ahead of the tank/master;
- whether adds or hostile clusters are nearby;
- whether current position risks pulling more enemies;
- whether current action intent requires repositioning;
- whether movement is forbidden by command.

Movement should have hysteresis. Do not move just because a position is slightly better. Move when:

- outside acceptable range band;
- in immediate danger;
- line of sight/range blocks important intent;
- command requires return/hold/close behavior;
- dungeon safety requires falling back.

Movement safety is a hard invariant. Friend mode should generate candidate positions, validate pathing/ground/height/leash/hazard safety, and only then move. Do not use blind vector movement for flee/spread/reposition decisions. If no safe candidate exists, hold position, return toward the leader, or choose a non-movement action.

This layer should prevent oscillation like running into melee for one swing, then running out for a spell.

## Intent Ranking

Friend mode should choose an intent first, then pick the best ability/action for that intent. This is better than flat-scanning every spell as an independent top-level action.

Intent selection should be a single weighted pass:

```text
mode weights
+ command weights
+ situation weights
+ short-term memory penalties/bonuses
=> ranked intents
```

Do not add a nested `Adventure` intent that then picks its own hidden goal. If the bot might resupply, explore, grind, gather, loot, or hang out, those are direct intent candidates in the same ranking pass as combat/support intents.

Initial intents:

- `ReturnToParty`
- `HoldPosition`
- `SaveSelf`
- `SavePartyMember`
- `GetToSafety`
- `StabilizePosition`
- `Interrupt`
- `ControlAdd`
- `AssistTank`
- `DealDamage`
- `ConserveDamage`
- `Pull`
- `RecoverResources`
- `Buff`
- `Cure`
- `TradeSupport`
- `LootNearby`
- `Resupply`
- `Gather`
- `Grind`
- `Explore`
- `HangOut`
- `FollowOrIdle`

Intent ranking should account for:

- mode, command, and execution state;
- fight pressure;
- party health and role needs;
- local threat;
- resources;
- dungeon safety;
- current posture;
- target state;
- recent actions and cooldowns.

Examples:

- Safe mage, full mana, high pressure: `DealDamage` can rank high.
- Mage in low-pressure grind with low mana: `ConserveDamage` or wand can outrank expensive spells.
- Healer at half mana, low pressure, party stable: wand/melee/idling can be acceptable.
- Healer with party damage: `SavePartyMember` outranks damage.
- Bot with threat in dungeon: `GetToSafety`/`StabilizePosition` outrank optional damage.

Roles should be soft. A bear druid may hold aggro for a while, a priest may briefly get threat while healing, and a hunter may pull when the leader is safe and nobody else has a good pull. Friend mode should not panic just because threat is imperfect. It should respond to actual danger, pressure, and party state.

## Ability And Action Selection

After choosing an intent, select the best concrete ability or action.

Use a hybrid model:

- generic category processors for common behavior;
- per-ability metadata for details that matter.

Suggested ability metadata:

- action name;
- category: direct damage, dot, heal, hot, shield, cure, buff, interrupt, cc, escape, item, wand, melee;
- preferred target type;
- preferred range band;
- movement policy;
- mana/resource cost;
- cooldown or reuse delay;
- cast time;
- school or lockout group;
- threat risk;
- dungeon safety;
- minimum target time-to-live;
- pressure threshold;
- posture compatibility.

Examples:

- Dots require enough target lifetime.
- Fear is heavily restricted in dungeons.
- Wand is cheap and good during low pressure or low mana.
- Melee is fallback for casters unless already close or committed.
- Expensive cooldowns require high pressure or command permission.

Avoid unique evaluator logic for every spell unless the ability is genuinely special.

## Action Reuse

Reuse existing actions as execution primitives where they behave well:

- spell casting actions;
- heal/cure/buff party actions;
- food, water, potion, healthstone, mana gem, bandage;
- give food/water and trade actions;
- loot actions;
- follow, move, reach, flee, set facing;
- target and party values.

Do not reuse the existing action relevance score as friend mode's main evaluation metric.

Existing actions generally answer:

```text
Can I do this exact thing now?
```

Friend mode also needs to answer:

```text
Should I try to make this possible?
Should I move first?
Should I wait?
Is this unsafe in this context?
Is this compatible with my current mode, command, selected intent, execution state, and posture?
```

Some existing actions may need friend-specific wrappers or replacements where they silently fail, move poorly, or hide important reasons from the controller.

## Execution State

Execution state exists only to carry a chosen multi-step task across ticks. It is not another intent selector.

It should answer:

- what task is currently being executed;
- what concrete target or point it is using;
- what phase it is in;
- when it started and when it expires;
- why it last failed or deferred;
- whether it can be interrupted by a higher-priority intent or command.

Examples:

- `Resupply`: chosen vendor/repair NPC or town target, phase `choose target -> move near vendor -> sell/repair/buy -> done`.
- `Explore`: chosen safe waypoint or area, phase `move -> look around -> done`.
- `Grind`: chosen nearby target or grind area, phase `pick safe target -> pull/attack -> recover if needed`.
- `Gather`: chosen object/node, phase `move near -> gather/loot -> done`.

Execution state prevents thrashing. Without it, a bot may request a vendor, move three yards, re-roll `HangOut`, clear travel, then request a vendor again. With it, the bot can remember "I am doing Resupply at Vendor X" while still allowing urgent intents like `SaveSelf`, `HealParty`, `Come`, or `Stop` to interrupt.

Execution state should be small and explicit. It should not own broad policy, mode behavior, party participation, or intent priority. Those belong to the main intent scoring pass.

## Execution Results

Friend mode should track richer execution results than ok/impossible/useless/failed.

Suggested result categories:

- `Done`
- `StartedMovement`
- `StartedCast`
- `IntentionalIdle`
- `BlockedNoMana`
- `BlockedCooldown`
- `BlockedRange`
- `BlockedLineOfSight`
- `BlockedTarget`
- `Unsafe`
- `ForbiddenByCommand`
- `FailedUnknown`

These results should feed diagnostics and short-term decision memory.

## Resource Model

Resource use should depend on fight pressure, mode, command, selected intent, and execution state.

Suggested resource modes:

- `Conserve`: low pressure, routine grinding, low mana.
- `Normal`: ordinary party fighting.
- `Burn`: hard fight or party danger.
- `Emergency`: survival/wipe-prevention.

Examples:

- Low pressure: cast a spell or two, then wand/melee/natural regen.
- Medium pressure: normal class kit.
- High pressure: burn mana and use strong abilities.
- Critical pressure: potions, healthstones, cooldowns, emergency heals.

Do not make mana-saving a static class rule. Tie it to party pressure, current mode, and selected intent.

Pressure should include trends, not just current values. A tank at 70% HP and falling fast is very different from a tank at 70% HP and stable. Track recent HP/resource deltas and use them to softly project near-future danger.

Suggested pressure inputs:

- current HP and mana;
- recent HP loss rate;
- incoming damage estimate where available;
- number of damaged party members;
- whether the leader/master is taking damage;
- whether the bot has threat;
- expected target time-to-die;
- elite/boss classification or unusually long fight duration.

## World PvE, Farming, And Elites

Friend bots should be useful for ordinary world PvE and farming without becoming overly cautious.

Routine farming behavior should support this rhythm:

- stay near the leader or party cluster;
- help tag or finish mobs;
- use cheap or varied class abilities;
- heal/buff when needed;
- recover after fights when the group is not ready;
- avoid chain-pulling when the party is injured or drinking;
- loot opportunistically when safe and allowed.

Pulling should be conservative but not forbidden. If the leader is healthy, nearby, and not already under pressure, a mage/hunter/warlock or similar bot can pull a reasonable target. Avoid pulling when the party is split, low HP, low mana, in a dungeon, or near dense hostile packs.

Elites and longer fights do not need a fully separate system at first. They should naturally raise fight pressure because targets live longer, party HP moves more, threat matters more, and resource burn becomes more valuable. If this is not enough, add a small elite/long-fight pressure modifier before adding encounter-specific logic.

## Dungeon Safety

Dungeons need stricter defaults:

- no casual exploring;
- no casual pulling unless assigned;
- no fear or risky displacement unless explicitly safe;
- no long chase into unknown packs;
- stay behind/near tank or master;
- return to party aggressively;
- avoid looting/trading during active danger;
- prefer stability over style.

World PvE can be looser and more expressive.

## Class And Capability Model

Start capability-driven, then add light class profiles.

The evaluator should ask what the bot can do based on spells/items. Class profiles should define what is sensible and flavorful.

All classes need baseline support from the first implementation because friend mode has no normal-engine fallback. The baseline does not need to be excellent, but it must prevent "stand there doing nothing" behavior.

Baseline for every class:

- follow/return/hold commands;
- defend self;
- assist leader or likely tank target;
- use at least one sensible damage path;
- use basic survival items or defensive tools when available;
- recover health/mana when safe;
- avoid unsafe pulls and dungeon wandering;
- expose diagnostics when no useful action is available.

Class flavor should then improve how each class uses its kit:

- Priest: healing, shielding, renew, dispel, fort/spirit, wand/cheap damage.
- Mage: ranged damage variety, decurse, armor, intellect, conjure/give water/food, mana gem, escape/control.
- Warlock: dots, bolts, pet support, healthstone/soulstone, fear restrictions, life tap/dark pact resource behavior.
- Hunter: ranged attacks, pet management, traps/utility where safe, conservative pulling.
- Paladin: blessings, off-heals, defensive tools, melee support, cleanse.
- Druid: forms, off-heals, buffs, ranged/melee flexibility, bear/cat/caster posture.
- Shaman: shocks, totems, heals, weapon imbues, ranged/melee flexibility.
- Rogue: melee positioning, interrupts, stuns, poisons, avoid bad pulls.
- Warrior: melee pressure, taunt/defensive help, stance-aware but not strict tank-only behavior.
- Death Knight: melee pressure, grips only when safe, defensive cooldowns, diseases.

The first pass can be simple across all classes, then tuned class by class.

## Diagnostics

Friend mode needs built-in reporting from the start.

`friend ?` should report:

- current mode/command/execution state;
- party participation decision;
- fight pressure;
- posture and desired anchor/range;
- top ranked intents;
- selected action;
- last execution result;
- top rejected reasons.

This is essential for tuning. Without it, failures will look like random idling.

A separate debug toggle should print full intent/action weights. This must be separate from normal `intent` or `debug` reporting because candidate weights are noisy. Suggested levels:

- `silent`: no routine reporting.
- `intent`: selected intent/action changes only.
- `debug`: selected result plus compact situation details.
- `weights`: top intent weights and top action/task weights for the selected intent.

## Risks And Mitigations

### No-Fallback Coverage

Risk: because friend mode owns the tick, unsupported classes or missing metadata may idle.

Approach: implement the all-class baseline before detailed class flavor. Every class must be able to follow, assist, do basic damage, recover, and report why it is idle.

### Assignment Loops

Risk: bots may never finish errands, or may ignore the party too long while away.

Approach: every multi-step task needs start conditions, completion conditions, failure conditions, max duration, interrupt threshold, and return behavior.

### Split Party Confusion

Risk: one explorer, one town bot, and the main group can distort the party pressure model.

Approach: track local cluster, leader/master cluster, and main party cluster separately. Weight the human leader/master heavily.

### Movement Jitter

Risk: constant position evaluation can interrupt useful casting or cause small pointless corrections.

Approach: use posture stickiness, movement thresholds, and action windows. Do not move for a slightly better position. Prefer finishing reasonable casts unless danger or command changes.

### Silent Action Failure

Risk: existing actions may only report useless/impossible/failed, hiding range, mana, target, or safety reasons.

Approach: add friend-specific evaluation wrappers and richer result categories before replacing actions. Replace only actions that repeatedly hide important state or move badly.

### Dungeon Mistakes

Risk: friend bots may pull extra packs, chase runners, break CC, or use fear badly.

Approach: strict dungeon policy by default: stay near leader/tank, no casual exploring, no risky pulls, no fear unless safe/allowed, no long chase, no AoE into CC.

### Farming Stalls

Risk: bots may become safe but not useful during repeated world PvE.

Approach: add a routine farming rhythm: follow leader, help tag/kill, recover only when needed, allow conservative pulls when leader and party are safe.

### Overly Sweaty Roles

Risk: the controller may become a rigid tank/heal/DPS optimizer instead of a natural friend bot.

Approach: treat roles as soft hints. Prioritize actual danger, leader state, class kit, and pressure over perfect threat tables or strict rotations.

### Elite And Long-Fight Tradeoffs

Risk: chill behavior may underperform in elites or hard encounters.

Approach: let pressure rise from HP deltas, long target lifetime, elite status, and resource strain. Burn stronger tools under high pressure, but keep the behavior understandable and not encounter-scripted unless necessary.

## Current Cleanup Direction

These notes reflect the direction after testing the first friend controller passes.

Keep:

- friend mode owns the top-level tick and returns without normal-engine fallback;
- `FriendSituation` as the compact snapshot;
- ability catalog and metadata-based action selection;
- existing spell/item/loot/vendor actions as low-level executors where they behave;
- soft roles, pressure-based resource use, and leader-weighted party behavior.

Clean up:

- remove the hidden `Adventure`/`idleGoal` decision layer as a behavior owner;
- promote `Resupply`, `Explore`, `Grind`, `Gather`, `LootNearby`, and `HangOut` into top-level weighted intents;
- make modes and temporary commands only adjust intent weights, except true hard overrides such as `stop` and `come`;
- replace scattered booleans such as "resupply travel requested" and "idle travel requested" with one small execution-state record;
- avoid using legacy travel target state as the friend task owner. It can be reused as a path/target provider, but friend mode should own the task phase and completion/failure reasons;
- add a separate `weights` debug toggle for intent/action score dumps;
- keep friend-specific movement consistent with the same task/execution-state model.

The next architecture cleanup should make the tick read roughly:

```text
BuildSituation
ScoreIntents(mode, command, situation, executionState)
SelectIntent
ScoreActionsForIntent(intent, situation, executionState)
ExecuteSelectedActionOrTaskStep
UpdateExecutionState
ReportResult
```

If a selected intent cannot produce a useful action, it should return a clear rejected reason and let the next tick re-score. It should not silently fall into a separate old strategy path.

## Implementation Principle

Friend mode should be a readable controller with explicit state and explicit reasons. It should not become a second invisible strategy soup.

When behavior is wrong, we should be able to ask:

```text
What mode, command, selected intent, and execution state are active?
What did the bot think the party state was?
Where did it want to stand?
What intent won?
Which ability/action did that intent choose?
Why were alternatives rejected?
```

If those questions are easy to answer, friend mode will be tunable.
