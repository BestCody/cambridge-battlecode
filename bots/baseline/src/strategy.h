#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cambc.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <vector>

namespace bc = cambc;
using bc::vec2;

// Policy coefficients, deliberately separate from engine-provided game rules.
namespace policy {
// A gold's value in gems in camp choice: early_gold_value at round 0, moving
// linearly to late_gold_value by half the game. Set to willow best girl's
// choice of gold-only groups (20% of their captures before round 300, 15% to
// round 800, 30% after, counted from the round dumps); 0.65 / 0.10 gave ours
// 54 / 30 / 20%, 0.25 / 0.20 gave 31 / 25 / 22%.
inline constexpr float early_gold_value = 0.35f; // 0.17645 before v106: see the note below
inline constexpr float late_gold_value = 0.1f;
// 0.35 early (v106): the peers that beat us (estrogen :3, QS2isLaw, MFF1)
// out-gold us two to one by round 300 and win the fights with the stats;
// locally 81/140 against test-bot and 79/140 against the stacker to 77 and 75
// at 0.17645 (analysis/gold_140).
// A hurt runner below this share of its health is followed into fog
// (worker_pursue()).
inline constexpr float pursue_health = 0.35402f;
// A champion heads home below this share of its health, away from camps
// (health_recovery_needed()), in a fight or not: willow best girl's
// turn for home (by recall in 88-89% of trips) most often at 25-35% (20-26%
// of its trips) and seldom at 35-45% (7-10%); ours, leaving at 45%, turned
// at 35-45% in 23-25% and at 25-35% in 13-14%. In contact its champions
// start for home most at 28-35% of their health (12-14 retreats per 100
// contact rounds, 6-8 at 35-45%), the same line; ours, at 28%, left at
// 28-35% in 8 of 100.
inline constexpr float retreat_health = 0.34436f;
// The health a camp visit can spend, before the walk out of the camps' reach:
// all of it, unless below the line we go home at away from camps (then home
// first). Print dont int's tanks leave camps at 15-20% of their health, 1-2
// rounds of the camps' damage past the walk out.
inline float camp_visit_health(float health, float max_health) {
    return health >= max_health * policy::retreat_health ? health : 0.0f;
}
// Awake camps this strong (golems 120, monoliths 200) of groups we do not farm
// keep us out of their reach (mark_camp_zones()).
inline constexpr float avoided_camp_strength = 100;
// After a capture with no enemy near, the holder recalls on the capture tile
// (the recall lands in the round of the reward) when below this share of its
// health, and shops all its gold at home.
inline constexpr float home_capture_health = 0.68275f;
// ... and, healthy, when it left spawn fewer than this many rounds before the
// recall starts (the reward 5 rounds later then comes under 30): willow best
// girl's champions went home after 58-86% of healthy captures within 30
// rounds of leaving spawn and after 2-18% of later ones (theirs Prism, which
// keeps its gems, 47% and 2%; ours 2-4% either way), so short trips out to
// the groups near home end in a recall home and the next one starts there.
inline constexpr int short_trip_rounds = 19;
// ... and, healthy or far out, carrying an emblem's gold (five shards) to
// spend: the trip home buys it.
inline constexpr unsigned home_capture_gold = 155;
// A retreat's recall counts enemies as chasing for this many of its turns:
// willow best girl's back line completes recalls with an enemy 3-8 tiles
// off (its Vesper 2.8 a game, ours 1.0 when we assumed a full chase).
inline constexpr int recall_chase_turns = 6;
// Turns of a walk to a target or an altar that make no step (walled off by
// camp bodies) before it is left out for stuck_rounds.
inline constexpr int stuck_turns = 2;
// A farm turn that ends on the tile of two turns ago counts as a stall too
// (bounce_stalls): on 00split a Vendetta stepped into an elk's reach and
// back out, (17,27) and (18,28), for 390 rounds at constant health with 7
// gems, farming nothing, because only a turn without a step counted.
inline constexpr bool bounce_stalls = true;
// ... after bounce_min bounces in a row (every bounce counted: 81/140 to 74, the fight's step in and out).
inline constexpr int bounce_min = 6;
inline constexpr int stuck_rounds = 15;
// Hard camps (golems and monoliths: hard_camp_health or more): a clear of one
// under way is seen through (worker_farm()). Alone a golem (5,000 health, 120
// a round, reach 4) outlasts any champion's health and a monolith (20,000,
// 200, reach 5) more so: camp choice plans them as parties.
inline constexpr float hard_camp_health = 5000;
inline constexpr float casualty_margin = 0.15155f;
inline constexpr int replan_period = 2;
inline constexpr float unused_cast_cost = 0.010918f;
// Squad assembly: converging on surviving allies is the real regroup.
inline constexpr float depleted_rally_bonus = 2.4569f;
// Fraction of the team that must still be alive for the squad to count as
// assembled. Below it the rally bias turns on. 0.0 disables regrouping entirely.
inline constexpr float depleted_alive_fraction = 0.45268f;
// Decisions never depend on the fuel left, so the same game plays the same at
// any fuel budget.
// Walking distance a camp choice first searches to, before groups have their
// own fields. A choice the groups beyond it could still win searches on.
inline constexpr std::uint16_t path_bound = 32;
// Work limits, measured as the fuel this turn has used so far: the same at
// any budget, unlike the fuel left, so they change no game between budgets.
// Background precomputation runs a map-wide search only while the turn's work
// so far plus that search's cost (the last one measured) stays under the
// first limit, and its smaller steps while the work is under the second.
inline constexpr std::uint64_t background_search_work = 80000;
inline constexpr std::uint64_t background_step_work = 60000;
// Log lines (ACTIVITY, DEATH, CRYSTAL...) for local analysis. Off in ladder
// builds; turning them on changes no decision.
inline constexpr bool diagnostic = false;
// Fuel profiler for local test builds (mechanics/profile.h). Off in ladder builds.
inline constexpr bool profile = false;
// Gem sourcing on maps with no farmable gem camp. `contest` carries 20 gold
// clusters and 2 monoliths and nothing else: no gem cluster, no wolf, no elk.
// Every camp on the map is therefore either worth 0 gems (gold clusters) or
// guarded by 20,000 health and 200 strength, and the standard risk model prices
// the monolith at ~1% of a gold cluster. Dropping to a 0-gem loss is not a risk
// the bot should be taking, so when the draft finds no cheap gem source the gem
// reward outranks gold and the fortress must be attacked anyway.
//
// A camp is "cheap" (farmable without losing bodies) below both thresholds:
// gems are still acquired, just at a cost the normal model already handles.
inline constexpr float cheap_camp_health = 6000.0f;
inline constexpr float cheap_camp_strength = 120.0f;
// Multiplier on a camp group's gem reward on a fortress-gem map. 1.0 = normal.
inline constexpr float siege_gem_weight = 0.82702f;
// Extra tiles beyond an enemy's reach plus move that still count as danger.
inline constexpr int worker_danger_margin = 1;
// Rounds an enemy out of sight is still counted after its last sighting
// (safety.cpp's ghosts): after two a walking enemy can be two tiles from
// where it was seen, the margin of every reach check, and the tiles it could
// be on say little. Measured: a ghost two rounds old is near its enemy two
// times in three, one four rounds old one time in three, and kept while
// updated (the old rule) four fifths of them were phantoms.
inline constexpr int ghost_lifetime = 5;
// Outnumbered where it stands (worker_skirmish()'s tile classes), the camps
// near those enemies are worth worker_sighting_penalty of their value for
// worker_sighting_memory rounds. Without it lone champions
// walked into a waiting group one at a time: half our ladder fights started
// outnumbered.
inline constexpr int worker_sighting_memory = 9;
inline constexpr float worker_sighting_penalty = 0.41529f;
// Banking: whatever a champion carries is banked on its way out from spawn
// and whenever an altar is within bank_nearby steps, as the top three bank on
// every trip home (about 20 banks a game of 28-39 gems; ours 6-10).
inline constexpr int bank_nearby = 3;
// A winning bag channels under fire only when the channel is this short (tribute.cpp).
inline constexpr int win_channel_turns = 4;
inline constexpr bool bank_nearby_clear = true; // the nearby trip only with no enemy in view (tribute.cpp)
// Exit risk's bag margin (exit_risk()): a round of the enemies' hits per
// this many gems carried. (Was the Prism's full bag, 200.)
inline constexpr unsigned exit_bag_gems = 216;
// Altar locks, as willow best girl places them (about 4 a game): a champion
// buys one at spawn when an altar wants one (lock_wanted(): the home altars,
// or one an enemy banked at within lock_memory rounds) and places it when it
// passes within lock_range of one.
inline constexpr int lock_memory = 248;
inline constexpr int lock_range = 2;
// Value multiplier for the camp a champion is already heading to, so it
// finishes camps instead of switching between them.
inline constexpr float worker_commit_bonus = 1.1102f;
// Camp choice clears a camp at the team's summed camp rate (ours and its
// claimants') times team_clear_share. A champion with a dash walks
// dash_travel_scale of the steps.
inline constexpr float team_clear_share = 0.61002f;
inline constexpr float dash_travel_scale = 0.69475f;
// Camp choice discounts a plan by its length, dividing its score by
// 1 + plan_hazard * duration (rounds): a long plan is more likely to be cut
// short by a fight, a death or a respawn. willow best girl captures clusters
// first (median round 16-38, in 52-57% of the games that have them) and
// wolves by round 38-65; ours, valuing a far golem group above a near
// cluster, 30% of those games and round 102.
inline constexpr float plan_hazard = 0.020924f;
// Siege pricing for monolith groups (siege_gems gems or more; 0: off), a
// specialist on the 22 maps that carry one. Measured on the peers' replays
// (NIRVAN_CLAUDE.md, "Monoliths", 2026-11-02): a monolith strikes only the
// nearest champion every round (200 before Jungle and Durability), so a party's
// fronts swap and the walk out is free; each stun (Ryth, Marcella's enhanced
// attack, Heart of Ruin's dash), a self-shield (1.5 x Focus) and a heal take
// about a hit in eight off the front; and three ordinary champions clear one
// in 66 rounds for about 3,200 health, test-bot's five in 40. The generalist
// model priced that at 14,000-44,000 damage, 50-300 trips home and a plan of
// 760-5,900 rounds, so no party ever began one; Ding, Tokenmaxxing and
// test-bot take them two or three times a game and win famine, big_world and
// rows on them alone. A 200-gem capture is not cut short the way a cluster
// run is: siege_hazard replaces plan_hazard.
inline constexpr float siege_gems = 150;
inline constexpr float siege_hazard = 0.0f;
inline constexpr float siege_min_front = 0.25f; // the front's share of the camp's hits with every save counted
// A siege begins only when the party finishes it without a trip home (at most
// siege_max_trips): priced at round 20 it was adopted at base stats and
// dropped within 10-40 rounds as teammates' states changed, over and over,
// so nobody's walk became a clear. The peers' first take is at round 550-850
// with 1,500-3,000 health each. An adopted siege (our last plan's party)
// scores siege_commit times its plan against the alternatives.
inline constexpr float siege_max_trips = 0;
inline constexpr float siege_commit = 1.5f;
// One monolith at most (the group's camp strength, before any neighbour):
// a pair strikes two champions a round, and the front that leaves at the
// retreat line walks four rounds out of reach 5 under both (boss-battle:
// deaths 6 to 30 in 2 games, 10 to the camps). Pairs and stacks (boss-battle,
// great-wall, rows, corridors) keep the generalist pricing.
inline constexpr float siege_max_pressure = 200;
// Camps struck from our own spawn tiles cost nothing: spawn heals to full at
// each turn end (docs/GameRules.md, "remain on spawn until that turn ends to
// heal fully"). 03homonoliths (ranked, 2026-11-02 22:30, 0-1200): the only
// gems are a 600-gem, 60,000-health, 600-strength monolith stack inside each
// spawn region; 剑桥夜爬者 chipped theirs from round 11 and took it at rounds
// 415 and 870, ours walked past it at round 2 and farmed 20-gold clusters,
// 4,864 gold to 3,380 and no gems: a champion on spawn with any health
// missing waited to heal, was struck at the round's start, and waited again.
// With spawn_siege such a group has no striker work, no pressure, its strike
// tiles are the spawn's, and a hit we outlast by spawn_siege_margin is no
// reason to wait.
inline constexpr bool spawn_siege = true;
inline constexpr float spawn_siege_margin = 1.5f;
// On a map whose only gems sit behind fortresses (fortress_gems), the living
// fortress at our spawn scores at least this (scores run 0.1-2): the whole
// team on it from round 1, as 剑桥夜爬者 played it (first take round 415;
// ours priced by health and hazard began at round 560 and took it at 835).
inline constexpr float spawn_siege_score = 10.0f;
// Pocket groups (Bot::prepare_pockets()): a group no walk from spawn reaches,
// inside a ring of holes a dash crosses (01big_world's 21-gem cluster triples,
// the-pond's four cluster groups, islands, fracture, membrane). Intercontinental
// Fish (ladder #2) dashes in, clears, captures and dashes out: four such
// captures by round 600 on big_world, where we take none. A champion with a
// dash of the entry's steps prices the group at its walk to the entry plus
// pocket_rounds (the dash in and the cooldown before the dash out).
inline constexpr bool pocket_entries = true;
inline constexpr int pocket_rounds = 3;
// A chain of dashes over stepping stones (the-pond's island: Intercontinental
// Fish's Issac dashes to a stone, waits the cooldown, dashes on): each further
// link costs the cooldown and the dash; at most pocket_max_links links, searched
// within pocket_chain_reach tiles of the group's centre, pocket_tiles_per_step
// take-off tiles a turn, at most pocket_region_tiles take-offs per region.
inline constexpr int pocket_link_rounds = 9;
inline constexpr int pocket_max_links = 4;
inline constexpr int pocket_chain_reach = 16;
inline constexpr int pocket_tiles_per_step = 12;
inline constexpr std::size_t pocket_region_tiles = 48;
// A group out of view that we last saw alive before a teammate's claim on
// it ended was dead at our choice claimed_dead_share of the time, until its
// respawn could be past (47-48% of 2,297 such choices in two logged runs of
// 140 games; with no teammate's claim since our sight, 6-7%).
inline constexpr float claimed_dead_share = 0.47f;
// Camp choice adds a group's walk to the nearest altar, up to altar_walk_cap
// tiles, to its duration at 0.25 rounds a tile, and a gem group's at
// scarce_altar_weight / (altar count) more: with few altars every gem is
// tributed at the same tiles, where the enemy waits and locks cost the most,
// so the gems near them are worth the more. 0 disables the scarcity term.
inline constexpr int altar_walk_cap = 22;
inline constexpr float scarce_altar_weight = 0.01444f;
// Camp choice also divides a group's score by 1 + travel_hazard * (travel +
// waiting) (the walk there and the wait for its respawn, in rounds; with the
// walk alone ours stood beside dead groups 140 rounds a game in rounds
// 150-400 to willow best girl's 40): relative to the walk, ours weighed gems 2-3
// times, gold 4-6, monster health 2.5-3 and teammates' groups 1.5-1.8
// times as much as willow best girl's choices (a logit of its new targets,
// both samples), and walked past near groups to far ones.
inline constexpr float travel_hazard = 0.10824f;
// Marcella's in place of travel_hazard: willow best girl's Marcella weighs
// a group's walk the least of its types (logit -2.39; ours -2.94) and takes
// one-elk groups in 41% of its new targets on the same maps (ours 26%).
inline constexpr float marcella_travel_hazard = 0.024222f;
// A group nearer the enemy spawn than ours (walking) scores
// enemy_side_factor of its value: willow best girl's choices avoid the
// enemy half more than ours did (logit -0.7..-0.9 against -0.5, ours
// alone -0.3..-0.4), and its lone contacts start 12-15 tiles from home
// (ours 18-19), so its lone champions are out of reach sooner.
inline constexpr float enemy_side_factor = 0.94067f;
// Camp choice values a group an enemy champion stands on this much more (a
// contest we can win there takes its reward from them).
inline constexpr float enemy_held_camp_bonus = 1.8336f;
// Camp choice values the capture we stand on and hold this much more, so a
// holder stays until it pays.
inline constexpr float held_capture_bonus = 4.4312f;
// An altar's risk for each enemy champion near it: this much, and
// altar_enemy_strength_risk per point of its strength (worker_tribute()).
inline constexpr float altar_enemy_risk = 3.5224f;
inline constexpr float altar_enemy_strength_risk = 0.040161f;
// A fight step's tile score: its walk times 10 plus the damage that can reach
// it times this (attack_combo()'s step).
inline constexpr float step_incoming_weight = 0.038261f;
// Ordinary camps farther than the map's natural camp spacing from every live
// teammate claim lose this much value per extra tile (test-bot's cohesion):
// the team stays within reach of each other. Only on maps whose spacing is at
// most cohesion_max_spacing (dense ones). Hard groups are left to the party
// plans. Off (0) until the GA tunes it: 0.03 everywhere was level (83/140),
// +3 on dense maps and -3 on sparse.
inline constexpr float cohesion_hazard = 0.0f; // 0.03: 88/140 at fuel 100,000 but 79 at 94,000; 0.05 83, 0.08 81: noise (2026-11-02)
inline constexpr float cohesion_max_spacing = 29.565f;
// The nearest groups (straight line) whose legs from a group the draft
// prices for a crystal hop (DraftPlan::leg_saving).
inline constexpr std::size_t leg_neighbours = 3;
// Vesper Vale (its gold bonus, its dash) values gold at
// vesper_scout_gold_value gems for its first vesper_scout_rounds (the others
// at early_gold_value): it farms the gold camps early and buys sooner.
// With it, 0.4 more kills and 0.2 fewer deaths a game against the test-bot
// (joshua_baseline's role; wins even).
inline constexpr int vesper_scout_rounds = 300;
inline constexpr float vesper_scout_gold_value = 0.5f;
// Marcella holds any capture she is on (capture_worth() adds this to her rank),
// so the team's gold makes one carry: 2,906 gold a game to her 2,076, 1 more
// kill a game, 253/420 games against test-bot to 243 (three fuel budgets,
// paired 39:29). naviepstokes and dromedaries run the same carry, earning up
// to 71% of their team's gold on her.
inline constexpr float marcella_capture_bonus = 100000.0f;
// On a map with at most scarce_map_gold on all its camps, gold is worth
// scarce_gold_boost times as much: items are what decide fights, and there is
// little to buy them with. On gemfight (about 500 gold) ours never cleared the
// elk groups, 22 gems and 110 gold, for 7-gem clusters worth no gold, and had
// 244 gold at round 300 to 1,217.
inline constexpr unsigned scarce_map_gold = 800;
inline constexpr float scarce_gold_boost = 1.0f;
// A capture the engine reports contested while we hold it with no enemy in
// view is swept (worker_farm()): its holder walks the tiles toward the enemy
// spawn and back until the enemy is seen. On vault (seat A against test-bot)
// both teams stood in the 343-gem centre from round 36 to 2,000, 0-0.
inline constexpr bool sweep_contested = true;
// Holding a contested capture with its enemy in view and nobody moving for
// this many rounds, the holder closes on the enemy (worker_skirmish()). 0
// switches it off.
inline constexpr int standoff_rounds = 6;
// Watching: the fight claims the turn with an enemy in view that can act on
// a teammate, or a side we outweigh, while no enemy can be struck from any
// tile a step reaches. On 00the-line (both teams in view all game along a
// 161-gem line) ours watched 70% of their turns and never farmed: a
// Marcella at 542 of 1,200 walked beside the line 25 rounds with no enemy
// within 3 tiles and no hit. A watching champion, safe where it stands,
// farms its camp when one is in its attack reach (farm_in_view), and after
// watch_limit strike-less turns in a row (0: never).
inline constexpr bool farm_in_view = true;
inline constexpr int watch_limit = 4;
// A carrier watching with a bank trip pending (on spawn, already banking, or
// an altar within bank_nearby steps) yields the turn to the tribute, on
// every map: ours stood ten rounds beside spawn with 56 gems while a
// Vendetta hovered 6-8 tiles off, the altar three tiles away (00packs
// against Olympus r287-300; the bag was carried 193 rounds).
inline constexpr bool watch_bank = true;
// A bag beside a camp waiting to respawn goes to the altar when the wait,
// times wait_bank, covers the round trip and the channel (0: never): on
// 03ultrastarvation (four 7-gem camps, 50-round respawns, one altar 30 steps
// off) ours sat 800 rounds beside its camp with 14 gems, banking 44 all game
// to QS2isLaw's 242 in 24 small lots.
inline constexpr float wait_bank = 1.5f;
// With no altar reachable on foot (an island of camps inside holes, reached
// by a crystal hop: 02the_pond), a bag of island_bag gems recalls home and
// banks from spawn; any bag does when the rounds left just cover that
// (worker_tribute(); 0: never). Ours ended the pond with 370 gems in hand.
inline constexpr unsigned island_bag = 60;
// The emblems every champion buys first, of its main stat, in this order,
// ahead of its own build (economy.cpp build_for; a build's own step of the
// family moves to the front). Capture (subscribe to caterpillow, 25-15 over
// its last 40 ranked games, all six wins studied; v104, 15/30 in practice
// against v79's 12/30) caps the team's capture timer at 3 turns instead of
// 10, joining mid-capture included. Prospector (estrogen :3 and QS2isLaw buy
// it first on every champion at rounds 42-300, MFF1's Marcella too) gives
// 25% more gold and gems from every capture.
inline constexpr std::array<bc::item_passive, 2> front_passives = {bc::item_passive::prospector, bc::item_passive::capture};
// Where the watch rules run (bot.h watch_map). Everywhere, they move the
// weak maps and lose as much elsewhere (280 games against test-bot at two
// fuel budgets: current 180, rules 177; the ten maps we never won 2 to 20 of
// 40, the rest 178 to 157), and no map feature or in-game signal (gem gap,
// the champion's own watching rate) separates the two. So an expert, routed
// by the map's print (width, height, camp groups, gems: the SDK gives no
// name): the maps where the rules gained 2+ of 4 games at both budgets.
// With watch_route off the rules run on every map.
inline constexpr bool watch_route = true;
struct MapPrint {
    int width, height, groups, gems;
};
inline constexpr MapPrint watch_maps[] = {
    {37, 31, 13, 258},  // 00anvil 0 to 2 of 4
    {41, 41, 20, 280},  // 00collision 0 to 3
    {40, 40, 14, 200},  // 00interlock 1 to 2
    {29, 29, 10, 124},  // 00oasis 0 to 3
    {30, 30, 14, 334},  // 00reverse-spiral 0 to 2
    {31, 31, 7, 359},   // 00vault 0 to 4
    {41, 61, 8, 252},   // adgato-gemcombat4 0 to 4
    {50, 41, 31, 1180}, // 01boss-battle 1 to 4
    {91, 75, 11, 380},  // 02famine 0 to 3
};
// Centre maps (a group with centre_share of the gems, opening_reach or more
// steps from spawn: gemfight's 78-gem centre is 30 steps through a spiral of
// holes, vault's 343). For the first opening_rounds, groups past
// opening_reach are worth opening_far_mul in camp choice and left out of the
// opening group: Intercontinental Fish clears the wolf and gold camps beside
// its spawn with all five, then the elk group, and reaches the centre as a
// group with 500 gold; ours walked there one by one with nothing and lost
// the fight (practice 2026-11-01, 0 of 10). 0 rounds switches it off.
// Against test-bot, which rushes the centre too, 80 rounds lost 85/140 to
// 91 (paired +3 -9: both gemfight seats, pilgrimage, tangle): we arrive
// second. Untested against the top teams beyond one 10-game probe (v80).
inline constexpr float centre_share = 0.35f;
inline constexpr int opening_reach = 20;
inline constexpr int opening_rounds = 80;
inline constexpr float opening_far_mul = 0.1f;
// Routed (opening_route): the opening runs only on the maps listed by print
// (bot.h opening_map), each with its own reach, and the centre_share rule
// is not used. 03colosseum (Olympus, 0/3 ranked): the only altar and all
// 196 gems are eight groups at the centre, 18-25 steps from spawn, and
// 100-gold wolf camps ring the edge 6-15 steps from spawn; Olympus's
// Vendetta farmed 122 turns in the first 300 rounds, ours 15-20, and
// their gold led 2578 to 968 at round 300.
inline constexpr bool opening_route = true;
// The harvest species (a second way to play, routed by map print like the
// watch expert): until a champion is ready it farms gold on our half of
// the map at harvest_gold_value gems a gold (camp choice), discounts the
// enemy's half by harvest_enemy_side instead of enemy_side_factor, takes
// no fight that is not a kill, a finish or a duel the duel model wins with
// the allies in reach (in an enemy's reach otherwise it breaks contact, out
// of reach it farms on) and chases nothing. Ready is
// harvest_emblems finished emblems or round harvest_until. The top teams'
// pattern on contested-centre maps (Intercontinental Fish on quarry: own
// side gold first, regroup, hit the enemy while it clears the centre; MFF1
// and caterpillow bank nothing before round 800 on one_gem, stampede and
// homonoliths and win on gold and kills). Candidate maps: most of the gems
// in the middle band and some gold on our half (analysis/mapfeat).
inline constexpr float harvest_gold_value = 0.6f;
inline constexpr float harvest_near_enemy = 0.1f; // a group with an enemy seen near it lately, while harvesting
// The species' cohesion hazard (cohesion_hazard's form), in both phases: in
// every death stepped through (anvil, oasis, hooks) the victim's teammates
// were 10-24 tiles away.
inline constexpr float harvest_cohesion = 0.1f;
inline constexpr int harvest_alone_range = 4; // the fight gate applies with no healthy teammate this near
inline constexpr float harvest_enemy_side = 0.3f;
inline constexpr int harvest_emblems = 3;
inline constexpr int harvest_until = 500;
inline constexpr bool harvest_all = false; // every map plays the species (map tables)
// The maps where the species won games v106 lost on both local opponents
// (analysis/harvest_all2 against analysis/v106_140, 4 games a map; compare.py):
// islands 1 to 4, gemfight 0 to 3, yellow-brick-road 1 to 3, anvil 0 to 2,
// gemcombat1 1 to 3, broken-curve 2 to 4, and +1 with fewer deaths on the rest.
// gate_alone: the fight gate applies only with no healthy teammate within
// harvest_alone_range (with teammates near the micro fights as usual). On
// anvil and gemfight 2/4 and 3/4 to 4/4 each; on broken-curve 4/4 to 1/4,
// yellow-brick-road 3/4 to 1/4, gemcombat1 3/4 to 1/4 (analysis/hv5), so it
// is a per-map choice.
struct HarvestMap {
    MapPrint print;
    bool gate_alone;
};
// The species' map class (analysis/mapfeat/tree.py on the species against
// v106, 4 games a map): the gold on the groups nearer our spawn than the
// enemy's (walk, home within harvest_own_share of the enemy's) at least
// harvest_own_gold with harvest_picks champions: islands +3, yellow-brick-road
// +2, boss-battle +1, gift-wrapping 0, no member worse. The rest of the
// hand-picked twelve had no rule behind them and were dropped; anvil and
// gemfight stay by identity with the alone gate (8/8 locally).
// Own-half gold alone cannot separate islands (1,280 in the engine's
// straight-line count) from split (1,330, where the species loses 2 of 4),
// so the class also needs a contested middle: harvest_mid_share of the gems
// in the band about as far from both spawns (within harvest_mid_band of the
// spawns' separation). Islands 54%, yellow-brick-road 36%, boss-battle 38%,
// gift-wrapping 32%; split 0%, road-of-riches 8%, the-farm 10%.
inline constexpr float harvest_own_gold = 1200;
inline constexpr float harvest_own_share = 0.8f;
inline constexpr float harvest_mid_share = 0.3f;
inline constexpr float harvest_mid_band = 0.2f;
inline constexpr int harvest_picks = 5;
inline constexpr HarvestMap harvest_maps[] = {
    {{37, 31, 13, 258}, true}, // 00anvil
    {{30, 30, 19, 200}, true}, // 00gemfight
};
struct OpeningMap {
    MapPrint print;
    int reach, rounds;
};
// v90 (80 rounds on colosseum): gold at round 300 968 to 1,365, early
// deaths 6 to 3, still 610-1008; Olympus farms the edge all game.
inline constexpr OpeningMap opening_maps[] = {
    {{51, 51, 42, 268}, 15, 80}, // 03colosseum (300 rounds: 383-1017, worse)
};
} // namespace policy

