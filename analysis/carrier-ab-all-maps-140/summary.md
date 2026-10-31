# Safe-carrier banking A/B: all maps

The candidate gives a champion with at least 200 gems and a nearby safe altar a chance to bank before skirmish. Both policies played the same baseline revision on all 70 maps in both seats at 100,000 fuel. Seeds and map hashes matched by map and test-bot seat.

| Test-bot policy | Wins / 140 | Gems collected | Gems submitted | Gold earned | Champion deaths | Bot faults |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Current order | 68 | 132,469 | 102,417 | 691,133 | 836 | 0 |
| Early safe-carrier bank | 67 | 131,403 | 102,885 | 680,911 | 857 | 0 |

The candidate gained two paired wins (`01boss-battle` seat A and `adgato-gemcombat5` seat B) and lost three (`00dunes` seat B and both `02the-pack` seats). On `01boss-battle` seat A, Marcella submitted 1,460 gems versus no team submission in the control, winning that match. The global rule still lost one net win, earned 10,222 less gold, and had 21 more deaths, so it remains off by default.

A follow-up on the four maps with changed outcomes required no visible enemy before the early bank. All eight outcomes then matched the current control, including the zero-submission `01boss-battle` seat. This narrows the original rule's effect to situations with a visible enemy that cannot immediately hit the carrier. Those situations need a better fight-versus-bank decision than a fixed bag threshold.

```powershell
python scripts/evaluate.py carrier:safe_carrier_banking=true,fortress_gear_phase=false --bot test-bot --baseline baseline --fuels 100000 --jobs 4 --out analysis/carrier-ab-all-maps-140
```
