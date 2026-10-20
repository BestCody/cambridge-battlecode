// Draft: choose the offensive or resource roster from the map, pick it, and
// write the opening plan to draft memory.
#include "draft.h"
#include "../strategy.h"
#include "team_compositions.h"
#include <cstdio>
#include <iostream>

namespace {
// Draft-time map inputs for the offense/resource decision. Every one is
// computed from the public map, so it works on any generated map.
struct DraftInputs {
    unsigned picks = 0;
    float tiles_per_champion = 0;
    int spawn_distance = bc::distance_field::unreachable;
    unsigned middle_altars = 0, side_altars = 0;
    float own_gem_share = 0, supply_per_champion = 0, grouped_gem_share = 0;
};

// Walking-distance balance between the spawns: 0 is the exact middle.
float side_balance(int ours, int theirs) {
    if (ours >= bc::distance_field::unreachable || theirs >= bc::distance_field::unreachable)
        return 0;
    return float(ours - theirs) / float(std::max(1, ours + theirs));
}

DraftInputs measure_map() {
    DraftInputs in;
    const auto terrain = bc::map::walls() | bc::map::holes() | bc::map::altars();
    bc::bitboard visited;
    bc::distance_field ally, enemy;
    ally.compute(terrain, bc::map::spawn_region(bc::team::ally), visited);
    enemy.compute(terrain, bc::map::spawn_region(bc::team::enemy), visited);
    auto nearest = [](const bc::distance_field &field, const bc::bitboard &tiles) {
        int best = bc::distance_field::unreachable;
        for (auto tile : tiles.positions())
            best = std::min(best, int(field.get(tile)));
        return best;
    };
    in.picks = bc::draft::team_size();
    const unsigned champions = std::max(1u, 2 * in.picks);
    const int open =
        bc::map::width() * bc::map::height() - int((bc::map::walls() | bc::map::holes()).count());
    in.tiles_per_champion = float(open) / champions;
    in.spawn_distance = nearest(ally, bc::map::spawn_region(bc::team::enemy));

    float gems = 0, own = 0, supply = 0, grouped = 0;
    for (const auto &group : bc::map::camp_groups()) {
        const float value = group.gem_value();
        if (value <= 0)
            continue;
        int ours = bc::distance_field::unreachable, theirs = bc::distance_field::unreachable;
        bool farmable = true;
        for (const auto &camp : group) {
            ours = std::min(ours, int(ally.get(camp.spawn_position())));
            theirs = std::min(theirs, int(enemy.get(camp.spawn_position())));
            const auto stats = camp.base_stats();
            farmable &= stats.base_health <= policy::cheap_camp_health &&
                        stats.base_strength <= policy::cheap_camp_strength;
        }
        gems += value;
        own += ours < theirs ? value : ours == theirs ? value / 2 : 0;
        supply += value / float(std::max(1, int(group.max_respawn_timer())));
        if (!farmable)
            grouped += value;
    }
    if (gems > 0) {
        in.own_gem_share = own / gems;
        in.grouped_gem_share = grouped / gems;
    }
    in.supply_per_champion = supply / champions;

    for (auto altar : bc::map::altars().positions()) {
        auto ring = around(altar, bc::constants::submit_gems_range) & ~terrain;
        if (std::abs(side_balance(nearest(ally, ring), nearest(enemy, ring))) <= 0.15f)
            ++in.middle_altars;
        else
            ++in.side_altars;
    }
    return in;
}

// +1 for each sign that contact and contesting will decide the game, -1 for
// each sign that farming alone should win. Offense at policy::offense_threshold.
int offense_score(const DraftInputs &in) {
    int score = 0;
    if (in.picks <= 3)
        score -= policy::small_team_penalty;
    else if (in.picks >= 5)
        score += policy::large_team_bonus;
    if (in.tiles_per_champion <= policy::crowded_tiles)
        ++score;
    else if (in.tiles_per_champion >= policy::sparse_tiles)
        --score;
    if (in.spawn_distance <= policy::close_spawns)
        ++score;
    else if (in.spawn_distance >= policy::far_spawns)
        --score;
    if (in.middle_altars > 0)
        ++score; // Every carrier must pass a contested point.
    else if (in.side_altars > 0)
        --score;
    if (in.own_gem_share >= policy::own_gems_high)
        --score;
    else if (in.own_gem_share <= policy::own_gems_low)
        ++score;
    if (in.supply_per_champion <= policy::scarce_supply)
        ++score;
    else if (in.supply_per_champion >= policy::plentiful_supply)
        --score;
    if (in.grouped_gem_share >= policy::grouped_gems_high)
        ++score;
    return score;
}

struct DraftState {
    bool ready = false;
    DraftPlan plan;
    DraftInputs inputs;
    int score = 0;