// Next-round action order. Tiers are disjoint so plain damage never outranks
// a finishing blow, whatever the raw damage values.
namespace priority_tier {
inline constexpr int idle = 1000;
inline constexpr int damage = 2000;
inline constexpr int finish = 4000;
inline constexpr int span = 999;
} // namespace priority_tier

// Published mechanics not exposed in the SDK settings structure.
namespace rules {
inline constexpr int crystal_range = 3;

// Engine settings, read from the engine on first use. Never use the SDK's
// bc::constants copies of them: those are filled during static initialisation
// in an unspecified order and in some builds copy the settings before they
// are loaded. winning_gems was 0 there, so every bag looked like the winning
// deposit. (bc::constants::passive_abilities are compile-time and safe.)
inline const bc::detail::game_settings &settings() {
    static const bc::detail::game_settings value = [] {
        bc::detail::game_settings result{};
        bc::detail::check(bc::detail::sdk_get_settings(&result));
        return result;
    }();
    return value;
}
} // namespace rules

// vec2's own distance works in 64 bits; map tiles fit 32 with room to spare.
inline int distance(vec2 a, vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }
// Exact ability::is_ready() for our own champion: off cooldown while it can
// cast. The SDK's version rebuilds the owner's whole champion view on every
// call (about 580 fuel); this reads the cooldown alone (about 140).
inline bool ready(const bc::ability &a, bool can_cast) { return can_cast && a.info() && a.cooldown() <= 0; }
inline bc::bitboard around(vec2 p, float radius) {
    bc::bitboard b;
    b.draw_chebyshev(p, radius);
    return b;
}
inline bc::bitboard terrain() {
    return bc::map::walls() | bc::map::holes() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy);
}
inline int channel_duration(unsigned gems) {
    return std::max(rules::settings().submit_gem_minimum_duration,
                    // The epsilon keeps ceil(30 * 0.1) at 3: the double product is 3.0000000000000004.
                    static_cast<int>(std::ceil(gems * rules::settings().submit_gem_duration_multiplier - 1e-9)) +
                        rules::settings().submit_gem_base_duration);
}
// The turn ends a recall shows left once its first turn end has ticked:
// Home makes it three turn ends instead of five.
inline bool fresh_recall(int left, bool home) {
    const int full = rules::settings().recall_time;
    return left == (home ? (full + 1) / 2 : full) - 1;
}
inline unsigned id(const bc::champion &c) {
    return static_cast<unsigned>(static_cast<bc::handle_champion_t>(c));
}

