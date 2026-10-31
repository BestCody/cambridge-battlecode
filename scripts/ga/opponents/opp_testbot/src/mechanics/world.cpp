// Observation, cached routes and the shared threat/power estimates.
#include "../bot.h"
#include <cstring>
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
    for (const auto &m : camps) {
        hard = hard || float(m.stats.base_health) >= policy::hard_camp_health;
        reach = std::max(reach, int(m.stats.base_attack_range));
    }
    whole_work = striker_work(*this, true);
    // Every camp lives at the start: the living facts are the whole's. (Worked
    // out again, they took the first turn past its fuel on a 130-group map.)
    live_hard = hard;
    live_reach = reach;
    live_work = whole_work;
}

void CampGroup::refresh_live() {
    live_hard = false;
    live_reach = 0;
    for (const auto &m : camps)
        if (m.health > 0) {
            live_hard = live_hard || float(m.stats.base_health) >= policy::hard_camp_health;
            live_reach = std::max(live_reach, int(m.stats.base_attack_range));
        }
    live_work = striker_work(*this, false);
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

float self_shield() {
    const auto state = read_buff_state(&bc::self());
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
    // A channel holds its champion through its next turn only while time is
    // left: at zero it completes as that turn starts, and a tribute or a
    // teleport then acts as usual (a recall leaves). A recall with time left
    // can still be cancelled, as some teams do on every turn: an enemy's
    // showing the time of a recall that just started is left to observe() to
    // tell from a fake, and a teammate's is told by its report there.
    // The engine writes every entry, so the array is not zero-filled first.
    std::array<std::uint32_t, static_cast<std::size_t>(bc::item_type::count)> items;
    bc::detail::check(bc::detail::sdk_champion_items(handle, items.data()));
    static const std::int32_t own_team = read_champion(bc::self()).team;
    recalling = state.contains(bc::champion_state::recalling);
    tributing = state.contains(bc::champion_state::submitting_gems);
    if (recalling ? info.team != own_team
                  : state.contains(bc::champion_state::submitting_gems) ||
                        state.contains(bc::champion_state::teleporting)) {
        std::int32_t left = 0;
        bc::detail::check(bc::detail::sdk_champion_channel_time(handle, &left));
        if (recalling) {
            recall_left = std::int8_t(left);
            const bool home = items[static_cast<unsigned>(bc::item_type::home_emblem_of_strength)] ||
                              items[static_cast<unsigned>(bc::item_type::home_emblem_of_focus)] ||
                              items[static_cast<unsigned>(bc::item_type::home_emblem_of_health)];
            channel = !fresh_recall(left, home);
        } else
            channel = left > 0;
    }
    read_buffs(*this);
    damage_rate = strength;
    for (unsigned ability_slot = 0; ability_slot < kit.spells.size(); ++ability_slot) {
        const auto &s = kit.spells[ability_slot];
        std::int32_t cooldown = 0;
        bc::detail::check(bc::detail::sdk_champion_cooldown(handle, ability_slot, &cooldown));
        // Castable at its next action: a slot at 1 is not (measured).
        const bool ready = cooldown <= 0;
        slot_ready = std::uint8_t(slot_ready | unsigned(ready) << ability_slot);
        const bool cast = cooldown >= s.cooldown - 2;
        if (cast && s.kind == bc::ability_kind::attack_reset)
            cast_damage += strength;
        else if (cast && (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                          s.kind == bc::ability_kind::aoe_damage ||
                          (s.kind == bc::ability_kind::dash && s.modifier > 0)))
            cast_damage += focus * s.modifier;
        if (s.kind == bc::ability_kind::attack_reset) {
            damage_rate += strength / s.cooldown;
            if (ready) {
                burst = std::max(burst, strength);
                reset_ready = true;
            }
        } else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                   s.kind == bc::ability_kind::aoe_damage) {
            damage_rate += focus * s.modifier / s.cooldown;
            spell_reach = std::max(spell_reach, int(s.range + s.radius));
            if (ready) {
                burst = std::max(burst, focus * s.modifier);
                spell_burst = std::max(spell_burst, focus * s.modifier);
            }
        } else if (s.kind == bc::ability_kind::dash && s.modifier > 0) {
            // A damaging dash (Heart of Ruin) lands its area its steps plus
            // its radius away.
            damage_rate += focus * s.modifier / s.cooldown;
            spell_reach = std::max(spell_reach, int(s.range + s.radius));
            if (ready) {
                burst = std::max(burst, focus * s.modifier);
                spell_burst = std::max(spell_burst, focus * s.modifier);
            }
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
        const float bonus = hor_bonus(health, max_health);
        strength *= bonus;
        damage_rate *= bonus;
        burst *= bonus;
        spell_burst *= bonus;
        cast_damage *= bonus;
    }
    const std::array durability{bc::item_type::durability_emblem_of_strength,
                                bc::item_type::durability_emblem_of_health,
                                bc::item_type::durability_emblem_of_focus};
    for (auto item : durability)
        if (items[static_cast<unsigned>(item)]) {
            mitigation = 1 - item_effect(item).magnitude;
            break;
        }
    const std::array jungle{bc::item_type::jungle_emblem_of_strength, bc::item_type::jungle_emblem_of_health,
                            bc::item_type::jungle_emblem_of_focus};
    for (auto item : jungle)
        if (items[static_cast<unsigned>(item)]) {
            camp_mitigation = 1 - item_effect(item).magnitude;
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
        if (siege_spell(s))
            siege_reach = std::max(siege_reach, s.range);
    if (bc::draft_memory.size() >= sizeof opening)
        std::memcpy(&opening, bc::draft_memory.data(), sizeof opening);
    roster = bc::champions(bc::team::ally);
    std::sort(roster.begin(), roster.end(), [](const auto &a, const auto &b) { return id(a) < id(b); });
    roster_power.reserve(roster.size());
    roster_kits.reserve(roster.size());
    // A type's power from its design: its health with its shield, against
    // its damage. The SDK already holds every design; building another
    // reloads it.
    const auto design_power = [&](bc::champion_type type) {
        const auto index = static_cast<std::size_t>(type);
        std::optional<bc::champion> loaded;
        const auto &immutable =
            index < bc::champion_designs.size() ? bc::champion_designs[index] : loaded.emplace(type);
        const auto design = immutable.base_stats();
        const auto &described = kit_of(immutable, type).spells;
        float hp = design.base_health, heal = 0;
        for (const auto &spell : described) {
            if (spell.kind == bc::ability_kind::shield)
                hp += design.base_focus * spell.modifier;
            if (spell.kind == bc::ability_kind::heal)
                heal += design.base_focus * spell.modifier / float(spell.cooldown);
        }
        return unit_power(hp, spell_dps(described, design.base_strength, design.base_focus), 0, heal, 1);
    };
    for (std::size_t s = 0; s < roster.size(); ++s) {
        // Draft order is public and persistent. A faster ally may already have
        // dashed into a bush before this champion's first turn, so even its
        // type must come from the draft rather than a live vision query. A
        // body past the plan's roster (more than it holds) takes its type
        // from its identity, which needs no vision either. The SDK already
        // holds every design; building another reloads it.
        const auto drafted =
            s < std::min<std::size_t>(opening.body_count, opening.roster_types.size()) ? opening.roster_types[s]
                                                                                       : roster[s].champion_name();
        const auto index = static_cast<std::size_t>(drafted);
        std::optional<bc::champion> loaded;
        const auto &immutable =
            index < bc::champion_designs.size() ? bc::champion_designs[index] : loaded.emplace(drafted);
        const auto design = immutable.base_stats();
        // The type's kit now, from its design, rather than on first sight:
        // its spells, described once.
        const auto &described = kit_of(immutable, drafted).spells;
        roster_power.push_back(design_power(drafted));
        roster_kits.push_back({described, design.camp_damage_multiplier,
                               camp_dps(described, design.base_strength, design.base_focus, 1) *
                                   design.camp_damage_multiplier,
                               float(design.base_focus), float(design.base_health)});
    }
    enemy_roster = bc::champions(bc::team::enemy);
    std::sort(enemy_roster.begin(), enemy_roster.end(),
              [](const auto &a, const auto &b) { return id(a) < id(b); });
    // Their types are public without vision.
    enemy_design_power.clear();
    for (const auto &e : enemy_roster)
        enemy_design_power.push_back(design_power(e.champion_name()));
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
    {
        int sx = 0, sy = 0, count = 0;
        for (const auto p : bc::map::spawn_region(bc::team::ally).positions()) {
            sx += p.x;
            sy += p.y;
            ++count;
        }
        home_centre = count ? vec2{sx / count, sy / count} : position;
    }
    search_job_cost = turn_work() - search_start;
    own_spawn = bc::map::spawn_region(bc::team::ally);
    own_spawn_tile.load(own_spawn);
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
            map_has_gold = map_has_gold || gold > 0;
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
        // The camp lanes are written in full here (groups list the pool in
        // order), so they are never filled first.
        camp_lanes = (camp_pool.size() + 3) & ~std::size_t{3};
        camp_x.reset(new std::int32_t[camp_lanes]);
        camp_y.reset(new std::int32_t[camp_lanes]);
        camp_group.reset(new std::uint16_t[camp_lanes]);
        std::size_t lane = 0;
        const std::size_t bytes = bc::bitboard::linear_buffer_size();
        std::vector<std::uint8_t> camp_bits(bytes, 0), unwalked_bits(bytes, 0);
        for (std::size_t i = 0; i < groups.size(); ++i)
            for (auto &c : groups[i].camps) {
                camp_x[lane] = c.position.x;
                camp_y[lane] = c.position.y;
                camp_group[lane++] = static_cast<std::uint16_t>(i);
                c.tile = tile_index(c.position);
                const auto bit = std::uint8_t(1u << (c.tile & 7));
                camp_bits[std::size_t(c.tile) >> 3] |= bit;
                tile_group[c.tile] = static_cast<std::uint16_t>(i);
                if (groups[i].home_distance != bc::distance_field::unreachable)
                    unwalked_bits[std::size_t(c.tile) >> 3] |= bit;
            }
        for (; lane < camp_lanes; ++lane) {
            camp_x[lane] = camp_y[lane] = far_tile;
            camp_group[lane] = 0;
        }
        camp_tiles.import_linear_buffer(camp_bits);
        unwalked_camps.import_linear_buffer(unwalked_bits);
    }
    measure_farthest(home_field, &CampGroup::home_farthest);
    // Groups' own walk fields are built in the background: first those out of
    // reach of spawn (their searches cover only their island), then the
    // farthest, so a champion's search for the groups still without one stays
    // short (see measure_paths()).
    // Farthest first, ties in group order (a stable sort by walk): one key per
    // group, INT_MAX minus its walk above and its index below, plain integers
    // that sort far cheaper than indices compared through the groups.
    walk_order.resize(groups.size());
    walk_of.assign(groups.size(), nullptr);
    {
        std::vector<std::uint64_t> keys(groups.size());
        for (std::size_t i = 0; i < groups.size(); ++i) {
            const auto nearer = std::uint32_t(std::numeric_limits<int>::max() - groups[i].home_distance);
            keys[i] = (std::uint64_t(nearer) << 32) | i;
        }
        std::sort(keys.begin(), keys.end());
        for (std::size_t i = 0; i < groups.size(); ++i)
            walk_order[i] = int(keys[i] & 0xffffffffu);
    }
    camp_count = camp_pool.size(); // Every group's camps.
    camp_tile.load(camp_tiles);
    bush_tile.load(bc::map::bushes());
    enemy_spawn_tile.load(bc::map::spawn_region(bc::team::enemy));
    dash_terrain = bc::map::walls() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy);
    enemy_spawn_tiles = bc::map::spawn_region(bc::team::enemy).positions();
    shared.set_groups(unsigned(groups.size()));
    jungle_share = 1 - item_effect(bc::item_type::jungle_emblem_of_strength).magnitude;
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
    // out must be a different mask from a and b. (a & b) ^ a, the engine's
    // intersection written straight into out: two whole-mask operations.
    bc::detail::check(bc::detail::sdk_Bitboard_Intersect(a.ptr(), b.ptr(), out.ptr()));
    out ^= a;
}

