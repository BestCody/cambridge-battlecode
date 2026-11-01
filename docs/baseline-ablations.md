# Baseline ablations, 2026-11-01

Each `baseline` rule or emblem family was switched off on its own and played against `test-bot`: 70 eval maps in both seats (140 games), fuel 100,000. Everything ran on one frozen copy of commit afeac14.

**Reference: 81/140, margin +12,409, 4.7 kills and 8.4 deaths a game, 5,840 gold a game.**

The run was stopped early because ablation explains existing rules but does not suggest new ones. Four planned rows did not finish: `no_ghosts`, `no_rally`, `no_camp_avoid` and `no_locks`.

## How far to trust this

- Each row is one run, and a total moves by about ±6 wins from chaos alone.
- "Gained" and "lost" count games whose result changed against the reference, on the same map and seat.
- **A result can flip between code versions.** Against the code before afeac14, `no_travel_hazard` scored 63 and `flat_gold` 82. Here they score 83 and 72.
- Treat anything within about 6 wins of 81 as no effect. Re-test on other fuel budgets before adopting a change.

## Rules

| Switched off | Wins | Margin | Gained / lost | Kills | Deaths | Gold |
| --- | --- | --- | --- | --- | --- | --- |
| Recall home after a capture | **57** | −15,728 | 16 / 40 | 4.1 | 8.9 | 5,729 |
| Long-plan penalty | **64** | −6,992 | 23 / 40 | 4.3 | 9.4 | 5,401 |
| Retreat at low health | **67** | −10,722 | 15 / 29 | 4.6 | 13.1 | 5,767 |
| Higher gold value early | 72 | +12,812 | 15 / 24 | 4.9 | 8.1 | 5,785 |
| All value on gold | 73 | +2,049 | 21 / 29 | 3.9 | 8.8 | 4,443 |
| Bonus for the current target | 78 | +12,821 | 18 / 21 | 4.7 | 8.0 | 5,776 |
| Enemy-half discount | 78 | +14,714 | 16 / 19 | 4.6 | 8.8 | 6,016 |
| Chasing hidden runners | 78 | +11,048 | 10 / 13 | 4.3 | 8.3 | 5,828 |
| Reading teammates' ended claims | 79 | +11,391 | 20 / 22 | 5.1 | 8.6 | 5,931 |
| Avoiding stronger groups | 82 | +13,695 | 18 / 17 | 5.4 | 8.5 | 6,096 |
| Avoiding risky altars | 82 | +16,321 | 5 / 4 | 5.0 | 8.1 | 5,851 |
| Walk and wait penalty | 83 | +20,167 | 27 / 25 | 5.2 | 8.0 | 5,785 |
| Bonus for a capture we hold | 83 | +14,451 | 2 / 0 | 4.9 | 8.2 | 5,895 |
| Vesper's gold scouting | 84 | +20,992 | 18 / 15 | 4.9 | 8.2 | 5,792 |
| Walk to an altar in camp choice | 84 | +23,121 | 27 / 24 | 5.7 | 7.9 | 6,383 |
| Bonus for camps enemies stand on | 85 | +16,464 | 6 / 2 | 5.1 | 8.0 | 5,958 |

## Emblem families

Each family was removed from every champion's build.

| Removed | Wins | Margin | Gained / lost | Kills | Deaths | Gold |
| --- | --- | --- | --- | --- | --- | --- |
| Prospector | **70** | +4,057 | 6 / 17 | 4.9 | 8.8 | 5,771 |
| Durability | **72** | +3,458 | 16 / 25 | 5.5 | 9.1 | 5,946 |
| Monster | 75 | +6,652 | 11 / 17 | 4.9 | 8.2 | 5,911 |
| Jungle | 76 | +14,602 | 10 / 15 | 5.5 | 8.2 | 6,189 |
| Skirmish | 77 | +5,963 | 7 / 11 | 4.6 | 8.8 | 5,804 |
| Capture | 78 | +12,145 | 9 / 12 | 5.0 | 8.5 | 5,915 |
| Treasure | 81 | +15,712 | 25 / 25 | 5.2 | 7.8 | 5,588 |
| Home | 81 | +15,117 | 5 / 5 | 5.1 | 8.2 | 5,936 |
| Power | 83 | +15,922 | 18 / 16 | 5.3 | 7.8 | 6,083 |

## Other tests on the same code

| Variant | Result | Note |
| --- | --- | --- |
| Captures with no favoured holder | 60/140 | Reference 81 |
| Captures to the least-equipped champion | 58/140 | Reference 81 |
| 4-pick roster with Willow (three variants) | 9-13/32 | Shipped roster 17/32 |

Giving captures to Vesper and Marcella first is among the strongest things `baseline` does. It leaves Vendetta and Heart of Ruin poor, but sharing the gold loses far more.

## What this says

- **Clearly important:** the recall home after a capture, the long-plan penalty, retreating at low health, the capture holder order, and Prospector and Durability.
- **Candidates to remove or weaken, unproven:** the bonus for camps enemies stand on (85, gained 6 and lost 2), and possibly the walk to an altar in camp choice and Vesper's scouting.
- **Nothing here reaches 100/140.** Removing a rule does not add what top teams do, such as one carry with stacked Bruiser.
