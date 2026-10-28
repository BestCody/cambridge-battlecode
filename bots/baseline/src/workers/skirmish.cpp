// Worker skirmish decisions against enemy champions.
#include "../bot.h"

bool Bot::worker_skirmish() {
    profiling::Scope scope(profiling::Stage::skirmish);
    if (enemies.empty())
        return hidden && confront_hidden();
    const auto abilities = bc::self().abilities();
    // Nothing acts before the second use below, so readiness is read once.
    std::array<bool, 2> is_ready{};
    {
        const bool can_cast = bc::self().can_cast();
        for (unsigned i = 0; i < is_ready.size(); ++i)
            is_ready[i] = ready(abilities[i], can_cast);
    }
    // A damaging dash (Heart of Ruin) counts as a damage spell.
    int ready_reach = range;
    for (unsigned i = 0; i < spells.size(); ++i)
        if (is_ready[i] && spells[i].modifier > 0 &&
            (spells[i].kind == bc::ability_kind::damage || spells[i].kind == bc::ability_kind::cc ||
             spells[i].kind == bc::ability_kind::aoe_damage ||
             spells[i].kind == bc::ability_kind::dash))
            ready_reach = std::max(ready_reach, int(spells[i].range + spells[i].radius));
    int selected = -1;
    float best = -1;
    bool selected_contact = false;
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto &e = enemies[i];
        if (enemy_spawn_tile[tile_index(e.position)])
            continue;
        int d = distance(position, e.position);
        // Choose the current engagement among enemies we can actually damage.
        // A wounded runner outside our reach must not redirect the whole fight
        // while other enemies are already inside our firing line.
        const bool contact = d <= ready_reach || (range <= 1 && d <= range + int(stats.base_move_speed));
        if (selected_contact && !contact)
            continue;
        float value = (150 + e.gems * 5 + e.strength * 2) / (std::max(100.0f, e.health + e.shield) * (d + 2));
        if (e.channel)
            value *= 2;
        value *= peel_factor(e);
        if (value > best || (contact && !selected_contact)) {
            best = value;
            selected = i;
            selected_contact = contact;
        }
    }
    if (selected < 0)
        return false;
    const auto victim = enemies[selected];
    // Each champion sees only its part of a fight: the enemy is as large as
    // the teammate who sees the most of it reports.
    float ours = friendly_power(victim.position),
          theirs = std::max(enemy_power(victim.position), reported_power(victim.position));
    // Price in imminent enemy respawns: a fight beside their base can be joined
    // by a full-health body, so the effective enemy power must reflect it.
    theirs += respawn_threat(position);
    float spell_burst = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (is_ready[i]) {
            if (spells[i].kind == bc::ability_kind::attack_reset &&
                distance(position, victim.position) <= range)
                spell_burst = std::max(spell_burst, basic_damage());
            else if ((spells[i].kind == bc::ability_kind::damage || spells[i].kind == bc::ability_kind::cc ||
                      spells[i].kind == bc::ability_kind::aoe_damage) &&
                     distance(position, victim.position) <= spells[i].range + spells[i].radius)
                spell_burst = std::max(spell_burst, focus * spells[i].modifier);
            else if (spells[i].kind == bc::ability_kind::dash &&
                     spells[i].modifier > 0 &&
                     distance(position, victim.position) <= spells[i].range + spells[i].radius)
                // The engine's damage for it, with Heart of Ruin's passive.
                spell_burst = std::max(spell_burst, abilities[i].damage_amount());
        }
    float burst = spell_burst + (distance(position, victim.position) <= range ? basic_damage() : 0);
    bool kill = victim.health + victim.shield <= burst * victim.mitigation;
    // A sliver of enemy health is a kill we can secure next turn. Finishing
    // removes their damage and any carried gems, so do not recall out of a
    // winning duel because the one-turn health math looks unfavourable. Only
    // chase a *channeling* sliver (recalling/depositing): interrupting it is
    // high-value and it is not fighting back, whereas a sliver still trading
    // has teammates behind it that punish an over-extend.
    bool nearly_dead = victim.health + victim.shield <= victim.max_health * 0.2f;
    bool finish = nearly_dead && victim.channel;
    // A champion with no teammate within 6 fights the opening exchange of a
    // contact at aggression and stays only while as strong as the enemy, one
    // with a single teammate there at pair_aggression: willow best girl's
    // lone champions hit in 34-78% of contacts and leave in 29-61% whatever
    // the odds, dying in 0-6%; ours, staying at aggression up to 3 times
    // their health against them, died in 10-32%.
    if (int(round) - contact_last > policy::contact_gap)
        contact_start = int(round);
    contact_last = int(round);
    const bool opening = int(round) - contact_start < policy::lone_opening;
    const bool dive = peel_factor(victim) > 1;
    bool favourable = fight_favourable(ours, theirs, opening, dive);
    // Too dangerous to stay, decided every round: our health and shields
    // reach no further than the damage the enemies deal us before we are
    // out of their reach from here (exit_damage(), their ready bursts at
    // full and their cooldowns read) plus a margin for our bag (a round of
    // their hits per prism_bag gems). The power test counts burst at a fifth
    // and the fixed retreat at 35% health came after it landed: one enemy
    // Marcella's spell took ours from 1226 to 307 in a round, 22 of our 72
    // deaths against one ladder team went from near-full health to dead in
    // 1-3 rounds, and 16 of 19 against another had a Marcella spell or a
    // Heart of Ruin dash in them. A kill this turn, or a channelling sliver
    // to finish, still holds us when we would survive the damage without
    // the victim's part.
    const bool immune = passive == bc::passive_type::stun_immunity;
    const auto risk = exit_risk(position, health, mitigation, camp_mitigation, gems, immune);
    float pool = health;
    bool too_dangerous = false, overridden = false;
    if (risk.damage > 0) {
        pool = exit_pool(self_shield(), is_ready);
        too_dangerous = pool <= risk.damage + risk.margin;
        overridden = too_dangerous && (kill || finish) &&
                     pool > exit_damage(position, immune, victim.position).damage * mitigation;
    }
    debug_block({
        std::printf("RISK r=%u id=%u type=%d hp=%.0f pool=%.0f exit=%.0f margin=%.0f gems=%u fav=%d kill=%d "
                    "finish=%d td=%d ovr=%d\n",
                    round, id(bc::self()), int(type), health, pool, risk.damage, risk.margin, gems, int(favourable),
                    int(kill), int(finish), int(too_dangerous), int(overridden));
    });
    if (too_dangerous && !overridden)
        return leave_fight(victim.position);
    if (!favourable && !kill && !finish) {
        // Break contact with a stronger group that can soon reach us rather
        // than walk into it alone: half our ladder fights started outnumbered.
        if (worker_danger(position).first) {
            worker_track_sightings(); // The camps by these enemies wait out the sighting.
            // Fight from where we stand: no dash into the group, and a ranged
            // champion shoots rather than walks.
            if (chase_mode(false) == Chase::fight && combat(true, -1, range <= 1))
                return true;
            if (range <= 1) {
                // A melee champion walks and strikes in one turn: strike, then
                // step. A failed step keeps the camp, so the turn is not spent
                // idle. Spells that exclude walking wait for the step.
                const bool stepped = walked;
                walked = true;
                const bool struck = combat(true, -1, true);
                walked = stepped;
                const vec2 step = step_away_tile();
                if (step != position && move_step(step)) {
                    target = -1;
                    return true;
                }
                if (struck)
                    return true;
            } else if (run_to_safety()) {
                target = -1;
                return true;
            }
        }
        if (distance(position, victim.position) <= std::max(victim.range + 1, 4)) {
            // Use a free ranged stun/poke on the way to the economic objective.
            // Ranged and unmoved, a shot costs no walk: fire back. Kept from
            // shooting there, our Prism stood beside a Vendetta for four
            // rounds, its spells cooling, and died without striking.
            // A melee champion's poke leaves spells that exclude walking: the
            // farming walk that follows still goes.
            const bool stepped = walked;
            walked = walked || range <= 1;
            combat(true, -1, !(range > 1 && !stepped));
            walked = stepped;
        }
        // The farming and travel that follow keep out of the stronger group's
        // reach.
        avoid_reach();
        return false;
    }
    int d = distance(position, victim.position);
    // A dive on a teammate is ours to answer while the power at the diver's
    // tile (the dived teammate there) is with us.
    const bool peel = peel_factor(victim) > 1;
    if (!favourable && d > range && !kill && !peel)
        return false;
    // A champion farming (it struck its camp in the last two rounds) farms
    // on while the victim is beyond its reach: willow best girl's farmers,
    // the nearest enemy 4-6 tiles away, keep hitting their camp in 49% of
    // rounds and step toward it in 18-21% (12-15% at 7-8 tiles, walking
    // between camps); ours kept farming in 36% and stepped closer in 28%,
    // and our Willow, holding 4 tiles off with nothing to cast, stood idle
    // in 27% of those rounds (willow's 4-11%). Farming's combat still heals
    // and shoots what is in range.
    // A capture's holder fights from its tiles: leaving them resets it.
    const bool on_capture = target >= 0 && groups[target].status == CampGroup::capture &&
                            groups[target].tiles().is_set(position);
    if (d > ready_reach && !kill && !peel && target >= 0 &&
        (groups[target].attacked >= int(round) - 2 || on_capture))
        return false;
    auto &me = bc::self();
    bool moved = false;
    // Prism and Willow fight from their spells' reach (Prism's stun and area
    // hit, Willow's heal), outside the basic attacks' reach of 3: their basic
    // attacks are weak and their spells are the damage. willow best girl's
    // stand 4-6 tiles from the nearest enemy in their teammates' fights
    // (within 2 in 5-8% of those rounds; ours, walking in to shoot, 18-19%).
    // An enemy within 2 makes them step back unless a Prism spell is ready to
    // cast (Willow heals as it walks); at 3 they shoot, as willow's do in a
    // quarter to a third of their rounds in contact. willow's Prism, above 35%
    // health within 2, shoots in 44-75% of rounds; ours standing there too
    // (stepping back only below 35% health) died more and Willow
    // healed less: ours reaches 2 tiles 18 rounds a game to willow's 7-13.
    const bool backline = type == bc::champion_type::prism || type == bc::champion_type::willow;
    // A teammate within 3 covers us: alone, the back line keeps a melee
    // enemy from walking in and striking (below).
    bool covered = false;
    for (const auto &a : allies)
        covered = covered || (a.health > 0 && distance(position, a.position) <= 3);
    if (backline && d < range && me.can_move() && !kill) {
        bool spell_ready = false;
        for (unsigned i = 0; i < spells.size(); ++i)
            spell_ready = spell_ready || (is_ready[i] && spells[i].modifier > 0 &&
                                          (spells[i].kind == bc::ability_kind::damage ||
                                           spells[i].kind == bc::ability_kind::cc ||
                                           spells[i].kind == bc::ability_kind::aoe_damage));
        int support = 0;
        for (const auto &a : allies)
            support += a.health > 0 && distance(position, a.position) <= policy::peel_support;
        const bool shoot_back =
            type == bc::champion_type::prism && support > 0 && health >= max_health * 0.5f;
        // Alone, the Prism steps out of reach the turn after its stun:
        // willow best girl's does in 67% of those rounds (20-26% with a
        // teammate within 3, where it shoots in 60-63%); ours shot back in
        // 73-83%, alone or not.
        bool stunned_last = false;
        for (unsigned i = 0; i < spells.size(); ++i)
            stunned_last = stunned_last || (spells[i].kind == bc::ability_kind::cc &&
                                            abilities[i].cooldown() >= spells[i].cooldown - 1);
        const bool slip = type == bc::champion_type::prism && stunned_last && !covered;
        if (((!spell_ready || type == bc::champion_type::willow) && !shoot_back) || slip) {
            const vec2 step = step_away_tile();
            if (step != position && move_step(step)) {
                observe_fight();
                combat(true, -1, true);
                return true;
            }
        }
    }
    // Below half health, a ranged attacker with an enemy beside it steps away
    // rather than shoot: willow best girl's Vesper does in 59-74% of those
    // rounds (3-15% at full health), ours in 33-57%, and ours stood beside
    // enemies 21 rounds a game to willow's 5-11.
    if (!backline && range > 1 && health < max_health * 0.5f && me.can_move() && !kill &&
        std::any_of(enemies.begin(), enemies.end(), [&](const auto &e) {
            return e.health > 0 && distance(position, e.position) <= 1;
        })) {
        const vec2 step = step_away_tile();
        if (step != position && move_step(step)) {
            observe_fight();
            combat(true, -1, true); // A ready reset still shoots.
            return true;
        }
    }
    // Alone, a ranged attacker steps back from a healthy melee enemy two
    // tiles off, which would walk in and strike, and shoots once it is
    // beside it: willow best girl's Vesper steps away at 2 tiles in 47-55%
    // of those rounds (17-21% when the enemy is below half health; at 1
    // tile 15-24%); ours in 30%.
    if (!backline && range > 1 && !covered && me.can_move() && !kill) {
        const ObservedUnit *closest = nullptr;
        int nearest = std::numeric_limits<int>::max();
        for (const auto &e : enemies)
            if (e.health > 0 && distance(position, e.position) < nearest) {
                nearest = distance(position, e.position);
                closest = &e;
            }
        if (closest && nearest == 2 && closest->range <= 1 && closest->health >= closest->max_health * 0.5f) {
            const vec2 step = step_away_tile();
            if (step != position && move_step(step)) {
                observe_fight();
                combat(true, -1, true); // A ready reset still shoots.
                return true;
            }
        }
    }
    if (range <= 1 && d > range) {
        // Deliver available spell burst before a walk would consume the cast.
        // The combo evaluator still chooses a basic/reset when already in range.
        if (d <= ready_reach && combat(true))
            return true;
        if (!on_capture) {
            moved = attack_dash(victim.position);
            moved = advance_attack(victim.position) || moved;
        }
        if (moved)
            observe_fight();
    } else if (range > 1 && d > range) {
        if (d <= ready_reach && combat(true))
            return true;
        // Hold at spell reach while a heal or spell is cast there; with
        // nothing to cast, farm on: ours stood there doing nothing (Prism 17.9
        // rounds a game with an enemy within 4, Willow 10.3; willow best girl's
        // 6.5-6.8 and 2.7-3.9, a third of them striking camps).
        if (backline && d <= policy::backline_reach && !kill)
            return combat(true);
        // The Prism walks in on a fight its teammates are in or on a hurt
        // enemy: willow best girl's, an enemy 5-7 tiles away, steps closer
        // in 20-24% of rounds when it is healthy and no teammate is on it
        // (ours, walking in to 4 tiles, 30%), 34% otherwise (ours 48%).
        if (type == bc::champion_type::prism && !kill && victim.health >= victim.max_health * 0.5f &&
            std::none_of(allies.begin(), allies.end(), [&](const auto &a) {
                return a.health > 0 && distance(a.position, victim.position) <= 3;
            }))
            return false;
        // No walk in on a healthy victim: willow best girl's Vesper, 4 tiles
        // from an enemy, steps closer in 35% of rounds (ours 52%): to a
        // healthy one 27-36% (ours 39-56%), to a hurt one 50-63%.
        if (!backline && !kill && victim.health >= victim.max_health * 0.5f)
            return false;
        if (!on_capture) {
            moved = attack_dash(victim.position);
            moved = advance_attack(victim.position, backline && !kill ? policy::backline_reach : 0) || moved;
        }
        if (moved)
            observe_fight();
    }
    bool acted = combat(true);
    if (acted && me.is_alive() && range <= 1 && me.can_move() && !on_capture)
        moved = follow_through() || moved;
    // A kill can refresh cooldowns (Vendetta), and the follow-through can bring
    // the next enemy into reach: act again with what is left of the turn.
    // Vendetta's damage to champions rose by a sixth.
    if (acted && me.is_alive() && range <= 1 && (me.can_attack() || me.can_cast())) {
        observe_fight();
        combat(true);
    }
    // Melee retains movement after attacking. Chase after a kill and reposition
    // out of splash clumps; ranged champions cannot walk after ordinary attacks.
    if (!acted && !moved && me.can_move() && !on_capture)
        moved = advance_attack(victim.position);
    return acted || moved;
}

