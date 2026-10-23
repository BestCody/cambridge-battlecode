// Worker turn: the independent policy every champion plays.
#include "../bot.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <tuple>

bool Bot::worker_escape() {
    profiling::Scope scope(profiling::Stage::retreat);
    // Flee even when an enemy can reach us next turn: fleeing loses some free
    // hits but survives far more often. Standing traded down to death in two
    // thirds of the fights we were losing, where the top team breaks contact.
    publish(TeamMemory::recall);
    if (bc::map::spawn_region(bc::team::ally).is_set(position) && health >= max_health) {
        retreating = false;
        return false;
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
    note_activity(Activity::idle); // Overwritten by whatever this turn ends up doing.
    track_enemy_respawns(); // Needs no vision: also while dead, recalling or banking.
    if (!me.is_alive()) {
        note_activity(Activity::dead);
        publish(TeamMemory::unavailable);
        buy_equipment();
        retreating = banking = false;
        target = -1;
        crystal_plan.takeoff = {-1, -1};
        return;
    }
    buy_equipment();
    if (me.is_recalling() || me.is_teleporting()) {
        note_activity(Activity::recall);
        publish(TeamMemory::recall);
        return;
    }
    observe();
    if (me.is_submitting_gems()) {
        note_activity(Activity::bank);
        publish(TeamMemory::bank);
        // Protect a valuable bag only when the remaining channel is lethal; a
        // winning deposit stands unless we die before our next turn.
        const bool wins = gems + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems;
        if (health < incoming(position) * (wins ? 1 : me.remaining_channel_time() + 1)) {
            me.cancel_submit_gems();
            retreating = true;
            worker_escape();
        }
        return;
    }
    const bool at_spawn = bc::map::spawn_region(bc::team::ally).is_set(position);
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
    // Health recovery precedes fighting, capture, bank travel and shopping.
    // Empty bags and a nearly cleared camp never waive this obligation.
    retreating |= health_recovery_needed();
    if (retreating && worker_escape()) {
        note_activity(at_spawn ? Activity::heal : Activity::retreat);
        return;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) {
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
    previous = position;
    worker_report();

    // A guaranteed winning deposit has higher value than any additional kill.
    if (gems && gems + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems) {
        banking = true;
        worker_tribute();
        return;
    }
    worker_update_formation();
    if (worker_skirmish()) {
        note_activity(Activity::fight);
        return;
    }
    if (worker_tribute()) {
        note_activity(Activity::bank);
        return;
    }
    if (worker_reinforce()) {
        note_activity(Activity::reinforce);
        return;
    }

    bool upgrade =
        me.gold_count() >= bc::item_type::power_shard_of_strength.cost() *
                               (is_carry(type) ? policy::carry_shopping_shards : policy::shopping_shards);
    bool about_to_capture = target >= 0 && groups[target].status == CampGroup::capture &&
                            distance(position, groups[target].centre) <= 2;
    // A camp waiting to respawn or being captured reads 0 health: only one
    // being cleared is nearly clear.
    bool nearly_clear = target >= 0 && groups[target].status == CampGroup::alive &&
                        groups[target].health < dps(groups[target]) * 3;
    if (!at_spawn && !about_to_capture && !nearly_clear && upgrade) {
        if (gems && altar_walk().get(position) <= 5) {
            banking = true;
            if (worker_tribute())
                return;
        }
        retreating = true;
        if (worker_escape()) {
            note_activity(Activity::shop);
            return;
        }
    }
    worker_farm();
    if (target >= 0)
        note_activity(distance(position, groups[target].centre) <= range + 3 ? Activity::farm
                                                                             : Activity::travel);
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
        static constexpr const char *names[] = {"idle",  "dead",      "recall", "heal", "retreat", "bank",
                                                "fight", "reinforce", "shop",   "farm", "travel"};
        std::printf("ACTIVITY r=%u type=%d switches=%u crystals=%u/%u", round, int(type), target_switches,
                    crystals_bought, crystals_used);
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
    if (me.is_alive() && !me.is_recalling() && !me.is_submitting_gems() && !me.is_teleporting() &&
        !enemies.empty()) {
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
            if (hp <= 0)
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
            const int tier = lethal ? priority_tier::finish : priority_tier::damage;
            const float value = lethal ? burst + 200 + e.gems * 7 : burst;
            best = std::max(best, tier + std::min(int(value), priority_tier::span));
        }
        if (best > 0)
            priority = best;
    }
    if (me.priority() != priority)
        me.set_priority(priority);
}
