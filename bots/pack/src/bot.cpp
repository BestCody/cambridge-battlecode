// Champion decisions for the pack bot. See bot.h for the design.
#include "bot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <variant>

namespace {
constexpr bool debug = true;
inline void probe(const char *what) { std::printf("FUEL %s %llu\n", what, (unsigned long long)bc::fuel_remaining()); }
constexpr int kUnreachable = bc::distance_field::unreachable;
constexpr float kGemGold = 12.0f;     // a gem's worth in gold for target choice
constexpr unsigned kBankBag = 40;     // voluntary bank trips carry this many
constexpr unsigned kShopGold = 300;   // recall to shop with this much and no bag
constexpr int kEnemyMemory = 40;      // rounds an enemy sighting keeps a group costly

using Item = bc::item_type;
struct Step {
    Item item;
    unsigned count;
};
// Build orders by champion: the families the top teams finish first, then a
// repeated stat shard. Buying health on spawn also heals.
std::span<const Step> build_for(bc::champion_type type, Item &repeat) {
    static const Step vesper[]{{Item::treasure_shard_of_strength, 5},  {Item::power_shard_of_strength, 5},
                               {Item::durability_shard_of_health, 5},  {Item::prospector_shard_of_strength, 5},
                               {Item::monster_shard_of_strength, 5},   {Item::jungle_shard_of_health, 5},
                               {Item::skirmish_shard_of_strength, 5}};
    static const Step heart[]{{Item::treasure_shard_of_focus, 5},  {Item::power_shard_of_focus, 5},
                              {Item::home_shard_of_health, 5},     {Item::durability_shard_of_health, 5},
                              {Item::prospector_shard_of_focus, 5}, {Item::monster_shard_of_focus, 5}};
    static const Step vendetta[]{{Item::treasure_shard_of_strength, 5}, {Item::durability_shard_of_health, 5},
                                 {Item::jungle_shard_of_health, 5},     {Item::monster_shard_of_strength, 5},
                                 {Item::capture_shard_of_strength, 5}};
    static const Step marcella[]{{Item::treasure_shard_of_focus, 5}, {Item::durability_shard_of_health, 5}};
    static const Step varg[]{{Item::treasure_shard_of_strength, 5}, {Item::durability_shard_of_health, 5},
                             {Item::jungle_shard_of_health, 5},     {Item::monster_shard_of_strength, 5}};
    static const Step other[]{{Item::treasure_shard_of_strength, 5}, {Item::durability_shard_of_health, 5}};
    switch (type) {
        case bc::champion_type::vesper_vale: repeat = Item::bruiser_shard_of_strength; return vesper;
        case bc::champion_type::heart_of_ruin: repeat = Item::bruiser_shard_of_focus; return heart;
        case bc::champion_type::vendetta: repeat = Item::bruiser_shard_of_strength; return vendetta;
        case bc::champion_type::marcella: repeat = Item::bruiser_shard_of_focus; return marcella;
        case bc::champion_type::varg: repeat = Item::bruiser_shard_of_strength; return varg;
        default: repeat = Item::bruiser_shard_of_strength; return other;
    }
}

std::optional<bc::detail::champion_info> sighting(const bc::champion &c) {
    bc::detail::champion_info info{};
    const auto code = bc::detail::sdk_champion(handle_of(c), &info);
    if (code == static_cast<std::int32_t>(bc::error_code::api_not_in_vision) ||
        code == static_cast<std::int32_t>(bc::error_code::api_not_found))
        return std::nullopt;
    bc::detail::check(code);
    if (info.x < 0 || info.y < 0)
        return std::nullopt;
    return info;
}

bool read_camp(const bc::camp &c, bc::detail::camp_state &out) {
    const auto code = bc::detail::sdk_camp(static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(c)), &out);
    if (code == static_cast<std::int32_t>(bc::error_code::api_not_in_vision) ||
        code == static_cast<std::int32_t>(bc::error_code::api_not_found))
        return false;
    bc::detail::check(code);
    return true;
}

int raw_group_state(const bc::map::camp_group &g, int &timer) {
    const auto handle = static_cast<std::uint32_t>(static_cast<bc::handle_camp_group_t>(g));
    std::uint32_t state = 0;
    bc::detail::check(bc::detail::sdk_camp_group_state(handle, &state));
    if (state == 3) {
        std::int32_t cooldown = 0;
        bc::detail::check(bc::detail::sdk_camp_group_cooldown(handle, &cooldown));
        timer = cooldown;
    }
    return int(state);
}

const std::array<vec2, 8> around8{vec2{-1, -1}, vec2{0, -1}, vec2{1, -1}, vec2{-1, 0},
                                   vec2{1, 0},   vec2{-1, 1}, vec2{0, 1},  vec2{1, 1}};
bool inside(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < bc::map::width() && p.y < bc::map::height(); }
} // namespace

// ---------------------------------------------------------------- world

