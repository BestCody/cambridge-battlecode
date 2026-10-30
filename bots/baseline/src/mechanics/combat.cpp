// Single-turn attack and cast selection, plus kill and linked-fate values.
#include "../bot.h"

namespace {
bool present(const ObservedUnit &u) { return live_health(u) > 0; }
} // namespace

bool Bot::combat(bool fighting_champions, int farm_group, bool escaping) {
    profiling::Scope scope(profiling::Stage::combat);
    auto &me = bc::self();
    const auto state = me.state();
    if (state.contains(bc::champion_state::teleporting) || state.contains(bc::champion_state::recalling) ||
        state.contains(bc::champion_state::submitting_gems))
        return false;
    if (!state.contains(bc::champion_state::can_attack) && !state.contains(bc::champion_state::can_cast))
        return false;
    // A damaging dash (Heart of Ruin) engages first; the rest of the turn
    // continues from the landing tile.
    const bool dashed = !escaping && impact_dash(fighting_champions, farm_group);
    if (dashed)
        observe_fight(); // The attack search reads the enemies as the landing left them.
    if (fighting_champions && attack_combo(escaping))
        return true;
    // A camp is a target only in the farmed group: the targets are its records
    // (every other camp weighs nothing and is never chosen).
    const std::span<ObservedUnit> targets =
        fighting_champions ? std::span<ObservedUnit>(enemies) : camps.group(farm_group);
    const bool has_retaliation =
        fighting_champions &&
        std::any_of(targets.begin(), targets.end(), [](const auto &u) { return u.retaliation() > 0; });
    auto read_shield = [&]() {
        if (!has_retaliation)
            return 0.0f;
        const auto buffs = me.buffs();
        return buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0.0f;
    };
    float own_shield = read_shield();
    // Storage kept between calls: a vector allocated afresh costs about 200
    // fuel. Every entry is written below.
    static std::vector<float> controls;
    controls.resize(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i)
        controls[i] = (targets[i].strength + targets[i].burst) * (escaping ? 3 : 1);
    auto eligible = [&](const ObservedUnit &u) {
        return (fighting_champions || u.group == farm_group) && u.health > 0 &&
               !(fighting_champions && enemy_spawn_tile[tile_index(u.position)]);
    };
    // Our rate on the farmed group's camps (camp choice's own).
    const float camp_rate = !fighting_champions && farm_group >= 0 ? dps(groups[farm_group]) : 1.0f;
    auto weight = [&](const ObservedUnit &u, float damage, bool cc) {
        if (!eligible(u))
            return 0.0f;
        float effective = damage * (u.enemy() ? u.mitigation : camp_multiplier);
        float value = std::min(effective, u.health + u.shield);
        const float useful = value;
        // An enemy champion as the micro counts it: a kill, then damage.
        if (effective >= u.health + u.shield)
            value += u.enemy() ? kill_value() : u.strength * 8;
        if (!u.enemy())
            // Strikers first (striker_work()): a hit ends a striker
            // useful / rate rounds sooner and spares its strength each of
            // them; a harmless camp spares nothing, as every camp must fall.
            // A one-hit kill on a harmless cluster outranked a hit on the elk
            // beside it, which struck through the whole clear.
            value += u.strength * useful / camp_rate;
        if (cc && !(u.stunned && u.stun_time > 0) && !u.immune) {
            if (!u.enemy())
                value += controls[&u - targets.data()];
            else if (effective < u.health + u.shield)
                value += stun_value(u, u.health + u.shield - effective);
        }
        if (u.retaliation() > 0 && effective > 0) {
            const float recoil = u.retaliation() * mitigation;
            value -= recoil;
            if (recoil >= health + own_shield)
                value -= death_cost();
        }
        return value;
    };
    auto basic_choice = [&]() -> int {
        if (!me.can_attack())
            return -1;
        me.attack_tiles_into(attack_area);
        const auto &attack_tiles = attack_area;
        float best = 0;
        int index = -1;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            auto &u = targets[i];
            if (!eligible(u) || !attack_tiles.is_set(u.position) || !present(u))
                continue;
            float value = weight(u, basic_damage(), false);
            if (value > best) {
                best = value;
                index = i;
            }
        }
        return index;
    };
    bool acted = false;
    auto do_attack = [&]() {
        int index = basic_choice();
        if (index < 0)
            return false;
        auto &u = targets[index];
        // An enemy only a teammate sees stands, for the engine, where we
        // last saw it: attacking it there threw ("attack target is out of
        // range") and lost the rest of the turn. Out of reach, it is out of
        // this turn's choices.
        if (u.enemy() && !attack_area.is_set(u.unit().position())) {
            u.health = 0;
            return false;
        }
        const vec2 struck = u.position;
        me.attack(u.unit());
        acted = true;
        if (u.group >= 0)
            groups[u.group].attacked = round;
        // Refresh health before the reset selection so a dead or overkilled
        // unit cannot consume actions, and ours after any retaliation.
        bool gone = false;
        for (auto &t : targets)
            if (eligible(t) && t.position == struck) {
                t.health = live_health(t);
                gone = gone || t.health <= 0;
            }
        // A champion killed leaves incoming()'s estimate: its packed enemies
        // and nearest units are made again.
        if (fighting_champions && gone)
            ++threat_revision;
        refresh_self();
        own_shield = read_shield();
        return true;
    };
    // Normal attack precedes resets. Ranged champions may also move, reset and
    // attack; the reset evaluator below accounts for that restored allowance.
    if (!escaping || range <= 1)
        do_attack();
    if (!me.is_alive())
        return acted || dashed;

    int best_slot = -1, best_target = -1;
    vec2 best_tile{};
    float best_score = 0;
    auto abilities = me.abilities();
    const bool can_cast = me.can_cast();
    for (int slot_index = 0; slot_index < 2; ++slot_index) {
        const auto &a = abilities[slot_index];
        const auto &s = spells[slot_index];
        if (!ready(a, can_cast) || (walked && s.exclusive))
            continue;
        if (s.kind == bc::ability_kind::attack_reset) {
            if (me.can_attack())
                continue;
            float score = 0;
            for (const auto &u : targets)
                if (eligible(u) && distance(position, u.position) <= range && present(u))
                    score = std::max(score, weight(u, basic_damage(), s.enhanced));
            if (score > best_score) {
                best_score = score;
                best_slot = slot_index;
                best_target = -1;
            }
        } else if (s.kind == bc::ability_kind::shield) {
            float danger = incoming(position);
            float score = std::min(a.shield_amount(), danger * std::max(1, s.duration)) * 0.85f;
            if (health < danger * 4)
                score *= 2;
            if (me.buffs().has_shield())
                score *= 0.2f;
            if (score > best_score) {
                best_score = score;
                best_slot = slot_index;
                best_target = -1;
            }
        } else if (s.kind == bc::ability_kind::aoe_damage) {
            // Evaluate each unit's surrounding centres, not only occupied tiles:
            // radius-one spells can hit targets five tiles from the caster.
            // A centre scores the weights of the eligible units in the radius.
            // Each weight is computed once and added onto a grid of the centres
            // in spell range, in unit order as a per-centre sum adds them.
            // Ties go to the centre first reached scanning each unit's
            // surroundings in unit order.
            // The grids are kept between calls, each cell at zero and never
            // reached until written; the cells the last call wrote are reset
            // first. Only written cells are scanned: the choice does not
            // depend on scan order, as each reached cell has its own first.
            const float damage = a.damage_amount();
            const int radius = static_cast<int>(s.radius), reach = static_cast<int>(s.range);
            const int side = 2 * reach + 1;
            static std::vector<float> score;
            static std::vector<int> first, written;
            static std::vector<std::uint8_t> touched;
            for (const int cell : written) {
                score[cell] = 0;
                first[cell] = std::numeric_limits<int>::max();
                touched[cell] = 0;
            }
            written.clear();
            if (score.size() < std::size_t(side) * side) {
                score.assign(std::size_t(side) * side, 0.0f);
                first.assign(std::size_t(side) * side, std::numeric_limits<int>::max());
                touched.assign(std::size_t(side) * side, 0);
            }
            // Each unit's surroundings are numbered in order (dx, then dy), and
            // only those within spell range of us are visited.
            const int around = 2 * radius + 1;
            int order = 0;
            for (const auto &u : targets) {
                if (!eligible(u))
                    continue;
                const float w = weight(u, damage, s.duration > 0);
                const bool scanned = distance(position, u.position) <= s.range + s.radius;
                const int ox = u.position.x - position.x, oy = u.position.y - position.y;
                const int dy_low = std::max(-radius, -reach - oy), dy_high = std::min(radius, reach - oy);
                for (int dx = std::max(-radius, -reach - ox), dx_high = std::min(radius, reach - ox); dx <= dx_high;
                     ++dx)
                    for (int dy = dy_low; dy <= dy_high; ++dy) {
                        const int cell = (oy + dy + reach) * side + ox + dx + reach;
                        if (!touched[cell]) {
                            touched[cell] = 1;
                            written.push_back(cell);
                        }
                        score[cell] += w;
                        if (scanned)
                            first[cell] = std::min(first[cell], order + (dx + radius) * around + dy + radius);
                    }
                order += around * around;
            }
            int chosen = -1;
            float chosen_score = 0;
            for (const int cell : written) {
                if (first[cell] == std::numeric_limits<int>::max())
                    continue;
                const vec2 tile{position.x + cell % side - reach, position.y + cell / side - reach};
                if (!bc::map::is_in_bounds(tile) || no_centre_tile[tile_index(tile)])
                    continue;
                const float value = score[cell];
                if (chosen < 0 || value > chosen_score ||
                    (value == chosen_score && first[cell] < first[chosen])) {
                    chosen = cell;
                    chosen_score = value;
                }
            }
            if (chosen >= 0 && chosen_score > best_score) {
                best_score = chosen_score;
                best_slot = slot_index;
                best_target = -1;
                best_tile = {position.x + chosen % side - reach, position.y + chosen / side - reach};
            }
        } else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc) {
            if (fighting_champions ? !s.targets.contains(bc::ability_target_mask::enemies)
                                   : !s.targets.contains(bc::ability_target_mask::camps))
                continue;
            float damage = a.damage_amount();
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const auto &u = targets[i];
                if (!eligible(u) || distance(position, u.position) > s.range || !present(u))
                    continue;
                float score = weight(u, damage, s.kind == bc::ability_kind::cc);
                if (score > best_score) {
                    best_score = score;
                    best_slot = slot_index;
                    best_target = i;
                }
            }
        }
    }
    if (best_slot >= 0 && best_score > 1) {
        const auto &a = abilities[best_slot];
        const auto &s = spells[best_slot];
        bool success = false;
        if (s.kind == bc::ability_kind::attack_reset)
            // Read again here, not as scored above: a turn that ran out of
            // fuel in between resumes next round with its attack back, and
            // resetting an available attack is an engine error that threw
            // away the rest of the turn (2-5 times in 42 games at 100k).
            success = !me.can_attack() && a.use();
        else if (s.kind == bc::ability_kind::shield)
            success = a.use();
        else if (s.kind == bc::ability_kind::aoe_damage)
            success = a.use(best_tile);
        else if (!targets[best_target].enemy() ||
                 distance(position, vec2(targets[best_target].unit().position())) <= s.range)
            success = std::visit([&](const auto *u) { return a.use(*u); }, targets[best_target].ref);
        if (success) {
            acted = true;
            // Only a cast that strikes the group wakes it: a shield cast on
            // the way woke sleeping camps in our records (and let a
            // hard group's party go in without its partner). A reset's
            // attack marks the group as any attack does.
            if (farm_group >= 0 && (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                                    s.kind == bc::ability_kind::aoe_damage))
                groups[farm_group].attacked = round;
            bool gone = false;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                auto &u = targets[i];
                if (eligible(u) && ((s.kind == bc::ability_kind::aoe_damage &&
                                     distance(u.position, best_tile) <= s.radius) ||
                                    ((s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc) &&
                                     int(i) == best_target))) {
                    u.health = live_health(u);
                    gone = gone || u.health <= 0;
                }
            }
            // As after an attack: a champion killed leaves incoming().
            if (fighting_champions && gone)
                ++threat_revision;
            refresh_self();
            own_shield = read_shield();
            do_attack();
        }
    }
    return acted || dashed;
}

