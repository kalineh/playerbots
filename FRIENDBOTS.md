# Friend Bots Design Plan

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

## Current Problem

Current friend mode is mostly a modifier over the existing strategy engine:

- `FriendStrategy` adds small random relevance changes and heal priority nudging.
- `EnableFriendMode()` applies many existing class/spec/role strategies.
- `Engine::addStrategy()` skips sibling-strategy removal while friend mode is active.

This creates more variety, but it is still the same one-action-per-tick priority engine. The existing model is good for "perform this role/job" behavior, but weak for friend-like behavior because it lacks:

- a persistent adventure goal;
- a continuous party participation decision;
- a standing positioning plan;
- an explicit fight pressure model;
- intent-level reasoning before ability selection;
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
  -> Adventure Goal / Party Participation
  -> Situation Snapshot
  -> Positioning Plan
  -> Intent Ranking
  -> Ability / Action Selection
  -> Execution
```

Each layer should produce simple, inspectable outputs for debugging.

## Mode, Command, And Goal Layer

Friend bots separate long-lived social policy from temporary explicit commands and current idle/adventure goals.

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

Goals are the current non-urgent activity, such as `resupply`, `loot`, `recover`, `orbit`, `loiter`, `gather`, or `grind`. Commands can force or heavily weight a goal, but goals should clear naturally when complete.

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
current_goal_value
vs
party_help_value - travel_cost - abandonment_cost
```

Examples:

- A bot on a `resupply` town goal should ignore routine party grinding.
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
- current command/goal interruptibility;
- time already spent away;
- dungeon/world/battleground context.

Use stickiness: goals should not flip every few seconds because the party tagged another mob.

## Situation Snapshot

Each tick, friend mode should build a compact snapshot from existing values plus new friend-specific summaries.

Suggested fields:

- bot health, mana, cooldown posture, movement state;
- recent bot health/mana deltas;
- current mode, command, and goal;
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
- leader weighting: prefer the human leader/master as the default anchor unless command, goal, or immediate danger says otherwise;
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

Initial intents:

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
- `FollowOrIdle`

Intent ranking should account for:

- mode, command, and goal;
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
Is this compatible with my current mode, command, goal, and posture?
```

Some existing actions may need friend-specific wrappers or replacements where they silently fail, move poorly, or hide important reasons from the controller.

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

Resource use should depend on fight pressure, mode, command, and goal.

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

Do not make mana-saving a static class rule. Tie it to party pressure and current mode/goal.

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

- current mode/command/goal;
- party participation decision;
- fight pressure;
- posture and desired anchor/range;
- top ranked intents;
- selected action;
- last execution result;
- top rejected reasons.

This is essential for tuning. Without it, failures will look like random idling.

## Risks And Mitigations

### No-Fallback Coverage

Risk: because friend mode owns the tick, unsupported classes or missing metadata may idle.

Approach: implement the all-class baseline before detailed class flavor. Every class must be able to follow, assist, do basic damage, recover, and report why it is idle.

### Assignment Loops

Risk: bots may never finish errands, or may ignore the party too long while away.

Approach: every command/goal needs start conditions, completion conditions, failure conditions, max duration, interrupt threshold, and return behavior.

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

## Phased Implementation

### Phase 1: Mode Ownership

- Add a friend controller owned by `PlayerbotAI`.
- Hook friend mode before the normal engine and return unconditionally.
- Change friend activation so it no longer applies every candidate strategy as the main behavior.
- Keep normal bots untouched.
- Preserve `strict`/disable behavior.

### Phase 2: Snapshot And Diagnostics

- Build `FriendSituation`.
- Add basic `friend ?` reporting.
- Include mode, command, goal, party pressure, HP/resource deltas, local danger, resources, posture, and last action result.

### Phase 3: Positioning

- Add `FriendPositioning`.
- Implement `stay close`, `come`, `hold`, and ranged/close-support posture.
- Use existing movement actions where acceptable.
- Add friend-specific movement helpers only where existing actions hide too much or cause bad movement.

### Phase 4: Intent Selector

- Implement initial intents:
  - `SaveSelf`
  - `SavePartyMember`
  - `GetToSafety`
  - `StabilizePosition`
  - `DealDamage`
  - `ConserveDamage`
  - `RecoverResources`
  - `Buff`
  - `FollowOrIdle`
- Add fight pressure and resource mode.

### Phase 5: Ability Selection Baseline

- Implement ability metadata and generic category processors.
- Add baseline ability catalogs for every class.
- Reuse existing actions for execution.
- Add result tracking for blocked/unsafe/idled cases.

### Phase 6: Assignments And Splitting

- Add richer explore, scout/pull, and town errand goals.
- Add party participation scoring and interruption thresholds.
- Add dungeon restrictions and leash rules.

### Phase 7: Polish And Expansion

- Add useful trading/resource support.
- Add opportunistic loot outside danger.
- Tune class profiles and class flavor.
- Remove or narrow old friend-mode strategy side effects and sibling-strategy bypass once no longer needed.

## Implementation Principle

Friend mode should be a readable controller with explicit state and explicit reasons. It should not become a second invisible strategy soup.

When behavior is wrong, we should be able to ask:

```text
What mode, command, and goal are active?
What did the bot think the party state was?
Where did it want to stand?
What intent won?
Which ability/action did that intent choose?
Why were alternatives rejected?
```

If those questions are easy to answer, friend mode will be tunable.
