// Observation, cached routes and the shared threat/power estimates.
#include "../bot.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <wasm_simd128.h>

CampGroup::CampGroup(bc::map::camp_group g, std::span<CampMemory> members)
    : unit(std::move(g)), camps(members) {
    for (const auto &m : camps) {
        centre += m.position;
        max_health += m.health;
        pressure += m.stats.base_strength;
    }
    if (!camps.empty())
        centre /= static_cast<int>(camps.size());
    health = max_health;
    max_pressure = pressure;
}

float CampGroup::count_splash() const {
    // Every pair (the draft usually counts these for us).
    float shape = 1;
    for (const auto &a : camps) {
        int hits = 0;
        for (const auto &b : camps)
            hits += distance(a.position, b.position) <= bc::constants::passive_abilities::varg_splash_radius;
        shape = std::max(shape, float(hits));
    }
    return shape;
}

void CampGroup::set_splash(float shape) {
    float toughest = 1;
    for (const auto &c : camps)
        toughest = std::max(toughest, c.health);
    shape_splash = shape;
    splash = full_splash = std::min(shape_splash, max_health / toughest);
}

namespace {
// Padding lanes: a reach no distance meets, and a tile far from every other.
constexpr std::int16_t no_reach = std::numeric_limits<std::int16_t>::min();
constexpr std::int32_t far_tile = 1 << 29;

// What a champion type always has, read once per type rather than on every
// sighting: these engine queries cost about 550 fuel per champion per turn.
struct Kit {
    bool known = false, immune = false;
    int move_speed = 1;
    std::array<Spell, 2> spells;
};
const Kit &kit_of(const bc::champion &c, bc::champion_type type) {
    static std::array<Kit, 32> kits;
    auto &kit = kits[static_cast<unsigned>(type) % kits.size()];
    if (!kit.known) {
        const auto design = c.base_stats();
        kit.move_speed = std::max(1, int(design.base_move_speed));
        kit.immune = c.passive() == bc::passive_type::stun_immunity;
        unsigned slot = 0;
        for (const auto &a : c.abilities())
            if (slot < kit.spells.size())
                kit.spells[slot++] = describe(a);
        kit.known = true;
    }
    return kit;
}

// unit::buffs() decodes every record into a map of variants; this reads the
// same records and keeps the stun and shield (the last of each kind).
struct BuffState {
    bool stunned = false, shielded = false;
    int stun_time = 0;
    float shield = 0;
};
BuffState read_buff_state(const std::variant<const bc::champion *, const bc::camp *> &ref) {
    constexpr auto kinds = static_cast<std::size_t>(bc::buff_types::count);
    BuffState state;
    std::uint32_t count = 0;
    auto record = [&](std::uint32_t type, std::int32_t duration, float value) {
        switch (static_cast<bc::buff_types>(type)) {
            case bc::buff_types::shield:
                state.shielded = true;
                state.shield = value;
                break;
            case bc::buff_types::stun:
                state.stunned = true;
                state.stun_time = duration;
                break;
            case bc::buff_types::enhanced_attack:
                break;
            default:
                throw std::runtime_error("unknown engine buff kind");
        }
    };
    // The engine writes the first `count` records, the only ones read.
    if (const auto *const *camp = std::get_if<const bc::camp *>(&ref)) {
        std::array<bc::detail::camp_buff_data, kinds> raw;
        bc::detail::check(
            bc::detail::sdk_camp_buffs(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(**camp)),
                                       raw.data(), raw.size(), &count));
        if (count > raw.size())
            throw std::runtime_error("invalid engine buff count");
        for (std::uint32_t i = 0; i < count; ++i)
            record(raw[i].type, raw[i].timer, raw[i].magnitude);
    } else {
        std::array<bc::detail::buff_data, kinds> raw;
        const auto &champion = *std::get<const bc::champion *>(ref);
        bc::detail::check(bc::detail::sdk_champion_buff_data(
            static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(champion)), raw.data(), raw.size(),
            &count));
        if (count > raw.size())
            throw std::runtime_error("invalid engine buff count");
        for (std::uint32_t i = 0; i < count; ++i) {
            const bool shield_record = raw[i].type == static_cast<std::uint32_t>(bc::buff_types::shield);
            record(raw[i].type, shield_record ? raw[i].data.shield.duration : raw[i].data.stun.duration,
                   shield_record ? raw[i].data.shield.value : 0.0f);
        }
    }
    return state;
}

// Item designs never change: each type's passive is read once.
struct ItemEffect {
    bool known = false;
    std::optional<bc::item_passive> passive;
    float magnitude = 0;
};
const ItemEffect &item_effect(bc::item_type item) {
    static std::array<ItemEffect, static_cast<unsigned>(bc::item_type::count)> effects;
    auto &effect = effects[static_cast<unsigned>(item)];
    if (!effect.known) {
        const auto info = item.info();
        effect.passive = info.passive;
        effect.magnitude = info.passive_magnitude;
        effect.known = true;
    }
    return effect;
}
} // namespace

void read_buffs(ObservedUnit &out) {
    const auto state = read_buff_state(out.ref);
    out.stunned = state.stunned;
    out.stun_time = state.stunned ? state.stun_time : -1;
    if (state.shielded)
        out.shield = state.shield;
}

float live_shield(const ObservedUnit &u) {
    const auto state = read_buff_state(u.ref);
    return state.shielded ? state.shield : 0;
}

bc::detail::champion_info read_champion(const bc::champion &c) {
    bc::detail::champion_info info{};
    bc::detail::check(
        bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)), &info));
    return info;
}

std::optional<bc::detail::champion_info> sighting(const bc::champion &c) {
    // champion::is_visible(), keeping the fields that query returns.
    bc::detail::champion_info info{};
    const auto code =
        bc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)), &info);
    if (code == static_cast<std::int32_t>(bc::error_code::api_not_in_vision))
        return std::nullopt;
    bc::detail::check(code);
    if (info.x < 0 || info.y < 0)
        return std::nullopt;
    return info;
}

bool read_camp(const bc::camp &c, bc::detail::camp_state &info) {
    // camp::is_visible(), keeping the fields that query returns.
    const auto code =
        bc::detail::sdk_camp(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(c)), &info);
    if (code == static_cast<std::int32_t>(bc::error_code::api_not_in_vision) ||
        code == static_cast<std::int32_t>(bc::error_code::api_not_found))
        return false;
    bc::detail::check(code);
    return true;
}

int group_state(const CampGroup &g, int &timer) {
    // camp_group::state() without its vision check, which first rebuilds the
    // whole vision mask: callers only ask about groups with a camp in view.
    const auto handle = static_cast<std::uint32_t>(static_cast<bc::handle_camp_group_t>(g.unit));
    std::uint32_t state = 0;
    bc::detail::check(bc::detail::sdk_camp_group_state(handle, &state));
    if (state == 3) {
        std::int32_t cooldown = 0;
        bc::detail::check(bc::detail::sdk_camp_group_cooldown(handle, &cooldown));
        timer = cooldown;
    } else if (state == 0)
        throw bc::error{bc::error_code::api_not_in_vision, "camp group state is unknown"};
    else if (state > 3)
        throw std::runtime_error("unknown engine camp group state");
    return int(state);
}

