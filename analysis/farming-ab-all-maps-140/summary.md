# Farm-value A/B: all maps

The current farm weights and a candidate with higher early gold value and Willow gold value, and lower Prism safe-gem weighting, each played baseline on all 70 maps in both seats at 100,000 fuel. The map hashes and seeds matched by map and test-bot seat.

| Test-bot policy | Wins / 140 | Gems collected | Gems submitted | Gold earned | Champion deaths | Bot faults |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Current weights | 68 | 132,469 | 102,417 | 691,133 | 836 | 0 |
| Farm-value candidate | 63 | 125,725 | 98,560 | 689,786 | 868 | 0 |

The candidate won 18 matches the current policy lost and lost 23 the current policy won. It reduced collection and submissions as well as wins, so the original weights remain selected. This A/B changes three weights together and does not isolate their individual effects.

```powershell
python scripts/evaluate.py economy:early_gold_value=0.25f,willow_gold_factor=0.25f,prism_safe_gem_factor=1.5f,fortress_gear_phase=false --bot test-bot --baseline baseline --fuels 100000 --jobs 4 --out analysis/farming-ab-all-maps-140
```