void Bot::initialise() {
    auto &me = bc::self();
    type = me.champion_name();
    stats = me.base_stats();
    melee = stats.base_attack_range <= 1;
    for (const auto &a : me.abilities())
        if (a.info() && a.is_dash()) {
            has_dash = true;
            dash_slot = int(a.slot());
        }
    ally_roster = bc::champions(bc::team::ally);
    enemy_roster = bc::champions(bc::team::enemy);
    std::sort(ally_roster.begin(), ally_roster.end(),
              [](const bc::champion &a, const bc::champion &b) { return handle_of(a) < handle_of(b); });
    const unsigned mine = handle_of(me);
    for (unsigned i = 0; i < ally_roster.size(); ++i)
        if (handle_of(ally_roster[i]) == mine)
            slot = i;

    holes = bc::map::holes();
    terrain = bc::map::walls();
    terrain |= bc::map::altars();
    terrain |= bc::map::spawn_region(bc::team::enemy);
    terrain_jump = terrain;
    terrain |= holes;
    own_spawn = bc::map::spawn_region(bc::team::ally);
    {
        long sx = 0, sy = 0, n = 0;
        for (auto p : own_spawn.positions()) {
            sx += p.x;
            sy += p.y;
            ++n;
        }
        home = n ? vec2{int(sx / n), int(sy / n)} : me.position();
        sx = sy = n = 0;
        for (auto p : bc::map::spawn_region(bc::team::enemy).positions()) {
            sx += p.x;
            sy += p.y;
            ++n;
        }
        enemy_home = n ? vec2{int(sx / n), int(sy / n)} : home;
    }
    any_spawn.clear();
    for (const auto &g : bc::map::camp_groups()) {
        Group group(g);
        group.gems = g.gem_value();
        group.gold = g.gold_value();
        group.respawn_max = g.max_respawn_timer();
        long sx = 0, sy = 0;
        for (const auto &c : g.camps()) {
            const vec2 p = c.spawn_position();
            group.tiles.push_back(p);
            any_spawn.set(p);
            sx += p.x;
            sy += p.y;
            const auto cs = c.base_stats();
            group.health_total += cs.base_health;
            group.strength_sum += cs.base_strength;
            group.strength_max = std::max(group.strength_max, cs.base_strength);
            group.range_max = std::max(group.range_max, cs.base_attack_range);
            if (c.is_monolith())
                group.monolith = true;
            if (c.is_golem())
                group.golem = true;
            if (!c.is_gem_cluster() && !c.is_gold_cluster())
                group.clusters_only = false;
        }
        group.spawns.clear();
        for (auto p : group.tiles)
            group.spawns.set(p);
        group.centre = vec2{int(sx / long(group.tiles.size())), int(sy / long(group.tiles.size()))};
        group.health_left = group.health_total;
        groups.push_back(std::move(group));
    }
    for (auto &g : groups) {
        for (auto t : g.tiles)
            for (auto d : around8) {
                const vec2 n = t + d;
                if (!inside(n) || terrain.is_set(n) || any_spawn.is_set(n))
                    continue;
                if (std::find(g.edges.begin(), g.edges.end(), n) == g.edges.end())
                    g.edges.push_back(n);
            }
    }
    altars = bc::map::altars().positions();
    altar_edges.resize(altars.size());
    for (std::size_t i = 0; i < altars.size(); ++i)
        for (auto d : around8) {
            const vec2 n = altars[i] + d;
            if (inside(n) && !terrain.is_set(n) && !any_spawn.is_set(n))
                altar_edges[i].push_back(n);
        }
    scratch.clear();
    home_field.compute(terrain, own_spawn, scratch);
    ready = true;
    if constexpr (debug)
        std::printf("INIT type=%d slot=%u groups=%zu altars=%zu dash=%d fuel=%llu\n", int(type), slot, groups.size(),
                    altars.size(), int(has_dash), (unsigned long long)bc::fuel_remaining());
}

void Bot::refresh_equipment() {
    auto &me = bc::self();
    jungle = 1.0f;
    camp_bonus = stats.camp_damage_multiplier;
    const auto inventory = me.item_count();
    for (unsigned i = 0; i < static_cast<unsigned>(Item::count); ++i) {
        const Item item(i);
        if (!inventory[item])
            continue;
        const auto passive = item.passive();
        if (passive == bc::item_passive::jungle)
            jungle = 1 - item.passive_magnitude();
        if (passive == bc::item_passive::monster)
            camp_bonus = stats.camp_damage_multiplier * (1 + item.passive_magnitude());
    }
}