ObservedUnit::ObservedUnit(const bc::champion &c) : ObservedUnit(c, read_champion(c)) {}

ObservedUnit::ObservedUnit(const bc::champion &c, const bc::detail::champion_info &info) : ref(&c) {
    // Raw engine reads: the SDK accessors wrap each in checks and copies.
    const auto handle = static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c));
    position = {info.x, info.y};
    health = info.health;
    max_health = info.max_health;
    strength = info.strength;
    focus = info.focus;
    range = info.attack_range;
    std::uint32_t carried = 0;
    bc::detail::check(bc::detail::sdk_champion_gems(handle, &carried));
    gems = carried;
    type = static_cast<bc::champion_type>(info.type);
    const auto &kit = kit_of(c, type);
    move_speed = kit.move_speed;
    immune = kit.immune;
    const auto state = bc::champion_state::from_bits_retain(info.state);
    completed = state.contains(bc::champion_state::completed_turn);
    channel = state.contains(bc::champion_state::submitting_gems) ||
              state.contains(bc::champion_state::recalling) ||
              state.contains(bc::champion_state::teleporting);
    read_buffs(*this);
    damage_rate = strength;
    for (unsigned ability_slot = 0; ability_slot < kit.spells.size(); ++ability_slot) {
        const auto &s = kit.spells[ability_slot];
        std::int32_t cooldown = 0;
        bc::detail::check(bc::detail::sdk_champion_cooldown(handle, ability_slot, &cooldown));
        // Castable at its next action: a slot at 1 is not (measured).
        const bool ready = cooldown <= 0;
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
        } else if (s.kind == bc::ability_kind::dash && s.modifier > 0) {
            // A damaging dash (Heart of Ruin) lands its area its steps plus
            // its radius away.
            damage_rate += focus * s.modifier / s.cooldown;
            spell_reach = std::max(spell_reach, int(s.range + s.radius));
            if (ready)
                burst = std::max(burst, focus * s.modifier);
        } else if (s.kind == bc::ability_kind::shield && ready)
            ready_shield = focus * s.modifier;
        else if (s.kind == bc::ability_kind::heal)
            healing_rate = focus * s.modifier / s.cooldown;
        if (s.kind == bc::ability_kind::dash) {
            dash_range = std::int8_t(s.range);
            dash_wait = std::int8_t(std::clamp(cooldown, 0, 100));
            dash_cooldown = std::int8_t(std::clamp(s.cooldown, 1, 100));
            dash_walks = !s.exclusive;
        }
    }
    if (type == bc::champion_type::heart_of_ruin) {
        // Its passive adds up to half again to all its damage with missing
        // health; the readings leave it out.
        const float bonus =
            1 + bc::constants::passive_abilities::heart_of_ruin_missing_health_damage_max_bonus *
                    (1 - health / std::max(1.0f, max_health));
        strength *= bonus;
        damage_rate *= bonus;
        burst *= bonus;
    }
    // The engine writes every entry, so the array is not zero-filled first.
    std::array<std::uint32_t, static_cast<std::size_t>(bc::item_type::count)> items;
    bc::detail::check(bc::detail::sdk_champion_items(handle, items.data()));
    const std::array durability{bc::item_type::durability_emblem_of_strength,
                                bc::item_type::durability_emblem_of_health,
                                bc::item_type::durability_emblem_of_focus};
    for (auto item : durability)
        if (items[static_cast<unsigned>(item)]) {
            mitigation = 1 - item_effect(item).magnitude;
            break;
        }
}

ObservedUnit::ObservedUnit(const CampMemory &c, int group_index, const bc::detail::camp_state &info,
                           bool asleep)
    : ref(&c.unit), position(c.position), group(group_index) {
    focus = c.stats.base_focus;
    range = c.stats.base_attack_range;
    reread_camp(info, asleep);
}

void ObservedUnit::recamp(const CampMemory &c, int group_index, const bc::detail::camp_state &info,
                          bool asleep) {
    ref = &c.unit;
    position = c.position;
    group = group_index;
    focus = c.stats.base_focus;
    range = c.stats.base_attack_range;
    reread_camp(info, asleep);
}

void ObservedUnit::reread_camp(const bc::detail::camp_state &info, bool asleep) {
    // What differs between two records of one camp (only health is ever
    // changed after one is made): the reading, and the buffs, read afresh.
    health = info.health;
    max_health = info.max_health;
    strength = info.strength;
    shield = 0;
    stunned = false;
    stun_time = -1;
    if (!asleep)
        read_buffs(*this);
}

