// Pure-resource worker safety: detect danger, remember it, and step away.
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

int Bot::enemy_index(const ObservedUnit &enemy) const {
    for (unsigned i = 0; i < enemy_roster.size(); ++i)
        if (std::get<bc::champion>(enemy.ref) == enemy_roster[i])
            return int(i);
    return -1;
}

bool Bot::enemy_closing(const ObservedUnit &enemy) const {
    const int i = enemy_index(enemy);
    // Compare against where it stood on the previous observation, measured
    // to where we stand now: only its own movement counts as approach.
    return i >= 0 && enemy_track[i].previous_round >= int(round) - 2 &&
           distance(enemy.position, position) < distance(enemy_track[i].previous, position);
}

void Bot::worker_track_sightings() {
    if (enemy_seen_near.size() != groups.size())
        enemy_seen_near.assign(groups.size(), -policy::worker_sighting_memory - 1);
    if (enemy_track.size() != enemy_roster.size())
        enemy_track.assign(enemy_roster.size(), EnemyTrack{});
    for (const auto &enemy : enemies) {
        if (enemy.health <= 0)
            continue;
        if (const int i = enemy_index(enemy); i >= 0 && enemy_track[i].last_round != int(round)) {
            auto &track = enemy_track[i];
            track.previous = track.last;
            track.previous_round = track.last_round;
            track.last = enemy.position;
            track.last_round = round;
        }
        for (std::size_t i = 0; i < groups.size(); ++i)
            for (const auto &camp : groups[i].camps)
                if (enemy_threatens(enemy, camp.position)) {
                    enemy_seen_near[i] = round;
                    break;
                }
    }
}

bool Bot::worker_disengage() {
    if (enemies.empty())
        return false;
    // Danger at a tile: how many enemies could hit it, and by how many tiles
    // the closest of them is still short (negative once inside its reach).
    auto danger = [&](vec2 tile) {
        int threats = 0, clearance = std::numeric_limits<int>::max();
        for (const auto &enemy : enemies)
            if (enemy_threatens(enemy, tile)) {
                ++threats;
                const int reach = std::max(enemy.range, enemy.spell_reach) + enemy.move_speed;
                clearance = std::min(clearance, distance(tile, enemy.position) - reach);
            }
        return std::pair{threats, clearance};
    };
    const auto [threats, clearance] = danger(position);
    if (!threats)
        return false;
    // A nearby enemy alone is not a reason to stop farming. Leave when one
    // can already hit us, is closing in, or the enemy side is locally stronger.
    bool leave = enemy_power(position) > friendly_power(position) * policy::worker_strength_margin;
    for (const auto &enemy : enemies)
        if (!leave && enemy_threatens(enemy, position))
            leave = distance(position, enemy.position) <= std::max(enemy.range, enemy.spell_reach) ||
                    enemy_closing(enemy);
    if (!leave)
        return false;
    publish(TeamMemory::under_threat);
    target = -1; // Choose the next camp afresh, with this sighting remembered.
    auto &me = bc::self();

    // Free kill: our burst finishes an enemy this turn and no other enemy
    // could reach us afterwards. Anything less is a fight we did not choose.
    const auto abilities = me.abilities();
    for (const auto &enemy : enemies) {
        if (!enemy_threatens(enemy, position))
            continue;
        const int d = distance(position, enemy.position);
        float burst = me.can_attack() && d <= range ? strength : 0;
        for (unsigned i = 0; i < spells.size(); ++i) {
            const auto &spell = spells[i];
            if (!abilities[i].is_ready() || !me.can_cast())
                continue;
            if (spell.kind == bc::ability_kind::attack_reset && d <= range)
                burst += strength;
            else if ((spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc ||
                      spell.kind == bc::ability_kind::aoe_damage) &&
                     d <= spell.range + spell.radius)
                burst += focus * spell.modifier;
        }
        const bool lethal = burst * enemy.mitigation >= enemy.health + enemy.shield;
        const bool alone = std::none_of(enemies.begin(), enemies.end(), [&](const auto &other) {
            return &other != &enemy && enemy_threatens(other, position);
        });
        if (lethal && alone && combat(true))
            return true;
    }

    // Step to the reachable tile with the fewest threats, then the most room,
    // then the shortest way home (or to an altar when carrying a bag).
    const auto &guide =
        gems && altar_field.get(position) < home_field.get(position) ? altar_field : home_field;
    vec2 best = position;
    auto best_score = std::tuple{threats, -clearance, int(guide.get(position))};
    if (me.can_move())
        for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
            if (bc::fuel_remaining() < policy::action_reserve)
                break;
            if (tile == position || blocked.is_set(tile))
                continue;
            const auto [count, room] = danger(tile);
            const auto score = std::tuple{count, -room, int(guide.get(tile))};
            if (score < best_score) {
                best_score = score;
                best = tile;
            }
        }
    if (best != position && move_step(best))
        return true;
    // Cornered: no step improves our position, so fight back.
    combat(true);
    return true;
}
