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
    // Near victory, each champion banks its share of the remaining score.
    const bool share = float(gems) >= float(remaining) / std::max(std::size_t(1), roster.size());
    bool capture_combo = false, holding = false;
    if (target >= 0 && groups[target].status == CampGroup::capture && groups[target].tiles().is_set(position)) {
        // Leaving a capture before it pays loses it, unless the tribute
        // starts here and ends with it.
        holding = true;
        if (closest == 0 && groups[target].holding_since >= 0) {
            const int remaining = capture_payout(groups[target]) - int(round);
            capture_combo = remaining > 0 && remaining <= channel_duration(gems);
        }
    }
    // Every trip banks, as willow best girl's champions do: whatever we carry
    // on the way out from spawn (the altar nearest it, from there), and at
    // once when an altar is a few steps away.
    const auto &spawn = own_spawn;
    // Altars left out lately (altar_open()) are no nearby altar.
    const int open_walk = open_altar_walk();
    const bool walled = open_walk > closest;
    const bool trip = spawn.is_set(position) || (closest <= policy::bank_nearby && !holding && !walled);
    // A bag worth its walk to an open altar is banked (bank_bag()), once its
    // capture has paid.
    const bool full =
        !holding && open_walk != bc::distance_field::unreachable && float(gems) >= bank_bag(open_walk);
    // A holder banks its share once its capture has paid.
    if (!banking && !win && !deadline && !trip && !full && !capture_combo && !(share && !holding))
        return false;
    if constexpr (policy::diagnostic)
        if (!banking && full && !trip && !win && !deadline)
            std::printf("BAG r=%u gems=%u walk=%d asked=%.1f\n", round, gems, open_walk, bank_bag(open_walk));
    if (!banking)
        bank_stalls = 0;
    banking = true;
    publish(TeamMemory::bank);
    float best_value = 1e20f;
    // With a crystal, an altar's walk is its hop's where that saves more
    // rounds than the crystal's price: from spawn its route from there, on
    // the way the plan's rounds left.
    const bool hopping = carrying_crystal();
    const bool on_spawn = hopping && hops_ready && home_field.get(position) == 0;
    const float threshold = hopping ? hop_threshold() : 0.0f;
    vec2 selected{-1, -1};
    int selected_index = -1;
    for (std::size_t index = 0; index < altars.size(); ++index) {
        // One left out lately waits (not for a winning bag).
        if (!altar_open(index) && !win && !deadline)
            continue;
        auto altar = altars[index];
        float d = index < altar_routes.size() ? altar_routes[index].get(position) : distance(position, altar);
        if (hopping) {
            if (on_spawn && d - float(altar_hops[index]) > threshold)
                d = float(altar_hops[index]);
            d = std::min(d, float(hop_rounds(altar)));
        }
        if (d == bc::distance_field::unreachable)
            continue;
        float risk = 0;
        for (const auto &e : enemies)
            if (distance(e.position, altar) <= 6)
                risk += policy::altar_enemy_risk + e.strength * policy::altar_enemy_strength_risk;
        bool locked = visible.is_set(altar) && bc::vision::has_altar_lock(altar) &&
                      bc::vision::altar_lock_at(altar).is_enemy();
        float value = d + risk + (locked ? rules::settings().altar_lock_hits : 0);
        if (value < best_value) {
            best_value = value;
            selected = altar;
            selected_index = int(index);
        }
    }
    if (selected.x < 0) {
        banking = false;
        return false;
    }
    // A holder banks only where the tribute starts beside its capture.
    if (holding && !win && !deadline && distance(position, selected) > rules::settings().submit_gems_range) {
        banking = false;
        return false;
    }
    if (distance(position, selected) <= rules::settings().submit_gems_range) {
        // A stunned champion cannot act: starting a tribute would throw and
        // lose the rest of the turn's work. Wait beside the altar instead.
        if (me.buffs().has_stun())
            return true;
        if (visible.is_set(selected) && bc::vision::has_altar_lock(selected) &&
            bc::vision::altar_lock_at(selected).is_enemy()) {
            // A melee champion within submit range may be out of the lock's
            // reach: step beside it first (attacking from two tiles threw).
            if (!me.attack_tiles().is_set(selected) && me.can_move())
                move_to(around(selected, 1), selected, false, false);
            const bool reach = me.attack_tiles().is_set(selected);
            if (reach && me.can_attack())
                me.attack(bc::vision::altar_lock_at(selected));
            if (reach && bc::vision::has_altar_lock(selected))
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
        // Nor can we act while it lasts: the enemies we know of must not be
        // able to kill us before it ends (channel_danger()).
        const bool lasts = win || !policy::channel_safety || channel_danger(channel_duration(gems)) < health;
        // At the start, the damage our last turn took, for the whole channel
        // (the turns of it then measure what comes).
        if (win || (lasts && (health > taken_last * float(channel_duration(gems) + 1) || hazard == 0))) {
            if constexpr (policy::diagnostic)
                if (capture_combo)
                    std::printf("CAPTURE_TRIBUTE r=%u gems=%u group=%d\n", round, gems, target);
            bank_waits = 0;
            me.submit_gems(selected);
            return true;
        }
        // The fight was asked this turn unless a winning bag came here first
        // (play_worker_turn()).
        if (skirmish_declined != int(round) && worker_skirmish())
            return true;
        // Nothing strikes us yet, but enemies would reach the channel: wait
        // beside the altar a while for them to go, then leave it out.
        if (hazard == 0) {
            if (++bank_waits <= policy::bank_wait_turns)
                return true;
            bank_waits = 0;
            close_altar(std::size_t(selected_index), policy::bank_wait_block);
            banking = false;
            return false;
        }
        retreating = true;
        return worker_escape();
    }
    // Walled off from the altar (camp bodies sealing its route): ours stood
    // beside a monolith pair 15+ rounds with a bag. Leave it out a while.
    if (bank_stalls >= policy::stuck_turns && !win && !deadline) {
        // For longer each time it walls us off (guardians' camps wall in its
        // one altar until they are cleared).
        if (altar_walled_times.size() < altars.size())
            altar_walled_times.resize(altars.size(), 0);
        const int times = altar_walled_times[selected_index]++;
        close_altar(std::size_t(selected_index), policy::stuck_rounds << std::min(times, 3));
        bank_stalls = 0;
        banking = false;
        return false;
    }
    // Score altar choices first; build a route mask only for the selected one.
    const auto approach = around(selected, rules::settings().submit_gems_range);
    const vec2 bank_from = position;
    move_to(approach, selected, false, true);
    bank_stalls = position == bank_from && me.can_attack() && me.can_cast() ? bank_stalls + 1 : 0;
    ward_ahead(selected);
    return true;
}

