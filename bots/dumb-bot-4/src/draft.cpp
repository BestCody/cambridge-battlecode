#include "draft.h"
#include "strategy.h"
#include <iostream>

namespace {
constexpr std::array roster{std::pair{bc::champion_draft::varg, bc::champion_type::varg},
                            std::pair{bc::champion_draft::vendetta, bc::champion_type::vendetta},
                            std::pair{bc::champion_draft::tavi, bc::champion_type::tavi},
                            std::pair{bc::champion_draft::stellar, bc::champion_type::stellar},
                            std::pair{bc::champion_draft::cassi_and_issac, bc::champion_type::cassi},
                            std::pair{bc::champion_draft::willow, bc::champion_type::willow},
                            std::pair{bc::champion_draft::marcella, bc::champion_type::marcella},
                            std::pair{bc::champion_draft::ryth, bc::champion_type::ryth},
                            std::pair{bc::champion_draft::vesper_vale, bc::champion_type::vesper_vale},
                            std::pair{bc::champion_draft::ren, bc::champion_type::ren},
                            std::pair{bc::champion_draft::prism, bc::champion_type::prism}};
struct DraftState {
    bool ready = false;
    DraftPlan plan;
    std::vector<bc::map::camp_group> groups;
    std::vector<int> friendly_distance, enemy_distance;
    std::vector<float> hp, pressure, splash, gems, gold;
    std::vector<unsigned> used;
    std::array<std::vector<float>, static_cast<unsigned>(bc::champion_type::tavi) + 1> job_rates;
    unsigned bodies = 0;
    void initialise() {
        groups = bc::map::camp_groups();
        auto walls = bc::map::walls() | bc::map::holes() | bc::map::altars();
        bc::distance_field home, away;
        bc::bitboard visited;
        home.compute(walls, bc::map::spawn_region(bc::team::ally), visited);
        away.compute(walls, bc::map::spawn_region(bc::team::enemy), visited);
        float dangerous = 0, total_splash = 0, contested = 0, total_weight = 0;
        unsigned productive = 0;
        for (const auto &g : groups) {
            int d = bc::distance_field::unreachable, e = d;
            float h = 0, p = 0, area = 1, toughest = 1;
            for (const auto &c : g) {
                auto pos = c.spawn_position();
                auto stats = c.base_stats();
                d = std::min(d, int(home.get(pos)));
                e = std::min(e, int(away.get(pos)));
                h += stats.base_health;
                p += stats.base_strength;
                toughest = std::max(toughest, stats.base_health);
                float hits = 0;
                for (const auto &other : g)
                    if (distance(pos, other.spawn_position()) <=
                        bc::constants::passive_abilities::varg_splash_radius)
                        ++hits;
                area = std::max(area, hits);
            }
            area = std::min(area, h / toughest);
            friendly_distance.push_back(d);
            enemy_distance.push_back(e);
            hp.push_back(h);
            gems.push_back(g.gem_value());
            gold.push_back(g.gold_value());
            pressure.push_back(p);
            splash.push_back(area);
            used.push_back(0);
            if (d != bc::distance_field::unreachable && e != bc::distance_field::unreachable) {
                float weight =
                    (gems.back() + gold.back() * policy::early_gold_value) / std::max(1.0f, h);
                contested += weight * (std::min(d, e) + 1) / float(std::max(d, e) + 1);
                total_weight += weight;
            }
            if (p > 0)
                ++dangerous;
            if (gems.back() > 0)
                ++productive;
            total_splash += area;
        }
        plan.group_count = groups.size();
        if (!groups.empty()) {
            plan.mean_splash = total_splash / groups.size();
            plan.dangerous_fraction = dangerous / groups.size();
        }
        plan.contestation = total_weight > 0 ? contested / total_weight : 0;
        plan.spacing = std::sqrt(float((~walls).count()) / std::max(1u, productive));
        plan.assault = !policy::adaptive_formation || plan.spacing <= policy::squad_spacing;
        plan.rush_size = plan.assault ? policy::rush_size : 0;
        plan.squad_size = plan.assault ? policy::squad_size : 0;
        plan.focus_weight = plan.assault ? policy::focus_weight : 1;
        if (!plan.assault)
            prepare_jobs();
        ready = true;
    }
    void prepare_jobs() {
        auto prepare = [&](bc::champion_type physical) {
            bc::champion unit(physical);
            const auto design = unit.base_stats();
            const auto abilities = unit.abilities();
            const std::array spells{describe(abilities[0]), describe(abilities[1])};
            const bool splashing = unit.passive() == bc::passive_type::splash_attack;
            auto &rates = job_rates[static_cast<unsigned>(physical)];
            rates.resize(groups.size());
            for (unsigned i = 0; i < groups.size(); ++i) {
                if (friendly_distance[i] == bc::distance_field::unreachable)
                    continue;
                const float area = splashing ? splash[i] : 1;
                float damage = spell_dps(spells, design.base_strength, design.base_focus, area);
                if (!splashing)
                    for (const auto &spell : spells)
                        if (spell.kind == bc::ability_kind::aoe_damage)
                            damage += design.base_focus * spell.modifier * (splash[i] - 1) / spell.cooldown;
                const float clear = hp[i] / std::max(1.0f, damage * design.camp_damage_multiplier);
                float loss = pressure[i] * clear / std::max(1.0f, area);
                for (const auto &spell : spells)
                    if ((spell.kind == bc::ability_kind::shield || spell.kind == bc::ability_kind::heal) &&
                        spell.targets.contains(bc::ability_target_mask::self))
                        loss -= design.base_focus * spell.modifier * (1 + clear / spell.cooldown);
                const float risk = std::max(0.0f, loss - design.base_health * (1 - policy::casualty_margin)) /
                                   std::max(1.0f, design.base_health);
                const float reward = gems[i] + policy::spread_gold_value * gold[i];
                const float duration = friendly_distance[i] + clear + bc::constants::capture_time + 1;
                rates[i] = reward / std::max(1.0f, duration) / (1 + risk * 4);
            }
        };
        for (auto [draft, physical] : roster)
            prepare(physical);
        prepare(bc::champion_type::issac);
    }
    std::pair<float, int> job_rate(bc::champion_type physical, int excluded = -1) const {
        const auto &rates = job_rates[static_cast<unsigned>(physical)];
        std::pair<float, int> best{0, -1};
        for (unsigned i = 0; i < rates.size(); ++i) {
            if (int(i) == excluded)
                continue;
            const float rate = rates[i] / (1 + used[i] * policy::partner_penalty);
            if (rate > best.first)
                best = {rate, i};
        }
        return best;
    }
};
} // namespace

