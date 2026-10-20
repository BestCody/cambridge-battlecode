// Per-turn dispatch between the squad and worker controllers.
#include "bot.h"

void Bot::play_turn() {
    round = bc::round_number();
    walked = false;
    if (!initialised) {
        initialise();
        if (bc::fuel_remaining() < policy::observation_reserve + policy::planning_reserve) {
            publish(TeamMemory::unavailable);
            return;
        }
    }
    report_death();
    if (squad_member()) {
        play_squad_turn();
        squad_priority();
    } else {
        play_worker_turn();
        worker_priority();
    }
    record_life();
    prepare_routes();
}

void Bot::record_life() {
    if constexpr (!policy::diagnostic)
        return;
    const auto &me = bc::self();
    if (!me.is_alive() || bc::fuel_remaining() < policy::action_reserve)
        return;
    // End-of-turn snapshot: deaths happen during the turns that follow ours,
    // so this is the last state we chose before the fatal damage.
    const int engaged = squad_member() ? squad.camp : target;
    auto &life = last_life;
    life.round = round;
    life.position = position;
    life.health = me.health();
    life.max_health = max_health;
    life.gems = gems;
    life.enemy_threat = incoming(position, engaged, false);
    life.camp_threat = incoming(position, engaged) - life.enemy_threat;
    life.enemies_near = 0;
    life.nearest_enemy = -1;
    for (const auto &enemy : enemies) {
        const int d = distance(position, enemy.position);
        if (d <= std::max(enemy.range, enemy.spell_reach) + enemy.move_speed)
            ++life.enemies_near;
        if (life.nearest_enemy < 0 || d < life.nearest_enemy)
            life.nearest_enemy = d;
    }
    life.phase = squad_member() ? int(squad.phase) : 100 + int(current_activity); // 100+: worker activity
    life.job = engaged;
    life.retreating = retreating;
}

void Bot::report_death() {
    if constexpr (!policy::diagnostic)
        return;
    if (last_life.round < 0 || bc::self().is_alive())
        return;
    const auto &life = last_life;
    const bool twin = type == bc::champion_type::cassi || type == bc::champion_type::issac;
    std::printf(
        "DEATH r=%u last=%d slot=%u type=%d role=%s twin=%d pos=%d,%d hp=%d/%d gems=%u enemy_threat=%.0f "
        "camp_threat=%.0f enemies_near=%d nearest=%d phase=%d job=%d retreating=%d\n",
        round, life.round, slot, int(type), squad_member() ? "squad" : "worker", twin, life.position.x,
        life.position.y, int(life.health), int(life.max_health), life.gems, life.enemy_threat,
        life.camp_threat, life.enemies_near, life.nearest_enemy, life.phase, life.job, life.retreating);
    last_life.round = -1; // Report each death once; the next life starts a new snapshot.
}
