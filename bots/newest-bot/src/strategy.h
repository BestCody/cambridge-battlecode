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
inline constexpr float early_gold_value = 0.26f;
inline constexpr float late_gold_value = 0.10f;
inline constexpr float aggression = 1.60f;
inline constexpr float contest_bonus = 0.22f;
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
inline constexpr float partner_penalty = 0.35f;
// Cassi and Issac share one fate: one death drops both bags, so the pair banks
// smaller amounts more often than a solo champion. 1.0 disables the adjustment.
inline constexpr float twin_bank_scale = 0.6f;
// Squad assembly. Avoiding contested camps when depleted measured harmful (it
// concedes the map); converging on surviving allies is the real regroup.
// depleted_penalty 1.0 disables the avoid-contest variant.
inline constexpr float depleted_penalty = 1.0f;
inline constexpr float depleted_rally_bonus = 3.0f;
// Fraction of the team that must still be alive for the squad to count as
// assembled. Below it the rally bias turns on. 0.0 disables regrouping entirely.
inline constexpr float depleted_alive_fraction = 0.5f;
inline constexpr std::uint64_t planning_reserve = 30000;
inline constexpr std::uint64_t action_reserve = 12000;
inline constexpr std::uint64_t ward_reserve = 15000;
inline constexpr std::uint64_t observation_reserve = 50000;
inline constexpr bool diagnostic = true;
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
// death also kills their partner. Measured: 0.50/0.60 saved no lives over
// 0.28 and cost about 11 worker gems a game, so both match retreat_health.
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
// Heart of Ruin hits up to 50% harder at low health and shields itself, so it
// stays in fights longer than the others.
inline constexpr float heart_retreat_health = 0.15f;
// The top team never buys altar locks.
inline constexpr bool buy_altar_locks = false;
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
} // namespace rules

inline int distance(vec2 a, vec2 b) { return static_cast<int>(a.chebyshev_distance(b)); }
inline bc::bitboard around(vec2 p, float radius) {
    bc::bitboard b;
    b.draw_chebyshev(p, radius);
    return b;
}
inline bc::bitboard terrain() {
    return bc::map::walls() | bc::map::holes() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy);
}
inline int channel_duration(unsigned gems) {
    return std::max(bc::constants::submit_gem_minimum_duration,
                    static_cast<int>(std::ceil(gems * bc::constants::submit_gem_duration_multiplier)) +
                        bc::constants::submit_gem_base_duration);
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
};
static_assert(sizeof(DraftPlan) <= 1024);

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