void Bot::initialise() {
    auto &me = bc::self();
    type = me.champion_name();
    passive = me.passive();
    stats = me.base_stats();
    auto a = me.abilities();
    spells = {describe(a[0]), describe(a[1])};
    for (const auto &s : spells)
        if (s.range > 3 && (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                            s.kind == bc::ability_kind::aoe_damage))
            siege_reach = std::max(siege_reach, s.range);
    if (bc::draft_memory.size() >= sizeof opening)
        std::memcpy(&opening, bc::draft_memory.data(), sizeof opening);
    roster = bc::champions(bc::team::ally);
    std::sort(roster.begin(), roster.end(), [](const auto &a, const auto &b) { return id(a) < id(b); });
    for (std::size_t s = 0; s < roster.size(); ++s) {
        // Draft order is public and persistent. A faster ally may already have
        // dashed into a bush before this champion's first turn, so even its
        // type must come from the draft rather than a live vision query. The
        // SDK already holds every design; building another reloads it.
        const auto index = static_cast<std::size_t>(opening.roster_types[s]);
        std::optional<bc::champion> loaded;
        const auto &immutable = index < bc::champion_designs.size() ? bc::champion_designs[index]
                                                                    : loaded.emplace(opening.roster_types[s]);
        const auto design = immutable.base_stats();
        // The type's kit now, from its design, rather than on first sight:
        // its spells, described once.
        const auto &described = kit_of(immutable, opening.roster_types[s]).spells;
        float hp = design.base_health;
        for (const auto &spell : described)
            if (spell.kind == bc::ability_kind::shield)
                hp += design.base_focus * spell.modifier;
        roster_power.push_back(std::sqrt(hp * spell_dps(described, design.base_strength, design.base_focus)));
        roster_kits.push_back({described, design.camp_damage_multiplier,
                               camp_dps(described, design.base_strength, design.base_focus, 1) *
                                   design.camp_damage_multiplier});
    }
    float ally_power_sum = 0;
    for (auto p : roster_power)
        ally_power_sum += p;
    enemy_average_power = roster_power.empty() ? 0.0f : ally_power_sum / float(roster_power.size());
    enemy_roster = bc::champions(bc::team::enemy);
    std::sort(enemy_roster.begin(), enemy_roster.end(),
              [](const auto &a, const auto &b) { return id(a) < id(b); });
    enemy_respawn.assign(enemy_roster.size(), -1);
    for (unsigned i = 0; i < roster.size(); ++i)
        if (roster[i] == me)
            slot = i;
    static_blocked = terrain();
    open_terrain = ~static_blocked;
    map_width = bc::map::width();
    map_height = bc::map::height();
    const auto tiles = std::size_t(map_width) * std::size_t(map_height);
    altars = bc::map::altars().positions();
    altar_goals = altar_goals_of(altars, static_blocked);
    const auto search_start = turn_work();
    home_field.compute(static_blocked, bc::map::spawn_region(bc::team::ally), search_visited);
    search_job_cost = turn_work() - search_start;
    const auto neutral_terrain = bc::map::walls() | bc::map::holes() | bc::map::altars();
    no_centre_tile.load(neutral_terrain);
    // The camp layout, as map::camp_groups() lists it, read with one engine
    // call into storage never filled first, each group made in place (the
    // listing fills and grows its buffers and moves each group again).
    {
        std::uint32_t count = 0, capacity = 4096;
        std::unique_ptr<std::uint32_t[]> words(new std::uint32_t[capacity]);
        bc::detail::check(bc::detail::sdk_map_camp_layout(words.get(), capacity, &count));
        if (count > capacity) {
            capacity = count;
            words.reset(new std::uint32_t[capacity]);
            bc::detail::check(bc::detail::sdk_map_camp_layout(words.get(), capacity, &count));
        }
        std::size_t group_count = 0, camp_total = 0;
        for (std::size_t i = 0; i + 5 <= count; ++group_count) {
            const auto size = words[i + 4];
            i += 5;
            for (std::uint32_t m = 0; m < size && i + 4 <= count; ++m, ++camp_total)
                i += 4;
        }
        groups.reserve(group_count);
        camp_pool.reserve(camp_total);
        for (std::size_t i = 0; i + 5 <= count;) {
            const auto handle = static_cast<bc::handle_camp_group_t>(words[i]);
            const auto gold = words[i + 1], gems = words[i + 2];
            const auto respawn = static_cast<std::int32_t>(words[i + 3]);
            const auto size = words[i + 4];
            i += 5;
            const std::size_t first = camp_pool.size();
            for (std::uint32_t m = 0; m < size && i + 4 <= count; ++m, i += 4)
                camp_pool.emplace_back(bc::detail::handle_access::make_camp(
                    static_cast<bc::handle_camp_t>(words[i]), handle,
                    static_cast<bc::camp_type>(words[i + 1]),
                    vec2{static_cast<std::int32_t>(words[i + 2]), static_cast<std::int32_t>(words[i + 3])}));
            groups.emplace_back(bc::detail::handle_access::make_camp_group(handle, gold, gems, respawn, {}),
                                std::span(camp_pool.data() + first, camp_pool.size() - first));
        }
    }
    camps.reset(groups);
    group_cx.resize(groups.size());
    group_cy.resize(groups.size());
    for (std::size_t i = 0; i < groups.size(); ++i) {
        group_cx[i] = groups[i].centre.x;
        group_cy[i] = groups[i].centre.y;
    }
    {
        // Every entry but the stamps is written before it is read.
        const std::size_t room = camps.slots_total() + 8;
        camp_threats.reset(new int[room]);
        threat_nearest.reset(new int[room]);
        threat_ties.reset(new int[room]);
        threat_stamp.assign(room, 0);
        for (auto *lanes : {&threat_x, &threat_y, &threat_reach})
            lanes->reset(new std::int16_t[room]);
    }
    // Each group's walking distance to our spawn, the enemy spawn and the
    // altars, which the draft measures with spare fuel. Without its numbers,
    // the fields are built here (as the draft would) and exported once rather
    // than read per tile; exports write every entry, so the buffers are never
    // filled first (memory fills cost about a fuel per byte).
    const bool measured = opening.magic == DraftPlan::tag && opening.group_count == groups.size();
    for (std::size_t i = 0; i < groups.size(); ++i)
        groups[i].set_splash(measured ? float(opening.group_splash[i]) : groups[i].count_splash());
    if (measured)
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const auto &distance = opening.group_distance[i];
            groups[i].home_distance = distance[DraftPlan::home];
            groups[i].enemy_distance = distance[DraftPlan::enemy];
            groups[i].altar_distance = distance[DraftPlan::altar];
        }
    if (!measured) {
        bc::distance_field enemy_field;
        enemy_field.compute(neutral_terrain, bc::map::spawn_region(bc::team::enemy), search_visited);
        const std::unique_ptr<std::uint16_t[]> home(new std::uint16_t[tiles]),
            enemy(new std::uint16_t[tiles]), altar(new std::uint16_t[tiles]);
        home_field.export_linear_buffer(std::span(home.get(), tiles));
        enemy_field.export_linear_buffer(std::span(enemy.get(), tiles));
        altar_walk().export_linear_buffer(std::span(altar.get(), tiles));
        for (auto &g : groups) {
            g.home_distance = g.enemy_distance = g.altar_distance = bc::distance_field::unreachable;
            for (const auto &c : g.camps) {
                const int index = tile_index(c.position);
                g.home_distance = std::min(g.home_distance, int(home[index]));
                g.enemy_distance = std::min(g.enemy_distance, int(enemy[index]));
                g.altar_distance = std::min(g.altar_distance, int(altar[index]));
            }
        }
    }
    // Camp tiles and their groups, each camp's tile index, and the camps of
    // spawn-reachable groups. The masks are written here and imported once
    // each: setting a tile costs an engine call. tile_group is never filled:
    // it is read only where camp_tile is set.
    tile_group.reset(new std::uint16_t[tiles]);
    {
        const std::size_t bytes = bc::bitboard::linear_buffer_size();
        std::vector<std::uint8_t> camp_bits(bytes, 0), unwalked_bits(bytes, 0);
        for (std::size_t i = 0; i < groups.size(); ++i)
            for (auto &c : groups[i].camps) {
                c.tile = tile_index(c.position);
                const auto bit = std::uint8_t(1u << (c.tile & 7));
                camp_bits[std::size_t(c.tile) >> 3] |= bit;
                tile_group[c.tile] = static_cast<std::uint16_t>(i);
                if (groups[i].home_distance != bc::distance_field::unreachable)
                    unwalked_bits[std::size_t(c.tile) >> 3] |= bit;
            }
        camp_tiles.import_linear_buffer(camp_bits);
        unwalked_camps.import_linear_buffer(unwalked_bits);
    }
    measure_farthest(home_field, &CampGroup::home_farthest);
    // Groups' own walk fields are built in the background: first those out of
    // reach of spawn (their searches cover only their island), then the
    // farthest, so a champion's search for the groups still without one stays
    // short (see measure_paths()).
    walk_order.resize(groups.size());
    walk_of.assign(groups.size(), nullptr);
    for (std::size_t i = 0; i < groups.size(); ++i)
        walk_order[i] = int(i);
    std::stable_sort(walk_order.begin(), walk_order.end(),
                     [&](int a, int b) { return groups[a].home_distance > groups[b].home_distance; });
    for (const auto &g : groups)
        camp_count += g.camps.size();
    camp_tile.load(camp_tiles);
    bush_tile.load(bc::map::bushes());
    enemy_spawn_tile.load(bc::map::spawn_region(bc::team::enemy));
    dash_terrain = bc::map::walls() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy);
    enemy_spawn_tiles = bc::map::spawn_region(bc::team::enemy).positions();
    initialised = true;
    last_plan = bc::round_number();
}

