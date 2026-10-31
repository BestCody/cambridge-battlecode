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
inline constexpr float early_gold_value = 0.15f;
inline constexpr float late_gold_value = 0.30f;
// A reported fight's squad size divides the enemy power by aggression times
// our average champion power (worker_update_formation()).
inline constexpr float aggression = 1.8f;
// A hurt runner below this share of its health is followed into fog
// (worker_pursue()).
inline constexpr float pursue_health = 0.6f;
// A champion heads home below this share of its health: willow best girl's
// turn for home (by recall in 88-89% of trips) most often at 25-35% (20-26%
// of its trips) and seldom at 35-45% (7-10%); ours, leaving at 45%, turned
// at 35-45% in 23-25% and at 25-35% in 13-14%.
inline constexpr float retreat_health = 0.35f;
// While an enemy champion can hit us, home only below this share: willow
// best girl's champions in contact start for home most at 28-35% of their
// health (12-14 retreats per 100 contact rounds, 6-8 at 35-45%), the same
// line as its trips home; how the fight stands (ahead or behind, a teammate
// beside) splits it differently in its two samples. Ours, at 28%, left at
// 28-35% in 8 of 100.
inline constexpr float fight_retreat_health = 0.35f;
// Awake camps this strong (golems 120, monoliths 200) of groups we do not farm
// keep us out of their reach (mark_camp_zones()).
inline constexpr float avoided_camp_strength = 100;
// After a capture with no enemy near, the holder recalls on the capture tile
// (the recall lands in the round of the reward) when below this share of its
// health, and shops all its gold at home.
inline constexpr float home_capture_health = 0.80f;
// ... and, healthy, when it left spawn fewer than this many rounds before the
// recall starts (the reward 5 rounds later then comes under 30): willow best
// girl's champions went home after 58-86% of healthy captures within 30
// rounds of leaving spawn and after 2-18% of later ones (theirs Prism, which
// keeps its gems, 47% and 2%; ours 2-4% either way), so short trips out to
// the groups near home end in a recall home and the next one starts there.
inline constexpr int short_trip_rounds = 25;
// ... and, healthy or far out, carrying an emblem's gold (five shards) to
// spend: the trip home buys it.
inline constexpr unsigned home_capture_gold = 150;
// Camp choice divides a group's score by 1 + camp_risk_weight * risk (the camp
// damage a clear costs past what we can spare): willow best girl swaps
// wolves for one-elk groups (strike rounds r0-300: wolves 112 vs ours 148,
// one elk 120 vs 74) and weighs monster health half as much as ours did
// at 4 (logit -0.32/-0.35 vs -0.61).
inline constexpr float camp_risk_weight = 2.0f;
// The Willow's camp choice counts gold willow_gold_factor times: willow
// best girl's Willow's target logit weighs log gold -0.18/-0.38 (both
// samples), ours +0.34 - it leaves the gold groups to the others.
// Not 0: with every gem group out of plan, gold-only groups tied at 0
// and the lowest index won at any distance.
inline constexpr float willow_gold_factor = 0.02f;
// A retreat's recall counts enemies as chasing for this many of its turns:
// willow best girl's back line completes recalls with an enemy 3-8 tiles
// off (its Vesper 2.8 a game, ours 1.0 when we assumed a full chase).
inline constexpr int recall_chase_turns = 3;
// Turns of a walk to a target or an altar that make no step (walled off by
// camp bodies) before it is left out for stuck_rounds.
inline constexpr int stuck_turns = 3;
inline constexpr int stuck_rounds = 40;
// Hard camps (golems and monoliths: hard_camp_health or more) are cleared by a
// party: whoever claims one first waits out of the camps' reach until a
// teammate claiming it too is there, then they strike together. Alone a golem
// (5,000 health, 120 a round, reach 4) outlasts any champion's health and a
// monolith (20,000, 200, reach 5) more so; willow best girl strikes monoliths
// with 2.41 champions a round, ours with 1.12, and ours struck golems 7 times
// a game and monoliths 2. The wait for the partner is in the group's score
// (camp choice), so a claimant whose partner is too far away chooses again.
inline constexpr float hard_camp_health = 5000;
inline constexpr float casualty_margin = 0.12f;
inline constexpr int replan_period = 4;
inline constexpr float unused_cast_cost = 0.01f;
// Camp choice divides a group's score by 1 + partner_penalty * c(c+1)/2 for
// the c teammates already claiming it (whose help it also counts as faster
// clearing): each further one costs more. willow
// best girl clears together and holds alone: two or more of its champions hit
// 52-61% of the groups it clears before round 150, 41% later (parties of
// 1/2/3/4+ in 59/26/10/5% of its clears from round 150 to 400, 1.7 on a gem
// group), and one champion holds the capture. At 1.0 ours cleared 71-78%
// alone and walked the map; at 0 (with cohesion) the whole team piled onto
// one group (2.2 a gem group); 0.4 per teammate gave 62/23/5/9%, 1.7, but
// 21% of opening clears with 4+ (theirs 9%) and a third of their captures of
// small gem-only groups (theirs 3.7-5.9 by round 300, ours 1.4).
inline constexpr float partner_penalty = 1.0f;
// Squad assembly: converging on surviving allies is the real regroup.
inline constexpr float depleted_rally_bonus = 3.0f;
// Fraction of the team that must still be alive for the squad to count as
// assembled. Below it the rally bias turns on. 0.0 disables regrouping entirely.
inline constexpr float depleted_alive_fraction = 0.5f;
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
inline constexpr float siege_gem_weight = 2.0f;
// Divisor on the risk penalty for gem-bearing camps on a fortress-gem map.
// 1.0 = normal; below 1 accepts a losing exchange because the alternative is a
// guaranteed 0-gem defeat.
inline constexpr float siege_risk_weight = 0.06f;
// Extra tiles beyond an enemy's reach plus move that still count as danger.
inline constexpr int worker_danger_margin = 1;
// Rounds an enemy out of sight is still counted after its last sighting
// (safety.cpp's ghosts): after two a walking enemy can be two tiles from
// where it was seen, the margin of every reach check, and the tiles it could
// be on say little. Measured: a ghost two rounds old is near its enemy two
// times in three, one four rounds old one time in three, and kept while
// updated (the old rule) four fifths of them were phantoms.
inline constexpr int ghost_lifetime = 2;
// Camps near a recently seen enemy are worth worker_sighting_penalty of their
// value for worker_sighting_memory rounds.
inline constexpr int worker_sighting_memory = 20;
inline constexpr float worker_sighting_penalty = 0.2f;
// Banking: whatever a champion carries is banked on its way out from spawn
// and whenever an altar is within bank_nearby steps, as the top three bank on
// every trip home (about 20 banks a game of 28-39 gems; ours 6-10).
inline constexpr int bank_nearby = 3;
// Prism normally banks this full bag (willow best girl's median: 198). On a
// no-gold map, before one full Health emblem's growth, it banks a small bag
// while the enemy has not been seen recently, but keeps enough gems for combat
// until the forced cap: gemcombat4 is fight-heavy, gemcombat5 is not.
inline constexpr unsigned prism_bag = 200;
inline constexpr unsigned prism_fragile_bag = 30;
inline constexpr unsigned prism_fragile_forced_bag = 100;
inline constexpr unsigned prism_health_shards = 5;
inline constexpr int prism_bank_danger_memory = 20;
// A champion that has lost this much health to enemy champions before its
// first shard opens with its first emblem's Health version: on engine 2.0.0
// willow best girl's Heart of Ruin takes Hunter-Health in 47 of 65 games at
// 500 or more and Hunter-Focus in 38 of 47 under 250, its Marcella
// Prospector-Health in 31 of 47 at 500 or more. Its Vendetta and Willow do
// not switch.
inline constexpr float health_treasure_damage = 500;
// A champion that lost this much health to enemy champions over the last
// 100-150 rounds starts its build's next Health emblem when it starts a new
// one (next_item()): willow best girl's Health share of new emblems is
// 61-64% after 800-1199 and 71-77% after more (30-31% under 150).
inline constexpr float health_pull_damage = 800;
// Altar locks, as willow best girl places them (about 4 a game): a champion
// buys one at spawn when an altar wants one (lock_wanted(): the home altars,
// or one an enemy banked at within lock_memory rounds) and places it when it
// passes within lock_range of one.
inline constexpr int lock_memory = 100;
inline constexpr int lock_range = 2;
// Camp choice prices a clear longer than our health lasts as trips home, not
// as the risk of dying (camps never regenerate, and a team's clear shares the
// camps' hits); visit_risk caps the risk of each such visit. With no contest
// factor on camps nearer the enemy, it won 48 of 80 head to head and 78 of 80
// against db11 (75): monolith captures late in the game rose fivefold.
inline constexpr float visit_risk = 0.25f;
// Value multiplier for the camp a champion is already heading to, so it
// finishes camps instead of switching between them.
inline constexpr float worker_commit_bonus = 1.08f;
// Weight of the rounds spent waiting for a respawning camp in camp choice's
// duration, against the rounds spent travelling and clearing. willow best
// girl farms a small territory: 72% of its clears before round 400 are of
// groups it cleared before, 8 tiles from the last (ours at 3: 55%, 10 tiles,
// and twice its long gaps between clears), so waiting counts as plain time.
inline constexpr float respawn_wait_weight = 1.0f;
// Camp choice clears a camp at the team's summed camp rate (ours and its
// claimants') times team_clear_share.
// With a helper the camps strike the tank: a ranged champion takes
// ranged_helped_loss of the camps' damage and a melee one, the tank,
// melee_helped_loss. A champion with a dash walks dash_travel_scale of the
// steps.
inline constexpr float team_clear_share = 0.85f;
inline constexpr float ranged_helped_loss = 0.15f;
inline constexpr float melee_helped_loss = 1.5f;
inline constexpr float dash_travel_scale = 0.8f;
// Camp choice discounts a plan by its length, dividing its score by
// 1 + plan_hazard * duration (rounds): a long plan is more likely to be cut
// short by a fight, a death or a respawn. willow best girl captures clusters
// first (median round 16-38, in 52-57% of the games that have them) and
// wolves by round 38-65; ours, valuing a far golem group above a near
// cluster, 30% of those games and round 102.
inline constexpr float plan_hazard = 0.02f;
// Camp choice adds a group's walk to the nearest altar, up to altar_walk_cap
// tiles, to its duration at 0.25 rounds a tile, and a gem group's at
// scarce_altar_weight / (altar count) more: with few altars every gem is
// tributed at the same tiles, where the enemy waits and locks cost the most,
// so the gems near them are worth the more. 0 disables the scarcity term.
inline constexpr int altar_walk_cap = 20;
inline constexpr float scarce_altar_weight = 0.5f;
// A heal waits for a teammate missing heal_trigger of a full heal (3 x
// Focus for Willow): willow best girl's Willow never heals one missing
// under 200 health (none of its casts in either sample; full heal 360) and
// lands 310-324 a cast, ours healed teammates missing under 150 in 37% of
// its casts (72 each), spending the 8-round cooldown for little.
inline constexpr float heal_trigger = 0.55f;
// The Willow's camp choice counts the group its Prism claims willow_follow
// times: willow best girl's Willow is nearest its Prism 35-37% of the time
// (ours 27-30% farming spread) and heals as much spread as clumped.
inline constexpr float willow_follow = 2.0f;
// Camp choice also divides a group's score by 1 + travel_hazard * (travel +
// waiting) (the walk there and the wait for its respawn, in rounds; with the
// walk alone ours stood beside dead groups 140 rounds a game in rounds
// 150-400 to willow best girl's 40): relative to the walk, ours weighed gems 2-3
// times, gold 4-6, monster health 2.5-3 and teammates' groups 1.5-1.8
// times as much as willow best girl's choices (a logit of its new targets,
// both samples), and walked past near groups to far ones. Travel already enters
// duration and plan_hazard, so 0.10 charged distant groups too strongly a third
// time; 0.07 keeps that safety discount nonzero while raising the full 70-map,
// two-seat result from 60/140 to 76/140 against baseline at 100000 fuel.
inline constexpr float travel_hazard = 0.07f;
// Ordinary camps farther than the map's natural camp spacing from every live
// teammate claim lose this much value per excess tile. Same-camp crowding
// still spreads champions onto neighbouring groups; this only discourages
// isolated trips that leave nobody within reinforcement distance. 0 disables.
inline constexpr float cohesion_hazard = 0.03f;
// Marcella's in place of travel_hazard: willow best girl's Marcella weighs
// a group's walk the least of its types (logit -2.39; ours -2.94) and takes
// one-elk groups in 41% of its new targets on the same maps (ours 26%).
inline constexpr float marcella_travel_hazard = 0.0f;
// A group nearer the enemy spawn than ours (walking) scores
// enemy_side_factor of its value: willow best girl's choices avoid the
// enemy half more than ours did (logit -0.7..-0.9 against -0.5, ours
// alone -0.3..-0.4), and its lone contacts start 12-15 tiles from home
// (ours 18-19), so its lone champions are out of reach sooner.
inline constexpr float enemy_side_factor = 0.4f;
// An altar's risk for each enemy champion near it: this much, and
// altar_enemy_strength_risk per point of its strength (worker_tribute()).
inline constexpr float altar_enemy_risk = 6.0f;
inline constexpr float altar_enemy_strength_risk = 0.05f;
// A fight step's tile score: its walk times 10 plus the damage that can reach
// it times this (attack_combo()'s step).
inline constexpr float step_incoming_weight = 0.14f;
// The team's elected detour runner counts an unclaimed live gold-bearing camp
// whose full clear and capture fit inside a teammate's cooling reservation with
// at least this enemy-side factor. The teammate remains the reservation; this
// champion need not return before the respawn. Recent sightings and every
// normal travel/risk term still apply, so only the idle window is relaxed.
inline constexpr float cooling_detour_enemy_side_factor = 0.8f;
// The Prism's camp choice counts a safe gem group (harmless clusters only)
// prism_safe_gem_factor times: willow best girl's Prism takes one in 39-40%
// of its new targets when one is up (ours 22%) and keeps its bag for the
// Focus it gives, so its teammates hold the monster captures.
inline constexpr float prism_safe_gem_factor = 2.0f;
// Behind in the gem race (behind_pressure()): how much cheaper a death looks.
inline constexpr float behind_risk_relief = 0.3f;
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
// leaving only of a recalling holder's claim, so one bit holds both. After the
// fields, one bit per slot says that champion carries a Jungle emblem (camp
// damage taken -30%), for a hard camp's party whose tank is out of view; the
// last four bits hold the Prism's bag in steps of 16 gems (it banks only a
// big bag, so the banked count alone reads us as behind).
// Zero is idle; the top codes are unavailable/bank/recall, never camp identifiers.
struct TeamMemory {
    static constexpr unsigned bits = 10, mask = (1u << bits) - 1;
    static constexpr unsigned field = bits + 1, field_mask = (1u << field) - 1;
    static constexpr unsigned bank = mask - 1, recall = mask;
    static constexpr unsigned unavailable = mask - 2;
    static constexpr unsigned slots = 5;
    static constexpr unsigned jungle_shift = slots * field;
    static constexpr unsigned bag_shift = jungle_shift + slots, bag_step = 16, bag_max = 15;
    static_assert(bag_shift + 4 <= 64);
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
    bool jungle(unsigned slot) const { return slot < slots && (raw >> (jungle_shift + slot) & 1); }
    unsigned prism_bag() const { return unsigned(raw >> bag_shift & bag_max) * bag_step; }
    void set_prism_bag(unsigned gems) {
        const std::uint64_t steps = std::min(bag_max, gems / bag_step);
        if ((raw >> bag_shift & bag_max) == steps)
            return;
        read();
        raw = (raw & ~(std::uint64_t(bag_max) << bag_shift)) | (steps << bag_shift);
        bc::shared_memory.write(0, std::span{&raw, 1});
    }
    void set_jungle(unsigned slot, bool value) {
        if (slot >= slots || jungle(slot) == value)
            return;
        read();
        const std::uint64_t bit = std::uint64_t{1} << (jungle_shift + slot);
        raw = value ? raw | bit : raw & ~bit;
        bc::shared_memory.write(0, std::span{&raw, 1});
    }
    void set_leaving(unsigned slot, bool value) {
        own_leaving = value;
        write(slot, get(slot));
    }

