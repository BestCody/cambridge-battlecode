// Equipment queues and wards.
#include "../bot.h"

namespace {
using Item = bc::item_type;
// print dont int's purchase order (16 ranked games, engine 2.0.0,
// 2026-10-29): each emblem in the order of its median first shard, kept
// when about two in five of the games finish it (comments: median rounds of
// the first and fifth shard, games finished), and the shard bought most
// repeated after. Whole emblems, since only a finished emblem gives its passive. Every shard adds
// one growth of its stat; five identical shards combine into an emblem
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
Build full_build_for(bc::champion_type type);
Build build_for(bc::champion_type type) {
    Build b = full_build_for(type);
    if constexpr (policy::capture_first) {
        // Capture of the repeat's stat first (strategy.h); the last step
        // makes room when the build is full.
        const Item r = b.repeat;
        const Item cap = r.strength_points() > r.focus_points() && r.strength_points() > r.health_points()
                             ? Item::capture_shard_of_strength
                         : r.focus_points() > r.health_points() ? Item::capture_shard_of_focus
                                                                : Item::capture_shard_of_health;
        bool present = false;
        for (unsigned i = 0; i < b.size; ++i)
            present = present || b.steps[i].item.passive() == bc::item_passive::capture;
        if (b.size > 0 && b.steps[0].item.passive() != bc::item_passive::capture) {
            if (present) {
                // Move the build's own Capture step to the front.
                unsigned at = 0;
                for (unsigned i = 0; i < b.size; ++i)
                    if (b.steps[i].item.passive() == bc::item_passive::capture)
                        at = i;
                const BuildStep step = b.steps[at];
                for (unsigned i = at; i > 0; --i)
                    b.steps[i] = b.steps[i - 1];
                b.steps[0] = step;
            } else {
                if (b.size == b.steps.size())
                    --b.size;
                for (unsigned i = b.size; i > 0; --i)
                    b.steps[i] = b.steps[i - 1];
                b.steps[0] = {cap, 5};
                ++b.size;
            }
        }
    }
    return b;
}
Build full_build_for(bc::champion_type type) {
    switch (type) {
        case bc::champion_type::vesper_vale:
            return {{{{Item::treasure_shard_of_strength, 5},        // r114-129, 15/15
                      {Item::power_shard_of_strength, 5},       // r129-283, 15/15
                      {Item::durability_shard_of_health, 5},    // r301-283, 13/15
                      {Item::prospector_shard_of_strength, 5},  // r333-430, 9/15
                      {Item::monster_shard_of_strength, 5},     // r378-378, 13/15
                      {Item::jungle_shard_of_health, 5},        // r430-416, 12/15
                      {Item::skirmish_shard_of_strength, 5}}},  // r438-498, 10/15
                    7,
                    Item::bruiser_shard_of_strength};           // 38 a game
        case bc::champion_type::heart_of_ruin:
            return {{{{Item::treasure_shard_of_focus, 5},           // r159-318, 15/15
                      {Item::power_shard_of_focus, 5},          // r379-532, 14/15
                      {Item::home_shard_of_health, 5},          // r527-570, 10/15
                      {Item::durability_shard_of_health, 5},    // r629-710, 8/15
                      {Item::prospector_shard_of_focus, 5},     // r710-890, 6/15
                      {Item::monster_shard_of_focus, 5}}},      // r890-972, 7/15
                    6,
                    Item::bruiser_shard_of_focus};              // 3.7 a game
        case bc::champion_type::vendetta:
            return {{{{Item::treasure_shard_of_strength, 5},        // r140-402, 12/14
                      {Item::durability_shard_of_health, 5},    // r468-648, 10/14
                      {Item::jungle_shard_of_health, 5},        // r682-796, 8/14
                      {Item::monster_shard_of_strength, 5},     // r716-753, 6/14
                      {Item::capture_shard_of_strength, 5}}},   // r826-826, 6/14
                    5,
                    Item::bruiser_shard_of_strength};           // 5.2 a game
        case bc::champion_type::marcella:
            // Treasure, Durability, then Bruiser/Focus: print dont int's first two,
            // then Olympus's stacking (its seven farming emblems lost to that in practice).
                return {{{{Item::treasure_shard_of_focus, 5},     // print dont int's first two, then
                          {Item::durability_shard_of_health, 5}}}, // Olympus's stacking
                        2,
                        Item::bruiser_shard_of_focus};
        case bc::champion_type::ryth:
            return {{{{Item::treasure_shard_of_health, 5},      // r140-366, 6/8
                      {Item::durability_shard_of_health, 5}}},  // r506-789, 4/8
                    2,
                    Item::monster_shard_of_strength};           // 1.9 a game
        default: // Not drafted.
            return {{}, 0, Item::power_shard_of_strength};
    }
}
using Bought = std::array<unsigned, static_cast<unsigned>(Item::count)>;
} // namespace

