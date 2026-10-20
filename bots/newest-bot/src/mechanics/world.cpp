// Observation, cached routes and the shared threat/power estimates.
#include "../bot.h"
#include <cstdio>
#include <cstring>
#include <iostream>

CampGroup::CampGroup(bc::map::camp_group g) : unit(std::move(g)), tiles(unit.spawn_tiles()) {
    camps.reserve(unit.camps().size());
    for (const auto &c : unit) {
        camps.emplace_back(c);
        auto &m = camps.back();
        centre += m.position;
        max_health += m.health;
        pressure += m.stats.base_strength;
    }
    if (!camps.empty())
        centre /= static_cast<int>(camps.size());
    health = max_health;
    max_pressure = pressure;
    float toughest = 1;
    for (const auto &a : camps) {
        float hits = 0;
        for (const auto &b : camps)
            if (distance(a.position, b.position) <= bc::constants::passive_abilities::varg_splash_radius)
                ++hits;
        shape_splash = std::max(shape_splash, hits);
        toughest = std::max(toughest, a.health);
    }
    splash = full_splash = std::min(shape_splash, max_health / toughest);
}

namespace {
void read_buffs(ObservedUnit &out) {
    auto buffs = out.unit().buffs();
    out.stunned = buffs.has_stun();
    out.stun_time = out.stunned ? buffs.get<bc::buff_stun>().duration : -1;
    if (buffs.has_shield())
        out.shield = buffs.get<bc::buff_shield>().value;
}
} // namespace

ObservedUnit::ObservedUnit(bc::champion c) : ref(c) {
    position = c.position();
    health = c.health();
    max_health = c.max_health();
    strength = c.strength();
    focus = c.focus();
    range = c.attack_range();
    gems = c.gem_count();
    type = c.champion_name();
    const auto design = c.base_stats();
    move_speed = std::max(1, int(design.base_move_speed));
    camp_multiplier = design.camp_damage_multiplier;
    immune = c.passive() == bc::passive_type::stun_immunity;
    const auto state = c.state();
    completed = state.contains(bc::champion_state::completed_turn);
    can_attack = state.contains(bc::champion_state::can_attack);
    can_cast = state.contains(bc::champion_state::can_cast);
    can_move = state.contains(bc::champion_state::can_move);
    movement_spent = range > 1 && !can_move && !can_attack;
    depositing = state.contains(bc::champion_state::submitting_gems);
    channel = depositing || state.contains(bc::champion_state::recalling) ||
              state.contains(bc::champion_state::teleporting);
    read_buffs(*this);
    damage_rate = strength;
    unsigned ability_slot = 0;
    for (const auto &a : c.abilities()) {
        const auto s = describe(a);
        abilities[ability_slot] = s;
        cooldowns[ability_slot] = a.cooldown();
        const bool ready = cooldowns[ability_slot++] <= 1;
        if (s.kind == bc::ability_kind::attack_reset) {
            damage_rate += strength / s.cooldown;
            if (ready)
                burst = std::max(burst, strength);
        } else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                   s.kind == bc::ability_kind::aoe_damage) {
            damage_rate += focus * s.modifier / s.cooldown;
            spell_reach = std::max(spell_reach, int(s.range + s.radius));
            if (ready)
                burst = std::max(burst, focus * s.modifier);
        } else if (s.kind == bc::ability_kind::shield && ready)
            ready_shield = focus * s.modifier;
        else if (s.kind == bc::ability_kind::heal)
            healing_rate = focus * s.modifier / s.cooldown;
    }
    auto items = c.item_count();
    const std::array durability{bc::item_type::durability_emblem_of_strength,
                                bc::item_type::durability_emblem_of_health,
                                bc::item_type::durability_emblem_of_focus};
    for (auto item : durability)
        if (items[item]) {
            mitigation = 1 - item.passive_magnitude();
            break;
        }
    for (unsigned i = 0; i < unsigned(bc::item_type::count); ++i) {
        const bc::item_type item(i);
        if (!items[item])
            continue;
        if (item.passive() == bc::item_passive::monster)
            camp_multiplier = design.camp_damage_multiplier * (1 + item.passive_magnitude());
        if (item.passive() == bc::item_passive::jungle)
            camp_mitigation = 1 - item.passive_magnitude();
    }
}

