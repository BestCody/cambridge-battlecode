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

void Bot::track_ghosts() {
    // An enemy we have seen and cannot see now could be on any tile it can
    // have walked to since, unless that tile is in view: start from its last
    // tile, spread a tile a round, drop the tiles in view and the walls. Only
    // tiles within our sight radius are kept: beyond it the enemy is as
    // unknown as one never seen, and the record goes once none is left.
    // Brought up to date only on turns that use it (enemies in view, or a
    // retreat): rounds skipped are spread at once, by doubling steps.
    if (ghost_update == int(round))
        return;
    ghost_update = int(round);
    const std::size_t n = enemy_roster.size();
    if (ghost_round.size() != n) {
        ghost_round.assign(n, -1);
        ghost_seen.assign(n, std::nullopt);
        ghost_built.assign(n, 0);
        ghost_where.resize(n);
        const std::size_t side = 2 * std::size_t(stats.vision_radius) + 1;
        ghost_tiles.resize(side * side);
    }
    ghosts.clear();
    bool keep_made = false;
    for (std::size_t i = 0; i < n; ++i) {
        const ObservedUnit *seen = nullptr;
        for (const auto &e : enemies)
            if (e.champion() == enemy_roster[i]) {
                seen = &e;
                break;
            }
        if (seen) {
            ghost_seen[i] = *seen;
            ghost_round[i] = int(round);
            ghost_built[i] = 0; // Its tiles are made when it leaves our sight.
            continue;
        }
        if (ghost_round[i] < 0)
            continue;
        const int elapsed = int(round) - ghost_round[i];
        // A dead enemy has a respawn round (track_enemy_respawns()).
        if (elapsed > int(stats.vision_radius) || (i < enemy_respawn.size() && enemy_respawn[i] >= 0)) {
            ghost_round[i] = -1;
            continue;
        }
        if (!keep_made) {
            ghost_keep.clear();
            ghost_keep.draw_chebyshev(position, float(stats.vision_radius));
            ghost_keep &= open_terrain;
            subtract(ghost_keep, visible);
            keep_made = true;
        }
        auto &where = ghost_where[i];
        if (!ghost_built[i]) {
            where.clear();
            where.set(ghost_seen[i]->position);
            ghost_built[i] = 1;
        }
        // Spread by 1, then 2, 4...: steps of s tiles each way add up to every
        // offset until they cover the rounds elapsed (an overshoot only keeps
        // more tiles possible).
        for (int covered = 0, step = 1; covered < elapsed; covered += step, step *= 2) {
            where.shift_right_into(step, ghost_spread);
            where |= ghost_spread;
            where.shift_right_into(-step, ghost_spread);
            where |= ghost_spread;
            where.shift_up_into(step, ghost_spread);
            where |= ghost_spread;
            where.shift_up_into(-step, ghost_spread);
            where |= ghost_spread;
        }
        where &= ghost_keep;
        ghost_round[i] = int(round);
        const std::uint32_t count = where.positions_into(ghost_tiles);
        if (!count) {
            ghost_round[i] = -1;
            continue;
        }
        vec2 nearest = ghost_tiles[0];
        for (std::uint32_t k = 1; k < count; ++k)
            if (distance(position, ghost_tiles[k]) < distance(position, nearest))
                nearest = ghost_tiles[k];
        auto &ghost = ghosts.emplace_back(*ghost_seen[i]);
        ghost.position = nearest;
        ghost.stunned = ghost.channel = false;
        ghost.stun_time = -1;
    }
}

// Danger at a tile: how many enemies could hit it, and by how many tiles the
// closest of them is still short (negative once inside its reach). Enemies
// out of sight count from their possible tile nearest us.
std::pair<int, int> Bot::worker_danger(vec2 tile) const {
    int threats = 0, clearance = std::numeric_limits<int>::max();
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &enemy : *list)
            if (enemy_threatens(enemy, tile)) {
                ++threats;
                const int reach = std::max(enemy.range, enemy.spell_reach) + enemy.move_speed;
                clearance = std::min(clearance, distance(tile, enemy.position) - reach);
            }
    return {threats, clearance};
}

bool Bot::stepping_pays(vec2 step) {
    // Walking away outruns nothing that keeps pace, so it is worth only what
    // it saves: home when we would reach it alive, else a step that saves
    // more damage next round than the attack it costs (a ranged champion
    // cannot attack after walking; a melee one only what is still in reach).
    const float taking = incoming(position);
    if (taking <= 0)
        return true;
    const int walk = home_field.get(position);
    if (walk != bc::distance_field::unreachable &&
        health > taking * float(walk) / float(std::max(1, int(stats.base_move_speed))))
        return true;
    auto &me = bc::self();
    const auto abilities = me.abilities();
    const bool can_cast = me.can_cast(), can_attack = me.can_attack();
    const auto strike = [&](vec2 from, bool after_walk) {
        float best = 0;
        for (const auto &e : enemies) {
            const int d = distance(from, e.position);
            const bool hits = can_attack && d <= range && (range <= 1 || !after_walk);
            float spell = 0;
            for (unsigned i = 0; i < spells.size(); ++i) {
                const auto &s = spells[i];
                if (!ready(abilities[i], can_cast) || (after_walk && s.exclusive))
                    continue;
                if (s.kind == bc::ability_kind::attack_reset)
                    spell = std::max(spell, hits ? basic_damage() : 0.0f);
                else if (d > int(s.range + s.radius))
                    continue;
                else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                         s.kind == bc::ability_kind::aoe_damage)
                    spell = std::max(spell, focus * s.modifier);
                else if (s.kind == bc::ability_kind::dash && s.modifier > 0)
                    spell = std::max(spell, abilities[i].damage_amount());
            }
            best = std::max(best, ((hits ? basic_damage() : 0.0f) + spell) * e.mitigation);
        }
        return best;
    };
    const float staying = strike(position, false);
    if (staying <= 0)
        return true;
    if (step == position)
        return false;
    return taking - incoming(step) >= staying - strike(step, true);
}

void Bot::avoid_reach() {
    // Moves for the rest of this turn keep out of the reach of the stronger
    // group in view: walking back in after stepping away kept a champion
    // hovering at its edge until it was caught.
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    avoid_zone.clear();
    for (const auto &e : enemies) {
        if (e.health <= 0 || e.channel || enemy_spawn.is_set(e.position))
            continue;
        const int move = e.stunned && e.stun_time > 0 ? 0 : e.move_speed;
        avoid_zone.draw_chebyshev(
            e.position, float(std::max(e.range, e.spell_reach) + move + policy::worker_danger_margin));
    }
    avoid_zone.set(position, false);
    avoid_round = int(round);
    blocked |= avoid_zone;
}

vec2 Bot::step_away_tile() {
    // The reachable tile with the fewest threats, then the most room, then
    // the shortest way to a teammate in view that is in this fight or coming
    // to it (help that answered), else home (or to an altar when carrying a
    // bag).
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
    if (bc::self().can_move()) {
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        for (auto tile : step_tiles.positions()) {
            if (tile == position)
                continue;
            const auto [count, room] = worker_danger(tile);
            const auto score = std::tuple{count, -room, way(tile)};
            if (score < best_score) {
                best_score = score;
                best = tile;
            }
        }
    }
    return best;
}