void Bot::occupied_into(bc::bitboard &out, const bc::bitboard &terrain) {
    // Built in place: a new mask costs about as much as six in-place unions.
    bc::vision::champion_tiles_into(bc::team::ally, out);
    bc::vision::champion_tiles_into(bc::team::enemy, occupancy);
    out |= occupancy;
    bc::vision::camp_tiles_into(occupancy);
    out |= occupancy;
    bc::vision::teleport_tiles_into(occupancy);
    out |= occupancy;
    out |= terrain;
    out.set(position, false);
}

void Bot::subtract_into(bc::bitboard &out, const bc::bitboard &a, const bc::bitboard &b) {
    // out must be a different mask from a and b.
    out.clear();
    out |= a;
    out &= b;
    out ^= a;
}

void Bot::subtract(bc::bitboard &a, const bc::bitboard &b) {
    occupancy.clear();
    occupancy |= a;
    occupancy &= b;
    a ^= occupancy;
}

float live_health(const ObservedUnit &u) {
    // is_visible() then health(), from one engine read.
    if (u.enemy()) {
        const auto info = sighting(u.champion());
        return info ? info->health : 0;
    }
    bc::detail::camp_state info{};
    return read_camp(u.camp(), info) ? info.health : 0;
}

const bc::bitboard &Bot::origin_at(vec2 p) {
    search_origin.clear();
    search_origin.set(p);
    return search_origin;
}

void Bot::refresh_self() {
    const auto &me = bc::self();
    const auto info = read_champion(me);
    if (info.x < 0 || info.y < 0)
        throw ChampionGone{};
    position = {info.x, info.y};
    health = info.health;
    max_health = info.max_health;
    strength = info.strength;
    focus = info.focus;
    range = info.attack_range;
    gems = me.gem_count();
}

const bc::distance_field &Bot::altar_walk() {
    // Background work builds it early; a need before then builds it at once.
    if (!altar_field_ready) {
        altar_field.compute(static_blocked, altar_goals, search_visited);
        altar_field_ready = true;
        measure_farthest(altar_field, &CampGroup::altar_farthest);
    }
    return altar_field;
}

void Bot::measure_farthest(const bc::distance_field &field, int CampGroup::*farthest) {
    // Many camps from one export (one costs about a read per 150 tiles); a
    // few with one read each.
    const auto tiles = std::size_t(map_width) * std::size_t(map_height);
    std::size_t camps_on_map = 0;
    for (const auto &g : groups)
        camps_on_map += g.camps.size();
    const bool exported = camps_on_map * 150 > tiles;
    if (exported) {
        if (!path_buffer)
            path_buffer.reset(new std::uint16_t[tiles]);
        field.export_linear_buffer(std::span(path_buffer.get(), tiles));
    }
    for (auto &g : groups) {
        int walk = 0;
        for (const auto &c : g.camps)
            walk = std::max(walk, int(exported ? path_buffer[c.tile] : field.get(c.position)));
        g.*farthest = walk;
    }
}

void Bot::measure_paths(int level) {
    // Walking distance from here to each group. Terrain never changes, so
    // this is reused until we move; dynamic occupancy is routed locally.
    const int home_here = home_field.get(position);
    const bool connected = home_here != bc::distance_field::unreachable;
    if (origin_position != position) {
        origin_position = position;
        path_search = 0;
        group_path.assign(groups.size(), bc::distance_field::unreachable);
        group_bounded.assign(groups.size(), 1);
        // A group with its own field: one read. A group out of reach of
        // spawn is out of reach of anywhere spawn reaches.
        const int here = tile_index(position);
        for (std::size_t i = 0; i < groups.size(); ++i)
            if (const std::uint16_t *walks = walk_of[i]) {
                group_path[i] = walks[here];
                group_bounded[i] = 0;
            } else if (connected && groups[i].home_distance == bc::distance_field::unreachable)
                group_bounded[i] = 0;
    }
    // The rest: one search from here, which stops once it has reached all
    // their camps (fields are built farthest from spawn first, so these are
    // near) or, first, at path_bound tiles. A group it misses is at least
    // that far, as far as its nearest camp in a straight line, and as far as
    // its walk from spawn exceeds ours (the triangle inequality).
    if (path_search >= level)
        return;
    path_search = level;
    const bool complete = level == 3;
    unsigned pending = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (group_bounded[i] && walk_of[i]) {
            // Built since this origin was measured, so not a destination now.
            group_path[i] = walk_of[i][tile_index(position)];
            group_bounded[i] = 0;
        } else
            pending += group_bounded[i] ? unsigned(groups[i].camps.size()) : 0;
    if (!pending)
        return;
    // Spawn-reachable camps of groups without a field are kept as a mask;
    // from anywhere else every pending camp is a destination.
    const bc::bitboard *destinations = &unwalked_camps;
    if (!connected) {
        search_goals.clear();
        for (std::size_t i = 0; i < groups.size(); ++i)
            if (group_bounded[i])
                for (const auto &c : groups[i].camps)
                    search_goals.set(c.position);
        destinations = &search_goals;
    }
    const std::uint16_t limit = complete     ? bc::distance_field::unreachable
                                : level == 2 ? std::uint16_t(2 * policy::path_bound)
                                             : policy::path_bound;
    origin_field.compute_to(static_blocked, origin_at(position), *destinations, search_visited, limit);
    // Read many camps from one export (one costs about a read per 150
    // tiles); a few with one read each.
    const auto tiles = std::size_t(map_width) * std::size_t(map_height);
    const bool exported = pending * 150 > tiles;
    const int altar_here =
        altar_field_ready ? int(altar_field.get(position)) : bc::distance_field::unreachable;
    if (exported) {
        if (!path_buffer)
            path_buffer.reset(new std::uint16_t[tiles]);
        origin_field.export_linear_buffer(std::span(path_buffer.get(), tiles));
    } else
        path_reached.load(search_visited); // A camp the search did not reach is unreachable: not read.
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (!group_bounded[i])
            continue;
        int path = bc::distance_field::unreachable;
        for (const auto &c : groups[i].camps)
            path = std::min(path, exported               ? int(path_buffer[c.tile])
                                  : path_reached[c.tile] ? int(origin_field.get(c.position))
                                                         : int(bc::distance_field::unreachable));
        if (complete || path != bc::distance_field::unreachable) {
            group_path[i] = path;
            group_bounded[i] = 0;
        } else {
            int straight = std::numeric_limits<int>::max();
            for (const auto &c : groups[i].camps)
                straight = std::min(straight, distance(position, c.position));
            // A landmark field L bounds the walk from both sides: the group is
            // at least L(nearest camp) - L(here) and L(here) - L(farthest
            // camp) away.
            const auto &g = groups[i];
            int least = std::max(int(limit) + 1, straight);
            if (connected)
                least = std::max({least, g.home_distance - home_here, home_here - g.home_farthest});
            if (altar_here != bc::distance_field::unreachable)
                least = std::max({least, g.altar_distance - altar_here, altar_here - g.altar_farthest});
            group_path[i] = least;
        }
    }
}

