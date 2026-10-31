# Visible-enemy guard probe

The first early carrier banking rule changed winners on five map-seat pairs. Requiring `enemies.empty()` before taking that early bank was tested on the four affected maps, in both seats. All eight winners matched the original control, including the scoreless `01boss-battle` seat. The tighter condition is not enabled.

```powershell
python scripts/evaluate.py carrier:safe_carrier_banking=true,safe_carrier_clear_vision=true,fortress_gear_phase=false --bot test-bot --baseline baseline --maps 00dunes,01boss-battle,02the-pack,adgato-gemcombat5 --fuels 100000 --jobs 4 --out analysis/carrier-quiet-probe-four-maps
```
