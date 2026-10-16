#include "bot.h"

void Bot::play_turn() {
    auto &me = bc::self();
    round = bc::round_number();
    walked = false;
    contact_force = 0;
    contact_group = -1;
    if (!initialised) {
        initialise();
        if (bc::fuel_remaining() < 45000)
            return;
    }
    if (!me.is_alive()) {
        publish(0);
        shop();
        retreating = banking = false;
        target = -1;
        return;
    }
    shop();
    if (me.is_recalling() || me.is_teleporting())
        return;
    prepare_routes();
    observe();
    if (me.is_submitting_gems()) {
        // Protect a valuable bag only when the remaining channel is lethal.
        if (health < incoming(position) * (me.remaining_channel_time() + 1)) {
            me.cancel_submit_gems();
            retreating = true;
            escape();
        }
        return;
    }
    if (!me.can_move() && !me.can_attack() && !me.can_cast())
        return;
    disrupt_tribute();
    const bool at_spawn = bc::map::spawn_region(bc::team::ally).is_set(position);
    if (at_spawn) {
        if (health < max_health)
            return;
        retreating = false;
    }
    if (position == previous)
        ++stalls;
    else
        stalls = 0;
    previous = position;
    report();

    // A guaranteed winning deposit has higher value than any additional kill.
    if (gems && gems + bc::global_submitted_gems(bc::team::ally) >= bc::constants::winning_gems) {
        banking = true;
        tribute();
        return;
    }
    if (retreating) {
        if (escape())
            return;
    }
    // Pressure first: punish exposed enemies and interrupt enemy deposits before
    // considering a routine farming or shopping trip.
    update_formation();
    // Establish the economic objective before the fight check. skirmish()
    // returns true whenever an exchange looks favourable, so on a contested map
    // farm() -- the only place the target is chosen -- can go unreached for the
    // whole match: every champion kept target=-1, idled on its spawn tile
    // between fights and never banked anything (contest: 0 gems in 1848 rounds).
    if (target < 0 && bc::fuel_remaining() > policy::observation_reserve)
        target = choose_group();
    if (skirmish())
        return;
    if (tribute())
        return;
    if (reinforce())
        return;

    bool poor_health = health < max_health * policy::retreat_health;
    bool upgrade =
        me.gold_count() >= bc::item_type::power_shard_of_strength.cost() *
                               (opening.assault ? policy::shopping_shards : policy::spread_shopping_shards) &&
        passive != bc::passive_type::remote_shop;
    bool about_to_capture = target >= 0 && groups[target].status == CampGroup::capture &&
                            distance(position, groups[target].centre) <= 2;
    bool nearly_clear = target >= 0 && groups[target].health < dps(groups[target]) * 3;
    if (!at_spawn && !about_to_capture && !nearly_clear && (poor_health || upgrade)) {
        if (gems && altar_field.get(position) <= 5) {
            banking = true;
            if (tribute())
                return;
        }
        retreating = true;
        if (escape())
            return;
    }
    farm();
}
