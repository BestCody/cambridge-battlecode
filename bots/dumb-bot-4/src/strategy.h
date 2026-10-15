#pragma once
#include <algorithm>
#include <array>
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
// The benchmark script creates isolated variants of this file for experiments.
namespace policy {
inline constexpr float early_gold_value = 0.26f;
inline constexpr float late_gold_value = 0.10f;
inline constexpr float aggression = 1.60f;
inline constexpr float contest_bonus = 0.22f;
inline constexpr float commit_bonus = 1.18f;
inline constexpr float retreat_health = 0.28f;
inline constexpr float casualty_margin = 0.12f;
inline constexpr float bank_base = 42.0f;
inline constexpr float bank_distance = 1.6f;
inline constexpr float bank_cap = 120.0f;
inline constexpr int shopping_shards = 5;
inline constexpr int replan_period = 4;
inline constexpr int rush_size = 3;
inline constexpr float rush_contest = 1.2f;
inline constexpr float draft_spell_weight = 3.0f;
inline constexpr float draft_melee_bonus = 0.0f;
inline constexpr float draft_heal_bonus = 0.0f;
inline constexpr float draft_retaliation_bonus = 0.0f;
inline constexpr float draft_counter_weight = 15.0f;
inline constexpr float focus_weight = 1.5f;
inline constexpr float attack_focus_bonus = 1.15f;
inline constexpr float assist_finish_credit = 0.55f;
inline constexpr float stun_followup_credit = 0.5f;
inline constexpr float unused_cast_cost = 0.01f;
inline constexpr float formation_advantage = 1.10f;
inline constexpr int squad_size = 3;
inline constexpr int squad_rounds = 180;
inline constexpr bool treasure_first = true;
inline constexpr float spread_gold_value = 0.26f;
inline constexpr float partner_penalty = 0.35f;
inline constexpr bool adaptive_formation = true;
inline constexpr float squad_spacing = 15.0f;
inline constexpr float spread_bank_base = 95.0f;
inline constexpr float spread_bank_distance = 1.0f;
inline constexpr int spread_shopping_shards = 5;
inline constexpr std::uint64_t planning_reserve = 30000;
inline constexpr std::uint64_t action_reserve = 12000;
inline constexpr std::uint64_t ward_reserve = 15000;
inline constexpr std::uint64_t observation_reserve = 50000;
inline constexpr bool diagnostic = true;
} // namespace policy

inline int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}
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
inline unsigned id(const bc::camp &c) {
    return static_cast<unsigned>(static_cast<bc::handle_camp_t>(c));
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

// Fixed wire format. Slots are allied champion handles in ascending order,
// including both bodies of the twin pick. Six 10-bit claims fit in eight bytes.
// Zero is idle; the two top codes are bank/recall, never camp identifiers.
struct TeamMemory {
    static constexpr unsigned bits = 10, mask = (1u << bits) - 1;
    static constexpr unsigned bank = mask - 1, recall = mask;
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
    void read() { bc::shared_memory.read(0, std::span{&raw, 1}); }
    unsigned get(unsigned slot) const { return slot < slots ? (raw >> (slot * bits)) & mask : 0; }
    unsigned focus() const { return static_cast<unsigned>(raw >> 60); }
    void focus(unsigned identity) {
        if (identity >= 15 || identity == focus())
            return;
        read();
        raw = (raw & ((std::uint64_t(1) << 60) - 1)) | (std::uint64_t(identity) << 60);
        bc::shared_memory.write(0, std::span{&raw, 1});
    }
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
};

struct DraftPlan {
    static constexpr std::uint32_t tag = 0x41454732;
    std::uint32_t magic = tag;
    std::array<bc::champion_type, TeamMemory::slots> roster_types{};
    std::array<std::uint16_t, static_cast<unsigned>(bc::champion_type::tavi) + 1> opening{};
    std::uint16_t group_count = 0;
    std::uint16_t rally_group = 0, rally_until = 0;
    std::uint16_t rush_size = 0, squad_size = 0;
    bool assault = false;
    float focus_weight = 1, spacing = 0;
    float mean_splash = 1, dangerous_fraction = 0, contestation = 0;
};
static_assert(sizeof(DraftPlan) <= 1024);

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
