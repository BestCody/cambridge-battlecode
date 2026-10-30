// Worker skirmish decisions against enemy champions.
#include "../bot.h"
#include <tuple>

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
    // The victim, by the micro's own rule (kills, then damage): the enemy we
    // kill in the fewest turns, the least health our damage must go
    // through, then the nearest.
    int selected = -1;
    std::pair<float, int> best{};
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
        const std::pair<float, int> key{-(e.health + e.shield) / std::max(0.01f, e.mitigation), -d};
        if (selected < 0 || key > best || (contact && !selected_contact)) {
            best = key;
            selected = i;
            selected_contact = contact;
        }
    }
    if (selected < 0)
        return false;
    const auto victim = enemies[selected];
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
    // Too dangerous to stay, decided every round: our health and shields
    // reach no further than the damage the enemies deal us before we are
    // out of their reach from here (exit_damage(), their ready bursts at
    // full and their cooldowns read) plus a margin for our bag (a round of
    // their hits per exit_bag_gems gems). The power test counts burst at a fifth
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
        std::printf("RISK r=%u id=%u type=%d hp=%.0f pool=%.0f exit=%.0f margin=%.0f gems=%u kill=%d "
                    "finish=%d td=%d ovr=%d\n",
                    round, id(bc::self()), int(type), health, pool, risk.damage, risk.margin, gems, int(kill),
                    int(finish), int(too_dangerous), int(overridden));
    });
    if (too_dangerous && !overridden)
        return leave_fight(victim.position);
    // The fight, as XSquare's open Battlecode micro decides it (2022-2024
    // winners'; most top teams ran a copy): no fight-or-flee rule and no
    // aggression factor, but every tile we can end on, scored each turn
    // from what is on the board, and the best taken. Their classes, tile by
    // tile: 0 a losing spot (we would die there, or take more there next
    // turn than we deal from it this turn, with our side no stronger than
    // theirs: a turn's trade on both sides, the damage taken at its most), 1
    // outnumbered (the enemies that can reach it next turn outweigh our
    // side that can), 2 safe; then
    // the most kills from it, then the most damage dealt from it (the damage taken only sets the
    // class, as in XSquare's), then distance (the longest reach while we can strike, else closer to
    // the victim chosen above: low health, gems, channelling, a dive on a
    // teammate; closing on the nearest enemy instead took our kills per
    // attack from 0.26 to 0.16), then the most of our side able to act
    // there. The side count ranked
    // before distance pulled a bystander toward an idle teammate and away
    // from the fight it was joining (00lost-world r214-221: seven rounds
    // 4-5 tiles from our Marcella dying 1v2). Nothing ready: every
    // tile counts as striking, so the farthest within reach wins (they back
    // off between shots). Sides are powers (health with damage), not counts: our
    // champions run 1,000 to 3,800 health. A teammate counts where it can
    // act, so a tile beside one in a fight is backed and the fight is
    // joined: bystanders 2-7 tiles from a teammate dying 1v3 stepped away
    // in all five of print dont int's ranked games we lost on replay.
    auto &me = bc::self();
    const bool on_capture = target >= 0 && groups[target].status == CampGroup::capture &&
                            groups[target].tiles().is_set(position);
    const bool attack_ready = me.can_attack();
    bool nothing_ready = !attack_ready;
    for (unsigned i = 0; i < spells.size(); ++i)
        if (is_ready[i] && (spells[i].kind == bc::ability_kind::attack_reset ||
                            (spells[i].modifier > 0 && (spells[i].kind == bc::ability_kind::damage ||
                                                        spells[i].kind == bc::ability_kind::cc ||
                                                        spells[i].kind == bc::ability_kind::aoe_damage ||
                                                        spells[i].kind == bc::ability_kind::dash))))
            nothing_ready = false;
    // Who can act on a tile by next turn: its attack or spell reach plus a
    // move (none while a stun holds it).
    const auto acts_on = [](const ObservedUnit &u, vec2 tile) {
        const int move = u.stunned && u.stun_time > 0 ? 0 : u.move_speed;
        return distance(u.position, tile) <= std::max(u.range, u.spell_reach) + move;
    };
    const auto power_of = [&](const ObservedUnit &u, bool full) {
        return unit_power((full ? u.max_health : u.health) + u.shield + u.ready_shield, u.damage_rate,
                          u.stunned && u.stun_time > 0 ? 0.0f : u.burst, u.healing_rate, u.mitigation);
    };
    // Each side's units once: enemies in view and out of sight (from their
    // possible tile nearest us), at full health on their spawn, where they
    // heal (enemy_power()); allies in view not channelling.
    struct Side {
        const ObservedUnit *unit;
        float power;
    };
    std::array<Side, 16> theirs_list{}, ours_list{};
    std::size_t their_count = 0, our_count = 0;
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &e : *list)
            if (e.health > 0 && !e.channel && their_count < theirs_list.size())
                theirs_list[their_count++] = {&e, power_of(e, enemy_spawn_tile[tile_index(e.position)])};
    for (const auto &a : allies)
        if (a.health > 0 && !a.channel && our_count < ours_list.size())
            ours_list[our_count++] = {&a, power_of(a, false)};
    const float own = own_power(), life = health + self_shield();
    // Each ready spell's damage to a champion (reset: a second attack; dash:
    // the engine's figure), read once, and whether its hit stuns, as the
    // executors count one (attack_combo(): a control or area spell with a
    // duration, a reset's enhanced hit; impact_dash(): its landing).
    const float basic = basic_damage();
    std::array<float, 2> spell_hit{};
    std::array<bool, 2> stuns{};
    {
        const auto kit = me.abilities();
        for (unsigned i = 0; i < spells.size(); ++i) {
            const auto &sp = spells[i];
            if (!is_ready[i])
                continue;
            if (sp.kind == bc::ability_kind::attack_reset)
                spell_hit[i] = basic;
            else if (sp.modifier > 0 && (sp.kind == bc::ability_kind::damage || sp.kind == bc::ability_kind::cc ||
                                         sp.kind == bc::ability_kind::aoe_damage))
                spell_hit[i] = focus * sp.modifier;
            else if (sp.modifier > 0 && sp.kind == bc::ability_kind::dash)
                spell_hit[i] = kit[i].damage_amount();
            stuns[i] = sp.kind == bc::ability_kind::attack_reset
                           ? sp.enhanced
                           : sp.duration > 0 && (sp.kind == bc::ability_kind::cc ||
                                                 sp.kind == bc::ability_kind::aoe_damage ||
                                                 (sp.kind == bc::ability_kind::dash && sp.modifier > 0));
        }
    }
    // The sides that can act on a tile by next turn: theirs, then ours (we
    // count wherever we stand).
    const auto sides = [&](vec2 tile) {
        std::pair<float, float> s{0.0f, own};
        for (std::size_t k = 0; k < their_count; ++k)
            if (acts_on(*theirs_list[k].unit, tile))
                s.first += theirs_list[k].power;
        for (std::size_t k = 0; k < our_count; ++k)
            if (acts_on(*ours_list[k].unit, tile))
                s.second += ours_list[k].power;
        return s;
    };
    // The enemies a tile we reach can strike, once: alive, off their spawn,
    // within our longest reach of a tile a step takes us to.
    std::array<const ObservedUnit *, 16> near{};
    std::size_t near_count = 0;
    {
        int reach = range;
        for (unsigned i = 0; i < spells.size(); ++i)
            if (spell_hit[i] > 0 || stuns[i])
                reach = std::max(reach, int(spells[i].range + spells[i].radius));
        reach += int(stats.base_move_speed);
        for (const auto &e : enemies)
            if (e.health > 0 && !enemy_spawn_tile[tile_index(e.position)] && near_count < near.size() &&
                distance(position, e.position) <= reach)
                near[near_count++] = &e;
    }
    // A stun on one of them that lives through our strike, worth what
    // stun_value() gives it in the executors: the damage its skipped turn
    // would have dealt, and the window (a kill when it covers what is left).
    // Read once per enemy, only with a stunning hit ready (a held enhanced
    // attack's included).
    const bool enhanced = attack_ready && me.buffs().has_enhanced_attack();
    std::array<float, 16> spared{}, window{};
    std::array<bool, 16> stunnable{};
    float stun_kill = 0;
    if (enhanced || stuns[0] || stuns[1]) {
        stun_kill = kill_value();
        for (std::size_t k = 0; k < near_count; ++k) {
            const auto &e = *near[k];
            stunnable[k] = !(e.stunned && e.stun_time > 0) && !e.immune;
            spared[k] = stun_prevented(e);
            window[k] = stun_follow_up(e);
        }
    }
    // Then the nearest enemy in our attack range from it (XSquare's 2022
    // attacker: inRange right after its safety class; nothing ready, every
    // tile counts as in range), then nearer our nearest teammate in view,
    // before the distance to the enemy (his 2023 attacker: minDistToAlly
    // between landing a hit and the distance to the enemy). Without the
    // range key a melee champion that struck before its step, every step
    // tile alike, stepped out of reach toward a teammate: 67% of the micro's
    // 13.5 hit-and-runs a game, 97% ending out of every enemy's reach.
    struct Score {
        int safety = 0, kills = 0, strike = 0;
        float dealt = 0, taken = 0, ours = 0;
        int in_range = 0, ally = 0, reach = 0;
        bool operator>(const Score &o) const {
            return std::tie(safety, kills, dealt, in_range, ally, reach, ours) >
                   std::tie(o.safety, o.kills, o.dealt, o.in_range, o.ally, o.reach, o.ours);
        }
    };
    // What we deal from a tile this turn, as XSquare's sage scores one: the
    // most kills, then the most damage (each enemy's at most its health,
    // and a stun's worth on one that lives, as the executors add it), of
    // every way to aim the ready attack (one enemy in range) and one ready
    // spell (a reset's second hit or a spell on one enemy, an area on every
    // enemy in its radius; one that excludes walking only where we stand).
    // A tile was a yes or no before: an area hit on two counted as on one,
    // and a kill as any strike once one tile had one.
    // A ranged champion walks or attacks, not both (a ready reset gives the
    // attack back after the walk: its one hit); the micro had counted its
    // attack from every tile it walks to.
    std::array<float, 16> hurt{};
    std::array<bool, 16> held{}; // Stunned by the aim's spell.
    const auto aims = [&](vec2 tile, bool walks, Score &s) {
        const bool attacks = attack_ready && (range <= 1 || !walks);
        const auto tally = [&]() {
            for (int basic_on = -1; basic_on < int(near_count); ++basic_on) {
                if (basic_on >= 0 && (!attacks || distance(tile, near[basic_on]->position) > range))
                    continue;
                int kills = 0;
                float dealt = 0;
                for (std::size_t k = 0; k < near_count; ++k) {
                    const auto &e = *near[k];
                    const float life_k = e.health + e.shield;
                    const bool struck = int(k) == basic_on;
                    const float d = hurt[k] + (struck ? basic * e.mitigation : 0.0f);
                    kills += d > 0 && d >= life_k;
                    dealt += std::min(d, life_k);
                    if ((held[k] || (struck && enhanced)) && stunnable[k] && d < life_k)
                        dealt += spared[k] + (life_k - d <= window[k] ? stun_kill : window[k]);
                }
                if (std::tie(kills, dealt) > std::tie(s.kills, s.dealt)) {
                    s.kills = kills;
                    s.dealt = dealt;
                }
            }
        };
        hurt.fill(0.0f);
        held.fill(false);
        tally();
        for (unsigned i = 0; i < spells.size(); ++i) {
            const auto &sp = spells[i];
            if ((spell_hit[i] <= 0 && !stuns[i]) || (sp.exclusive && walks))
                continue;
            if (sp.kind == bc::ability_kind::aoe_damage) {
                const int radius = int(sp.radius);
                for (std::size_t k = 0; k < near_count; ++k) {
                    if (distance(tile, near[k]->position) > sp.range + sp.radius)
                        continue;
                    for (int dx = -radius; dx <= radius; ++dx)
                        for (int dy = -radius; dy <= radius; ++dy) {
                            const vec2 centre{near[k]->position.x + dx, near[k]->position.y + dy};
                            if (distance(tile, centre) > sp.range || !bc::map::is_in_bounds(centre) ||
                                no_centre_tile[tile_index(centre)])
                                continue;
                            for (std::size_t j = 0; j < near_count; ++j) {
                                const bool in = distance(centre, near[j]->position) <= radius;
                                hurt[j] = in ? spell_hit[i] * near[j]->mitigation : 0.0f;
                                held[j] = in && stuns[i];
                            }
                            tally();
                        }
                }
                continue;
            }
            const float reach = sp.kind == bc::ability_kind::attack_reset ? float(range) : sp.range + sp.radius;
            for (std::size_t k = 0; k < near_count; ++k) {
                if (float(distance(tile, near[k]->position)) > reach)
                    continue;
                hurt.fill(0.0f);
                held.fill(false);
                hurt[k] = spell_hit[i] * near[k]->mitigation;
                held[k] = stuns[i];
                tally();
            }
        }
    };
    // A melee champion with an enemy in its reach now strikes from where it
    // stands before it steps (strike_before_step(), spells that exclude
    // walking held): a tile it steps to scores that strike, not one from the
    // tile (the attack it planned from there was spent before the step).
    bool strikes_first = false;
    if (range <= 1)
        for (const auto &e : enemies)
            strikes_first = strikes_first || (e.health > 0 && distance(position, e.position) <= range);
    Score struck_here;
    if (strikes_first)
        aims(position, true, struck_here);
    const auto score = [&](vec2 tile) {
        Score s;
        // The most it can take next turn (incoming(): every enemy that can hit
        // it does, one ability a turn; the camps; our mitigation).
        s.taken = incoming(tile);
        if (strikes_first && tile != position) {
            s.kills = struck_here.kills;
            s.dealt = struck_here.dealt;
        } else
            aims(tile, tile != position, s);
        const float dealt = s.dealt;
        int nearest = std::numeric_limits<int>::max();
        for (const auto &e : enemies)
            if (e.health > 0)
                nearest = std::min(nearest, distance(tile, e.position));
        s.strike = nothing_ready || dealt > 0;
        if (our_count > 0) {
            int mate = std::numeric_limits<int>::max();
            for (std::size_t k = 0; k < our_count; ++k)
                mate = std::min(mate, distance(tile, ours_list[k].unit->position));
            s.ally = -mate;
        }
        const auto [theirs, ours] = sides(tile);
        s.ours = ours;
        if (s.taken >= life || (s.taken > dealt && s.ours <= theirs))
            s.safety = 0;
        else if (theirs > s.ours)
            s.safety = 1;
        else
            s.safety = 2;
        // The longest reach from the nearest enemy while we strike from it and
        // it stays in our attack range there (XSquare's inRange: in range,
        // the farthest; out of it, closer), else closer to the victim; with
        // nothing ready, every tile the farthest (his alwaysInRange). Without
        // the range, a melee champion that struck before its step stepped
        // three tiles off, then spent its next move walking back (00ambush
        // r369-377, three of ours on one enemy at 710 of 2400): the micro's
        // own hit-and-runs went from 6 to 30 a game.
        s.in_range = nearest != std::numeric_limits<int>::max() && (nothing_ready || nearest <= range);
        s.reach = s.strike && s.in_range ? nearest : -distance(tile, victim.position);
        return s;
    };
    // The fight is ours to play when an enemy in view can act on our tile,
    // we can strike one from a tile we reach, one can act on a teammate in
    // view (the bystander joins), or we outweigh the sides on its tile (the
    // chase: its tile is safe for us, so the tiles toward it are); otherwise
    // farming and travel go on. Ghosts weigh on the tiles but start no
    // fight. Without the chase a wounded enemy walked off while we farmed
    // and the teammate that followed it fought alone (00collision, r53-83).
    bool engaged = false;
    for (const auto &e : enemies) {
        if (engaged)
            break;
        if (e.health <= 0 || e.channel || enemy_spawn_tile[tile_index(e.position)])
            continue;
        engaged = acts_on(e, position);
        for (std::size_t j = 0; j < our_count && !engaged; ++j)
            engaged = acts_on(e, ours_list[j].unit->position);
        if (!engaged) {
            const auto [theirs, ours] = sides(e.position);
            engaged = ours > theirs;
        }
    }
    vec2 stand = position;
    Score best_score = score(position);
    const Score here = best_score;
    if (!on_capture && me.can_move()) {
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        for (const vec2 tile : step_positions()) {
            if (tile == position)
                continue;
            const Score s = score(tile);
            engaged = engaged || (s.strike && !nothing_ready);
            if (s > best_score) {
                best_score = s;
                stand = tile;
            }
        }
    }
    engaged = engaged || (here.strike && !nothing_ready);
    debug_block({
        std::printf("MICRO r=%u id=%u type=%d engaged=%d here=%d/%d/%d stand=%d,%d %d/%d/%d taken=%.0f ours=%.0f "
                    "theirs=%.0f\n",
                    round, id(bc::self()), int(type), int(engaged), here.safety, here.kills, here.strike, stand.x,
                    stand.y, best_score.safety, best_score.kills, best_score.strike, best_score.taken,
                    best_score.ours, sides(stand).first);
    });
    if (!engaged)
        return false;
    // Outnumbered where we stand: the camps by these enemies wait out the
    // sighting (camp choice).
    if (here.safety < 2)
        worker_track_sightings();
    // No safe tile within a step and no kill: a group's reach and move cover
    // every tile one step takes us to, so the step cannot leave it; the
    // retreat (a step out of reach, else the escape home) takes over. Held
    // to the best tile, our champion alone beside an enemy Heart of Ruin with
    // four more coming struck from tiles of class 1 for four rounds and died
    // (00lost-world r223-229).
    if (best_score.safety < 2 && !best_score.kills && !kill && !finish)
        return leave_fight(victim.position);
    // A champion farming (it struck its camp in the last two rounds) farms
    // on from a safe tile while no enemy is in its strike: willow best
    // girl's farmers, the nearest enemy 4-6 tiles away, keep hitting their
    // camp in 49% of rounds. Held by the micro instead, ours stopped
    // clearing as soon as an enemy came near (captures by r300 8.8 to 7.5;
    // 65% of the micro's turns had no strike from any tile in reach).
    if (here.safety == 2 && !(here.strike && !nothing_ready) && target >= 0 &&
        groups[target].attacked >= int(round) - 2)
        return false;
    if (stand != position) {
        // A melee champion walks and strikes in one turn: it strikes what
        // reaches it first, then steps (spells that exclude walking wait).
        strike_before_step();
        if (move_step(stand))
            observe_fight();
    }
    // The tile is the micro's: the attacks below take no step of their own.
    placed = true;
    bool acted = combat(true);
    // Holding with no champion to strike: a camp of our group in reach
    // still is (farming's combat).
    if (!acted && target >= 0 && me.is_alive())
        acted = combat(false, target);
    // A kill can refresh cooldowns (Vendetta): act again with what is left
    // of the turn.
    if (acted && me.is_alive() && range <= 1 && (me.can_attack() || me.can_cast())) {
        observe_fight();
        combat(true);
    }
    placed = false;
    // The fight holds the turn: a tile held is the micro's choice, not
    // farming's to walk off.
    return true;
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
    // Our side against theirs at the bush, no factor (as worker_skirmish()
    // weighs a tile's sides).
    const float theirs = std::max(enemy_power(at), reported_power(at)) + respawn_threat(position);
    if (friendly_power(at) < theirs)
        return out();
    // The unseen bush tile within a step nearest it that we would walk out
    // of alive.
    vec2 best = position;
    int nearest = std::numeric_limits<int>::max();
    blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
    for (const vec2 tile : step_positions()) {
        if (tile == position || !bush_tile[tile_index(tile)] || visible.is_set(tile) || distance(tile, at) >= nearest)
            continue;
        // With the bag margin, as the stay test above weighs it.
        if (const auto risk = exit_risk(tile, health, mitigation, camp_mitigation, gems, immune);
            risk.damage + risk.margin < pool) {
            nearest = distance(tile, at);
            best = tile;
        }
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
