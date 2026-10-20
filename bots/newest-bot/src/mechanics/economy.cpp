// Equipment queues, upgrade estimates, altar locks, tributes and wards.
#include "../bot.h"

namespace {
using Item = bc::item_type;
// The top team's purchase order: a few single shards early, then one item
// bought indefinitely. Every shard adds one growth of its stat; five identical
// shards combine into an emblem automatically (a power emblem counts as six).
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
        case bc::champion_type::stellar:
        case bc::champion_type::heart_of_ruin: // Carries: damage after a little growth.
            return {{{{Item::treasure_shard_of_focus, 1},
                      {Item::power_shard_of_focus, 1},
                      {Item::home_shard_of_health, 1},
                      {Item::durability_shard_of_health, 1}}},
                    4,
                    Item::power_shard_of_focus};
        case bc::champion_type::ren: // Tank first, then focus for its stun.
            return {{{{Item::bruiser_shard_of_health, 2}, {Item::durability_shard_of_health, 2}}},
                    2,
                    Item::power_shard_of_focus};
        case bc::champion_type::ryth:
            return {{{{Item::bruiser_shard_of_health, 1}, {Item::durability_shard_of_health, 1}}},
                    2,
                    Item::power_shard_of_strength};
        case bc::champion_type::cassi: // Becomes tanky: a death also kills Issac.
            return {{{{Item::power_shard_of_focus, 1}, {Item::durability_shard_of_focus, 2}}},
                    2,
                    Item::bruiser_shard_of_health};
        case bc::champion_type::issac:
            return {{{{Item::treasure_shard_of_strength, 1},
                      {Item::power_shard_of_strength, 1},
                      {Item::home_shard_of_health, 1},
                      {Item::durability_shard_of_health, 1}}},
                    4,
                    Item::power_shard_of_strength};
        default:
            return {{},
                    0,
                    type == bc::champion_type::tavi || type == bc::champion_type::prism ||
                            type == bc::champion_type::willow
                        ? Item::power_shard_of_focus
                        : Item::power_shard_of_strength};
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
    }
}

void Bot::buy_equipment(bool crystal_required) {
    auto &me = bc::self();
    // Access is a controller decision. Reserve this champion's own crystal
    // before spending on the fixed equipment queue; Tavi cannot buy it afield.
    const auto crystal = bc::consumable_type::tp_crystal;
    const bool missing = crystal_required && !me.consumables()[crystal].count();
    if (missing && me.can_buy(crystal))
        me.buy(crystal);
    const unsigned reserve = crystal_required && !me.consumables()[crystal].count() ? crystal.cost() : 0;
    while (bc::fuel_remaining() > policy::planning_reserve) {
        const auto item = next_item(type, bought);
        if (me.gold_count() < reserve + item.cost())
            break;
        const unsigned count =
            std::min((me.gold_count() - reserve) / item.cost(), step_left(type, bought, item));
        if (!count || !me.can_buy(item, count))
            break;
        me.buy(item, count);
        bought[static_cast<unsigned>(item)] += count;
    }
    const auto lock = bc::consumable_type::altar_lock;
    if (policy::buy_altar_locks && me.gold_count() >= reserve + lock.cost() && me.can_buy(lock))
        me.buy(lock);
    refresh_equipment();
    if (me.is_alive())
        refresh_self();
}

float Bot::upgrade_gain(bc::champion_type champion, float hp, float attack, float spell,
                        unsigned gold) const {
    if (champion != type || !gold)
        return 0; // Private wallets never enter another champion's estimate.
    const auto item = next_item(champion, bought);
    if (item == Item::count)
        return 0;
    const unsigned count = std::min(gold / item.cost(), item.combine_count());
    if (!count)
        return 0;
    float spell_weight = 0;
    for (const auto &ability : spells)
        spell_weight += ability.modifier / ability.cooldown;
    const float damage = count * (item.strength_points() * stats.strength_growth +
                                  item.focus_points() * stats.focus_growth * spell_weight);
    const float survival = count * item.health_points() * stats.health_growth / std::max(1.0f, hp);
    return damage / std::max(1.0f, attack + spell * spell_weight) + survival;
}

