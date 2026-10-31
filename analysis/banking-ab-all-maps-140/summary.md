# Narrow Prism endgame banking A/B: all maps

On all 70 maps in both seats at 100,000 fuel, the current fixed 200-gem policy and a narrow endgame rule each played baseline. The candidate allows a Prism carrying at least 120 gems to bank near a safe altar when the team is within 250 submitted gems of winning. It keeps the fixed 200-gem target otherwise.

| Test-bot policy | Wins / 140 | Team gems submitted | Prism gems submitted | Prism gems lost on death | Prism end bags | Bot faults |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Fixed control | 68 | 102,417 | 19,466 | 6,691 | 3,817 | 0 |
| Narrow endgame candidate | 68 | 102,447 | 20,006 | 6,500 | 3,171 | 0 |

No paired outcomes changed. The candidate shifted Prism gems into submissions but did not improve the team win count or meaningfully change total team submissions. The switch remains off by default.

```powershell
python scripts/evaluate.py banking:prism_endgame_banking=true,fortress_gear_phase=false --bot test-bot --baseline baseline --fuels 100000 --jobs 4 --out analysis/banking-ab-all-maps-140
```