bool Bot::confront_hidden() {
    // An enemy out of sight hit us (note_hidden_hit()). It cannot be struck,
    // so the fight decision has three ends: too dangerous to stay (its exit
    // damage, its spells counted ready at the biggest burst of their
    // champions), we leave; with our ward charged we look into its bush and,
    // seeing enemies, fight them as any; a melee champion healthy enough to
    // take that burst and the walk out steps onto the bush, which shows the
    // whole bush, and strikes what it finds (a ranged one would shoot only
    // a round later); otherwise we carry on. Help is called once an enemy is
    // in view, by the usual report: one hit is no squad's worth.
    auto &me = bc::self();
    const vec2 at = hidden->position;
    const bool immune = passive == bc::passive_type::stun_immunity;
    std::array<bool, 2> is_ready{};
    {
        const auto abilities = me.abilities();
        const bool can_cast = me.can_cast();
        for (unsigned i = 0; i < is_ready.size(); ++i)
            is_ready[i] = ready(abilities[i], can_cast);
    }
    const float pool = exit_pool(self_shield(), is_ready);
    if (const auto risk = exit_risk(position, health, mitigation, camp_mitigation, gems, immune);
        risk.damage > 0 && pool <= risk.damage + risk.margin) {
        debug_block({ std::printf("HIDDENACT r=%u id=%u leave\n", round, id(bc::self())); });
        return leave_fight(at);
    }
    // The rest answers a hit this round: without one the record only counts
    // in the danger reckoning (stepping out of reach of a record unhit, our
    // farmers walked back in to farm and out again).
    if (hidden_hit != int(round))
        return false;
    // The ward on the placement tile nearest it, then everything we see.
    if (me.ward_cooldown() <= 0 && !me.buffs().has_stun() &&
        !me.restrictions().contains(bc::buff_restrictions::ward)) {
        auto &placements = step_tiles;
        me.ward_placement_tiles_into(placements);
        bc::vision::ward_tiles_into(bc::team::ally, occupancy);
        occupancy |= blocked;
        occupancy &= placements;
        placements ^= occupancy;
        vec2 spot{-1, -1};
        for (const vec2 p : placements.positions())
            if (spot.x < 0 || distance(p, at) < distance(spot, at))
                spot = p;
        if (spot.x >= 0 && me.place_ward(spot)) {
            ward = spot;
            bc::vision::visible_tiles_into(visible);
            observe_fight();
            if (!enemies.empty()) {
                debug_block({ std::printf("HIDDENACT r=%u id=%u ward-found %zu\n", round, id(bc::self()), enemies.size()); });
                drop_hidden();
                return worker_skirmish();
            }
            if (visible.is_set(at)) {
                debug_block({ std::printf("HIDDENACT r=%u id=%u ward-empty\n", round, id(bc::self())); });
                drop_hidden(); // Nobody there now.
                return false;
            }
        }
    }
    // Not stepping in, a step away from it (step_away_tile(): fewer threats,
    // then more room): our Willow, ranged, stood six rounds under an unseen
    // Willow's 55-damage hits from a bush. Its reach with a ready burst is 3
    // to 5 tiles, which one step never left. A capture's holder stays
    // (leaving its tiles resets the capture).
    const bool on_capture = target >= 0 && groups[target].status == CampGroup::capture &&
                            groups[target].tiles().is_set(position);
    const auto out = [&] {
        if (on_capture)
            return false;
        const vec2 step = step_away_tile();
        const bool stepped = step != position && move_step(step);
        if (stepped) {
            observe_fight();
            combat(true, -1, true);
        }
        debug_block({ std::printf("HIDDENACT r=%u id=%u out=%d\n", round, id(bc::self()), int(stepped)); });
        return stepped;
    };
    if (range > 1 || health < max_health * policy::hidden_step_health || !me.can_move())
        return out();
    // The fight rule worker_skirmish() decides by (no opening or dive here).
    const float theirs = std::max(enemy_power(at), reported_power(at)) + respawn_threat(position);
    if (!fight_favourable(friendly_power(at), theirs, false, false))
        return out();
    // The unseen bush tile within a step nearest it that we would walk out
    // of alive.
    vec2 best = position;
    int nearest = std::numeric_limits<int>::max();
    blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
    for (const vec2 tile : step_positions())
        if (tile != position && bush_tile[tile_index(tile)] && !visible.is_set(tile) && distance(tile, at) < nearest &&
            exit_risk(tile, health, mitigation, camp_mitigation, gems, immune).damage < pool) {
            nearest = distance(tile, at);
            best = tile;
        }
    if (best == position || !move_step(best))
        return out();
    bc::vision::visible_tiles_into(visible);
    observe_fight();
    debug_block({ std::printf("HIDDENACT r=%u id=%u step found=%zu\n", round, id(bc::self()), enemies.size()); });
    combat(true);
    return true;
}