bool Bot::background_search() {
    // One map-wide search per call, in a fixed order; false when none is left.
    if (!altar_field_ready) {
        altar_walk();
        return true;
    }
    // Groups' own fields before the altar routes: until they are built, a
    // camp choice searches for the groups without one.
    if (walk_cursor < walk_order.size()) {
        const int index = walk_order[walk_cursor++];
        auto &g = groups[index];
        subtract_into(search_goals, g.tiles(), static_blocked);
        g.walk.emplace().compute(static_blocked, search_goals, search_visited);
        const auto tiles = std::size_t(map_width) * std::size_t(map_height);
        g.walk_tiles.reset(new std::uint16_t[tiles]);
        g.walk->export_linear_buffer(std::span(g.walk_tiles.get(), tiles));
        walk_of[index] = g.walk_tiles.get();
        subtract(unwalked_camps, g.tiles());
        return true;
    }
    if (altar_routes.size() < altars.size()) {
        prepare_routes();
        return true;
    }
    return false;
}

void Bot::prepare_routes() {
    // Terrain routes are immutable. Build one per turn in the early game, a
    // fixed pace that never depends on the fuel left.
    if (altar_routes.size() < altars.size()) {
        auto goals = around(altars[altar_routes.size()], rules::settings().submit_gems_range);
        goals &= open_terrain;
        altar_routes.emplace_back().compute(static_blocked, goals, search_visited);
    }
}

void Bot::observe() {
    profiling::Scope scope(profiling::Stage::observe);
    refresh_self();
    bc::vision::visible_tiles_into(visible);
    refresh_blocked();
    enemies.clear();
    allies.clear();
    camps.clear();
    nearby_healing = 0;
    // The rosters hold every champion; checking them is far cheaper than the
    // engine's per-team vision list (about 3,700 fuel each).
    // A champion in view is alive: the dead leave the map.
    for (std::size_t i = 0; i < enemy_roster.size() && i < enemy_slot.size(); ++i)
        if (const auto info = sighting(enemy_roster[i])) {
            enemy_slot[enemies.size()] = std::uint8_t(i);
            enemies.emplace_back(enemy_roster[i], *info);
        }
    seen_slots = 0;
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (s == slot)
            continue;
        else if (const auto info = sighting(roster[s])) {
            ally_slot[allies.size()] = std::uint8_t(s);
            seen_slots |= 1u << s;
            allies.emplace_back(roster[s], *info);
        }
    ++threat_revision; // New units: what incoming() listed from them is stale.
    for (const auto &a : allies)
        if (distance(position, a.position) <= 6)
            nearby_healing += a.healing_rate;
    mark_camps_in_view();
    mark_group_holders();
    threat_count = 0;
    // A group out of view and unheld, with no records listed, no hold timed,
    // no respawn due and no capture past its timer, would be left as it is:
    // only the others are observed.
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto &g = groups[i];
        if (group_in_view[i] || group_held[i] || camps.count(int(i)) || g.holding_since != -1 ||
            (g.status == CampGroup::cooling && int(round) >= g.respawn) ||
            (g.status == CampGroup::capture && int(round) > g.seen + rules::settings().capture_time + 1))
            observe_group(i);
    }
    close_threats();
    shared.read();
    ghosts.clear();
    ghost_last.clear();
    // Ghosts are kept for ghost_lifetime rounds after a sighting, so they
    // are tracked on those rounds too: an enemy that just stepped into a bush
    // beside us is still there.
    if (!enemies.empty())
        enemy_sighted = int(round);
    if (int(round) - enemy_sighted <= policy::ghost_lifetime)
        track_ghosts();
}

void Bot::mark_camps_in_view(int group) {
    // Camp tiles in view, packed from one engine export, and their groups,
    // from one listing of those tiles: a group out of view costs no tests.
    occupancy.clear();
    occupancy |= visible;
    occupancy &= camp_tiles;
    camp_in_view.load(occupancy);
    if (group_in_view.size() != groups.size())
        group_in_view.assign(groups.size(), 0);
    std::uint32_t count = 0;
    if (group >= 0) {
        count = occupancy.count();
        bool seen = false;
        for (const auto &c : groups[group].camps)
            seen = seen || camp_in_view[c.tile];
        group_in_view[group] = seen;
    } else {
        std::fill(group_in_view.begin(), group_in_view.end(), 0);
        sighted_camps.resize(camp_count);
        count = occupancy.positions_into(sighted_camps);
        // Four tiles' indices per step, then each one's group.
        const std::uint16_t *group_of = tile_group.get();
        std::uint8_t *seen = group_in_view.data();
        const v128_t width = wasm_i32x4_splat(map_width);
        std::uint32_t k = 0;
        for (; k + 4 <= count; k += 4) {
            const v128_t a = wasm_v128_load(&sighted_camps[k]), b = wasm_v128_load(&sighted_camps[k + 2]);
            const v128_t index = wasm_i32x4_add(wasm_i32x4_mul(wasm_i32x4_shuffle(a, b, 1, 3, 5, 7), width),
                                                wasm_i32x4_shuffle(a, b, 0, 2, 4, 6));
            seen[group_of[wasm_i32x4_extract_lane(index, 0)]] = 1;
            seen[group_of[wasm_i32x4_extract_lane(index, 1)]] = 1;
            seen[group_of[wasm_i32x4_extract_lane(index, 2)]] = 1;
            seen[group_of[wasm_i32x4_extract_lane(index, 3)]] = 1;
        }
        for (; k < count; ++k)
            seen[group_of[tile_index(sighted_camps[k])]] = 1;
    }
    // Sleeping camps carry no buffs: with enough camps in view, one mask of
    // them is cheaper than a buff read each.
    asleep_known = count >= 6;
    if (asleep_known) {
        bc::vision::asleep_camp_tiles_into(occupancy);
        camp_asleep.load(occupancy);
    }
    // The living camps in view: a camp tile in view without one holds a dead
    // camp, whose read fails, so it is not read.
    bc::vision::camp_tiles_into(occupancy);
    camp_living.load(occupancy);
}

void Bot::mark_group_holders(int group) {
    // The camp groups we and our visible allies stand on.
    if (group >= 0 && group_held.size() == groups.size()) {
        bool held = group_at(position) == group;
        for (const auto &ally : allies)
            held = held || group_at(ally.position) == group;
        group_held[group] = held;
        return;
    }
    group_held.assign(groups.size(), 0);
    if (const int g = group_at(position); g >= 0)
        group_held[g] = 1;
    for (const auto &ally : allies)
        if (const int g = group_at(ally.position); g >= 0)
            group_held[g] = 1;
}

