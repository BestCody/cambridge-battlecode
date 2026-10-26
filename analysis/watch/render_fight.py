"""Draw a fight from watch_fight.mjs output as a filmstrip PNG (one frame per
round) and print a step-by-step log. Frame: walls dark, bushes green, altars
gold; camps brown with a letter (W wolf, E elk, G gem, O gold, M monolith, L golem); our champions blue, theirs red, letter = type, bar = health
(the part lost this round in pink), yellow border = stunned, hollow = an enemy
none of our champions sees; grey line = the move this round; lines = attacks
and damaging abilities this round (ours cyan, theirs orange; on camps brown;
awake camps' attacks purple);
the focus champion has a black ring; X = died this round.
usage: python -I -X utf8 render_fight.py <watch.json> <our team> <focus id> <first round> <last round> <out.png> [columns]"""
import json, struct, sys, zlib

TYPES = ["Ryth", "Vesper", "Vendetta", "Cassi", "Issac", "Stellar", "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi", "HoR"]
LETTER = "RVDCISPMWNGTH"
CAMP = ["Wolf", "Elk", "Gem cluster", "Gold cluster", "Monolith", "Golem"]
CAMP_LETTER = "WEGOML"
KIND = {4: "move", 5: "attack", 6: "ability", 11: "recall", 12: "submit gems", 13: "shop", 14: "ward", 15: "consumable",
        16: "cancel channel", 21: "teleport channel", 22: "teleport", 23: "blocked"}
ABILITY = ["none", "attack reset", "damage", "aoe damage", "stun", "heal", "dash", "shield"]
FONT = {  # 3x5 glyphs, rows top to bottom, 3 bits each
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111", "4": "101101111001001",
    "5": "111100111001111", "6": "111100111101111", "7": "111001001001001", "8": "111101111101111", "9": "111101111001111",
    "A": "010101111101101", "B": "110101110101110", "C": "011100100100011", "D": "110101101101110", "E": "111100110100111",
    "F": "111100110100100", "G": "011100101101011", "H": "101101111101101", "I": "111010010010111", "J": "001001001101010",
    "K": "101101110101101", "L": "100100100100111", "M": "101111111101101", "N": "110101101101101", "O": "010101101101010",
    "P": "110101110100100", "Q": "010101101110011", "R": "110101110101101", "S": "011100010001110", "T": "111010010010010",
    "U": "101101101101111", "V": "101101101101010", "W": "101101111111101", "X": "101101010101101", "Y": "101101010010010",
    "Z": "111001010100111", "-": "000000111000000", " ": "000000000000000", "/": "001001010100100", ":": "000010000010000",
}
FLOOR, WALL, BUSH, ALTAR = (236, 233, 222), (62, 62, 68), (122, 172, 104), (214, 178, 64)
OURS, THEIRS = (40, 92, 220), (212, 52, 40)
OURS_HIT, THEIRS_HIT = (0, 190, 230), (255, 140, 0)