ObservedUnit::ObservedUnit(const CampMemory &c, int group_index)
    : ref(c.unit), position(c.position), group(group_index) {
    health = c.unit.health();
    max_health = c.unit.max_health();
    strength = c.unit.strength();
    focus = c.stats.base_focus;
    range = c.stats.base_attack_range;
    read_buffs(*this);
}

ObservedUnit::ObservedUnit(const Bot &bot)
    : ref(bc::self()), position(bot.position), health(bot.health), max_health(bot.max_health),
      strength(bot.strength), focus(bot.focus), mitigation(bot.mitigation), range(bot.range), gems(bot.gems),
      type(bot.type) {
    const auto state = bc::self().state();
    completed = state.contains(bc::champion_state::completed_turn);
    can_attack = state.contains(bc::champion_state::can_attack);
    can_cast = state.contains(bc::champion_state::can_cast);
    can_move = state.contains(bc::champion_state::can_move);
    movement_spent = bot.walked;
    move_speed = std::max(1, int(bot.stats.base_move_speed));
    camp_multiplier = bot.camp_multiplier;
    camp_mitigation = bot.camp_mitigation;
    sight = bot.visible;
    depositing = state.contains(bc::champion_state::submitting_gems);
    channel = state.contains(bc::champion_state::recalling) ||
              state.contains(bc::champion_state::submitting_gems) ||
              state.contains(bc::champion_state::teleporting);
    abilities = bot.spells;
    const auto &kit = bc::self().abilities();
    damage_rate = spell_dps(abilities, strength, focus);
    for (unsigned i = 0; i < abilities.size(); ++i) {
        cooldowns[i] = kit[i].cooldown();
        const auto &spell = abilities[i];
        spell_reach = std::max(spell_reach, int(spell.range + spell.radius));
        if (cooldowns[i] <= 1)
            burst = std::max(burst, spell.kind == bc::ability_kind::attack_reset ? strength
                                                                                 : focus * spell.modifier);
    }
    read_buffs(*this);
}

void Bot::initialise() {
    auto &me = bc::self();
    type = me.champion_name();
    passive = me.passive();
    stats = me.base_stats();
    auto a = me.abilities();
    spells = {describe(a[0]), describe(a[1])};
    if (bc::draft_memory.size() >= sizeof opening)
        std::memcpy(&opening, bc::draft_memory.data(), sizeof opening);
    roster = bc::champions(bc::team::ally);
    std::sort(roster.begin(), roster.end(), [](const auto &a, const auto &b) { return id(a) < id(b); });
    for (std::size_t s = 0; s < roster.size(); ++s) {
        // Draft order is public and persistent. A faster ally may already have
        // dashed into a bush before this champion's first turn, so even its
        // type must come from the draft rather than a live vision query.
        const bc::champion immutable(opening.roster_types[s]);
        const auto design = immutable.base_stats();
        const auto kit = immutable.abilities();
        const std::array described{describe(kit[0]), describe(kit[1])};
        float hp = design.base_health;
        for (const auto &spell : described)
            if (spell.kind == bc::ability_kind::shield)
                hp += design.base_focus * spell.modifier;
        roster_power.push_back(std::sqrt(hp * spell_dps(described, design.base_strength, design.base_focus)));
    }
    float ally_power_sum = 0;
    for (auto p : roster_power)
        ally_power_sum += p;
    enemy_average_power = roster_power.empty() ? 0.0f : ally_power_sum / float(roster_power.size());
    enemy_roster = bc::champions(bc::team::enemy);
    std::sort(enemy_roster.begin(), enemy_roster.end(),
              [](const auto &a, const auto &b) { return id(a) < id(b); });
    enemy_respawn.assign(enemy_roster.size(), -1);
    enemy_spawned.assign(enemy_roster.size(), 0);
    for (unsigned i = 0; i < roster.size(); ++i)
        if (roster[i] == me)
            slot = i;
    if (opening.magic == DraftPlan::tag && opening.body_count == roster.size())
        squad.members =
            squads::StellarTaviCassi::membership(std::span(opening.roster_types).first(roster.size()));
    static_blocked = terrain();
    altars = bc::map::altars().positions();
    for (auto p : altars)
        altar_goals |= around(p, bc::constants::submit_gems_range);
    altar_goals &= ~static_blocked;
    bc::bitboard visited;
    home_field.compute(static_blocked, bc::map::spawn_region(bc::team::ally), visited);
    altar_field.compute(static_blocked, altar_goals, visited);
    bc::distance_field enemy_field;
    auto neutral_terrain = bc::map::walls() | bc::map::holes() | bc::map::altars();
    enemy_field.compute(neutral_terrain, bc::map::spawn_region(bc::team::enemy), visited);
    auto layout = bc::map::camp_groups();
    groups.reserve(layout.size());
    for (auto &g : layout) {
        groups.emplace_back(std::move(g));
        auto &info = groups.back();
        info.home_distance = info.enemy_distance = info.altar_distance = bc::distance_field::unreachable;
        for (const auto &c : info.camps) {
            info.home_distance = std::min(info.home_distance, int(home_field.get(c.position)));
            info.enemy_distance = std::min(info.enemy_distance, int(enemy_field.get(c.position)));
            info.altar_distance = std::min(info.altar_distance, int(altar_field.get(c.position)));
        }
    }
    initialised = true;
    last_plan = bc::round_number();
}