struct Spell {
    bc::ability_kind kind{};
    float range = 0, modifier = 0, radius = 0;
    int cooldown = 1, duration = 0;
    bool exclusive = false, enhanced = false;
    bc::ability_target_mask targets{};
};
inline Spell describe(const bc::ability &a) {
    Spell s;
    if (!a.info())
        return s;
    std::visit(
        [&](const auto &p) {
            s.kind = p.kind;
            s.cooldown = std::max(1, p.max_cooldown);
            s.exclusive = p.exclusive_with_movement;
            s.targets = p.valid_targets;
            if constexpr (requires { p.range; })
                s.range = p.range;
            if constexpr (requires { p.modifier; })
                s.modifier = p.modifier;
            if constexpr (requires { p.radius; })
                s.radius = p.radius;
            if constexpr (requires { p.duration; })
                s.duration = p.duration;
            if constexpr (requires { p.enhanced_attack; })
                s.enhanced = p.enhanced_attack;
        },
        *a.info());
    return s;
}

// Fixed wire format. Slots are allied champion handles in ascending order: five
// 11-bit fields (our rosters have at most five bodies: no twin pick), each a
// 10-bit claim and one flag. The flag of a fight request is set while an enemy
// can hit that champion next round (attacked); of any other claim, while it
// rides a capture's recall home (leaving: its claim kept, so teammates leave
// the capture to it). Readers ask for attacked only with a request and for
// leaving only of a recalling holder's claim, so one bit holds both.
// Zero is idle; the top codes are unavailable/bank/recall, never camp identifiers.
struct TeamMemory {
    static constexpr unsigned bits = 10, mask = (1u << bits) - 1;
    static constexpr unsigned field = bits + 1, field_mask = (1u << field) - 1;
    static constexpr unsigned bank = mask - 1, recall = mask;
    static constexpr unsigned unavailable = mask - 2;
    static constexpr unsigned slots = 5;
    // Normal objective claims retain their original codes. Extra codes carry
    // an observed fight's nearest objective and the squad it takes to beat the
    // enemies the reporter sees, or identify a champion travelling to that
    // fight. Each owner refreshes only its own slot; responders never turn an
    // old request into a new request.
    static bool formations(unsigned groups) { return groups && groups * (slots + 2) < bank; }
    static unsigned request(unsigned group, unsigned force, unsigned groups) {
        return groups + 1 + group * slots + std::clamp(force, 1u, slots) - 1;
    }
    static unsigned joining(unsigned group, unsigned groups) { return groups * (slots + 1) + 1 + group; }
    static int objective(unsigned code, unsigned groups) {
        return code && code <= groups ? int(code - 1) : fight_group(code, groups);
    }
    static int fight_group(unsigned code, unsigned groups) {
        if (!formations(groups) || code <= groups || code >= bank)
            return -1;
        const unsigned start = groups + 1;
        if (code < start + groups * slots)
            return (code - start) / slots;
        if (code < start + groups * (slots + 1))
            return code - start - groups * slots;
        return -1;
    }
    static unsigned requested_force(unsigned code, unsigned groups) {
        if (!formations(groups) || code <= groups || code > groups * (slots + 1))
            return 0;
        return (code - groups - 1) % slots + 1;
    }
    std::uint64_t raw = 0;
    // Striking groups' party codes, after the fight ones: a champion whose
    // camp choice planned a party asks teammates to come (camp_request()),
    // a teammate that chose the group answers (camp_coming()); both claim
    // it. Without them a partner was only expected, never told: the one
    // expected came in a median 17 rounds where 11 were planned (free) or
    // 7 where 15 were (busy). A map with more such groups than codes left
    // asks for the first ones only.
    void set_camps(const std::vector<int> &hard, unsigned groups) {
        camp_base = formations(groups) ? groups * (slots + 2) + 1 : groups + 1;
        camp_list.clear();
        camp_of.assign(groups, -1);
        for (const int g : hard)
            if (camp_base + 2 * unsigned(camp_list.size()) + 1 < unavailable) {
                camp_of[std::size_t(g)] = int(camp_list.size());
                camp_list.push_back(g);
            }
    }
    unsigned camp_request(int group) const {
        return group >= 0 && std::size_t(group) < camp_of.size() && camp_of[std::size_t(group)] >= 0
                   ? camp_base + 2 * unsigned(camp_of[std::size_t(group)])
                   : 0;
    }
    unsigned camp_coming(int group) const {
        const unsigned code = camp_request(group);
        return code ? code + 1 : 0;
    }
    // The group a request or an answer is for, else -1.
    int camp_claim(unsigned code) const {
        if (code < camp_base || code >= unavailable)
            return -1;
        const unsigned k = (code - camp_base) / 2;
        return k < camp_list.size() ? camp_list[k] : -1;
    }
    // A request's codes, from the map's camp group count (none without
    // formations): a range test where requested_force() divides.
    void set_groups(unsigned groups) {
        request_low = formations(groups) ? groups + 1 : 1;
        request_high = formations(groups) ? groups * (slots + 1) : 0;
    }
    bool is_request(unsigned code) const { return code >= request_low && code <= request_high; }
    void read() { bc::shared_memory.read(0, std::span{&raw, 1}); }
    unsigned get(unsigned slot) const { return slot < slots ? (raw >> (slot * field)) & mask : 0; }
    // Our own claim (each owner is the only writer of its own field).
    void set(unsigned slot, unsigned value) { write(slot, value & mask); }
    bool attacked(unsigned slot) const { return flag(slot) && is_request(get(slot)); }
    void set_attacked(unsigned slot, bool value) {
        own_attacked = value;
        write(slot, get(slot));
    }
    bool leaving(unsigned slot) const { return flag(slot) && !is_request(get(slot)); }
    void set_leaving(unsigned slot, bool value) {
        own_leaving = value;
        write(slot, get(slot));
    }

