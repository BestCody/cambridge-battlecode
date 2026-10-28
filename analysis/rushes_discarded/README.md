# Team rushes on hard camps (discarded)

Tried on `bots/baseline` on 2026-10-28, after commit `1fd6d8f` (the altar-scarcity change). Reverted without committing.

## The idea

When a champion picks a hard camp group (golem or monolith, `hard_camp_health` or more), it becomes the leader and calls a **rush**: every teammate drops its own camp and goes to that group. The party gathers out of reach, strikes together, and the leader ends the rush once the group is cleared.

## How it was built

- **Shared memory.** A new claim code, `rush(group)`, sat above the formation and joining codes in `TeamMemory`. The layout didn't change. Only the leader wrote it, into its own slot. If two champions called at once, the lower slot won. `camp_claim()` read a rush code as a claim on its group, so the existing party maths for hard camps counted the whole team.
- **Followers** took the rush group as their target with no re-planning. A champion holding a capture finished it first. Fights, retreats and tributes still came first in the turn order.
- **Gathering.** The leader waited until every teammate claiming the group was in sight, up to `rush_gather_rounds`. Followers waited for the leader's first strike.
- **Ending.** The rush ended when the group was cleared, after `rush_max_rounds`, or when the leader died or was walled off. Followers kept a call they couldn't read for `rush_memory` rounds, which covered the leader briefly reporting a fight in its slot.
- **Calling.** A rush needed the camp's score to be `rush_score_ratio` times the caller's next-best option, at least `rush_min_alive` living champions, and `rush_cooldown` rounds since the caller's last rush.

## Results

`scripts/evaluate.py`, baseline against test-bot, 70 maps, both seats, fuel 100000, 140 games per version:

| version | settings | wins | gems ours / them |
|---|---|---|---|
| **norush** | rushes off | **107/140 (76%)** | 122764 / 73375 |
| current | ratio 1.0, gather 40, max 150 | 81/140 (58%) | 107567 / 88155 |
| strict | ratio 3.0 | 99/140 (71%) | 120847 / 77490 |
| short | gather 15, max 50 | 99/140 (71%) | 120359 / 80116 |
| strictshort | ratio 3.0, gather 15, max 50 | 102/140 (73%) | 123698 / 75271 |

Noise is about ±6 wins in 140 games. The full rush lost 26 wins. Narrowing it cut the loss but never beat having no rush.

## Why it likely failed

These are guesses from one diagnostic game (`00monolith`) and the sweep above. None of them was tested directly.

- **Nobody farms during a rush.** Two rushes in the test game ran 134 and 151 rounds. The whole team stood at one camp for most of that time while the enemy farmed the rest of the map unopposed.
- **The existing two-champion party already handles hard camps.** Sending the other three adds little damage and costs a lot of farming.
- **Followers arrive one at a time** across the map, which exposes them to enemies on the way.

## If someone revisits this

- Make it **"enough bodies", not "everybody"**: ask for the squad size the camp's clear actually needs, the way `formation.cpp` sizes fight requests.
- Limit it to monoliths, or to fortress-gem maps (`fortress_gems()`), where a hard camp is the only source of gems.
- Compare against `norush` with `evaluate.py` before keeping any version.
