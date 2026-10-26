// Worker safety: detect danger, remember it, and step away.
#include "../bot.h"
#include <tuple>

bool Bot::enemy_threatens(const ObservedUnit &enemy, vec2 at, vec2 tile) const {
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    // Channelling enemies cannot hit us this turn, and enemies on their spawn
    // are shopping or healing, not hunting.
    if (enemy.health <= 0 || enemy.channel || enemy_spawn.is_set(at))
        return false;
    // A stunned enemy loses its next move, so only its standing reach counts.
    const int move = enemy.stunned && enemy.stun_time > 0 ? 0 : enemy.move_speed;
    const int reach = std::max(enemy.range, enemy.spell_reach) + move + policy::worker_danger_margin;
    return distance(tile, at) <= reach;
}

void Bot::worker_track_sightings() {
    if (enemy_seen_near.size() != groups.size())
        enemy_seen_near.assign(groups.size(), -policy::worker_sighting_memory - 1);
    for (const auto &enemy : enemies) {
        if (enemy.health <= 0)
            continue;
        for (std::size_t i = 0; i < groups.size(); ++i)
            for (const auto &camp : groups[i].camps)
                if (enemy_threatens(enemy, camp.position)) {
                    enemy_seen_near[i] = round;
                    break;
                }
    }
}

void Bot::track_ghosts() {
    // An enemy we have seen and cannot see now could be on any tile it can
    // have walked to since, unless that tile is in view: start from its last
    // tile, spread a tile a round, drop the tiles in view and the walls. Only
    // tiles within our sight radius are kept: beyond it the enemy is as
    // unknown as one never seen, and the record goes once none is left, or
    // ghost_lifetime rounds after the sighting (counted from the sighting:
    // counted from each update, records kept in every fight and retreat put
    // enemies long gone on the nearest bush). Brought up to date on turns
    // that use it (enemies in view, a retreat, or the rounds just after a
    // sighting): rounds skipped are spread at once, by doubling steps.
    if (ghost_update == int(round))
        return;
    ghost_update = int(round);
    const std::size_t n = enemy_roster.size();
    if (ghost_round.size() != n) {
        ghost_round.assign(n, -1);
        ghost_sighted.assign(n, -1);
        ghost_seen.assign(n, std::nullopt);
        ghost_built.assign(n, 0);
        ghost_where.resize(n);
        const std::size_t side = 2 * std::size_t(stats.vision_radius) + 1;
        ghost_tiles.resize(side * side);
    }
    ghosts.clear();
    ghost_last.clear();
    bool keep_made = false;
    for (std::size_t i = 0; i < n; ++i) {
        const ObservedUnit *seen = nullptr;
        for (std::size_t k = 0; k < enemies.size(); ++k)
            if (enemy_slot[k] == i) {
                seen = &enemies[k];
                break;
            }
        if (seen) {
            ghost_seen[i] = *seen;
            ghost_round[i] = ghost_sighted[i] = int(round);
            ghost_built[i] = 0; // Its tiles are made when it leaves our sight.
            continue;
        }
        if (ghost_round[i] < 0)
            continue;
        const int elapsed = int(round) - ghost_round[i];
        // A dead enemy has a respawn round (track_enemy_respawns()).
        if (int(round) - ghost_sighted[i] > policy::ghost_lifetime ||
            (i < enemy_respawn.size() && enemy_respawn[i] >= 0)) {
            ghost_round[i] = -1;
            continue;
        }
        if (!keep_made) {
            ghost_keep.clear();
            ghost_keep.draw_chebyshev(position, float(stats.vision_radius));
            ghost_keep &= open_terrain;
            subtract(ghost_keep, visible);
            keep_made = true;
        }
        auto &where = ghost_where[i];
        if (!ghost_built[i]) {
            where.clear();
            where.set(ghost_seen[i]->position);
            ghost_built[i] = 1;
        }
        // Spread by 1, then 2, 4...: steps of s tiles each way add up to every
        // offset until they cover the rounds elapsed (an overshoot only keeps
        // more tiles possible).
        for (int covered = 0, step = 1; covered < elapsed; covered += step, step *= 2) {
            where.shift_right_into(step, ghost_spread);
            where |= ghost_spread;
            where.shift_right_into(-step, ghost_spread);
            where |= ghost_spread;
            where.shift_up_into(step, ghost_spread);
            where |= ghost_spread;
            where.shift_up_into(-step, ghost_spread);
            where |= ghost_spread;
        }
        where &= ghost_keep;
        ghost_round[i] = int(round);
        const std::uint32_t count = where.positions_into(ghost_tiles);
        if (!count) {
            ghost_round[i] = -1;
            continue;
        }
        vec2 nearest = ghost_tiles[0];
        for (std::uint32_t k = 1; k < count; ++k)
            if (distance(position, ghost_tiles[k]) < distance(position, nearest))
                nearest = ghost_tiles[k];
        auto &ghost = ghosts.emplace_back(*ghost_seen[i]);
        ghost.position = nearest;
        ghost.stunned = ghost.channel = false;
        ghost.stun_time = -1;
        ghost_last.push_back(ghost_seen[i]->position);
    }
}