  private:
    // Our own flags, kept here: the field holds the one its claim carries.
    bool own_attacked = false, own_leaving = false;
    unsigned request_low = 1, request_high = 0;
    unsigned camp_base = mask;
    std::vector<int> camp_list, camp_of;
    bool flag(unsigned slot) const { return slot < slots && (raw >> (slot * field + bits) & 1); }
    void write(unsigned slot, unsigned code) {
        // Observation refreshed the word and no teammate acts during this
        // turn.
        if (slot >= slots)
            return;
        const bool carried = is_request(code) ? own_attacked : own_leaving;
        const std::uint64_t value = code | unsigned(carried) << bits;
        if (((raw >> (slot * field)) & field_mask) == value)
            return;
        read(); // Preserve claims written by earlier teammates this round.
        raw = (raw & ~(std::uint64_t(field_mask) << (slot * field))) | (value << (slot * field));
        bc::shared_memory.write(0, std::span{&raw, 1});
    }
};

struct DraftPlan {
    static constexpr std::uint32_t tag = 0x41454735;
    std::uint32_t magic = tag;
    std::array<bc::champion_type, TeamMemory::slots> roster_types{};
    std::uint8_t body_count = 0;
    // Number of gem-bearing groups farmable without a siege. Gameplay values
    // fortress camps more highly when the full map has no cheaper gem source.
    std::uint8_t cheap_gem_groups = 0;
    // All the gold on the map's camps, in forties (capped at 10,200).
    std::uint8_t map_gold_forties = 0;
    float spacing = 0;
    // Walking distance from each camp group, in camp_groups() order, to our
    // spawn, the enemy spawn and an altar's tribute tiles, and from our
    // spawn with one crystal hop (hop_walks(); unreachable where it saves
    // nothing): map-wide searches the draft affords on its first turn, which
    // a champion's turns cannot (the hops alone froze each champion 20-75
    // rounds on the ladder). group_count is 0 when they are not here. The
    // 70 maps have at most 51 groups and 18 altars.
    static constexpr std::size_t max_groups = 96, max_altars = 24;
    enum Ground { home, enemy, altar, hop };
    std::uint16_t group_count = 0;
    std::array<std::array<std::uint16_t, 4>, max_groups> group_distance{};
    // And each group's splash shape: the most camps within splash radius of
    // one of its camps (at least 1), counted over every pair.
    std::array<std::uint8_t, max_groups> group_splash{};
    // Each altar's walk from our spawn with one hop, as for the groups.
    std::array<std::uint16_t, max_altars> altar_hop{};
    // The most rounds one crystal hop saves on a leg out of each group (to
    // its nearest altar or one of its leg_neighbours nearest groups in a
    // straight line), in units of two rounds, four bits a group: the walks
    // champions take between camps, which the hops from spawn above leave
    // out (a wall between two camp areas). Units past 15 count as 15.
    std::array<std::uint8_t, max_groups / 2> leg_saving{};
    int leg_rounds(std::size_t group) const {
        return group < max_groups ? 2 * ((leg_saving[group / 2] >> (4 * (group % 2))) & 15) : 0;
    }
};
static_assert(sizeof(DraftPlan) <= 1024);