void Bot::subtract(bc::bitboard &a, const bc::bitboard &b) {
    bc::detail::check(bc::detail::sdk_Bitboard_Intersect(a.ptr(), b.ptr(), occupancy.ptr()));
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
            walk = std::max(walk, int(exported ? path_buffer[tile_index(c.position)] : field.get(c.position)));
        g.*farthest = walk;
    }
}

int Bot::walk_bound(vec2 from, int group) const {
    if (const std::uint16_t *walks = walk_of[group])
        return walks[tile_index(from)];
    return landmark_bound(group, from, home_field.get(from),
                          altar_field_ready ? int(altar_field.get(from)) : bc::distance_field::unreachable);
}

int Bot::landmark_bound(int group, vec2 from, int home_here, int altar_here) const {
    // The straight line to the nearest camp, and a landmark field L bounds
    // the walk from both sides: the group is at least L(nearest camp) -
    // L(here) and L(here) - L(farthest camp) away (unreachable: no bound).
    const auto &g = groups[group];
    int least = std::numeric_limits<int>::max();
    for (const auto &c : g.camps)
        least = std::min(least, distance(from, c.position));
    if (home_here != bc::distance_field::unreachable)
        least = std::max({least, g.home_distance - home_here, home_here - g.home_farthest});
    if (altar_here != bc::distance_field::unreachable)
        least = std::max({least, g.altar_distance - altar_here, altar_here - g.altar_farthest});
    return least;
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
            // Past the search's limit, and at least the landmark bound.
            group_path[i] = std::max(int(limit) + 1, landmark_bound(int(i), position, home_here, altar_here));
        }
    }
}

