// Delivery planning: altar choice, guarded channel waves and crossings.
#include "../bot.h"
#include <tuple>

namespace {
bool across(BankStage stage) {
    return stage == BankStage::crossed || stage == BankStage::assembled || stage == BankStage::guard ||
           stage == BankStage::channel || stage == BankStage::done;
}
} // namespace

void Bot::observe_cargo() {
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (!squad.contains(i))
            continue;
        const auto *member = observed_member(i);
        if (i != slot && roster[i].is_alive() && !member)
            continue;
        const unsigned bag = !roster[i].is_alive() ? 0 : i == slot ? gems : unsigned(member->gems);
        if (!bag)
            squad.cargo_since[i] = -1;
        else if (!squad.cargo_seen[i])
            squad.cargo_since[i] = round;
        squad.cargo_seen[i] = bag;
    }
}

bool Bot::channel_protected(vec2 tile, int start, int duration, unsigned guards, const BankEstimate *plan,
                            unsigned *committed_guards) const {
    const ObservedUnit self(*this);
    float carrier_health = health;
    if (plan)
        for (unsigned i = 0; i < roster.size(); ++i)
            if ((plan->participants & (1u << i)) && plan->positions[i] == tile)
                if (const auto *member = i == slot ? &self : observed_member(i))
                    carrier_health = member->health;
    float damage = incoming(tile) * duration;
    unsigned spent = committed_guards ? *committed_guards : 0;
    const int end = start + duration;
    for (const auto &enemy : enemies) {
        if (enemy.health <= 0 || enemy.channel)
            continue;
        const int distance_to = distance(enemy.position, tile);
        const int attacks_at =
            std::max(0, distance_to - std::max(enemy.range, enemy.spell_reach)) / enemy.move_speed;
        if (attacks_at > 0 && attacks_at < end)
            damage += (enemy.strength + enemy.damage_rate) * std::max(0, end - std::max(start, attacks_at));
        int interrupts = bc::distance_field::unreachable;
        for (unsigned a = 0; a < enemy.abilities.size(); ++a) {
            const auto &spell = enemy.abilities[a];
            if (spell.kind != bc::ability_kind::cc || spell.duration <= 0)
                continue;
            const int approach = std::max(0, distance_to - int(spell.range + spell.radius));
            const int arrival = (approach + enemy.move_speed - 1) / enemy.move_speed;
            interrupts = std::min(
                interrupts, std::max({arrival + (arrival && spell.exclusive ? 1 : 0), enemy.cooldowns[a],
                                      enemy.stunned ? enemy.stun_time + 1 : 0, enemy.completed ? 1 : 0}));
        }
        const int altar = plan ? plan->altar : squad.altar;
        if (altar >= 0 && enemy.champion().consumables()[bc::consumable_type::altar_lock].count())
            interrupts = std::min(
                interrupts, std::max(0, distance(enemy.position, altars[altar]) - rules::altar_lock_range) /
                                enemy.move_speed);
        if (interrupts >= end)
            continue;
        bool covered = false;
        for (unsigned i = 0; i < roster.size() && !covered; ++i) {
            if (!(guards & (1u << i)) || (spent & (1u << i)))
                continue;
            const auto *member = i == slot ? &self : observed_member(i);
            if (!member || member->channel || member->stunned)
                continue;
            auto defender = *member;
            std::optional<bc::bitboard> defender_sight;
            int ready = member->completed ? 1 : 0;
            if (plan) {
                defender.position = plan->positions[i];
                defender_sight = bc::champion(member->type).visible_tiles_from(defender.position);
                defender.sight = &*defender_sight;
                ready = std::max(ready, plan->arrivals[i]);
            }
            // Do not assume that our cast precedes an interleaved enemy turn.
            if (ready >= interrupts)
                continue;
            const auto opportunity = firing(defender, enemy, true);
            const bool lethal = opportunity.damage >= enemy.health + enemy.shield;
            const bool held = opportunity.control > 0 && !enemy.immune && ready + opportunity.control >= end;
            if (lethal || held) {
                spent |= 1u << i;
                covered = true;
            }
        }
        if (!covered)
            return false;
    }
    if (carrier_health <= damage)
        return false;
    if (committed_guards)
        *committed_guards = spent;
    return true;
}