void Bot::observe() {
    auto &me = bc::self();
    round = bc::round_number();
    pos = me.position();
    health = me.health();
    max_health = me.max_health();
    strength = me.strength();
    focus = me.focus();
    gems = me.gem_count();
    gold = me.gold_count();
    at_spawn = own_spawn.is_set(pos);
    moved = attacked = cast = false;
    bc::vision::visible_tiles_into(visible);
    bc::shared_memory.read(0, std::span{&shared, 1});

    enemies.clear();
    for (const auto &c : enemy_roster)
        if (const auto info = sighting(c)) {
            Unit u;
            u.handle = handle_of(c);
            u.pos = vec2{info->x, info->y};
            u.health = info->health;
            u.max_health = info->max_health;
            u.strength = info->strength;
            u.range = info->attack_range;
            u.type = info->type;
            u.state = std::uint32_t(info->state);
            enemies.push_back(u);
        }
    allies.clear();
    for (const auto &c : ally_roster) {
        if (handle_of(c) == handle_of(me))
            continue;
        if (const auto info = sighting(c)) {
            Unit u;
            u.handle = handle_of(c);
            u.pos = vec2{info->x, info->y};
            u.health = info->health;
            u.max_health = info->max_health;
            u.strength = info->strength;
            u.range = info->attack_range;
            u.type = info->type;
            u.state = std::uint32_t(info->state);
            allies.push_back(u);
        }
    }
    bc::vision::awake_camp_tiles_into(awake);
    observe_groups();

    if (alive_spawns_dirty) {
        alive_spawns.clear();
        for (const auto &g : groups)
            if (g.status == Group::alive)
                alive_spawns |= g.spawns;
        alive_spawns_dirty = false;
    }
    blocked = terrain;
    blocked |= alive_spawns;
    bc::vision::champion_tiles_into(scratch);
    blocked |= scratch;
    bc::vision::teleport_tiles_into(scratch);
    blocked |= scratch;
    blocked.set(pos, false);
    if (has_dash) {
        blocked_jump = blocked;
        blocked_jump &= ~holes;
        // Holes beside a hole stay closed: a dash lands two tiles out.
    }
    probe("pre-reach");
    dest.clear();
    dest.set(pos);
    reach.compute(blocked, dest, scratch, 120);
    probe("reach");
    reach_jump_ok = false;
    if (has_dash) {
        reach_jump.compute(blocked_jump, dest, scratch, 120);
        reach_jump_ok = true;
    }
}

void Bot::observe_groups() {
    camps.clear();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        auto &g = groups[i];
        const auto before = g.status;
        g.in_view = false;
        for (auto t : g.tiles)
            if (visible.is_set(t)) {
                g.in_view = true;
                break;
            }
        g.enemies_near = 0;
        for (const auto &e : enemies)
            if (dist(e.pos, g.centre) <= 6)
                ++g.enemies_near;
        if (g.enemies_near) {
            g.enemy_seen = int(round);
            shared.enemy_group = std::uint8_t(std::min<std::size_t>(i + 1, 255));
            shared.enemy_round = std::uint8_t((round / 4) & 0xFF);
        }
        if (!g.in_view) {
            if (g.status == Group::reward && int(round) > g.seen + settings().capture_time + 1) {
                g.status = Group::cooling;
                g.respawn_at = g.seen + settings().capture_time + g.respawn_max;
            }
            if (g.status == Group::cooling && int(round) >= g.respawn_at) {
                g.status = Group::alive;
                g.health_left = g.health_total;
            }
        } else {
            g.seen = int(round);
            int timer = 0;
            const int state = raw_group_state(g.unit, timer);
            if (state == 3) {
                g.status = Group::cooling;
                g.respawn_at = int(round) + std::max(1, timer);
                g.health_left = 0;
            } else if (state == 2) {
                g.status = Group::reward;
                g.health_left = 0;
            } else if (state == 1) {
                g.status = Group::alive;
                if (dist(pos, g.centre) <= 10) {
                    float left = 0;
                    int index = 0;
                    for (const auto &c : g.unit.camps()) {
                        bc::detail::camp_state cs{};
                        if (read_camp(c, cs) && cs.health > 0) {
                            left += cs.health;
                            CampSeen seen{vec2{cs.x, cs.y}, cs.health, c.base_stats().base_strength,
                                          c.base_stats().base_attack_range, int(i), index,
                                          awake.is_set(vec2{cs.x, cs.y})};
                            camps.push_back(seen);
                        } else if (!visible.is_set(c.spawn_position())) {
                            left += c.base_stats().base_health; // unseen member, assume whole
                        }
                        ++index;
                    }
                    g.health_left = left;
                }
            }
        }
        if (g.status != before)
            alive_spawns_dirty = true;
    }
}

void Bot::publish(int claim, bool farming) {
    if (slot < 6)
        shared.claim[slot] = std::uint8_t((claim >= 0 ? std::min(claim + 1, 127) : 0) | (farming ? 0 : 0x80));
    bc::shared_memory.write(0, std::span{&shared, 1});
}

// ---------------------------------------------------------------- navigation

int Bot::walk_to_group(const Group &g) const {
    int best = kUnreachable;
    if (g.status == Group::alive) {
        for (auto e : g.edges)
            best = std::min(best, int(reach.get(e)));
    } else {
        for (auto t : g.tiles)
            best = std::min(best, int(reach.get(t)));
    }
    return best;
}