// Danger at a tile: how many enemies could hit it, and by how many tiles the
// closest of them is still short (negative once inside its reach). Enemies
// out of sight count from the tile they were last seen on: placed on their
// possible tile nearest us, ghosts made two danger calls in three wrongly
// even when fresh, the tile they were seen on fewer.
std::pair<int, int> Bot::worker_danger(vec2 tile) const {
    int threats = 0, clearance = std::numeric_limits<int>::max();
    const auto count = [&](const ObservedUnit &enemy, vec2 at) {
        if (!enemy_threatens(enemy, at, tile))
            return;
        ++threats;
        const int reach = std::max(enemy.range, enemy.spell_reach) + enemy.move_speed;
        clearance = std::min(clearance, distance(tile, at) - reach);
    };
    for (const auto &enemy : enemies)
        count(enemy, enemy.position);
    for (std::size_t k = 0; k < ghosts.size(); ++k)
        count(ghosts[k], ghost_last[k]);
    return {threats, clearance};
}

namespace {
// Tiles an enemy closes on us in a round and still strikes: a melee one walks
// and strikes in one turn, a ranged one cannot walk and shoot unless a ready
// attack reset gives the shot back; a ready dash adds its tiles (in place of
// the walk when no walk can follow it).
int closing(const ObservedUnit &e, bool dash) {
    const int walk = e.range <= 1 || e.reset_ready ? e.move_speed : 0;
    if (!dash || e.dash_range <= 0)
        return walk;
    return e.dash_walks ? walk + e.dash_range : std::max(walk, int(e.dash_range));
}

// Our dash: tiles, rounds until usable (-1: no dash), cooldown, and whether a
// walk can follow it.
struct OwnDash {
    int range = 0, wait = -1, cooldown = 1;
    bool walks = false;
};
OwnDash own_dash(const std::array<Spell, 2> &spells) {
    OwnDash d;
    auto &me = bc::self();
    const auto abilities = me.abilities();
    for (unsigned i = 0; i < spells.size(); ++i)
        if (spells[i].kind == bc::ability_kind::dash) {
            d.range = int(spells[i].range);
            d.cooldown = spells[i].cooldown;
            d.walks = !spells[i].exclusive;
            d.wait = std::max(me.can_cast() ? 0 : 1, int(abilities[i].cooldown()));
        }
    return d;
}

// Every ability's cooldown (docs/GameRules.md): a healer's heal is its
// healing rate over that many rounds.
constexpr float heal_period = 8;
} // namespace

std::uint32_t Bot::threat_bits() const {
    // The enemies that could hit us next round, as bits of their roster
    // entries: within their range after closing in, or within a ready
    // spell's reach.
    std::uint32_t bits = 0;
    for (std::size_t k = 0; k < enemies.size(); ++k) {
        const auto &e = enemies[k];
        if (e.health <= 0 || e.channel || (e.stunned && e.stun_time > 0) || enemy_spawn_tile[tile_index(e.position)])
            continue;
        int reach = e.range + closing(e, e.dash_wait == 0);
        if (e.spell_burst > 0)
            reach = std::max(reach, e.spell_reach);
        if (distance(position, e.position) <= reach)
            bits |= 1u << enemy_slot[k];
    }
    return bits;
}