void Bot::observe_group(std::size_t i) {
    auto &g = groups[i];
    const bool friendly_holder = group_held[i];
    if (!friendly_holder)
        g.holding_since = -1;
    if (!group_in_view[i]) {
        camps.restart(int(i));
        if (g.status == CampGroup::capture && int(round) > g.seen + rules::settings().capture_time + 1) {
            // Unseen past its capture timer: taken by then, so it respawns
            // after the engine's group cooldown (80 for golems, where the
            // rules' table says 60).
            g.status = CampGroup::cooling;
            g.respawn = g.seen + rules::settings().capture_time + g.unit.max_respawn_timer();
        }
        if (g.status == CampGroup::cooling && int(round) >= g.respawn) {
            g.status = CampGroup::alive;
            g.health = g.max_health;
            g.pressure = g.max_pressure;
            g.splash = g.full_splash;
            for (auto &c : g.camps)
                c.health = c.stats.base_health;
        }
        return;
    }
    // The group's last records, brought up to date in place where they are of
    // the same camps.
    const std::span<ObservedUnit> last = camps.group(int(i));
    camps.restart(int(i));
    g.seen = round;
    int timer = 0;
    const int state = group_state(g, timer);
    if (state != 1)
        g.quiet = -10;
    if (state == 3) {
        g.status = CampGroup::cooling;
        g.respawn = round + std::max(1, timer);
        g.health = 0;
        g.pressure = g.max_pressure;
        g.splash = g.full_splash;
        return;
    }
    if (state == 2) {
        g.status = CampGroup::capture;
        g.health = 0;
        bool contested = false;
        for (const auto &e : enemies)
            if (group_at(e.position) == int(i))
                contested = true;
        if (contested)
            g.holding_since = -1;
        else if (friendly_holder && g.holding_since < 0)
            g.holding_since = round;
        for (auto &c : g.camps)
            c.health = 0;
        return;
    }
    const bool was_alive = g.status == CampGroup::alive;
    if (!was_alive)
        for (auto &c : g.camps)
            c.health = c.stats.base_health;
    g.status = CampGroup::alive;
    const bool readings_stand = rules::settings().camp_sleep_time >= 2;
    // Quiet since last round's observation, which found every camp in view
    // asleep and living: the same camps in view now, still asleep and living,
    // their records untouched. Each reading stands and each record would be
    // made again as it is, so the records are listed as they stand, and the
    // totals, from the same healths, stay as they are.
    if (was_alive && readings_stand && asleep_known && g.quiet == int(round) - 1 &&
        last.size() == g.quiet_records) {
        bool quiet = true;
        std::size_t k = 0;
        for (const auto &c : g.camps) {
            const bool in_view = camp_in_view[c.tile];
            if (in_view != c.quiet_view || (in_view && (!camp_asleep[c.tile] || !camp_living[c.tile] ||
                                                        last[k++].health != c.read_health))) {
                quiet = false;
                break;
            }
        }
        if (quiet) {
            camps.keep(int(i), g.quiet_records);
            for (std::uint32_t slot = camps.first_slot(int(i)); slot < camps.next_slot(int(i)); ++slot)
                list_threat(slot);
            for (auto &c : g.camps)
                if (c.quiet_view)
                    c.read_round = int(round);
            g.quiet = int(round);
            return;
        }
    }
    // Totals in locals (nothing below reads the group's own).
    float health = 0, pressure = 0, toughest = 1;
    bool quiet = asleep_known && readings_stand;
    for (auto &c : g.camps) {
        c.quiet_view = camp_in_view[c.tile];
        if (const int index = c.tile; c.quiet_view) {
            const bool asleep = asleep_known && camp_asleep[index];
            // Only the reading's health, maximum and strength are used, each
            // written first.
            bc::detail::camp_state info;
            bool seen_camp = true;
            // Read the round before (camps fall asleep only rounds after the
            // last hit), and not earlier this turn, when we may have hit it.
            if (asleep && c.read_asleep && int(round) - c.read_round == 1 && readings_stand) {
                info.health = c.read_health;
                info.max_health = c.read_max_health;
                info.strength = c.read_strength;
            } else
                seen_camp = camp_living[index] && read_camp(c.unit, info);
            if (seen_camp) {
                c.read_round = int(round);
                c.read_asleep = asleep;
                c.read_health = info.health;
                c.read_max_health = info.max_health;
                c.read_strength = info.strength;
                // Each slot holds the last observation's record until this one
                // reaches it: one of this camp is brought up to date in place
                // (only a reading and the buffs differ), else made anew.
                const std::size_t slot = camps.count(int(i));
                const std::uint32_t listed = camps.next_slot(int(i));
                if (slot < last.size() && last[slot].position == c.position) {
                    last[slot].reread_camp(info, asleep);
                    camps.keep_next(int(i));
                    c.health = last[slot].health;
                } else
                    c.health = camps.emplace(int(i), c, info, asleep).health;
                list_threat(listed);
            } else {
                c.read_round = -10;
                c.health = 0;
            }
            quiet = quiet && asleep && seen_camp;
        }
        health += c.health;
        if (c.health > 0)
            pressure += c.stats.base_strength;
        toughest = std::max(toughest, c.health);
    }
    g.health = health;
    g.pressure = pressure;
    g.quiet = quiet ? int(round) : -10;
    g.quiet_records = camps.count(int(i));
    // Splash accelerates only the health shared by surviving targets.
    // Eight 500-HP clusters beside one 20,000-HP boss are not a 9x clear.
    g.splash = std::max(1.0f, std::min(g.shape_splash, g.health / toughest));
}

void Bot::observe_after_move(int group) {
    // After our own move only our position, the view and the engaged group can
    // have changed: refresh those instead of observing everything again.
    profiling::Scope scope(profiling::Stage::observe);
    refresh_self();
    bc::vision::visible_tiles_into(visible);
    refresh_blocked();
    // The camp marks are read only by observe_group(), and the next full
    // observation marks them all again: mark only this group's.
    if (group < 0 || group >= int(groups.size()))
        return;
    mark_camps_in_view(group);
    camps.drop(group);
    mark_group_holders(group);
    drop_threats(group);
    observe_group(std::size_t(group));
    close_threats();
}

void Bot::list_threat(std::uint32_t slot) {
    // A stun whose timer has run out ends before the camp acts.
    if (const auto &c = camps.slot(slot); c.strength > 0 && !(c.stunned && c.stun_time > 0)) {
        camp_threats[threat_count] = int(slot);
        threat_x[threat_count] = std::int16_t(c.position.x);
        threat_y[threat_count] = std::int16_t(c.position.y);
        threat_reach[threat_count++] = std::int16_t(c.range);
    }
}

void Bot::drop_threats(int group) {
    // The group's records were listed together, so are its threats.
    const int low = int(camps.first_slot(group)), high = int(camps.next_slot(group));
    std::size_t at = 0;
    while (at < threat_count && (camp_threats[at] < low || camp_threats[at] >= high))
        ++at;
    std::size_t end = at;
    while (end < threat_count && camp_threats[end] >= low && camp_threats[end] < high)
        ++end;
    if (end == at)
        return;
    std::copy(camp_threats.get() + end, camp_threats.get() + threat_count, camp_threats.get() + at);
    for (auto *lanes : {&threat_x, &threat_y, &threat_reach})
        std::copy(lanes->get() + end, lanes->get() + threat_count, lanes->get() + at);
    threat_count -= end - at;
}