void play_draft() {
    static DraftState state;
    if (!state.ready)
        state.initialise();
    const auto chosen = bc::draft::current_draft(bc::team::ally);
    float enemy_stuns = 0, enemy_area = 0;
    if constexpr (policy::draft_counter_weight > 0) {
        for (auto enemy : bc::draft::current_draft(bc::team::enemy))
            for (auto [draft, physical] : roster)
                if (enemy == draft) {
                    bc::champion counter(physical);
                    for (const auto &a : counter.abilities()) {
                        auto s = describe(a);
                        if (s.kind == bc::ability_kind::cc || s.enhanced ||
                            (s.kind == bc::ability_kind::aoe_damage && s.duration > 0))
                            ++enemy_stuns;
                        if (s.kind == bc::ability_kind::aoe_damage)
                            ++enemy_area;
                    }
                }
    }
    auto pick = bc::champion_draft::varg;
    auto type = bc::champion_type::varg;
    float best = -1;
    for (auto [candidate, physical] : roster) {
        if (std::find(chosen.begin(), chosen.end(), candidate) != chosen.end())
            continue;
        if (best >= 0 && bc::fuel_remaining() < policy::planning_reserve)
            break; // Leave room to assign every physical body and submit the pick.
        if (!state.plan.assault) {
            // Travel and capture are independent work: compare two distinct
            // Twin jobs with one champion's job, rather than summing raw DPS.
            const auto first = state.job_rate(physical);
            float score = first.first;
            if (physical == bc::champion_type::cassi)
                score += state.job_rate(bc::champion_type::issac, first.second).first;
            if (score > best) {
                best = score;
                pick = candidate;
                type = physical;
            }
            continue;
        }
        bc::champion c(physical);
        auto stats = c.base_stats();
        auto abilities = c.abilities();
        std::array spells{describe(abilities[0]), describe(abilities[1])};
        float area = c.passive() == bc::passive_type::splash_attack ? state.plan.mean_splash : 1;
        float score =
            spell_dps(spells, stats.base_strength, stats.base_focus, area) + stats.base_health / 75.0f;
        if (stats.base_attack_range <= 1)
            score += policy::draft_melee_bonus;
        for (const auto &s : spells) {
            if (s.kind == bc::ability_kind::aoe_damage && c.passive() != bc::passive_type::splash_attack)
                score += stats.base_focus * s.modifier * (state.plan.mean_splash - 1) / s.cooldown;
            if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                s.kind == bc::ability_kind::aoe_damage)
                score += ((state.plan.assault ? policy::draft_spell_weight : 1) - 1) * stats.base_focus *
                         s.modifier / s.cooldown *
                         (s.kind == bc::ability_kind::aoe_damage ? state.plan.mean_splash : 1);
            if (s.kind == bc::ability_kind::heal)
                score += policy::draft_heal_bonus;
            if (s.kind == bc::ability_kind::shield)
                score += stats.base_focus * s.modifier / s.cooldown * 0.6f;
            if (s.kind == bc::ability_kind::dash)
                score += 9;
            if (s.kind == bc::ability_kind::cc || s.enhanced)
                score += 8;
            if (s.kind == bc::ability_kind::heal)
                score += chosen.empty()
                             ? 0
                             : stats.base_focus * s.modifier / s.cooldown * state.plan.dangerous_fraction;
        }
        if (c.passive() == bc::passive_type::remote_shop)
            score += 22;
        if (c.passive() == bc::passive_type::reset_cooldowns_on_kill)
            score += 11;
        if (c.passive() == bc::passive_type::retaliation) {
            score += policy::draft_retaliation_bonus;
            // Retaliation is offensive output too: clustered attackers each
            // trigger it. Estimate exposure from map contestation and roster
            // size rather than ignoring this passive in a team-fight draft.
            const float attackers = 1 + state.plan.contestation * (bc::draft::team_size() - 1);
            if (state.plan.assault)
                score += stats.base_focus * bc::constants::passive_abilities::ren_retaliation_focus_ratio *
                         attackers;
        }
        if (c.passive() == bc::passive_type::stun_immunity)
            score += enemy_stuns * policy::draft_counter_weight;
        if (physical == bc::champion_type::cassi) {
            bc::champion twin(bc::champion_type::issac);
            auto t = twin.base_stats();
            score += t.base_strength * 0.9f + 18 * (1 - state.plan.dangerous_fraction) - 12;
            score -= enemy_area * policy::draft_counter_weight * 0.35f;
        }
        // Prefer a healer after two damage dealers on maps dominated by hard camps.
        if (physical == bc::champion_type::willow && chosen.size() >= 2)
            score += 18 * state.plan.dangerous_fraction;
        if (state.plan.assault) {
            // An opening squad must keep delivering its damage through the
            // enemy volley. Use the same health-times-offense model as fight
            // selection instead of treating a frontline's health as a small
            // additive draft bonus. Scattered farming keeps its existing draft.
            float fighting_health = stats.base_health;
            for (const auto &spell : spells)
                if (spell.kind == bc::ability_kind::shield)
                    fighting_health += stats.base_focus * spell.modifier;
            if (physical == bc::champion_type::cassi)
                // Either twin dying removes both bodies. Their joint damage
                // remains useful, but a focused attack only has to exhaust
                // the smaller health pool to remove it from the fight.
                fighting_health = std::min(fighting_health,
                    bc::champion(bc::champion_type::issac).base_stats().base_health);
            score = std::sqrt(std::max(1.0f, score - stats.base_health / 75.0f) * fighting_health);
        }
        if (score > best) {
            best = score;
            pick = candidate;
            type = physical;
        }
    }
    auto assign = [&](bc::champion_type physical) {
        if (state.bodies < state.plan.roster_types.size())
            state.plan.roster_types[state.bodies] = physical;
        bc::champion c(physical);
        auto s = c.base_stats();
        auto a = c.abilities();
        std::array spells{describe(a[0]), describe(a[1])};
        float best_value = -1;
        int target = -1;
        bool raiding = state.bodies < state.plan.rush_size;
        if (raiding && state.plan.rally_group)
            target = state.plan.rally_group - 1;
        for (std::size_t i = 0; i < state.groups.size(); ++i) {
            if (raiding && state.plan.rally_group)
                break;
            if (state.friendly_distance[i] == bc::distance_field::unreachable)
                continue;
            float area = c.passive() == bc::passive_type::splash_attack ? state.splash[i] : 1;
            float dps = spell_dps(spells, s.base_strength, s.base_focus, area);
            float clear = state.hp[i] / std::max(1.0f, dps * (raiding ? state.plan.rush_size : 1));
            float loss = state.pressure[i] * clear / std::max(1.0f, area);
            for (const auto &sp : spells)
                if (sp.kind == bc::ability_kind::shield)
                    loss -= s.base_focus * sp.modifier * (1 + clear / sp.cooldown);
            float danger = std::max(0.0f, loss - s.base_health * 0.65f) / s.base_health;
            float reward =
                state.gems[i] + policy::early_gold_value * state.gold[i];
            float value = reward / (state.friendly_distance[i] + clear + bc::constants::capture_time + 8);
            value /= 1 + danger * 4 + state.used[i] * 2.5f;
            if (state.enemy_distance[i] <= state.friendly_distance[i] + 8)
                value *= 1 + policy::contest_bonus;
            if (raiding) {
                float timing = std::abs(state.friendly_distance[i] - state.enemy_distance[i]);
                value *= 1 + policy::rush_contest / (1 + timing / 10);
            }
            if (value > best_value) {
                best_value = value;
                target = i;
            }
        }
        if (target >= 0) {
            state.plan.opening[static_cast<unsigned>(physical)] = target + 1;
            ++state.used[target];
            if (raiding && !state.plan.rally_group) {
                state.plan.rally_group = target + 1;
                float area = c.passive() == bc::passive_type::splash_attack ? state.splash[target] : 1;
                state.plan.rally_until = std::min<unsigned>(
                    bc::constants::max_rounds / 10,
                    state.friendly_distance[target] +
                        state.hp[target] / spell_dps(spells, s.base_strength, s.base_focus, area) +
                        bc::constants::capture_time * 3);
            }
        }
        ++state.bodies;
    };
    assign(type);
    if (pick == bc::champion_draft::cassi_and_issac)
        assign(bc::champion_type::issac);
    bc::draft_memory = std::span{&state.plan, 1};
    if constexpr (policy::diagnostic)
        std::cout << "DRAFT " << bc::to_string(pick) << " value=" << best
                  << " danger=" << state.plan.dangerous_fraction << " splash=" << state.plan.mean_splash
                  << " contest=" << state.plan.contestation << " spacing=" << state.plan.spacing
                  << " assault=" << state.plan.assault << " fuel=" << bc::fuel_remaining() << '\n';
    bc::draft::pick(pick);
}
