"""Turn records (from extract/) -> one fixed-length feature vector per champion turn, plus labels.

Everything here must be computable by a deployed bot from what the SDK exposes to *that* champion,
so the same function can be ported line for line to C++ (cpp/pdi_features.h mirrors this file; keep
FEATURE_VERSION in sync and bump it whenever a feature changes). Inputs are restricted to:

  * static map data (walls, holes, bushes, altars, spawns, camp groups) - available to every bot;
  * the champion's own state (hp, gold, gems, items, cooldowns, channel, ward, ...);
  * what the engine reported visible from this champion's perspective this turn
    (enemies/allies/camps/camp-group states/altar locks/teleports);
  * this champion's *own* memory of earlier turns (vision is per champion, and we ignore the
    8-byte shared memory and the 1024-byte draft memory, which we cannot see into).

Missing values are NaN (e.g. no visible enemy); XGBoost routes them down learned default branches
and the exported C++ does the same.
"""
import math
from collections import deque

import numpy as np

FEATURE_VERSION = 4

CHAMPS = ["ryth", "vesper", "vendetta", "cassi", "issac", "stellar", "prism", "marcella", "willow", "ren", "varg", "tavi", "hor"]
CAMP_HP = [1350, 2400, 120, 120, 20000, 5000]    # wolf, elk, gem cluster, gold cluster, monolith, golem
CAMP_STR = [20, 60, 0, 0, 200, 120]
CAMP_RANGE = [3, 4, 0, 0, 5, 4]
# Move directions in a fixed order (dx, dy); y grows downwards in map coordinates.
DIRS = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)]
DIR_NAMES = ["n", "ne", "e", "se", "s", "sw", "w", "nw"]
FAR = 500.0          # path distance for unreachable tiles
N_ENEMY_SLOTS, N_ALLY_SLOTS, N_CAMP_SLOTS = 3, 2, 4
ITEM_FAMILIES = 14
WIN_GEMS = 1000

# Step kinds in turn records (see extract/engine.mjs STEP).
K_MOVE, K_ATTACK, K_ABILITY, K_RECALL, K_SUBMIT, K_BUY, K_WARD, K_CONSUMABLE, K_CANCEL = 4, 5, 6, 11, 12, 13, 14, 15, 16
K_LOCK, K_TP_CHANNEL, K_BLOCKED = 20, 21, 23
ST_ALIVE, ST_RECALLING, ST_SUBMITTING, ST_CAN_MOVE, ST_CAN_ATTACK, ST_CAN_CAST = 1, 2, 4, 8, 16, 2048


def cheb(ax, ay, bx, by):
    return max(abs(ax - bx), abs(ay - by))


# ----------------------------------------------------------------------------------------------
# Static map data (computed once per map; the C++ bot computes the same at its first turn)
# ----------------------------------------------------------------------------------------------
class MapStatic:
    def __init__(self, header):
        self.W, self.H = header["W"], header["H"]
        grid = lambda rows: np.array([[c == "1" for c in r] for r in rows], dtype=bool)
        self.walls, self.bushes = grid(header["walls"]), grid(header["bushes"])
        self.altars, self.holes = grid(header["altars"]), grid(header["holes"])
        self.team = header["team"]
        self.symmetry = header.get("symmetry", -1)
        sp = header["spawns"]
        self.own_spawn = self._rect(sp[self.team])
        self.enemy_spawn = self._rect(sp[1 - self.team])
        self.passable = ~(self.walls | self.holes | self.altars | self.enemy_spawn)
        self.altar_tiles = [(int(x), int(y)) for y, x in zip(*np.nonzero(self.altars))]
        near_altar = self._dilate(self.altars) & self.passable
        self.groups = header["groups"]
        self.group_index = {g["group"]: i for i, g in enumerate(self.groups)}   # engine handle -> list index
        self.group_of_tile = {}
        for gi, g in enumerate(self.groups):
            for c in g["camps"]:
                self.group_of_tile[(c["x"], c["y"])] = gi
            types = [c["type"] for c in g["camps"]]
            g["n_by_type"] = [types.count(t) for t in range(6)]
            g["hp"] = sum(CAMP_HP[t] for t in types)
            g["cx"] = sum(c["x"] for c in g["camps"]) / len(g["camps"])
            g["cy"] = sum(c["y"] for c in g["camps"]) / len(g["camps"])
        self.d_own = self.bfs(self.own_spawn)
        self.d_enemy = self.bfs(self._dilate(self.enemy_spawn) & self.passable)
        self.d_altar = self.bfs(near_altar)
        self.d_group = []
        for g in self.groups:
            roots = np.zeros_like(self.passable)
            for c in g["camps"]:
                roots[c["y"], c["x"]] = True
            self.d_group.append(self.bfs(roots))
        self.total_gems = sum(g["gems"] for g in self.groups)

    def _rect(self, r):
        m = np.zeros((self.H, self.W), dtype=bool)
        m[r["y0"]:r["y1"], r["x0"]:r["x1"]] = True
        return m

    def _dilate(self, m):
        out = m.copy()
        for dx, dy in DIRS:
            out |= self._shift(m, dx, dy)
        return out

    @staticmethod
    def _shift(m, dx, dy):
        out = np.zeros_like(m)
        H, W = m.shape
        out[max(dy, 0):H + min(dy, 0), max(dx, 0):W + min(dx, 0)] = m[max(-dy, 0):H + min(-dy, 0), max(-dx, 0):W + min(-dx, 0)]
        return out

    def bfs(self, roots):
        """8-connected step distance from any root through passable tiles (roots always count)."""
        dist = np.full((self.H, self.W), FAR, dtype=np.float32)
        H, W = self.H, self.W
        q = deque()
        for y, x in zip(*np.nonzero(roots)):
            dist[y, x] = 0
            q.append((int(x), int(y)))
        passable = self.passable
        while q:
            x, y = q.popleft()
            nd = dist[y, x] + 1
            for dx, dy in DIRS:
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and passable[ny, nx] and dist[ny, nx] > nd:
                    dist[ny, nx] = nd
                    q.append((nx, ny))
        return dist

    def at(self, field, x, y):
        if 0 <= x < self.W and 0 <= y < self.H:
            return float(field[y, x])
        return FAR