bool Bot::can_protect_channel(int duration) const {
    unsigned guards = 0;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (i != slot && squad.contains(i) && same_job(shared.get(i)) &&
            (BankStage(TeamMemory::deposit_stage(shared.get(i), groups.size())) == BankStage::guard ||
             BankStage(TeamMemory::deposit_stage(shared.get(i), groups.size())) == BankStage::done))
            if (const auto *member = observed_member(i))
                if (!member->channel && !member->stunned)
                    guards |= 1u << i;
    return channel_protected(position, 0, duration, guards);
}

bool Bot::schedule_bank(BankEstimate &plan, const std::array<unsigned, TeamMemory::slots> &bags,
                        unsigned already_channeling) const {
    unsigned carriers = 0;
    for (unsigned i = 0; i < roster.size(); ++i)
        if ((plan.participants & (1u << i)) && bags[i])
            carriers |= 1u << i;
    if (enemies.empty()) {
        int ready = plan.locks;
        for (unsigned i = 0; i < roster.size(); ++i)
            if (plan.participants & (1u << i))
                ready = std::max(ready, plan.arrivals[i] + plan.locks);
        plan.completion = ready;
        for (unsigned i = 0; i < roster.size(); ++i) {
            if (!(carriers & (1u << i)))
                continue;
            plan.starts[i] = (already_channeling & (1u << i)) ? 0 : ready;
            const int duration = (already_channeling & (1u << i)) ? roster[i].remaining_channel_time()
                                                                  : channel_duration(bags[i]);
            plan.finishes[i] = plan.starts[i] + duration;
            plan.completion = std::max(plan.completion, plan.finishes[i]);
        }
        plan.guards = plan.participants & ~carriers;
        plan.channel = plan.completion - ready;
        plan.feasible = true;
        return true;
    }
    int best_finish = bc::distance_field::unreachable;
    BankEstimate best = plan;
    // At most three carriers: enumerate disjoint channel waves. Each wave
    // explicitly reserves the other members as guards until its finish.
    auto visit = [&](auto &&recurse, BankEstimate candidate, unsigned remaining, int earliest, bool first,
                     unsigned committed_guards) -> void {
        if (!remaining) {
            if (earliest < best_finish) {
                best_finish = earliest;
                best = candidate;
            }
            return;
        }
        for (unsigned wave = remaining; wave; wave = (wave - 1) & remaining) {
            if (first && (wave & already_channeling) != already_channeling)
                continue;
            const unsigned guards = plan.participants & ~wave;
            int starts = earliest;
            for (unsigned i = 0; i < roster.size(); ++i)
                if (plan.participants & (1u << i))
                    starts = std::max(starts, plan.arrivals[i]);
            starts += first ? plan.locks : 0;
            auto next = candidate;
            auto reserved_guards = committed_guards;
            int finish = starts;
            bool safe = true;
            for (unsigned i = 0; i < roster.size(); ++i) {
                if (!(wave & (1u << i)))
                    continue;
                int duration = channel_duration(bags[i]);
                int at = starts;
                if (already_channeling & (1u << i)) {
                    at = 0;
                    duration = roster[i].remaining_channel_time();
                }
                next.starts[i] = at;
                next.finishes[i] = at + duration;
                finish = std::max(finish, at + duration);
                safe &= channel_protected(plan.positions[i], at, duration, guards, &plan, &reserved_guards);
                if (!safe)
                    break;
            }
            if (!safe || finish >= best_finish)
                continue;
            if (first)
                next.guards = guards;
            recurse(recurse, next, remaining & ~wave, finish, false, reserved_guards);
        }
    };
    if (!carriers) {
        plan.feasible = true;
        return true;
    }
    visit(visit, plan, carriers, 0, true, 0);
    if (best_finish == bc::distance_field::unreachable)
        return false;
    plan = best;
    plan.completion = best_finish;
    plan.channel = std::max(0, best_finish - plan.travel - plan.access - plan.locks);
    plan.feasible = true;
    return true;
}