bool Bot::leave_fight(vec2 victim) {
    // A step out of every enemy's reach (no exit damage there) leaves us
    // near, to come back once their spells are cooling: the tile nearest the
    // victim. With none, the whole escape home.
    const bool immune = passive == bc::passive_type::stun_immunity;
    vec2 best = position;
    if (bc::self().can_move()) {
        int nearest = std::numeric_limits<int>::max();
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        for (const vec2 tile : step_positions())
            if (tile != position && distance(tile, victim) < nearest && exit_damage(tile, immune).damage <= 0) {
                nearest = distance(tile, victim);
                best = tile;
            }
    }
    debug_block({ std::printf("RISKLEAVE r=%u id=%u step=%d\n", round, id(bc::self()), int(best != position)); });
    if (best != position && move_step(best)) {
        observe_fight();
        combat(true, -1, true);
        return true;
    }
    return worker_escape();
}

bool Bot::worker_pursue() {
    // willow best girl's champions follow a hurt runner until it dies (their
    // Heart of Ruin chased an Issac seven rounds, from 45% to its death, and
    // 56% of their kills were of champions running 4+ tiles); ours stopped as
    // soon as one stepped into fog. With no enemy in view, an enemy seen in
    // the last ghost_lifetime rounds below pursue_health of its health, and
    // the only one tracked, is followed to its possible tile nearest us while
    // we are above half health.
    const float behind = behind_pressure();
    if (!enemies.empty() || ghosts.size() != 1 || health < max_health * 0.5f * (1.0f - 0.3f * behind))
        return false;
    // A holder stays on its capture: leaving the tiles resets it.
    if (target >= 0 && groups[target].status == CampGroup::capture && groups[target].tiles().is_set(position))
        return false;
    const auto &runner = ghosts.front();
    if (runner.health > runner.max_health * policy::pursue_health * (1.0f + 0.5f * behind))
        return false;
    return advance_attack(runner.position);
}