float Bot::bank_bag(int walk) const {
    const auto &settings = rules::settings();
    const float progress = std::min(1.0f, float(round) / float(std::max(1u, settings.max_rounds)));
    const float urgency = std::max(policy::bag_urgency_floor, 1.0f - policy::bag_time_urgency * progress -
                                                                  policy::bag_behind_urgency * behind_pressure());
    return (policy::bag_gems + policy::bag_gems_per_step * float(std::min(walk, policy::bag_walk_cap))) * urgency;
}

bool Bot::altar_open(std::size_t i) const {
    return i >= altar_closed_until.size() || int(round) >= altar_closed_until[i];
}

void Bot::close_altar(std::size_t i, int rounds) {
    if (altar_closed_until.size() < altars.size())
        altar_closed_until.resize(altars.size(), -1);
    if (i < altar_closed_until.size())
        altar_closed_until[i] = int(round) + rounds;
}

int Bot::open_altar_walk() const {
    // Until every altar's route is built, the field to them all.
    if (altar_routes.size() < altars.size())
        return altar_field_ready ? int(altar_field.get(position)) : int(bc::distance_field::unreachable);
    int best = bc::distance_field::unreachable;
    for (std::size_t i = 0; i < altars.size(); ++i)
        if (altar_open(i))
            best = std::min(best, int(altar_routes[i].get(position)));
    return best;
}