# ----------------------------------------------------------------------------------------------
# Per-champion memory (each bot instance keeps its own; nothing is shared between champions)
# ----------------------------------------------------------------------------------------------
class Memory:
    def __init__(self, ms):
        n = len(ms.groups)
        self.g_state = [1] * n            # every group starts alive
        self.g_seen = [0] * n             # round of last observation
        self.g_ready = [0] * n            # round a respawning group is expected back
        self.enemy_last = {}              # enemy id -> (round, x, y)

    def update(self, ms, rnd, obs):
        for handle, state, cd, _cap, _timer in obs["groups"]:
            g = ms.group_index[handle]
            self.g_state[g] = state
            self.g_seen[g] = rnd
            if state == 3:
                self.g_ready[g] = rnd + max(cd, 0)
        for e in obs["enemies"]:
            if e.get("vis"):
                self.enemy_last[e["id"]] = (rnd, e["x"], e["y"])

    def alive_guess(self, g, rnd):
        s = self.g_state[g]
        return s == 1 or (s == 3 and rnd >= self.g_ready[g])


# ----------------------------------------------------------------------------------------------
# Feature vector
# ----------------------------------------------------------------------------------------------
def _item_aggregates(items):
    """items: {item_type id (str): count}. Shard and emblem counts by stat, emblem families, value."""
    shards = [0, 0, 0]
    emblems = [0, 0, 0]
    fam = [0] * ITEM_FAMILIES
    value = 0
    for k, n in items.items():
        i = int(k)
        family, rest = divmod(i, 6)
        stat, emblem = divmod(rest, 2)
        if emblem:
            emblems[stat] += n
            fam[family] = 1
            value += n * (750 if family == 12 else 150)
        else:
            shards[stat] += n
            value += 30 * n
    return shards, emblems, fam, value


def feature_names():
    names = ["round", "round_frac", "sub_ally", "sub_enemy", "sub_diff", "gems_to_win", "map_w", "map_h",
             "n_altars", "n_groups", "map_gems", "symmetry", "side", "n_allies", "n_enemies",
             "allies_alive", "enemies_alive"]
    names += ["alive", "x_frac", "y_frac", "hp", "max_hp", "hp_frac", "str", "focus", "gold", "gems",
              "can_win_now", "cd0", "cd1", "ward_charges", "ward_cd", "ward_dist", "chan", "recalling", "submitting",
              "can_move", "can_attack", "can_cast", "stun", "shield", "in_bush", "on_spawn",
              "d_spawn", "d_enemy_spawn", "d_altar", "cheb_altar", "adj_altar",
              "shards_str", "shards_foc", "shards_hp", "emblems_str", "emblems_foc", "emblems_hp", "item_value",
              "tp_crystals", "altar_locks", "priority"]
    names += [f"fam_{i}" for i in range(ITEM_FAMILIES)]
    names += ["n_vis_enemies", "enemy_hp_r4", "n_enemies_r3", "max_enemy_gems", "since_enemy_seen", "last_enemy_dist"]
    for i in range(N_ENEMY_SLOTS):
        names += [f"e{i}_{f}" for f in ("dx", "dy", "dist", "hp", "hp_frac", "type", "gems", "stun", "recalling", "str", "focus")]
    names += ["n_vis_allies", "ally_hp_r4", "n_allies_r3", "hp_ratio_r4"]
    for i in range(N_ALLY_SLOTS):
        names += [f"a{i}_{f}" for f in ("dx", "dy", "dist", "hp_frac", "type", "gems")]
    names += ["n_vis_camps", "n_awake", "threat_n", "threat_str"]
    for i in range(N_CAMP_SLOTS):
        names += [f"c{i}_{f}" for f in ("dx", "dy", "dist", "type", "hp", "hp_frac", "awake", "group_gems", "group_left")]
    for tag in ("gnear", "gbest"):
        names += [f"{tag}_{f}" for f in ("dist", "dx", "dy", "gems", "gold", "hp", "n_wolf", "n_elk", "n_gem", "n_gold",
                                          "n_monolith", "n_golem")]
    names += ["greward_dist", "greward_gems", "groups_alive_known", "groups_alive_20", "cap_state", "cap_timer",
              "n_enemy_locks", "n_ally_locks", "lock_on_near_altar", "n_enemy_tps"]
    for d in DIR_NAMES:
        names += [f"mv_{d}_{f}" for f in ("legal", "d_spawn", "d_altar", "d_gnear", "d_gbest", "d_enemy", "d_camp",
                                       "threat", "enemies_r3", "bush")]
    names += ["tp_gain_spawn", "tp_gain_altar", "tp_gain_gbest", "tp_gain_enemy_spawn"]
    return names