int Bot::jump_to_group(const Group &g) const {
    if (!reach_jump_ok)
        return kUnreachable;
    int best = kUnreachable;
    if (g.status == Group::alive) {
        for (auto e : g.edges)
            best = std::min(best, int(reach_jump.get(e)));
    } else {
        for (auto t : g.tiles)
            best = std::min(best, int(reach_jump.get(t)));
    }
    return best;
}

int Bot::walk_to_altar(std::size_t i) const {
    int best = kUnreachable;
    for (auto e : altar_edges[i])
        best = std::min(best, int(reach.get(e)));
    return best;
}

bool Bot::dash_toward(const bc::distance_field &field) {
    auto &me = bc::self();
    if (!has_dash || !me.can_cast())
        return false;
    const auto a = me.abilities()[dash_slot];
    if (!is_ready(a))
        return false;
    const int here = field.get(pos);
    int best = here;
    vec2 landing{-1, -1};
    for (auto c : a.valid_tiles().positions()) {
        if (holes.is_set(c) || blocked.is_set(c))
            continue;
        const int d = field.get(c);
        if (d < best) {
            best = d;
            landing = c;
        }
    }
    if (landing.x < 0)
        return false;
    if (a.use(landing)) {
        cast = true;
        moved = true;
        pos = me.position();
        return true;
    }
    return false;
}

bool Bot::go(const std::vector<vec2> &targets, bool allow_jump) {
    auto &me = bc::self();
    if (!me.can_move() || moved)
        return false;
    dest.clear();
    bool any = false;
    for (auto t : targets) {
        if (t == pos)
            return false;
        dest.set(t);
        any = true;
    }
    if (!any)
        return false;
    scratch = blocked;
    for (auto t : targets)
        scratch.set(t, false);
    probe("pre-goal");
    goal.compute(scratch, dest, visible, 200); // visible is scratch here; it is rebuilt next turn
    probe("goal");
    bc::vision::visible_tiles_into(visible);
    const int d = goal.get(pos);
    if (allow_jump && has_dash) {
        scratch = blocked_jump;
        for (auto t : targets)
            scratch.set(t, false);
        bc::distance_field &jump = reach_jump; // reuse the buffer; reach_jump is not read after this turn's choices
        jump.compute(scratch, dest, visible, 200);
        bc::vision::visible_tiles_into(visible);
        reach_jump_ok = false;
        const int dj = jump.get(pos);
        if (dj != kUnreachable && (d == kUnreachable || dj + 6 < d)) {
            const auto delta = jump.next_move_from(pos);
            if (delta) {
                const vec2 next = pos + *delta;
                if (!holes.is_set(next) && !blocked.is_set(next)) {
                    if (me.move(*delta)) {
                        moved = true;
                        pos = me.position();
                        return true;
                    }
                } else if (dash_toward(jump)) {
                    return true;
                } else if (d == kUnreachable) {
                    return true; // wait at the hole's edge for the dash
                }
            }
        }
    }
    if (d == kUnreachable)
        return false;
    const auto delta = goal.next_move_from(pos);
    if (!delta)
        return false;
    if (me.move(*delta)) {
        moved = true;
        pos = me.position();
        return true;
    }
    return false;
}