BankEstimate Bot::estimate_bank(int altar) {
    BankEstimate plan;
    if (altar < 0 || altar >= int(altar_access.size()) || !altar_access[altar].prepared ||
        TeamMemory::deposit(altar, unsigned(BankStage::prepared), groups.size()) == TeamMemory::unavailable)
        return plan;
    plan.altar = altar;
    const auto &access = altar_access[altar];
    const auto &route = altar_routes[altar];
    const ObservedUnit self(*this);
    std::array<unsigned, TeamMemory::slots> bags{};
    unsigned channels = 0;
    auto destinations =
        around(altars[altar], rules::settings().submit_gems_range) & ~static_blocked & ~known_camp_obstacles;
    bc::bitboard occupied_channels;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i))
            if (const auto *member = i == slot ? &self : observed_member(i))
                if (distance(member->position, altars[altar]) <= rules::settings().submit_gems_range)
                    occupied_channels.set(member->position);
    destinations &= ~occupied_channels;
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (!squad.contains(i) || !roster[i].is_alive())
            continue;
        const auto *member = i == slot ? &self : observed_member(i);
        if (!member)
            continue; // Unseen bags/readiness are not available resources.
        plan.participants |= 1u << i;
        bags[i] = member->gems;
        plan.cargo += bags[i];
        if (member->depositing)
            channels |= 1u << i;
        const bool crossing = !access.region.is_set(member->position);
        int travel = 0;
        vec2 best{-1, -1};
        if (distance(member->position, altars[altar]) <= rules::settings().submit_gems_range) {
            best = member->position;
        } else if (!crossing) {
            const auto &cached = member_routes[i];
            const auto &from = member_route(i);
            int nearest = bc::distance_field::unreachable;
            for (auto tile : destinations.positions()) {
                const int d = from.get(tile);
                if (d < nearest) {
                    nearest = d;
                    best = tile;
                }
            }
            if (best.x < 0)
                return BankEstimate{};
            travel = (nearest + member->move_speed - 1) / member->move_speed;
        } else {
            if (access.entries.is_empty())
                return BankEstimate{};
            const int entry = access.entry_routes.get(member->position);
            if (entry == bc::distance_field::unreachable)
                return BankEstimate{};
            plan.teleport = true;
            plan.entry = access.rendezvous;
            plan.access = rules::settings().tp_crystal_time * std::popcount(squad.members) + 1;
            plan.supply |= !roster[i].consumables()[bc::consumable_type::tp_crystal].count();
            travel = (entry + member->move_speed - 1) / member->move_speed;
            if (plan.supply) {
                int from_spawn = bc::distance_field::unreachable;
                for (auto spawn : bc::map::spawn_region(bc::team::ally).positions())
                    from_spawn = std::min(from_spawn, int(access.entry_routes.get(spawn)));
                if (from_spawn == bc::distance_field::unreachable)
                    return BankEstimate{};
                travel = std::min(int(home_field.get(member->position)), rules::settings().recall_time) + 1 +
                         from_spawn;
            }
            int inside = bc::distance_field::unreachable;
            for (auto landing : (around(access.rendezvous, rules::crystal_range) & access.region).positions())
                inside = std::min(inside, int(route.get(landing)));
            travel += inside;
            // The final walk is planned from the common landing component.
            for (auto tile : destinations.positions()) {
                if (best.x < 0 || distance(tile, access.rendezvous) < distance(best, access.rendezvous))
                    best = tile;
            }
        }
        if (best.x < 0)
            return BankEstimate{};
        plan.positions[i] = best;
        destinations.set(best, false);
        plan.travel = std::max(plan.travel, travel);
        plan.arrivals[i] = travel + (crossing ? plan.access : 0);
    }
    if (!plan.participants)
        return BankEstimate{};
    if (visible.is_set(altars[altar]) && bc::vision::has_altar_lock(altars[altar]) &&
        bc::vision::altar_lock_at(altars[altar]).is_enemy())
        plan.locks = (rules::settings().altar_lock_hits + std::popcount(plan.participants) - 1) /
                     std::popcount(plan.participants);
    for (const auto &enemy : enemies) {
        const int approach =
            std::max(0, distance(enemy.position, altars[altar]) - std::max(enemy.range, enemy.spell_reach) -
                            rules::settings().submit_gems_range);
        plan.interference = std::min(plan.interference, approach / enemy.move_speed);
    }
    if (!schedule_bank(plan, bags, channels))
        return BankEstimate{};
    plan.winning = plan.cargo + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems;
    const int next = squad.resume_camp >= 0 ? squad.resume_camp : squad.camp;
    int direct = 0;
    if (next >= 0) {
        int exit = bc::distance_field::unreachable;
        for (const auto &camp : groups[next].camps)
            exit = std::min(exit, int(route.get(camp.position)));
        plan.recall_exit = exit == bc::distance_field::unreachable;
        plan.exit = plan.recall_exit ? rules::settings().recall_time + 1 + groups[next].home_distance : exit;
        direct = std::max(0, distance(position, groups[next].centre) - range);
    } else {
        plan.recall_exit = (access.region & bc::map::spawn_region(bc::team::ally)).is_empty();
        plan.exit = plan.recall_exit ? rules::settings().recall_time + 1 : 0;
    }
    if (plan.winning)
        plan.exit = 0;
    plan.cost = plan.completion + plan.exit;
    plan.overhead = std::max(0, plan.completion - plan.channel + plan.exit - direct);
    return plan;
}

