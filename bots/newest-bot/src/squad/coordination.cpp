// Shared-memory job claims, leadership, progress supervision and focus.
#include "../bot.h"

float Bot::collateral_risk(vec2 centre, float radius, int engaged_group) const {
    if (!squad_member())
        return 0;
    float damage = 0;
    int previous_group = -1;
    // Observations are ordered by camp group. A splash wakes the entire group,
    // including monsters outside the spell radius, not just the struck monster.
    for (const auto &struck : camps) {
        if (struck.group == previous_group || struck.health <= 0 ||
            distance(centre, struck.position) > radius)
            continue;
        previous_group = struck.group;
        if (struck.group == engaged_group)
            continue;
        if (groups[struck.group].health < groups[struck.group].max_health ||
            groups[struck.group].attacked >= int(round) - 1)
            continue;
        for (const auto &camp : camps) {
            if (camp.group != struck.group || camp.health <= 0 || camp.strength <= 0)
                continue;
            int nearest = distance(position, camp.position);
            for (const auto &ally : allies)
                nearest = std::min(nearest, distance(ally.position, camp.position));
            bool enemy_tanks = false;
            for (const auto &enemy : enemies)
                enemy_tanks |= distance(enemy.position, camp.position) < nearest;
            if (!enemy_tanks && nearest <= camp.range)
                damage += camp.strength * camp_mitigation * mitigation;
        }
    }
    return damage * bc::constants::camp_sleep_time;
}

FiringOpportunity Bot::firing(const ObservedUnit &member, const ObservedUnit &enemy, bool next_turn) const {
    FiringOpportunity result;
    if (member.health <= 0 || member.channel || (!next_turn && member.completed) ||
        (member.stunned && member.stun_time > 0))
        return result;
    const int reach = distance(member.position, enemy.position);
    const bool moved = !next_turn && member.movement_spent;
    if (squad_member() && !member.sight.is_set(enemy.position))
        return result;
    float basic = (next_turn || member.can_attack) && reach <= member.range ? member.strength : 0;
    float spell = 0;
    if (next_turn || member.can_cast)
        for (unsigned i = 0; i < member.abilities.size(); ++i) {
            const auto &ability = member.abilities[i];
            if (member.cooldowns[i] > (next_turn ? 1 : 0) || (moved && ability.exclusive))
                continue;
            if (ability.kind == bc::ability_kind::attack_reset && reach <= member.range)
                spell = std::max(spell, member.strength);
            if (reach > ability.range + ability.radius)
                continue;
            if (ability.kind == bc::ability_kind::aoe_damage) {
                bool centre_available = false;
                for (int dx = -int(ability.radius); dx <= int(ability.radius) && !centre_available; ++dx)
                    for (int dy = -int(ability.radius); dy <= int(ability.radius); ++dy) {
                        const auto centre = enemy.position + vec2{dx, dy};
                        if (bc::map::is_in_bounds(centre) && !static_blocked.is_set(centre) &&
                            distance(member.position, centre) <= ability.range) {
                            centre_available = true;
                            break;
                        }
                    }
                if (!centre_available)
                    continue;
            }
            if (ability.kind == bc::ability_kind::damage || ability.kind == bc::ability_kind::cc ||
                ability.kind == bc::ability_kind::aoe_damage)
                spell = std::max(spell, member.focus * ability.modifier);
            if (ability.duration > 0 && !enemy.immune && (!enemy.stunned || enemy.stun_time <= 0))
                result.control = std::max(result.control, float(ability.duration));
        }
    result.damage = (basic + spell) * enemy.mitigation;
    return result;
}

unsigned Bot::living_core() const {
    unsigned mask = 0;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i) && roster[i].is_alive())
            mask |= 1u << i;
    return mask;
}

const ObservedUnit *Bot::observed_member(unsigned member) const {
    for (const auto &ally : allies)
        if (std::get<bc::champion>(ally.ref) == roster[member])
            return &ally;
    return nullptr;
}