float Bot::basic_damage() const {
    // Our basic hit before the target's mitigation. Heart of Ruin's passive
    // adds up to half again with missing health, which the strength reading
    // leaves out (its hits matched strength x (1 + 0.5 x missing) exactly).
    if (passive != bc::passive_type::missing_health_damage)
        return strength;
    return strength * hor_bonus(health, max_health);
}

float Bot::stun_prevented(const ObservedUnit &e) const {
    if ((e.stunned && e.stun_time > 0) || e.immune)
        return 0;
    // One ability a turn: its hit, then a reset's second hit or its burst.
    const float blow = e.strength + std::max(e.reset_ready ? e.strength : 0.0f, e.spell_burst);
    const int reach = std::max(e.range, e.spell_reach) + e.move_speed;
    float most = distance(position, e.position) <= reach ? std::min(blow * mitigation, health) : 0.0f;
    for (const auto &a : allies)
        if (a.health > 0 && distance(a.position, e.position) <= reach)
            most = std::max(most, std::min(blow * a.mitigation, a.health));
    return most;
}

float Bot::stun_follow_up(const ObservedUnit &e) const {
    // Teammates that finished this round cannot act before its next turn.
    float damage = 0;
    for (const auto &a : allies) {
        if (a.health <= 0 || a.completed || (a.stunned && a.stun_time > 0) || a.channel)
            continue;
        const int d = distance(a.position, e.position);
        if (d <= a.range)
            damage += a.strength;
        if (d <= std::max(a.range, a.spell_reach))
            damage += a.burst;
    }
    return damage * e.mitigation;
}

float Bot::stun_value(const ObservedUnit &e, float left) const {
    const float window = stun_follow_up(e);
    return stun_prevented(e) + (left <= window ? kill_value() : window);
}

float Bot::kill_value() const {
    float value = 1;
    for (const auto &e : enemies)
        if (e.health > 0)
            value += e.health + e.shield;
    return value;
}