void Bot::refresh_self() {
    const auto &me = bc::self();
    position = me.position();
    health = me.health();
    max_health = me.max_health();
    strength = me.strength();
    focus = me.focus();
    range = me.attack_range();
    gems = me.gem_count();
    if (!gems)
        squad.cargo_since[slot] = -1;
    else if (!squad.cargo_seen[slot])
        squad.cargo_since[slot] = round;
    squad.cargo_seen[slot] = gems;
}

void Bot::prepare_routes() {
    if (!altar_access.empty() && !altar_access.back().prepared) {
        if (bc::fuel_remaining() < policy::observation_reserve)
            return;
        auto &access = altar_access.back();
        if (access.prepare_stage == 0) {
            access.entry_candidates = access.entries.positions();
            access.prepare_stage = 1;
            return;
        }
        if (access.prepare_stage == 1) {
            while (access.entry_cursor < access.entry_candidates.size()) {
                if (bc::fuel_remaining() < policy::planning_reserve)
                    return;
                const auto tile = access.entry_candidates[access.entry_cursor++];
                const int path = home_field.get(tile);
                if (path >= access.entry_distance)
                    continue;
                const auto landings = around(tile, rules::crystal_range) & access.region & ~static_blocked;
                if (landings.count() >= unsigned(std::max(1, std::popcount(squad.members)))) {
                    access.entry_distance = path;
                    access.rendezvous = tile;
                    access.landings = landings.positions();
                }
            }
            access.prepare_stage = 2;
            return;
        }
        if (access.rendezvous.x >= 0) {
            access.entries &= around(access.rendezvous, 1);
            const auto altar = altars[altar_access.size() - 1];
            std::sort(access.landings.begin(), access.landings.end(), [&](vec2 a, vec2 b) {
                const int da = distance(a, altar), db = distance(b, altar);
                return da != db ? da < db : a.y != b.y ? a.y < b.y : a.x < b.x;
            });
        } else
            access.entries.clear();
        bc::bitboard visited;
        access.entry_routes.compute(static_blocked, access.entries, visited);
        access.entry_candidates.clear();
        access.prepared = true;
        return;
    }
    // Terrain routes are immutable. Build them across early turns using spare
    // fuel, so altar choice respects walls without rerunning every route.
    if (altar_routes.size() < altars.size() &&
        bc::fuel_remaining() > policy::planning_reserve + policy::action_reserve) {
        auto goals = around(altars[altar_routes.size()], bc::constants::submit_gems_range) & ~static_blocked;
        bc::bitboard visited;
        altar_routes.emplace_back().compute(static_blocked, goals, visited);
        AltarAccess access;
        access.region = visited;
        auto border = visited;
        for (int covered = 0; covered < rules::crystal_range;) {
            const int step = std::min(covered + 1, rules::crystal_range - covered);
            border |= border.shift_right(step) | border.shift_right(-step);
            covered += step;
        }
        for (int covered = 0; covered < rules::crystal_range;) {
            const int step = std::min(covered + 1, rules::crystal_range - covered);
            border |= border.shift_up(step) | border.shift_up(-step);
            covered += step;
        }
        access.entries = border & ~visited & ~static_blocked;
        altar_access.push_back(std::move(access));
    }
}

