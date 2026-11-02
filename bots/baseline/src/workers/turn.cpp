// Worker turn: the independent policy every champion plays.
#include "../bot.h"
#include <cstdio>

bool Bot::worker_escape() {
    profiling::Scope scope(profiling::Stage::retreat);
    cluster_run.clear(); // A retreat ends a cluster run.
    // Flee even when an enemy can reach us next turn: fleeing loses some free
    // hits but survives far more often. Standing traded down to death in two
    // thirds of the fights we were losing, where the top team breaks contact.
    // A champion caught while it goes keeps reporting the fight, so help can
    // still come.
    if (!worker_update_formation())
        publish(TeamMemory::homeward);
    if (own_spawn_tile[tile_index(position)]) {
        if (health >= max_health) {
            retreating = false;
            return false;
        }
        // The spawn heals us at the turn's end: strike what reaches us
        // from here rather than stand idle under fire (a melee champion
        // without its dash, which would leave the spawn).
        combat(true, -1, range <= 1);
        return true;
    }
    retreat_to_spawn();
    return true;
}

void Bot::worker_report() {
    if constexpr (policy::diagnostic) {
        if (int(round) - last_report >= 50 || stalls == 8) {
            std::printf("STATE r=%u id=%u type=%d pos=%d,%d hp=%d gems=%u gold=%u target=%d retreat=%d "
                        "bank=%d fuel=%llu\n",
                        round, id(bc::self()), int(type), position.x, position.y, int(health), gems,
                        bc::self().gold_count(), target, retreating, banking,
                        (unsigned long long)bc::fuel_remaining());
            last_report = round;
        }
    }
}

