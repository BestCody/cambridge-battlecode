# Design notes

## Current team plan

newest-bot replicates the play of Intercontinental Fish, the #1 ladder team, as seen in their match summaries. The earlier squad bot is archived as `bots/squad-bot` (tag `squad-bot-final`).

| 3 picks | 4 picks | 5 picks |
| --- | --- | --- |
| Stellar, Heart of Ruin, Ren | + Twins | + Twins, Ryth |

Rosters live in `draft/team_compositions.h`. The squad controller is off (`policy::squad_enabled`), so every champion runs the worker policy:

- **Farm near each other.** Like the top team, champions farm neighbouring camps rather than spreading out, so help is within reach when a fight starts.
  - Camp value is divided by 1 + `cohesion_weight` × (tiles beyond `cohesion_radius` (8) to the nearest teammate's claimed camp or position) ÷ 8.
  - The weight is 1. Weights 0.5 and 2 measured worse.
- **Farm and fight.** Champions farm their own side and fight when a fight comes to them. Heart of Ruin and other dash champions use damaging dashes on enemies and camps (`impact_dash`).
- **Bank small bags often.** Carries (Stellar, Heart of Ruin) bank at 28 gems, everyone else at 20. Anyone passing within 2 tiles of an altar banks 10 or more.
- **Shop on a fixed build.** Carries go home with 10 shards' worth of gold, others with 5. Each champion buys Fish's build order (`mechanics/economy.cpp`). No altar locks: Fish never buys them.
- **Siege only when it works.** Stellar and Cassi have range-4 spells, and camps strike at range 3. A champion sieges, standing 4 tiles out where only its spells reach, only when all three hold:
  - a solo clear would cost too much health;
  - a tile out of every camp's reach exists;
  - no ally is closer to the camps, since camps attack the nearest champion.

  Otherwise it attacks from normal range. Before this rule, Stellar stood still for 55% of its camp turns waiting for siege tiles that didn't exist.
- **Retreat late, fight back when caught.** Heart of Ruin retreats at 15% health, the others at 28%. A low-health champion an enemy can reach next turn fights back instead of fleeing (`policy::stand_when_caught`).
- **Teleport crystal shortcuts** (`mechanics/crystal.cpp`). Each champion builds a hop map once per map from spare end-of-turn fuel. The hop map holds the turns from spawn to each tile by walking, and by walking plus one 3-tile hop.
  - **At spawn:** after buying shards, a champion buys a crystal only if one hop toward its camp or altar saves at least `policy::crystal_min_saving` (8) turns, after counting the 3-turn channel and the landing turn.
  - **On the way:** a champion carrying a crystal hops wherever doing so saves 8 or more turns, but only when no visible enemy could stun it during the channel.

  The threshold comes from the crystal's cost (10 gold, about 3 turns of a champion's income), not from the map pool.

### Measured (35 maps, both seats)
| Change | Against db10/db11/db12 | Against squad-bot |
| --- | --- | --- |
| Fight back when caught | 76/210 wins (was 60), margin −181/game (was −271) | 57/70 |
| Teleport crystals (3 fuel budgets) | 198/630 wins (was 197), margin −291/game (was −292) | 172/210 (was 169) |
| Camp choice before fuel runs out; runtime engine settings (3 fuel budgets) | 231/630 wins (was 198), margin −189/game (was −291) | 188/210 (was 172) |
| Siege only where possible and when no ally is closer (3 fuel budgets) | 283/630 wins (was 231), margin −70/game (was −189) | 202/210 (was 188) |
| Farm near teammates, cohesion weight 1 (1 fuel budget) | 101/210 wins (was 95), margin +32/game (was −84) | 63/70 (was 68) |

Crystals fired on 14 of 35 maps. There they won 151/336 (was 137) and improved the gem margin by 51 a game. On the other maps the results only show noise.

### Fuel and engine settings
- **Choose a camp first.** A champion without a camp chooses one right after observing, before the rest of the turn spends the fuel. With every champion in view, observation once cost 55k of the 100k budget. Camp choice, which needed 50k left, then never ran, and the team idled in spawn all game on `00reverse-spiral`.
- **Cheaper observation.** Visible champions come from the known rosters instead of the engine's per-team vision list (3,700 fuel a call), and each champion type's fixed kit is cached (about 550 fuel per champion seen). With all ten champions in view that saves roughly 12k a turn.
- **Never use `bc::constants` for engine settings; use `rules::settings()`.** The SDK copies the settings into `bc::constants` during static initialisation, in an order that varies between builds. In some builds the copies read the settings before they were loaded and stayed 0. With `winning_gems` at 0, every bag looked like the winning deposit, so champions banked single gems. Only `bc::constants::passive_abilities` are compile-time and safe.

### Evaluating changes
Use `scripts/evaluate.py` against several opponents, both seats and several fuel budgets. Single games are deterministic but chaotic, so one game per map proves nothing. Judge on the gem margin, not just wins.

---

## Squad controller rules

