#include "bot.h"

bool Bot::move_step(vec2 tile) {
    auto &me = bc::self();
    if (!me.can_move() || tile == me.position())
        return false;
    if (me.move_absolute(tile)) {
        walked = true;
        position = me.position();
        return true;
    }
    // Hidden occupancy reveals the blocked tile without spending the allowance.
    blocked |= bc::vision::champion_tiles() | bc::vision::camp_tiles();
    blocked.set(me.position(), false);
    return false;
}

bool Bot::cast_mobility(const bc::distance_field &field, bool urgent, int engaged_group) {
    auto &me = bc::self();
    for (auto a : me.abilities()) {
        if (!a.is_dash() || !a.is_ready())
            continue;
        const auto design = describe(a);
        if (walked && design.exclusive)
            continue;
        auto dash_blocked = bc::map::walls() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy) |
                            bc::vision::camp_tiles() | bc::vision::champion_tiles() |
                            bc::vision::teleport_tiles();
        dash_blocked.set(position, false);
        auto legal = dash_blocked.reachable_tiles(position, static_cast<unsigned>(design.range)) & ~blocked;
        auto choices = legal.positions();
        for (int attempt = 0; attempt < 3; ++attempt) {
            vec2 best = position;
            float original = field.get(position), score = original;
            for (auto tile : choices) {
                if (bc::fuel_remaining() < policy::action_reserve)
                    break;
                if (tile == position || blocked.is_set(tile))
                    continue;
                float value = field.get(tile) + incoming(tile, engaged_group) * (urgent ? 0.06f : 0.012f);
                if (value < score) {
                    score = value;
                    best = tile;
                }
            }
            if (best == position || (!urgent && original - score < 1.75f))
                return false;
            if (a.use(best)) {
                position = me.position();
                if (design.exclusive)
                    walked = true;
                return true;
            }
            blocked |= bc::vision::champion_tiles();
            blocked.set(best);
            blocked.set(position, false);
        }
    }
    return false;
}