void Bot::close_threats() {
    // Lanes of eight; padding reaches no tile.
    threat_lanes = (threat_count + 7) & ~std::size_t{7};
    for (std::size_t k = threat_count; k < threat_lanes; ++k) {
        threat_x[k] = 0;
        threat_y[k] = 0;
        threat_reach[k] = no_reach;
    }
    ++threat_revision;
}

int Bot::nearest_unit(vec2 tile) const {
    if (unit_revision != threat_revision) {
        unit_revision = threat_revision;
        const std::size_t count = allies.size() + enemies.size(), padded = (count + 3) & ~std::size_t{3};
        unit_x.resize(padded);
        unit_y.resize(padded);
        std::size_t n = 0;
        for (const auto &a : allies) {
            unit_x[n] = a.position.x;
            unit_y[n++] = a.position.y;
        }
        for (const auto &e : enemies) {
            unit_x[n] = e.position.x;
            unit_y[n++] = e.position.y;
        }
        for (; n < padded; ++n)
            unit_x[n] = unit_y[n] = far_tile;
    }
    // Four units per step, then the least lane.
    const v128_t x = wasm_i32x4_splat(tile.x), y = wasm_i32x4_splat(tile.y);
    v128_t nearest = wasm_i32x4_splat(std::numeric_limits<std::int32_t>::max());
    for (std::size_t k = 0; k < unit_x.size(); k += 4)
        nearest = wasm_i32x4_min(
            nearest, wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(x, wasm_v128_load(&unit_x[k]))),
                                    wasm_i32x4_abs(wasm_i32x4_sub(y, wasm_v128_load(&unit_y[k])))));
    nearest = wasm_i32x4_min(nearest, wasm_i32x4_shuffle(nearest, nearest, 2, 3, 0, 1));
    nearest = wasm_i32x4_min(nearest, wasm_i32x4_shuffle(nearest, nearest, 1, 0, 3, 2));
    return wasm_i32x4_extract_lane(nearest, 0);
}

int Bot::nearest_unit(vec2 tile, int &ties) const {
    const int nearest = nearest_unit(tile);
    // The units that near, four a step (padding is never that near).
    const v128_t x = wasm_i32x4_splat(tile.x), y = wasm_i32x4_splat(tile.y), at = wasm_i32x4_splat(nearest);
    v128_t count = wasm_i32x4_splat(0);
    for (std::size_t k = 0; k < unit_x.size(); k += 4)
        count = wasm_i32x4_sub(
            count,
            wasm_i32x4_eq(at, wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(x, wasm_v128_load(&unit_x[k]))),
                                             wasm_i32x4_abs(wasm_i32x4_sub(y, wasm_v128_load(&unit_y[k]))))));
    ties = wasm_i32x4_extract_lane(count, 0) + wasm_i32x4_extract_lane(count, 1) +
           wasm_i32x4_extract_lane(count, 2) + wasm_i32x4_extract_lane(count, 3);
    return nearest;
}

void Bot::publish(unsigned claim) { shared.set(slot, claim); }

float Bot::dps(const CampGroup &g) const {
    // Direct and area spell DPS share the one-cast limit; cooldowns are long
    // enough that the two slots can alternate without sustained contention.
    // Camp choice's own rate: hits with Heart of Ruin's passive, the spells
    // cast on camps, a damaging dash on up to two camps.
    float value = camp_dps(spells, basic_damage(), focus, float(std::min<std::size_t>(2, g.camps.size())));
    for (const auto &s : spells)
        if (s.kind == bc::ability_kind::aoe_damage)
            value += focus * s.modifier * (g.splash - 1) / s.cooldown;
    return std::max(1.0f, value * camp_multiplier);
}

float Bot::clear_loss(const CampGroup &g, float clear_time) const {
    float loss = g.pressure * clear_time * camp_mitigation * mitigation;
    if (g.camps.size() > 1)
        loss *= camp_loss_factor(g); // Sequential kills remove attackers during a clear.
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
    auto &packed = enemy_lanes;
    if (packed.revision != threat_revision) {
        packed.revision = threat_revision;
        const std::size_t room = (enemies.size() + 3) & ~std::size_t{3};
        if (packed.x.size() < room) {
            for (auto *lanes : {&packed.x, &packed.y, &packed.reach, &packed.lunge, &packed.spell})
                lanes->resize(room);
            for (auto *lanes : {&packed.strength, &packed.lunge_strength, &packed.half_burst})
                lanes->resize(room);
        }
        std::size_t n = 0;
        for (const auto &e : enemies) {
            // A stun whose timer has run out ends before the enemy acts.
            if ((e.stunned && e.stun_time > 0) || e.channel)
                continue;
            packed.x[n] = e.position.x;
            packed.y[n] = e.position.y;
            packed.reach[n] = e.range;
            packed.lunge[n] = e.range <= 1 ? e.range + 1 : -1;
            packed.spell[n] = e.spell_reach;
            packed.strength[n] = e.strength;
            packed.lunge_strength[n] = e.strength * 0.85f;
            packed.half_burst[n++] = e.burst * 0.5f;
        }
        packed.lanes = (n + 3) & ~std::size_t{3};
        for (; n < packed.lanes; ++n) {
            packed.x[n] = packed.y[n] = 0;
            packed.reach[n] = packed.lunge[n] = packed.spell[n] = -1;
            packed.strength[n] = packed.lunge_strength[n] = packed.half_burst[n] = 0;
        }
    }
    // Four enemies a step: what each adds by its reach (a hit, else a melee
    // lunge; and a spell), then added one by one in their order, as a loop
    // over them adds. One out of reach adds zero, which leaves the total as it
    // is (never below zero).
    float damage = 0;
    {
        const v128_t x = wasm_i32x4_splat(tile.x), y = wasm_i32x4_splat(tile.y), none = wasm_f32x4_splat(0);
        for (std::size_t k = 0; k < packed.lanes; k += 4) {
            const v128_t d = wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(x, wasm_v128_load(&packed.x[k]))),
                                            wasm_i32x4_abs(wasm_i32x4_sub(y, wasm_v128_load(&packed.y[k]))));
            const v128_t hit = wasm_i32x4_le(d, wasm_v128_load(&packed.reach[k]));
            const v128_t lunge = wasm_v128_andnot(wasm_i32x4_le(d, wasm_v128_load(&packed.lunge[k])), hit);
            const v128_t spell = wasm_i32x4_le(d, wasm_v128_load(&packed.spell[k]));
            if (!wasm_v128_any_true(wasm_v128_or(wasm_v128_or(hit, lunge), spell)))
                continue;
            const v128_t strike = wasm_v128_bitselect(
                wasm_v128_load(&packed.strength[k]),
                wasm_v128_bitselect(wasm_v128_load(&packed.lunge_strength[k]), none, lunge), hit);
            const v128_t cast = wasm_v128_bitselect(wasm_v128_load(&packed.half_burst[k]), none, spell);
            damage += wasm_f32x4_extract_lane(strike, 0);
            damage += wasm_f32x4_extract_lane(cast, 0);
            damage += wasm_f32x4_extract_lane(strike, 1);
            damage += wasm_f32x4_extract_lane(cast, 1);
            damage += wasm_f32x4_extract_lane(strike, 2);
            damage += wasm_f32x4_extract_lane(cast, 2);
            damage += wasm_f32x4_extract_lane(strike, 3);
            damage += wasm_f32x4_extract_lane(cast, 3);
        }
    }
    if (!include_camps)
        return damage * mitigation;
    const auto threat = [&](std::size_t k) {
        const auto &c = camps.slot(std::uint32_t(camp_threats[k]));
        const auto &g = groups[c.group];
        // Sleeping camps, of the engaged group too, do not strike.
        if (g.attacked < int(round) - rules::settings().camp_sleep_time - 1 &&
            (g.health >= g.max_health || (asleep_known && camp_asleep[tile_index(c.position)])))
            return;
        // Camps attack the nearest champion, including our enemy. Avoid taking
        // aggro from the current tank (a unit nearer the camp than the tile);
        // a tie shares the hit among the units tied with us.
        if (threat_stamp[k] != threat_revision) {
            threat_nearest[k] = nearest_unit(c.position, threat_ties[k]);
            threat_stamp[k] = threat_revision;
        }
        if (threat_nearest[k] < distance(tile, c.position))
            return;
        if (threat_nearest[k] == distance(tile, c.position)) {
            damage += c.strength * camp_mitigation / float(1 + threat_ties[k]);
            return;
        }
        damage += c.strength * camp_mitigation;
    };
    // Which camps reach the tile, eight at a time; those in order as listed.
    const v128_t x = wasm_i16x8_splat(std::int16_t(tile.x)), y = wasm_i16x8_splat(std::int16_t(tile.y));
    for (std::size_t k = 0; k < threat_lanes; k += 8) {
        const v128_t dx = wasm_i16x8_abs(wasm_i16x8_sub(x, wasm_v128_load(&threat_x[k])));
        const v128_t dy = wasm_i16x8_abs(wasm_i16x8_sub(y, wasm_v128_load(&threat_y[k])));
        const v128_t hit = wasm_i16x8_le(wasm_i16x8_max(dx, dy), wasm_v128_load(&threat_reach[k]));
        if (!wasm_v128_any_true(hit))
            continue;
        for (unsigned lanes = wasm_i16x8_bitmask(hit); lanes; lanes &= lanes - 1)
            threat(k + unsigned(__builtin_ctz(lanes)));
    }
    return damage * mitigation;
}

