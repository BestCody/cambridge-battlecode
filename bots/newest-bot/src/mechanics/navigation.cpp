// Movement, mobility casts and the shared health-recovery retreat.
#include "../bot.h"
#include <tuple>

bool Bot::move_step(vec2 tile) {
    auto &me = bc::self();
    if (!me.can_move() || tile == me.position())
        return false;
    bool moved = false;
    try {
        moved = me.move_absolute(tile);
    } catch (const bc::error &error) {
        // A tile reserved since the occupancy was read (a teleport landing
        // set during this turn) is refused outright; nothing is spent.
        if (error.code() != bc::error_code::invalid_action_move_bad_destination)
            throw;
    }
    if (moved) {
        walked = true;
        position = me.position();
        refresh_blocked();
        return true;
    }
    // A refusal reveals the blocking occupant without spending the allowance.
    bc::vision::champion_tiles_into(bc::team::ally, occupancy);
    blocked |= occupancy;
    bc::vision::champion_tiles_into(bc::team::enemy, occupancy);
    blocked |= occupancy;
    bc::vision::camp_tiles_into(occupancy);
    blocked |= occupancy;
    bc::vision::teleport_tiles_into(occupancy);
    blocked |= occupancy;
    blocked.set(me.position(), false);
    return false;
}

bool Bot::cast_mobility(const bc::distance_field &field, bool urgent, int engaged_group) {
    auto &me = bc::self();
    const bool can_cast = me.can_cast();
    for (auto a : me.abilities()) {
        if (!a.is_dash() || !ready(a, can_cast))
            continue;
        const auto design = describe(a);
        if (walked && design.exclusive)
            continue;
        occupied_into(dash_blocked, dash_terrain);
        dash_blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(design.range), step_tiles);
        subtract_into(search_goals, step_tiles, blocked);
        // The landings in reading order, and their walks: from one export of
        // the field when there are many (an export costs about one read per
        // 150 tiles), else read one by one.
        const std::size_t tiles = std::size_t(map_width) * map_height;
        // Every landing is within the dash's range on both axes.
        const std::size_t span = 2 * std::size_t(static_cast<std::uint16_t>(design.range)) + 1;
        if (dash_landings.size() < span * span)
            dash_landings.resize(span * span);
        const std::uint32_t count = search_goals.positions_into(dash_landings);
        const bool exported = std::size_t(count) * 150 > tiles;
        if (exported) {
            if (!dash_walks)
                dash_walks.reset(new std::uint16_t[tiles]);
            field.export_linear_buffer(std::span(dash_walks.get(), tiles));
        }
        const auto walk_at = [&](vec2 p) { return int(exported ? dash_walks[tile_index(p)] : field.get(p)); };
        const vec2 *choices = dash_landings.data();
        const int here = walk_at(position);
        const float original = float(here);
        // A landing scores its walk plus the incoming estimate, which only
        // adds: one walking further than the best score so far cannot win, and
        // unless urgent one saving under two steps is never taken (it saves
        // less than the 1.75 asked even before its estimate). The rest, by
        // walk then reading order, as a sorted scan visits them; a tie goes
        // to the landing first in reading order.
        const int limit = urgent ? here : here - 2;
        static std::vector<std::int32_t> landing, walk;
        landing.clear();
        walk.clear();
        for (std::uint32_t k = 0; k < count; ++k)
            if (const int w = walk_at(choices[k]); w <= limit && choices[k] != position) {
                landing.push_back(std::int32_t(k));
                walk.push_back(w);
            }
        for (int attempt = 0; attempt < 3; ++attempt) {
            int best = -1;
            float score = original;
            for (int level = -1;;) {
                int next = std::numeric_limits<int>::max();
                for (const int w : walk)
                    if (w > level && w < next)
                        next = w;
                if (next == std::numeric_limits<int>::max() || float(next) > score)
                    break;
                for (std::size_t j = 0; j < landing.size() && float(next) <= score; ++j)
                    if (walk[j] == next) {
                        const int k = landing[j];
                        const float value =
                            float(next) + incoming(choices[k], engaged_group) * (urgent ? 0.06f : 0.012f);
                        if (value < score || (value == score && best >= 0 && k < best)) {
                            score = value;
                            best = k;
                        }
                    }
                level = next;
            }
            if (best < 0 || (!urgent && original - score < 1.75f))
                return false;
            if (a.use(choices[best])) {
                position = me.position();
                if (design.exclusive)
                    walked = true;
                return true;
            }
            blocked |= bc::vision::champion_tiles();
            blocked.set(choices[best]);
            blocked.set(position, false);
            // A refused landing reveals occupants: drop them from the choices.
            std::size_t kept = 0;
            for (std::size_t j = 0; j < landing.size(); ++j)
                if (!blocked.is_set(choices[landing[j]])) {
                    landing[kept] = landing[j];
                    walk[kept++] = walk[j];
                }
            landing.resize(kept);
            walk.resize(kept);
        }
    }
    return false;
}