// Every champion earns gems, fights the skirmishes it can win and steps away
// from stronger groups.
void Bot::play_worker_turn() {
    auto &me = bc::self();
    const float before = health; // As our last turn left it.
    note_activity(Activity::idle); // Overwritten by whatever this turn ends up doing.
    track_enemy_respawns(); // Needs no vision: also while dead, recalling or banking.
    // A capture's recall that ended (home, hit, stunned or dead) is over.
    if (capture_recall && !me.is_recalling())
        capture_recall = false;
    if (!me.is_alive()) {
        note_activity(Activity::dead);
        recall_tile = {-1, -1};
        publish(TeamMemory::unavailable);
        shared.set_attacked(slot, false);
        chase.mode = Chase::none;
        hidden.reset();
        buy_equipment();
        retreating = banking = false;
        target = -1;
        cluster_run.clear();
        return;
    }
    buy_equipment();
    // Read once: nothing below acts before its last use (a turn that acts
    // returns), and only actions change it.
    const auto state = me.state();
    if (state.contains(bc::champion_state::recalling) || state.contains(bc::champion_state::teleporting)) {
        note_activity(Activity::recall);
        // A capture's holder riding its recall home keeps its claim, so
        // teammates leave the capture to it; a hop keeps its claim too.
        if (!capture_recall && !state.contains(bc::champion_state::teleporting))
            publish(TeamMemory::homeward);
        shared.set_attacked(slot, false);
        chase.mode = Chase::none;
        return;
    }
    // What camps in reach as our last turn left the records could have
    // struck (note_hidden_hit()): looked up before observe() rewrites them,
    // only when struck.
    camp_bound = me.health() < before ? camp_reach_damage() : 0.0f;
    observe();
    hit = health < before;
    taken_last = std::max(0.0f, before - health);
    // A chase ends once no enemy can reach us, and a stun means deciding it
    // again; a turn spent on anything else keeps it.
    if (chase.mode != Chase::none && (!worker_danger(position).first || me.buffs().has_stun()))
        chase.mode = Chase::none;
    // Teammates help only champions an enemy in view can hit next round.
    bool attacked = false;
    for (const auto &e : enemies)
        attacked = attacked || enemy_threatens(e, position);
    shared.set_attacked(slot, attacked);
    // Struck for more than everything we know of could: an enemy out of
    // sight did.
    if (hit)
        note_hidden_hit(before - health);
    if (state.contains(bc::champion_state::submitting_gems)) {
        note_activity(Activity::bank);
        publish(TeamMemory::bank);
        // Protect a valuable bag only when the remaining channel is lethal; a
        // winning deposit stands unless we die before our next turn.
        const bool wins = gems + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems;
        // The damage we have taken a turn since the tribute began, for the
        // turns left: what reaches us, not every enemy in reach each round
        // (that read 2.2 times what came, while the threat lasted the whole
        // channel rather than until teammates beat it).
        tribute_taken += taken_last;
        ++tribute_turns;
        const float rate = tribute_taken / float(tribute_turns);
        if (health < rate * (wins ? 1.0f : float(me.remaining_channel_time() + 1))) {
            me.cancel_submit_gems();
            retreating = true;
            worker_escape();
        }
        return;
    }
    tribute_taken = 0;
    tribute_turns = 0;
    const bool at_spawn = own_spawn_tile[tile_index(position)];
    // Home (or dead): the last recall's tile no longer marks one a hit cancelled.
    if (at_spawn)
        recall_tile = {-1, -1};
    if (at_spawn)
        left_spawn = int(round);
    if (at_spawn && health >= max_health)
        retreating = false;
    // A winning bag within three steps of an altar is banked even when hurt:
    // the deposit ends the game, and damage does not interrupt it.
    if (gems && gems + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems &&
        altar_walk().get(position) <= 3) {
        banking = true;
        worker_tribute();
        return;
    }
    if (!state.intersects(bc::champion_state::can_move | bc::champion_state::can_attack |
                          bc::champion_state::can_cast)) {
        // A stunned champion still reports the fight it is caught in: help
        // matters most while it cannot act.
        if (!worker_update_formation())
            publish(TeamMemory::unavailable);
        return;
    }
    // A champion without a camp chooses one before anything else this turn.
    if (target < 0)
        target = worker_choose_group();
    if (position == previous)
        ++stalls;
    else
        stalls = 0;
    before_previous = previous;
    previous = position;
    worker_report();

    // A guaranteed winning deposit has higher value than any additional kill.
    if (gems && gems + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems) {
        banking = true;
        worker_tribute();
        return;
    }
    // XSquare's order: the fight first, a hurt champion playing it in hurt
    // mode (no strength of its own on our side, the safest tile, a fight only
    // when an enemy can hit it or it kills), then the walk home. Our health
    // retreat ran first: it walked away from enemies we were finishing (a
    // stunned Heart of Ruin at 10 hp, ours at 473 of 1000, 00galaxy). Moved
    // without the hurt mode, our deaths doubled.
    if (harvest_map && !harvest_ready) {
        harvest_ready = int(round) >= policy::harvest_until || finished_emblems() >= policy::harvest_emblems;
        if constexpr (policy::diagnostic)
            if (harvest_ready)
                std::printf("HARVEST r=%u id=%u ready emblems=%d\n", round, id(bc::self()), finished_emblems());
    }
    hurt_turn = retreating || health_recovery_needed();
    // Hurt, the champion is retreating from now on, whoever plays the turn:
    // when the fight took it, the retreat was never latched and the next turn
    // played as if healthy. Hurt, no chase.
    retreating = retreating || hurt_turn;
    worker_update_formation();
    if (worker_skirmish() || (!hurt_turn && !harvesting() && worker_pursue())) {
        note_activity(Activity::fight);
        return;
    }
    // Health recovery precedes capture, bank travel and shopping. Empty bags
    // and a nearly cleared camp never waive this obligation.
    retreating |= hurt_turn;
    if (retreating && worker_escape()) {
        note_activity(at_spawn ? Activity::heal : Activity::retreat);
        return;
    }
    skirmish_declined = int(round); // A pursuit declined acts on nothing.
    if (worker_tribute()) {
        note_activity(Activity::bank);
        return;
    }
    place_lock();
    if (worker_reinforce()) {
        note_activity(Activity::reinforce);
        return;
    }
    // Shopping needs no trip of its own: every trip home (a capture's recall,
    // or health) buys all the gold affords (buy_equipment()).
    worker_farm();
    if (target >= 0)
        note_activity(distance(position, groups[target].centre) <= range + 3 ? Activity::farm
                                                                             : Activity::travel);
}