  private:
    // Our own flags, kept here: the field holds the one its claim carries.
    bool own_attacked = false, own_leaving = false;
    unsigned request_low = 1, request_high = 0;
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
    float spacing = 0;
    // Walking distance from each camp group, in camp_groups() order, to our
    // spawn, the enemy spawn and an altar's tribute tiles: three map-wide
    // searches the draft affords on its first turn, which a champion's first
    // turn cannot. group_count is 0 when they are not here.
    static constexpr std::size_t max_groups = 130;
    enum Ground { home, enemy, altar };
    std::uint16_t group_count = 0;
    std::array<std::array<std::uint16_t, 3>, max_groups> group_distance{};
    // And each group's splash shape: the most camps within splash radius of
    // one of its camps (at least 1), counted over every pair.
    std::array<std::uint8_t, max_groups> group_splash{};
};
static_assert(sizeof(DraftPlan) <= 1024);

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
// Divisor applied to the risk penalty of a gem-bearing camp. Normal maps keep
// the full penalty; a fortress-gem map must accept the exchange.
inline float gem_risk_weight(const DraftPlan &plan) {
    return fortress_gems(plan) ? policy::siege_risk_weight : 1.0f;
}

// A damaging dash (Heart of Ruin) counts as a damage spell.
// A unit's fighting power, ours, an ally's, an enemy's or a type's: its
// health with its shields, against what it deals (its damage rate, a fifth of
// its ready burst, its healing), over its mitigation. Four formulas left our
// own shields and healing out where an enemy's counted them: a lone Willow
// undervalued itself.
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
