# Test-bot roster A/B: all maps

The current test-bot roster and the alternate baseline roster played the same baseline revision on all 70 maps, both seats, at 100,000 fuel. Each alternate match used the same map hash and engine seed as its current-roster control. Only the roster switch changed.

| Test-bot roster | Wins / 140 | Gems collected | Gems submitted | Gold earned | Champion deaths | Bot faults |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Current: Vendetta, Marcella, Heart of Ruin, then Prism and Willow | 68 | 132,469 | 102,417 | 691,133 | 836 | 0 |
| Baseline: Vesper Vale, Marcella, Heart of Ruin, then Vendetta and Ryth | 47 | 116,797 | 93,844 | 692,448 | 956 | 0 |

The alternate roster won 18 matches the current roster lost, but lost 39 that the current roster won: a net 21-win regression. Gold income barely moved (+1,315), while gem collection fell by 15,672 and deaths rose by 120. The current roster remains selected.

```powershell
python scripts/evaluate.py roster:baseline_roster=true,fortress_gear_phase=false --bot test-bot --baseline baseline --fuels 100000 --jobs 4 --out analysis/roster-ab-all-maps-140
```
