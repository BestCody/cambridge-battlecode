// Obstruction: interrupt enemy tributes, hunt carriers the squad can kill, and
// bias fallback farming toward the enemy side and the altar they use.
#include "../bot.h"
#include <string_view>
#include <tuple>

void Bot::observe_enemy_tributes() {
    if (altar_tributes.size() != altars.size())
        altar_tributes.assign(altars.size(), 0);
    for (const auto &enemy : enemies)
        if (enemy.depositing)
            for (unsigned a = 0; a < altars.size(); ++a)
                if (distance(enemy.position, altars[a]) <= rules::settings().submit_gems_range)
                    ++altar_tributes[a];
}

int Bot::watched_altar() const {
    // Where enemies have been seen tributing most; before any sighting, the
    // altar closest to their spawn.
    int best = -1;
    for (unsigned a = 0; a < altars.size(); ++a) {
        const int seen = a < altar_tributes.size() ? altar_tributes[a] : 0;
        const int best_seen = best >= 0 && unsigned(best) < altar_tributes.size() ? altar_tributes[best] : 0;
        if (best < 0 || seen > best_seen ||
            (seen == best_seen && altar_enemy_distance[a] < altar_enemy_distance[best]))
            best = a;
    }
    return best;
}

float Bot::obstruction_bonus(int camp) const {
    if (!policy::squad_obstruction || camp < 0)
        return 1;
    const auto &group = groups[camp];
    float bonus = 1;
    if (group.enemy_distance < group.home_distance)
        bonus *= policy::squad_enemy_side_bonus;
    const int altar = watched_altar();
    if (altar >= 0 && distance(group.centre, altars[altar]) <= policy::squad_altar_watch_radius)
        bonus *= policy::squad_altar_bonus;
    return bonus;
}

float Bot::squad_burst(const ObservedUnit &enemy) const {
    // One volley from every member close enough to land it next turn: basic
    // attack plus the best ready spell, after the enemy's damage reduction.
    const ObservedUnit self(*this);
    float total = 0;
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (!squad.contains(i))
            continue;
        const auto *member = i == slot ? &self : observed_member(i);
        if (!member || member->health <= 0 || member->channel)
            continue;
        const int reach = std::max(member->range, member->spell_reach) + member->move_speed;
        if (distance(member->position, enemy.position) <= reach)
            total += member->strength + member->burst;
    }
    return total * enemy.mitigation;
}

const ObservedUnit *Bot::hunt_enemy() const {
    if (squad.hunt_target < 0 || unsigned(squad.hunt_target) >= enemy_roster.size())
        return nullptr;
    for (const auto &enemy : enemies)
        if (std::get<bc::champion>(enemy.ref) == enemy_roster[squad.hunt_target] && enemy.health > 0)
            return &enemy;
    return nullptr;
}

void Bot::end_hunt(const char *reason) {
    if constexpr (policy::diagnostic)
        std::printf("HUNT_END r=%u slot=%u target=%d reason=%s rounds=%d\n", round, slot, squad.hunt_target,
                    reason, int(round) - squad.hunt_since);
    // A chase that failed would fail again at once: rest that target.
    if (std::string_view(reason) == "timeout" || std::string_view(reason) == "outnumbered") {
        squad.hunt_refused = squad.hunt_target;
        squad.hunt_refused_until = round + policy::hunt_max_rounds;
    }
    squad.hunt_target = -1;
    publish_squad(true);
}