bool Bot::move_to(bc::bitboard goals, bool urgent, bool use_dash, int engaged_group) {
    auto &me = bc::self();
    if (me.is_teleporting() || me.is_recalling() || me.is_submitting_gems())
        return false;
    if (bc::fuel_remaining() < policy::planning_reserve)
        return false;
    goals &= ~static_blocked;
    if (goals.is_empty() || goals.is_set(position))
        return false;
    bc::distance_field field;
    bc::bitboard visited;
    field.compute_to(blocked, goals & ~blocked, around(position, 0), visited);
    if (bc::fuel_remaining() < policy::action_reserve)
        return false;
    bool breaking_through = false;
    if (field.get(position) == bc::distance_field::unreachable &&
        bc::fuel_remaining() > policy::planning_reserve) {
        // Camp bodies can seal an entire corridor. Route through destructible
        // obstacles, then clear the next blocking group instead of idling.
        auto temporary = static_blocked | bc::vision::champion_tiles() | bc::vision::teleport_tiles();
        temporary.set(position, false);
        field.compute_to(temporary, goals & ~temporary, around(position, 0), visited);
        breaking_through = field.get(position) != bc::distance_field::unreachable;
    }
    if (field.get(position) == bc::distance_field::unreachable) {
        // A short teleport may bridge an isolated island. Check landing terrain
        // and occupancy, then compare a terrain-only route beyond the barrier.
        auto crystal = me.consumables()[bc::consumable_type::tp_crystal];
        if (crystal.count() && !me.restrictions().contains(bc::buff_restrictions::consumable)) {
            // Crystal range is part of the published kit, not a map coordinate.
            constexpr int crystal_range = 3;
            vec2 destination = position;
            int closest = bc::distance_field::unreachable;
            for (auto p : (around(position, crystal_range) & ~blocked).positions()) {
                int value = field.get(p);
                if (value < closest) {
                    closest = value;
                    destination = p;
                }
            }
            if (destination != position &&
                incoming(destination, engaged_group) * bc::constants::tp_crystal_time < health * 0.5f)
                return crystal.use(destination);
        }
        return false;
    }
    if (breaking_through && !urgent && bc::fuel_remaining() > policy::action_reserve + 5000) {
        int next_group = -1, best_distance = field.get(position);
        for (const auto &c : camps)
            if (distance(position, c.position) <= range) {
                int d = field.get(c.position);
                if (d < best_distance) {
                    best_distance = d;
                    next_group = c.group;
                }
            }
        if (next_group >= 0 && combat(false, next_group))
            return true;
    }
    bool moved = false;
    if (use_dash && bc::fuel_remaining() > policy::planning_reserve)
        moved = cast_mobility(field, urgent, engaged_group);
    if (moved) {
        // A dash can reveal previously hidden camps and champions. Their tiles
        // must be removed before choosing the ordinary step in the same turn.
        blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                  bc::vision::teleport_tiles();
        blocked.set(position, false);
    }
    if (!me.can_move())
        return moved;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (bc::fuel_remaining() < policy::action_reserve + 5000)
            break;
        auto legal = blocked.reachable_tiles(position, stats.base_move_speed);
        vec2 best = position;
        float best_score = std::numeric_limits<float>::infinity();
        const int remaining = field.get(position);
        for (auto p : legal.positions()) {
            if (bc::fuel_remaining() < policy::action_reserve)
                break;
            if (p == position || blocked.is_set(p))
                continue;
            int d = field.get(p);
            if (d == bc::distance_field::unreachable)
                continue;
            if (!urgent && d >= remaining)
                continue;
            float score = float(d) * 10;
            score += incoming(p, engaged_group) * (urgent ? 0.8f : 0.14f);
            // Respect bush cover when taking an aggressive approach.
            if (bc::map::bushes().is_set(p))
                score -= 0.4f;
            if (score < best_score) {
                best_score = score;
                best = p;
            }
        }
        if (best == position)
            break;
        if (move_step(best))
            return true;
        if (bc::fuel_remaining() < policy::planning_reserve)
            break;
        field.compute_to(blocked, goals & ~blocked, around(position, 0), visited);
    }
    return moved;
}

bool Bot::escape() {
    auto &me = bc::self();
    publish(TeamMemory::recall);
    if (bc::map::spawn_region(bc::team::ally).is_set(position)) {
        // Turn-end recovery must occur before moving off the spawn.
        if (health < max_health)
            return true;
        retreating = false;
        return false;
    }
    float immediate = incoming(position);
    if (immediate == 0) {
        bool chased = false;
        for (const auto &e : enemies)
            if (!e.stunned && distance(e.position, position) <= 6)
                chased = true;
        if (!chased) {
            me.recall();
            return true;
        }
    }
    if (home_field.get(position) <= bc::constants::recall_time + 1) {
        move_to(bc::map::spawn_region(bc::team::ally), true, true);
        combat(true, -1, true);
        return true;
    }
    // Reach a tile outside camp attack range, then recall. Taking damage during
    // recall cancels it, so starting a channel in an active camp is never useful.
    constexpr int search_radius = 6;
    auto safe = around(position, search_radius) & ~blocked;
    bc::bitboard threatened;
    for (const auto &c : camps)
        if (c.strength > 0 && distance(position, c.position) <= search_radius + c.range)
            threatened.draw_chebyshev(c.position, c.range);
    for (const auto &e : enemies) {
        const int reach = std::max(e.range + 1, 4);
        if (distance(position, e.position) <= search_radius + reach)
            threatened.draw_chebyshev(e.position, reach);
    }
    // Union the same exclusion regions once. Distant ward sightings cannot
    // intersect this local search, and need no temporary full-map mask.
    safe &= ~threatened;
    if (!safe.is_empty())
        move_to(safe, true, true);
    else
        move_to(bc::map::spawn_region(bc::team::ally), true, true);
    combat(true, -1, true);
    return true;
}