NAMES = feature_names()
assert len(set(NAMES)) == len(NAMES), "duplicate feature names"
INDEX = {n: i for i, n in enumerate(NAMES)}
NF = len(NAMES)


def nan_if(v, cond):
    return float("nan") if cond else float(v)


def turn_features(ms, mem, rec, econ=None):
    """Feature vector for one turn record. `econ` overrides the self economy (shop decisions)."""
    nan = float("nan")
    o = rec["obs"]
    s = o["self"]
    e_now = econ or o
    f = [nan] * NF
    put = lambda name, v: f.__setitem__(INDEX[name], float(v))
    rnd = rec["r"]
    alive = bool(s.get("alive", 1)) and s.get("vis", 0)
    x, y = (s["x"], s["y"]) if alive else (None, None)
    if econ and econ.get("x") is not None:
        x, y = econ["x"], econ["y"]

    # ---- context ----
    sub_a, sub_e = o["sub"]
    put("round", rnd); put("round_frac", rnd / 2000)
    put("sub_ally", sub_a); put("sub_enemy", sub_e); put("sub_diff", sub_a - sub_e)
    put("map_w", ms.W); put("map_h", ms.H); put("n_altars", len(ms.altar_tiles)); put("n_groups", len(ms.groups))
    put("map_gems", ms.total_gems); put("symmetry", ms.symmetry); put("side", ms.team)
    allies, enemies = o["allies"], o["enemies"]
    put("n_allies", len(allies) + 1); put("n_enemies", len(enemies))
    put("allies_alive", sum(1 for a in allies if a.get("alive", 1) == 1) + int(alive))
    put("enemies_alive", sum(1 for e in enemies if e.get("alive", 1) == 1))

    # ---- self ----
    gems = e_now.get("gems") or 0
    put("alive", alive)
    put("gold", e_now.get("gold") or 0); put("gems", gems)
    put("gems_to_win", WIN_GEMS - sub_a - gems); put("can_win_now", gems + sub_a >= WIN_GEMS)
    cd = e_now.get("cd") or [None, None]
    put("cd0", nan_if(cd[0], cd[0] is None)); put("cd1", nan_if(cd[1], cd[1] is None))
    ward = s.get("ward") or [0, 0]
    put("ward_charges", ward[0]); put("ward_cd", ward[1])
    st = s.get("st", 0) or 0
    put("recalling", bool(st & ST_RECALLING)); put("submitting", bool(st & ST_SUBMITTING))
    put("can_move", bool(st & ST_CAN_MOVE)); put("can_attack", bool(st & ST_CAN_ATTACK)); put("can_cast", bool(st & ST_CAN_CAST))
    chan = s.get("chan")
    put("chan", -1 if chan is None else chan)
    shards, emblems, fam, value = _item_aggregates(e_now.get("items") or {})
    for i, k in enumerate(("str", "foc", "hp")):
        put(f"shards_{k}", shards[i]); put(f"emblems_{k}", emblems[i])
    for i in range(ITEM_FAMILIES):
        put(f"fam_{i}", fam[i])
    put("item_value", value)
    cons = e_now.get("cons") or [0, 0, 0, 0]
    put("tp_crystals", cons[0]); put("altar_locks", cons[2]); put("priority", s.get("prio", 0))
    if alive:
        put("hp", s["hp"]); put("max_hp", s["mhp"]); put("hp_frac", s["hp"] / max(s["mhp"], 1))
        put("str", s["str"]); put("focus", s["foc"]); put("stun", s.get("stun", 0)); put("shield", s.get("shield", 0))
        put("x_frac", x / ms.W); put("y_frac", y / ms.H)
        put("in_bush", ms.bushes[y, x]); put("on_spawn", ms.own_spawn[y, x])
        put("d_spawn", ms.at(ms.d_own, x, y)); put("d_enemy_spawn", ms.at(ms.d_enemy, x, y)); put("d_altar", ms.at(ms.d_altar, x, y))
        ca = min((cheb(x, y, ax, ay) for ax, ay in ms.altar_tiles), default=FAR)
        put("cheb_altar", ca); put("adj_altar", ca <= 1)
        wp = s.get("wardPos") or []
        put("ward_dist", min((cheb(x, y, wx, wy) for wx, wy in wp), default=FAR))
    if not alive:
        mem.update(ms, rnd, o)
        return f

    # ---- enemies ----
    vis_e = [e for e in enemies if e.get("vis")]
    vis_e.sort(key=lambda e: (cheb(x, y, e["x"], e["y"]), e["hp"]))
    put("n_vis_enemies", len(vis_e))
    put("enemy_hp_r4", sum(e["hp"] for e in vis_e if cheb(x, y, e["x"], e["y"]) <= 4))
    put("n_enemies_r3", sum(1 for e in vis_e if cheb(x, y, e["x"], e["y"]) <= 3))
    put("max_enemy_gems", max((e.get("gems") or 0 for e in vis_e), default=0))
    last = [v for v in mem.enemy_last.values()]
    if vis_e:
        put("since_enemy_seen", 0)
    elif last:
        put("since_enemy_seen", rnd - max(v[0] for v in last))
    else:
        put("since_enemy_seen", 2000)
    if last:
        put("last_enemy_dist", min(cheb(x, y, lx, ly) for _, lx, ly in last))
    for i, e in enumerate(vis_e[:N_ENEMY_SLOTS]):
        p = f"e{i}_"
        put(p + "dx", e["x"] - x); put(p + "dy", e["y"] - y); put(p + "dist", cheb(x, y, e["x"], e["y"]))
        put(p + "hp", e["hp"]); put(p + "hp_frac", e["hp"] / max(e["mhp"], 1)); put(p + "type", e.get("type", -1))
        put(p + "gems", e.get("gems") or 0); put(p + "stun", e.get("stun", 0)); put(p + "recalling", bool((e.get("st") or 0) & ST_RECALLING))
        put(p + "str", e["str"]); put(p + "focus", e["foc"])

    # ---- allies ----
    vis_a = [a for a in allies if a.get("vis")]
    vis_a.sort(key=lambda a: (cheb(x, y, a["x"], a["y"]), a["hp"]))
    put("n_vis_allies", len(vis_a))
    ally_hp = s["hp"] + sum(a["hp"] for a in vis_a if cheb(x, y, a["x"], a["y"]) <= 4)
    put("ally_hp_r4", ally_hp)
    put("n_allies_r3", sum(1 for a in vis_a if cheb(x, y, a["x"], a["y"]) <= 3))
    put("hp_ratio_r4", ally_hp / (f[INDEX["enemy_hp_r4"]] + 1))
    for i, a in enumerate(vis_a[:N_ALLY_SLOTS]):
        p = f"a{i}_"
        put(p + "dx", a["x"] - x); put(p + "dy", a["y"] - y); put(p + "dist", cheb(x, y, a["x"], a["y"]))
        put(p + "hp_frac", a["hp"] / max(a["mhp"], 1)); put(p + "type", a.get("type", -1)); put(p + "gems", a.get("gems") or 0)

    # ---- camps ----
    camps = [c for c in o["camps"] if c[3] > 0]   # [handle, x, y, hp, maxHp, state]; state 1 awake, 2 asleep
    camp_type = {}
    for g in ms.groups:
        for c in g["camps"]:
            camp_type[c["handle"]] = c["type"]
    camps.sort(key=lambda c: (cheb(x, y, c[1], c[2]), c[3]))
    put("n_vis_camps", len(camps))
    put("n_awake", sum(1 for c in camps if c[5] == 1))
    threat = [c for c in camps if c[5] == 1 and cheb(x, y, c[1], c[2]) <= CAMP_RANGE[camp_type.get(c[0], 0)]]
    put("threat_n", len(threat)); put("threat_str", sum(CAMP_STR[camp_type.get(c[0], 0)] for c in threat))
    alive_in_group = {}
    for c in camps:
        g = ms.group_of_tile.get(_spawn_of(ms, c[0]))
        alive_in_group[g] = alive_in_group.get(g, 0) + 1
    for i, c in enumerate(camps[:N_CAMP_SLOTS]):
        p = f"c{i}_"
        t = camp_type.get(c[0], 0)
        g = ms.group_of_tile.get(_spawn_of(ms, c[0]))
        put(p + "dx", c[1] - x); put(p + "dy", c[2] - y); put(p + "dist", cheb(x, y, c[1], c[2])); put(p + "type", t)
        put(p + "hp", c[3]); put(p + "hp_frac", c[3] / max(c[4], 1)); put(p + "awake", c[5] == 1)
        put(p + "group_gems", ms.groups[g]["gems"] if g is not None else 0)
        put(p + "group_left", alive_in_group.get(g, 0))

    # ---- camp groups (memory) ----
    mem.update(ms, rnd, o)
    alive_groups = [gi for gi in range(len(ms.groups)) if mem.alive_guess(gi, rnd)]
    dists = {gi: float(ms.d_group[gi][y, x]) for gi in alive_groups}
    put("groups_alive_known", len(alive_groups))
    put("groups_alive_20", sum(1 for d in dists.values() if d <= 20))
    gnear = min(alive_groups, key=lambda gi: dists[gi], default=None)
    gbest = max(alive_groups, key=lambda gi: (ms.groups[gi]["gems"] + 0.1 * ms.groups[gi]["gold"]) / (dists[gi] + 15),
                default=None)
    for tag, gi in (("gnear", gnear), ("gbest", gbest)):
        if gi is None or dists[gi] >= FAR:
            continue
        g = ms.groups[gi]
        put(f"{tag}_dist", dists[gi]); put(f"{tag}_dx", g["cx"] - x); put(f"{tag}_dy", g["cy"] - y)
        put(f"{tag}_gems", g["gems"]); put(f"{tag}_gold", g["gold"]); put(f"{tag}_hp", g["hp"])
        for t, nm in enumerate(("n_wolf", "n_elk", "n_gem", "n_gold", "n_monolith", "n_golem")):
            put(f"{tag}_{nm}", g["n_by_type"][t])
    rewards = [gi for gi in range(len(ms.groups)) if mem.g_state[gi] == 2 and rnd - mem.g_seen[gi] <= 30]
    if rewards:
        gr = min(rewards, key=lambda gi: float(ms.d_group[gi][y, x]))
        put("greward_dist", ms.d_group[gr][y, x]); put("greward_gems", ms.groups[gr]["gems"])
    here = ms.group_of_tile.get((x, y))
    put("cap_state", -1); put("cap_timer", -1)
    if here is not None:
        for g, state, cd, cap, timer in o["groups"]:
            if g == ms.groups[here]["group"]:
                put("cap_state", cap); put("cap_timer", timer)
    locks = o.get("locks") or []
    put("n_enemy_locks", sum(1 for l in locks if l["team"] != ms.team))
    put("n_ally_locks", sum(1 for l in locks if l["team"] == ms.team))
    if ms.altar_tiles:
        ax, ay = min(ms.altar_tiles, key=lambda a: cheb(x, y, a[0], a[1]))
        put("lock_on_near_altar", any(l["x"] == ax and l["y"] == ay and l["team"] != ms.team for l in locks))
    put("n_enemy_tps", sum(1 for t in (o.get("tps") or []) if t["team"] != ms.team))

    # ---- per-direction move features ----
    occupied = {(u["x"], u["y"]) for u in vis_e + vis_a} | {(c[1], c[2]) for c in camps}
    ne = vis_e[0] if vis_e else None
    nc = camps[0] if camps else None
    base = {"d_spawn": ms.at(ms.d_own, x, y), "d_altar": ms.at(ms.d_altar, x, y),
            "d_gnear": ms.at(ms.d_group[gnear], x, y) if gnear is not None else None,
            "d_gbest": ms.at(ms.d_group[gbest], x, y) if gbest is not None else None}
    for (dx, dy), dn in zip(DIRS, DIR_NAMES):
        nx, ny = x + dx, y + dy
        inb = 0 <= nx < ms.W and 0 <= ny < ms.H
        legal = inb and ms.passable[ny, nx] and (nx, ny) not in occupied
        put(f"mv_{dn}_legal", legal)
        if not inb:
            continue
        put(f"mv_{dn}_d_spawn", ms.at(ms.d_own, nx, ny) - base["d_spawn"])
        put(f"mv_{dn}_d_altar", ms.at(ms.d_altar, nx, ny) - base["d_altar"])
        if gnear is not None:
            put(f"mv_{dn}_d_gnear", ms.at(ms.d_group[gnear], nx, ny) - base["d_gnear"])
        if gbest is not None:
            put(f"mv_{dn}_d_gbest", ms.at(ms.d_group[gbest], nx, ny) - base["d_gbest"])
        if ne is not None:
            put(f"mv_{dn}_d_enemy", cheb(nx, ny, ne["x"], ne["y"]) - cheb(x, y, ne["x"], ne["y"]))
        if nc is not None:
            put(f"mv_{dn}_d_camp", cheb(nx, ny, nc[1], nc[2]) - cheb(x, y, nc[1], nc[2]))
        put(f"mv_{dn}_threat", sum(1 for c in camps if c[5] == 1 and cheb(nx, ny, c[1], c[2]) <= CAMP_RANGE[camp_type.get(c[0], 0)]))
        put(f"mv_{dn}_enemies_r3", sum(1 for e in vis_e if cheb(nx, ny, e["x"], e["y"]) <= 3))
        put(f"mv_{dn}_bush", ms.bushes[ny, nx])

    # ---- teleport opportunity: best path-distance saving from landing within Chebyshev 3 ----
    gains = {"spawn": 0.0, "altar": 0.0, "gbest": 0.0, "enemy_spawn": 0.0}
    fields = {"spawn": ms.d_own, "altar": ms.d_altar, "enemy_spawn": ms.d_enemy,
              "gbest": ms.d_group[gbest] if gbest is not None else None}
    for k, fld in fields.items():
        if fld is None:
            continue
        here_d = fld[y, x]
        best = 0.0
        for ty in range(max(0, y - 3), min(ms.H, y + 4)):
            for tx in range(max(0, x - 3), min(ms.W, x + 4)):
                if ms.passable[ty, tx] and (tx, ty) not in occupied and fld[ty, tx] < FAR:
                    best = max(best, float(here_d - fld[ty, tx]))
        gains[k] = min(best, FAR)
    put("tp_gain_spawn", gains["spawn"]); put("tp_gain_altar", gains["altar"])
    put("tp_gain_gbest", gains["gbest"]); put("tp_gain_enemy_spawn", gains["enemy_spawn"])
    return f