void Bot::observe() {
    refresh_self();
    visible = bc::vision::visible_tiles();
    blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
              bc::vision::teleport_tiles();
    blocked.set(position, false);
    enemies.clear();
    allies.clear();
    camps.clear();
    nearby_healing = 0;
    for (auto c : bc::vision::champions(bc::team::enemy))
        if (c.is_alive())
            enemies.emplace_back(c);
    for (auto c : bc::vision::champions(bc::team::ally))
        if (c != bc::self() && c.is_alive()) {
            allies.emplace_back(c);
            if (squad_member() && !enemies.empty())
                for (unsigned i = 0; i < roster.size(); ++i)
                    if (squad.contains(i) && roster[i] == c) {
                        allies.back().sight =
                            bc::champion(allies.back().type).visible_tiles_from(allies.back().position);
                        break;
                    }
        }
    // Track enemy death timers across turns so an engagement beside their
    // spawn can price in a fresh body arriving at full strength.
    for (std::size_t i = 0; i < enemy_roster.size() && i < enemy_respawn.size(); ++i) {
        bool alive = false;
        try {
            alive = enemy_roster[i].is_alive();
        } catch (...) {
            continue;
        }
        if (alive) {
            // Hidden enemies cannot have walked further than this from spawn.
            if (enemy_respawn[i] >= 0)
                enemy_spawned[i] = std::min(int(round), enemy_respawn[i]);
            enemy_respawn[i] = -1;
        } else if (enemy_respawn[i] < 0)
            enemy_respawn[i] = int(round) + bc::constants::respawn_time;
    }
    for (const auto &a : allies)
        if (distance(position, a.position) <= 6)
            nearby_healing += a.healing_rate;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (bc::fuel_remaining() < policy::planning_reserve)
            break;
        auto &g = groups[i];
        bool friendly_holder = g.tiles.is_set(position);
        for (const auto &ally : allies)
            friendly_holder |= g.tiles.is_set(ally.position);
        if (!friendly_holder)
            g.holding_since = -1;
        bool seen = false;
        for (const auto &c : g.camps)
            if (visible.is_set(c.position)) {
                seen = true;
                break;
            }
        if (!seen) {
            if (g.status == CampGroup::cooling && int(round) >= g.respawn) {
                g.status = CampGroup::alive;
                g.health = g.max_health;
                g.pressure = g.max_pressure;
                g.splash = g.full_splash;
                for (auto &c : g.camps)
                    c.health = c.stats.base_health;
            }
            continue;
        }
        g.seen = round;
        auto state = g.unit.state();
        if (auto *cooldown = std::get_if<bc::map::camp_group_respawning>(&state)) {
            g.status = CampGroup::cooling;
            g.respawn = round + std::max(1, cooldown->timer);
            g.health = 0;
            g.pressure = g.max_pressure;
            g.splash = g.full_splash;
            continue;
        }
        if (std::holds_alternative<bc::map::camp_group_reward_available>(state)) {
            g.status = CampGroup::capture;
            g.health = 0;
            bool contested = false;
            for (const auto &e : enemies)
                if (g.tiles.is_set(e.position))
                    contested = true;
            if (contested)
                g.holding_since = -1;
            else if (friendly_holder && g.holding_since < 0)
                g.holding_since = round;
            if (squad_member() && int(i) == squad.camp && g.holding_since >= 0 &&
                int(round) - g.holding_since <= bc::constants::capture_time)
                squad.progress = round;
            for (auto &c : g.camps)
                c.health = 0;
            continue;
        }
        if (g.status != CampGroup::alive)
            for (auto &c : g.camps)
                c.health = c.stats.base_health;
        g.status = CampGroup::alive;
        const float previous_health = g.health;
        g.health = 0;
        g.pressure = 0;
        float toughest = 1;
        for (auto &c : g.camps) {
            if (visible.is_set(c.position)) {
                if (c.unit.is_visible()) {
                    camps.emplace_back(c, i);
                    c.health = camps.back().health;
                } else
                    c.health = 0;
            }
            g.health += c.health;
            if (c.health > 0)
                g.pressure += c.stats.base_strength;
            toughest = std::max(toughest, c.health);
        }
        // Splash accelerates only the health shared by surviving targets.
        // Eight 500-HP clusters beside one 20,000-HP boss are not a 9x clear.
        g.splash = std::max(1.0f, std::min(g.shape_splash, g.health / toughest));
        // Only our damage is clear progress. An enemy wearing the camp down
        // gives us no capture control, so it must not keep the job alive.
        if (squad_member() && int(i) == squad.camp && g.health < previous_health &&
            std::none_of(
                enemies.begin(), enemies.end(),
                [&](const auto &enemy) {
                    return std::any_of(g.camps.begin(), g.camps.end(), [&](const auto &camp) {
                        return distance(enemy.position, camp.position) <=
                               std::max(enemy.range, enemy.spell_reach);
                    });
                }))
            squad.progress = round;
    }
    if (squad_member()) {
        // Update only groups whose occupancy changed. Rebuilding every camp's
        // map-sized mask each turn can consume the whole movement allowance.
        bool changed = false;
        const auto previous_obstacles = known_camp_obstacles;
        for (auto &group : groups) {
            if (group.obstacle_status == int(group.status) && group.obstacle_health == group.health)
                continue;
            group.obstacle_status = int(group.status);
            group.obstacle_health = group.health;
            known_camp_obstacles &= ~group.tiles;
            if (group.status == CampGroup::alive) {
                if (group.health >= group.max_health)
                    known_camp_obstacles |= group.tiles;
                else
                    for (const auto &camp : group.camps)
                        if (camp.health > 0)
                            known_camp_obstacles.set(camp.position);
            }
            changed = true;
        }
        if (changed && !(previous_obstacles ^ known_camp_obstacles).is_empty())
            ++terrain_revision;
    }
    observe_cargo();
    shared.read();
}