bool Bot::step_away(const std::vector<vec2> &threats, bool toward_home) {
    auto &me = bc::self();
    if (!me.can_move() || moved || threats.empty())
        return false;
    auto score = [&](vec2 p) {
        int nearest = 99;
        for (auto t : threats)
            nearest = std::min(nearest, dist(p, t));
        float s = float(std::min(nearest, 6)) * 10.0f;
        if (toward_home) {
            const int h = home_field.get(p);
            s -= (h == kUnreachable ? 200.0f : float(h)) * 0.5f;
        }
        return s;
    };
    vec2 best = pos;
    float best_score = score(pos);
    for (auto d : around8) {
        const vec2 n = pos + d;
        if (!inside(n) || blocked.is_set(n))
            continue;
        const float s = score(n);
        if (s > best_score) {
            best_score = s;
            best = n;
        }
    }
    if (best == pos)
        return false;
    if (me.move_absolute(best)) {
        moved = true;
        pos = me.position();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- helpers

bool Bot::is_ready(const bc::ability &a) const { return a.info() && a.cooldown() <= 0; }

float Bot::my_dps() const { return strength * camp_bonus + focus * 0.2f; }

float Bot::camp_incoming(vec2 at) const {
    float total = 0;
    for (const auto &c : camps) {
        if (!c.awake || c.health <= 0 || dist(at, c.pos) > c.range)
            continue;
        // Each camp hits only the nearest champion.
        bool nearest = true;
        const int mine = dist(at, c.pos);
        for (const auto &a : allies)
            if (dist(a.pos, c.pos) < mine)
                nearest = false;
        if (nearest)
            total += c.strength * jungle;
    }
    return total;
}

bool Bot::safe_to_clear(const Group &g, int claimers) const {
    if (g.clusters_only)
        return true;
    if (g.monolith)
        return false;
    const float dps = my_dps() * float(1 + claimers);
    const float turns = g.health_left / std::max(1.0f, dps);
    // Camps in range of a standing tile hit the nearest champion: alone, all
    // of them hit me; in a pack, about half.
    const float share = claimers ? 0.6f : 1.0f;
    const float expected = g.strength_sum * jungle * turns * share * 0.8f;
    return health > expected + 120.0f;
}

bool Bot::altar_locked_by_enemy(vec2 altar) const {
    return visible.is_set(altar) && bc::vision::has_altar_lock(altar) && bc::vision::altar_lock_at(altar).is_enemy();
}

bool Bot::use_attack_reset() {
    auto &me = bc::self();
    if (cast || !me.can_cast() || me.can_attack())
        return false;
    for (const auto &a : me.abilities())
        if (a.is_attack_reset() && is_ready(a)) {
            if (a.use()) {
                cast = true;
                return true;
            }
        }
    return false;
}

bool Bot::use_shield() {
    auto &me = bc::self();
    if (cast || !me.can_cast())
        return false;
    for (const auto &a : me.abilities())
        if (a.is_shield() && is_ready(a)) {
            if (a.use()) {
                cast = true;
                return true;
            }
        }
    return false;
}

// ---------------------------------------------------------------- shop

bool Bot::shop() {
    auto &me = bc::self();
    Item repeat = Item::bruiser_shard_of_strength;
    const auto steps = build_for(type, repeat);
    bool bought = false;
    for (int guard = 0; guard < 12; ++guard) {
        const auto inventory = me.item_count();
        Item next = repeat;
        for (const auto &step : steps) {
            unsigned have = inventory[step.item];
            if (const auto emblem = step.item.result())
                have += 5 * inventory[*emblem];
            if (have < step.count) {
                next = step.item;
                break;
            }
        }
        if (me.gold_count() < next.cost() || !me.can_buy(next, 1))
            break;
        me.buy(next, 1);
        bought = true;
    }
    if (bought) {
        refresh_equipment();
        last_shop_round = int(round);
    }
    return bought;
}

// ---------------------------------------------------------------- fight

void Bot::fight_abilities(const Unit &target) {
    auto &me = bc::self();
    if (cast || !me.can_cast())
        return;
    const bc::champion *handle = nullptr;
    for (const auto &c : enemy_roster)
        if (handle_of(c) == target.handle)
            handle = &c;
    if (!handle)
        return;
    const int d = dist(pos, target.pos);
    for (const auto &a : me.abilities()) {
        if (!is_ready(a) || !a.info())
            continue;
        const auto &info = *a.info();
        const bool exclusive = std::visit([](const auto &p) { return p.exclusive_with_movement; }, info);
        if (exclusive && moved)
            continue;
        if (a.is_cc() && a.can_target_enemies()) {
            const auto &p = std::get<bc::ability_cc>(info);
            if (d <= int(p.range) && a.use(*handle)) {
                cast = true;
                return;
            }
        } else if (a.is_damage() && a.can_target_enemies()) {
            const auto &p = std::get<bc::ability_damage>(info);
            if (d <= int(p.range) && a.use(*handle)) {
                cast = true;
                return;
            }
        } else if (a.is_aoe_damage() && a.can_target_ground()) {
            const auto &p = std::get<bc::ability_aoe_damage>(info);
            if (d <= int(p.range) && a.use(target.pos)) {
                cast = true;
                return;
            }
        } else if (a.is_dash() && melee && d == 2) {
            // Heart of Ruin's landing stuns; a plain dash closes the gap.
            for (auto dd : around8) {
                const vec2 land = target.pos + dd;
                if (inside(land) && dist(land, pos) <= 2 && !blocked.is_set(land) && !holes.is_set(land) &&
                    land != pos) {
                    if (a.use(land)) {
                        cast = true;
                        moved = true;
                        pos = me.position();
                        return;
                    }
                }
            }
        }
    }
}

bool Bot::fight() {
    auto &me = bc::self();
    if (enemies.empty())
        return false;
    int nearest = 99;
    for (const auto &e : enemies)
        nearest = std::min(nearest, dist(pos, e.pos));
    if (nearest > 7)
        return false;
    // Local power: health times damage, ours within five tiles, theirs within six.
    const float mine = health * (strength * 1.3f + focus * 0.2f);
    float ours = mine, theirs = 0;
    for (const auto &a : allies)
        if (dist(a.pos, pos) <= 5 && !a.recalling())
            ours += a.health * (a.strength * 1.3f + 20.0f);
    for (const auto &e : enemies)
        if (dist(e.pos, pos) <= 6)
            theirs += e.health * (e.strength * 1.3f + 20.0f);
    const float need = gems >= kBankBag ? 1.8f : 1.15f;
    const bool engage = theirs > 0 && ours >= need * theirs && health > 0.35f * max_health;
    if constexpr (debug)
        std::printf("FIGHT r=%u slot=%u ours=%.0f theirs=%.0f engage=%d nearest=%d gems=%u\n", round, slot, ours,
                    theirs, int(engage), nearest, gems);
    if (engage) {
        // Focus the weakest enemy in reach; otherwise close on the nearest.
        const Unit *best = nullptr;
        const int range = me.attack_range();
        for (const auto &e : enemies)
            if (dist(pos, e.pos) <= range && (!best || e.health < best->health))
                best = &e;
        if (best) {
            const bc::champion *handle = nullptr;
            for (const auto &c : enemy_roster)
                if (handle_of(c) == best->handle)
                    handle = &c;
            if (handle && me.can_attack()) {
                me.attack(*handle);
                attacked = true;
                if (use_attack_reset() && me.can_attack())
                    me.attack(*handle);
            }
            fight_abilities(*best);
            if (health < 0.7f * max_health)
                use_shield();
            return true;
        }
        const Unit *near = nullptr;
        for (const auto &e : enemies)
            if (!near || dist(pos, e.pos) < dist(pos, near->pos))
                near = &e;
        if (near) {
            fight_abilities(*near);
            if (!moved && melee)
                go_tile(near->pos, false);
            else if (!moved && !melee && dist(pos, near->pos) > range)
                go_tile(near->pos, false);
            return true;
        }
        return false;
    }
    // Not engaging: leave when they can reach us soon.
    bool threatened = false;
    std::vector<vec2> threats;
    for (const auto &e : enemies) {
        threats.push_back(e.pos);
        if (dist(pos, e.pos) <= e.range + 2)
            threatened = true;
    }
    if (!threatened)
        return false;
    // A melee champion hits what stands beside it before stepping off.
    if (melee && me.can_attack()) {
        for (const auto &e : enemies)
            if (dist(pos, e.pos) <= 1) {
                for (const auto &c : enemy_roster)
                    if (handle_of(c) == e.handle) {
                        me.attack(c);
                        attacked = true;
                    }
                break;
            }
    }
    if (health < 0.7f * max_health)
        use_shield();
    step_away(threats, true);
    return true;
}

bool Bot::survive() {
    auto &me = bc::self();
    const float incoming = camp_incoming(pos);
    const bool low = health < 0.3f * max_health;
    std::vector<vec2> camp_threats;
    for (const auto &c : camps)
        if (c.awake && c.health > 0 && dist(pos, c.pos) <= c.range)
            camp_threats.push_back(c.pos);
    int enemy_near = 99;
    for (const auto &e : enemies)
        enemy_near = std::min(enemy_near, dist(pos, e.pos));
    if (low) {
        if (enemy_near <= 8) {
            std::vector<vec2> threats;
            for (const auto &e : enemies)
                threats.push_back(e.pos);
            if (health < 0.7f * max_health)
                use_shield();
            return step_away(threats, true);
        }
        if (incoming > 0) {
            use_shield();
            if (step_away(camp_threats, true))
                return true;
        }
        if (!at_spawn && me.can_move() && !moved) {
            me.recall();
            return true;
        }
        return false;
    }
    // Two more camp turns would leave less than a fifth: step out of range.
    if (incoming > 0 && health - incoming * 2.5f < 0.2f * max_health) {
        use_shield();
        return step_away(camp_threats, true);
    }
    return false;
}

// ---------------------------------------------------------------- bank

bool Bot::bank() {
    auto &me = bc::self();
    if (gems == 0) {
        banking = false;
        return false;
    }
    const int remaining = std::max(1, int(settings().winning_gems) - int(bc::global_submitted_gems(bc::team::ally)));
    const bool win = int(gems) >= remaining;
    int best = -1, best_d = kUnreachable;
    float best_value = 1e9f;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const int d = walk_to_altar(i);
        if (d == kUnreachable)
            continue;
        float value = float(d);
        if (altar_locked_by_enemy(altars[i]))
            value += 8;
        for (const auto &e : enemies)
            if (dist(e.pos, altars[i]) <= 5)
                value += 6;
        if (value < best_value) {
            best_value = value;
            best = int(i);
            best_d = d;
        }
    }
    if (best < 0) {
        banking = false;
        if (gems >= kBankBag && !at_spawn && enemies.empty() && camp_incoming(pos) == 0 && me.can_move() && !moved) {
            me.recall(); // walled in: bank from spawn
            return true;
        }
        return false;
    }
    const bool deadline = int(settings().max_rounds - round) <= best_d + channel_turns(gems) + 6;
    const bool nearby = gems >= 10 && best_d <= 2;
    const bool trip = banking || win || deadline || gems >= kBankBag || nearby || (at_spawn && gems >= 10);
    if (!trip)
        return false;
    banking = true;
    bank_altar = best;
    const vec2 altar = altars[best];
    if (dist(pos, altar) <= settings().submit_gems_range) {
        if (me.buffs().has_stun())
            return true;
        if (altar_locked_by_enemy(altar)) {
            if (me.can_attack() && dist(pos, altar) <= me.attack_range()) {
                me.attack(bc::vision::altar_lock_at(altar));
                attacked = true;
                if (use_attack_reset() && me.can_attack() && bc::vision::has_altar_lock(altar))
                    me.attack(bc::vision::altar_lock_at(altar));
            } else if (!moved) {
                go(altar_edges[best], true);
            }
            return true;
        }
        me.submit_gems(altar);
        return true;
    }
    if (!go(altar_edges[best], true))
        ++stuck_turns;
    return true;
}

// ---------------------------------------------------------------- farm

int Bot::choose_target() {
    int best = -1;
    float best_value = 0;
    // Claims by allies (their shared-memory slot, not mine).
    std::array<int, 128> claimers{};
    for (unsigned i = 0; i < 6; ++i) {
        if (i == slot)
            continue;
        const std::uint8_t c = shared.claim[i];
        if (c && !(c & 0x80))
            ++claimers[c & 0x7F];
    }
    int enemies_visible = int(enemies.size());
    int pack = 1;
    for (const auto &a : allies)
        if (dist(a.pos, pos) <= 6)
            ++pack;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto &g = groups[i];
        if (g.monolith || int(round) < g.unreachable_until)
            continue;
        const int others = i + 1 < 128 ? claimers[i + 1] : 0;
        int d = walk_to_group(g);
        bool via_jump = false;
        if (has_dash) {
            const int dj = jump_to_group(g);
            if (dj != kUnreachable && (d == kUnreachable || dj + 6 < d)) {
                d = dj + 4;
                via_jump = true;
            }
        }
        if (d == kUnreachable)
            continue;
        const float reward = float(g.gems) * kGemGold + float(g.gold);
        float time = 0;
        const float dps = my_dps() * float(1 + others);
        if (g.status == Group::alive) {
            if (int(round) < g.unsafe_until && others == 0)
                continue;
            time = float(d) + g.health_left / std::max(1.0f, dps) + float(settings().capture_time);
        } else if (g.status == Group::reward) {
            time = float(d) + float(settings().capture_time);
        } else {
            const int wait = g.respawn_at - int(round);
            if (wait > 60)
                continue;
            time = float(std::max(d, wait)) + g.health_total / std::max(1.0f, dps) + float(settings().capture_time);
        }
        float value = reward / float(1 + others) / std::max(1.0f, time);
        if (others > 0)
            value *= g.gems >= 15 ? 1.3f : 0.5f;
        if (int(round) - g.enemy_seen <= kEnemyMemory)
            value *= pack >= std::max(1, g.enemies_near) ? 0.8f : 0.35f;
        if (g.golem && others == 0 && max_health < 1500)
            value *= 0.4f;
        if (int(i) == target)
            value *= 1.25f;
        if (via_jump)
            value *= 0.9f;
        if (value > best_value) {
            best_value = value;
            best = int(i);
        }
    }
    if constexpr (debug)
        if (best != target)
            std::printf("TARGET r=%u slot=%u group=%d value=%.2f\n", round, slot, best, best_value);
    return best;
}