bool Bot::duel_won(std::uint32_t threats, float pool) {
    // A chase pulls the chasers away from their group: we beat them when we
    // and the teammates in reach kill every one that can hit us, the
    // quickest first, before they kill us; each kill takes its damage away.
    // Killing one is not enough: champions took on four for a low one and
    // died. A healer's heal counts as health on each of them, and teammates
    // heading home are leaving the fight. The damage we take by the end of
    // each round is kept to check the fight against.
    struct Foe {
        float lasts, rate, burst;
    };
    static std::vector<Foe> foes;
    foes.clear();
    float heal = 0;
    for (const auto &e : enemies)
        heal = std::max(heal, e.healing_rate * heal_period);
    const float ours = spell_dps(spells, strength, focus);
    for (std::size_t j = 0; j < enemies.size(); ++j) {
        const auto &e = enemies[j];
        if (!(threats >> enemy_slot[j] & 1))
            continue;
        float rate = ours;
        for (std::size_t k = 0; k < allies.size(); ++k) {
            const auto &a = allies[k];
            if (a.channel || shared.get(ally_slot[k]) == TeamMemory::recall ||
                a.health < a.max_health * policy::fight_retreat_health)
                continue;
            if (distance(a.position, e.position) <= a.range + closing(a, false))
                rate += a.damage_rate;
        }
        // Its ready burst lands in the first round (race() counts it too).
        foes.push_back({(e.health + e.shield + heal) / (rate * e.mitigation), e.damage_rate * mitigation,
                        (e.stunned && e.stun_time > 0 ? 0.0f : e.burst) * mitigation});
    }
    if (foes.empty())
        return false;
    std::sort(foes.begin(), foes.end(), [](const Foe &a, const Foe &b) { return a.lasts < b.lasts; });
    // Each falls when the kills before it and its own are done.
    for (std::size_t k = 1; k < foes.size(); ++k)
        foes[k].lasts += foes[k - 1].lasts;
    chase.foreseen.clear();
    float taken = 0;
    for (int r = 0; float(r) < foes.back().lasts; ++r) {
        for (const auto &f : foes)
            if (f.lasts > float(r))
                taken += f.rate + (r == 0 ? f.burst : 0.0f);
        chase.foreseen.push_back(taken);
        if (taken >= pool)
            return false;
    }
    return true;
}

