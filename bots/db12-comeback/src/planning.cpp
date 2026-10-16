#include "bot.h"

int Bot::choose_group() {
    // This field depends only on immutable terrain and our origin. Reuse it
    // while clearing or holding a camp; dynamic occupancy is routed locally.
    if (origin_position != position) {
        bc::bitboard visited;
        origin_field.compute(static_blocked, around(position, 0), visited);
        origin_position = position;
    }
    float best = -1;
    int choice = -1;
    float progress = std::min(1.0f, round / float(std::max(1u, bc::constants::max_rounds / 2)));
    float gold_value =
        (opening.assault ? policy::early_gold_value : policy::spread_gold_value) * (1 - progress) +
        policy::late_gold_value * progress;
    // Comeback mode: once the farming race is being lost, out-farming the leader
    // rarely closes the gap. Raise the gold valuation so the squad finishes its
    // equipment instead, which is the only way back in through taking bags.
    // The baseline play is unchanged while the race is close or ahead.
    const int our_gems = int(bc::global_submitted_gems(bc::team::ally));
    const int their_gems = int(bc::global_submitted_gems(bc::team::enemy));
    if (their_gems > our_gems + policy::comeback_margin)
        gold_value = std::max(gold_value, policy::comeback_gold_value);
    const float reserve = std::max(max_health * policy::casualty_margin, incoming(position) * 2);
    // Squad assembly: count living allies. After a wipe a lone respawn must not
    // walk straight back into a grouped enemy and feed one champion at a time.
    int alive_allies = 0;
    for (const auto &ally_champion : roster)
        if (ally_champion.is_alive())
            ++alive_allies;
    const bool depleted = float(alive_allies) <= float(roster.size()) * policy::depleted_alive_fraction;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (bc::fuel_remaining() < 28000)
            break;
        const auto &g = groups[i];
        int path = bc::distance_field::unreachable;
        for (const auto &c : g.camps)
            path = std::min(path, int(origin_field.get(c.position)));
        if (path == bc::distance_field::unreachable)
            continue;
        int travel = std::max(0, path - range);
        int waiting = g.status == CampGroup::cooling ? std::max(0, g.respawn - int(round) - travel) : 0;
        unsigned claims = 0, partners = 0;
        for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
            if (s != slot && shared.get(s) == i + 1) {
                ++claims;
                ++partners; // Any actual claimant can supply the next clear's damage.
            }
        bool ally_on = false, enemy_on = false;
        if (g.status == CampGroup::capture) {
            for (const auto &a : allies)
                if (distance(a.position, g.centre) <= int(g.camps.size()) && g.tiles.is_set(a.position))
                    ally_on = true;
            for (const auto &e : enemies)
                if (distance(e.position, g.centre) <= int(g.camps.size()) && g.tiles.is_set(e.position))
                    enemy_on = true;
        }
        if (g.status == CampGroup::capture && ally_on && !g.tiles.is_set(position) && !enemy_on)
            continue;
        float remaining = g.status == CampGroup::cooling ? g.max_health : g.health;
        float speed = dps(g) * (1 + claims * 0.65f);
        float clear = remaining / speed;
        float loss = clear_loss(g, clear);
        float risk = std::max(0.0f, loss - std::max(0.0f, health - reserve)) / std::max(100.0f, health);
        float ranged_dps = 0;
        for (const auto &s : spells)
            if (s.range > 3 && (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                                s.kind == bc::ability_kind::aoe_damage))
                ranged_dps +=
                    focus * s.modifier / s.cooldown * (s.kind == bc::ability_kind::aoe_damage ? g.splash : 1);
        if (risk > 0.15f && ranged_dps > 0) {
            // A slower but lossless range-four clear is an offensive option.
            float siege = remaining / (ranged_dps * camp_multiplier);
            if (siege < clear * (1 + 3 * risk)) {
                clear = siege;
                risk = 0.1f;
            }
        }
        if (g.status == CampGroup::capture) {
            clear = 0;
            risk = 0;
        }
        float reward = g.unit.gem_value() + gold_value * g.unit.gold_value();
        float duration = travel + clear + waiting + bc::constants::capture_time + 1;
        duration += std::min(20, g.altar_distance) * 0.25f;
        float value = reward / std::max(1.0f, duration);
        value /= 1 + risk * 4 + (claims - partners) * 0.55f + partners * policy::partner_penalty;
        if (enemy_on)
            value *= 1.4f;
        // While the squad is depleted, converge on the allies still on the map
        // instead of spreading out and feeding into a grouped enemy one at a
        // time. Avoiding contested camps alone measured worse: it concedes the
        // map, so the bias here is toward the team, not away from the enemy.
        if (g.enemy_distance <= g.home_distance + 8)
            value *= depleted ? policy::depleted_penalty : (1 + policy::contest_bonus);
        if (depleted && claims > 0)
            value *= policy::depleted_rally_bonus;
        if (int(i) == target)
            value *= policy::commit_bonus;
        if (g.status == CampGroup::capture && g.tiles.is_set(position))
            value *= 5;
        // Low-health units keep attacking harmless resources instead of wasting
        // a long reset whenever that still produces score safely.
        if (health < max_health * policy::retreat_health && g.pressure > 0 && ranged_dps == 0)
            value *= 0.1f;
        if (value > best) {
            best = value;
            choice = i;
        }
    }
    last_plan = round;
    return choice;
}