bool Bot::attack_camp(const Group &g) {
    auto &me = bc::self();
    if (!me.can_attack() || attacked)
        return false;
    const int range = me.attack_range();
    const CampSeen *focus = nullptr;
    for (const auto &c : camps) {
        if (c.group != int(&g - groups.data()) || c.health <= 0 || dist(pos, c.pos) > range)
            continue;
        if (!focus || c.health < focus->health)
            focus = &c;
    }
    if (!focus)
        return false;
    const auto camp_list = g.unit.camps();
    me.attack(camp_list[focus->index]);
    attacked = true;
    if (use_attack_reset() && me.can_attack())
        me.attack(camp_list[focus->index]);
    camp_abilities(g, focus);
    return true;
}

void Bot::camp_abilities(const Group &g, const CampSeen *focus) {
    auto &me = bc::self();
    if (cast || !me.can_cast())
        return;
    const auto camp_list = g.unit.camps();
    for (const auto &a : me.abilities()) {
        if (!is_ready(a) || !a.info())
            continue;
        const auto &info = *a.info();
        const bool exclusive = std::visit([](const auto &p) { return p.exclusive_with_movement; }, info);
        if (exclusive && moved)
            continue;
        if ((a.is_damage() || a.is_cc()) && a.can_target_camps() && focus) {
            const float range = a.is_damage() ? std::get<bc::ability_damage>(info).range
                                              : std::get<bc::ability_cc>(info).range;
            if (dist(pos, focus->pos) <= int(range) && focus->health > strength * 0.5f) {
                if (a.use(camp_list[focus->index])) {
                    cast = true;
                    return;
                }
            }
        } else if (a.is_aoe_damage() && a.can_target_ground()) {
            const auto &p = std::get<bc::ability_aoe_damage>(info);
            // The tile hitting most living camps of this group.
            vec2 best{-1, -1};
            int best_hits = 0;
            for (const auto &c : camps) {
                if (c.group != focus->group || c.health <= 0 || dist(pos, c.pos) > int(p.range))
                    continue;
                int hits = 0;
                for (const auto &o : camps)
                    if (o.group == c.group && o.health > 0 && dist(o.pos, c.pos) <= int(p.radius))
                        ++hits;
                if (hits > best_hits) {
                    best_hits = hits;
                    best = c.pos;
                }
            }
            if (best.x >= 0 && a.use(best)) {
                cast = true;
                return;
            }
        } else if (a.is_dash() && type == bc::champion_type::heart_of_ruin && !moved) {
            // Land beside two or more camps: damage and a stun on each.
            vec2 best{-1, -1};
            int best_hits = 1;
            for (auto e : g.edges) {
                if (dist(e, pos) > 2 || e == pos || blocked.is_set(e))
                    continue;
                int hits = 0;
                for (const auto &c : camps)
                    if (c.group == focus->group && c.health > 0 && dist(c.pos, e) <= 1)
                        ++hits;
                if (hits > best_hits) {
                    best_hits = hits;
                    best = e;
                }
            }
            if (best.x >= 0 && a.use(best)) {
                cast = true;
                moved = true;
                pos = me.position();
                return;
            }
        }
    }
    // Shield against the camps once they are awake on us.
    if (camp_incoming(pos) > 0 && health < 0.8f * max_health)
        use_shield();
}

