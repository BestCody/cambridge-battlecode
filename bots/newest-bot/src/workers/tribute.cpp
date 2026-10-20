// Worker tribute decisions and enemy tribute disruption.
#include "../bot.h"

bool Bot::worker_tribute() {
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
    float threshold = std::min(policy::bank_cap, policy::bank_base + closest * policy::bank_distance);
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
        if (worker_skirmish())
            return true;
        retreating = true;
        return worker_escape();
    }
    // Score altar choices first; build a route mask only for the selected one.
    move_to(around(selected, bc::constants::submit_gems_range), false, true);
    ward_ahead(selected);
    return true;
}

void Bot::worker_disrupt_tribute() {
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