// The walk from our spawn with one teleport-crystal hop to every tile
// (navigation.cpp), from the walk without (`walk`, row-major) and the open
// tiles; written row-major to `hop`.
void hop_walks(const std::uint16_t *walk, const bc::bitboard &open, int width, int height, std::uint16_t *hop);
// A field's least value within crystal_range along each row, for rows
// first_row to last_row and columns first_column (a multiple of 8) up to
// end_column: rows padded by the range both sides into `rows` (row stride
// (width + 2 range + 7) & ~7, 8 entries more), the minimum into `near` (row
// stride (width + 7) & ~7). With `run_start` (walking-region runs, as
// Bot::region_start/end/rows/label hold them), a tile in no run whose label
// is one of the `kinds` in `allowed` counts as unreachable.
void row_window_min(const std::uint16_t *field, int width, int height, std::uint16_t *rows, std::uint16_t *near,
                    int first_row, int last_row, int first_column, int end_column, const int *run_start = nullptr,
                    const int *run_end = nullptr, const int *row_runs = nullptr,
                    const std::uint16_t *run_label = nullptr, const std::uint16_t *allowed = nullptr, int kinds = 0);
// The least walk from `from`'s roots to `to`'s with one crystal hop
// over the box x0..x1, y0..y1 (navigation.cpp): row-major walks of the
// same map, `from` padded by 8.
int hop_leg(const std::uint16_t *from, const std::uint16_t *to, int width, int height, int x0, int y0, int x1, int y1);
// An altar's walk with a hop (the least over its tribute tiles) where shorter
// than its walk, else unreachable.
int altar_hop_walk(vec2 altar, const std::uint16_t *walk, const std::uint16_t *hop, int width, int height);

