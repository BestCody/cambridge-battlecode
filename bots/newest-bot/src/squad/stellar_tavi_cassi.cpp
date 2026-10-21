// Stellar/Tavi/Cassi composition: membership, clearing and recovery.
#include "stellar_tavi_cassi.h"
#include "../bot.h"

namespace squads {
unsigned StellarTaviCassi::membership(std::span<const bc::champion_type> roster) {
    constexpr std::array composition{bc::champion_type::stellar, bc::champion_type::tavi,
                                     bc::champion_type::cassi};
    unsigned members = 0;
    for (auto type : composition) {
        const auto found = std::find(roster.begin(), roster.end(), type);
        if (found == roster.end())
            return 0;
        members |= 1u << (found - roster.begin());
    }
    return members;
}
} // namespace squads

void Bot::squad_combat() {
    if (std::none_of(enemies.begin(), enemies.end(),
                     [&](const auto &enemy) { return relevant_enemy(enemy); }))
        return;
    squad_focus();
    // The bounded search chooses direct damage, area damage, a distinct pure
    // control target, and legal basic/reset orders. It cannot create a new job.
    combat(true);
}

void Bot::squad_resource_turn() {
    if (squad.camp < 0)
        return;
    auto &group = groups[squad.camp];
    if (group.status == CampGroup::cooling) {
        if (group.tiles().is_set(position))
            squad_move(around(group.centre, range) & ~group.tiles());
        return;
    }
    squad_combat();
    if (!bc::self().is_alive())
        return;
    if (group.status == CampGroup::capture) {
        squad.phase = SquadPhase::capture;
        if (!group.tiles().is_set(position)) {
            auto goals = group.tiles() & ~blocked;
            const auto &assigned = squad.plan.firing[slot];
            if (assigned.capture.x >= 0 && goals.is_set(assigned.capture))
                goals = around(assigned.capture, 0);
            if (goals.is_empty())
                goals = around(group.centre, range) & ~group.tiles() & ~blocked;
            squad_move(goals);
        }
        ward_ahead(group.centre);
        return;
    }
    // Keep executing while a replacement estimate is being resumed. A fuel
    // deferral must not discard the current assignment or a legal camp shot.
    // A move deferred last turn runs before any new estimate this turn; the
    // refresh at the end of the turn still gets the surplus.
    const bool resume_motion = squad.motion.status == MoveStatus::deferred;
    if (!resume_motion &&
        (squad.plan.camp != squad.camp || !squad.plan.complete || !(squad.plan.participants & (1u << slot))))
        prepare_objective();
    if (squad.plan.camp != squad.camp || !squad.plan.complete) {
        const auto staging = objective_staging(squad.camp);
        if (staging.x >= 0)
            squad_move(around(staging, 0));
        return;
    }
    const auto &place = squad.plan.firing[slot];
    squad.siege = squad.plan.siege;
    bc::bitboard fire;
    if (squad.siege)
        for (const auto &camp : group.camps)
            if (camp.health > 0)
                fire |= around(camp.position, camp.stats.base_attack_range);
    const bool threatened = std::any_of(enemies.begin(), enemies.end(), [&](const auto &enemy) {
        return relevant_enemy(enemy) && !enemy.channel && !enemy.stunned;
    });
    bool ready = true;
    for (unsigned i = 0; threatened && i < roster.size(); ++i) {
        if (!(squad.plan.participants & (1u << i)) || i == slot)
            continue;
        const auto *member = observed_member(i);
        if (!member || member->channel || member->stunned) {
            ready = false;
            continue;
        }
        const auto &at = squad.plan.firing[i];
        ready &= member_route(i).get(at.tile) <= member->move_speed;
    }
    // Clear at once when the reward lands before anyone can interfere.
    // Otherwise hold cooldowns for denial until the squad is in position.
    // Damage already on the camp is sunk cost and is not a reason to continue.
    const bool uncontested =
        squad.plan.interference > squad.plan.reward_time && relevant_threats() <= squad.plan.threats;
    const bool clear = (!threatened || ready || uncontested) && (!squad.siege || !fire.is_set(position));
    if (clear) {
        const auto &me = bc::self();
        int ready_reach = me.can_attack() ? range : 0;
        if (me.can_cast()) {
            const auto abilities = me.abilities();
            for (unsigned i = 0; i < spells.size(); ++i) {
                const auto &spell = spells[i];
                if (!abilities[i].is_ready() || spell.modifier <= 0 ||
                    (spell.kind != bc::ability_kind::damage && spell.kind != bc::ability_kind::cc &&
                     spell.kind != bc::ability_kind::aoe_damage))
                    continue;
                const int step = me.can_move() && !spell.exclusive ? int(stats.base_move_speed) : 0;
                ready_reach = std::max(ready_reach, int(spell.range + spell.radius) + step);
            }
        }
        const bool reachable = std::any_of(camps.begin(), camps.end(), [&](const auto &camp) {
            return camp.group == squad.camp && distance(position, camp.position) <= ready_reach;
        });
        // Seeing a distant monster does not create an attack opportunity.
        // Preserve the movement budget until a legal damaging sequence is in reach.
        if (reachable && combat(false, squad.camp))
            squad.progress = round;
        // Measure damage only from successful own actions, including zero-output
        // clear turns. Enemy damage never improves our projected clear speed.
        if (place.tile.x >= 0 && distance(position, place.tile) <= stats.base_move_speed)
            finish_clear_turn(squad.camp, place.output);
    }
    if (bc::self().can_move() && place.tile.x >= 0 && position != place.tile) {
        auto goals = around(place.tile, 0);
        if (blocked.is_set(place.tile)) {
            squad.plan.assessed = -1;
            goals = around(place.tile, stats.base_move_speed) & ~blocked & ~fire;
        }
        squad_move(goals);
    }
    // Refresh only after useful actions. Re-estimation must not consume every
    // travel/clear turn merely because the previous estimate has aged.
    prepare_objective();
}