bool Bot::move_to(bc::bitboard goals, bool urgent, bool use_dash, int engaged_group, bool allow_teleport) {
    profiling::Scope scope(profiling::Stage::move);
    auto &me = bc::self();
    const auto state = me.state();
    if (state.contains(bc::champion_state::teleporting) || state.contains(bc::champion_state::recalling) ||
        state.contains(bc::champion_state::submitting_gems))
        return false;
    goals &= open_terrain;
    if (goals.is_empty() || goals.is_set(position))
        return false;
    auto &field = move_field;
    auto &visited = search_visited;
    subtract_into(search_goals, goals, blocked);
    field.compute_to(blocked, search_goals, origin_at(position), visited);
    bool breaking_through = false;
    if (field.get(position) == bc::distance_field::unreachable) {
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
                if (incoming(destination, engaged_group) * rules::settings().tp_crystal_time >= health * 0.5f)
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
    if (breaking_through && !urgent) {
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
    if (use_dash)
        moved = cast_mobility(field, urgent, engaged_group);
    if (moved) {
        // A dash can reveal previously hidden camps and champions. Their tiles
        // must be removed before choosing the ordinary step in the same turn.
        refresh_blocked();
    }
    if (!me.can_move())
        return moved;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // Reachable tiles exclude everything in blocked already.
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        vec2 best = position;
        float best_score = std::numeric_limits<float>::infinity();
        const int remaining = field.get(position);
        for (auto p : step_tiles.positions()) {
            if (p == position)
                continue;
            int d = field.get(p);
            if (d == bc::distance_field::unreachable)
                continue;
            if (!urgent && d >= remaining)
                continue;
            const bool bush = bush_tile[tile_index(p)];
            float least = float(d) * 10;
            if (bush)
                least -= 0.4f;
            if (!(least < best_score))
                continue; // The incoming estimate only adds: this tile cannot win.
            float score = float(d) * 10;
            score += incoming(p, engaged_group) * (urgent ? 0.8f : 0.14f);
            // Respect bush cover when taking an aggressive approach.
            if (bush)
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
        subtract_into(search_goals, goals, blocked);
        field.compute_to(blocked, search_goals, origin_at(position), visited);
    }
    return moved;
}

bool Bot::health_recovery_needed() const {
    if (bc::map::spawn_region(bc::team::ally).is_set(position))
        return health < max_health;
    float threshold = policy::retreat_health;
    if (type == bc::champion_type::heart_of_ruin)
        threshold = policy::heart_retreat_health; // Stronger when hurt, and it shields itself.
    // Also home when the health left is twice the damage it is taking (camp
    // damage included).
    return health < max_health * threshold || health <= incoming(position, target) * 2;
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
        if (camp.strength > 0 && distance(position, camp.position) <= stats.vision_radius + camp.range &&
            !camp_dormant(camp))
            threatened.draw_chebyshev(camp.position, camp.range);
    for (const auto &enemy : enemies) {
        const auto design = enemy.champion().base_stats();
        const int reach =
            std::max(enemy.range, enemy.spell_reach) + rules::settings().recall_time * design.base_move_speed;
        if (distance(position, enemy.position) <= stats.vision_radius + reach)
            threatened.draw_chebyshev(enemy.position, reach);
    }
    // A hit on the tile of our last recall cancelled it: whatever struck,
    // seen or not (a camp out of view, an enemy in a bush), would cancel the
    // next one too. Recalling again on the same tile kept champions standing
    // under those hits, often until they died, so walk home instead.
    const bool cancelled = hit && position == recall_tile;
    const bool safe = !threatened.is_set(position) && incoming(position) == 0 && !cancelled;
    const int walk = home_field.get(position);
    if (safe && walk > rules::settings().recall_time * stats.base_move_speed) {
        me.recall();
        recall_tile = position;
        return;
    }
    const auto safe_tiles = around(position, stats.vision_radius) & ~blocked & ~threatened;
    const auto goals = walk <= rules::settings().recall_time * stats.base_move_speed || safe_tiles.is_empty() ||
                               cancelled
                           ? spawn
                           : safe_tiles;
    const bool moved = move_to(goals, true, true);
    if (!moved && safe) {
        // Occupancy can block a short walk home. Do not wait on that route when
        // this tile already permits the recall that fulfils the same recovery.
        me.recall();
        recall_tile = position;
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
            const auto value = score(tile);
            if (value < best_score) {
                best = tile;
                best_score = value;
            }
        }
        if (best != position)
            move_step(best);
    }
    if (!spawn.is_set(position)) {
        observe_fight();
        // Ranged and unmoved, a shot costs no walk: fire back.
        combat(true, -1, !(range > 1 && !walked));
    }
}