float Bot::upgrade_output(unsigned member, unsigned reward, bool siege, float splash) const {
    const auto *seen = member == slot ? nullptr : observed_member(member);
    if (member != slot && (!seen || !roster[member].is_visible()))
        return 0;
    const auto champion = member == slot ? type : seen->type;
    const auto &unit = roster[member];
    const auto item = next_item(champion, member == slot ? bought : Bought{});
    if (item == Item::count || reward < item.cost())
        return 0;
    const auto inventory = unit.item_count();
    const unsigned owned = inventory[item];
    const unsigned count = std::min(reward / item.cost(), item.combine_count() - owned);
    float attack_points = count * item.strength_points(), focus_points = count * item.focus_points();
    const auto emblem = item.result();
    const bool completed = emblem && count + owned == item.combine_count();
    if (completed) {
        attack_points = emblem->strength_points() - owned * item.strength_points();
        focus_points = emblem->focus_points() - owned * item.focus_points();
    }
    const auto design = bc::champion(champion).base_stats();
    const auto &kit = member == slot ? spells : seen->abilities;
    const float attack = member == slot ? strength : seen->strength;
    const float spell = member == slot ? focus : seen->focus;
    float multiplier = design.camp_damage_multiplier;
    for (auto monster :
         {Item::monster_emblem_of_strength, Item::monster_emblem_of_focus, Item::monster_emblem_of_health})
        if (inventory[monster])
            multiplier *= 1 + monster.passive_magnitude();
    auto output = [&](float basic, float magic) {
        float rate = spell_dps(kit, siege ? 0 : basic, magic);
        for (const auto &ability : kit)
            if (ability.kind == bc::ability_kind::aoe_damage)
                rate += magic * ability.modifier * (splash - 1) / ability.cooldown;
        return rate;
    };
    const float before = output(attack, spell) * multiplier;
    if (completed && emblem->passive() == bc::item_passive::monster && !inventory[*emblem])
        multiplier *= 1 + emblem->passive_magnitude();
    const float after =
        output(attack + attack_points * design.strength_growth, spell + focus_points * design.focus_growth) *
        multiplier;
    return std::max(0.0f, after - before);
}

bool Bot::clear_altar_lock(int altar) {
    auto &me = bc::self();
    if (altar < 0 || altar >= int(altars.size()) || distance(position, altars[altar]) > range ||
        !bc::vision::visible_tiles().is_set(altars[altar]))
        return false;
    const auto at = altars[altar];
    if (!bc::vision::has_altar_lock(at) || !bc::vision::altar_lock_at(at).is_enemy())
        return false;
    bool acted = false;
    if (me.can_attack()) {
        me.attack(bc::vision::altar_lock_at(at));
        acted = true;
    }
    for (const auto &ability : me.abilities())
        if (bc::vision::has_altar_lock(at) && ability.is_attack_reset() && ability.is_ready() &&
            !me.can_attack() && ability.use() && me.can_attack()) {
            me.attack(bc::vision::altar_lock_at(at));
            acted = true;
        }
    return acted;
}

ActionResult Bot::deposit_at(int altar) {
    auto &me = bc::self();
    if (!gems)
        return ActionResult::complete;
    if (me.is_submitting_gems())
        return ActionResult::progress;
    if (altar < 0 || altar >= int(altars.size()) ||
        distance(position, altars[altar]) > rules::settings().submit_gems_range || me.is_recalling() ||
        me.is_teleporting() || me.buffs().has_stun())
        return ActionResult::blocked;
    const auto at = altars[altar];
    if (!visible.is_set(at))
        return ActionResult::blocked;
    if (bc::vision::has_altar_lock(at) && bc::vision::altar_lock_at(at).is_enemy()) {
        return clear_altar_lock(altar) ? ActionResult::progress : ActionResult::blocked;
    }
    // This executor has no independent fight, retreat or altar selection.
    me.submit_gems(at);
    return ActionResult::progress;
}

void Bot::interrupt_deposit() {
    auto &me = bc::self();
    if (me.is_teleporting() || me.is_submitting_gems() || me.is_recalling() ||
        me.restrictions().contains(bc::buff_restrictions::consumable))
        return;
    auto lock = me.consumables()[bc::consumable_type::altar_lock];
    if (!lock.count())
        return;
    for (const auto &enemy : enemies)
        if (enemy.depositing && relevant_enemy(enemy))
            for (const auto altar : altars)
                if (distance(position, altar) <= rules::altar_lock_range &&
                    distance(enemy.position, altar) <= rules::settings().submit_gems_range &&
                    visible.is_set(altar) && !bc::vision::has_altar_lock(altar)) {
                    lock.use(altar);
                    return;
                }
}

void Bot::ward_ahead(vec2 goal) {
    if (bc::fuel_remaining() < policy::ward_reserve)
        return;
    auto &me = bc::self();
    if (me.is_teleporting() || me.is_recalling() || me.is_submitting_gems())
        return;
    if (me.ward_cooldown() > 0 || me.restrictions().contains(bc::buff_restrictions::ward))
        return;
    vec2 best = position;
    float value = -1e20f;
    auto placements = me.ward_placement_tiles() & ~(blocked | bc::vision::ward_tiles(bc::team::ally));
    // Put vision into the next brush/approach instead of repeatedly scanning all
    // hypothetical fields of view (which would consume the tactical budget).
    for (auto p : placements.positions()) {
        if (bc::fuel_remaining() < policy::action_reserve)
            break;
        // Allied wards are not shared through vision. Partition placement
        // tiles by stable team slot so two allies can never reserve one tile.
        if (unsigned(p.x + p.y) % roster.size() != slot)
            continue;
        float score = -distance(p, goal) * 3 + distance(p, position) * 0.3f;
        if (bc::map::bushes().is_set(p))
            score += 2;
        if (!visible.is_set(p))
            score += 1;
        if (blocked.is_set(p))
            score -= 4;
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