def _spawn_of(ms, handle):
    """Spawn tile of a camp handle (camps can move while chasing? they don't; spawn == group tile)."""
    sp = ms.camp_spawn.get(handle) if hasattr(ms, "camp_spawn") else None
    if sp is None:
        ms.camp_spawn = {c["handle"]: (c["x"], c["y"]) for g in ms.groups for c in g["camps"]}
        sp = ms.camp_spawn.get(handle)
    return sp


# ----------------------------------------------------------------------------------------------
# Labels
# ----------------------------------------------------------------------------------------------
# Target classes shared by the attack and ability-target heads.
TARGET_CLASSES = ["none"] + [f"enemy{i}" for i in range(N_ENEMY_SLOTS)] + [f"camp{i}" for i in range(N_CAMP_SLOTS)] + \
                 ["enemy_other", "camp_other", "altar_lock", "self_or_ground"]
OFFSET5 = [(dx, dy) for dy in range(-2, 3) for dx in range(-2, 3)]       # dash / ward offsets
OFFSET7 = [(dx, dy) for dy in range(-3, 4) for dx in range(-3, 4)]       # teleport offsets


def _target_class(rec, x, y, tx, ty):
    o = rec["obs"]
    if (tx, ty) == (x, y):
        return TARGET_CLASSES.index("self_or_ground")
    vis_e = sorted((e for e in o["enemies"] if e.get("vis")), key=lambda e: (cheb(x, y, e["x"], e["y"]), e["hp"]))
    for i, e in enumerate(vis_e):
        if (e["x"], e["y"]) == (tx, ty):
            return 1 + i if i < N_ENEMY_SLOTS else TARGET_CLASSES.index("enemy_other")
    camps = sorted((c for c in o["camps"] if c[3] > 0), key=lambda c: (cheb(x, y, c[1], c[2]), c[3]))
    for i, c in enumerate(camps):
        if (c[1], c[2]) == (tx, ty):
            return 1 + N_ENEMY_SLOTS + i if i < N_CAMP_SLOTS else TARGET_CLASSES.index("camp_other")
    if any(l["x"] == tx and l["y"] == ty for l in (o.get("locks") or [])):
        return TARGET_CLASSES.index("altar_lock")
    # Ground-targeted spells: the visible unit nearest the target tile.
    units = [(cheb(tx, ty, e["x"], e["y"]), 1 + i) for i, e in enumerate(vis_e[:N_ENEMY_SLOTS])]
    units += [(cheb(tx, ty, c[1], c[2]), 1 + N_ENEMY_SLOTS + i) for i, c in enumerate(camps[:N_CAMP_SLOTS])]
    units = [u for u in units if u[0] <= 1]
    return min(units)[1] if units else TARGET_CLASSES.index("self_or_ground")


