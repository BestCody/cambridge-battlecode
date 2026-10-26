// Equipment queues and wards.
#include "../bot.h"

namespace {
using Item = bc::item_type;
// willow best girl's purchase order (their 40 ranked games, 2026-10-24): each
// emblem in the order of its median first shard, kept when most games buy it
// (comments: median rounds of the first and fifth shard, share of games).
// The first emblem is the Health Treasure for a champion enemy champions have
// hurt (policy::health_treasure_damage). Whole emblems, since only a finished
// emblem gives its passive. Every shard
// adds one growth of its stat; five identical shards combine into an emblem
// automatically.
struct BuildStep {
    Item item;
    unsigned count;
};
struct Build {
    std::array<BuildStep, 7> steps;
    unsigned size;
    Item repeat;
};
Build build_for(bc::champion_type type, bool hurt) {
    switch (type) {
        case bc::champion_type::vesper_vale:
            return {{{{Item::treasure_shard_of_strength, 5},  // r82-128, 85%
                      {Item::power_shard_of_strength, 5},     // r194-293, 65%
                      {Item::jungle_shard_of_strength, 5},    // r237-286, 98%
                      {Item::durability_shard_of_health, 5},  // r338-408, 82%
                      {Item::bruiser_shard_of_strength, 10},  // r374-418, 85%; 31 a game
                      {Item::bruiser_shard_of_health, 5}}},   // r564-608, 57%
                    6,
                    Item::bruiser_shard_of_strength};
        case bc::champion_type::heart_of_ruin:
            return {{{{hurt ? Item::treasure_shard_of_health : Item::treasure_shard_of_focus, 5}, // r98-171
                      {Item::jungle_shard_of_focus, 5},       // r306-365, 75%
                      {Item::bruiser_shard_of_health, 5},     // r370-428, 68%
                      {Item::bruiser_shard_of_focus, 5},      // r394-494, 68%; 21 a game
                      {Item::durability_shard_of_health, 5}}}, // r662-675, 50%
                    5,
                    Item::bruiser_shard_of_focus};
        case bc::champion_type::marcella:
            return {{{{hurt ? Item::treasure_shard_of_health : Item::treasure_shard_of_focus, 5}, // r77-126
                      // Strength: willow best girl's Marcella hits 62-93 with basic attacks at
                      // r200-600 against the same opponent (ours 45-50) at flat spell damage.
                      {Item::jungle_shard_of_strength, 5},    // r258-390, 31% (Focus 57%)
                      {Item::bruiser_shard_of_health, 5},     // r317-383, 77%
                      {Item::bruiser_shard_of_focus, 5},      // r329-380, 57%; 22 a game
                      {Item::durability_shard_of_health, 5},  // r450-520, 77%
                      {Item::bruiser_shard_of_strength, 5}}}, // r405-488, 40%
                    6,
                    Item::bruiser_shard_of_focus};
        case bc::champion_type::prism:
            return {{{{Item::monster_shard_of_focus, 5},      // r70-183, 70%
                      {Item::jungle_shard_of_health, 5},      // r206-339, 90%
                      {Item::bruiser_shard_of_focus, 5},      // r289-476, 65%; 17 a game
                      {Item::home_shard_of_health, 5},        // r408-609, 50%
                      {Item::durability_shard_of_health, 5}}}, // r650-697, 50%
                    5,
                    Item::bruiser_shard_of_focus};
        case bc::champion_type::willow:
            return {{{{hurt ? Item::treasure_shard_of_health : Item::treasure_shard_of_strength, 5}, // r223-309
                      {Item::jungle_shard_of_strength, 5}}},  // r412-607, 46%
                    2,
                    Item::power_shard_of_strength};           // 4 a game
        default: // Not drafted.
            return {{}, 0, Item::power_shard_of_strength};
    }
}
using Bought = std::array<unsigned, static_cast<unsigned>(Item::count)>;
// Every sixth item is a family's Health shard.
bool health_shard(Item item) { return static_cast<unsigned>(item) % 6 == 4; }
// The next emblem of the build order, or, when pressed (enemy champions hurt
// us lately), its next Health one: willow best girl starts a Health emblem in
// 30-31% of new emblems after under 150 champion damage in the 150 rounds
// before, 51-59% after 500-799 and 71-77% after 1200+ (both samples); its
// Vesper took Durability-Health after its Treasure and Jungle Strength, its
// Prism Jungle-Health before Monster-Focus in 16 of 40 games. An emblem begun
// is finished first: only a finished one gives its passive.
Item next_item(bc::champion_type type, bool hurt, bool pressed, const Bought &bought) {
    const auto build = build_for(type, hurt);
    const auto left = [&](unsigned i) { return bought[static_cast<unsigned>(build.steps[i].item)] < build.steps[i].count; };
    for (unsigned i = 0; i < build.size; ++i)
        if (left(i) && bought[static_cast<unsigned>(build.steps[i].item)] > 0)
            return build.steps[i].item;
    unsigned first = 0;
    while (first < build.size && !left(first))
        ++first;
    if (first == build.size)
        return build.repeat;
    const Item item = build.steps[first].item;
    if (!pressed || health_shard(item))
        return item;
    for (unsigned i = first + 1; i < build.size; ++i)
        if (left(i) && health_shard(build.steps[i].item))
            return build.steps[i].item;
    return item;
}
unsigned step_left(bc::champion_type type, bool hurt, const Bought &bought, Item item) {
    const auto build = build_for(type, hurt);
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
    recall_turns = rules::settings().recall_time;
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
        if (passive == bc::item_passive::home)
            recall_turns = (rules::settings().recall_time + 1) / 2;
    }
}

