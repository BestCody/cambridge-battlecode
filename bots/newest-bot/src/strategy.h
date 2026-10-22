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
// A gold's value in gems in camp choice: early_gold_value at round 0, falling
// linearly to late_gold_value by half the game. Early gold buys the first
// shards that decide the first fights: after the clear-model calibrations
// favoured long gem clears, 0.26 left the team without them on small maps,
// where rushing enemies arrived mid-clear. 0.65 won 14 more of 490 games and
// cut 2.8 deaths a game against the old bots.
inline constexpr float early_gold_value = 0.65f;
inline constexpr float late_gold_value = 0.10f;
inline constexpr float aggression = 1.60f;
// Camp choice's factor on contested camps (enemy no more than 8 tiles further
// than us) is 1 + contest_bonus: steering away from them cut deaths by a fifth.
inline constexpr float contest_bonus = -0.30f;
inline constexpr float commit_bonus = 1.08f;
inline constexpr float retreat_health = 0.28f;
inline constexpr float casualty_margin = 0.12f;
inline constexpr float bank_base = 56.0f;
inline constexpr float bank_distance = 1.6f;
inline constexpr float bank_cap = 120.0f;
inline constexpr int shopping_shards = 5;
inline constexpr int replan_period = 4;
inline constexpr float assist_finish_credit = 0.55f;
inline constexpr float stun_followup_credit = 0.5f;
inline constexpr float unused_cast_cost = 0.01f;
inline constexpr float formation_advantage = 1.10f;
// Camp choice divides a group's score by 1 + partner_penalty per teammate
// already claiming it (whose help it also counts as faster clearing). The team
// gets the whole reward whoever captures it, so no penalty: champions join each
// other on big groups, as the top team does.
inline constexpr float partner_penalty = 0.0f;
// Cassi and Issac share one fate: one death drops both bags, so the pair banks
// smaller amounts more often than a solo champion. 1.0 disables the adjustment.
inline constexpr float twin_bank_scale = 0.6f;
// Squad assembly. Avoiding contested camps when depleted concedes the map;
// converging on surviving allies is the real regroup.
// depleted_penalty 1.0 disables the avoid-contest variant.
inline constexpr float depleted_penalty = 1.0f;
inline constexpr float depleted_rally_bonus = 3.0f;
// Fraction of the team that must still be alive for the squad to count as
// assembled. Below it the rally bias turns on. 0.0 disables regrouping entirely.
inline constexpr float depleted_alive_fraction = 0.5f;
// Decisions never depend on the fuel left, so the same game plays the same at
// any fuel budget.
// Tiles of the crystal hop map built per turn (two passes over the map).
inline constexpr unsigned hop_tiles_per_turn = 400;
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
// Bounded search widths, not map/champion-specific rules.
inline constexpr unsigned objective_finalists = 6;
inline constexpr unsigned firing_trials = 6;
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
// Worker policy (Issac, Ryth, Varg). true runs them as pure resource gatherers:
// they never start fights, step away from approaching enemies, avoid
// contested camps and bank smaller bags. Offense and defense belong to the
// squad. false restores the older aggressive worker loop entirely.
inline constexpr bool worker_pure_resource = false;
// Extra tiles beyond an enemy's reach plus move that still count as danger.
inline constexpr int worker_danger_margin = 1;
// A worker in danger range keeps farming unless an enemy can already hit it,
// is closing in, or enemy power nearby exceeds ours times this margin.
// 0.0 restores leaving whenever any enemy is in danger range.
inline constexpr float worker_strength_margin = 1.0f;
// Health fraction at which a worker goes home; twins may differ because a
// death also kills their partner. Both currently match retreat_health.
inline constexpr float worker_retreat_health = 0.28f;
inline constexpr float twin_retreat_health = 0.28f;
// Whether a worker also goes home when its health drops to twice the damage it
// is taking (camp damage included). The old bots retreat on low health only.
// false: a pure worker leaves clears only for low health or approaching enemies.
inline constexpr bool worker_damage_retreat = true;
// Value multiplier for camps closer to the enemy spawn than ours. 1.0 disables.
inline constexpr float worker_enemy_side_penalty = 0.7f;
// Rounds a worker remembers an enemy near a camp, and the value multiplier
// applied to that camp meanwhile. 0 rounds disables the memory.
inline constexpr int worker_sighting_memory = 20;
inline constexpr float worker_sighting_penalty = 0.2f;
// Multiplier on worker bank thresholds: smaller bags are lost less often.
// 1.0 restores the old thresholds.
inline constexpr float worker_bank_scale = 0.5f;
// Obstruction squad (stage 1). true lets the squad break off its camp job to
// interrupt enemy tributes and hunt carriers it can kill, and biases its camp
// choice toward the enemy side and the altar enemies use. false restores the
// farming-only squad.
inline constexpr bool squad_obstruction = false;
// Tiles from the coordinator within which an enemy can become a hunt target.
inline constexpr int hunt_radius = 8;
// Bag size that makes a lone enemy worth chasing even without a sure kill.
inline constexpr unsigned hunt_min_gems = 20;
// Hunts are refused or abandoned when enemy power near the squad exceeds ours
// times this margin.
inline constexpr float hunt_abort_margin = 1.2f;
// Rounds before an unfinished hunt is abandoned.
inline constexpr int hunt_max_rounds = 15;
// Camp selection multipliers for the squad's fallback farming: camps on the
// enemy's side, and camps near the watched altar. 1.0 disables each.
inline constexpr float squad_enemy_side_bonus = 1.5f;
inline constexpr float squad_altar_bonus = 1.5f;
inline constexpr int squad_altar_watch_radius = 10;
// Whether Stellar, Tavi and Cassi run the coordinated squad controller. false
// plays every champion with the independent worker policy.
inline constexpr bool squad_enabled = false;
// Squad job planning. A plan is refreshed on any material change; without one
// it expires after this many work windows. 1 restores the old age-only refresh.
inline constexpr int plan_lifetime_windows = 3;
// Expected delay per round of overlap between a hidden enemy's earliest arrival
// and our reward. Hidden enemies may be elsewhere, so this is below 1.
// 0 ignores hidden enemies entirely.
inline constexpr float hidden_interference_weight = 0.5f;
// Capture timers lost to contest before the current job loses its commitment
// bonus and must win the comparison against other jobs on its own merit.
inline constexpr unsigned capture_reset_limit = 2;
// Control credit multiplier for stunning an enemy other than the shared focus
// target while it threatens a member. 1.0 disables the preference.
inline constexpr float secondary_control_bonus = 1.25f;
// Replica of the top ladder team's play (see docs/design-notes.md): every
// champion farms its own side and fights, banking small bags often.
// Carries are Stellar and Heart of Ruin.
// Gems carried before a champion walks to bank. They also bank on passing an
// altar with policy::fish_bag_nearby or more.
inline constexpr unsigned fish_bag_carry = 28;
inline constexpr unsigned fish_bag_other = 20;
inline constexpr unsigned fish_bag_nearby = 10;
// Gold, in shards, that sends a champion home to shop. Carries earn about
// three times the gold of the others and shop about every 100 rounds.
inline constexpr int carry_shopping_shards = 10;
// Builds of whole emblems, as the top team buys them (only a finished emblem
// gives its passive): carries Treasure, Power, Home and Durability emblems,
// then Power of Focus; Ren Bruiser and Durability emblems, then Power of
// Focus. One shard of each instead (no passives) lost 150 to 121 in 210
// games, with 5.5 more deaths a game.
inline constexpr bool full_emblems = true;
// Heart of Ruin hits up to 50% harder at low health and shields itself, so it
// stays in fights longer than the others.
inline constexpr float heart_retreat_health = 0.15f;
// The top team never buys altar locks.
inline constexpr bool buy_altar_locks = false;
// Value multiplier for the camp a champion is already heading to, so it
// finishes camps instead of switching between them.
inline constexpr float worker_commit_bonus = 1.08f;
// Whether a low-health champion an enemy can reach next turn fights back
// instead of fleeing. Fleeing loses some free hits but survives far more often:
// standing traded down to death in two thirds of the fights we were losing,
// where the top team breaks contact. false always flees.
inline constexpr bool stand_when_caught = false;
// Teleport crystals: buy one at spawn, and use it, only when one hop over a
// wall or hole strip saves at least this many turns over walking to the next
// camp or altar, after paying for the channel. Shards are bought first.
inline constexpr int crystal_min_saving = 8;
// Spell-range siege (Stellar, Cassi): only when no ally is closer to the camp.
// Camps attack the nearest champion, so with a closer ally we attack from
// normal range. false sieges whenever a solo clear would be too costly.
inline constexpr bool siege_alone_only = true;
// Whether melee champions with a long-range spell (Ren, Ryth) siege too: they
// then stand beyond the camps' reach casting once a cooldown (about 150 turns a
// game each). false leaves sieges to the ranged casters.
inline constexpr bool siege_melee = false;
// Weight of the rounds spent waiting for a respawning camp in camp choice's
// duration, against the rounds spent travelling and clearing. Waiting wastes
// the turns another camp could use; the top team moves on and comes back.
// 2-3 beat 1 by about 40 wins in 630 games; 5 was worse again.
inline constexpr float respawn_wait_weight = 3.0f;
// Team cohesion in camp choice. The top team farms neighbouring camps about
// 8 tiles apart, close enough to join each other's fights. A camp farther than cohesion_radius from every
// teammate's claimed camp or visible position has its value divided by
// 1 + cohesion_weight * excess / cohesion_radius. 0 disables. Since camp
// choice counts free nearby allies as helpers, 1.0 kept the whole team in one
// blob where the top team splits into groups of 3-4; 0.25 beat 1.0 by 22 wins
// in 560 games over five opponent sets (0.75 by 6), while 0 and 1.5 lost.
inline constexpr int cohesion_radius = 8;
inline constexpr float cohesion_weight = 0.25f;
// Camp choice counts the living allies within partner_radius that are free (no
// claim, or a claim on a finished camp) as helpers on every camp, besides the
// camp's claimants: a group moving together clears any camp at the group's
// pace. Scored alone, an elk camp loses to the gold cluster the group already
// shares, so the group never starts one; the top team opens on elk camps
// together. 6 beat claimants only (0) by 16 wins in 350 games, with fewer
// deaths and gems by round 100 up from 17 to 22; 4 and stopping at round 300
// gained less, and counting every nearby ally, free or not, lost.
inline constexpr int partner_radius = 6;
// Camp choice's clear rate with helpers: our own rate times 1 + helper_speed
// per helper, for both the clear time and the health it costs. Fitted to our
// measured clear times by group size (1.3; 0.65 fitted worst): each helper
// adds about a champion's damage and more, as one tank takes the camp's hits.
// The old 0.65 made big camps look slow and risky to a group, so it took
// small ones; 1.3 won 10 more of 490 games and raised gems by round 300 from
// 85 to 97, with more elk and fewer wolf clears early.
inline constexpr float helper_speed = 1.3f;
// Camp loss (camp choice's risk, the siege decision) by the number of camps
// that strike back, fitted to measured clears (see camp_loss_factor in bot.h);
// false: 0.68 for every group of several camps, which overstated the cost of
// two- and three-monster camps 1.4-3x. Won 16 more of 490 games (46 of 70 head
// to head) and raised gems by round 300 from 97 to 107.
inline constexpr bool fitted_camp_loss = true;
// One holder per capture: with no enemy in view, a champion on its way to (or
// standing on) a camp being captured that sees a higher-ranked ally on it
// takes the next camp instead. Carries (Stellar, Heart of Ruin) rank first, so
// they collect the rewards' gold. The top team holds 91% of captures with one
// champion and its carries earn most of its gold. Won 48 of 70 against the
// previous commit with a better margin against every opponent; first come
// holding (no carry rank) raised deaths and lost to the old bots.
inline constexpr bool single_holder = true;
} // namespace policy