BankEstimate Bot::choose_bank() {
    BankEstimate best;
    for (unsigned i = 0; i < altar_access.size(); ++i) {
        if (int(i) == squad.failed_altar && int(round) < squad.altar_retry)
            continue;
        const auto candidate = estimate_bank(i);
        if (candidate.feasible && (best.altar < 0 || candidate.cost < best.cost))
            best = candidate;
    }
    return best;
}

bool Bot::review_banking(bool urgent) {
    if (!urgent && squad.coordinator != int(slot))
        return false;
    if (squad.access_preparation)
        return false;
    unsigned cargo = gems;
    int oldest = gems ? squad.cargo_since[slot] : -1;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i))
            if (const auto *member = observed_member(i)) {
                cargo += member->gems;
                if (member->gems && squad.cargo_since[i] >= 0)
                    oldest = oldest < 0 ? squad.cargo_since[i] : std::min(oldest, squad.cargo_since[i]);
            }
    if (!cargo) {
        squad.cargo_deadline = -1;
        return false;
    }
    const bool winning = cargo + bc::global_submitted_gems(bc::team::ally) >= rules::settings().winning_gems;
    if (!urgent && !winning && cargo == unsigned(squad.reviewed_gems) &&
        int(enemies.size()) == squad.last_enemies && int(round) < squad.review &&
        (squad.cargo_deadline < 0 || int(round) < squad.cargo_deadline))
        return false;
    const auto plan = choose_bank();
    if (!plan.feasible)
        return false;
    const int next_time =
        squad.plan.camp == squad.camp && squad.plan.complete ? squad.plan.reward_time : work_window();
    if (oldest >= 0 && squad.cargo_deadline < oldest)
        squad.cargo_deadline = oldest + plan.completion + next_time + work_window();
    const bool aged = squad.cargo_deadline >= 0 && int(round) >= squad.cargo_deadline;
    const int hard = int(rules::settings().max_rounds) - int(round);
    const float race =
        squad.enemy_pace > 0 ? (rules::settings().winning_gems - squad.enemy_score) / squad.enemy_pace : hard;
    const bool deadline = std::min(float(hard), race) <= plan.completion + next_time;
    bool continuation = false;
    if (squad.plan.camp == squad.camp && squad.plan.complete && squad.camp >= 0) {
        const auto &next = groups[squad.camp];
        const float reward = next.unit.gem_value();
        const float later =
            next_time + plan.cost + channel_duration(cargo + unsigned(reward)) - channel_duration(cargo);
        continuation = reward > 0 && squad.plan.control && later < std::min(float(hard), race) &&
                       (cargo + reward) / std::max(1.0f, later) > cargo / std::max(1.0f, plan.cost) &&
                       plan.interference > plan.completion + next_time;
    }
    squad.batch = std::max(1.0f, squad.pressure_rate * plan.completion);
    squad.reviewed_gems = cargo;
    squad.last_enemies = enemies.size();
    squad.review = round + std::max(1, std::min(work_window(), plan.completion));
    const bool capture = squad.phase == SquadPhase::capture && squad.camp >= 0 &&
                         groups[squad.camp].holding_since >= 0 && squad.plan.control;
    const bool pending_objective = squad.camp < 0 || !squad.plan.complete || squad.plan.camp != squad.camp;
    if (!winning && !deadline && ((!aged && (continuation || pending_objective)) || capture))
        return false;
    squad.resume_camp = squad.camp;
    squad.altar = plan.altar;
    squad.need_crystal = plan.supply;
    squad.bank_stage = plan.supply ? BankStage::preparing : BankStage::travel;
    squad.phase = plan.supply ? SquadPhase::recover : SquadPhase::deposit;
    squad.changed = squad.progress = round;
    squad.route_best = bc::distance_field::unreachable;
    squad.participants = living_core();
    squad.crossed = 0;
    squad.crossing_deadline = round + plan.travel + plan.access + work_window();
    squad.arrival_deadline = round + plan.completion + work_window();
    job_search = {};
    active_search = {};
    publish_squad(true);
    if constexpr (policy::diagnostic)
        std::printf("BANK_PLAN r=%u slot=%u altar=%d cargo=%u age=%d trip=%.1f tp=%d supply=%d win=%d\n",
                    round, slot, plan.altar, cargo, oldest < 0 ? 0 : int(round) - oldest, plan.cost,
                    plan.teleport, plan.supply, winning);
    return true;
}

