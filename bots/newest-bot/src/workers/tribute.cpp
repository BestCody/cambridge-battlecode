// Worker tribute decisions.
#include "../bot.h"

bool Bot::worker_tribute() {
    profiling::Scope scope(profiling::Stage::tribute);
    auto &me = bc::self();
    if (!gems) {
        banking = false;
        return false;
    }
    int closest = int(altar_walk().get(position));
    const int remaining =
        std::max(1, int(rules::settings().winning_gems) - int(bc::global_submitted_gems(bc::team::ally)));
    bool win = int(gems) >= remaining;
    const bool reachable = closest != bc::distance_field::unreachable;
    if (!reachable && !win)
        return false;
    bool deadline = int(rules::settings().max_rounds - round) <= closest + channel_duration(gems) + 5;
    // Small bags banked often: the top team's carries bank around 28 gems and
    // everyone else around 20, so a death rarely costs much.
    const bool carry = is_carry(type);
    float threshold = float(carry ? policy::fish_bag_carry : policy::fish_bag_other);
    // Near victory, each champion should bank its share of the remaining score.
    threshold = std::min(threshold, float(remaining) / std::max(std::size_t(1), roster.size()));
    bool capture_combo = false;
    if (target >= 0 && closest == 0 && groups[target].status == CampGroup::capture &&
        groups[target].holding_since >= 0) {
        int remaining = rules::settings().capture_time + 1 - (int(round) - groups[target].holding_since);
        capture_combo = remaining > 0 && remaining <= channel_duration(gems);
    }
    if (!banking && !win && !deadline && !capture_combo && gems < threshold &&
        !(closest <= 2 && gems >= policy::fish_bag_nearby))
        return false;
    banking = true;
    publish(TeamMemory::bank);
    float best_value = 1e20f;
    vec2 selected{-1, -1};
    int selected_index = -1;
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
        float value = d + risk + (locked ? rules::settings().altar_lock_hits : 0);
        if (value < best_value) {
            best_value = value;
            selected = altar;
            selected_index = int(index);
        }
    }
    if (selected.x < 0)
        return false;
    if (distance(position, selected) <= rules::settings().submit_gems_range) {
        // A stunned champion cannot act: starting a tribute would throw and
        // lose the rest of the turn's work. Wait beside the altar instead.
        if (me.buffs().has_stun())
            return true;
        if (visible.is_set(selected) && bc::vision::has_altar_lock(selected) &&
            bc::vision::altar_lock_at(selected).is_enemy()) {
            if (me.can_attack())
                me.attack(bc::vision::altar_lock_at(selected));
            if (bc::vision::has_altar_lock(selected))
                for (const auto &a : me.abilities())
                    if (a.is_attack_reset() && ready(a, me.can_cast()) && !me.can_attack()) {
                        if (a.use() && me.can_attack())
                            me.attack(bc::vision::altar_lock_at(selected));
                        break;
                    }
            return true;
        }
        // Damage cannot cancel this channel. Stuns and a lethal burst can.
        float hazard = incoming(position);
        if (health > hazard * (channel_duration(gems) + 1) || win || hazard == 0) {
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
    const auto approach = around(selected, rules::settings().submit_gems_range);
    if (crystal_travel(int(groups.size()) + selected_index, approach))
        return true;
    move_to(approach, false, true);
    ward_ahead(selected);
    return true;
}