void Bot::publish(unsigned claim) { shared.set(slot, claim); }

float Bot::dps(const CampGroup &g) const {
    float area = passive == bc::passive_type::splash_attack ? g.splash : 1;
    auto adjusted = spells;
    // Direct and area spell DPS share the one-cast limit; cooldowns are long
    // enough that the two slots can alternate without sustained contention.
    float value = spell_dps(adjusted, strength, focus, area);
    if (passive != bc::passive_type::splash_attack) {
        for (const auto &s : spells)
            if (s.kind == bc::ability_kind::aoe_damage)
                value += focus * s.modifier * (g.splash - 1) / s.cooldown;
    }
    if (passive == bc::passive_type::retaliation)
        value += focus * bc::constants::passive_abilities::ren_retaliation_focus_ratio;
    return std::max(1.0f, value * camp_multiplier);
}

float Bot::clear_loss(const CampGroup &g, float clear_time) const {
    float loss = g.pressure * clear_time * camp_mitigation;
    if (g.camps.size() > 1)
        loss *= 0.68f; // Sequential kills remove attackers during a clear.
    for (const auto &s : spells) {
        if (s.kind == bc::ability_kind::shield)
            loss -= focus * s.modifier * (1 + clear_time / s.cooldown);
        if (s.kind == bc::ability_kind::cc)
            loss -= g.pressure * clear_time / s.cooldown;
    }
    loss -= nearby_healing * clear_time;
    return std::max(0.0f, loss);
}

