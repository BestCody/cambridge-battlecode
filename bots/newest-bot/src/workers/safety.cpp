// Worker safety: detect danger, remember it, and step away.
#include "../bot.h"
#include <tuple>

bool Bot::enemy_threatens(const ObservedUnit &enemy, vec2 tile) const {
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    // Channelling enemies cannot hit us this turn, and enemies on their spawn
    // are shopping or healing, not hunting.
    if (enemy.health <= 0 || enemy.channel || enemy_spawn.is_set(enemy.position))
        return false;
    // A stunned enemy loses its next move, so only its standing reach counts.
    const int move = enemy.stunned && enemy.stun_time > 0 ? 0 : enemy.move_speed;
    const int reach = std::max(enemy.range, enemy.spell_reach) + move + policy::worker_danger_margin;
    return distance(tile, enemy.position) <= reach;
}

void Bot::worker_track_sightings() {
    if (enemy_seen_near.size() != groups.size())
        enemy_seen_near.assign(groups.size(), -policy::worker_sighting_memory - 1);
    for (const auto &enemy : enemies) {
        if (enemy.health <= 0)
            continue;
        for (std::size_t i = 0; i < groups.size(); ++i)
            for (const auto &camp : groups[i].camps)
                if (enemy_threatens(enemy, camp.position)) {
                    enemy_seen_near[i] = round;
                    break;
                }
    }
}

// Danger at a tile: how many enemies could hit it, and by how many tiles the
// closest of them is still short (negative once inside its reach).
std::pair<int, int> Bot::worker_danger(vec2 tile) const {
    int threats = 0, clearance = std::numeric_limits<int>::max();
    for (const auto &enemy : enemies)
        if (enemy_threatens(enemy, tile)) {
            ++threats;
            const int reach = std::max(enemy.range, enemy.spell_reach) + enemy.move_speed;
            clearance = std::min(clearance, distance(tile, enemy.position) - reach);
        }
    return {threats, clearance};
}

bool Bot::worker_step_away() {
    // Step to the reachable tile with the fewest threats, then the most room,
    // then the shortest way to a teammate in view that is in this fight or
    // coming to it (help that answered), else home (or to an altar when
    // carrying a bag).
    const auto [threats, clearance] = worker_danger(position);
    const auto &guide =
        gems && altar_walk().get(position) < home_field.get(position) ? altar_walk() : home_field;
    vec2 helper{-1, -1};
    int nearest = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < allies.size(); ++i) {
        const int fight = TeamMemory::fight_group(shared.get(ally_slot[i]), groups.size());
        if (allies[i].channel || !same_fight(fight, position))
            continue;
        if (const int d = distance(position, allies[i].position); d < nearest) {
            nearest = d;
            helper = allies[i].position;
        }
    }
    const auto way = [&](vec2 tile) { return helper.x >= 0 ? distance(tile, helper) : int(guide.get(tile)); };
    vec2 best = position;
    auto best_score = std::tuple{threats, -clearance, way(position)};
    if (bc::self().can_move())
        for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
            if (tile == position)
                continue;
            const auto [count, room] = worker_danger(tile);
            const auto score = std::tuple{count, -room, way(tile)};
            if (score < best_score) {
                best_score = score;
                best = tile;
            }
        }
    return best != position && move_step(best);
}