void Bot::farm() {
    auto &me = bc::self();
    if (bc::fuel_remaining() < policy::planning_reserve)
        return;
    bool finished = target >= 0 && groups[target].status == CampGroup::cooling;
    bool opening_rush = slot < opening.rush_size && round < opening.rally_until &&
                        target + 1 == opening.rally_group && !finished;
    if (target < 0 || finished || (!opening_rush && int(round) - last_plan >= policy::replan_period)) {
        if (bc::fuel_remaining() > policy::observation_reserve)
            target = choose_group();
    }
    // Reconsider each member's marginal objective value after the opening.
    // Claims make cooperative clears attractive when the extra damage saves
    // enough time; no permanent leader forces a fixed three-unit formation.
    if (target < 0) {
        publish(0);
        return;
    }
    auto &g = groups[target];
    publish(target < TeamMemory::bank - 1 ? target + 1 : 0);
    if (g.status == CampGroup::capture) {
        if (!g.tiles.is_set(position))
            move_to(g.tiles, false, true);
        combat(false, target); // Heals and shields still work while holding a camp.
        ward_ahead(g.centre);
        return;
    }
    if (g.status == CampGroup::cooling) {
        auto staging = around(g.centre, range + 1) & ~g.tiles;
        if (!staging.is_set(position))
            move_to(staging);
        combat(false, target);
        ward_ahead(g.centre);
        return;
    }
    float clear = g.health / dps(g);
    bool siege = clear_loss(g, clear) > health * (1 - policy::casualty_margin);
    float spell_range = 0;
    for (const auto &s : spells)
        if (s.modifier > 0 && (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                               s.kind == bc::ability_kind::aoe_damage))
            spell_range = std::max(spell_range, s.range);
    siege = siege && spell_range > 3;
    int attack_range = siege ? static_cast<int>(spell_range) : range;
    bc::bitboard goals;
    const CampMemory *nearest = nullptr;
    int best_distance = bc::distance_field::unreachable;
    for (const auto &c : g.camps) {
        if (c.health <= 0)
            continue;
        int d = distance(position, c.position);
        if (d < best_distance) {
            best_distance = d;
            nearest = &c;
        }
        goals |= around(c.position, attack_range);
    }
    goals &= ~blocked;
    if (passive == bc::passive_type::splash_attack && !siege &&
        bc::fuel_remaining() > policy::planning_reserve) {
        // Stand beside the best splash centre. Stopping at the nearest edge
        // of a row can halve Varg's damage for the entire camp clear.
        float best = -1;
        for (const auto &primary : g.camps)
            if (primary.health > 0) {
                if (bc::fuel_remaining() < policy::planning_reserve)
                    break;
                auto approaches = around(primary.position, range) & ~blocked;
                if (approaches.is_empty())
                    continue;
                float damage = 0;
                for (const auto &other : g.camps)
                    if (distance(primary.position, other.position) <=
                        bc::constants::passive_abilities::varg_splash_radius)
                        damage += std::min(strength, std::max(0.0f, other.health));
                float value =
                    damage - strength * 0.1f * std::max(0, distance(position, primary.position) - range);
                if (value > best) {
                    best = value;
                    goals = std::move(approaches);
                }
            }
    }
    if (siege)
        for (const auto &c : g.camps)
            if (c.health > 0 && c.stats.base_strength > 0)
                goals &= ~around(c.position, c.stats.base_attack_range);
    if (!nearest) {
        target = -1;
        return;
    }
    if (siege && !goals.is_set(position)) {
        move_to(goals, true, false, target);
        // Walking precludes ranged damage spells this turn.
        if (walked) {
            ward_ahead(nearest->position);
            return;
        }
    }
    if (range <= 1 && !goals.is_set(position) && !siege) {
        move_to(goals, false, true, target);
        if (bc::fuel_remaining() > policy::observation_reserve)
            observe();
    }
    combat(false, target);
    if (!me.is_alive())
        return;
    if (me.can_move() && !goals.is_set(position) && bc::fuel_remaining() > policy::planning_reserve) {
        move_to(goals, false, true, target);
        if (bc::fuel_remaining() > policy::observation_reserve) {
            observe();
            combat(false, target);
        }
    }
    // Move onto the cleared region immediately with the melee move allowance.
    if (g.seen == int(round) && bc::fuel_remaining() > 14000 &&
        !(g.tiles & bc::vision::visible_tiles()).is_empty()) {
        auto state = g.unit.state();
        if (std::holds_alternative<bc::map::camp_group_reward_available>(state)) {
            g.status = CampGroup::capture;
            blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                      bc::vision::teleport_tiles();
            blocked.set(position, false);
            if (!g.tiles.is_set(position))
                move_to(g.tiles, false, false);
        }
    }
    if (bc::fuel_remaining() > 6000)
        ward_ahead(nearest->position);
}
