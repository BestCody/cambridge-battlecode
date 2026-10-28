// C++ port of training/features.py: the per-turn feature vector the print-dont-int imitation models
// read. Keep this file and features.py in lockstep (same order of operations, same tie-breaks, same
// rounding); PDI_FEATURE_VERSION in the generated pdi_feature_index.h must equal kVersion below.
//
// Usage (one FeatureBuilder per champion bot instance; it holds that champion's own memory):
//     static pdi::FeatureBuilder fb;
//     float x[pdi::NUM_FEATURES];
//     fb.compute(x);                 // once at the start of the turn (also updates memory)
//     fb.refresh_economy(x);         // after buying, before asking the shop head again
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include <cambc.h>

#include "generated/pdi_feature_index.h"

namespace pdi {

namespace bc = cambc;

inline constexpr int kVersion = 4;
static_assert(kVersion == PDI_FEATURE_VERSION, "pdi_features.h is out of date with features.py / the exported models");

inline constexpr float FAR = 500.0f;
inline constexpr int WIN_GEMS = 1000;
inline constexpr int N_ENEMY_SLOTS = 3, N_ALLY_SLOTS = 2, N_CAMP_SLOTS = 4, ITEM_FAMILIES = 14;
inline constexpr int CAMP_HP[6] = {1350, 2400, 120, 120, 20000, 5000};
inline constexpr int CAMP_STR[6] = {20, 60, 0, 0, 200, 120};
inline constexpr int CAMP_RANGE[6] = {3, 4, 0, 0, 5, 4};
inline constexpr int DIRS[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
inline constexpr std::uint32_t ST_RECALLING = 2, ST_SUBMITTING = 4, ST_CAN_MOVE = 8, ST_CAN_ATTACK = 16, ST_CAN_CAST = 2048;
inline const float NaN = std::numeric_limits<float>::quiet_NaN();

inline int cheb(int ax, int ay, int bx, int by) { return std::max(std::abs(ax - bx), std::abs(ay - by)); }
// The extractor stores health/strength/focus/shield rounded to 0.1 (JS Math.round); do the same.
inline double round1(double v) { return std::floor(v * 10.0 + 0.5) / 10.0; }

struct Unit {  // a visible champion
    std::uint32_t id;
    int x, y, type;
    double hp, mhp, str, foc;
    int gems, stun;
    bool recalling;
    bc::champion handle;
};

struct CampView {
    std::uint32_t handle;
    int x, y, type, state, group;  // state: 1 awake, 2 asleep
    double hp, mhp;
    bc::camp camp;
};

// ---------------------------------------------------------------------------------------------
// Static map data (MapStatic in features.py)
// ---------------------------------------------------------------------------------------------
struct Group {
    std::uint32_t handle;
    int gems, gold, hp, n_by_type[6] = {};
    double cx, cy;
    bc::distance_field dist;
};

struct MapStatic {
    int W = 0, H = 0, team = 0, symmetry = -1, total_gems = 0;
    bc::bitboard passable, bushes, own_spawn;
    std::vector<bc::vec2> altar_tiles;
    std::vector<Group> groups;
    std::unordered_map<std::uint32_t, int> group_index;   // engine group handle -> index
    std::unordered_map<int, int> group_of_tile;           // y * W + x -> group index (camp spawn tiles)
    bc::distance_field d_own, d_enemy, d_altar;

    void init() {
        W = bc::map::width();
        H = bc::map::height();
        team = bc::detail::cached_my_team;
        std::int32_t sym = -1;
        if (bc::detail::sdk_bc_map_symmetry(&sym) == 0)
            symmetry = sym;
        const auto& enemy_spawn = bc::map::spawn_region(bc::team::enemy);
        const bc::bitboard blocked = bc::map::walls() | bc::map::holes() | bc::map::altars() | enemy_spawn;
        passable = ~blocked;
        bushes = bc::map::bushes();
        own_spawn = bc::map::spawn_region(bc::team::ally);
        altar_tiles = bc::map::altars().positions();
        std::sort(altar_tiles.begin(), altar_tiles.end(), [](bc::vec2 a, bc::vec2 b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        bc::bitboard visited;
        d_own.compute(blocked, own_spawn, visited);
        d_enemy.compute(blocked, dilate(enemy_spawn) & passable, visited);
        d_altar.compute(blocked, dilate(bc::map::altars()) & passable, visited);
        for (const auto& g : bc::map::camp_groups()) {
            Group gr;
            gr.handle = static_cast<std::uint32_t>(static_cast<bc::handle_camp_group_t>(g));
            gr.gems = int(g.gem_value());
            gr.gold = int(g.gold_value());
            gr.hp = 0;
            double sx = 0, sy = 0;
            bc::bitboard roots;
            const int gi = int(groups.size());
            for (const auto& c : g.camps()) {
                const auto p = c.spawn_position();
                const int t = int(c.kind());
                gr.n_by_type[t]++;
                gr.hp += CAMP_HP[t];
                sx += p.x;
                sy += p.y;
                roots.set(p);
                group_of_tile[p.y * W + p.x] = gi;
            }
            const auto n = std::max<std::size_t>(1, g.camps().size());
            gr.cx = sx / double(n);
            gr.cy = sy / double(n);
            gr.dist.compute(blocked, roots, visited);
            total_gems += gr.gems;
            group_index[gr.handle] = gi;
            groups.push_back(std::move(gr));
        }
    }

    static bc::bitboard dilate(const bc::bitboard& m) {
        bc::bitboard out = m;
        for (const auto& d : DIRS)
            out |= m.shift_right(d[0]).shift_up(-d[1]);   // shift_up(+1) moves tiles towards smaller y
        return out;
    }

    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < W && y < H; }
    bool is_passable(int x, int y) const { return in_bounds(x, y) && passable.is_set({x, y}); }

    static float at(const bc::distance_field& f, int x, int y, int W, int H) {
        if (x < 0 || y < 0 || x >= W || y >= H)
            return FAR;
        const auto v = f.get({x, y});
        return v == bc::distance_field::unreachable ? FAR : float(v);
    }
    float at(const bc::distance_field& f, int x, int y) const { return at(f, x, y, W, H); }
};

// ---------------------------------------------------------------------------------------------
// Per-champion memory (Memory in features.py)
// ---------------------------------------------------------------------------------------------
struct Memory {
    std::vector<int> g_state, g_seen, g_ready;
    struct Seen { int round, x, y; };
    std::unordered_map<std::uint32_t, Seen> enemy_last;

    void init(std::size_t n) {
        g_state.assign(n, 1);
        g_seen.assign(n, 0);
        g_ready.assign(n, 0);
    }
    bool alive_guess(int g, int rnd) const { return g_state[g] == 1 || (g_state[g] == 3 && rnd >= g_ready[g]); }
};

// ---------------------------------------------------------------------------------------------
// Feature builder
// ---------------------------------------------------------------------------------------------
class FeatureBuilder {
public:
    MapStatic ms;
    Memory mem;
    // Sorted visible units of the last compute(), for resolving target classes to units.
    std::vector<Unit> vis_enemies, vis_allies;
    std::vector<CampView> camps;
    int self_x = 0, self_y = 0;
    bool alive = false;

    void compute(float* f) {
        if (!ready_) {
            ms.init();
            mem.init(ms.groups.size());
            ready_ = true;
        }
        std::fill(f, f + NUM_FEATURES, NaN);
        auto put = [f](int i, double v) { f[i] = float(v); };
        auto& me = bc::self();
        const int rnd = int(bc::round_number());
        alive = me.is_alive();
        if (alive) {
            const auto p = me.position();
            self_x = p.x;
            self_y = p.y;
        }
        const int x = self_x, y = self_y;

        // ---- context ----
        const int sub_a = int(bc::global_submitted_gems(bc::team::ally));
        const int sub_e = int(bc::global_submitted_gems(bc::team::enemy));
        put(F_round, rnd); put(F_round_frac, rnd / 2000.0);
        put(F_sub_ally, sub_a); put(F_sub_enemy, sub_e); put(F_sub_diff, sub_a - sub_e);
        put(F_map_w, ms.W); put(F_map_h, ms.H); put(F_n_altars, ms.altar_tiles.size()); put(F_n_groups, ms.groups.size());
        put(F_map_gems, ms.total_gems); put(F_symmetry, ms.symmetry); put(F_side, ms.team);
        auto allies = sorted_champions(bc::team::ally);
        auto enemies = sorted_champions(bc::team::enemy);
        const auto my_id = static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(me));
        allies.erase(std::remove_if(allies.begin(), allies.end(),
                                    [&](const bc::champion& c) { return id_of(c) == my_id; }), allies.end());
        put(F_n_allies, allies.size() + 1); put(F_n_enemies, enemies.size());
        int aa = alive ? 1 : 0, ea = 0;
        for (auto& a : allies) aa += a.is_alive();
        for (auto& e : enemies) ea += e.is_alive();
        put(F_allies_alive, aa); put(F_enemies_alive, ea);

        // ---- self ----
        const int gems = int(me.gem_count());
        put(F_alive, alive);
        put(F_gold, me.gold_count()); put(F_gems, gems);
        put(F_gems_to_win, WIN_GEMS - sub_a - gems); put(F_can_win_now, gems + sub_a >= WIN_GEMS);
        const auto abilities = me.abilities();
        put(F_cd0, safe([&] { return double(abilities[0].cooldown()); }));
        put(F_cd1, safe([&] { return double(abilities[1].cooldown()); }));
        bc::detail::ward_resources wr{};
        if (bc::detail::sdk_champion_wards(my_id, &wr) == 0) {
            put(F_ward_charges, wr.charges); put(F_ward_cd, wr.cooldown);
        } else {
            put(F_ward_charges, 0); put(F_ward_cd, 0);
        }
        const std::uint32_t st = alive ? me.state().bits() : 0;
        put(F_recalling, (st & ST_RECALLING) != 0); put(F_submitting, (st & ST_SUBMITTING) != 0);
        put(F_can_move, (st & ST_CAN_MOVE) != 0); put(F_can_attack, (st & ST_CAN_ATTACK) != 0);
        put(F_can_cast, (st & ST_CAN_CAST) != 0);
        put(F_chan, safe([&] { return double(me.remaining_channel_time()); }, -1.0));
        refresh_economy(f);
        put(F_priority, safe([&] { return double(me.priority()); }, 0.0));
        if (alive) {
            const double hp = round1(me.health()), mhp = round1(me.max_health());
            put(F_hp, hp); put(F_max_hp, mhp); put(F_hp_frac, hp / std::max(mhp, 1.0));
            put(F_str, round1(me.strength())); put(F_focus, round1(me.focus()));
            int stun = 0;
            double shield = 0;
            buffs_of(me, stun, shield);
            put(F_stun, stun); put(F_shield, shield);
            put(F_x_frac, double(x) / ms.W); put(F_y_frac, double(y) / ms.H);
            put(F_in_bush, ms.bushes.is_set({x, y})); put(F_on_spawn, ms.own_spawn.is_set({x, y}));
            put(F_d_spawn, ms.at(ms.d_own, x, y)); put(F_d_enemy_spawn, ms.at(ms.d_enemy, x, y));
            put(F_d_altar, ms.at(ms.d_altar, x, y));
            int ca = int(FAR);
            for (auto a : ms.altar_tiles) ca = std::min(ca, cheb(x, y, a.x, a.y));
            put(F_cheb_altar, ca); put(F_adj_altar, ca <= 1);
            int wd = int(FAR);
            for (auto w : me.ward_positions()) wd = std::min(wd, cheb(x, y, w.x, w.y));
            put(F_ward_dist, wd);
        }
        vis_enemies.clear();
        vis_allies.clear();
        camps.clear();
        if (!alive) {
            update_memory(rnd, enemies);
            return;
        }

        // ---- enemies ----
        for (auto& e : enemies)
            if (e.is_visible()) vis_enemies.push_back(unit_of(e));
        sort_units(vis_enemies, x, y);
        put(F_n_vis_enemies, vis_enemies.size());
        double ehp4 = 0;
        int n3 = 0, max_gems = 0;
        for (auto& e : vis_enemies) {
            const int d = cheb(x, y, e.x, e.y);
            if (d <= 4) ehp4 += e.hp;
            if (d <= 3) ++n3;
            max_gems = std::max(max_gems, e.gems);
        }
        put(F_enemy_hp_r4, ehp4); put(F_n_enemies_r3, n3); put(F_max_enemy_gems, max_gems);
        if (!vis_enemies.empty()) {
            put(F_since_enemy_seen, 0);
        } else if (!mem.enemy_last.empty()) {
            int last = 0;
            for (auto& [id, s] : mem.enemy_last) last = std::max(last, s.round);
            put(F_since_enemy_seen, rnd - last);
        } else {
            put(F_since_enemy_seen, 2000);
        }
        if (!mem.enemy_last.empty()) {
            int best = std::numeric_limits<int>::max();
            for (auto& [id, s] : mem.enemy_last) best = std::min(best, cheb(x, y, s.x, s.y));
            put(F_last_enemy_dist, best);
        }
        for (int i = 0; i < N_ENEMY_SLOTS && i < int(vis_enemies.size()); ++i) {
            const auto& e = vis_enemies[i];
            const int b = F_e0_dx + i * (F_e1_dx - F_e0_dx);
            put(b + 0, e.x - x); put(b + 1, e.y - y); put(b + 2, cheb(x, y, e.x, e.y));
            put(b + 3, e.hp); put(b + 4, e.hp / std::max(e.mhp, 1.0)); put(b + 5, e.type);
            put(b + 6, e.gems); put(b + 7, e.stun); put(b + 8, e.recalling); put(b + 9, e.str); put(b + 10, e.foc);
        }

        // ---- allies ----
        for (auto& a : allies)
            if (a.is_visible()) vis_allies.push_back(unit_of(a));
        sort_units(vis_allies, x, y);
        put(F_n_vis_allies, vis_allies.size());
        double ahp = round1(me.health());
        int na3 = 0;
        for (auto& a : vis_allies) {
            const int d = cheb(x, y, a.x, a.y);
            if (d <= 4) ahp += a.hp;
            if (d <= 3) ++na3;
        }
        put(F_ally_hp_r4, ahp); put(F_n_allies_r3, na3); put(F_hp_ratio_r4, ahp / (double(f[F_enemy_hp_r4]) + 1));
        for (int i = 0; i < N_ALLY_SLOTS && i < int(vis_allies.size()); ++i) {
            const auto& a = vis_allies[i];
            const int b = F_a0_dx + i * (F_a1_dx - F_a0_dx);
            put(b + 0, a.x - x); put(b + 1, a.y - y); put(b + 2, cheb(x, y, a.x, a.y));
            put(b + 3, a.hp / std::max(a.mhp, 1.0)); put(b + 4, a.type); put(b + 5, a.gems);
        }

        // ---- camps (the SDK route: camp occupancy, then camp_at) ----
        auto tiles = bc::vision::camp_tiles().positions();
        std::sort(tiles.begin(), tiles.end(), [](bc::vec2 a, bc::vec2 b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        for (auto p : tiles) {
            try {
                auto c = bc::vision::camp_at(p);
                CampView v{static_cast<std::uint32_t>(static_cast<bc::handle_camp_t>(c)), p.x, p.y, int(c.kind()),
                           int(static_cast<std::uint32_t>(c.state())), -1, round1(c.health()), round1(c.max_health()), c};
                if (v.hp <= 0) continue;
                const auto sp = c.spawn_position();
                auto it = ms.group_of_tile.find(sp.y * ms.W + sp.x);
                v.group = it == ms.group_of_tile.end() ? -1 : it->second;
                camps.push_back(v);
            } catch (const std::exception&) {
            }
        }
        std::stable_sort(camps.begin(), camps.end(), [x, y](const CampView& a, const CampView& b) {
            const int da = cheb(x, y, a.x, a.y), db = cheb(x, y, b.x, b.y);
            return da != db ? da < db : a.hp < b.hp;
        });
        put(F_n_vis_camps, camps.size());
        int awake = 0, threat_n = 0, threat_str = 0;
        std::unordered_map<int, int> left;
        for (auto& c : camps) {
            awake += c.state == 1;
            if (c.state == 1 && cheb(x, y, c.x, c.y) <= CAMP_RANGE[c.type]) {
                ++threat_n;
                threat_str += CAMP_STR[c.type];
            }
            left[c.group]++;
        }
        put(F_n_awake, awake); put(F_threat_n, threat_n); put(F_threat_str, threat_str);
        for (int i = 0; i < N_CAMP_SLOTS && i < int(camps.size()); ++i) {
            const auto& c = camps[i];
            const int b = F_c0_dx + i * (F_c1_dx - F_c0_dx);
            put(b + 0, c.x - x); put(b + 1, c.y - y); put(b + 2, cheb(x, y, c.x, c.y)); put(b + 3, c.type);
            put(b + 4, c.hp); put(b + 5, c.hp / std::max(c.mhp, 1.0)); put(b + 6, c.state == 1);
            put(b + 7, c.group >= 0 ? ms.groups[c.group].gems : 0); put(b + 8, left[c.group]);
        }

        // ---- camp groups (memory) ----
        update_memory(rnd, enemies);
        std::vector<int> alive_groups;
        for (int gi = 0; gi < int(ms.groups.size()); ++gi)
            if (mem.alive_guess(gi, rnd)) alive_groups.push_back(gi);
        put(F_groups_alive_known, alive_groups.size());
        int within20 = 0, gnear = -1, gbest = -1;
        double dnear = 0, sbest = 0;
        for (int gi : alive_groups) {
            const double d = ms.at(ms.groups[gi].dist, x, y);
            within20 += d <= 20;
            if (gnear < 0 || d < dnear) { gnear = gi; dnear = d; }
            const double s = (ms.groups[gi].gems + 0.1 * ms.groups[gi].gold) / (d + 15);
            if (gbest < 0 || s > sbest) { gbest = gi; sbest = s; }
        }
        put(F_groups_alive_20, within20);
        put_group(f, F_gnear_dist, gnear, x, y);
        put_group(f, F_gbest_dist, gbest, x, y);
        int greward = -1;
        double drew = 0;
        for (int gi = 0; gi < int(ms.groups.size()); ++gi) {
            if (mem.g_state[gi] != 2 || rnd - mem.g_seen[gi] > 30) continue;
            const double d = ms.at(ms.groups[gi].dist, x, y);
            if (greward < 0 || d < drew) { greward = gi; drew = d; }
        }
        if (greward >= 0) {
            put(F_greward_dist, drew); put(F_greward_gems, ms.groups[greward].gems);
        }
        put(F_cap_state, -1); put(F_cap_timer, -1);
        if (auto it = ms.group_of_tile.find(y * ms.W + x); it != ms.group_of_tile.end()) {
            bc::detail::camp_group_capture_state cs{};
            std::uint32_t state = 0;
            const auto h = ms.groups[it->second].handle;
            if (bc::detail::sdk_camp_group_state(h, &state) == 0 && state != 0 &&
                bc::detail::sdk_camp_group_capture_state(h, &cs) == 0) {
                put(F_cap_state, cs.state); put(F_cap_timer, cs.timer);
            }
        }
        int el = 0, al = 0;
        bool lock_near = false;
        const auto locks = bc::vision::altar_locks();
        bc::vec2 near_altar{-1, -1};
        int best_altar = std::numeric_limits<int>::max();
        for (auto a : ms.altar_tiles)
            if (const int d = cheb(x, y, a.x, a.y); d < best_altar) { best_altar = d; near_altar = a; }
        for (auto& l : locks) {
            if (l.is_enemy()) {
                ++el;
                if (l.position() == near_altar) lock_near = true;
            } else {
                ++al;
            }
        }
        put(F_n_enemy_locks, el); put(F_n_ally_locks, al);
        if (!ms.altar_tiles.empty()) put(F_lock_on_near_altar, lock_near);
        int etp = 0;
        for (auto& t : bc::vision::teleports()) etp += t.is_enemy();
        put(F_n_enemy_tps, etp);

        // ---- per-direction move features ----
        auto occupied = [&](int tx, int ty) {
            for (auto& u : vis_enemies) if (u.x == tx && u.y == ty) return true;
            for (auto& u : vis_allies) if (u.x == tx && u.y == ty) return true;
            for (auto& c : camps) if (c.x == tx && c.y == ty) return true;
            return false;
        };
        const Unit* ne = vis_enemies.empty() ? nullptr : &vis_enemies[0];
        const CampView* nc = camps.empty() ? nullptr : &camps[0];
        const float b_spawn = ms.at(ms.d_own, x, y), b_altar = ms.at(ms.d_altar, x, y);
        const float b_near = gnear >= 0 ? ms.at(ms.groups[gnear].dist, x, y) : 0;
        const float b_best = gbest >= 0 ? ms.at(ms.groups[gbest].dist, x, y) : 0;
        const int stride = F_mv_ne_legal - F_mv_n_legal;
        for (int k = 0; k < 8; ++k) {
            const int nx = x + DIRS[k][0], ny = y + DIRS[k][1], b = F_mv_n_legal + k * stride;
            const bool inb = ms.in_bounds(nx, ny);
            put(b + 0, inb && ms.passable.is_set({nx, ny}) && !occupied(nx, ny));
            if (!inb) continue;
            put(b + 1, ms.at(ms.d_own, nx, ny) - b_spawn);
            put(b + 2, ms.at(ms.d_altar, nx, ny) - b_altar);
            if (gnear >= 0) put(b + 3, ms.at(ms.groups[gnear].dist, nx, ny) - b_near);
            if (gbest >= 0) put(b + 4, ms.at(ms.groups[gbest].dist, nx, ny) - b_best);
            if (ne) put(b + 5, cheb(nx, ny, ne->x, ne->y) - cheb(x, y, ne->x, ne->y));
            if (nc) put(b + 6, cheb(nx, ny, nc->x, nc->y) - cheb(x, y, nc->x, nc->y));
            int thr = 0, er3 = 0;
            for (auto& c : camps) thr += c.state == 1 && cheb(nx, ny, c.x, c.y) <= CAMP_RANGE[c.type];
            for (auto& e : vis_enemies) er3 += cheb(nx, ny, e.x, e.y) <= 3;
            put(b + 7, thr); put(b + 8, er3); put(b + 9, ms.bushes.is_set({nx, ny}));
        }

        // ---- teleport opportunity ----
        const bc::distance_field* fields[4] = {&ms.d_own, &ms.d_altar, gbest >= 0 ? &ms.groups[gbest].dist : nullptr, &ms.d_enemy};
        const int outs[4] = {F_tp_gain_spawn, F_tp_gain_altar, F_tp_gain_gbest, F_tp_gain_enemy_spawn};
        for (int k = 0; k < 4; ++k) {
            double best = 0;
            if (fields[k]) {
                const float here = ms.at(*fields[k], x, y);
                for (int ty = std::max(0, y - 3); ty < std::min(ms.H, y + 4); ++ty)
                    for (int tx = std::max(0, x - 3); tx < std::min(ms.W, x + 4); ++tx) {
                        const float d = ms.at(*fields[k], tx, ty);
                        if (ms.passable.is_set({tx, ty}) && !occupied(tx, ty) && d < FAR)
                            best = std::max(best, double(here - d));
                    }
            }
            put(outs[k], std::min(best, double(FAR)));
        }
    }

    // Gold, items and consumables only (they change while shopping).
    void refresh_economy(float* f) const {
        auto& me = bc::self();
        f[F_gold] = float(me.gold_count());
        const auto items = me.item_count();
        int shards[3] = {}, emblems[3] = {}, fam[ITEM_FAMILIES] = {}, value = 0;
        for (std::uint32_t i = 0; i < 84; ++i) {
            const int n = int(items[bc::item_type(i)]);
            if (!n) continue;
            const int family = int(i) / 6, rest = int(i) % 6, stat = rest / 2;
            if (rest % 2) {
                emblems[stat] += n;
                fam[family] = 1;
                value += n * (family == 12 ? 750 : 150);
            } else {
                shards[stat] += n;
                value += 30 * n;
            }
        }
        f[F_shards_str] = float(shards[0]); f[F_shards_foc] = float(shards[1]); f[F_shards_hp] = float(shards[2]);
        f[F_emblems_str] = float(emblems[0]); f[F_emblems_foc] = float(emblems[1]); f[F_emblems_hp] = float(emblems[2]);
        for (int i = 0; i < ITEM_FAMILIES; ++i) f[F_fam_0 + i] = float(fam[i]);
        f[F_item_value] = float(value);
        f[F_tp_crystals] = float(me.consumables()[bc::consumable_type::tp_crystal].count());
        f[F_altar_locks] = float(me.consumables()[bc::consumable_type::altar_lock].count());
    }

private:
    bool ready_ = false;

    static std::uint32_t id_of(const bc::champion& c) { return static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(c)); }

    static std::vector<bc::champion> sorted_champions(bc::team side) {
        auto v = bc::champions(side);
        std::sort(v.begin(), v.end(), [](const bc::champion& a, const bc::champion& b) { return id_of(a) < id_of(b); });
        return v;
    }

    template <class Fn>
    static double safe(Fn fn, double fallback = std::numeric_limits<double>::quiet_NaN()) {
        try {
            return fn();
        } catch (const std::exception&) {
            return fallback;
        }
    }

    static void buffs_of(const bc::champion& c, int& stun, double& shield) {
        stun = 0;
        shield = 0;
        for (const auto& b : c.buffs()) {
            if (auto* s = std::get_if<bc::buff_stun>(&b)) stun = s->duration;
            if (auto* s = std::get_if<bc::buff_shield>(&b)) shield = round1(s->value);
        }
    }

    static Unit unit_of(const bc::champion& c) {
        const auto p = c.position();
        Unit u{id_of(c), p.x, p.y, int(c.champion_name()), round1(c.health()), round1(c.max_health()), round1(c.strength()),
               round1(c.focus()), int(c.gem_count()), 0, c.state().contains(bc::champion_state::recalling), c};
        double shield = 0;
        buffs_of(c, u.stun, shield);
        return u;
    }

    static void sort_units(std::vector<Unit>& v, int x, int y) {
        std::stable_sort(v.begin(), v.end(), [x, y](const Unit& a, const Unit& b) {
            const int da = cheb(x, y, a.x, a.y), db = cheb(x, y, b.x, b.y);
            return da != db ? da < db : a.hp < b.hp;
        });
    }

    void put_group(float* f, int base, int gi, int x, int y) const {
        if (gi < 0) return;
        const auto& g = ms.groups[gi];
        const float d = ms.at(g.dist, x, y);
        if (d >= FAR) return;
        f[base + 0] = d; f[base + 1] = float(g.cx - x); f[base + 2] = float(g.cy - y);
        f[base + 3] = float(g.gems); f[base + 4] = float(g.gold); f[base + 5] = float(g.hp);
        for (int t = 0; t < 6; ++t) f[base + 6 + t] = float(g.n_by_type[t]);
    }

    void update_memory(int rnd, const std::vector<bc::champion>& enemies) {
        for (int gi = 0; gi < int(ms.groups.size()); ++gi) {
            std::uint32_t state = 0;
            if (bc::detail::sdk_camp_group_state(ms.groups[gi].handle, &state) != 0 || state == 0) continue;
            mem.g_state[gi] = int(state);
            mem.g_seen[gi] = rnd;
            if (state == 3) {
                std::int32_t cd = 0;
                bc::detail::sdk_camp_group_cooldown(ms.groups[gi].handle, &cd);
                mem.g_ready[gi] = rnd + std::max(cd, 0);
            }
        }
        for (const auto& e : enemies)
            if (e.is_visible()) {
                const auto p = e.position();
                mem.enemy_last[id_of(e)] = {rnd, p.x, p.y};
            }
    }
};

} // namespace pdi