bool Bot::needs_recovery() const {
    if (health_recovery_needed())
        return true;
    if (bc::map::spawn_region(bc::team::ally).is_set(position))
        return false;
    if (squad.altar >= 0 || passive == bc::passive_type::remote_shop || squad.camp < 0 ||
        groups[squad.camp].status == CampGroup::capture)
        return false;
    const auto &group = groups[squad.camp];
    const float gain = upgrade_gain(type, max_health, strength, focus, bc::self().gold_count());
    const int home = home_field.get(position);
    const float trip = std::min(home, rules::settings().recall_time) + group.home_distance + 1;
    return gain > 0 && group.health / dps(group) * gain / (1 + gain) > trip;
}

void Bot::squad_recover(bool supply) {
    auto &me = bc::self();
    const auto spawn = bc::map::spawn_region(bc::team::ally);
    if (spawn.is_set(position)) {
        buy_equipment(supply);
        if (health < max_health) {
            squad.bank_stage = BankStage::preparing;
            publish_squad();
            return; // Healing occurs at this turn's end, after arrival/purchases.
        }
        if (supply && !me.consumables()[bc::consumable_type::tp_crystal].count()) {
            // A private wallet cannot fund a promised crossing. Abort the
            // supply plan; the next common job must earn the missing gold.
            adopt_job(TeamMemory::supplies_needed);
            publish_squad(true);
            return;
        }
        squad.bank_stage = BankStage::prepared;
        squad.progress = round;
        publish_squad();
        bool ready = true;
        const auto participants = squad.participants & living_core();
        for (unsigned i = 0; i < roster.size(); ++i) {
            if (!(participants & (1u << i)) || i == slot)
                continue;
            const auto code = shared.get(i);
            ready &=
                squad.altar >= 0
                    ? TeamMemory::deposit_altar(code, groups.size(), altars.size()) == squad.altar &&
                          BankStage(TeamMemory::deposit_stage(code, groups.size())) == BankStage::prepared
                    : code == TeamMemory::recovered;
        }
        if (ready && squad.coordinator == int(slot)) {
            if (squad.altar >= 0) {
                squad.crossed = 0;
                squad.crossing_deadline = squad.arrival_deadline = -1;
                squad.phase = SquadPhase::deposit;
                squad.need_crystal = false;
                squad.bank_stage = BankStage::travel;
                squad.changed = squad.progress = round;
                squad.route_best = bc::distance_field::unreachable;
                publish_squad(true);
            } else {
                // Exit recovery independently of an expensive selection. The
                // next turn gets a full budget and cannot remain trapped here.
                enter_selection();
                squad.next_job_review = round;
            }
        }
        return;
    }
    squad.bank_stage = BankStage::preparing;
    publish_squad();
    retreat_to_spawn();
}
