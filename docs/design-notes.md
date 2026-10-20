# Battlecode notes

Retained details from the trio design approved for implementation, plus the eight complete rosters explicitly requested for recovery. No new proposals are included.

## Central contest selection

- Select a central gem camp using the actual layout and both teams' terrain routes. Compare the trio's first useful firing/control opportunities with plausible enemy interference, including movement restrictions and alternating turns. Do not delay merely to arrive simultaneously with an opponent.
- Preserve the opening window. Take a home-camp detour only when it preserves useful readiness or provides preparation actually required for the contest; otherwise leave that camp to the resource workers.
- Clear immediately when clear and capture can finish before credible interference. Otherwise compare spending health and cooldowns on monsters with keeping the actions needed to deny the opponent entry. If the selected contest becomes infeasible, compare other central or forward camps before temporary preparation work.

## Planning and action scheduling

- Rank camps cheaply before refining finalists, including the current job. Keep the best completed estimate and remaining candidate cursor when computation is deferred. Invalidate affected estimates after material changes, not merely because the round advanced; a deferred calculation must not erase a valid job.
- When there is no job, reserve selection work before optional combat searches and ward work. Reserve enough computation for a useful legal action, essential safety evaluation and publication. Spend only the surplus on route caches and optional observations.
- If blocked-movement planning is deferred, retain its failure reason and prioritize resolving it next turn. Publish the actual result rather than claiming successful travel.

## Combat follow-through

- Cassi can control a secondary enemy that could punish the damage dealers while Stellar and Tavi damage the primary blocker. Choose Cassi's damage instead when additional control would not improve the exchange.
- Use Cassi's walk-stun only when the interruption itself matters or an actual teammate opportunity can exploit it before recovery. Check personal visibility, movement restrictions, readiness and intervening enemy turns rather than assuming nearby allies can follow up.
- Avoid pure control that adds no useful action window. Tavi can still damage an already stunned enemy to finish it. Re-evaluate stun expiry, immunity, health and turn timing after actions instead of following a fixed alternating-stun sequence.
- Keep members with productive shots in place while a threatened member repositions, unless a combined finish is better. Set next-round priorities from useful opportunities: decisive finish/interruption, enabling control, then damage or movement that can exploit it. Include other allied champions and interleaved enemy turns.
- Stop pursuit when the enemy can no longer interfere with the capture or delivery, or when chasing would lose the capture window.

## Continuing or abandoning a long clear

- Before a long clear, check who can enter a reward tile after the final hit, whether the others can protect occupancy, and which observed enemies can interfere before the reward. If control is missing, fight for access or choose another forward camp before investing further damage.
- Compare the remaining clear/capture/delivery cycle with other forward jobs and the named preparation alternative. Refresh after a useful ability cycle and immediately after a death, lost capture, route obstruction or important enemy arrival. Damage already dealt is sunk cost; enemy damage reduces remaining health but does not establish our capture control.
- Count actual phase progress: improving terrain-route distance, decreasing remaining clear work, maintained uncontested capture, and bank access/channel progress. Circling, rereading a claim or reselecting the same camp cannot reset the operation's deadline. Repeated capture resets require reconsideration; a legitimate cooldown wait follows the next useful ready action.

## Archived eight complete rosters

These are the eight alternatives requested for recovery, not an instruction to re-enable roster selection. Twins is one draft pick producing Cassi and Issac.

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