The rest of this document specifies the Stellar/Tavi/Cassi squad controller in `bots/newest-bot/src/squad/`. It is switched off in the replica (`policy::squad_enabled`) and plays in full in `bots/squad-bot`. Each rule below names the code that implements it.

- **Done** means the rule is implemented.
- **Partial** means a simpler version of the rule is implemented, and the gap is described.
- **Open** means the rule is not implemented.

These rules were only ever match-tested as a whole, as part of the squad bot. Two of them, the secondary-stun bonus and the capture-reset limit, never changed a game in testing. Validate any tuning with `scripts/evaluate.py`.

Terms used throughout:

- **Reward time**: rounds until the camp's reward is captured (travel + clear + occupancy + capture timer).
- **Interference**: the earliest round at which an enemy champion could stand on the capture tiles.
- **Material event**: a member's death, a new relevant enemy, a lost capture timer, or a terrain/obstacle change.

## 1. Central contest selection

**1.1 Rank by contest first, then by gems.** The coordinator groups jobs into three categories and takes the best-rated job from the first non-empty one:

1. Contested gem camps: the enemy's terrain distance is at most our home distance + 2 × the map's camp spacing.
2. Other gem camps.
3. Gold-only camps.

The current job keeps `commit_bonus` only against jobs in the same category. *Done*: `select_squad_job`, [selection.cpp](../bots/newest-bot/src/squad/selection.cpp).

**1.2 Leave home camps to workers.** A camp that a living worker (Issac, Ryth or Varg) currently claims is skipped during ranking. The exceptions are a contested camp and the squad's own current camp. Gold camps reach the squad only as a preparation detour (1.4) or when no gem job exists. *Done*: ranking loop in `select_squad_job`.

**1.3 Price interference from enemies we cannot see.** A hidden enemy that (re)spawned at round `s` can be no closer to a camp than `enemy_distance - (round - s)`. While that bound is positive, the overlap with our reward time is added to the job's interference work, scaled by `policy::hidden_interference_weight` (0.5). Once the bound reaches zero the enemy could be anywhere, so it stops contributing; counting it would only tax every job equally. Visible enemies are still timed from their real positions. *Done*: `hidden_interference` in [coordination.cpp](../bots/newest-bot/src/squad/coordination.cpp), and stages 2–3 of `estimate_job` in [objectives.cpp](../bots/newest-bot/src/squad/objectives.cpp).

**1.4 Detour home only if it pays for itself.** A gold or upgrade detour replaces a contested siege job only when the clear time it saves exceeds the detour (reward + recall + walk back) × `commit_bonus`. *Done*: existing preparation block in `select_squad_job`.

**1.5 Clear immediately when uncontested.** If the plan's interference is later than its reward time, and no new relevant enemy has appeared since the plan was made, the squad clears without waiting for every member to reach its firing tile. Otherwise, while a relevant enemy is visible, members hold their cooldowns for denial until the squad is in position. *Done*: clear gate in `squad_resource_turn`, [stellar_tavi_cassi.cpp](../bots/newest-bot/src/squad/stellar_tavi_cassi.cpp).

**1.6 Fall back to other forward camps first.** If the selected contest is rejected, the comparison falls through to the next category before any preparation work. *Done*: follows from 1.1.

## 2. Planning and action scheduling

**2.1 Resume searches without losing work.** Ranking is cheap and covers all camps. Detailed estimates run only for `objective_finalists` (6) plus the current job. Each search keeps its cursor and best result across turns, and running out of fuel never clears a valid job. *Done*: unchanged.

**2.2 Invalidate plans on material change, not age.** A plan records the terrain revision, camp status and relevant-threat count it was made under. It is re-estimated when any of these change, or when membership changes. Age alone expires a plan only after `policy::plan_lifetime_windows` (3) work windows; it used to be 1. Calibration after each clear window still forces a refresh. *Done*: `prepare_objective` in objectives.cpp, and the new `JobEstimate` fields in [squad.h](../bots/newest-bot/src/squad/squad.h).

**2.3 Selection comes before optional work.** With no job, the coordinator runs selection before optional combat searches or ward placement, keeping `planning_reserve` back for a legal action and publication. Route caches only use leftover fuel. *Done*: unchanged ordering in `play_squad_turn`.

**2.4 Resume deferred moves first.** If last turn's move was deferred for fuel, the next turn moves before running a new estimate. The estimate still runs at the end of the turn on leftover fuel. *Done*: `resume_motion` in `squad_resource_turn`.

**2.5 Publish actual results.** `squad.motion` records moved, arrived, waiting, blocked or deferred, with a reason. Progress is credited only for real improvements (4.3). *Done*.

## 3. Combat follow-through

**3.1 Cassi controls a secondary threat.** The shared focus target is marked. A stun on the focus target earns no control credit if the allies who can still act this round will finish it anyway. The exception is a channelling target, where the stun is an interruption. A stun on a *different* enemy that threatens a member gets × `policy::secondary_control_bonus` (1.25). When control adds nothing, the combo search's best option is Cassi's damage spell. *Done*: `attack_combo` in [micro.cpp](../bots/newest-bot/src/mechanics/micro.cpp).