unsigned Bot::squad_claim() const {
    if (!bc::self().is_alive())
        return TeamMemory::unavailable;
    if (squad.altar >= 0 && (squad.phase == SquadPhase::deposit || squad.phase == SquadPhase::recover))
        return TeamMemory::deposit(squad.altar, unsigned(squad.bank_stage), groups.size());
    if (squad.phase == SquadPhase::recover)
        return squad.bank_stage == BankStage::prepared ? TeamMemory::recovered : TeamMemory::recall;
    if (squad.camp < 0)
        return squad.access_preparation ? TeamMemory::supplies_needed : 0;
    if (groups[squad.camp].status == CampGroup::capture && groups[squad.camp].tiles.is_set(position))
        return TeamMemory::holding(squad.camp, groups.size());
    return squad.phase == SquadPhase::travel && TeamMemory::formations(groups.size())
               ? TeamMemory::joining(squad.camp, groups.size())
               : unsigned(squad.camp + 1);
}

void Bot::publish_squad(bool reset_focus) {
    if (reset_focus && squad.coordinator == int(slot))
        shared.commit(slot, squad_claim(), 0);
    else
        publish(squad_claim());
}

bool Bot::same_job(unsigned code) const {
    if (squad.altar >= 0 && (squad.phase == SquadPhase::deposit || squad.phase == SquadPhase::recover))
        return TeamMemory::deposit_altar(code, groups.size(), altars.size()) == squad.altar;
    if (squad.phase == SquadPhase::recover)
        return code == TeamMemory::recall || code == TeamMemory::recovered;
    return squad.camp >= 0 && TeamMemory::objective(code, groups.size()) == squad.camp;
}

int Bot::work_window() const {
    return std::max(bc::constants::capture_time + 1, std::max(spells[0].cooldown, spells[1].cooldown));
}

void Bot::enter_selection() {
    squad.phase = SquadPhase::select;
    squad.camp = target = squad.altar = -1;
    squad.need_crystal = false;
    squad.bank_stage = BankStage::travel;
    squad.route_best = bc::distance_field::unreachable;
    squad.motion = {};
    job_search = {};
    active_search = {};
    squad.plan = {};
    squad.arrival_deadline = squad.crossing_deadline = -1;
    squad.crossed = 0;
    travel_route.revision = ~0u;
    publish_squad(true);
}

void Bot::begin_recovery() {
    if (squad.phase != SquadPhase::recover) {
        squad.resume_camp = squad.camp;
        squad.changed = squad.progress = round;
        squad.route_best = bc::distance_field::unreachable;
        squad.bank_stage = BankStage::preparing;
        squad.phase = SquadPhase::recover;
        job_search = {};
    }
    // Preparing retains the selected altar. It says nothing about which
    // supplies are missing: infer crystal need from this actor's actual route.
    squad.need_crystal = squad.altar >= 0 && squad.altar < int(altar_access.size()) &&
                         !altar_access[squad.altar].region.is_set(position) &&
                         !bc::self().consumables()[bc::consumable_type::tp_crystal].count();
    publish_squad(true);
}