bool Bot::background_search() {
    // One map-wide search per call, in a fixed order; false when none is left.
    if (!altar_field_ready) {
        altar_walk();
        return true;
    }
    // The altar routes (a few) first: until one is built the bank choice
    // reads straight-line distance, which walls make wrong. Then the groups'
    // own fields: until they are built, a camp choice searches for the
    // groups without one.
    if (altar_routes.size() < altars.size()) {
        prepare_routes();
        return true;
    }
    if (walk_cursor < walk_order.size()) {
        const int index = walk_order[walk_cursor++];
        auto &g = groups[index];
        subtract_into(search_goals, g.tiles(), static_blocked);
        walk_field.compute(static_blocked, search_goals, search_visited);
        const auto tiles = std::size_t(map_width) * std::size_t(map_height);
        g.walk_tiles.reset(new std::uint16_t[tiles]);
        walk_field.export_linear_buffer(std::span(g.walk_tiles.get(), tiles));
        walk_of[index] = g.walk_tiles.get();
        subtract(unwalked_camps, g.tiles());
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
    // The rosters hold every champion; checking them is far cheaper than the
    // engine's per-team vision list (about 3,700 fuel each).
    // A champion in view is alive: the dead leave the map.
    for (std::size_t i = 0; i < enemy_roster.size() && i < enemy_slot.size(); ++i)
        if (const auto info = sighting(enemy_roster[i])) {
            enemy_slot[enemies.size()] = std::uint8_t(i);
            judge_recall(enemies.emplace_back(enemy_roster[i], *info), i);
        }
    seen_slots = 0;
    slot_ally.fill(-1);
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (s == slot)
            continue;
        else if (const auto info = sighting(roster[s])) {
            ally_slot[allies.size()] = std::uint8_t(s);
            slot_ally[s] = int(allies.size());
            seen_slots |= 1u << s;
            allies.emplace_back(roster[s], *info);
        }
    ++threat_revision; // New units: what incoming() listed from them is stale.
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
    mark_camp_zones();
    note_altar_locks();
    shared.read();
    // A teammate's recall is real when it reports one, or rides a capture's
    // recall home: every champion ends its turns in a recall it cancels at
    // the next (fake_recall()).
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (allies[i].recalling)
            allies[i].channel =
                shared.get(ally_slot[i]) == TeamMemory::recall || shared.leaving(ally_slot[i]);
    ghosts.clear();
    ghost_last.clear();
    sighted_ghosts = 0;
    // Ghosts are kept for ghost_lifetime rounds after a sighting, so they
    // are tracked on those rounds too: an enemy that just stepped into a bush
    // beside us is still there. So is a hidden attacker's record.
    if (!enemies.empty())
        enemy_sighted = int(round);
    if (int(round) - enemy_sighted <= policy::ghost_lifetime || hidden)
        track_ghosts();
}

float Bot::camp_reach_damage() const {
    // Groups far from us are passed by their centre (a group spans a few
    // tiles; the longest camp reach is 5).
    float damage = 0;
    const auto group_damage = [&](const CampGroup &g) {
        if (g.status == CampGroup::alive)
            for (const auto &c : g.camps)
                if (c.health > 0 && distance(position, c.position) <= int(c.stats.base_attack_range))
                    damage += std::max(c.read_strength, float(c.stats.base_strength));
    };
    // The centres within 10, four groups a step, in group order.
    const std::size_t count = groups.size(), whole = count & ~std::size_t{3};
    const v128_t x = wasm_i32x4_splat(position.x), y = wasm_i32x4_splat(position.y), near = wasm_i32x4_splat(10);
    for (std::size_t i = 0; i < whole; i += 4) {
        const v128_t d = wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(x, wasm_v128_load(&group_cx[i]))),
                                        wasm_i32x4_abs(wasm_i32x4_sub(y, wasm_v128_load(&group_cy[i]))));
        for (unsigned lanes = wasm_i32x4_bitmask(wasm_i32x4_le(d, near)); lanes; lanes &= lanes - 1)
            group_damage(groups[i + unsigned(__builtin_ctz(lanes))]);
    }
    for (std::size_t i = whole; i < count; ++i)
        if (distance(position, groups[i].centre) <= 10)
            group_damage(groups[i]);
    return damage * camp_mitigation * mitigation; // As incoming() takes camp hits.
}