// Tiles within tribute range of an altar, for the altar field.
inline bc::bitboard altar_goals_of(const std::vector<vec2> &altars, const bc::bitboard &static_blocked) {
    // Drawn straight into one mask (drawing adds tiles): no mask per altar.
    bc::bitboard goals;
    for (auto p : altars)
        goals.draw_chebyshev(p, rules::settings().submit_gems_range);
    goals &= ~static_blocked;
    return goals;
}

// The draft found gems on the map but none of them behind a farmable camp, so
// every gem on the map sits behind a fortress (contest: 20 gold clusters and two
// monoliths). Gold clusters cannot win the game; the fortress has to be taken.
inline bool fortress_gems(const DraftPlan &plan) { return plan.cheap_gem_groups == 0; }
// Weight on a camp group's gem reward. 1.0 normally; raised on a fortress-gem
// map so gems outrank the gold clusters that would otherwise soak every job.
inline float gem_reward_weight(const DraftPlan &plan) {
    return fortress_gems(plan) ? policy::siege_gem_weight : 1.0f;
}

// A damaging dash (Heart of Ruin) counts as a damage spell.
// A unit's fighting power, ours, an ally's, an enemy's or a type's: its
// health with its shields, against what it deals (its damage rate, a fifth of
// its ready burst, its healing), over its mitigation. Four formulas left our
// own shields and healing out where an enemy's counted them: a lone Willow
// undervalued itself.
// The fighting power a champion of `type` gains along its build, per gold,
// as a share of its design's (economy.cpp).
float build_gain_per_gold(bc::champion_type type, const bc::champion_stats &design, const std::array<Spell, 2> &spells);
inline float unit_power(float pool, float rate, float burst, float heal, float mitigation) {
    return std::sqrt(std::max(1.0f, pool) * (rate + burst * 0.2f + heal) / mitigation);
}