void Bot::adopt_job(unsigned code) {
    if (code == TeamMemory::unavailable)
        return;
    if (code == TeamMemory::supplies_needed) {
        squad.access_preparation = true;
        enter_selection();
        return;
    }
    const int altar = TeamMemory::deposit_altar(code, groups.size(), altars.size());
    if (altar >= 0) {
        const auto stage = BankStage(TeamMemory::deposit_stage(code, groups.size()));
        const bool preparing = stage == BankStage::preparing || stage == BankStage::prepared;
        const bool changed = squad.altar != altar ||
                             (squad.phase != SquadPhase::deposit && squad.phase != SquadPhase::recover);
        if (changed) {
            squad.resume_camp = squad.camp;
            squad.altar = altar;
            squad.bank_stage = BankStage::travel;
            squad.changed = squad.progress = round;
            squad.route_best = bc::distance_field::unreachable;
            squad.participants = living_core();
            squad.crossed = 0;
            squad.crossing_deadline = -1;
            squad.arrival_deadline = -1;
            travel_route.revision = ~0u;
        }
        squad.access_preparation = squad.upgrade_preparation = false;
        if (preparing)
            begin_recovery();
        else {
            if (squad.phase == SquadPhase::recover) {
                squad.bank_stage = BankStage::travel;
                squad.crossed = 0;
                squad.crossing_deadline = squad.arrival_deadline = -1;
            }
            squad.phase = SquadPhase::deposit;
            squad.need_crystal = false;
        }
        return;
    }
    if (code == TeamMemory::recall || code == TeamMemory::recovered) {
        // A plain recovery directive means the previous delivery is finished
        // or abandoned. Followers must acknowledge the same recovery job.
        squad.altar = -1;
        squad.need_crystal = false;
        begin_recovery();
        return;
    }
    const int camp = TeamMemory::objective(code, groups.size());
    if (camp >= 0 && camp < int(groups.size())) {
        if (camp != squad.camp || squad.phase == SquadPhase::deposit || squad.phase == SquadPhase::recover) {
            squad.upgrade_preparation = false;
            squad.changed = squad.progress = round;
            squad.route_best = bc::distance_field::unreachable;
            squad.phase = SquadPhase::travel;
            squad.participants = living_core();
            job_search = {};
            active_search = {};
            squad.plan = {};
            squad.arrival_deadline = -1;
            travel_route.revision = ~0u;
        }
        squad.camp = target = camp;
        squad.altar = -1;
        squad.need_crystal = false;
    } else if (!code && (squad.phase != SquadPhase::select || squad.camp >= 0 || squad.altar >= 0)) {
        enter_selection();
    }
}

void Bot::reconcile_squad() {
    shared.read();
    const auto alive = living_core();
    squad.last_positions[slot] = position;
    squad.last_seen[slot] = round;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i))
            if (const auto *member = observed_member(i)) {
                squad.last_positions[i] = member->position;
                squad.last_seen[i] = round;
            }
    int owner = -1;
    // Select (claim zero) is a directive, and retains leadership.
    for (unsigned i = 0; i < roster.size(); ++i)
        if ((alive & (1u << i)) && shared.get(i) != TeamMemory::unavailable) {
            owner = i;
            break;
        }
    if (owner < 0)
        for (unsigned i = 0; i < roster.size(); ++i)
            if (alive & (1u << i)) {
                owner = i;
                break;
            }
    const int previous_owner = squad.coordinator;
    squad.coordinator = owner;
    if (owner >= 0 && owner != int(slot))
        adopt_job(shared.get(owner));
    else if (owner == int(slot) && previous_owner >= 0 && previous_owner != owner &&
             (alive & (1u << previous_owner)))
        adopt_job(shared.get(previous_owner));
    // Only a new, observed emergency requests a change to the owner's job.
    // A delayed acknowledgement of an older operation cannot resurrect it.
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (!squad.contains(i) || i == slot)
            continue;
        const unsigned code = shared.get(i);
        const bool fresh = code != squad.requests[i];
        if (fresh) {
            squad.requests[i] = code;
            squad.request_seen[i] = round;
        }
        if (owner != int(slot) || !fresh || !(alive & (1u << i)))
            continue;
        const auto *member = observed_member(i);
        const int altar = TeamMemory::deposit_altar(code, groups.size(), altars.size());
        const bool preparing =
            altar >= 0 && BankStage(TeamMemory::deposit_stage(code, groups.size())) == BankStage::preparing;
        if (member && (code == TeamMemory::recall || preparing) &&
            (member->health < member->max_health * policy::retreat_health || roster[i].is_recalling() ||
             (preparing && altar == squad.altar)))
            begin_recovery();
        if (member && code == TeamMemory::supplies_needed &&
            bc::map::spawn_region(bc::team::ally).is_set(member->position)) {
            squad.access_preparation = true;
            enter_selection();
        }
        if (member && member->gems && code != squad_claim())
            squad.review = -1;
    }
    squad.participants &= alive;
    squad.participants |= 1u << slot;
    for (unsigned i = 0; i < roster.size(); ++i) {
        if (i == slot || !(alive & (1u << i)))
            continue;
        if (observed_member(i) && same_job(shared.get(i)))
            squad.participants |= 1u << i;
        else if (!(squad.phase == SquadPhase::deposit && same_job(shared.get(i))) &&
                 int(round) - std::max(squad.last_seen[i], squad.changed) >
                     work_window() + int(stats.vision_radius))
            squad.participants &= ~(1u << i);
    }
    if (squad.camp >= 0 && squad.phase != SquadPhase::deposit && squad.phase != SquadPhase::recover) {
        target = squad.camp;
        const auto &group = groups[target];
        if (group.status == CampGroup::capture)
            squad.phase = SquadPhase::capture;
        else if (distance(position, group.centre) <= stats.vision_radius)
            squad.phase = SquadPhase::clear;
        else
            squad.phase = SquadPhase::travel;
    }
}

