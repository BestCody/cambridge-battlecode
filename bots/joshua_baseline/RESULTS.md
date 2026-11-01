# joshua_baseline

`baseline` plus map casework and champion roles. Every rule has a constant in
`src/strategy.h`.

## Rules

| Rule | When | What |
| --- | --- | --- |
| Boss gems | Map has a golem or monolith | Boss gems weigh 1.6x in camp choice while the living team could take the kill for at most 60% of its spare health |
| Keep together | Compact map (gem groups 5 tiles or less from their nearest on average, at least 6) with a boss | Camps far from teammates' claims lose 0.05 value per tile |
| Priority targets | Enemy Prism or Willow | Worth 1.5x as a target, chased from 1.5x the health |
| Vesper scouts | First 300 rounds | Gold is worth 0.5 gems to Vesper Vale (others about 0.18), and it keeps no company |
| Vesper teleports | Detour map (a camp's walk from spawn at least 1.6x its straight line, camps 8+ tiles away) | Vesper always carries a teleport crystal |
| Vendetta + Marcella | Five-champion teams | Each one's camp choice is pulled toward the camp the other claims (0.1 per tile past the camp spacing) |

## Results (fuel 100,000, 70 eval maps x 2 seats)

| Bot | vs opp_testbot (frozen test-bot) |
| --- | --- |
| **joshua_baseline** | **90/140, margin +22,598** |
| baseline | 85/140, margin +19,488 |
| macro (casework only, no roles) | 85/140, margin +20,364 |

Single 140-game runs swing about +/-6 wins, so +5 is promising, not proven.

Over 700 games (five fuel budgets), the casework alone (`macro`) won 446/700
against opp_testbot to baseline's 432/700, and 358/700 head to head against
baseline. Its gains were on boss maps: compact boss maps 41/70 (baseline 31/70),
spread boss maps 196/250 (baseline 189/250). Keeping together on compact maps
*without* a boss lost (34/80 against 40/80), so that rule needs a boss.