bool Bot::race(vec2 goal, int arrive, bool use_dash, float pool) {
    // Round by round we walk to safety, dashing when ours is ready, and each
    // enemy closes on us at its own pace: a melee one walks (and dashes when
    // a walk falls short) and strikes once within its range; a ranged one
    // shoots while we are in its range, dashes when that brings us in range
    // and shoots, else walks without shooting. A ready spell lands once.
    // Safety: within `arrive` tiles of the goal (-1: home, by the walk
    // there), a gap none of them closes during a recall, or a whole dash
    // cycle in which none hit us and no gap shrank (the cycle only repeats,
    // so none catches us). The damage by the end of each round is kept to
    // check the chase against. Distances ignore walls: a wall between us and
    // a chaser lengthens the real chase.
    struct Runner {
        int x, y, range, reach, recall_reach, walk, dash_walk, move, dash_range, cooldown, wait, mark;
        float strength, burst;
        bool still;
    };
    static std::vector<Runner> runners;
    runners.clear();
    const int recall = recall_turns;
    OwnDash dash;
    if (use_dash)
        dash = own_dash(spells);
    int cycle = dash.range > 0 ? dash.cooldown : 1;
    for (const auto &e : enemies) {
        if (e.health <= 0 || e.channel || enemy_spawn_tile[tile_index(e.position)])
            continue;
        const int reach = std::max(e.range, e.spell_reach);
        const int mark = distance(e.position, position);
        runners.push_back({e.position.x, e.position.y, e.range, reach, reach + recall * e.move_speed, closing(e, false),
                           closing(e, true), e.move_speed, e.dash_range, e.dash_cooldown,
                           e.dash_range > 0 ? int(e.dash_wait) : -1, mark, e.strength, e.burst,
                           e.stunned && e.stun_time > 0});
        if (e.dash_range > 0)
            cycle = std::max(cycle, int(e.dash_cooldown));
    }
    const int walk = home_field.get(position);
    const int home = walk != bc::distance_field::unreachable ? walk : map_width + map_height;
    const int speed = std::max(1, int(stats.base_move_speed));
    chase.foreseen.clear();
    int x = position.x, y = position.y, covered = 0;
    float taken = 0, worst = 0, recent = 0;
    for (int t = 1;; ++t) {
        int step = speed;
        if (dash.range > 0 && dash.wait == 0) {
            step = dash.walks ? step + dash.range : std::max(step, dash.range);
            dash.wait = dash.cooldown;
        }
        x += std::clamp(goal.x - x, -step, step);
        y += std::clamp(goal.y - y, -step, step);
        covered += step;
        float dealt = 0;
        bool apart = true;
        for (auto &r : runners) {
            int d = std::max(std::abs(r.x - x), std::abs(r.y - y));
            if (r.still)
                r.still = false; // A stun costs it this round.
            else {
                bool hits = d <= r.range;
                if (!hits) {
                    int moved;
                    if (r.range <= 1) {
                        const bool dashing = r.wait == 0 && d - r.range > r.walk;
                        moved = std::min(d - r.range, dashing ? r.dash_walk : r.walk);
                        hits = moved == d - r.range;
                        if (dashing)
                            r.wait = r.cooldown;
                    } else if (r.wait == 0 && d - r.dash_range <= r.range) {
                        moved = d - r.range;
                        hits = true;
                        r.wait = r.cooldown;
                    } else
                        moved = r.move;
                    r.x += std::clamp(x - r.x, -moved, moved);
                    r.y += std::clamp(y - r.y, -moved, moved);
                    d = std::max(std::abs(r.x - x), std::abs(r.y - y));
                }
                if (hits)
                    dealt += r.strength;
                if (r.burst > 0 && d <= r.reach) {
                    dealt += r.burst;
                    r.burst = 0;
                }
                if (r.wait > 0)
                    --r.wait;
            }
            apart = apart && d > r.recall_reach + (r.dash_range > 0 && r.wait <= recall ? r.dash_range : 0);
        }
        if (dash.wait > 0)
            --dash.wait;
        dealt *= mitigation;
        taken += dealt;
        worst = std::max(worst, dealt);
        chase.foreseen.push_back(taken);
        if (taken >= pool)
            return false;
        bool safe = apart || (arrive >= 0 ? std::max(std::abs(x - goal.x), std::abs(y - goal.y)) <= arrive
                                          : covered >= home);
        recent += dealt;
        if (!safe && t % cycle == 0) {
            safe = recent == 0;
            for (auto &r : runners) {
                const int d = std::max(std::abs(r.x - x), std::abs(r.y - y));
                safe = safe && d >= r.mark;
                r.mark = d;
            }
            recent = 0;
        }
        // Arrive with a round of their worst damage still to spare.
        if (safe)
            return pool - taken > worst;
    }
}