def shop_item_label(before, after):
    """Item bought in one BUY step: the shard id (a completed emblem is mapped back to its shard),
    or 84 + consumable slot."""
    bi, ai = before.get("items") or {}, after.get("items") or {}
    for k, n in ai.items():
        if n > bi.get(k, 0):
            i = int(k)
            return i - (i % 2)            # emblem id -> its shard id
    bc, ac = before.get("cons") or [0] * 4, after.get("cons") or [0] * 4
    for i in range(4):
        if ac[i] > bc[i]:
            return 84 + i
    return None


SHOP_NONE = 88


def turn_labels(ms, rec, future):
    """Labels for one turn. -1 means 'not applicable to this turn' (row excluded from that head)."""
    o = rec["obs"]
    s = o["self"]
    alive = bool(s.get("alive", 1)) and s.get("vis", 0)
    steps = [st["act"] for st in rec["steps"]]
    kinds = [a["k"] for a in steps]
    st = s.get("st", 0) or 0
    channeling = bool(st & (ST_RECALLING | ST_SUBMITTING))
    cancelled = K_CANCEL in kinds
    free = alive and not s.get("stun") and (not channeling or cancelled)
    L = {}
    L["cancel"] = int(cancelled) if alive and channeling else -1
    x, y = (s.get("x"), s.get("y"))
    L["move"] = L["attack"] = L["ability"] = L["ward"] = L["recall"] = -1
    L["ability_target"] = L["dash"] = L["ward_offset"] = L["submit"] = L["lock"] = L["teleport"] = L["tp_offset"] = -1
    L["recall_commit"] = -1
    if free:
        mv = next((a for a in steps if a["k"] == K_MOVE and cheb(a["fx"], a["fy"], a["tx"], a["ty"]) == 1), None)
        L["move"] = DIRS.index((mv["tx"] - mv["fx"], mv["ty"] - mv["fy"])) if mv else 8
        at = next((a for a in steps if a["k"] == K_ATTACK), None)
        L["attack"] = _target_class(rec, x, y, at["tx"], at["ty"]) if at else 0
        ab = next((a for a in steps if a["k"] == K_ABILITY), None)
        L["ability"] = 1 + ab["slot"] if ab else 0
        if ab and ab.get("at") == 6:          # dash: landing offset from where it was cast
            off = (ab["tx"] - ab["fx"], ab["ty"] - ab["fy"])
            L["dash"] = OFFSET5.index(off) if off in OFFSET5 else -1
        elif ab and ab.get("at") in (2, 3, 4, 5):   # unit / ground targeted spells
            L["ability_target"] = _target_class(rec, x, y, ab["tx"], ab["ty"])
        wd = next((a for a in steps if a["k"] == K_WARD), None)
        L["ward"] = int(wd is not None)
        if wd:
            wx, wy = _pos_before(rec, wd, x, y)
            off = (wd["tx"] - wx, wd["ty"] - wy)
            L["ward_offset"] = OFFSET5.index(off) if off in OFFSET5 else -1
        L["recall"] = int(K_RECALL in kinds)
        if K_RECALL in kinds:
            L["recall_commit"] = recall_arrives(ms, rec, future)
        gems = o.get("gems") or 0
        if gems > 0 and any(cheb(x, y, ax, ay) <= 1 for ax, ay in ms.altar_tiles):
            L["submit"] = int(K_SUBMIT in kinds)
        cons = o.get("cons") or [0] * 4
        if cons[2] > 0:
            L["lock"] = int(K_LOCK in kinds)
        if cons[0] > 0:
            tp = next((a for a in steps if a["k"] in (K_CONSUMABLE, K_TP_CHANNEL)), None)
            L["teleport"] = int(tp is not None)
            if tp:
                tx0, ty0 = _pos_before(rec, tp, x, y)
                off = (tp["tx"] - tx0, tp["ty"] - ty0)
                L["tp_offset"] = OFFSET7.index(off) if off in OFFSET7 else -1
    return L