bool Bot::crossing_ready() {
    const auto alive = living_core() & squad.participants;
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (!(alive & (1u << i)))
            continue;
        if (i == slot && altar_access[squad.altar].region.is_set(position))
            squad.crossed |= 1u << i;
        else if (same_job(shared.get(i)) &&
                 across(BankStage(TeamMemory::deposit_stage(shared.get(i), groups.size()))))
            squad.crossed |= 1u << i;
    }
    if (squad.coordinator != int(slot) && squad.coordinator >= 0 && same_job(shared.get(squad.coordinator))) {
        const auto stage = BankStage(TeamMemory::deposit_stage(shared.get(squad.coordinator), groups.size()));
        if (across(stage) && stage != BankStage::crossed)
            return true;
    }
    if ((squad.crossed & alive) == alive) {
        if constexpr (policy::diagnostic)
            std::printf("BANK_CROSSED r=%u slot=%u members=%u\n", round, slot, alive);
        return true;
    }
    squad.motion = {MoveStatus::waiting, MoveReason::support, -1, squad.crossing_deadline};
    return false;
}

bool Bot::teleport_assignment(vec2 &landing) const {
    const auto participants = living_core() & squad.participants;
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (!(participants & (1u << i)))
            continue;
        const auto code = shared.get(i);
        if (!same_job(code))
            return false;
        const auto stage = BankStage(TeamMemory::deposit_stage(code, groups.size()));
        if (stage != BankStage::ready && stage != BankStage::crossing && !across(stage))
            return false;
    }
    // Each fixed member owns a disjoint subset of terrain-derived landings.
    // Neither disappearance behind a wall nor a death renumbers reservations.
    const unsigned count = std::popcount(squad.members);
    const unsigned ordinal = std::popcount(squad.members & ((1u << slot) - 1));
    const auto &landings = altar_access[squad.altar].landings;
    for (unsigned i = ordinal; i < landings.size(); i += count)
        if (distance(position, landings[i]) <= rules::crystal_range && !blocked.is_set(landings[i])) {
            landing = landings[i];
            return true;
        }
    return false;
}