// Next-round action order. Tiers are disjoint so an enabling stun never
// outranks a finishing blow, whatever the raw damage values.
namespace priority_tier {
inline constexpr int idle = 1000;
inline constexpr int damage = 2000;
inline constexpr int control = 3000;
inline constexpr int finish = 4000;
inline constexpr int span = 999;
} // namespace priority_tier

// Published mechanics not exposed in the SDK settings structure.
namespace rules {
inline constexpr int crystal_range = 3;
inline constexpr int altar_lock_range = 2;

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
                    static_cast<int>(std::ceil(gems * rules::settings().submit_gem_duration_multiplier)) +
                        rules::settings().submit_gem_base_duration);
}
inline unsigned id(const bc::champion &c) {
    return static_cast<unsigned>(static_cast<bc::handle_champion_t>(c));
}
inline unsigned id(const bc::camp &c) { return static_cast<unsigned>(static_cast<bc::handle_camp_t>(c)); }

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

// Fixed wire format. Slots are allied champion handles in ascending order,
// including both bodies of the twin pick. Six 10-bit claims fit in eight bytes.
// Zero is idle; the top codes are unavailable/bank/recall, never camp identifiers.
struct TeamMemory {
    static constexpr unsigned bits = 10, mask = (1u << bits) - 1;
    static constexpr unsigned bank = mask - 1, recall = mask;
    static constexpr unsigned unavailable = mask - 2;
    static constexpr unsigned recovered = mask - 3;
    static constexpr unsigned supplies_needed = mask - 4;
    // A worker stepping away from an approaching enemy. Squad defense will
    // read it; deposit codes stop below it so the two can never collide.
    static constexpr unsigned under_threat = mask - 5;
    // The squad is hunting the enemy named by the shared focus field.
    static constexpr unsigned hunting = mask - 6;
    static constexpr unsigned slots = 64 / bits;
    // Normal objective claims retain their original codes. Extra codes carry
    // an observed fight's nearest objective and desired total squad size, or
    // identify a champion travelling to that fight. Each owner refreshes only
    // its own slot; responders never turn an old request into a new request.
    static bool formations(unsigned groups) { return groups && groups * (slots + 2) < bank; }
    static unsigned request(unsigned group, unsigned force, unsigned groups) {
        return groups + 1 + group * slots + std::clamp(force, 1u, slots) - 1;
    }
    static unsigned joining(unsigned group, unsigned groups) { return groups * (slots + 1) + 1 + group; }
    // Holders retain ownership of the current resource operation.
    static bool captures(unsigned groups) { return groups && groups * (slots + 3) < supplies_needed; }
    static unsigned holding(unsigned group, unsigned groups) {
        return captures(groups) ? groups * (slots + 2) + 1 + group : group + 1;
    }
    static int held_group(unsigned code, unsigned groups) {
        const unsigned start = groups * (slots + 2) + 1;
        return captures(groups) && code >= start && code < start + groups ? int(code - start) : -1;
    }
    static int objective(unsigned code, unsigned groups) {
        if (code && code <= groups)
            return int(code - 1);
        const int held = held_group(code, groups);
        return held >= 0 ? held : fight_group(code, groups);
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
    // These codes follow all worker formation/capture codes. One altar and a
    // small owner state fit without claiming another actor's slot or vision.
    static constexpr unsigned bank_stages = 10;
    static unsigned bank_start(unsigned groups) { return groups * (slots + 3) + 1; }
    static unsigned deposit(unsigned altar, unsigned stage, unsigned groups) {
        const unsigned code = bank_start(groups) + altar * bank_stages + stage;
        return code < hunting ? code : unavailable;
    }
    static int deposit_altar(unsigned code, unsigned groups, unsigned altars) {
        const unsigned start = bank_start(groups);
        return code >= start && code < hunting && code - start < altars * bank_stages
                   ? int((code - start) / bank_stages)
                   : -1;
    }
    static unsigned deposit_stage(unsigned code, unsigned groups) {
        return (code - bank_start(groups)) % bank_stages;
    }
    std::uint64_t raw = 0;
    void read() { bc::shared_memory.read(0, std::span{&raw, 1}); }
    unsigned get(unsigned slot) const { return slot < slots ? (raw >> (slot * bits)) & mask : 0; }
    unsigned focus() const { return static_cast<unsigned>(raw >> 60); }
    void set(unsigned slot, unsigned value) {
        // Observation refreshed the word and no teammate acts during this
        // turn. Each owner is the only writer of its own claim.
        if (slot >= slots || get(slot) == (value & mask))
            return;
        read(); // Preserve claims written by earlier teammates this round.
        raw =
            (raw & ~(std::uint64_t(mask) << (slot * bits))) | (std::uint64_t(value & mask) << (slot * bits));
        bc::shared_memory.write(0, std::span{&raw, 1});
    }
    void commit(unsigned slot, unsigned value, unsigned victim) {
        if (slot >= slots || value > mask || victim >= 15)
            return;
        read();
        raw = (raw & ~(std::uint64_t(mask) << (slot * bits))) | (std::uint64_t(value) << (slot * bits));
        raw = (raw & ((std::uint64_t(1) << 60) - 1)) | (std::uint64_t(victim) << 60);
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
    float mean_splash = 1, dangerous_fraction = 0;
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
    bc::bitboard goals;
    for (auto p : altars)
        goals |= around(p, rules::settings().submit_gems_range);
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

inline float spell_dps(const std::array<Spell, 2> &spells, float strength, float focus, float splash = 1) {
    float result = strength * splash;
    for (const auto &s : spells) {
        if (s.kind == bc::ability_kind::attack_reset)
            result += strength * splash / s.cooldown;
        if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc)
            result += focus * s.modifier / s.cooldown;
        if (s.kind == bc::ability_kind::aoe_damage)
            result += focus * s.modifier * splash / s.cooldown;
    }
    return result;
}