void Bot::supervise_progress() {
    const unsigned score = bc::global_submitted_gems(bc::team::enemy);
    if (score > squad.enemy_score) {
        const float pace = (score - squad.enemy_score) / float(std::max(1, int(round) - squad.score_round));
        squad.enemy_pace = squad.enemy_pace ? (squad.enemy_pace + pace) * 0.5f : pace;
        squad.enemy_score = score;
        squad.score_round = round;
        squad.next_job_review = std::min(squad.next_job_review, int(round));
    }
    if (squad.coordinator != int(slot) || squad.phase == SquadPhase::recover)
        return;
    const bool scheduled_wait =
        squad.motion.status == MoveStatus::waiting && squad.motion.until >= int(round);
    const bool arriving = squad.phase == SquadPhase::deposit && squad.arrival_deadline >= int(round);
    const bool expired = !scheduled_wait && !arriving &&
                         int(round) - squad.progress > work_window() + int(stats.base_move_speed);
    if (!expired)
        return;
    if constexpr (policy::diagnostic)
        if (int(round) - squad.motion_since >= work_window()) {
            std::printf("SQUAD_BLOCK r=%u slot=%u phase=%d camp=%d altar=%d move=%d reason=%d\n", round, slot,
                        int(squad.phase), squad.camp, squad.altar, int(squad.motion.status),
                        int(squad.motion.reason));
            squad.motion_since = round;
        }
    if (squad.phase == SquadPhase::deposit) {
        squad.failed_altar = squad.altar;
        squad.altar_retry = round + work_window();
        const auto alternative = choose_bank();
        if (alternative.altar >= 0 && alternative.altar != squad.altar) {
            squad.altar = alternative.altar;
            squad.changed = squad.progress = round;
            squad.route_best = bc::distance_field::unreachable;
            squad.bank_stage = BankStage::travel;
            squad.crossed = 0;
            squad.arrival_deadline = round + alternative.completion + work_window();
            squad.crossing_deadline = round + alternative.travel + alternative.access + work_window();
            if (alternative.supply)
                begin_recovery();
            publish_squad(true);
        } else
            begin_recovery();
    } else if (squad.camp >= 0 && squad.motion.status != MoveStatus::deferred) {
        if (squad.failed_camp != squad.camp || int(round) >= squad.retry_after) {
            squad.failed_camp = squad.camp;
            squad.retry_after = round + work_window() + int(stats.vision_radius);
        }
        enter_selection();
    }
}

