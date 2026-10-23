// Equipment queues and wards.
#include "../bot.h"

namespace {
using Item = bc::item_type;
// The top team's purchase order: a few whole emblems early (only a finished
// emblem gives its passive; one shard of each instead lost 150 to 121 in 210
// games, with 5.5 more deaths a game), then one item bought indefinitely.
// Every shard adds one growth of its stat; five identical shards combine into
// an emblem automatically (a power emblem counts as six).
struct BuildStep {
    Item item;
    unsigned count;
};
struct Build {
    std::array<BuildStep, 4> steps;
    unsigned size;
    Item repeat;
};
Build build_for(bc::champion_type type) {
    switch (type) {
        case bc::champion_type::vesper_vale: // The top team's 1.0.3 builds.
            return {{{{Item::treasure_shard_of_strength, 5},
                      {Item::bruiser_shard_of_strength, 5},
                      {Item::durability_shard_of_health, 5}}},
                    3,
                    Item::power_shard_of_strength};
        case bc::champion_type::vendetta:
            return {{{{Item::treasure_shard_of_health, 5},
                      {Item::jungle_shard_of_strength, 5},
                      {Item::bruiser_shard_of_health, 5},
                      {Item::durability_shard_of_health, 5}}},
                    4,
                    Item::bruiser_shard_of_strength};
        case bc::champion_type::heart_of_ruin:
            return {{{{Item::treasure_shard_of_health, 5}, {Item::jungle_shard_of_focus, 5}}},
                    2,
                    Item::bruiser_shard_of_health};
        case bc::champion_type::ryth:
            return {{{{Item::bruiser_shard_of_health, 1}, {Item::durability_shard_of_health, 1}}},
                    2,
                    Item::power_shard_of_strength};
        case bc::champion_type::marcella: // The ladder teams' common build.
        case bc::champion_type::willow:
            return {{{{Item::jungle_shard_of_health, 5},
                      {Item::treasure_shard_of_strength, 5},
                      {Item::durability_shard_of_health, 5},
                      {Item::bruiser_shard_of_strength, 5}}},
                    4,
                    Item::power_shard_of_strength};
        default: // Not drafted.
            return {{}, 0, Item::power_shard_of_strength};
    }
}
using Bought = std::array<unsigned, static_cast<unsigned>(Item::count)>;
Item next_item(bc::champion_type type, const Bought &bought) {
    const auto build = build_for(type);
    for (unsigned i = 0; i < build.size; ++i)
        if (bought[static_cast<unsigned>(build.steps[i].item)] < build.steps[i].count)
            return build.steps[i].item;
    return build.repeat;
}
unsigned step_left(bc::champion_type type, const Bought &bought, Item item) {
    const auto build = build_for(type);
    for (unsigned i = 0; i < build.size; ++i)
        if (build.steps[i].item == item && bought[static_cast<unsigned>(item)] < build.steps[i].count)
            return build.steps[i].count - bought[static_cast<unsigned>(item)];
    return ~0u; // The repeating item: as many as the gold allows.
}
} // namespace

void Bot::refresh_equipment() {
    mitigation = camp_mitigation = 1;
    camp_multiplier = stats.camp_damage_multiplier;
    // Capture gold, by Vesper's passive (measured 1.327) and a finished
    // Treasure emblem (1.200); the bonuses add.
    gold_bonus = 1 + (passive == bc::passive_type::bonus_gold
                          ? bc::constants::passive_abilities::vesper_vale_bonus_gold_percent / 100.0f
                          : 0.0f);
    const auto inventory = bc::self().item_count();
    for (unsigned i = 0; i < static_cast<unsigned>(Item::count); ++i) {
        const Item item(i);
        if (!inventory[item])
            continue;
        const auto passive = item.passive();
        if (passive == bc::item_passive::durability)
            mitigation = 1 - item.passive_magnitude();
        if (passive == bc::item_passive::jungle)
            camp_mitigation = 1 - item.passive_magnitude();
        if (passive == bc::item_passive::monster)
            camp_multiplier = stats.camp_damage_multiplier * (1 + item.passive_magnitude());
        if (passive == bc::item_passive::treasure)
            gold_bonus += item.passive_magnitude();
    }
}

void Bot::buy_equipment() {
    profiling::Scope scope(profiling::Stage::buy);
    auto &me = bc::self();
    // Shards only: the top team never buys altar locks. They sell only at spawn
    // or while dead, so elsewhere the purchase loop could only fail.
    const bool can_shop = !me.is_alive() || bc::map::spawn_region(bc::team::ally).is_set(me.position());
    while (can_shop) {
        const auto item = next_item(type, bought);
        if (me.gold_count() < item.cost())
            break;
        const unsigned count = std::min(me.gold_count() / item.cost(), step_left(type, bought, item));
        if (!count || !me.can_buy(item, count))
            break;
        me.buy(item, count);
        bought[static_cast<unsigned>(item)] += count;
        equipment_dirty = true;
    }
    // Item stats change only when items do.
    if (equipment_dirty) {
        refresh_equipment();
        equipment_dirty = false;
    }
    if (me.is_alive())
        refresh_self();
}

void Bot::ward_ahead(vec2 goal) {
    profiling::Scope scope(profiling::Stage::ward);
    auto &me = bc::self();
    if (me.is_teleporting() || me.is_recalling() || me.is_submitting_gems() || me.buffs().has_stun())
        return;
    if (me.ward_cooldown() > 0 || me.restrictions().contains(bc::buff_restrictions::ward))
        return;
    vec2 best = position;
    float value = -1e20f;
    // Placement tiles not blocked or holding an allied ward, built in place.
    auto &placements = step_tiles;
    me.ward_placement_tiles_into(placements);
    bc::vision::ward_tiles_into(bc::team::ally, occupancy);
    occupancy |= blocked;
    occupancy &= placements;
    placements ^= occupancy;
    // Unseen placement tiles, from one packed export rather than a query each.
    subtract_into(search_goals, placements, visible);
    unseen_tile.load(search_goals);
    // Put vision into the next brush/approach instead of repeatedly scanning all
    // hypothetical fields of view (which would consume the tactical budget).
    for (auto p : placements.positions()) {
        // Allied wards are not shared through vision. Partition placement
        // tiles by stable team slot so two allies can never reserve one tile.
        if (unsigned(p.x + p.y) % roster.size() != slot)
            continue;
        float score = -distance(p, goal) * 3 + distance(p, position) * 0.3f;
        if (bush_tile[tile_index(p)])
            score += 2;
        if (unseen_tile[tile_index(p)])
            score += 1;
        if (score > value) {
            value = score;
            best = p;
        }
    }
    if (value > -1e19f &&
        (best != ward || distance(position, ward) > int(rules::settings().ward_vision_radius) / 2))
        if (me.place_ward(best))
            ward = best;
}
