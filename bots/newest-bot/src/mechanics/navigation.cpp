// Movement, mobility casts and the shared health-recovery retreat.
#include "../bot.h"
#include <tuple>

bool Bot::move_step(vec2 tile) {
    auto &me = bc::self();
    if (!me.can_move() || tile == me.position())
        return false;
    if (me.move_absolute(tile)) {
        walked = true;
        position = me.position();
        blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                  bc::vision::teleport_tiles();
        blocked.set(position, false);
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

bool Bot::move_to(bc::bitboard goals, bool urgent, bool use_dash, int engaged_group, bool allow_teleport) {
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
        if (allow_teleport && crystal.count() &&
            !me.restrictions().contains(bc::buff_restrictions::consumable)) {
            // Crystal range is part of the published kit, not a map coordinate.
            // Try reachable landings best-first; a hidden occupant blocks a
            // landing without consuming the crystal, so retry the next
            // candidate instead of abandoning the crossing.
            std::vector<std::pair<int, vec2>> landings;
            landings.reserve(24);
            for (auto p : (around(position, rules::crystal_range) & ~blocked).positions()) {
                int value = field.get(p);
                if (value != bc::distance_field::unreachable)
                    landings.emplace_back(value, p);
            }
            std::sort(landings.begin(), landings.end(),
                      [](const auto &l, const auto &r) { return l.first < r.first; });
            for (int attempt = 0; attempt < 3 && attempt < int(landings.size()); ++attempt) {
                const vec2 destination = landings[attempt].second;
                if (incoming(destination, engaged_group) * bc::constants::tp_crystal_time >= health * 0.5f)
                    continue;
                if (crystal.use(destination))
                    return true;
                blocked |= bc::vision::champion_tiles();
                blocked.set(destination);
                blocked.set(position, false);
            }
            return false;
        }
        return false;
    }
    if (breaking_through && !squad_member() && !urgent &&
        bc::fuel_remaining() > policy::action_reserve + 5000) {
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

bool Bot::health_recovery_needed() const {
    if (bc::map::spawn_region(bc::team::ally).is_set(position))
        return health < max_health;
    return health < max_health * policy::retreat_health ||
           health <= incoming(position, squad_member() ? squad.camp : target) * 2;
}

void Bot::retreat_to_spawn() {
    auto &me = bc::self();
    const auto spawn = bc::map::spawn_region(bc::team::ally);
    if (spawn.is_set(position) || me.is_recalling() || me.is_teleporting() || me.is_submitting_gems() ||
        me.buffs().has_stun())
        return;
    // One hit cancels recall. Include reach over the channel, including enemies
    // whose current stun can expire, rather than just their current attack tile.
    bc::bitboard threatened;
    for (const auto &camp : camps)
        if (camp.strength > 0 && distance(position, camp.position) <= stats.vision_radius + camp.range)
            threatened.draw_chebyshev(camp.position, camp.range);
    for (const auto &enemy : enemies) {
        const auto design = std::get<bc::champion>(enemy.ref).base_stats();
        const int reach =
            std::max(enemy.range, enemy.spell_reach) + bc::constants::recall_time * design.base_move_speed;
        if (distance(position, enemy.position) <= stats.vision_radius + reach)
            threatened.draw_chebyshev(enemy.position, reach);
    }
    const bool safe = !threatened.is_set(position) && incoming(position) == 0;
    const int walk = home_field.get(position);
    if (safe && walk > bc::constants::recall_time * stats.base_move_speed) {
        me.recall();
        return;
    }
    const auto safe_tiles = around(position, stats.vision_radius) & ~blocked & ~threatened;
    const auto goals = walk <= bc::constants::recall_time * stats.base_move_speed || safe_tiles.is_empty()
                           ? spawn
                           : safe_tiles;
    const bool moved = move_to(goals, true, true);
    if (!moved && safe) {
        // Occupancy can block a short walk home. Do not wait on that route when
        // this tile already permits the recall that fulfils the same recovery.
        me.recall();
        return;
    }
    if (!moved && me.can_move()) {
        // A disconnected safe region or a blocked corridor must not turn
        // recovery into idling. Minimize immediate damage, then increase the
        // distance from attack/spell reach before preferring the way home.
        auto clearance = [&](vec2 tile) {
            int value = std::numeric_limits<int>::max();
            for (const auto &enemy : enemies)
                value = std::min(value,
                                 distance(tile, enemy.position) - std::max(enemy.range, enemy.spell_reach));
            for (const auto &camp : camps)
                if (camp.strength > 0)
                    value = std::min(value, distance(tile, camp.position) - camp.range);
            return value;
        };
        auto score = [&](vec2 tile) {
            return std::tuple{incoming(tile), -clearance(tile), home_field.get(tile)};
        };
        vec2 best = position;
        auto best_score = score(position);
        for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
            if (bc::fuel_remaining() < policy::action_reserve)
                break;
            if (blocked.is_set(tile))
                continue;
            const auto value = score(tile);
            if (value < best_score) {
                best = tile;
                best_score = value;
            }
        }
        if (best != position && bc::fuel_remaining() >= policy::action_reserve)
            move_step(best);
    }
    if (!spawn.is_set(position) && bc::fuel_remaining() > policy::observation_reserve) {
        observe_fight();
        combat(true, -1, true);
    }
}
