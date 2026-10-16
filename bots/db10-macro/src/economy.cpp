#include "bot.h"

namespace {
struct CombatUpgrade {
    bc::item_type shard;
    bc::item_info ingredient, emblem;
};

const std::vector<CombatUpgrade> &combat_upgrades() {
    static const auto choices = [] {
        std::vector<CombatUpgrade> result;
        for (unsigned i = 0; i < static_cast<unsigned>(bc::item_type::count); ++i) {
            const bc::item_type item(i);
            const auto info = item.info();
            if (!info.purchasable || !info.result)
                continue;
            const auto emblem = info.result->info();
            if (!emblem.passive || emblem.passive == bc::item_passive::durability)
                result.push_back({item, info, emblem});
        }
        return result;
    }();
    return choices;
}
} // namespace

void Bot::shop() {
    auto &me = bc::self();
    if (!me.can_buy(bc::item_type::power_shard_of_strength))
        return;
    const bool spawn = me.is_alive() && bc::map::spawn_region(bc::team::ally).is_set(me.position());
    auto inventory = me.item_count();
    auto has = [&](bc::item_passive passive) {
        for (unsigned i = 0; i < static_cast<unsigned>(bc::item_type::count); ++i) {
            bc::item_type item(i);
            if (inventory[item] && item.passive() == passive)
                return true;
        }
        return false;
    };
    int buys = 0;
    while (me.gold_count() >= bc::item_type::power_shard_of_strength.cost() && buys++ < 24) {
        if (bc::fuel_remaining() < policy::planning_reserve)
            break;
        float attack_growth = stats.strength_growth, spell_growth = 0;
        for (const auto &s : spells) {
            if (s.kind == bc::ability_kind::attack_reset)
                attack_growth += stats.strength_growth / s.cooldown;
            if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                s.kind == bc::ability_kind::aoe_damage)
                spell_growth += stats.focus_growth * s.modifier / s.cooldown;
        }
        if (passive == bc::passive_type::retaliation)
            spell_growth += stats.focus_growth *
                            bc::constants::passive_abilities::ren_retaliation_focus_ratio *
                            opening.mean_splash;
        bool focus_build = spell_growth * opening.focus_weight > attack_growth * 1.08f;
        // Power is only a +20% emblem while Bruiser is +40%, so the untouched
        // default must not decide the purchase: the auction below prices both.
        auto default_item =
            focus_build ? bc::item_type::power_shard_of_focus : bc::item_type::power_shard_of_strength;
        auto item = default_item;
        // Defence is purchased only to keep the attacking unit on the map.
        bool need_health = spawn && me.max_health() < me.strength() * 9;
        if (need_health)
            item = bc::item_type::bruiser_shard_of_health;
        else if (!opening.assault && !has(bc::item_passive::monster))
            // Sparse maps are a pure farming race with little or no PvP: camp
            // damage compounds into more clears, so take Monster ahead of the
            // gold economy instead of after it.
            item = focus_build ? bc::item_type::monster_shard_of_focus
                               : bc::item_type::monster_shard_of_strength;
        else if (policy::treasure_first && !has(bc::item_passive::treasure) &&
                 round < bc::constants::max_rounds / 4)
            item = focus_build ? bc::item_type::treasure_shard_of_focus
                               : bc::item_type::treasure_shard_of_strength;
        else if (!has(bc::item_passive::jungle) && opening.dangerous_fraction > 0.45f && range <= 1)
            item =
                focus_build ? bc::item_type::jungle_shard_of_focus : bc::item_type::jungle_shard_of_strength;
        else if (!has(bc::item_passive::monster))
            item = focus_build ? bc::item_type::monster_shard_of_focus
                               : bc::item_type::monster_shard_of_strength;
        else if (!has(bc::item_passive::home))
            item = focus_build ? bc::item_type::home_shard_of_focus
                               : bc::item_type::home_shard_of_strength;
        else if (!has(bc::item_passive::treasure) && round < bc::constants::max_rounds / 4)
            item = focus_build ? bc::item_type::treasure_shard_of_focus
                               : bc::item_type::treasure_shard_of_strength;
        else if (!has(bc::item_passive::durability) && me.max_health() < me.strength() * 14)
            item = focus_build ? bc::item_type::durability_shard_of_focus
                               : bc::item_type::durability_shard_of_strength;
        // Assault always auctions. Spread now auctions too whenever the passive
        // chain above did not choose, so spare gold buys the best marginal
        // combat power per gold instead of defaulting to a weaker emblem.
        const bool chain_pick = !(item == default_item);
        if (opening.assault || !chain_pick) {
            // A contested opening needs its combat spike on the first shop.
            // Spend each set on damage delivered through a brawl instead of
            // delaying that spike for income and neutral-camp passives.
            // Price the unfinished set using its actual combination metadata.
            const float current_health = me.max_health();
            const float current_strength = me.strength();
            const float current_focus = me.focus();
            float shield_ratio = 0;
            for (const auto &spell : spells)
                if (spell.kind == bc::ability_kind::shield)
                    shield_ratio += spell.modifier;
            const float physical_rate = attack_growth / std::max(1.0f, stats.strength_growth);
            const float spell_rate = spell_growth * opening.focus_weight /
                                     std::max(1.0f, stats.focus_growth);
            auto power = [&](float hp, float physical, float magic, float reduction) {
                return (hp + magic * shield_ratio) *
                       (physical * physical_rate + magic * spell_rate) / reduction;
            };
            const bool durable = has(bc::item_passive::durability);
            float current_reduction = 1;
            if (durable)
                current_reduction = 1 - bc::item_type::durability_emblem_of_strength.passive_magnitude();
            const float current_power = power(current_health, current_strength, current_focus,
                                              current_reduction);
            float best_gain = -1;
            for (const auto &choice : combat_upgrades()) {
                const auto held = inventory[choice.shard];
                const auto needed = choice.ingredient.combine_count - held;
                if (!needed)
                    continue;
                float hp_gain = (choice.emblem.health_points -
                                 int(held) * choice.ingredient.health_points) * stats.health_growth;
                float strength_gain = (choice.emblem.strength_points -
                                       int(held) * choice.ingredient.strength_points) * stats.strength_growth;
                const float focus_gain = (choice.emblem.focus_points -
                                          int(held) * choice.ingredient.focus_points) * stats.focus_growth;
                if (passive == bc::passive_type::health_strength)
                    strength_gain += hp_gain * bc::constants::passive_abilities::ryth_health_strength_ratio;
                // Remote health purchases do not heal the current attacking life.
                if (me.is_alive() && !spawn)
                    hp_gain = 0;
                float reduction = current_reduction;
                if (!durable && choice.emblem.passive == bc::item_passive::durability)
                    reduction = 1 - choice.emblem.passive_magnitude;
                const float gain = (power(current_health + hp_gain, current_strength + strength_gain,
                                          current_focus + focus_gain, reduction) - current_power) /
                                   (needed * choice.ingredient.cost);
                if (gain > best_gain) {
                    best_gain = gain;
                    item = choice.shard;
                }
            }
            need_health = false;
        }
        unsigned count = std::min<unsigned>(me.gold_count() / item.cost(),
                                            std::max(1u, item.combine_count()) - inventory[item]);
        if (need_health)
            count = std::min(count, std::max(1u, unsigned(std::ceil((me.strength() * 9 - me.max_health()) /
                                                                    stats.health_growth))));
        if (!count || !me.can_buy(item, count))
            break;
        me.buy(item, count);
        inventory = me.item_count();
    }
    if (me.can_buy(bc::consumable_type::altar_lock))
        me.buy(bc::consumable_type::altar_lock);
    // A crystal is insurance against disconnected terrain, not ordinary travel.
    if (me.gold_count() >= bc::consumable_type::tp_crystal.cost() &&
        me.can_buy(bc::consumable_type::tp_crystal))
        me.buy(bc::consumable_type::tp_crystal);
    mitigation = camp_mitigation = 1;
    camp_multiplier = stats.camp_damage_multiplier;
    for (unsigned i = 0; i < static_cast<unsigned>(bc::item_type::count); ++i) {
        bc::item_type item(i);
        if (!inventory[item])
            continue;
        auto p = item.passive();
        if (p == bc::item_passive::durability)
            mitigation = 1 - item.passive_magnitude();
        if (p == bc::item_passive::jungle)
            camp_mitigation = 1 - item.passive_magnitude();
        if (p == bc::item_passive::monster)
            camp_multiplier = stats.camp_damage_multiplier * (1 + item.passive_magnitude());
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

bool Bot::tribute() {
    auto &me = bc::self();
    if (!gems) {
        banking = false;
        return false;
    }
    int closest = int(altar_field.get(position));
    const int remaining =
        std::max(1, int(bc::constants::winning_gems) - int(bc::global_submitted_gems(bc::team::ally)));
    bool win = int(gems) >= remaining;
    const bool reachable = closest != bc::distance_field::unreachable;
    if (!reachable && !win)
        return false;
    bool deadline = int(bc::constants::max_rounds - round) <= closest + channel_duration(gems) + 5;
    float threshold =
        std::min(policy::bank_cap, opening.assault
                                       ? policy::bank_base + closest * policy::bank_distance
                                       : policy::spread_bank_base + closest * policy::spread_bank_distance);
    // Cassi and Issac share one fate: either death drops both bags, so the pair
    // banks smaller amounts more often than a solo champion does.
    if (type == bc::champion_type::cassi || type == bc::champion_type::issac)
        threshold *= policy::twin_bank_scale;
    // Near victory, each champion should bank its share of the remaining score.
    threshold = std::min(threshold, float(remaining) / std::max(std::size_t(1), roster.size()));
    bool capture_combo = false;
    if (target >= 0 && closest == 0 && groups[target].status == CampGroup::capture &&
        groups[target].holding_since >= 0) {
        int remaining = bc::constants::capture_time + 1 - (int(round) - groups[target].holding_since);
        capture_combo = remaining > 0 && remaining <= channel_duration(gems);
    }
    if (!banking && !win && !deadline && !capture_combo && gems < threshold && !(closest <= 2 && gems >= 20))
        return false;
    banking = true;
    publish(TeamMemory::bank);
    float best_value = 1e20f;
    vec2 selected{-1, -1};
    for (std::size_t index = 0; index < altars.size(); ++index) {
        auto altar = altars[index];
        float d = index < altar_routes.size() ? altar_routes[index].get(position) : distance(position, altar);
        if (d == bc::distance_field::unreachable)
            continue;
        float risk = 0;
        for (const auto &e : enemies)
            if (distance(e.position, altar) <= 6)
                risk += 6 + e.strength * 0.05f;
        bool locked = visible.is_set(altar) && bc::vision::has_altar_lock(altar) &&
                      bc::vision::altar_lock_at(altar).is_enemy();
        float value = d + risk + (locked ? bc::constants::altar_lock_hits : 0);
        if (value < best_value) {
            best_value = value;
            selected = altar;
        }
    }
    if (selected.x < 0)
        return false;
    if (distance(position, selected) <= bc::constants::submit_gems_range) {
        if (visible.is_set(selected) && bc::vision::has_altar_lock(selected) &&
            bc::vision::altar_lock_at(selected).is_enemy()) {
            if (me.can_attack())
                me.attack(bc::vision::altar_lock_at(selected));
            if (bc::vision::has_altar_lock(selected))
                for (const auto &a : me.abilities())
                    if (a.is_attack_reset() && a.is_ready() && !me.can_attack()) {
                        if (a.use() && me.can_attack())
                            me.attack(bc::vision::altar_lock_at(selected));
                        break;
                    }
            return true;
        }
        // Damage cannot cancel this channel. Stuns and a lethal burst can.
        float hazard = incoming(position);
        if (health > hazard * (channel_duration(gems) + 1) || win || hazard == 0) {
            auto lock = me.consumables()[bc::consumable_type::altar_lock];
            if (lock.count() && !bc::vision::has_altar_lock(selected))
                lock.use(selected);
            if constexpr (policy::diagnostic)
                if (capture_combo)
                    std::printf("CAPTURE_TRIBUTE r=%u gems=%u group=%d\n", round, gems, target);
            me.submit_gems(selected);
            return true;
        }
        if (skirmish())
            return true;
        retreating = true;
        return escape();
    }
    // Score altar choices first; build a route mask only for the selected one.
    move_to(around(selected, bc::constants::submit_gems_range), false, true);
    ward_ahead(selected);
    return true;
}

void Bot::disrupt_tribute() {
    if (enemies.empty() || bc::fuel_remaining() < policy::planning_reserve)
        return;
    auto &me = bc::self();
    auto lock = me.consumables()[bc::consumable_type::altar_lock];
    if (!lock.count() || me.restrictions().contains(bc::buff_restrictions::consumable))
        return;
    for (const auto &e : enemies)
        if (e.depositing && e.gems > 0) {
            for (auto altar : altars) {
                // The tribute radius is a conservative subset of lock placement
                // range, and is available directly from the engine settings.
                if (distance(position, altar) > bc::constants::submit_gems_range ||
                    distance(e.position, altar) > bc::constants::submit_gems_range ||
                    bc::vision::has_altar_lock(altar))
                    continue;
                if (lock.use(altar)) {
                    if constexpr (policy::diagnostic)
                        std::printf("TRIBUTE_INTERRUPTED r=%u gems=%d\n", round, e.gems);
                    return;
                }
            }
        }
}