bool Bot::farm() {
    auto &me = bc::self();
    if (target < 0)
        return false;
    auto &g = groups[target];
    if (g.status == Group::reward) {
        if (g.spawns.is_set(pos))
            return true; // hold the capture
        return go(g.tiles, true) || true;
    }
    if (g.status == Group::cooling) {
        if (walk_to_group(g) > 2)
            go(g.edges, true);
        return true;
    }
    // Alive: a lone champion checks the camps' damage against its health first.
    const int d = walk_to_group(g);
    if (d <= 1) {
        int others = 0;
        for (const auto &a : allies)
            if (dist(a.pos, g.centre) <= 5)
                ++others;
        if (!safe_to_clear(g, others) && camp_incoming(pos) == 0) {
            g.unsafe_until = int(round) + 40;
            target = -1;
            return false;
        }
    }
    bool in_range = false;
    const int range = me.attack_range();
    for (const auto &c : camps)
        if (c.group == target && c.health > 0 && dist(pos, c.pos) <= range)
            in_range = true;
    if (melee) {
        if (in_range) {
            attack_camp(g);
        } else {
            if (!go(g.edges, true))
                ++stuck_turns;
            // Fresh vision after the step may put a camp in reach.
            attack_camp(g);
        }
    } else {
        if (in_range)
            attack_camp(g);
        else if (!go(g.edges, true))
            ++stuck_turns;
    }
    return true;
}