void Bot::note_hidden_hit(float lost) {
    // More health lost than everything that could have struck us since our
    // last turn: enemies in view within reach (two hits, and the spells they
    // cast since: the turn order swaps each round, so up to two enemy turns
    // fall between two of ours; counted with one, a third of the hidden hits
    // raised were a visible enemy's second), enemies seen in the last rounds
    // (two hits and their bursts),
    // and the camps that had us in reach as our last turn left the records
    // (camp_bound: one a teammate killed since may have struck first, as a
    // wolf did our Heart of Ruin's, sending it into empty bushes). An enemy
    // we cannot see struck for the rest, most likely from a bush: 14-23% of
    // the damage ours took in ladder games came from unseen attackers (the
    // opponents' 1-9%), against test-bot 12.5%, 56% of it while we fought
    // others in view, whom the fight decision counted alone (19% of our
    // deaths had such a hit in their last 5 rounds). Heals and shields only
    // hide damage. An enemy that died since may have struck first: that turn
    // is left out.
    if (enemy_died == int(round))
        return;
    float bound = camp_bound;
    for (const auto &e : enemies)
        if (enemy_threatens(e, position))
            bound += (2 * e.strength + e.cast_damage) * mitigation;
    track_ghosts();
    for (std::size_t k = 0; k < sighted_ghosts; ++k)
        if (enemy_threatens(ghosts[k], ghost_last[k], position))
            bound += (2 * ghosts[k].strength + ghosts[k].burst + ghosts[k].cast_damage) * mitigation;
    const float excess = lost - bound;
    if (excess <= 0)
        return;
    // Which enemy is not known: the record takes the hit as its strike each
    // round, the biggest burst of the enemies alive out of view (their kits
    // at the focus last seen, else the design's), and the average of their
    // health. The tile: the unseen bush tile nearest us within the longest
    // reach among them, else any unseen tile there; with neither, no enemy
    // could have struck from out of sight.
    // Only an enemy alive and unseen in the last rounds (those seen are
    // counted above) can have struck; with none, nobody hidden did. An
    // excess under half the weakest of their hits is a reading's slack.
    int chosen = -1, reach = 0;
    float biggest = -1, health_sum = 0, chosen_focus = 0, weakest = std::numeric_limits<float>::max();
    unsigned bodies = 0;
    std::uint32_t from = 0;
    for (std::size_t i = 0; i < enemy_roster.size(); ++i) {
        if (i < enemy_respawn.size() && enemy_respawn[i] >= 0)
            continue; // Dead.
        bool seen = i < ghost_sighted.size() && ghost_sighted[i] >= 0 &&
                    int(round) - ghost_sighted[i] <= policy::ghost_lifetime;
        for (std::size_t k = 0; k < enemies.size(); ++k)
            seen = seen || enemy_slot[k] == i;
        if (seen)
            continue;
        from |= 1u << i;
        const auto &c = enemy_roster[i];
        const auto kind = c.champion_name();
        const auto design = c.base_stats();
        const auto &kit = kit_of(c, kind);
        const bool known = i < ghost_seen.size() && ghost_seen[i];
        const float f = known ? std::max(ghost_seen[i]->focus, float(design.base_focus)) : float(design.base_focus);
        const float s = known ? std::max(ghost_seen[i]->strength, float(design.base_strength))
                              : float(design.base_strength);
        float burst = 0;
        int far = int(design.base_attack_range);
        for (const auto &spell : kit.spells) {
            if (spell.kind == bc::ability_kind::attack_reset)
                burst = std::max(burst, s);
            else if (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc ||
                     spell.kind == bc::ability_kind::aoe_damage ||
                     (spell.kind == bc::ability_kind::dash && spell.modifier > 0)) {
                burst = std::max(burst, f * spell.modifier);
                far = std::max(far, int(spell.range + spell.radius));
            }
        }
        reach = std::max(reach, far);
        weakest = std::min(weakest, float(design.base_strength));
        health_sum += known ? ghost_seen[i]->max_health : float(design.base_health);
        ++bodies;
        if (burst > biggest) {
            biggest = burst;
            chosen = int(i);
            chosen_focus = f;
        }
    }
    if (chosen < 0 || excess < weakest * mitigation * 0.5f)
        return;
    auto unseen = around(position, float(reach));
    unseen &= open_terrain;
    subtract(unseen, visible);
    vec2 at{-1, -1};
    for (const bool bush : {true, false}) {
        for (const vec2 p : unseen.positions())
            if ((!bush || bush_tile[tile_index(p)]) && (at.x < 0 || distance(position, p) < distance(position, at)))
                at = p;
        if (at.x >= 0)
            break;
    }
    if (at.x < 0)
        return;
    const auto &c = enemy_roster[std::size_t(chosen)];
    const auto kind = c.champion_name();
    const auto &kit = kit_of(c, kind);
    ObservedUnit ghost(c);
    ghost.type = kind;
    ghost.position = at;
    ghost.health = ghost.max_health = health_sum / float(bodies);
    ghost.strength = ghost.damage_rate = excess / mitigation;
    ghost.focus = chosen_focus;
    ghost.burst = ghost.spell_burst = biggest;
    ghost.range = distance(position, at); // It struck us from there.
    ghost.move_speed = kit.move_speed;
    ghost.immune = kit.immune;
    ghost.slot_ready = 3;
    for (const auto &spell : kit.spells) {
        if (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc ||
            spell.kind == bc::ability_kind::aoe_damage || (spell.kind == bc::ability_kind::dash && spell.modifier > 0))
            ghost.spell_reach = std::max(ghost.spell_reach, int(spell.range + spell.radius));
        if (spell.kind == bc::ability_kind::attack_reset)
            ghost.reset_ready = true;
        if (spell.kind == bc::ability_kind::dash) {
            ghost.dash_range = std::int8_t(spell.range);
            ghost.dash_wait = 0;
            ghost.dash_cooldown = std::int8_t(std::clamp(int(spell.cooldown), 1, 100));
            ghost.dash_walks = !spell.exclusive;
        }
    }
    // A new hit replaces the last record.
    ghosts.erase(ghosts.begin() + std::ptrdiff_t(sighted_ghosts), ghosts.end());
    ghost_last.resize(sighted_ghosts);
    hidden = ghost;
    hidden_hit = int(round);
    hidden_from = from;
    ghosts.push_back(ghost);
    ghost_last.push_back(at);
    ++threat_revision;
    debug_block({
        std::printf("HIDDEN r=%u id=%u lost=%.0f excess=%.0f seen=%zu at=%d,%d d=%d type=%d burst=%.0f\n", round,
                    id(bc::self()), lost, excess, enemies.size(), at.x, at.y, distance(position, at), int(kind), biggest);
    });
}

void Bot::judge_recall(ObservedUnit &enemy, std::size_t entry) {
    // A recall restarted on every turn shows the same time at every
    // sighting; a real one has counted down by the enemy turn ends since the
    // last. With none between (the turn order swaps each round), the last
    // verdict stands.
    if (!enemy.recalling)
        return;
    const int tick = int(round) + enemy.completed;
    if (tick != recall_tick[entry]) {
        recall_real[entry] =
            tick > recall_tick[entry] && recall_seen[entry] - (tick - recall_tick[entry]) == enemy.recall_left;
        recall_tick[entry] = tick;
        recall_seen[entry] = enemy.recall_left;
    }
    enemy.channel = enemy.channel || recall_real[entry];
}

