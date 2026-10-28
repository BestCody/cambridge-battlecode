// A champion turn driven entirely by the exported imitation models (training/export_cpp.py).
// Drop-in for a bot's main loop:
//
//     #include "pdi/pdi_policy.h"
//     int main() {
//         pdi::Policy policy;
//         while (true) {
//             try {
//                 if (cambc::is_draft()) pdi::draft();
//                 else policy.play_turn();
//             } catch (const std::exception& e) { std::cout << "pdi error: " << e.what() << '\n'; }
//             cambc::end_turn();
//         }
//     }
//
// Every decision comes from one head; when a head is missing for this champion (not exported, or a
// hero print dont int never plays) that decision is skipped. Turn structure mirrors theirs:
//   dead -> shop | channeling -> cancel? | on spawn -> shop | ward, tribute, teleport, lock,
//   attack / ability / move | end of turn -> (speculative) recall.
#pragma once
#include <array>
#include <cstdint>
#include <iostream>
#include <span>

#include <cambc.h>

#include "generated/pdi_models.h"
#include "pdi_features.h"
#include "pdi_tree.h"

namespace pdi {

// Draft: print dont int's fixed pick orders by team size (see the macro report):
//   3: Vesper > Marcella > HoR    4: Vesper > HoR > Vendetta > Marcella    5: ... > Ryth > Marcella
inline void draft() {
    using D = bc::champion_draft;
    static constexpr D three[] = {D::vesper_vale, D::marcella, D::heart_of_ruin};
    static constexpr D four[] = {D::vesper_vale, D::heart_of_ruin, D::vendetta, D::marcella};
    static constexpr D five[] = {D::vesper_vale, D::heart_of_ruin, D::vendetta, D::ryth, D::marcella};
    const auto size = bc::draft::team_size();
    const std::span<const D> order = size <= 3 ? std::span<const D>(three) : size == 4 ? std::span<const D>(four)
                                                                                     : std::span<const D>(five);
    const auto mine = bc::draft::current_draft(bc::team::ally);
    for (auto pick : order)
        if (std::find(mine.begin(), mine.end(), pick) == mine.end()) {
            bc::draft::pick(pick);
            return;
        }
}

class Policy {
public:
    // Fuel to keep in reserve; heads are skipped once the turn's budget runs below it.
    std::uint64_t fuel_reserve = 12000;
    // Binary heads act when P(yes) exceeds this.
    float threshold = 0.5f;
    bool log = false;
    // Print "PDIF <round> <champion id> f0,f1,..." every turn, for training/compare_features.py.
    bool dump_features = false;

    void play_turn() {
        auto& me = bc::self();
        const int type = int(me.champion_name());
        fb_.compute(x_);
        if (dump_features) {
            std::cout << "PDIF " << bc::round_number() << ' '
                      << static_cast<std::uint32_t>(static_cast<bc::handle_champion_t>(me));
            for (int i = 0; i < NUM_FEATURES; ++i)
                std::cout << (i ? ',' : ' ') << x_[i];
            std::cout << '\n';
        }

        if (!me.is_alive()) {
            shop(type);
            return;
        }
        const auto st = me.state();
        if (st.contains(bc::champion_state::recalling) || st.contains(bc::champion_state::submitting_gems)) {
            if (!yes(type, "cancel"))
                return;   // keep channeling
            if (st.contains(bc::champion_state::recalling)) me.cancel_recall();
            else me.cancel_submit_gems();
        }
        if (fb_.ms.own_spawn.is_set(me.position()))
            shop(type);
        if (me.buffs().has_stun())
            return;

        const auto pos = me.position();
        if (yes(type, "ward"))
            ward(type, pos);
        if (x_[F_gems] > 0 && x_[F_adj_altar] > 0 && yes(type, "submit")) {
            submit(pos);
            return;
        }
        if (x_[F_tp_crystals] > 0 && yes(type, "teleport") && teleport(type, pos))
            return;
        if (x_[F_altar_locks] > 0 && yes(type, "lock"))
            lock(pos);

        const int attack = cls(type, "attack");
        const int ability = cls(type, "ability");
        const int move = cls(type, "move", [&](int c) { return c == 8 || x_[F_mv_n_legal + c * (F_mv_ne_legal - F_mv_n_legal)] > 0; });

        // Attack first when the target is already in range (ranged champions then cannot walk);
        // otherwise walk first and attack if the step brought it into range.
        const auto target = resolve(attack);
        const bool in_range_now = target && target->in_range(me.position(), me.attack_range());
        if (in_range_now) try_attack(*target);
        if (ability > 0) cast(type, ability - 1);
        if (move >= 0 && move < 8 && me.can_move()) {
            try {
                me.move({DIRS[move][0], DIRS[move][1]});
            } catch (const std::exception& e) {
                if (log) std::cout << "pdi move failed: " << e.what() << '\n';
            }
        }
        if (!in_range_now && target && target->in_range(me.position(), me.attack_range())) try_attack(*target);

        if (yes(type, "recall")) {
            try {
                me.recall();
            } catch (const std::exception&) {
            }
        }
    }

private:
    FeatureBuilder fb_;
    float x_[NUM_FEATURES]{};

    struct Target {
        std::optional<bc::champion> champ;
        std::optional<bc::camp> camp;
        std::optional<bc::altar_lock> lock;
        bc::vec2 pos;
        bool in_range(bc::vec2 from, int range) const { return cheb(from.x, from.y, pos.x, pos.y) <= range; }
    };

    bool budget() const { return bc::fuel_remaining() > fuel_reserve; }

    const Model* model(int type, const char* head) const { return budget() ? models::find(type, head) : nullptr; }

    bool yes(int type, const char* head) const {
        const Model* m = model(type, head);
        return m && yes_probability(*m, x_) > threshold;
    }