inline float spell_dps(const std::array<Spell, 2> &spells, float strength, float focus, float splash = 1) {
    float result = strength * splash;
    for (const auto &s : spells) {
        if (s.kind == bc::ability_kind::attack_reset)
            result += strength * splash / s.cooldown;
        if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc)
            result += focus * s.modifier / s.cooldown;
        if (s.kind == bc::ability_kind::aoe_damage)
            result += focus * s.modifier * splash / s.cooldown;
        if (s.kind == bc::ability_kind::dash && s.modifier > 0)
            result += focus * s.modifier / s.cooldown;
    }
    return result;
}

// Heart of Ruin's passive: up to half again on all its damage with missing
// health (the engine's readings leave it out).
inline float hor_bonus(float health, float max_health) {
    return 1 + bc::constants::passive_abilities::heart_of_ruin_missing_health_damage_max_bonus *
                   (1 - health / std::max(1.0f, max_health));
}

// A spell that clears camps from beyond their reach (a spell-range siege):
// an area spell, or a damage or stun spell that can target a camp, reaching
// four or more and dealing damage. One rule for camp choice's siege rate,
// the farming stand and the background siege checks: three rules sent a
// champion whose only long spell cannot target camps to stand where it
// could not strike.
inline bool siege_spell(const Spell &s) {
    return s.range > 3 && s.modifier > 0 &&
           (s.kind == bc::ability_kind::aoe_damage ||
            ((s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc) &&
             s.targets.contains(bc::ability_target_mask::camps)));
}

// Damage per round on camps, for camp choice: hits, resets, area spells, the
// targeted spells that can target a camp (Ryth's stun never does) and a
// damaging dash landing on dash_hits camps. Area splash is the caller's.
inline float camp_dps(const std::array<Spell, 2> &spells, float strength, float focus, float dash_hits) {
    float result = strength;
    for (const auto &s : spells) {
        if (s.kind == bc::ability_kind::attack_reset)
            result += strength / s.cooldown;
        if ((s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc) &&
            s.targets.contains(bc::ability_target_mask::camps))
            result += focus * s.modifier / s.cooldown;
        if (s.kind == bc::ability_kind::aoe_damage)
            result += focus * s.modifier / s.cooldown;
        if (s.kind == bc::ability_kind::dash && s.modifier > 0)
            result += focus * s.modifier * dash_hits / s.cooldown;
    }
    return result;
}