void Bot::buy_equipment() {
    profiling::Scope scope(profiling::Stage::buy);
    auto &me = bc::self();
    // They sell only at spawn or while dead, so elsewhere the purchase loop
    // could only fail. An altar lock first when an altar wants one
    // (lock_wanted()), as willow best girl buys them (about 8 a game).
    const bool can_shop = !me.is_alive() || bc::map::spawn_region(bc::team::ally).is_set(me.position());
    if (can_shop && me.is_alive()) {
        carrying_lock = me.consumables()[bc::consumable_type::altar_lock].count() > 0;
        bool wanted = false;
        for (std::size_t i = 0; i < altars.size(); ++i)
            wanted = wanted || lock_wanted(i);
        if (!carrying_lock && wanted && me.gold_count() >= bc::consumable_type::altar_lock.cost())
            carrying_lock = bc::detail::sdk_buy_consumable(bc::detail::raw_consumable_ids[1]) == 0;
    }
    // The Treasure stat is chosen at its first shard and kept.
    const auto shards = [&](Item item) { return bought[static_cast<unsigned>(item)]; };
    const bool hurt = shards(Item::treasure_shard_of_health) > 0 ||
                      (!shards(Item::treasure_shard_of_focus) && !shards(Item::treasure_shard_of_strength) &&
                       champion_damage >= policy::health_treasure_damage);
    // Champion damage over the last 100-150 rounds, from marks every 50.
    if (int(round) >= damage_mark_round + 50) {
        damage_marks = {damage_marks[1], damage_marks[2], champion_damage};
        damage_mark_round = int(round);
    }
    const bool pressed = champion_damage - damage_marks[0] >= policy::health_pull_damage;
    while (can_shop) {
        const auto item = next_item(type, hurt, pressed, bought);
        if (me.gold_count() < item.cost())
            break;
        const unsigned count = std::min(me.gold_count() / item.cost(), step_left(type, hurt, bought, item));
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
    // The stun is read again here: one read as absent at the start of this
    // turn's ward choice was present at the placement, which threw
    // ("champion cannot act") and lost the rest of the turn.
    if (value > -1e19f && !me.buffs().has_stun() &&
        (best != ward || distance(position, ward) > int(rules::settings().ward_vision_radius) / 2))
        if (me.place_ward(best))
            ward = best;
}