bool Bot::relevant_enemy(const ObservedUnit &enemy) const {
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    if (enemy.health <= 0 || enemy_spawn.is_set(enemy.position))
        return false;
    if (enemy.depositing &&
        enemy.gems + bc::global_submitted_gems(bc::team::enemy) >= bc::constants::winning_gems)
        return distance(position, enemy.position) <=
               std::max(spells[0].range, spells[1].range) + stats.base_move_speed;
    const int reach = std::max(enemy.range, enemy.spell_reach) + int(stats.base_move_speed);
    if (distance(position, enemy.position) <= reach)
        return true;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i))
            if (const auto *member = observed_member(i))
                if (distance(member->position, enemy.position) <= reach)
                    return true;
    if (squad.phase == SquadPhase::deposit && squad.altar >= 0)
        return distance(altars[squad.altar], enemy.position) <= reach + bc::constants::submit_gems_range;
    if (squad.camp >= 0 && squad.phase != SquadPhase::recover)
        for (const auto &camp : groups[squad.camp].camps)
            if (distance(camp.position, enemy.position) <= reach)
                return true;
    return false;
}

float Bot::control_value(const ObservedUnit &enemy) const {
    if (!relevant_enemy(enemy) || enemy.immune || (enemy.stunned && enemy.stun_time > 0))
        return 0;
    float value = 0;
    if (!enemy.channel) {
        const int reach = std::max(enemy.range, enemy.spell_reach) + int(stats.base_move_speed);
        if (distance(enemy.position, position) <= reach)
            value = enemy.strength + enemy.burst;
        for (unsigned i = 0; i < roster.size(); ++i)
            if (squad.contains(i))
                if (const auto *member = observed_member(i))
                    if (distance(enemy.position, member->position) <= reach)
                        value = std::max(value, enemy.strength + enemy.burst);
    }
    if (enemy.depositing)
        value += enemy.gems;
    // An enemy already finished this round still loses its next opportunity
    // while stunned. No reward for control on an irrelevant distant runner.
    return value;
}

void Bot::squad_focus() {
    if (squad.coordinator != int(slot))
        return;
    unsigned preferred = 0;
    float best = 0;
    const ObservedUnit self(*this);
    for (const auto &enemy : enemies) {
        if (!relevant_enemy(enemy))
            continue;
        float ready = firing(self, enemy).damage;
        for (unsigned i = 0; i < roster.size(); ++i)
            if (squad.contains(i) && same_job(shared.get(i)))
                if (const auto *member = observed_member(i)) {
                    // Opposing turns interleave every allied action. Exposed
                    // allies cannot promise a volley they may not survive.
                    if (member->health > enemy.burst * enemy.mitigation || enemy.completed || enemy.stunned)
                        ready += firing(*member, enemy).damage;
                }
        float score = ready / std::max(1.0f, enemy.health + enemy.shield);
        if (ready >= enemy.health + enemy.shield)
            score += 2;
        score += control_value(enemy) / std::max(1.0f, enemy.max_health);
        for (unsigned i = 0; i < enemy_roster.size(); ++i)
            if (std::get<bc::champion>(enemy.ref) == enemy_roster[i]) {
                if (shared.focus() == i + 1)
                    score *= policy::commit_bonus;
                if (score > best) {
                    best = score;
                    preferred = i + 1;
                }
            }
    }
    shared.commit(slot, squad_claim(), preferred);
}

void Bot::squad_priority() {
    auto &me = bc::self();
    if (!me.is_alive() || bc::fuel_remaining() < policy::planning_reserve)
        return;
    int priority = 1000 - int(slot);
    if (me.is_submitting_gems())
        priority += int(gems); // Completion can resolve before an enemy lock.
    else if (!me.is_recalling() && !me.is_teleporting()) {
        const ObservedUnit self(*this);
        for (const auto &enemy : enemies) {
            if (bc::fuel_remaining() < policy::planning_reserve)
                break;
            if (!relevant_enemy(enemy))
                continue;
            const auto next = firing(self, enemy, true);
            float value = next.damage;
            if (next.damage >= enemy.health + enemy.shield)
                value += kill_value(enemy);
            if (next.control > 0)
                value += control_value(enemy);
            priority = std::max(priority, 2000 + int(value));
        }
    }
    if (me.priority() != priority)
        me.set_priority(priority);
}
