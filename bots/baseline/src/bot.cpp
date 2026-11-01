// Per-turn dispatch: the worker controller, then background precomputation.
#include "bot.h"

void Bot::play_turn() {
    {
        profiling::Scope turn_scope(profiling::Stage::turn);
        take_turn();
    }
    // Outside the turn's measurement: printing costs as much as a turn.
    if (policy::profile && round % 250 == 0)
        profiling::report(round, int(type));
}

void Bot::take_turn() {
    turn_start_fuel = bc::fuel_remaining();
    round = bc::round_number();
    // Per-turn flags: a turn cut short (ChampionGone) leaves them set.
    walked = placed = false;
    if (!initialised) {
        // The first turn only sets up: on a large map initialisation takes
        // most of even this turn's doubled fuel. Champions shop and measure
        // the walk to the nearby camps for their first choice, and move from
        // the next turn.
        initialise();
        buy_equipment();
        worker_priority();
        // The search the first camp choice starts from, as far as the second
        // bound: the doubled fuel of this turn takes the walks a choice would
        // otherwise search for on the next, and the largest map still fits.
        // The choice searches further itself until no camp beyond could change
        // it, so it is the same whatever is measured here.
        if (bc::self().is_alive())
            measure_paths(2);
        spare_region_steps(); // Last: the turn's required work is done.
        return;
    }
    {
        profiling::Scope scope(profiling::Stage::diagnostics);
        report_death();
    }
    try {
        play_worker_turn();
        worker_priority();
        fake_recall();
    } catch (const ChampionGone &) {
        // Killed by one of its own actions: the background work still runs.
    }
    {
        profiling::Scope scope(profiling::Stage::diagnostics);
        record_life();
    }
    {
        // Precomputation from terrain in turns with work to spare: map-wide
        // searches while they fit, then a step of the smaller jobs.
        profiling::Scope scope(profiling::Stage::background);
        // As many as fit: on a small map a search costs a few thousand.
        while (turn_work() + search_job_cost < policy::background_search_work) {
            profiling::Scope job(profiling::Stage::routes);
            const auto start = turn_work();
            if (!background_search())
                break;
            // Searches differ (an altar's covers less), so keep the dearest. A
            // fuel overrun that suspended the search restarted turn_work().
            const auto now = turn_work();
            search_job_cost = std::max(search_job_cost, now > start ? now - start : 0);
        }
        // The crystal hops from spawn once, kept out of search_job_cost: a
        // pass over every tile in our code, far dearer than an engine search,
        // it raised the cost so no search fitted again and the groups' own
        // walk fields were never built.
        if (turn_work() < policy::background_step_work) {
            profiling::Scope job(profiling::Stage::sieges);
            if (!hops_ready)
                prepare_hops();
            else
                prepare_sieges();
        }
        // After every other job, so theirs are as before: the walking-region
        // rows still to label, in the fuel left.
        spare_region_steps();
    }
}

void Bot::record_life() {
    if constexpr (!policy::diagnostic)
        return;
    const auto &me = bc::self();
    if (!me.is_alive())
        return;
    // End-of-turn snapshot: deaths happen during the turns that follow ours,
    // so this is the last state we chose before the fatal damage.
    const int engaged = target;
    auto &life = last_life;
    life.round = round;
    life.position = position;
    life.health = me.health();
    life.max_health = max_health;
    life.gems = gems;
    life.enemy_threat = incoming(position, false);
    life.camp_threat = incoming(position) - life.enemy_threat;
    life.enemies_near = 0;
    life.nearest_enemy = -1;
    for (const auto &enemy : enemies) {
        const int d = distance(position, enemy.position);
        if (d <= damage::act_reach(enemy, false))
            ++life.enemies_near;
        if (life.nearest_enemy < 0 || d < life.nearest_enemy)
            life.nearest_enemy = d;
    }
    life.phase = 100 + int(current_activity); // 100+: worker activity
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
        "DEATH r=%u last=%d slot=%u type=%d twin=%d pos=%d,%d hp=%d/%d gems=%u enemy_threat=%.0f "
        "camp_threat=%.0f enemies_near=%d nearest=%d phase=%d job=%d retreating=%d\n",
        round, life.round, slot, int(type), twin, life.position.x, life.position.y, int(life.health),
        int(life.max_health), life.gems, life.enemy_threat, life.camp_threat, life.enemies_near,
        life.nearest_enemy, life.phase, life.job, life.retreating);
    last_life.round = -1; // Report each death once; the next life starts a new snapshot.
}