float Bot::channel_danger(int turns) const {
    // Each enemy in view or remembered that can reach us before the channel
    // ends (at its move speed, from its reach) deals its burst and then its
    // rate for the turns left. One channelling (recalling, banking) stays.
    float danger = 0;
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &e : *list) {
            if (e.health <= 0 || e.channel)
                continue;
            const int speed = std::max(1, e.move_speed);
            const int gap = std::max(0, distance(e.position, position) - std::max(e.range, e.spell_reach));
            const int arrival = (gap + speed - 1) / speed;
            if (arrival < turns)
                danger += e.burst + e.damage_rate * float(turns - arrival);
        }
    return danger * mitigation;
}

int Bot::spawn_altar_walk() const {
    // Unreachable until the altar field is built (altar_walk()).
    if (!altar_field_ready)
        return bc::distance_field::unreachable;
    if (spawn_bank_walk < 0) {
        spawn_bank_walk = bc::distance_field::unreachable;
        for (const auto p : own_spawn.positions())
            spawn_bank_walk = std::min(spawn_bank_walk, int(altar_field.get(p)));
    }
    return spawn_bank_walk;
}

void Bot::note_altar_locks() {
    // Enemies banking at an altar, and locks on the altars in view.
    if (altar_enemy_bank.size() != altars.size()) {
        altar_enemy_bank.assign(altars.size(), -1000);
        altar_locked.assign(altars.size(), -1000);
    }
    const int reach = int(rules::settings().submit_gems_range);
    for (const auto &e : enemies)
        if (e.tributing)
            for (std::size_t i = 0; i < altars.size(); ++i)
                if (distance(e.position, altars[i]) <= reach)
                    altar_enemy_bank[i] = int(round);
    for (std::size_t i = 0; i < altars.size(); ++i)
        if (visible.is_set(altars[i]))
            altar_locked[i] = bc::vision::has_altar_lock(altars[i]) ? int(round) : -1000;
}

bool Bot::lock_wanted(std::size_t i) const {
    // Where willow best girl's locks go (156 placements in 40 games): their
    // own home altar (the one nearest their spawn, 63%), the enemy's home
    // altar (42%; the two coincide on some maps), and other altars (17%).
    // Locks block only the other team's tributes. So: the two home altars
    // unless we know them locked, and altars an enemy banked at lately that we
    // have not seen locked since.
    if (i >= altar_locked.size())
        return false;
    static const auto nearest_altar = [&](bc::team side) {
        int x = 0, y = 0, n = 0;
        for (const auto p : bc::map::spawn_region(side).positions()) {
            x += p.x;
            y += p.y;
            ++n;
        }
        const vec2 centre = n ? vec2{x / n, y / n} : vec2{0, 0};
        int best = 0;
        for (std::size_t k = 1; k < altars.size(); ++k)
            if (distance(altars[k], centre) < distance(altars[best], centre))
                best = int(k);
        return best;
    };
    static const int home = nearest_altar(bc::team::ally), enemy_home = nearest_altar(bc::team::enemy);
    if (int(i) == home || int(i) == enemy_home)
        return altar_locked[i] < 0;
    return int(round) - altar_enemy_bank[i] <= policy::lock_memory && altar_locked[i] < altar_enemy_bank[i];
}

void Bot::place_lock() {
    // A carried lock goes on an altar that wants one within reach. willow best
    // girl carries its locks and places them as it passes (half of those it
    // buys, a median 137 rounds after buying); walking out for one kept our
    // Prism off its camps for 47 rounds.
    if (!carrying_lock)
        return;
    for (std::size_t i = 0; i < altars.size(); ++i) {
        const vec2 altar = altars[i];
        if (distance(position, altar) > policy::lock_range || !lock_wanted(i))
            continue;
        std::int32_t placed = 0;
        // The engine's own call: a refused placement returns a code instead of
        // ending the turn. Refused, the altar is locked out of view.
        if (bc::detail::sdk_use_consumable(bc::detail::raw_consumable_ids[1], altar.x, altar.y, &placed) == 0 && placed)
            carrying_lock = false;
        altar_locked[i] = int(round);
        return;
    }
}