void Bot::update_hunt() {
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    if (squad.coordinator != int(slot) || bc::fuel_remaining() < policy::planning_reserve)
        return;
    const bool outnumbered = enemy_power(position) > friendly_power(position) * policy::hunt_abort_margin;
    if (squad.hunt_target >= 0) {
        const auto *enemy = hunt_enemy();
        if (enemy)
            squad.hunt_seen = round;
        if (!enemy && int(round) - squad.hunt_seen > 2)
            end_hunt("lost");
        else if (enemy && enemy_spawn.is_set(enemy->position))
            end_hunt("escaped");
        else if (int(round) - squad.hunt_since > policy::hunt_max_rounds)
            end_hunt("timeout");
        else if (outnumbered)
            end_hunt("outnumbered");
        return;
    }
    // A secured capture or a bank run is worth more than a chase.
    if (squad.phase == SquadPhase::deposit || squad.phase == SquadPhase::recover || outnumbered)
        return;
    if (squad.camp >= 0 && groups[squad.camp].status == CampGroup::capture &&
        groups[squad.camp].holding_since >= 0)
        return;
    int chosen = -1;
    float best = 0;
    const char *why = "";
    for (const auto &enemy : enemies) {
        if (enemy.health <= 0 || enemy_spawn.is_set(enemy.position) ||
            distance(position, enemy.position) > policy::hunt_radius)
            continue;
        if (enemy_power(enemy.position) > friendly_power(enemy.position) * policy::hunt_abort_margin)
            continue;
        int responders = 0;
        for (const auto &other : enemies)
            if (&other != &enemy && other.health > 0 &&
                distance(other.position, enemy.position) <=
                    std::max(other.range, other.spell_reach) + other.move_speed + 1)
                ++responders;
        if (enemy_index(enemy) == squad.hunt_refused && int(round) < squad.hunt_refused_until)
            continue;
        const bool kill = squad_burst(enemy) >= enemy.health + enemy.shield;
        float score = 0;
        const char *reason = "";
        if (enemy.depositing) {
            score = 1000 + enemy.gems; // Every interrupted tribute delays their score.
            reason = "interrupt";
        } else if (kill && responders <= 1) {
            score = 500 + enemy.gems * 5.0f;
            reason = "kill";
        } else if (unsigned(enemy.gems) >= policy::hunt_min_gems && responders == 0) {
            score = enemy.gems * 3.0f;
            reason = "carrier";
        }
        if (score > best) {
            best = score;
            chosen = enemy_index(enemy);
            why = reason;
        }
    }
    if (chosen < 0)
        return;
    squad.hunt_target = chosen;
    squad.hunt_since = squad.hunt_seen = round;
    shared.commit(slot, squad_claim(), unsigned(chosen) + 1);
    if constexpr (policy::diagnostic) {
        const auto *enemy = hunt_enemy();
        std::printf("HUNT_START r=%u slot=%u target=%d type=%d gems=%d hp=%d reason=%s dist=%d\n", round,
                    slot, chosen, enemy ? int(enemy->type) : -1, enemy ? enemy->gems : 0,
                    enemy ? int(enemy->health) : 0, why, enemy ? distance(position, enemy->position) : -1);
    }
}

bool Bot::squad_hunt_turn() {
    if (squad.hunt_target < 0 || bc::fuel_remaining() < policy::planning_reserve)
        return false;
    // Chasing is progress for the underlying job, which resumes afterwards.
    squad.progress = round;
    const auto *enemy = hunt_enemy();
    if (!enemy) {
        // Out of our own sight: close on the coordinator, who can see it.
        if (squad.coordinator >= 0 && squad.coordinator != int(slot))
            if (const auto *leader = observed_member(squad.coordinator))
                squad_move(around(leader->position, 2), true);
        return true;
    }
    const vec2 at = enemy->position;
    squad_combat(); // Focus is the hunt target; stuns land on a channelling target first.
    auto &me = bc::self();
    if (!me.is_alive())
        return true;
    if (me.can_move() && bc::fuel_remaining() > policy::action_reserve) {
        // Kite: stay inside our spell range but outside the target's reach if
        // we can, and prefer tiles other enemies cannot hit.
        int reach = range;
        for (const auto &spell : spells)
            reach = std::max(reach, int(spell.range));
        const int danger = std::max(enemy->range, enemy->spell_reach) + enemy->move_speed;
        auto score = [&](vec2 tile) {
            const int d = distance(tile, at);
            return std::tuple{d > reach, d <= danger, incoming(tile), d};
        };
        vec2 best = position;
        auto best_score = score(position);
        for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
            if (tile == position || blocked.is_set(tile))
                continue;
            const auto value = score(tile);
            if (value < best_score) {
                best_score = value;
                best = tile;
            }
        }
        if (best != position)
            move_step(best);
    }
    ward_ahead(at);
    return true;
}
