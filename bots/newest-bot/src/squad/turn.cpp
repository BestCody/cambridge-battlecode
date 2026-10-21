// Squad turn: observe, reconcile the shared job, then execute one phase.
#include "../bot.h"

void Bot::play_squad_turn() {
    auto &me = bc::self();
    if (!me.is_alive()) {
        publish(TeamMemory::unavailable);
        buy_equipment(squad.need_crystal || squad.access_preparation);
        squad.phase = SquadPhase::select;
        squad.camp = target = squad.altar = -1;
        retreating = false;
        squad.cargo_seen[slot] = 0;
        squad.cargo_since[slot] = -1;
        squad.plan = {};
        active_search = {};
        clear_progress = {};
        job_search = {};
        return;
    }
    observe();
    reconcile_squad();
    if ((squad.camp < 0 && altar_walk().get(position) == bc::distance_field::unreachable &&
         (altar_access.size() < altars.size() || (!altar_access.empty() && !altar_access.back().prepared))) ||
        squad.altar >= int(altar_access.size()) ||
        (squad.altar >= 0 && !altar_access[squad.altar].prepared)) {
        // A shared altar ID is valid even when this actor has not built its
        // route yet. Make that dependency useful work, not an idle bank turn.
        prepare_routes();
    }
    buy_equipment(squad.need_crystal || squad.access_preparation);
    if (position != previous)
        stalls = 0;
    else
        ++stalls;
    previous = position;
    if constexpr (policy::diagnostic)
        if (int(round) - last_report >= 50) {
            std::printf("CORE r=%u slot=%u phase=%d camp=%d altar=%d pos=%d,%d hp=%d gems=%u fuel=%llu "
                        "plan=%d search=%d/%u/%u/%u job=%u/%u/%u\n",
                        round, slot, int(squad.phase), squad.camp, squad.altar, position.x, position.y,
                        int(health), gems, (unsigned long long)bc::fuel_remaining(), squad.plan.complete,
                        active_search.camp, active_search.stage, active_search.next_member,
                        active_search.firing.stage, job_search.work.stage, job_search.work.next_member,
                        job_search.work.firing.stage);
            last_report = round;
        }
    if (me.is_teleporting()) {
        squad.bank_stage = BankStage::crossing;
        squad.progress = round;
        publish_squad();
        return;
    }
    if (me.is_recalling()) {
        squad.phase = SquadPhase::recover;
        squad.bank_stage = BankStage::preparing;
        squad.progress = round;
        publish_squad();
        return;
    }
    if (me.is_submitting_gems()) {
        squad.bank_stage = BankStage::channel;
        const int remaining = std::max(1, int(me.remaining_channel_time()));
        if (can_protect_channel(remaining)) {
            squad.progress = round;
            publish_squad();
            return;
        }
        me.cancel_submit_gems();
        squad.bank_stage = BankStage::ready;
    }
    // A member can enter recovery without waiting for a coordinator decision
    // or a bank search. Keep that commitment until spawn healing completes.
    if (bc::map::spawn_region(bc::team::ally).is_set(position) && health >= max_health)
        retreating = false;
    retreating |= health_recovery_needed();
    if (retreating)
        begin_recovery();
    if (squad.phase == SquadPhase::recover) {
        squad_recover(squad.need_crystal);
        publish_squad();
        return;
    }
    interrupt_deposit();
    supervise_progress();
    unsigned known_cargo = gems;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i))
            if (const auto *member = observed_member(i))
                known_cargo += member->gems;
    const bool can_win = known_cargo && known_cargo + bc::global_submitted_gems(bc::team::ally) >=
                                            rules::settings().winning_gems;
    if (can_win && squad.phase != SquadPhase::deposit && squad.altar < 0 && review_banking(true)) {
        return;
    }
    if (squad.phase != SquadPhase::recover && needs_recovery())
        begin_recovery();
    if (squad.phase == SquadPhase::recover) {
        squad_recover(squad.need_crystal);
        publish_squad();
        return;
    }
    if (me.buffs().has_stun()) {
        publish_squad();
        return;
    }
    if constexpr (policy::squad_obstruction) {
        // Obstruction takes precedence over the camp job, which resumes after.
        observe_enemy_tributes();
        update_hunt();
        if (squad.phase != SquadPhase::deposit && squad.phase != SquadPhase::recover && squad_hunt_turn()) {
            publish_squad();
            return;
        }
    }
    if (squad.phase == SquadPhase::deposit) {
        squad_bank();
        publish_squad();
        return;
    }
    const bool completed = squad.camp >= 0 && groups[squad.camp].status == CampGroup::cooling;
    if (completed && squad.coordinator == int(slot) && squad.upgrade_preparation) {
        squad.upgrade_preparation = false;
        begin_recovery();
        squad_recover();
        return;
    }
    if (completed && squad.access_preparation && squad.coordinator == int(slot)) {
        squad.access_preparation = false;
        if (review_banking(true)) {
            return;
        }
    }
    if (squad.coordinator == int(slot)) {
        if (squad.camp < 0 || completed)
            select_squad_job();
    }
    if (review_banking()) {
        // The next turn executes the newly published job with fresh
        // observations and a full budget after comparing complete routes.
        return;
    } else if (squad.camp >= 0) {
        squad_resource_turn();
        // Productive actions get first use of the turn budget. Reassessment
        // continues from its saved cursor with the remaining planning budget.
        const bool secured =
            groups[squad.camp].status == CampGroup::capture && groups[squad.camp].holding_since >= 0;
        if (squad.coordinator == int(slot) && !secured && int(round) >= squad.next_job_review)
            select_squad_job();
    } else if (squad.coordinator >= 0 && squad.coordinator != int(slot))
        if (const auto *leader = observed_member(squad.coordinator))
            squad_move(around(leader->position, range));
    publish_squad();
}