float build_gain_per_gold(bc::champion_type type, const bc::champion_stats &design, const std::array<Spell, 2> &spells) {
    // A champion's fighting power (unit_power(), as the fight weighs one)
    // from its design stats, then with every shard of its build added (each
    // shard adds its stat points times the champion's growth), per gold.
    float health = design.base_health, strength = design.base_strength, focus = design.base_focus;
    const auto power = [&]() {
        float burst = 0;
        for (const auto &s : spells)
            if (s.kind == bc::ability_kind::attack_reset)
                burst = std::max(burst, strength);
            else if (s.modifier > 0 && s.kind != bc::ability_kind::shield && s.kind != bc::ability_kind::heal)
                burst = std::max(burst, focus * s.modifier);
        return unit_power(health, spell_dps(spells, strength, focus), burst, 0, 1);
    };
    const float before = power();
    const auto build = build_for(type);
    float gold = 0;
    for (unsigned i = 0; i < build.size; ++i) {
        const Item item = build.steps[i].item;
        const float n = float(build.steps[i].count);
        health += n * float(item.health_points()) * design.health_growth;
        strength += n * float(item.strength_points()) * design.strength_growth;
        focus += n * float(item.focus_points()) * design.focus_growth;
        gold += n * float(item.cost());
    }
    return gold > 0 && before > 0 ? (power() / before - 1) / gold : 0.0f;
}

namespace {
// The next emblem of the build order, in its order whatever the damage
// taken: switching to a Health emblem when hurt (willow best girl's habit)
// put our Marcella's first 300 gold into Health, her spell at 120 to round
// 900 in 15 of 100 ranked games (13 lost); without the switches 95/140
// against 86 (paired 16:7, 4M fuel). An emblem begun is finished first:
// only a finished one gives its passive.
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
    // Treasure emblem (1.100 since engine 2.0.0); the bonuses add.
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
    // Shopping neither kills nor revives: life is read once.
    const bool alive = me.is_alive();
    const bool can_shop = !alive || own_spawn.is_set(me.position());
    // Income (hop_threshold()): what the gold and the gems carried grew by
    // since our last turn's shopping (only shopping and banking spend them).
    {
        const unsigned gold = me.gold_count(), carried = me.gem_count();
        earned_gold += float(gold > gold_seen ? gold - gold_seen : 0);
        earned_gems += float(carried > gems_seen ? carried - gems_seen : 0);
        gems_seen = carried;
    }
    if (can_shop && alive) {
        carrying_lock = me.consumables()[bc::consumable_type::altar_lock].count() > 0;
        bool wanted = false;
        for (std::size_t i = 0; i < altars.size(); ++i)
            wanted = wanted || lock_wanted(i);
        if (!carrying_lock && wanted && me.gold_count() >= bc::consumable_type::altar_lock.cost())
            carrying_lock = bc::detail::sdk_buy_consumable(bc::detail::raw_consumable_ids[1]) == 0;
    }
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
    // A teleport crystal from what the shards leave, on a map where a hop
    // from spawn saves walking (prepare_hops(); whether to hop is decided on
    // each way, move_to()): bought before the shards, it took the first
    // shard's gold at rounds 18-38 and delayed it a trip; bought on every
    // map, its 10 gold split games from the crystal-free ones where no hop
    // ever paid.
    // Or where a leg out of the group we farm (DraftPlan::leg_saving) saves
    // more rounds than a hop costs: walls between camp areas that no hop
    // from spawn crosses.
    const bool leg_crystal = target >= 0 && draft_measured() &&
                             float(opening.leg_rounds(std::size_t(target))) > hop_threshold();
    if (can_shop && alive && (hops_useful || leg_crystal) && !carrying_crystal() &&
        me.gold_count() >= bc::consumable_type::tp_crystal.cost())
        bc::detail::sdk_buy_consumable(bc::detail::raw_consumable_ids[0]);
    // Item stats change only when items do.
    if (equipment_dirty) {
        refresh_equipment();
        equipment_dirty = false;
    }
    gold_seen = me.gold_count();
    if (alive)
        refresh_self();
}

void Bot::ward_ahead(vec2 goal) {
    profiling::Scope scope(profiling::Stage::ward);
    auto &me = bc::self();
    // Reads only, so their order changes nothing: the recharge first, the
    // usual refusal, then one state read for the channels.
    if (me.ward_cooldown() > 0)
        return;
    const auto state = me.state();
    if (state.contains(bc::champion_state::teleporting) || state.contains(bc::champion_state::recalling) ||
        state.contains(bc::champion_state::submitting_gems) || me.buffs().has_stun() ||
        me.restrictions().contains(bc::buff_restrictions::ward))
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
