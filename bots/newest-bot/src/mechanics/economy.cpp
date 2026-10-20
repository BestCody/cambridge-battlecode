// Equipment queues, upgrade estimates, altar locks, tributes and wards.
#include "../bot.h"

namespace {
using Item = bc::item_type;
struct EquipmentPath {
    std::array<Item, 4> prefix;
    unsigned size;
    Item repeat;
};
EquipmentPath equipment_path(bc::champion_type type) {
    switch (type) {
        case bc::champion_type::stellar:
            return {
                {Item::power_shard_of_focus, Item::monster_shard_of_focus, Item::durability_shard_of_health},
                3,
                Item::power_shard_of_focus};
        case bc::champion_type::tavi:
            return {{Item::treasure_shard_of_focus, Item::monster_shard_of_focus,
                     Item::durability_shard_of_focus, Item::bruiser_shard_of_health},
                    4,
                    Item::power_shard_of_focus};
        case bc::champion_type::cassi:
            return {{Item::bruiser_shard_of_health, Item::durability_shard_of_focus,
                     Item::monster_shard_of_focus},
                    3,
                    Item::power_shard_of_focus};
        case bc::champion_type::issac:
            return {{Item::jungle_shard_of_strength, Item::monster_shard_of_strength,
                     Item::durability_shard_of_health},
                    3,
                    Item::power_shard_of_strength};
        case bc::champion_type::ryth:
            return {{Item::jungle_shard_of_strength, Item::monster_shard_of_strength,
                     Item::bruiser_shard_of_health, Item::durability_shard_of_strength},
                    4,
                    Item::power_shard_of_strength};
        case bc::champion_type::varg:
            return {{Item::jungle_shard_of_strength, Item::monster_shard_of_strength,
                     Item::durability_shard_of_strength, Item::bruiser_shard_of_health},
                    4,
                    Item::power_shard_of_strength};
        default:
            return {{}, 0, Item::count};
    }
}
Item next_item(bc::champion_type type, const bc::champion &unit = bc::self()) {
    const auto path = equipment_path(type);
    const auto inventory = unit.item_count();
    for (unsigned stage = 0; stage < path.size; ++stage) {
        const auto result = path.prefix[stage].result();
        if (result && !inventory[*result])
            return path.prefix[stage];
    }
    return path.repeat;
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
        const auto item = next_item(type);
        if (item == Item::count || me.gold_count() < reserve + item.cost())
            break;
        const unsigned remainder = me.item_count()[item];
        const unsigned count =
            std::min((me.gold_count() - reserve) / item.cost(), item.combine_count() - remainder);
        if (!count || !me.can_buy(item, count))
            break;
        me.buy(item, count);
        // Emblems replace ingredients. Re-read inventory on the next iteration;
        // the repeating Power stage deliberately never becomes 'completed'.
    }
    const auto lock = bc::consumable_type::altar_lock;
    if (me.gold_count() >= reserve + lock.cost() && me.can_buy(lock))
        me.buy(lock);
    refresh_equipment();
    if (me.is_alive())
        refresh_self();
}

float Bot::upgrade_gain(bc::champion_type champion, float hp, float attack, float spell,
                        unsigned gold) const {
    if (champion != type || !gold)
        return 0; // Private wallets never enter another champion's estimate.
    const auto item = next_item(champion);
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
    const auto item = next_item(champion, unit);
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
        distance(position, altars[altar]) > bc::constants::submit_gems_range || me.is_recalling() ||
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
                    distance(enemy.position, altar) <= bc::constants::submit_gems_range &&
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
        (best != ward || distance(position, ward) > int(bc::constants::ward_vision_radius) / 2))
        if (me.place_ward(best))
            ward = best;
}