**3.2 Walk-stun needs a concrete payoff.** A stun cast after stepping (Cassi's walk-compatible slot) earns control credit only if one of these is true:

- the target is channelling;
- the target can kill an ally or this champion next turn;
- an ally who has not yet finished its turn can follow up on the target.

Allies who have already finished the round are excluded from that last check, because an enemy turn may come before they act again. *Done*: `walked_for_it` in `attack_combo`.

**3.3 No control without a useful window.** `control_value` is zero unless the enemy can reach a member within its own movement. Reach now uses the enemy's `move_speed`, not ours. Stun expiry, immunity and health are re-read every turn, so there is no fixed stun rotation. *Done*: [coordination.cpp](../bots/newest-bot/src/squad/coordination.cpp).

**3.4 Order turns by tier.** Next-round priority uses disjoint tiers from `priority_tier` in [strategy.h](../bots/newest-bot/src/strategy.h):

| Tier | Base | Condition |
| --- | --- | --- |
| finish | 4000 | A lethal hit, a stun on a channelling enemy, or damage on a recalling enemy |
| control | 3000 | A stun that a visible member can follow up with damage |
| damage | 2000 | Any other damage on a relevant enemy |
| idle | 1000 | No opportunity |

Value within a tier is capped at 999. Workers use the same tiers, so a worker's raw damage can no longer outrank a squad finishing blow. *Done*: `squad_priority` and `worker_priority`.

**3.5 Keep shots in place.** Ranged members that attacked cannot walk in the same turn, so members with productive shots stay put. A threatened member retreats through `health_recovery_needed`. *Partial*: there is no explicit "hold position while a teammate repositions" coordination.

**3.6 Stop pursuit when it stops mattering.** `relevant_enemy` limits combat to enemies near a member, the job's camp or the delivery altar. A sole capture holder never steps off the capture tiles to chase. *Done*: unchanged.

## 4. Continuing or abandoning a long clear

**4.1 Check reward access before committing.** Before clearing, the estimate assigns capture tiles, times the slowest member's occupancy, and simulates each visible contender against our capture-window actions. *Done*: unchanged stages 1–3 of `estimate_job`.

**4.2 Re-plan on material events.** The coordinator detects a member death, an increase in relevant threats, and a lost capture timer. Each one sets the job for review this round and invalidates the plan. Terrain obstruction is handled through 2.2. *Done*: `detect_material_events` in coordination.cpp.

**4.3 Count only real progress.** Progress means one of:

- a new best route distance for the current goals;
- our own damage on the camp;
- a held capture;
- a bank/crossing stage advancing.

Stepping away and back no longer counts, because only a new best distance does. Camp health lost while an enemy is in range of the camp is not counted, because their damage gives us no control. Reselecting the same camp never resets the deadline. *Done*: `squad_move` in [movement.cpp](../bots/newest-bot/src/squad/movement.cpp) and `observe` in [world.cpp](../bots/newest-bot/src/mechanics/world.cpp).

**4.4 Treat damage dealt as sunk cost.** Estimates use remaining health only. The clear gate no longer treats a partly damaged camp as a reason to keep clearing under threat (see 1.5). *Done*.

**4.5 Repeated capture resets.** After `policy::capture_reset_limit` (2) lost capture timers on the same job, the job loses its commitment bonus. It must then beat alternatives on its own estimate. *Done*: `select_squad_job`.

**4.6 Cooldown waits.** A wait for cooldowns counts as progress once the next ready action lands. *Open*: ranged basic attacks are available every turn, so this has not been needed.

## New policy constants

All live in `policy::` in [strategy.h](../bots/newest-bot/src/strategy.h) so `scripts/benchmark.ps1` can grid-search them.

| Constant | Default | Disables at |
| --- | --- | --- |
| `plan_lifetime_windows` | 3 | 1 (old age-only refresh) |
| `hidden_interference_weight` | 0.5 | 0 |
| `capture_reset_limit` | 2 | a large value |
| `secondary_control_bonus` | 1.25 | 1.0 |

## Archived complete rosters

These eight alternatives are kept for reference; they are not an instruction to re-enable roster selection. The live rosters are in [team_compositions.h](../bots/newest-bot/src/draft/team_compositions.h). Twins is one draft pick that produces Cassi and Issac.

| Picks | Complete roster | Intended use |
| --- | --- | --- |
| 3 | Stellar + Tavi + Twins | Ranged burst and control, with Issac available for separate resource work |
| 3 | Stellar + Tavi + Ren | Concentrated aggression against opponents approaching the squad |
| 3 | Stellar + Ren + Twins | Control and burst |
| 3 | Varg + Vendetta + Twins | Spread-out gathering and rapid camp clearing |
| 4 | Ren + Ryth + Stellar + Twins | Additional control and melee clearing |
| 5 | Varg + Ryth + Stellar + Tavi + Twins | Ranged pressure alongside strong resource clearing |
| 5 | Ren + Ryth + Stellar + Varg + Twins | Broader control and melee presence |
| 5 | Prism + Ryth + Stellar + Tavi + Twins | Concentration on exceptionally valuable gem camps |