bool Bot::fight_favourable(float ours, float theirs, bool opening, bool dive) const {
    // A champion with no teammate within 6 fights the opening exchange of a
    // contact at aggression and stays only while as strong as the enemy, one
    // with a single teammate there at pair_aggression: willow best girl's
    // lone champions hit in 34-78% of contacts and leave in 29-61% whatever
    // the odds, dying in 0-6%; ours, staying at aggression up to 3 times
    // their health against them, died in 10-32%.
    int mates = 0;
    for (const auto &a : allies)
        mates += distance(position, a.position) <= 6;
    const float stay = mates == 0 ? 1.0f : (mates == 1 ? policy::pair_aggression : policy::aggression);
    // Alone after the opening, our own power against theirs: teammates that
    // might arrive kept ours in fights against two or more until they died
    // (13% of healthy lone contacts, willow best girl's 4-5%; its champions
    // trade the opening and are out of reach by round 5 in half of them).
    const float holding = mates == 0 && !opening && !dive ? own_power() : ours;
    // Heart of Ruin stays in (policy::hor_fight_factor).
    const float tank = type == bc::champion_type::heart_of_ruin ? policy::hor_fight_factor : 1.0f;
    return holding * (opening || dive ? policy::aggression : stay) * tank *
               (1.0f + policy::behind_fight_bonus * behind_pressure()) >=
           theirs;
}