def recall_arrives(ms, rec, future):
    """1 if a recall started this turn really takes the champion home: within the following turns it
    appears on its own spawn after a jump, without cancelling or dying first. (Their bots start a
    recall at the end of most turns and usually cancel it next turn; damage also interrupts it.)"""
    s = rec["obs"]["self"]
    px, py = s.get("x"), s.get("y")
    for r in future:
        o = r["obs"]["self"]
        if not (o.get("alive", 1) and o.get("vis", 0)):
            return 0
        x, y = o["x"], o["y"]
        if ms.own_spawn[y, x] and cheb(px, py, x, y) > 2:
            return 1
        if any(st["act"]["k"] == K_CANCEL for st in r["steps"]):
            return 0
        px, py = x, y
    return 0


def _pos_before(rec, act, x, y):
    for st in rec["steps"]:
        if st["act"] is act:
            e = st.get("econ")
            return (e["x"], e["y"]) if e and e.get("x") is not None else (x, y)
    return x, y


def shop_rows(rec):
    """(econ_state, label) pairs for every shop decision in the turn: one per BUY step and a final
    'stop' decision when the champion could shop (on its spawn or dead)."""
    o = rec["obs"]
    rows = []
    state = {"gold": o.get("gold"), "gems": o.get("gems"), "items": o.get("items"), "cons": o.get("cons"),
             "cd": o.get("cd"), "x": o.get("x"), "y": o.get("y")}
    shopped = False
    for st in rec["steps"]:
        a = st["act"]
        if a["k"] != K_BUY or "after" not in a:
            continue
        lab = shop_item_label(state, a["after"])
        if lab is not None:
            rows.append((dict(state), lab))
        state = dict(state, gold=a["after"]["gold"], items=a["after"]["items"], cons=a["after"]["cons"])
        shopped = True
    return rows, state, shopped