    template <class Allowed>
    int cls(int type, const char* head, Allowed allowed) const {
        const Model* m = model(type, head);
        return m ? best_class(*m, x_, allowed) : -1;
    }
    int cls(int type, const char* head) const { return cls(type, head, [](int) { return true; }); }

    // Target classes (features.TARGET_CLASSES): 0 none, 1-3 enemy slots, 4-7 camp slots,
    // 8 other enemy, 9 other camp, 10 altar lock, 11 self/ground.
    std::optional<Target> resolve(int cls_id) const {
        if (cls_id >= 1 && cls_id <= 3 && cls_id - 1 < int(fb_.vis_enemies.size())) {
            const auto& u = fb_.vis_enemies[cls_id - 1];
            return Target{u.handle, std::nullopt, std::nullopt, {u.x, u.y}};
        }
        if (cls_id >= 4 && cls_id <= 7 && cls_id - 4 < int(fb_.camps.size())) {
            const auto& c = fb_.camps[cls_id - 4];
            return Target{std::nullopt, c.camp, std::nullopt, {c.x, c.y}};
        }
        if (cls_id == 8 && !fb_.vis_enemies.empty()) {
            const auto& u = fb_.vis_enemies.back();
            return Target{u.handle, std::nullopt, std::nullopt, {u.x, u.y}};
        }
        if (cls_id == 9 && !fb_.camps.empty()) {
            const auto& c = fb_.camps.back();
            return Target{std::nullopt, c.camp, std::nullopt, {c.x, c.y}};
        }
        if (cls_id == 10)
            for (const auto& l : bc::vision::altar_locks())
                if (l.is_enemy()) return Target{std::nullopt, std::nullopt, l, l.position()};
        return std::nullopt;
    }

    void try_attack(const Target& t) {
        auto& me = bc::self();
        if (!me.can_attack()) return;
        try {
            if (t.champ) me.attack(*t.champ);
            else if (t.camp) me.attack(*t.camp);
            else if (t.lock) me.attack(*t.lock);
        } catch (const std::exception& e) {
            if (log) std::cout << "pdi attack failed: " << e.what() << '\n';
        }
    }

    void cast(int type, int slot) {
        auto& me = bc::self();
        if (!me.can_cast()) return;
        const auto ab = me.abilities()[slot];
        try {
            if (!ab.is_ready()) return;
            if (ab.is_dash()) {
                const int off = cls(type, "dash");
                if (off < 0) return;
                const auto p = me.position();
                ab.use(bc::vec2{p.x + off % 5 - 2, p.y + off / 5 - 2});
            } else if (ab.can_target_enemies() || ab.can_target_camps()) {
                const auto t = resolve(cls(type, "ability_target"));
                if (!t) return;
                if (t->champ) ab.use(*t->champ);
                else if (t->camp) ab.use(*t->camp);
                else if (ab.can_target_ground()) ab.use(t->pos);
            } else if (ab.can_target_ground()) {
                const auto t = resolve(cls(type, "ability_target"));
                if (t) ab.use(t->pos);
            } else {
                ab.use();   // self-cast: attack resets, shields
            }
        } catch (const std::exception& e) {
            if (log) std::cout << "pdi cast failed: " << e.what() << '\n';
        }
    }

    void ward(int type, bc::vec2 pos) {
        const int off = cls(type, "ward_offset");
        if (off < 0) return;
        try {
            bc::self().place_ward({pos.x + off % 5 - 2, pos.y + off / 5 - 2});
        } catch (const std::exception&) {
        }
    }

    void submit(bc::vec2 pos) {
        for (auto a : fb_.ms.altar_tiles)
            if (cheb(pos.x, pos.y, a.x, a.y) <= 1) {
                try {
                    bc::self().submit_gems(a);
                    return;
                } catch (const std::exception&) {
                }
            }
    }

    bool teleport(int type, bc::vec2 pos) {
        const int off = cls(type, "tp_offset");
        if (off < 0) return false;
        try {
            return bc::self().consumables()[bc::consumable_type::tp_crystal].use(bc::vec2{pos.x + off % 7 - 3, pos.y + off / 7 - 3});
        } catch (const std::exception&) {
            return false;
        }
    }

    void lock(bc::vec2 pos) {
        for (auto a : fb_.ms.altar_tiles)
            if (cheb(pos.x, pos.y, a.x, a.y) <= 2) {
                try {
                    bc::self().consumables()[bc::consumable_type::altar_lock].use(a);
                    return;
                } catch (const std::exception&) {
                }
            }
    }

    // Shop decisions: ask the shop head repeatedly (economy refreshed between buys) until it says stop.
    void shop(int type) {
        auto& me = bc::self();
        for (int i = 0; i < 30; ++i) {
            const Model* m = model(type, "shop");
            if (!m) return;
            const int gold = int(me.gold_count());
            const int item = best_class(*m, x_, [&](int c) {
                if (c == 88) return true;                       // stop
                if (c == 84) return me.can_buy(bc::consumable_type::tp_crystal);
                if (c == 86) return me.can_buy(bc::consumable_type::altar_lock);
                if (c >= 84) return false;                      // control ward / tp scroll: not in the SDK
                return gold >= 30 && me.can_buy(bc::item_type(std::uint32_t(c)));
            });
            if (item < 0 || item == 88) return;
            try {
                if (item == 84) me.buy(bc::consumable_type::tp_crystal);
                else if (item == 86) me.buy(bc::consumable_type::altar_lock);
                else me.buy(bc::item_type(std::uint32_t(item)));
            } catch (const std::exception&) {
                return;
            }
            fb_.refresh_economy(x_);
        }
    }
};

} // namespace pdi