void Bot::capture_ride() {
    // A capture's holder goes home on a recall (home_after_capture()). A
    // channel blocks every later action, so it is the turn's last; one
    // already going needs none, and a stun refuses it.
    if (!homeward)
        return;
    homeward = false;
    const auto state = bc::self().state();
    if (!state.contains(bc::champion_state::alive) || state.contains(bc::champion_state::recalling) ||
        state.contains(bc::champion_state::submitting_gems) || state.contains(bc::champion_state::teleporting) ||
        own_spawn.is_set(position) || bc::self().buffs().has_stun())
        return;
    capture_recall = bc::detail::sdk_recall() == 0;
}

void Bot::note_activity(Activity activity) {
    if constexpr (!policy::diagnostic)
        return;
    // The last note of a turn wins: replace an earlier note from this round.
    const bool repeat = last_activity_round == int(round);
    if (repeat)
        --activity_turns[static_cast<unsigned>(current_activity)];
    ++activity_turns[static_cast<unsigned>(activity)];
    current_activity = activity;
    last_activity_round = round;
    if (!repeat && int(round) % 250 == 0 && round > 0) {
        static constexpr const char *names[] = {"idle",  "dead",      "recall", "heal",
                                                "retreat", "bank", "fight", "reinforce", "farm", "travel"};
        std::printf("ACTIVITY r=%u type=%d switches=%u", round, int(type), target_switches);
        for (unsigned i = 0; i < activity_turns.size(); ++i)
            std::printf(" %s=%u", names[i], activity_turns[i]);
        std::printf("\n");
    }
}

void Bot::worker_priority() {
    profiling::Scope scope(profiling::Stage::priority);
    // Priority applies next round. Put an available burst or finishing blow
    // early in the allied order instead of using a permanent champion order.
    if (!initialised)
        return;
    auto &me = bc::self();
    int priority = priority_tier::idle - static_cast<int>(slot);
    // Reads only, so their order changes nothing: no enemy in view (most
    // turns) needs none of the engine's.
    const auto busy = bc::champion_state::recalling | bc::champion_state::submitting_gems |
                      bc::champion_state::teleporting;
    if (!enemies.empty() && me.is_alive() && !me.state().intersects(busy)) {
        const auto abilities = me.abilities();
        std::array<bool, 2> ready{};
        for (unsigned i = 0; i < ready.size(); ++i)
            ready[i] = abilities[i].cooldown() <= 1;
        // A ready damaging dash (Heart of Ruin) counts as a damage spell, at
        // the engine's damage with the passive.
        std::array<float, 2> dash_damage{};
        for (unsigned i = 0; i < spells.size(); ++i)
            if (ready[i] && spells[i].kind == bc::ability_kind::dash && spells[i].modifier > 0)
                dash_damage[i] = abilities[i].damage_amount();
        int best = 0;
        for (const auto &e : enemies) {
            const float hp = live_health(e);
            if (hp <= 0 || enemy_spawn_tile[tile_index(e.position)])
                continue;
            const int d = distance(position, e.position);
            const float basic = d <= range ? basic_damage() : 0;
            float spell = 0;
            for (unsigned i = 0; i < spells.size(); ++i) {
                if (!ready[i])
                    continue;
                const auto &s = spells[i];
                if (s.kind == bc::ability_kind::attack_reset && d <= range)
                    spell = std::max(spell, basic_damage());
                if ((s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                     s.kind == bc::ability_kind::aoe_damage) &&
                    d <= s.range + s.radius)
                    spell = std::max(spell, focus * s.modifier);
                if (dash_damage[i] > 0 && d <= s.range + s.radius)
                    spell = std::max(spell, dash_damage[i]);
            }
            const float burst = (basic + spell) * e.mitigation;
            if (burst <= 0)
                continue;
            const bool lethal = burst >= hp + e.shield;
            // The tier says a kill; within it the bigger burst goes first (a
            // kill's value on top had every finisher at the cap).
            const int tier = lethal ? priority_tier::finish : priority_tier::damage;
            best = std::max(best, tier + std::min(int(burst), priority_tier::span));
        }
        if (best > 0)
            priority = best;
    }
    if (me.priority() != priority)
        me.set_priority(priority);
}