void Bot::place_ward() {
    auto &me = bc::self();
    if (int(round) - ward_round < 12 || me.ward_cooldown() > 0 || at_spawn)
        return;
    if (target < 0 || walk_to_group(groups[target]) > 3)
        return;
    vec2 best{-1, -1};
    int best_score = -1;
    for (auto t : me.ward_placement_tiles().positions()) {
        const int score = dist(t, home) - dist(t, enemy_home);
        if (score > best_score) {
            best_score = score;
            best = t;
        }
    }
    if (best.x >= 0 && me.place_ward(best))
        ward_round = int(round);
}

// ---------------------------------------------------------------- turn

void Bot::play_turn() {
    auto &me = bc::self();
    if (!ready) {
        initialise();
        refresh_equipment();
    }
    if (!me.is_alive()) {
        shop();
        round = bc::round_number();
        bc::shared_memory.read(0, std::span{&shared, 1});
        publish(-1, false);
        target = -1;
        banking = false;
        return;
    }
    probe("start");
    observe();
    probe("observed");
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) {
        publish(-1, false);
        return;
    }
    if (at_spawn)
        shop();
    if (pos == last_pos)
        ++stuck_turns;
    else
        stuck_turns = 0;
    last_pos = pos;
    if (stuck_turns >= 8 && target >= 0) {
        groups[target].unreachable_until = int(round) + 40;
        target = -1;
        stuck_turns = 0;
    }

    if (fight()) {
        publish(target, false);
        return;
    }
    if (survive()) {
        publish(target, false);
        return;
    }
    if (bank()) {
        publish(target, false);
        return;
    }
    // Home for the shop with a full purse and an empty bag.
    if (!at_spawn && gold >= kShopGold && gems < 10 && enemies.empty() && camp_incoming(pos) == 0 && me.can_move() &&
        int(round) - last_shop_round > 60) {
        me.recall();
        publish(-1, false);
        return;
    }
    target = choose_target();
    if (!farm()) {
        target = choose_target();
        farm();
    }
    place_ward();
    publish(target, true);
    probe("end");
    if constexpr (debug)
        std::printf("TURN r=%u slot=%u pos=%d,%d hp=%.0f gems=%u gold=%u target=%d fuel=%llu\n", round, slot, pos.x,
                    pos.y, health, gems, gold, target, (unsigned long long)bc::fuel_remaining());
}