void Bot::squad_bank() {
    if (squad.need_crystal) {
        squad_recover(true);
        return;
    }
    if (squad.altar < 0 || squad.altar >= int(altar_access.size()) || !altar_access[squad.altar].prepared)
        return;
    auto &me = bc::self();
    const auto &access = altar_access[squad.altar];
    squad.phase = SquadPhase::deposit;
    if (squad.arrival_deadline < 0) {
        int travel = 0, duration = channel_duration(gems);
        for (unsigned i = 0; i < roster.size(); ++i) {
            if (!squad.contains(i))
                continue;
            const auto *member = observed_member(i);
            if (i != slot && !member)
                continue;
            const auto at = i == slot ? position : member->position;
            const int path =
                access.region.is_set(at) ? altar_routes[squad.altar].get(at) : access.entry_routes.get(at);
            if (path != bc::distance_field::unreachable)
                travel = std::max(travel, path);
            duration = std::max(duration, channel_duration(i == slot ? gems : member->gems));
        }
        squad.crossing_deadline =
            round + travel + rules::settings().tp_crystal_time * std::popcount(squad.members) + work_window();
        squad.arrival_deadline = squad.crossing_deadline + duration + work_window();
    }
    if (!access.region.is_set(position)) {
        if (!me.consumables()[bc::consumable_type::tp_crystal].count()) {
            begin_recovery();
            squad_recover(true);
            return;
        }
        bc::bitboard entries;
        const unsigned count = std::popcount(squad.members),
                       ordinal = std::popcount(squad.members & ((1u << slot) - 1));
        for (unsigned i = ordinal; i < access.landings.size(); i += count)
            entries |= around(access.landings[i], rules::crystal_range);
        entries &= access.entries;
        if (!entries.is_set(position)) {
            squad.bank_stage = BankStage::travel;
            publish_squad();
            squad_move(entries);
            squad_combat();
            return;
        }
        squad.bank_stage = BankStage::ready;
        publish_squad();
        vec2 landing;
        if (can_protect_channel(rules::settings().tp_crystal_time) &&
            !me.restrictions().contains(bc::buff_restrictions::consumable)) {
            while (teleport_assignment(landing)) {
                if (incoming(landing) * rules::settings().tp_crystal_time < health &&
                    me.consumables()[bc::consumable_type::tp_crystal].use(landing)) {
                    squad.bank_stage = BankStage::crossing;
                    if constexpr (policy::diagnostic)
                        std::printf("BANK_TELEPORT r=%u slot=%u altar=%d landing=%d,%d\n", round, slot,
                                    squad.altar, landing.x, landing.y);
                    publish_squad();
                    return;
                }
                blocked.set(landing); // Retry another owned landing after hidden occupancy.
            }
        }
        squad_combat();
        return;
    }
    if (squad.bank_stage == BankStage::crossing || squad.bank_stage == BankStage::crossed) {
        squad.bank_stage = BankStage::crossed;
        publish_squad();
        if (!crossing_ready()) {
            squad_combat();
            return;
        }
        squad.bank_stage = BankStage::assembled;
        squad.progress = round;
        publish_squad();
    }
    // Travel is an executable part of the committed job. Rebuilding a full
    // three-carrier schedule every step can starve the walk that makes that
    // schedule possible. Assign distinct final positions once members arrive.
    if (distance(position, altars[squad.altar]) > rules::settings().submit_gems_range) {
        squad.bank_stage = BankStage::assembled;
        publish_squad();
        squad_move(around(altars[squad.altar], rules::settings().submit_gems_range));
        squad_combat();
        return;
    }
    const auto plan = estimate_bank(squad.altar);
    if (!plan.feasible) {
        squad_combat();
        return;
    }
    if (clear_altar_lock(squad.altar)) {
        squad.progress = round;
        return;
    }
    if (!gems)
        squad.bank_stage = BankStage::done;
    else
        squad.bank_stage = BankStage::assembled;
    bool complete = !gems;
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (i == slot || !(squad.participants & living_core() & (1u << i)))
            continue;
        complete &= same_job(shared.get(i)) &&
                    BankStage(TeamMemory::deposit_stage(shared.get(i), groups.size())) == BankStage::done;
    }
    if (complete && squad.coordinator == int(slot)) {
        if constexpr (policy::diagnostic)
            std::printf("BANK_DONE r=%u slot=%u altar=%d recall=%d\n", round, slot, squad.altar,
                        plan.recall_exit);
        squad.review = -1;
        squad.cargo_deadline = -1;
        if (plan.recall_exit || needs_recovery()) {
            squad.altar = -1;
            begin_recovery();
            squad_recover();
        } else {
            squad.resume_camp = -1;
            enter_selection();
        }
        return;
    }
    if (plan.guards & (1u << slot)) {
        // An empty member can guard without withdrawing its completion ack.
        squad.bank_stage = gems ? BankStage::guard : BankStage::done;
        publish_squad();
        squad_combat();
        return;
    }
    bool acknowledged = true;
    for (unsigned i = 0; i < roster.size(); ++i)
        if ((plan.guards & (1u << i)) && !enemies.empty()) {
            const auto *member = observed_member(i);
            const auto stage = BankStage(TeamMemory::deposit_stage(shared.get(i), groups.size()));
            acknowledged &= member && !member->channel && same_job(shared.get(i)) &&
                            (stage == BankStage::guard || stage == BankStage::done);
        }
    if (gems && acknowledged && plan.starts[slot] <= 1 && can_protect_channel(channel_duration(gems))) {
        if (deposit_at(squad.altar) == ActionResult::progress) {
            squad.bank_stage = me.is_submitting_gems() ? BankStage::channel : BankStage::assembled;
            squad.progress = round;
            if constexpr (policy::diagnostic)
                std::printf("BANK_CHANNEL r=%u slot=%u gems=%u finish=%d guards=%u\n", round, slot, gems,
                            plan.finishes[slot], plan.guards);
            publish_squad();
            return;
        }
    }
    publish_squad();
    squad_combat();
}
