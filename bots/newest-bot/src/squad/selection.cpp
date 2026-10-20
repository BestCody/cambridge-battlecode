// Coordinator job selection: rank camps cheaply, then refine finalists.
#include "../bot.h"

Selection Bot::select_squad_job() {
    if (squad.coordinator != int(slot) || bc::fuel_remaining() < policy::planning_reserve)
        return Selection::deferred;
    if (job_search.started < 0)
        job_search.started = round;
    if (job_search.order.empty()) {
        auto &ranked = job_search.ranked;
        while (job_search.rank_cursor < groups.size()) {
            if (bc::fuel_remaining() < policy::planning_reserve)
                return Selection::deferred;
            const unsigned i = job_search.rank_cursor++;
            const auto &g = groups[i];
            if (int(i) == squad.failed_camp && int(round) < squad.retry_after)
                continue;
            const float reward = g.unit.gem_value() ? g.unit.gem_value() : g.unit.gold_value();
            const float cost = distance(position, g.centre) + g.health / std::max(1.0f, dps(g)) +
                               bc::constants::capture_time + 1;
            const bool pressure = g.unit.gem_value() && g.enemy_distance != bc::distance_field::unreachable &&
                                  g.enemy_distance <= g.home_distance + 2 * opening.spacing;
            ranked.push_back({{pressure, reward / cost}, i});
        }
        if (bc::fuel_remaining() < policy::planning_reserve)
            return Selection::deferred;
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        for (const auto &[priority, i] : ranked) {
            if (job_search.order.size() < policy::objective_finalists || int(i) == squad.camp ||
                (!priority.first &&
                 std::none_of(job_search.order.begin(), job_search.order.end(),
                              [&](unsigned other) { return !groups[other].unit.gem_value(); })))
                job_search.order.push_back(i);
        }
    }
    while (job_search.cursor < job_search.order.size() && bc::fuel_remaining() > policy::planning_reserve &&
           (squad.camp >= 0 || job_search.assessed.empty())) {
        const auto estimate = estimate_job(job_search.order[job_search.cursor], job_search.work);
        if (!estimate.complete) {
            if (bc::fuel_remaining() < policy::observation_reserve)
                break;
            continue;
        }
        ++job_search.cursor;
        if (estimate.camp >= 0)
            job_search.assessed.push_back(estimate);
    }
    if (bc::fuel_remaining() < policy::planning_reserve)
        return Selection::deferred;
    // Resume the remaining candidates next turn. Retain both the current job
    // and the best completed estimates; fuel exhaustion is not infeasibility.
    const bool pending = job_search.cursor < job_search.order.size();
    if (pending && (squad.camp >= 0 && groups[squad.camp].status != CampGroup::cooling))
        return Selection::deferred;
    if (pending && job_search.assessed.empty())
        return Selection::deferred;
    JobEstimate pressure, gather, current;
    for (const auto &estimate : job_search.assessed) {
        if (groups[estimate.camp].status == CampGroup::cooling &&
            groups[estimate.camp].respawn > int(round) + estimate.travel + bc::constants::capture_time)
            continue;
        auto &best = estimate.pressure ? pressure : gather;
        if (estimate.rate > best.rate)
            best = estimate;
        if (estimate.camp == squad.camp)
            current = estimate;
    }
    JobEstimate choice = pressure.camp >= 0 ? pressure : gather;
    if (current.camp >= 0 && current.pressure == choice.pressure &&
        groups[current.camp].status != CampGroup::cooling &&
        current.rate * policy::commit_bonus >= choice.rate)
        choice = current;
    bool preparation = false;
    if (choice.camp >= 0 && choice.pressure && choice.siege && !squad.access_preparation) {
        const auto objective = choice;
        float best_saved = 0;
        for (const auto &candidate : job_search.assessed) {
            if (bc::fuel_remaining() < policy::observation_reserve)
                break;
            const auto &camp = groups[candidate.camp];
            if (candidate.camp == objective.camp || candidate.siege || !camp.unit.gold_value())
                continue;
            unsigned participants = 1;
            for (unsigned i = 0; i < roster.size(); ++i)
                if (squad.contains(i) && observed_member(i))
                    ++participants;
            const unsigned occupants = std::min(participants, camp.tiles.count());
            const unsigned share = camp.unit.gold_value() / std::max(1u, occupants);
            float improvement = 0;
            for (unsigned i = 0; i < roster.size(); ++i)
                if (squad.contains(i) && (i == slot || observed_member(i))) {
                    const float gain = upgrade_output(i, share, true, groups[objective.camp].splash);
                    if (occupants == participants)
                        improvement += gain;
                    else
                        improvement = std::max(improvement, gain);
                }
            const float output = groups[objective.camp].health / std::max(1.0f, objective.clear);
            const float saved = objective.clear * improvement / std::max(1.0f, output + improvement);
            const float detour = candidate.reward_time + bc::constants::recall_time + 1 +
                                 groups[objective.camp].home_distance - objective.travel;
            if (saved > detour * policy::commit_bonus && saved - detour > best_saved) {
                best_saved = saved - detour;
                choice = candidate;
                choice.pressure = false;
                preparation = true;
                squad.resume_camp = objective.camp;
            }
        }
    }
    if (!pending)
        job_search = {};
    squad.next_job_review = pending ? int(round) + 1 : int(round) + work_window();
    if (choice.camp < 0) {
        if (pending)
            return Selection::deferred;
        enter_selection();
        return Selection::unavailable;
    }
    const bool changed =
        choice.camp != squad.camp || squad.phase == SquadPhase::recover || squad.phase == SquadPhase::deposit;
    squad.camp = target = choice.camp;
    squad.altar = -1;
    squad.need_crystal = false;
    squad.upgrade_preparation =
        preparation || (!groups[target].unit.gem_value() && !squad.access_preparation);
    squad.role = choice.pressure ? SquadRole::attack : SquadRole::gather;
    squad.siege = choice.siege;
    squad.pressure_rate = choice.rate;
    squad.plan = choice;
    squad.phase = groups[target].status == CampGroup::capture ? SquadPhase::capture : SquadPhase::travel;
    if (changed) {
        active_search = {};
        squad.arrival_deadline = round + choice.travel + work_window();
        travel_route.revision = ~0u;
        squad.changed = squad.progress = round;
        squad.route_best = bc::distance_field::unreachable;
        squad.participants = living_core();
        if constexpr (policy::diagnostic)
            std::printf("SQUAD_JOB r=%u owner=%u camp=%d role=%s rate=%.2f siege=%d\n", round, slot, target,
                        choice.pressure ? "attack" : "gather", choice.rate, choice.siege);
    }
    publish_squad(changed);
    return Selection::selected;
}
