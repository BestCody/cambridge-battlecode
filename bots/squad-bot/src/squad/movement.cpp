// Squad travel: common approach, support-aware steps and yielding.
#include "../bot.h"
#include <tuple>

MoveResult Bot::squad_move(bc::bitboard goals, bool urgent) {
    auto finish = [&](MoveStatus state, MoveReason why = MoveReason::none, int blocker = -1, int until = -1) {
        squad.motion = {state, why, blocker, until};
        return squad.motion;
    };
    auto &me = bc::self();
    if (!me.can_move())
        return finish(MoveStatus::waiting, MoveReason::allowance, -1, round + 1);
    if (bc::fuel_remaining() < policy::planning_reserve)
        return finish(MoveStatus::deferred, MoveReason::fuel);
    const auto obstacles = static_blocked | known_camp_obstacles;
    goals &= ~obstacles;
    if (goals.is_empty())
        return finish(MoveStatus::blocked, MoveReason::terrain);
    const bool resource = squad.phase != SquadPhase::deposit && squad.phase != SquadPhase::recover;
    // All members share the home-facing approach. Once it is reached, fan out
    // into the individually validated firing/capture positions.
    if (resource && squad.plan.staging.x >= 0 && squad.camp >= 0) {
        const auto staging = around(squad.plan.staging, 0);
        if (approach_route.revision != terrain_revision || !(approach_route.goals ^ staging).is_empty() ||
            approach_route.field.get(position) == bc::distance_field::unreachable) {
            if (bc::fuel_remaining() < policy::observation_reserve)
                return finish(MoveStatus::deferred, MoveReason::fuel);
            bc::bitboard visited;
            approach_route.field.compute_to(obstacles, staging, around(position, 0), visited);
            approach_route.goals = staging;
            approach_route.revision = terrain_revision;
        }
        const int approach = approach_route.field.get(position);
        if (approach != bc::distance_field::unreachable && approach > int(stats.vision_radius))
            goals = around(squad.plan.staging, 0);
    }
    if (bc::fuel_remaining() < policy::planning_reserve)
        return finish(MoveStatus::deferred, MoveReason::fuel);
    if (travel_route.revision != terrain_revision || !(travel_route.goals ^ goals).is_empty() ||
        travel_route.field.get(position) == bc::distance_field::unreachable) {
        if (bc::fuel_remaining() < policy::observation_reserve)
            return finish(MoveStatus::deferred, MoveReason::fuel);
        bc::bitboard visited;
        travel_route.field.compute_to(obstacles, goals, around(position, 0), visited);
        travel_route.goals = goals;
        travel_route.revision = terrain_revision;
        // Distances to different goals are not comparable: restart the best.
        squad.route_best = travel_route.field.get(position);
    }
    if (bc::fuel_remaining() < policy::planning_reserve)
        return finish(MoveStatus::deferred, MoveReason::fuel);
    const auto &route = travel_route.field;
    const int original = route.get(position);
    if (original == bc::distance_field::unreachable)
        return finish(MoveStatus::blocked, MoveReason::terrain);
    if (squad.arrival_deadline < 0)
        squad.arrival_deadline = round + original + work_window();
    std::array<const bc::distance_field *, TeamMemory::slots> paths{};
    std::array<std::optional<bc::distance_field>, TeamMemory::slots> local_support;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (i != slot && squad.contains(i) && observed_member(i) && !observed_member(i)->channel &&
            bc::fuel_remaining() > policy::observation_reserve) {
            // Support is a local arrival question. A bounded terrain search
            // gives exact nearby distances without flooding the whole map.
            bc::bitboard visited;
            auto &field = local_support[i].emplace();
            field.compute(obstacles, around(observed_member(i)->position, 0), visited,
                          stats.vision_radius + 2 * stats.base_move_speed);
            paths[i] = &field;
        }
    auto priority = [&](unsigned i) {
        const auto *member = observed_member(i);
        const auto at = i == slot ? position : member ? member->position : vec2{-1, -1};
        const bool holder = squad.camp >= 0 && at.x >= 0 && groups[squad.camp].status == CampGroup::capture &&
                            groups[squad.camp].tiles.is_set(at);
        const unsigned bag = i == slot ? gems : member ? member->gems : 0;
        return std::tuple{holder, squad.phase == SquadPhase::deposit ? bag : 0u, -int(i)};
    };
    int yield_to = -1;
    if (goals.is_set(position)) {
        // A holder/channel is never evicted. A staging member yields a narrow
        // passage to the deterministic higher-priority traveller.
        for (unsigned i = 0; i < roster.size(); ++i)
            if (paths[i] && priority(i) > priority(slot)) {
                const auto *member = observed_member(i);
                if (distance(member->position, position) <= member->move_speed &&
                    route.get(member->position) > original) {
                    yield_to = i;
                    break;
                }
            }
        if (yield_to < 0)
            return finish(MoveStatus::arrived);
    }
    int waiting_for = -1;
    const unsigned committed = squad.participants & living_core();
    const int support_window = std::max(1, std::min(range, int(std::min(spells[0].range, spells[1].range))));
    auto supported = [&](vec2 tile) {
        if (urgent || squad.phase == SquadPhase::recover)
            return true;
        const bool danger = incoming(tile, squad.camp) > 0 ||
                            std::any_of(enemies.begin(), enemies.end(), [&](const auto &enemy) {
                                return distance(tile, enemy.position) <=
                                       std::max(enemy.range, enemy.spell_reach) + enemy.move_speed;
                            });
        for (unsigned i = 0; i < roster.size(); ++i) {
            if (i == slot || !(committed & (1u << i)))
                continue;
            const auto *member = observed_member(i);
            if (!member) {
                if (squad.last_seen[i] >= 0 && int(round) < squad.arrival_deadline &&
                    distance(tile, squad.last_positions[i]) > int(stats.vision_radius) &&
                    distance(tile, squad.last_positions[i]) > distance(position, squad.last_positions[i])) {
                    waiting_for = i;
                    return false;
                }
                continue;
            }
            if (member->channel)
                continue;
            if (!paths[i]) {
                if (danger) {
                    waiting_for = i;
                    return false;
                }
                continue;
            }
            const int d = paths[i]->get(tile), current = paths[i]->get(position);
            if (d == bc::distance_field::unreachable && current == d) {
                if (distance(tile, member->position) >= distance(position, member->position)) {
                    waiting_for = i;
                    return false;
                }
                continue;
            }
            const int support = std::max(1, std::min(member->range, support_window));
            const int delay = d == bc::distance_field::unreachable
                                  ? d
                                  : std::max(0, d - support + member->move_speed - 1) / member->move_speed +
                                        (member->stunned ? member->stun_time + 1 : 0) +
                                        (member->completed ? 1 : 0);
            if (d > current && ((danger && delay > 1) || d > int(stats.vision_radius))) {
                waiting_for = i;
                return false;
            }
        }
        return true;
    };
    vec2 best = position;
    float best_score = std::numeric_limits<float>::infinity();
    for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
        if (bc::fuel_remaining() < policy::action_reserve)
            break;
        if (tile == position || blocked.is_set(tile) || tile == squad.last_step || !supported(tile))
            continue;
        int onward = route.get(tile);
        if (onward == bc::distance_field::unreachable)
            continue;
        if (yield_to >= 0) {
            const auto *member = observed_member(yield_to);
            if (distance(tile, member->position) <= distance(position, member->position) ||
                route.get(tile) < original)
                continue;
        } else if (onward >= original) {
            // Permit a short detour around temporary occupancy. Evaluate the
            // second legal step, rather than oscillating between equal tiles.
            int second = onward;
            for (auto after : blocked.reachable_tiles(tile, stats.base_move_speed).positions()) {
                if (bc::fuel_remaining() < policy::action_reserve)
                    break;
                if (after != position && !blocked.is_set(after))
                    second = std::min(second, int(route.get(after)));
            }
            if (second >= original)
                continue;
        }
        const float score = onward + incoming(tile, squad.camp) / std::max(1.0f, health) * work_window();
        if (score < best_score) {
            best = tile;
            best_score = score;
        }
    }
    if (best != position && bc::fuel_remaining() >= policy::action_reserve) {
        const auto before = position;
        if (move_step(best)) {
            squad.last_step = before;
            // Only a new best distance is progress. Stepping away and back
            // around an obstacle must not keep extending the deadline.
            if (route.get(position) < squad.route_best || yield_to >= 0) {
                squad.route_best = std::min(squad.route_best, int(route.get(position)));
                squad.progress = round;
            }
            return finish(MoveStatus::moved, yield_to >= 0 ? MoveReason::occupancy : MoveReason::none,
                          yield_to);
        }
    }
    squad.last_step = {-1, -1};
    if (waiting_for >= 0)
        return finish(MoveStatus::waiting, MoveReason::support, waiting_for, squad.arrival_deadline);
    return finish(MoveStatus::blocked, MoveReason::occupancy);
}