float Bot::incoming(vec2 tile, int engaged_group, bool include_camps) const {
    float damage = 0;
    for (const auto &e : enemies) {
        if (e.stunned)
            continue;
        int d = distance(tile, e.position);
        if (d <= e.range)
            damage += e.strength;
        else if (e.range <= 1 && d <= e.range + 1)
            damage += e.strength * 0.85f;
        if (d <= e.spell_reach)
            damage += e.burst * 0.5f;
    }
    if (include_camps)
        for (const auto &c : camps) {
            if (c.strength <= 0 || c.stunned || distance(tile, c.position) > c.range)
                continue;
            const auto &g = groups[c.group];
            if (c.group != engaged_group && g.attacked < int(round) - bc::constants::camp_sleep_time - 1 &&
                g.health >= g.max_health)
                continue;
            // Camps attack the nearest champion, including our enemy. Avoid taking
            // aggro from the current tank; ties remain conservatively dangerous.
            int d = distance(tile, c.position);
            bool tanked = false;
            for (const auto &a : allies)
                if (distance(a.position, c.position) < d) {
                    tanked = true;
                    break;
                }
            if (!tanked)
                for (const auto &e : enemies)
                    if (distance(e.position, c.position) < d) {
                        tanked = true;
                        break;
                    }
            if (!tanked)
                damage += c.strength * camp_mitigation;
        }
    return damage * mitigation;
}

float Bot::friendly_power(vec2 at) const {
    float ready_shield = 0, burst = 0;
    for (unsigned i = 0; i < spells.size(); ++i)
        if (bc::self().abilities()[i].is_ready()) {
            const auto &s = spells[i];
            if (s.kind == bc::ability_kind::shield)
                ready_shield = focus * s.modifier;
            else if (s.kind == bc::ability_kind::attack_reset)
                burst = std::max(burst, strength);
            else if (s.modifier > 0 && s.kind != bc::ability_kind::heal)
                burst = std::max(burst, focus * s.modifier);
        }
    float reflected = passive == bc::passive_type::retaliation
                          ? focus * bc::constants::passive_abilities::ren_retaliation_focus_ratio
                          : 0;
    float p = std::sqrt(std::max(1.0f, health + ready_shield) *
                        (spell_dps(spells, strength, focus) + reflected + burst * 0.2f) / mitigation);
    for (const auto &a : allies) {
        int arrival = std::max(0, distance(at, a.position) - std::max(a.range, a.spell_reach));
        if (arrival > 6 || a.channel || a.stunned)
            continue;
        p += std::sqrt(std::max(1.0f, a.health + a.shield + a.ready_shield) *
                       (a.damage_rate + a.retaliation() + a.burst * 0.2f + a.healing_rate) / a.mitigation) *
             0.9f / (1 + arrival * 0.4f);
    }
    return p;
}

float Bot::enemy_power(vec2 at) const {
    float p = 0;
    for (const auto &e : enemies) {
        int arrival = std::max(0, distance(at, e.position) - std::max(e.range, e.spell_reach));
        if (arrival > 6)
            continue;
        // Focus fire also multiplies a retaliator's damage: each attacker pays
        // for its own hit, including the hit that kills the target.
        float reflected = e.retaliation();
        if (reflected > 0)
            for (const auto &a : allies)
                if (!a.stunned && !a.channel &&
                    distance(a.position, e.position) <= std::max(a.range, a.spell_reach) + 1)
                    reflected += e.retaliation();
        p += std::sqrt(std::max(1.0f, e.health + e.shield + e.ready_shield) *
                       (e.damage_rate + reflected + e.burst * 0.2f + e.healing_rate) / e.mitigation) /
             (1 + arrival * 0.3f);
    }
    return p;
}

float Bot::respawn_threat(vec2 at) const {
    if (enemy_respawn.empty() || enemy_average_power <= 0)
        return 0;
    const auto spawn = bc::map::spawn_region(bc::team::enemy);
    int spawn_dist = 100000;
    for (auto p : spawn.positions())
        spawn_dist = std::min(spawn_dist, distance(at, p));
    if (spawn_dist > 9)
        return 0; // Too far from their base for a fresh body to reach the fight.
    float bodies = 0;
    for (std::size_t i = 0; i < enemy_respawn.size(); ++i)
        if (enemy_respawn[i] >= 0 && enemy_respawn[i] - int(round) <= 5)
            bodies += 1;
    if (bodies <= 0)
        return 0;
    // A respawn is a full-health body arriving from their spawn; discount it by
    // how far away the fight is so a gank beside the base is priced correctly.
    return bodies * enemy_average_power * (1.0f - float(spawn_dist) / 10.0f);
}