float Bot::friendly_power(vec2 at) const {
    float ready_shield = 0, burst = 0;
    const auto kit = bc::self().abilities();
    const bool can_cast = bc::self().can_cast();
    for (unsigned i = 0; i < spells.size(); ++i)
        if (ready(kit[i], can_cast)) {
            const auto &s = spells[i];
            if (s.kind == bc::ability_kind::shield)
                ready_shield = focus * s.modifier;
            else if (s.kind == bc::ability_kind::attack_reset)
                burst = std::max(burst, strength);
            else if (s.modifier > 0 && s.kind != bc::ability_kind::heal)
                burst = std::max(burst, focus * s.modifier);
        }
    float p = std::sqrt(std::max(1.0f, health + ready_shield) *
                        (spell_dps(spells, strength, focus) + burst * 0.2f) / mitigation);
    // Allies count if they can join before we fall (our health over the
    // damage we take), and not while their reports put them in another fight.
    const float taking = incoming(position);
    const float lasting = taking > 0 ? health / taking : std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < allies.size(); ++i) {
        const auto &a = allies[i];
        int arrival = std::max(0, distance(at, a.position) - std::max(a.range, a.spell_reach));
        // A stunned ally still counts (a stun costs it one action), without
        // its burst while the stun has time left.
        if (float(arrival) > lasting || a.channel)
            continue;
        const int fight = TeamMemory::fight_group(shared.get(ally_slot[i]), groups.size());
        if (fight >= 0 && !same_fight(fight, at))
            continue;
        const float ally_burst = a.stunned && a.stun_time > 0 ? 0.0f : a.burst;
        p += std::sqrt(std::max(1.0f, a.health + a.shield + a.ready_shield) *
                       (a.damage_rate + ally_burst * 0.2f + a.healing_rate) / a.mitigation) *
             0.9f / (1 + arrival * 0.4f);
    }
    // Teammates out of our sight under attack in this fight are in it, at
    // their starting kit.
    const unsigned count = std::min<unsigned>(roster.size(), TeamMemory::slots);
    for (unsigned s = 0; s < count; ++s) {
        if (s == slot || (seen_slots >> s & 1) || !shared.attacked(s))
            continue;
        const unsigned code = shared.get(s);
        if (TeamMemory::requested_force(code, groups.size()) &&
            same_fight(TeamMemory::fight_group(code, groups.size()), at) && roster[s].is_alive())
            p += roster_power[s] * 0.9f;
    }
    return p;
}

void Bot::track_enemy_respawns() {
    // Enemy death timers across turns, so an engagement beside their spawn can
    // price in a fresh body arriving at full strength. Needs no vision.
    for (std::size_t i = 0; i < enemy_roster.size() && i < enemy_respawn.size(); ++i) {
        bool alive = false;
        try {
            alive = enemy_roster[i].is_alive();
        } catch (...) {
            continue;
        }
        if (alive)
            enemy_respawn[i] = -1;
        else if (enemy_respawn[i] < 0)
            enemy_respawn[i] = int(round) + rules::settings().respawn_time;
    }
}

bool Bot::camp_dormant(const ObservedUnit &c) const {
    // Asleep and not woken by us: untouched at full health, or shown asleep.
    const auto &g = groups[c.group];
    return g.attacked < int(round) - rules::settings().camp_sleep_time - 1 &&
           (g.health >= g.max_health || (asleep_known && camp_asleep[tile_index(c.position)]));
}

float Bot::enemy_power(vec2 at) const {
    // Only enemies that can fight: not channelling or on their spawn, as
    // friendly_power leaves out channelling allies. A stunned one counts
    // without its burst (a stun costs it one action). Enemies out of sight
    // count from their possible tile nearest us.
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    float p = 0;
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &e : *list) {
            if (e.channel || enemy_spawn.is_set(e.position))
                continue;
            int arrival = std::max(0, distance(at, e.position) - std::max(e.range, e.spell_reach));
            if (arrival > 6)
                continue;
            // Focus fire also multiplies a retaliator's damage: each attacker
            // pays for its own hit, including the hit that kills the target.
            float reflected = e.retaliation();
            if (reflected > 0)
                for (const auto &a : allies)
                    if (!a.stunned && !a.channel &&
                        distance(a.position, e.position) <= std::max(a.range, a.spell_reach) + 1)
                        reflected += e.retaliation();
            p += std::sqrt(std::max(1.0f, e.health + e.shield + e.ready_shield) *
                           (e.damage_rate + reflected +
                            (e.stunned && e.stun_time > 0 ? 0.0f : e.burst) * 0.2f + e.healing_rate) /
                           e.mitigation) /
                 (1 + arrival * 0.3f);
        }
    return p;
}

float Bot::respawn_threat(vec2 at) const {
    if (enemy_respawn.empty() || enemy_average_power <= 0)
        return 0;
    int spawn_dist = 100000;
    for (auto p : enemy_spawn_tiles)
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