Bot::Chase::Mode Bot::chase_mode(bool retreat) {
    // Breaking contact with a stronger group: run, unless the first round of
    // the race to safety kills us (walking would only give the chasers free
    // hits), and fight when we win the duel with the chasers. Decided
    // once and kept: flipping between the two gets neither the distance nor
    // the damage. Only what the decision did not foresee changes it: a new
    // enemy joins, a chaser dies or is stunned, we are hit harder than the
    // race or the duel foresaw, we reach the teammate we ran to, or,
    // fighting, our dash comes back or nothing is left in our reach. The
    // chase itself ends when no enemy can reach us, a stun, death or a recall
    // (play_worker_turn()). A melee champion strikes as it walks, so it runs
    // unless it wins the duel. A turn decides once (the fight path can hand
    // over to the retreat).
    if (chase.kept == int(round))
        return chase.mode;
    // The enemies the race runs against: every one in view that is not
    // channelling or on its spawn. One not among them at the decision joins.
    std::uint32_t in_view = 0, stunned = 0;
    for (std::size_t k = 0; k < enemies.size(); ++k) {
        const auto &e = enemies[k];
        if (e.health <= 0 || e.channel || enemy_spawn_tile[tile_index(e.position)])
            continue;
        in_view |= 1u << enemy_slot[k];
        if (e.stunned && e.stun_time > 0)
            stunned |= 1u << enemy_slot[k];
    }
    // Health and shield, read only when a check or a decision needs them.
    float pool = -1;
    const auto read_pool = [&] {
        const auto buffs = bc::self().buffs();
        pool = health + (buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0.0f);
    };
    const bool has_dash = std::any_of(spells.begin(), spells.end(),
                                      [](const Spell &s) { return s.kind == bc::ability_kind::dash; });
    bool keep = chase.mode != Chase::none && !(in_view & ~chase.chasers) && !(stunned & chase.chasers);
    for (std::size_t i = 0; keep && i < enemy_respawn.size(); ++i)
        keep = !(chase.chasers >> i & 1) || enemy_respawn[i] < 0;
    if (keep) {
        const std::size_t since = std::size_t(int(round) - chase.decided);
        if (since >= 1 && since <= chase.foreseen.size()) {
            read_pool();
            keep = chase.pool - pool <= chase.foreseen[since - 1] + 1;
        }
    }
    if (keep && chase.mode == Chase::run) {
        for (std::size_t i = 0; i < allies.size(); ++i)
            if (int(ally_slot[i]) == chase.mate && distance(position, allies[i].position) <= 2)
                keep = false;
    } else if (keep) {
        bool reach = false;
        for (const auto &e : enemies)
            reach = reach || (e.health > 0 && distance(position, e.position) <= range);
        keep = reach && (chase.dash_ready || !has_dash || own_dash(spells).wait != 0);
    }
    if (!keep) {
        if (pool < 0)
            read_pool();
        const std::uint32_t threats = threat_bits();
        chase.decided = int(round);
        chase.chasers = in_view;
        chase.pool = pool;
        chase.mate = -1;
        chase.dash = false;
        chase.dash_ready = has_dash && own_dash(spells).wait == 0;
        // A fight is checked against the duel and a run against the race; a
        // melee champion's walk, and a stand when the run's first round
        // kills us, have nothing foreseen to check.
        if (duel_won(threats, pool))
            chase.mode = Chase::fight;
        else if (range <= 1) {
            chase.mode = Chase::run;
            chase.foreseen.clear();
        } else {
            // Safety is the nearest teammate in view further from the
            // enemies than we are (on the fight path; a retreat heads home),
            // else home.
            static const vec2 home = [] {
                const auto tiles = bc::map::spawn_region(bc::team::ally).positions();
                int x = 0, y = 0, n = 0;
                for (const auto p : tiles) {
                    x += p.x;
                    y += p.y;
                    ++n;
                }
                return n ? vec2{x / n, y / n} : vec2{0, 0};
            }();
            const auto exposure = [&](vec2 at) {
                int nearest = std::numeric_limits<int>::max();
                for (const auto &e : enemies)
                    nearest = std::min(nearest, distance(at, e.position));
                return nearest;
            };
            vec2 goal = home;
            int arrive = -1;
            if (!retreat) {
                const int ours = exposure(position);
                int best = std::numeric_limits<int>::max();
                for (std::size_t i = 0; i < allies.size(); ++i) {
                    const auto &a = allies[i];
                    if (a.channel || exposure(a.position) <= ours)
                        continue;
                    if (const int d = distance(position, a.position); d < best) {
                        best = d;
                        goal = a.position;
                        arrive = 2;
                        chase.mate = int(ally_slot[i]);
                    }
                }
            }
            // When no run ends alive against chasers that follow perfectly, we
            // still run, checked against the race (a chaser often breaks off:
            // standing and shooting traded one Willow down to death where
            // walking away lived), and stand only when the run's first round
            // kills us anyway.
            if (race(goal, arrive, false, pool))
                chase.mode = Chase::run;
            else if (has_dash && race(goal, arrive, true, pool)) {
                chase.mode = Chase::run;
                chase.dash = true;
            } else if (chase.foreseen.size() > 1) {
                chase.mode = Chase::run;
                chase.dash = has_dash;
            } else {
                chase.mode = Chase::fight;
                chase.foreseen.clear();
            }
        }
    }
    chase.kept = int(round);
    return chase.mode;
}