# ----------------------------------------------------------------------------------------------
# One game -> arrays
# ----------------------------------------------------------------------------------------------
HEADS = ["move", "attack", "ability", "ability_target", "dash", "ward", "ward_offset", "recall", "recall_commit",
         "cancel", "submit", "lock", "teleport", "tp_offset"]
N_CLASSES = {"move": 9, "attack": len(TARGET_CLASSES), "ability": 3, "ability_target": len(TARGET_CLASSES),
             "dash": 25, "ward": 2, "ward_offset": 25, "recall": 2, "recall_commit": 2, "cancel": 2, "submit": 2,
             "lock": 2, "teleport": 2, "tp_offset": 49, "shop": SHOP_NONE + 1}


def game_arrays(header, records):
    """Returns dict of numpy arrays: X [n, NF], hero [n], round [n], labels per head, and the shop
    dataset (X_shop, y_shop, hero_shop)."""
    ms = MapStatic(header)
    types = {c["id"]: c["type"] for c in header["champions"]}
    by_champ = {}
    for i, r in enumerate(records):
        by_champ.setdefault(r["c"], []).append(i)
    future = [[] for _ in records]          # the champion's next 7 turns (recall outcome lookahead)
    for idx in by_champ.values():
        for k, a in enumerate(idx):
            future[a] = [records[b] for b in idx[k + 1:k + 8]]
    mems = {c: Memory(ms) for c in by_champ}
    X = np.empty((len(records), NF), dtype=np.float32)
    labels = {h: np.full(len(records), -1, dtype=np.int16) for h in HEADS}
    hero = np.empty(len(records), dtype=np.int8)
    rnd = np.empty(len(records), dtype=np.int16)
    Xs, ys, hs = [], [], []
    for i, r in enumerate(records):
        mem = mems[r["c"]]
        X[i] = turn_features(ms, mem, r)
        hero[i] = types[r["c"]]
        rnd[i] = r["r"]
        for h, v in turn_labels(ms, r, future[i]).items():
            labels[h][i] = v
        # Shop decisions: re-use the turn features with the economy at each decision point.
        s = r["obs"]["self"]
        alive = bool(s.get("alive", 1)) and s.get("vis", 0)
        can_shop = (not alive) or (alive and ms.own_spawn[s["y"], s["x"]])
        if can_shop:
            rows, final_state, _ = shop_rows(r)
            for state, lab in rows:
                Xs.append(_with_econ(X[i], state)); ys.append(lab); hs.append(hero[i])
            Xs.append(_with_econ(X[i], final_state)); ys.append(SHOP_NONE); hs.append(hero[i])
    out = {"X": X, "hero": hero, "round": rnd, **{f"y_{h}": v for h, v in labels.items()}}
    out["X_shop"] = np.array(Xs, dtype=np.float32).reshape(-1, NF)
    out["y_shop"] = np.array(ys, dtype=np.int16)
    out["hero_shop"] = np.array(hs, dtype=np.int8)
    return out


def _with_econ(xrow, state):
    """Copy of a turn's feature row with the economy fields replaced (for shop decisions)."""
    row = xrow.copy()
    row[INDEX["gold"]] = state.get("gold") or 0
    shards, emblems, fam, value = _item_aggregates(state.get("items") or {})
    for i, k in enumerate(("str", "foc", "hp")):
        row[INDEX[f"shards_{k}"]] = shards[i]
        row[INDEX[f"emblems_{k}"]] = emblems[i]
    for i in range(ITEM_FAMILIES):
        row[INDEX[f"fam_{i}"]] = fam[i]
    row[INDEX["item_value"]] = value
    cons = state.get("cons") or [0, 0, 0, 0]
    row[INDEX["tp_crystals"]] = cons[0]
    row[INDEX["altar_locks"]] = cons[2]
    return row