void Bot::mark_camps_in_view(int group) {
    // Camp tiles in view, packed from one engine export, and their groups,
    // from one listing of those tiles: a group out of view costs no tests.
    bc::detail::check(bc::detail::sdk_Bitboard_Intersect(visible.ptr(), camp_tiles.ptr(), occupancy.ptr()));
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
    // them is cheaper than a buff read each. A group below its health is
    // told asleep only by the mask (camps never heal): without it a golem or
    // monolith cleared in part read as awake, its reach shut to walks and
    // recalls.
    bool damaged = false;
    if (count < 6)
        for (std::size_t i = 0; i < groups.size() && !damaged; ++i)
            damaged = group_in_view[i] && groups[i].status == CampGroup::alive &&
                      groups[i].health < groups[i].max_health;
    asleep_known = count >= 6 || damaged;
    if (asleep_known) {
        bc::vision::asleep_camp_tiles_into(occupancy);
        camp_asleep.load(occupancy);
    }
    // The living camps in view: a camp tile in view without one holds a dead
    // camp, whose read fails, so it is not read. Read only for camps in view
    // (observe_group()), so with none in view the old marks are never read.
    if (count) {
        bc::vision::camp_tiles_into(occupancy);
        camp_living.load(occupancy);
    }
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
            // after the engine's group cooldown (80 for golems).
            g.status = CampGroup::cooling;
            g.respawn = g.seen + rules::settings().capture_time + g.unit.max_respawn_timer();
            g.pressure = g.max_pressure; // It respawns whole, as the seen path sets.
            g.splash = g.full_splash;
        }
        if (g.status == CampGroup::cooling && int(round) >= g.respawn) {
            g.status = CampGroup::alive;
            g.health = g.max_health;
            g.pressure = g.max_pressure;
            g.splash = g.full_splash;
            for (auto &c : g.camps)
                c.health = c.stats.base_health;
            g.refresh_live();
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
    // Whether any camp's health changed: a group alive before keeps living
    // facts made from these same healths (refresh_live() follows every
    // other change of them while it lives), so with none they stand.
    bool changed = !was_alive;
    // The group's records listed so far, from its first slot (camps.count()
    // and camps.next_slot() as each record is listed).
    const std::uint32_t first_here = camps.first_slot(int(i));
    std::size_t listed_here = 0;
    for (auto &c : g.camps) {
        c.quiet_view = camp_in_view[c.tile];
        if (const int index = c.tile; c.quiet_view) {
            const bool asleep = asleep_known && camp_asleep[index];
            // Only the reading's health, maximum and strength are used, each
            // written first.
            bc::detail::camp_state info;
            bool seen_camp = true;
            // Read the round before (camps fall asleep only rounds after the
            // last hit), and not earlier this turn, when we may have hit it:
            // the reading stands as kept.
            if (asleep && c.read_asleep && int(round) - c.read_round == 1 && readings_stand) {
                info.health = c.read_health;
                info.max_health = c.read_max_health;
                info.strength = c.read_strength;
            } else {
                seen_camp = camp_living[index] && read_camp(c.unit, info);
                if (seen_camp) {
                    c.read_asleep = asleep;
                    c.read_health = info.health;
                    c.read_max_health = info.max_health;
                    c.read_strength = info.strength;
                }
            }
            if (seen_camp) {
                c.read_round = int(round);
                // Each slot holds the last observation's record until this one
                // reaches it: one of this camp is brought up to date in place
                // (only a reading and the buffs differ), else made anew.
                const std::size_t slot = listed_here;
                const std::uint32_t listed = first_here + std::uint32_t(listed_here++);
                float now;
                if (slot < last.size() && last[slot].position == c.position) {
                    last[slot].reread_camp(info, asleep);
                    camps.keep_next(int(i));
                    now = last[slot].health;
                } else
                    now = camps.emplace(int(i), c, info, asleep).health;
                changed = changed || now != c.health;
                c.health = now;
                list_threat(listed);
            } else {
                c.read_round = -10;
                changed = changed || c.health != 0;
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
    if (changed)
        g.refresh_live();
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
        // An enemy killed this turn (health written 0) no longer draws a camp.
        for (const auto &e : enemies)
            if (e.health > 0) {
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

int Bot::threat_target(std::size_t k) const {
    if (threat_stamp[k] != threat_revision) {
        threat_nearest[k] = nearest_unit(camps.slot(std::uint32_t(camp_threats[k])).position, threat_ties[k]);
        threat_stamp[k] = threat_revision;
    }
    return threat_nearest[k];
}

void Bot::publish(unsigned claim) {
    // A capture's holder keeps its claim through fights, banking and stuns:
    // teammates leave a claimed capture to it (capture_outranked()), and two
    // on the tiles split the reward. A recall still says it is leaving.
    if (claim != TeamMemory::recall && TeamMemory::requested_force(claim, groups.size()) == 0 && target >= 0 &&
        target < int(TeamMemory::bank) - 1 &&
        groups[target].status == CampGroup::capture && groups[target].tiles().is_set(position) &&
        bc::self().is_alive())
        claim = unsigned(target + 1);
    shared.set(slot, claim);
}

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

bool Bot::visit_affordable(const CampGroup &g) const {
    // The reach camp choice reads (CampGroup's shared facts).
    const float reach = float(g.status == CampGroup::alive ? g.live_reach : g.reach);
    const float exit_steps = std::max(0.0f, reach - float(range - 1));
    const float usable = std::max(100.0f, max_health * (1 - policy::retreat_health));
    return usable - g.pressure * exit_steps * camp_mitigation * mitigation >= 100.0f;
}

float Bot::self_heal() const {
    float rate = 0;
    for (const auto &s : spells)
        if (s.kind == bc::ability_kind::heal && s.targets.contains(bc::ability_target_mask::self))
            rate += focus * s.modifier / float(s.cooldown);
    return rate;
}

int Bot::mate_claim(unsigned s) const {
    // A fight a teammate reports near a group, or the claim a dead one left,
    // is no claim: cluster runs, capture holders and hard groups' partners
    // read them as one where camp choice did not.
    const unsigned code = shared.get(s);
    return s != slot && s < roster.size() && roster[s].is_alive() && code >= 1 && code <= groups.size()
               ? int(code) - 1
               : -1;
}

float Bot::slot_heal(unsigned s) const {
    if (s < slot_ally.size() && slot_ally[s] >= 0)
        return allies[std::size_t(slot_ally[s])].healing_rate;
    float rate = 0;
    if (s < roster_kits.size())
        for (const auto &spell : roster_kits[s].spells)
            if (spell.kind == bc::ability_kind::heal)
                rate += roster_kits[s].focus * spell.modifier / float(spell.cooldown);
    return rate;
}

float Bot::group_heal(int group) const {
    // Only the healers at the clear: the allies near us now healed every
    // clear far away (at spawn a Willow beside everyone made every group's
    // camp damage nothing).
    float rate = self_heal();
    const unsigned count = unsigned(std::min<std::size_t>(roster.size(), TeamMemory::slots));
    for (unsigned s = 0; s < count; ++s)
        if (mate_claim(s) == group)
            rate += slot_heal(s);
    return rate;
}

__attribute__((always_inline)) inline float Bot::incoming_enemies(vec2 tile) const {
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
            if (e.health <= 0 || (e.stunned && e.stun_time > 0) || e.channel)
                continue;
            // A ready attack reset is a second hit where the first lands; a
            // ranged champion may walk and still shoot once with it (the
            // reset gives back the attack the walk took).
            const float hits = e.reset_ready ? 2.0f : 1.0f;
            packed.x[n] = e.position.x;
            packed.y[n] = e.position.y;
            packed.reach[n] = e.range;
            packed.lunge[n] = e.range <= 1 || e.reset_ready ? e.range + e.move_speed : -1;
            packed.spell[n] = e.spell_reach;
            packed.strength[n] = e.strength * hits;
            packed.lunge_strength[n] = e.strength * (e.range <= 1 ? hits : 1.0f) * 0.85f;
            packed.half_burst[n++] = e.spell_burst * 0.5f;
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
    return damage;
}

float Bot::incoming(vec2 tile, int engaged_group, bool include_camps) const {
    float damage = incoming_enemies(tile);
    if (!include_camps)
        return damage * mitigation;
    const auto threat = [&](std::size_t k) {
        const auto &c = camps.slot(std::uint32_t(camp_threats[k]));
        if (c.health <= 0) // Killed earlier this turn.
            return;
        const auto &g = groups[c.group];
        // Sleeping camps, of the engaged group too, do not strike.
        if (g.attacked < int(round) - rules::settings().camp_sleep_time - 1 &&
            (g.health >= g.max_health || (asleep_known && camp_asleep[tile_index(c.position)])))
            return;
        // Camps attack the nearest champion, including our enemy. Avoid taking
        // aggro from the current tank (a unit nearer the camp than the tile);
        // a tie shares the hit among the units tied with us.
        const int nearest = threat_target(k);
        if (nearest < distance(tile, c.position))
            return;
        if (nearest == distance(tile, c.position)) {
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

float Bot::incoming_scan(vec2 tile) const {
    // incoming(tile) while nothing changes between calls (one serial): each
    // camp's tests that do not depend on the tile (alive, awake, its hit,
    // the nearest unit) made on its first hit and kept for the serial. The
    // camps' hits are added as incoming() adds them, in threat order.
    float damage = incoming_enemies(tile);
    const v128_t x = wasm_i16x8_splat(std::int16_t(tile.x)), y = wasm_i16x8_splat(std::int16_t(tile.y));
    for (std::size_t k = 0; k < threat_lanes; k += 8) {
        const v128_t dx = wasm_i16x8_abs(wasm_i16x8_sub(x, wasm_v128_load(&threat_x[k])));
        const v128_t dy = wasm_i16x8_abs(wasm_i16x8_sub(y, wasm_v128_load(&threat_y[k])));
        const v128_t hit = wasm_i16x8_le(wasm_i16x8_max(dx, dy), wasm_v128_load(&threat_reach[k]));
        if (!wasm_v128_any_true(hit))
            continue;
        for (unsigned lanes = wasm_i16x8_bitmask(hit); lanes; lanes &= lanes - 1) {
            const std::size_t t = k + unsigned(__builtin_ctz(lanes));
            if (scan_stamp[t] != scan_serial) {
                scan_stamp[t] = scan_serial;
                scan_near[t] = std::numeric_limits<int>::min(); // Strikes nobody.
                const auto &c = camps.slot(std::uint32_t(camp_threats[t]));
                if (c.health <= 0)
                    continue;
                const auto &g = groups[c.group];
                if (g.attacked < int(round) - rules::settings().camp_sleep_time - 1 &&
                    (g.health >= g.max_health || (asleep_known && camp_asleep[tile_index(c.position)])))
                    continue;
                scan_near[t] = threat_target(t);
                scan_hit[t] = c.strength * camp_mitigation;
            }
            const int nearest = scan_near[t];
            if (nearest == std::numeric_limits<int>::min())
                continue;
            const int d = std::max(std::abs(tile.x - int(threat_x[t])), std::abs(tile.y - int(threat_y[t])));
            if (nearest < d)
                continue;
            if (nearest == d)
                damage += scan_hit[t] / float(1 + threat_ties[t]);
            else
                damage += scan_hit[t];
        }
    }
    return damage * mitigation;
}

void Bot::begin_scan() {
    if (scan_stamp.size() < threat_lanes) {
        scan_stamp.resize(threat_lanes, 0);
        scan_near.resize(threat_lanes);
        scan_hit.resize(threat_lanes);
    }
    if (++scan_serial == 0) {
        std::fill(scan_stamp.begin(), scan_stamp.end(), 0);
        scan_serial = 1;
    }
}

const Bot::ExitLanes &Bot::exit_packed() const {
    auto &packed = exit_lanes;
    if (packed.revision != threat_revision || packed.ghosts != ghost_update) {
        packed.revision = threat_revision;
        packed.ghosts = ghost_update;
        const std::size_t room = (enemies.size() + ghosts.size() + 3) & ~std::size_t{3};
        if (packed.x.size() < room) {
            for (auto *lanes : {&packed.x, &packed.y, &packed.held, &packed.reach, &packed.lunge, &packed.reach_a,
                                &packed.reach_b, &packed.stun})
                lanes->resize(room);
            for (auto *lanes : {&packed.hit, &packed.burst_a, &packed.burst_b})
                lanes->resize(room);
        }
        packed.most = packed.hits = 0;
        std::size_t n = 0;
        // Ghosts, as enemy_power() counts them; an unseen enemy's cooldowns
        // are unknown, so its slots and dash count as ready. One on its spawn
        // strikes from there.
        for (const auto *list : {&enemies, &ghosts})
            for (const auto &e : *list) {
                if (e.health <= 0 || e.channel)
                    continue;
                const bool unseen = list == &ghosts;
                const auto &kit = kit_of(e.champion(), e.type);
                const unsigned ready = unseen ? 3u : e.slot_ready;
                // Its strike next round (closing_tiles()), its reset and dash
                // ready as read (an unseen one's counted ready).
                bool reset = false;
                for (unsigned k = 0; k < kit.spells.size(); ++k)
                    reset = reset || (ready >> k & 1 && kit.spells[k].kind == bc::ability_kind::attack_reset);
                const int lunge = e.range + closing_tiles(e, reset, unseen || e.dash_wait == 0);
                // Heart of Ruin's passive raises its spells as its hits
                // (the strength reading already has it).
                const float bonus =
                    e.type == bc::champion_type::heart_of_ruin ? hor_bonus(e.health, e.max_health) : 1.0f;
                std::array<float, 2> burst{};
                std::array<int, 2> reach{-1, -1};
                int stun = -1;
                for (unsigned k = 0; k < kit.spells.size(); ++k) {
                    const auto &s = kit.spells[k];
                    if (!(ready >> k & 1))
                        continue;
                    // A cast from where it stands, or after a walk when the
                    // spell allows one.
                    const int cast = int(s.range + s.radius) + (s.exclusive ? 0 : e.move_speed);
                    if (s.kind == bc::ability_kind::attack_reset) {
                        burst[k] = e.strength;
                        reach[k] = lunge;
                        // Marcella's enhanced hit stuns.
                        if (s.enhanced)
                            stun = std::max(stun, lunge);
                    } else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                               s.kind == bc::ability_kind::aoe_damage ||
                               (s.kind == bc::ability_kind::dash && s.modifier > 0)) {
                        burst[k] = e.focus * s.modifier * bonus;
                        reach[k] = cast;
                        if (s.duration > 0 && s.kind != bc::ability_kind::damage)
                            stun = std::max(stun, cast);
                    }
                }
                packed.x[n] = e.position.x;
                packed.y[n] = e.position.y;
                packed.held[n] = e.stunned && e.stun_time > 0 ? e.stun_time : 0;
                packed.reach[n] = std::max(e.range + e.move_speed, e.spell_reach);
                packed.lunge[n] = lunge;
                packed.reach_a[n] = reach[0];
                packed.reach_b[n] = reach[1];
                packed.stun[n] = stun;
                packed.hit[n] = e.strength;
                packed.burst_a[n] = burst[0];
                packed.burst_b[n] = burst[1];
                // A hit a round over its reach, one more for a stun on us
                // (reach is at least 2: every champion walks).
                packed.most += burst[0] + burst[1] + e.strength * float(packed.reach[n] + 1);
                packed.hits += e.strength;
                ++n;
            }
        packed.lanes = (n + 3) & ~std::size_t{3};
        for (; n < packed.lanes; ++n) {
            packed.x[n] = packed.y[n] = packed.held[n] = 0;
            packed.reach[n] = packed.lunge[n] = packed.reach_a[n] = packed.reach_b[n] = packed.stun[n] = -1;
            packed.hit[n] = packed.burst_a[n] = packed.burst_b[n] = 0;
        }
    }
    return packed;
}

float Bot::exit_camps(vec2 tile) const {
    // Awake camps strike the nearest champion every round, and leaving a
    // fight under one costs a hit a round until we are out of its reach:
    // left out, ours fought from beside awake elks as if the way out were
    // free (on 01hooks against SkuBap elk hits were 31% of the damage we
    // took before round 350; our Prism stood beside one, its nearest
    // champion, for 8 rounds and died, and our Vendetta died walking out
    // past one). Only with an enemy to leave: alone, camp choice prices the
    // camps (visit_affordable()).
    if (!exit_packed().lanes)
        return 0;
    // Each camp that would strike the tile, as incoming() finds them (its
    // threats, eight a step): its hits from now until we walk out of its
    // reach (it acts at the start of each round). One whose nearest
    // champion is nearer than we are strikes that one; a tie shares the
    // hits as incoming() shares them, a unit on the tile itself (a
    // teammate's own) not counted as another.
    float camp = 0;
    const v128_t x = wasm_i16x8_splat(std::int16_t(tile.x)), y = wasm_i16x8_splat(std::int16_t(tile.y));
    for (std::size_t k = 0; k < threat_lanes; k += 8) {
        const v128_t dx = wasm_i16x8_abs(wasm_i16x8_sub(x, wasm_v128_load(&threat_x[k])));
        const v128_t dy = wasm_i16x8_abs(wasm_i16x8_sub(y, wasm_v128_load(&threat_y[k])));
        const v128_t hit = wasm_i16x8_le(wasm_i16x8_max(dx, dy), wasm_v128_load(&threat_reach[k]));
        if (!wasm_v128_any_true(hit))
            continue;
        for (unsigned lanes = wasm_i16x8_bitmask(hit); lanes; lanes &= lanes - 1) {
            const std::size_t t = k + unsigned(__builtin_ctz(lanes));
            const auto &c = camps.slot(std::uint32_t(camp_threats[t]));
            if (c.health <= 0 || camp_dormant(c))
                continue;
            const int d = distance(tile, c.position), nearest = threat_target(t);
            if (d > nearest)
                continue;
            int ties = 0;
            if (d == nearest) {
                ties = threat_ties[t];
                for (std::size_t u = 0; u < unit_x.size(); ++u)
                    ties -= unit_x[u] == tile.x && unit_y[u] == tile.y;
            }
            camp += c.strength * float(c.range - d + 1) / float(1 + ties); // The unit's camp share is the caller's.
        }
    }
    return camp;
}

Bot::ExitDamage Bot::exit_damage(vec2 tile, bool immune, vec2 without) const {
    return exit_damage(tile, immune, without, exit_camps(tile) * camp_mitigation);
}

Bot::ExitDamage Bot::exit_damage(vec2 tile, bool immune, vec2 without, float camps) const {
    const auto &packed = exit_packed();
    if (!packed.lanes)
        return {};
    // Four enemies a step (one on `without` adds nothing). Distances count
    // from the round each acts again (a stunned one loses its next), so
    // every reach test is one compare.
    const v128_t x = wasm_i32x4_splat(tile.x), y = wasm_i32x4_splat(tile.y);
    const v128_t out_x = wasm_i32x4_splat(without.x), out_y = wasm_i32x4_splat(without.y);
    const v128_t one = wasm_i32x4_splat(1), none = wasm_f32x4_splat(0);
    // One pass: what a ready stun on us adds (a hit from each enemy whose
    // reach we stand in, and its round's hits where it had none) is summed
    // apart and counted when any stun reaches us.
    const v128_t zero = wasm_i32x4_splat(0);
    v128_t damage = none, round_hits = none, held = none, held_round = none, stun = zero;
    for (std::size_t k = 0; k < packed.lanes; k += 4) {
        const v128_t x_k = wasm_v128_load(&packed.x[k]), y_k = wasm_v128_load(&packed.y[k]);
        const v128_t d = wasm_i32x4_add(wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(x, x_k)),
                                                       wasm_i32x4_abs(wasm_i32x4_sub(y, y_k))),
                                        wasm_v128_load(&packed.held[k]));
        const v128_t keep = wasm_v128_not(wasm_v128_and(wasm_i32x4_eq(x_k, out_x), wasm_i32x4_eq(y_k, out_y)));
        stun = wasm_v128_or(stun, wasm_v128_and(keep, wasm_i32x4_le(d, wasm_v128_load(&packed.stun[k]))));
        const v128_t next = wasm_i32x4_add(d, one);
        const v128_t reach = wasm_v128_load(&packed.reach[k]);
        // Rounds within its reach as we walk a tile a round, at least the
        // strike its lunge lands.
        const v128_t walking = wasm_i32x4_max(wasm_i32x4_sub(reach, d), zero);
        const v128_t hits = wasm_i32x4_max(
            walking, wasm_v128_and(wasm_i32x4_le(d, wasm_v128_load(&packed.lunge[k])), one));
        const v128_t hit = wasm_v128_and(wasm_v128_load(&packed.hit[k]), keep);
        // One cast a turn: the bursts in the better order, the second a tile further.
        const v128_t a = wasm_v128_load(&packed.burst_a[k]), b = wasm_v128_load(&packed.burst_b[k]);
        const v128_t reach_a = wasm_v128_load(&packed.reach_a[k]), reach_b = wasm_v128_load(&packed.reach_b[k]);
        const v128_t a_first = wasm_f32x4_add(wasm_v128_and(a, wasm_i32x4_le(d, reach_a)),
                                              wasm_v128_and(b, wasm_i32x4_le(next, reach_b)));
        const v128_t b_first = wasm_f32x4_add(wasm_v128_and(b, wasm_i32x4_le(d, reach_b)),
                                              wasm_v128_and(a, wasm_i32x4_le(next, reach_a)));
        damage = wasm_f32x4_add(damage, wasm_f32x4_add(wasm_f32x4_mul(wasm_f32x4_convert_i32x4(hits), hit),
                                                       wasm_v128_and(wasm_f32x4_max(a_first, b_first), keep)));
        const v128_t hitting = wasm_i32x4_gt(hits, zero), within = wasm_i32x4_le(d, reach);
        round_hits = wasm_f32x4_add(round_hits, wasm_v128_and(hit, hitting));
        held = wasm_f32x4_add(held, wasm_v128_and(hit, within));
        held_round = wasm_f32x4_add(held_round, wasm_v128_and(hit, wasm_v128_andnot(within, hitting)));
    }
    if (!immune && wasm_v128_any_true(stun)) {
        damage = wasm_f32x4_add(damage, held);
        round_hits = wasm_f32x4_add(round_hits, held_round);
    }
    return {camps + wasm_f32x4_extract_lane(damage, 0) + wasm_f32x4_extract_lane(damage, 1) +
                wasm_f32x4_extract_lane(damage, 2) + wasm_f32x4_extract_lane(damage, 3),
            wasm_f32x4_extract_lane(round_hits, 0) + wasm_f32x4_extract_lane(round_hits, 1) +
                wasm_f32x4_extract_lane(round_hits, 2) + wasm_f32x4_extract_lane(round_hits, 3)};
}

Bot::ExitRisk Bot::exit_risk(vec2 tile, float floor, float mitigation, float camp_share, unsigned carried,
                             bool immune) const {
    // The lanes' bound first, with the camps' part (a few camps, most out
    // of reach of the tile): below the floor, nothing more is computed.
    const auto &packed = exit_packed();
    const float margin_share = float(carried) / float(policy::prism_bag);
    const float camps = exit_camps(tile) * camp_share; // Its Jungle emblem's, ours or an ally's.
    if ((packed.most + packed.hits * margin_share + camps) * mitigation < floor)
        return {};
    const auto taken = exit_damage(tile, immune, {-1, -1}, camps);
    return {taken.damage * mitigation, taken.round * margin_share * mitigation};
}

float Bot::exit_pool(float shield, const std::array<bool, 2> &is_ready) const {
    float pool = health + shield;
    for (unsigned i = 0; i < spells.size(); ++i)
        if (is_ready[i] && spells[i].kind == bc::ability_kind::shield)
            pool += focus * spells[i].modifier;
    return pool;
}

float Bot::exit_pool() const {
    // Only a shield slot's readiness counts: the others are not read.
    std::array<bool, 2> is_ready{};
    for (unsigned i = 0; i < spells.size(); ++i)
        if (spells[i].kind == bc::ability_kind::shield)
            is_ready[i] = ready(bc::self().abilities()[i], bc::self().can_cast());
    return exit_pool(self_shield(), is_ready);
}

bool Bot::ExitCheck::lethal(vec2 tile) {
    const bool immune = bot.passive == bc::passive_type::stun_immunity;
    const float damage = bot.exit_risk(tile, bot.health, bot.mitigation, bot.camp_mitigation, 0, immune).damage;
    if (damage <= 0)
        return false;
    if (pool < 0) {
        pool = bot.exit_pool();
        here = bot.exit_damage(bot.position, immune).damage * bot.mitigation;
    }
    return damage >= pool && damage > here;
}

float Bot::own_power() const {
    float ready_shield = 0, burst = 0, heal = 0;
    const auto kit = bc::self().abilities();
    const bool can_cast = bc::self().can_cast();
    for (unsigned i = 0; i < spells.size(); ++i) {
        const auto &s = spells[i];
        if (s.kind == bc::ability_kind::heal)
            heal += focus * s.modifier / float(s.cooldown); // As ObservedUnit's healing_rate.
        if (ready(kit[i], can_cast)) {
            if (s.kind == bc::ability_kind::shield)
                ready_shield = focus * s.modifier;
            else if (s.kind == bc::ability_kind::attack_reset)
                burst = std::max(burst, strength);
            else if (s.modifier > 0 && s.kind != bc::ability_kind::heal)
                burst = std::max(burst, focus * s.modifier);
        }
    }
    return unit_power(health + self_shield() + ready_shield, spell_dps(spells, strength, focus), burst, heal,
                      mitigation);
}

bool Bot::acts_on(const ObservedUnit &u, vec2 tile) const {
    const int move = u.stunned && u.stun_time > 0 ? 0 : u.move_speed;
    return distance(u.position, tile) <= std::max(u.range, u.spell_reach) + move;
}

float Bot::unit_power_of(const ObservedUnit &u, bool full) const {
    return unit_power((full ? u.max_health : u.health) + u.shield + u.ready_shield, u.damage_rate,
                      u.stunned && u.stun_time > 0 ? 0.0f : u.burst, u.healing_rate, u.mitigation);
}

std::pair<float, float> Bot::sides_at(vec2 tile) const {
    std::pair<float, float> s{0.0f, own_power()};
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &e : *list)
            if (e.health > 0 && !e.channel && acts_on(e, tile))
                s.first += unit_power_of(e, enemy_spawn_tile[tile_index(e.position)]);
    for (const auto &a : allies)
        if (a.health > 0 && !a.channel && acts_on(a, tile))
            s.second += unit_power_of(a, false);
    return s;
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
        else if (enemy_respawn[i] < 0) {
            enemy_respawn[i] = int(round) + rules::settings().respawn_time;
            enemy_died = int(round);
            if (i < ghost_seen.size())
                ghost_seen[i].reset(); // Its bag went with it (behind_pressure()).
        }
    }
}

void Bot::mark_camp_zones() {
    // The reach of an awake golem or monolith of a group we do not farm is no
    // way through: they strike the nearest champion for 120-200 a round,
    // outreach every champion, and a hit cancels a recall. Ours walked up to
    // monoliths an enemy had woken, or fought beside them, and took 3 times
    // willow best girl's monolith damage, most of it walking back out. One
    // whose reach we stand in is left out, so we can walk out.
    camp_zone_round = -1;
    zone_target = target;
    for (const auto &c : camps) {
        if (c.health <= 0 || c.strength < policy::avoided_camp_strength || c.group == target ||
            distance(position, c.position) <= c.range || camp_dormant(c))
            continue;
        if (camp_zone_round != int(round)) {
            camp_zone.clear();
            camp_zone_round = int(round);
        }
        camp_zone.draw_chebyshev(c.position, float(c.range));
    }
    if (camp_zone_round == int(round))
        blocked |= camp_zone;
}

bool Bot::camp_dormant(const ObservedUnit &c) const {
    // Asleep and not woken by us: untouched at full health, or shown asleep.
    const auto &g = groups[c.group];
    return g.attacked < int(round) - rules::settings().camp_sleep_time - 1 &&
           (g.health >= g.max_health || (asleep_known && camp_asleep[tile_index(c.position)]));
}

float Bot::enemy_power(vec2 at) const {
    // Only enemies that can fight: not channelling. A stunned one counts
    // without its burst (a stun costs it one action). Enemies out of sight
    // count from their possible tile nearest us. One on its spawn fights from there and heals to full
    // at the end of each of its turns there (docs/GameRules.md): it counts at
    // full health, and as we do not strike it there, it reflects nothing.
    // Left out, fights at their spawn's edge looked winnable: on
    // 02treasure_box against DICTATOR_Sahur 9 of our 11 deaths began within 2
    // tiles of their spawn, and 36% of their damage to our champions came
    // from spawn tiles.
    float p = 0;
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &e : *list) {
            if (e.channel)
                continue;
            int arrival = std::max(0, distance(at, e.position) - std::max(e.range, e.spell_reach));
            if (arrival > 6)
                continue;
            const bool on_spawn = enemy_spawn_tile[tile_index(e.position)];
            // Focus fire also multiplies a retaliator's damage: each attacker
            // pays for its own hit, including the hit that kills the target.
            float reflected = on_spawn ? 0.0f : e.retaliation();
            if (reflected > 0)
                for (const auto &a : allies)
                    if (!a.stunned && !a.channel &&
                        distance(a.position, e.position) <= std::max(a.range, a.spell_reach) + 1)
                        reflected += e.retaliation();
            p += unit_power((on_spawn ? e.max_health : e.health) + e.shield + e.ready_shield,
                            e.damage_rate + reflected, e.stunned && e.stun_time > 0 ? 0.0f : e.burst,
                            e.healing_rate, e.mitigation) /
                 (1 + arrival * 0.3f);
        }
    return p;
}

float Bot::respawn_threat(vec2 at) const {
    // A dead enemy back by next round that can act on the tile from its
    // spawn by its next turn (its attack or damaging spell reach and a
    // move, as acts_on() counts one), at its design's full health. It was
    // every body back within 5 rounds, within 9 tiles of their spawn, cut
    // by a tenth a tile.
    int spawn_dist = std::numeric_limits<int>::max();
    for (auto p : enemy_spawn_tiles)
        spawn_dist = std::min(spawn_dist, distance(at, p));
    float p = 0;
    for (std::size_t i = 0; i < enemy_respawn.size() && i < enemy_roster.size(); ++i) {
        if (enemy_respawn[i] < 0 || enemy_respawn[i] - int(round) > 1)
            continue;
        const auto &c = enemy_roster[i];
        const auto &kit = kit_of(c, c.champion_name());
        int far = int(c.base_stats().base_attack_range);
        for (const auto &spell : kit.spells)
            if (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc ||
                spell.kind == bc::ability_kind::aoe_damage ||
                (spell.kind == bc::ability_kind::dash && spell.modifier > 0))
                far = std::max(far, int(spell.range + spell.radius));
        if (spawn_dist <= far + kit.move_speed)
            p += enemy_design_power[i];
    }
    return p;
}