bool Bot::run_to_safety() {
    // Toward the teammate the race ran to, else home, dashing only when the
    // race needed it; spells that allow walking still go off. The step away
    // (toward any teammate in the fight) is cheaper but is not the run the
    // race checked: it lost 109 gems a game to v35 where this won 67.
    bc::bitboard goals = bc::map::spawn_region(bc::team::ally);
    vec2 toward = home_centre;
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (int(ally_slot[i]) == chase.mate) {
            goals = around(allies[i].position, 2);
            toward = allies[i].position;
        }
    strike_before_step();
    if (!move_to(goals, toward, true, chase.dash))
        return false;
    observe_fight();
    combat(true, -1, true);
    return true;
}

void Bot::strike_before_step() {
    // A melee champion strikes and steps in one turn, so it strikes first,
    // while the enemy is in reach: willow best girl's Heart of Ruin and
    // Marcella, below 35% health beside an enemy, strike in 67-78% of those
    // rounds and step away in 46-63%; ours, stepping first, struck in 24-31%.
    // Spells that exclude walking wait for the step.
    if (range > 1)
        return;
    bool reach = false;
    for (const auto &e : enemies)
        reach = reach || (e.health > 0 && distance(position, e.position) <= range);
    if (!reach)
        return;
    const bool stepped = walked;
    walked = true; // combat() then leaves spells that exclude walking.
    combat(true, -1, true);
    walked = stepped;
}

void Bot::avoid_reach() {
    // Moves for the rest of this turn keep out of the reach of the stronger
    // group in view: walking back in after stepping away kept a champion
    // hovering at its edge until it was caught.
    static const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    avoid_zone.clear();
    const auto counted = [&](const ObservedUnit &e) {
        return e.health > 0 && !e.channel && !enemy_spawn.is_set(e.position);
    };
    for (const auto &e : enemies) {
        if (!counted(e))
            continue;
        const int move = e.stunned && e.stun_time > 0 ? 0 : e.move_speed;
        avoid_zone.draw_chebyshev(
            e.position, float(std::max(e.range, e.spell_reach) + move + policy::worker_danger_margin));
    }
    avoid_zone.set(position, false);
    // A step further from every enemy in view stays open: in a corner or a
    // corridor the zone boxed champions in where a step away was free.
    const auto exposure = [&](vec2 at) {
        int nearest = std::numeric_limits<int>::max();
        for (const auto &e : enemies)
            if (counted(e))
                nearest = std::min(nearest, distance(at, e.position));
        return nearest;
    };
    const int here = exposure(position);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 step{position.x + dx, position.y + dy};
            if ((dx || dy) && step.x >= 0 && step.y >= 0 && step.x < map_width && step.y < map_height &&
                exposure(step) > here)
                avoid_zone.set(step, false);
        }
    avoid_round = int(round);
    blocked |= avoid_zone;
}

vec2 Bot::step_away_tile() {
    // The reachable tile with the fewest threats, then the most room, then
    // the shortest way to a teammate in view that is in this fight or coming
    // to it (help that answered), else home (or to an altar when carrying a
    // bag).
    const auto [threats, clearance] = worker_danger(position);
    const auto &guide =
        gems && altar_walk().get(position) < home_field.get(position) ? altar_walk() : home_field;
    vec2 helper{-1, -1};
    int nearest = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < allies.size(); ++i) {
        const int fight = TeamMemory::fight_group(shared.get(ally_slot[i]), groups.size());
        if (allies[i].channel || !same_fight(fight, position))
            continue;
        if (const int d = distance(position, allies[i].position); d < nearest) {
            nearest = d;
            helper = allies[i].position;
        }
    }
    const auto way = [&](vec2 tile) { return helper.x >= 0 ? distance(tile, helper) : int(guide.get(tile)); };
    vec2 best = position;
    auto best_score = std::tuple{threats, -clearance, way(position)};
    if (bc::self().can_move()) {
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        for (auto tile : step_tiles.positions()) {
            if (tile == position)
                continue;
            const auto [count, room] = worker_danger(tile);
            const auto score = std::tuple{count, -room, way(tile)};
            if (score < best_score) {
                best_score = score;
                best = tile;
            }
        }
    }
    return best;
}