def png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    path, ours, focus, ra, rb, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
    cols = int(sys.argv[7]) if len(sys.argv) > 7 else 5
    d = json.load(open(path, encoding="utf-8"))
    grid, steps, rounds = d["map"], d["steps"], {int(k): v for k, v in d["rounds"].items()}
    info = {c["id"]: c for v in rounds.values() for c in v["champions"]}
    by_round = {}
    for s in steps:
        by_round.setdefault(s["r"], []).append(s)
    frames = [r for r in range(ra, rb + 1) if r in by_round]
    name = lambda cid: ("o" if info[cid]["team"] == ours else "t") + TYPES[info[cid]["type"]] + str(cid)
    # Area: everyone within 10 of the focus at some point, plus a margin.
    pts = []
    for r in frames:
        for s in by_round[r][-1:]:
            st = {c[0]: c for c in s["c"]}
            f = st.get(focus)
            for c in s["c"]:
                if c[1] >= 0 and f and f[1] >= 0 and max(abs(c[1] - f[1]), abs(c[2] - f[2])) <= 10:
                    pts.append((c[1], c[2]))
    x0, x1 = max(0, min(p[0] for p in pts) - 2), min(d["width"] - 1, max(p[0] for p in pts) + 2)
    y0, y1 = max(0, min(p[1] for p in pts) - 2), min(d["height"] - 1, max(p[1] for p in pts) + 2)
    if x1 - x0 > 29:
        cx = sum(p[0] for p in pts) // len(pts); x0, x1 = max(0, cx - 14), min(d["width"] - 1, cx + 15)
    if y1 - y0 > 29:
        cy = sum(p[1] for p in pts) // len(pts); y0, y1 = max(0, cy - 14), min(d["height"] - 1, cy + 15)
    T = 16
    fw, fh = (x1 - x0 + 1) * T, (y1 - y0 + 1) * T + 14
    rows_n = (len(frames) + cols - 1) // cols
    W, H = cols * (fw + 6) + 6, rows_n * (fh + 6) + 6
    px = bytearray([255]) * (W * H * 3)

    def put(x, y, col):
        if 0 <= x < W and 0 <= y < H:
            i = (y * W + x) * 3
            px[i:i + 3] = bytes(col)

    def rect(x, y, w, h, col):
        for yy in range(max(0, y), min(H, y + h)):
            i = (yy * W + max(0, x)) * 3
            n = max(0, min(W, x + w) - max(0, x))
            px[i:i + n * 3] = bytes(col) * n

    def frame_rect(x, y, w, h, col, t=1):
        rect(x, y, w, t, col); rect(x, y + h - t, w, t, col); rect(x, y, t, h, col); rect(x + w - t, y, t, h, col)

    def text(x, y, s, col, k=1):
        for ch in s.upper():
            g = FONT.get(ch, FONT[" "])
            for i, b in enumerate(g):
                if b == "1":
                    rect(x + (i % 3) * k, y + (i // 3) * k, k, k, col)
            x += 4 * k

    def line(xa, ya, xb, yb, col, t=1):
        dx, dy = abs(xb - xa), -abs(yb - ya)
        sx, sy = (1 if xa < xb else -1), (1 if ya < yb else -1)
        e = dx + dy
        while True:
            rect(xa - t // 2, ya - t // 2, t, t, col)
            if xa == xb and ya == yb:
                break
            e2 = 2 * e
            if e2 >= dy:
                e += dy; xa += sx
            if e2 <= dx:
                e += dx; ya += sy

    log = []
    taken = {(t, src): 0 for t in ("ours", "theirs") for src in ("champions", "camps")}
    deaths = {"ours": [], "theirs": []}
    stunned = {"ours": 0, "theirs": 0}
    for fi, r in enumerate(frames):
        ox, oy = 6 + (fi % cols) * (fw + 6), 6 + (fi // cols) * (fh + 6)
        text(ox + 2, oy + 2, f"R{r}", (0, 0, 0), 2)
        my = oy + 14
        tile = lambda tx, ty: (ox + (tx - x0) * T, my + (ty - y0) * T)
        for ty in range(y0, y1 + 1):
            for tx in range(x0, x1 + 1):
                ch = grid[ty][tx]
                rect(*tile(tx, ty), T, T, WALL if ch == "#" else BUSH if ch == '"' else ALTAR if ch == "A" else FLOOR)
        for tx in range(x0, x1 + 1):  # faint grid every 5 tiles
            if tx % 5 == 0:
                rect(tile(tx, y0)[0], my, 1, (y1 - y0 + 1) * T, (205, 200, 190))
        for ty in range(y0, y1 + 1):
            if ty % 5 == 0:
                rect(ox, tile(x0, ty)[1], (x1 - x0 + 1) * T, 1, (205, 200, 190))
        ss = by_round[r]
        prev_round = by_round.get(r - 1)
        start = {c[0]: c for c in (prev_round[-1]["c"] if prev_round else ss[0]["c"])}
        end = {c[0]: c for c in ss[-1]["c"]}
        inside = lambda x, y: x0 <= x <= x1 and y0 <= y <= y1
        centre = lambda x, y: (tile(x, y)[0] + T // 2, tile(x, y)[1] + T // 2)
        camp_max = {}
        for k in rounds.get(r, {}).get("camps", []):
            camp_max[k[0]] = max(camp_max.get(k[0], 0), k[4])
            if inside(k[2], k[3]):
                cx, cy = tile(k[2], k[3])
                rect(cx + 2, cy + 2, T - 4, T - 4, (130, 88, 52))
                text(cx + 6, cy + 5, CAMP_LETTER[k[1]] if 0 <= k[1] < len(CAMP_LETTER) else "?", (240, 220, 190), 1)
        camp_at = {(k[2], k[3]) for k in rounds.get(r, {}).get("camps", [])}
        camp_by_id = {k[0]: k for k in rounds.get(r, {}).get("camps", [])}
        # Moves.
        for cid, c in end.items():
            a = start.get(cid)
            if a and a[4] and c[4] and (a[1], a[2]) != (c[1], c[2]) and inside(a[1], a[2]) and inside(c[1], c[2]):
                line(*centre(a[1], a[2]), *centre(c[1], c[2]), (150, 150, 150), 2)
        # Attacks and damage, step by step.
        before = start
        rlog = []
        for s in ss:
            now = {c[0]: c for c in s["c"]}
            hits = []
            for cid, c in now.items():
                b = before.get(cid)
                if b and b[3] != c[3] and b[4]:
                    hits.append((cid, b[3], c[3]))
            k = s["kind"]
            for cid, b, a in hits:
                if cid in info and a < b:
                    side = "ours" if info[cid]["team"] == ours else "theirs"
                    taken[(side, "camps" if s["ak"] == 2 else "champions")] += b - a
                    if a <= 0:
                        deaths[side].append(f"R{r} {name(cid)} by {'a camp' if s['ak'] == 2 else name(s['a']) if s['a'] in info else '?'}")
            if s["ak"] == 1 and s["a"] in info:
                actor = now.get(s["a"])
                if k in (5, 6) and actor and inside(actor[1], actor[2]) and inside(s["tx"], s["ty"]) and (k == 5 or s["at"] in (2, 3, 4)):
                    on_camp = (s["tx"], s["ty"]) in camp_at and not any((c[1], c[2]) == (s["tx"], s["ty"]) for c in now.values())
                    col = (150, 105, 60) if on_camp else OURS_HIT if info[s["a"]]["team"] == ours else THEIRS_HIT
                    line(*centre(actor[1], actor[2]), *centre(s["tx"], s["ty"]), col, 1 if on_camp else 2)
                if k in KIND and k != 4:
                    what = KIND[k]
                    if k == 6:
                        what += f" {s['slot']} ({ABILITY[s['at']] if 0 <= s['at'] < len(ABILITY) else s['at']})"
                    target = next((name(c[0]) for c in now.values() if (c[1], c[2]) == (s["tx"], s["ty"]) and c[0] != s["a"]), None)
                    if k in (5, 6) and not target:
                        target = "camp" if (s["tx"], s["ty"]) in camp_at else f"({s['tx']},{s['ty']})"
                    dmg = ", ".join(f"{name(cid)} {b}->{a}" for cid, b, a in hits if cid in info)
                    if k == 5 and target == "camp" and not dmg:
                        continue
                    rlog.append(f"{name(s['a'])} {what}" + (f" -> {target}" if target else "") + (f": {dmg}" if dmg else ""))
            elif s["ak"] == 2 and k == 5 and s["a"] in camp_by_id:
                cp = camp_by_id[s["a"]]
                if inside(cp[2], cp[3]) and inside(s["tx"], s["ty"]):
                    line(*centre(cp[2], cp[3]), *centre(s["tx"], s["ty"]), (160, 40, 200), 2)
                target = next((name(c[0]) for c in now.values() if (c[1], c[2]) == (s["tx"], s["ty"])), f"({s['tx']},{s['ty']})")
                dmg = ", ".join(f"{name(cid)} {b}->{a}" for cid, b, a in hits if cid in info)
                rlog.append(f"camp {CAMP[cp[1]] if 0 <= cp[1] < len(CAMP) else cp[1]}@{cp[2]},{cp[3]} attack -> {target}" + (f": {dmg}" if dmg else ""))
            elif hits:
                rlog.append(f"(phase {k}) " + ", ".join(f"{name(cid)} {b}->{a}" for cid, b, a in hits if cid in info))
            before = now
        seen_by_ours = set()
        for cid, vis in rounds.get(r, {}).get("seen", {}).items():
            if info[int(cid)]["team"] == ours:
                seen_by_ours.update(vis)
        for cid, c in end.items():
            a = start.get(cid)
            if not inside(c[1], c[2]) or c[1] < 0:
                continue
            tx, ty = tile(c[1], c[2])
            col = OURS if info[cid]["team"] == ours else THEIRS
            if not c[4]:
                if a and a[4]:
                    line(tx + 2, ty + 2, tx + T - 3, ty + T - 3, col, 2); line(tx + T - 3, ty + 2, tx + 2, ty + T - 3, col, 2)
                continue
            hidden = info[cid]["team"] != ours and cid not in seen_by_ours
            if hidden:
                frame_rect(tx + 1, ty + 3, T - 2, T - 4, col, 2)
            else:
                rect(tx + 1, ty + 3, T - 2, T - 4, col)
            if c[5] >= 0:
                frame_rect(tx, ty + 2, T, T - 2, (250, 220, 0), 2)
            if cid == focus:
                frame_rect(tx - 1, ty + 1, T + 2, T, (0, 0, 0), 1)
            text(tx + 5, ty + 5, LETTER[info[cid]["type"]], (255, 255, 255) if not hidden else col, 1)
            mx = max(1, info[cid]["maxHp"])
            w_now = round((T - 2) * max(0, c[3]) / mx)
            w_then = round((T - 2) * max(0, a[3] if a and a[4] else c[3]) / mx)
            rect(tx + 1, ty, T - 2, 2, (60, 60, 60))
            rect(tx + 1, ty, max(w_then, w_now), 2, (255, 150, 170))
            rect(tx + 1, ty, w_now, 2, (40, 200, 60))
        for cid, c in end.items():
            if c[4] and c[5] >= 0 and inside(c[1], c[2]):
                stunned["ours" if info[cid]["team"] == ours else "theirs"] += 1
        # Round summary: everyone in the area.
        alive = [f"{name(cid)} {c[3]}@{c[1]},{c[2]}" + (" STUN" if c[5] >= 0 else "") + (" (unseen)" if info[cid]['team'] != ours and cid not in seen_by_ours else "")
                 for cid, c in sorted(end.items()) if c[4] and inside(c[1], c[2])]
        log.append(f"R{r}: " + "; ".join(alive))
        log.extend("    " + e for e in rlog)
    png(out, W, H, px)
    print(f"area x {x0}-{x1}, y {y0}-{y1}; frames {frames[0]}-{frames[-1]}; o = ours, t = theirs")
    print("\n".join(log))
    for side in ("ours", "theirs"):
        print(f"{side}: damage taken from champions {taken[(side, 'champions')]}, from camps {taken[(side, 'camps')]}; "
              f"champion-rounds ending stunned {stunned[side]}; deaths: {'; '.join(deaths[side]) or '-'}")


main()