    void initialise() {
        const auto groups = bc::map::camp_groups();
        auto walls = bc::map::walls() | bc::map::holes() | bc::map::altars();
        float dangerous = 0, total_splash = 0;
        unsigned productive = 0;
        unsigned cheap_gem_groups = 0;
        for (const auto &g : groups) {
            float h = 0, p = 0, area = 1, toughest = 1;
            bool farmable = true;
            for (const auto &c : g) {
                auto pos = c.spawn_position();
                auto stats = c.base_stats();
                h += stats.base_health;
                p += stats.base_strength;
                // Classified per camp, not per group: three elks are a 7,200
                // health group but still farmable, whereas one monolith in a
                // group defends the whole group.
                if (stats.base_health > policy::cheap_camp_health ||
                    stats.base_strength > policy::cheap_camp_strength)
                    farmable = false;
                toughest = std::max(toughest, stats.base_health);
                float hits = 0;
                for (const auto &other : g)
                    if (distance(pos, other.spawn_position()) <=
                        bc::constants::passive_abilities::varg_splash_radius)
                        ++hits;
                area = std::max(area, hits);
            }
            area = std::min(area, h / toughest);
            if (p > 0)
                ++dangerous;
            if (g.gem_value() > 0) {
                ++productive;
                if (farmable)
                    ++cheap_gem_groups;
            }
            total_splash += area;
        }
        plan.cheap_gem_groups = static_cast<std::uint8_t>(std::min(cheap_gem_groups, 255u));
        if (!groups.empty()) {
            plan.mean_splash = total_splash / groups.size();
            plan.dangerous_fraction = dangerous / groups.size();
        }
        plan.spacing = std::sqrt(float((~walls).count()) / std::max(1u, productive));
        inputs = measure_map();
        score = offense_score(inputs);
        plan.offense = policy::draft_mode >= 0 ? policy::draft_mode : score >= policy::offense_threshold;
        if constexpr (policy::diagnostic)
            std::printf("DRAFT_MODE offense=%d score=%d forced=%d picks=%u tiles_per_champion=%.0f "
                        "spawn_distance=%d middle_altars=%u side_altars=%u own_gem_share=%.2f "
                        "supply_per_champion=%.2f grouped_gem_share=%.2f\n",
                        plan.offense, score, policy::draft_mode >= 0, inputs.picks, inputs.tiles_per_champion,
                        inputs.spawn_distance, inputs.middle_altars, inputs.side_altars, inputs.own_gem_share,
                        inputs.supply_per_champion, inputs.grouped_gem_share);
        ready = true;
    }

    void write_roster(const compositions::Entry &entry) {
        // next_pick follows this same roster order, including both Twin bodies.
        const auto bodies = compositions::expand(entry.picks);
        plan.roster_types = bodies.types;
        plan.body_count = bodies.count;
    }
};
} // namespace

void play_draft() {
    static DraftState state;
    if (!state.ready)
        state.initialise();
    const auto chosen = bc::draft::current_draft(bc::team::ally);
    const auto committed = compositions::mask(chosen);
    const auto *selected = compositions::roster_for(bc::draft::team_size(), state.plan.offense);
    if (!selected) // Unsupported team size: fall back to the largest offensive roster.
        selected = &compositions::offensive.back();
    auto pick = compositions::next_pick(*selected, committed);
    if (!(selected->picks & compositions::bit(pick))) {
        // Roster exhausted (more picks than it holds): take any unpicked champion.
        for (unsigned p = 0; p < compositions::physical.size(); ++p)
            if (!(committed & compositions::bit(compositions::Pick(p)))) {
                pick = compositions::Pick(p);
                break;
            }
    }
    state.write_roster(*selected);
    bc::draft_memory = std::span{&state.plan, 1};
    if constexpr (policy::diagnostic) {
        std::cout << "DRAFT " << bc::to_string(pick) << " roster=" << selected->name
                  << " offense=" << unsigned(state.plan.offense)
                  << " bodies=" << unsigned(state.plan.body_count) << " fuel=" << bc::fuel_remaining()
                  << '\n';
    }
    bc::draft::pick(pick);
}
