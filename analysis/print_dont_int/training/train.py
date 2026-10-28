"""Train one XGBoost model per (hero, head) to imitate print dont int's champions turn by turn.

    python training/train.py --hero vesper --head move
    python training/train.py --hero all --head all            # every hero x every head, in sequence
    python training/train.py --hero hor --head attack --resume  # carry on from checkpoints/.../current

Data: training/data/features/*.npz (build_dataset.py). Rows are champion turns; each head only uses
the turns where it applies (label >= 0). Split is by game (a game's turns never straddle splits):
--test-frac of games are held out for the final test, --val-frac for validation/early stopping.

Checkpoints (default analysis/print_dont_int/checkpoints/<hero>/<head>/):
  current/  model.json, state.json (history, class map, args), curves.png, confusion.png, importance.png
  best/     the same for the best validation log-loss so far (+ test_metrics.json, test_confusion.png)
Written every --ckpt-every boosting rounds. A summary of every finished run is kept in
checkpoints/summary.csv.
"""
import argparse
import csv
import hashlib
import json
import os
import sys
import time
import traceback
from pathlib import Path

import numpy as np
from tqdm import tqdm

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F  # noqa: E402

TRAINING = Path(__file__).resolve().parent
FEATURES = TRAINING / "data" / "features"
CACHE = TRAINING / "data" / "cache"
DEFAULT_CKPT = TRAINING.parent / "checkpoints"
HEROES = {"vesper": 1, "vendetta": 2, "marcella": 7, "hor": 12, "ryth": 0}
ALL_HEADS = F.HEADS + ["shop"]
# Heads that are not worth a model on their own: labels that are rare or purely analytical.
DEFAULT_HEADS = ["move", "attack", "ability", "ability_target", "dash", "ward", "ward_offset", "recall", "cancel",
                 "submit", "lock", "teleport", "tp_offset", "shop", "recall_commit"]


# ----------------------------------------------------------------------------------------------
# Data
# ----------------------------------------------------------------------------------------------
def split_of(match_id, val_frac, test_frac):
    """Deterministic game-level split from a hash of the match id."""
    u = int(hashlib.sha1(match_id.encode()).hexdigest()[:8], 16) / 0xFFFFFFFF
    return "test" if u < test_frac else "val" if u < test_frac + val_frac else "train"


def load_hero(hero_type):
    """All turns (and shop decisions) of one hero, concatenated across games, cached as .npy."""
    files = sorted(FEATURES.glob("*.npz"))
    if not files:
        sys.exit("no feature files: run training/extract/run_extract.py then training/build_dataset.py")
    key = hashlib.sha1(("|".join(f.name for f in files) + f"|v{F.FEATURE_VERSION}|h{hero_type}").encode()).hexdigest()[:12]
    cdir = CACHE / f"hero{hero_type}_{key}"
    if (cdir / "done").exists():
        return {p.stem: np.load(p, mmap_mode="r") for p in cdir.glob("*.npy")}, json.loads((cdir / "games.json").read_text())
    parts, games = {}, []
    for gi, f in enumerate(tqdm(files, desc=f"loading hero {hero_type}", unit=" games", leave=False)):
        with np.load(f) as z:
            if int(z["version"]) != F.FEATURE_VERSION:
                sys.exit(f"{f.name} was built with an older feature version: rerun build_dataset.py")
            m, ms = z["hero"] == hero_type, z["hero_shop"] == hero_type
            if not m.any():
                continue
            games.append(f.stem)
            gid = len(games) - 1
            parts.setdefault("X", []).append(z["X"][m])
            parts.setdefault("round", []).append(z["round"][m])
            parts.setdefault("game", []).append(np.full(m.sum(), gid, dtype=np.int32))
            for h in F.HEADS:
                parts.setdefault(f"y_{h}", []).append(z[f"y_{h}"][m])
            parts.setdefault("X_shop", []).append(z["X_shop"][ms])
            parts.setdefault("y_shop", []).append(z["y_shop"][ms])
            parts.setdefault("game_shop", []).append(np.full(ms.sum(), gid, dtype=np.int32))
    if not games:
        sys.exit(f"no turns for hero type {hero_type}")
    cdir.mkdir(parents=True, exist_ok=True)
    for k, v in parts.items():
        np.save(cdir / f"{k}.npy", np.concatenate(v))
    (cdir / "games.json").write_text(json.dumps(games))
    (cdir / "done").write_text("ok")
    return {p.stem: np.load(p, mmap_mode="r") for p in cdir.glob("*.npy")}, games


def head_rows(data, games, head, args):
    if head == "shop":
        X, y, g = data["X_shop"], data["y_shop"], data["game_shop"]
    else:
        X, y, g = data["X"], data[f"y_{head}"], data["game"]
    keep = np.nonzero(np.asarray(y) >= 0)[0]
    split = np.array([split_of(games[i], args.val_frac, args.test_frac) for i in range(len(games))])
    parts = {}
    rng = np.random.default_rng(args.seed)
    for name in ("train", "val", "test"):
        idx = keep[split[np.asarray(g)[keep]] == name]
        cap = args.max_rows if name == "train" else args.max_rows // 4
        if cap and len(idx) > cap:
            idx = np.sort(rng.choice(idx, cap, replace=False))
        parts[name] = (np.asarray(X[idx], dtype=np.float32), np.asarray(y[idx], dtype=np.int64))
    return parts


# ----------------------------------------------------------------------------------------------
# Metrics and plots
# ----------------------------------------------------------------------------------------------
def metrics(y_true, prob, n_classes):
    pred = prob.argmax(1)
    acc = float((pred == y_true).mean()) if len(y_true) else float("nan")
    recalls, f1s = [], []
    for c in range(n_classes):
        t, p = y_true == c, pred == c
        if t.sum() == 0:
            continue
        tp = float((t & p).sum())
        rec = tp / t.sum()
        prec = tp / p.sum() if p.sum() else 0.0
        recalls.append(rec)
        f1s.append(2 * prec * rec / (prec + rec) if prec + rec else 0.0)
    eps = 1e-12
    ll = float(-np.log(np.clip(prob[np.arange(len(y_true)), y_true], eps, 1)).mean()) if len(y_true) else float("nan")
    majority = float(np.bincount(y_true, minlength=n_classes).max() / len(y_true)) if len(y_true) else float("nan")
    return {"logloss": ll, "accuracy": acc, "balanced_accuracy": float(np.mean(recalls)) if recalls else float("nan"),
            "macro_f1": float(np.mean(f1s)) if f1s else float("nan"), "majority_baseline": majority, "n": int(len(y_true))}


def confusion(y_true, pred, n):
    m = np.zeros((n, n), dtype=np.int64)
    np.add.at(m, (y_true, pred), 1)
    return m


def savefig(fig, path):
    """Render to a temp file, then swap it in (retrying while Windows holds the old file open)."""
    tmp = path.with_name(path.stem + ".tmp.png")
    fig.savefig(tmp, dpi=110)
    replace(tmp, path)


def plot_confusion(path, conf, class_names, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    n = conf.shape[0]
    norm = conf / np.maximum(conf.sum(1, keepdims=True), 1)
    fig, ax = plt.subplots(figsize=(max(5, .45 * n + 2), max(4, .45 * n + 1.5)))
    im = ax.imshow(norm, cmap="Blues", vmin=0, vmax=1)
    ax.set_xticks(range(n)); ax.set_yticks(range(n))
    ax.set_xticklabels(class_names, rotation=60, ha="right", fontsize=7); ax.set_yticklabels(class_names, fontsize=7)
    if n <= 16:
        for i in range(n):
            for j in range(n):
                if conf[i, j]:
                    ax.text(j, i, f"{norm[i, j]:.2f}\n{conf[i, j]}", ha="center", va="center", fontsize=6,
                            color="white" if norm[i, j] > .5 else "black")
    ax.set_xlabel("predicted"); ax.set_ylabel("actual (row-normalised)"); ax.set_title(title)
    fig.colorbar(im, ax=ax, fraction=.046); fig.tight_layout(); savefig(fig, path)
    plt.close(fig)


def save_plots(folder, history, conf, class_names, importance, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    folder.mkdir(parents=True, exist_ok=True)
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.2))
    r = np.arange(1, len(history["train_loss"]) + 1)
    axes[0].plot(r, history["train_loss"], label="train", color="#4C78A8")
    axes[0].plot(r, history["val_loss"], label="validation", color="#F58518")
    if history.get("best_round"):
        axes[0].axvline(history["best_round"], color="#888", ls="--", lw=1, label=f"best ({history['best_round']})")
    axes[0].set_xlabel("boosting round"); axes[0].set_ylabel("log-loss"); axes[0].set_title("loss"); axes[0].legend()
    ck = history["checkpoints"]
    if ck:
        x = [c["round"] for c in ck]
        axes[1].plot(x, [c["val"]["accuracy"] for c in ck], "o-", label="val accuracy", color="#F58518")
        axes[1].plot(x, [c["val"]["balanced_accuracy"] for c in ck], "s-", label="val balanced acc", color="#54A24B")
        axes[1].plot(x, [c["train"]["accuracy"] for c in ck], "o--", label="train accuracy", color="#4C78A8", alpha=.6)
        axes[1].axhline(ck[-1]["val"]["majority_baseline"], color="#888", ls=":", label="majority baseline")
        axes[1].set_ylim(0, 1.02)
    axes[1].set_xlabel("boosting round"); axes[1].set_title("accuracy (at checkpoints)"); axes[1].legend(fontsize=8)
    fig.suptitle(title); fig.tight_layout(); savefig(fig, folder / "curves.png"); plt.close(fig)

    if conf is not None:
        plot_confusion(folder / "confusion.png", conf, class_names, title + " - validation")

    if importance:
        top = sorted(importance.items(), key=lambda kv: kv[1], reverse=True)[:25][::-1]
        fig, ax = plt.subplots(figsize=(7, 7))
        ax.barh([k for k, _ in top], [v for _, v in top], color="#4C78A8")
        ax.set_xlabel("total gain"); ax.set_title(title + " - top features")
        ax.tick_params(axis="y", labelsize=8)
        fig.tight_layout(); savefig(fig, folder / "importance.png"); plt.close(fig)


# ----------------------------------------------------------------------------------------------
# Training
# ----------------------------------------------------------------------------------------------
def class_names_for(head, classes):
    if head in ("attack", "ability_target"):
        return [F.TARGET_CLASSES[c] for c in classes]
    if head == "move":
        return [(F.DIR_NAMES + ["stay"])[c] for c in classes]
    if head == "ability":
        return [["none", "slot0", "slot1"][c] for c in classes]
    if head in ("dash", "ward_offset"):
        return [f"{F.OFFSET5[c][0]:+d},{F.OFFSET5[c][1]:+d}" for c in classes]
    if head == "tp_offset":
        return [f"{F.OFFSET7[c][0]:+d},{F.OFFSET7[c][1]:+d}" for c in classes]
    if head == "shop":
        fams = ["monster", "power", "durability", "treasure", "jungle", "home", "bruiser", "tribute", "capture",
                "prospector", "hunter", "skirmish", "greed", "collectors"]
        cons = ["tp_crystal", "control_ward", "altar_lock", "tp_scroll"]
        out = []
        for c in classes:
            if c == F.SHOP_NONE:
                out.append("stop")
            elif c >= 84:
                out.append(cons[c - 84])
            else:
                fam, rest = divmod(c, 6)
                out.append(f"{fams[fam]}_{['str', 'foc', 'hp'][rest // 2]}")
        return out
    return ["no", "yes"] if len(classes) == 2 else [str(c) for c in classes]


def train_one(hero, head, data, games, args):
    import xgboost as xgb

    title = f"{hero} / {head}"
    out = Path(args.ckpt_dir) / hero / head
    cur_dir, best_dir = out / "current", out / "best"
    parts = head_rows(data, games, head, args)
    Xtr, ytr = parts["train"]
    Xva, yva = parts["val"]
    Xte, yte = parts["test"]
    if len(ytr) < args.min_rows or len(np.unique(ytr)) < 2 or len(yva) == 0:
        tqdm.write(f"skip {title}: {len(ytr)} train rows, {len(np.unique(ytr))} classes, {len(yva)} val rows")
        return None

    # Compact class ids: only classes seen in training (rarer ones fold into nothing and are dropped).
    classes = sorted(int(c) for c in np.unique(ytr))
    remap = {c: i for i, c in enumerate(classes)}
    keep_va, keep_te = np.isin(yva, classes), np.isin(yte, classes)
    ytr_c = np.array([remap[c] for c in ytr])
    yva_c = np.array([remap[c] for c in yva[keep_va]])
    yte_c = np.array([remap[c] for c in yte[keep_te]])
    Xva, Xte = Xva[keep_va], Xte[keep_te]
    K = len(classes)
    names = class_names_for(head, classes)

    weights = None
    if args.balance != "none":
        freq = np.bincount(ytr_c, minlength=K).astype(float)
        w = (freq.sum() / np.maximum(freq, 1)) ** (0.5 if args.balance == "sqrt" else 1.0)
        weights = (w / w[ytr_c].mean())[ytr_c]
    dtr = xgb.DMatrix(Xtr, label=ytr_c, weight=weights, feature_names=F.NAMES, missing=np.nan)
    dva = xgb.DMatrix(Xva, label=yva_c, feature_names=F.NAMES, missing=np.nan)
    params = {"tree_method": "hist", "max_depth": args.max_depth, "eta": args.lr, "subsample": args.subsample,
              "colsample_bytree": args.colsample, "min_child_weight": args.min_child_weight, "lambda": args.l2,
              "max_bin": 256, "nthread": args.threads, "seed": args.seed}
    if K == 2:
        params.update(objective="binary:logistic", eval_metric="logloss")
    else:
        params.update(objective="multi:softprob", num_class=K, eval_metric="mlogloss")

    def predict(booster, dm, n_rounds=None):
        p = booster.predict(dm, iteration_range=(0, n_rounds) if n_rounds else (0, 0))
        return np.stack([1 - p, p], 1) if K == 2 else p

    state = {"hero": hero, "head": head, "classes": classes, "class_names": names, "params": params,
             "args": vars(args), "feature_version": F.FEATURE_VERSION, "n_train": int(len(ytr_c)),
             "n_val": int(len(yva_c)), "n_test": int(len(yte_c)),
             "history": {"train_loss": [], "val_loss": [], "checkpoints": [], "best_round": 0, "best_val_loss": None}}
    start_model = None
    if args.resume and (cur_dir / "model.json").exists():
        old = json.loads((cur_dir / "state.json").read_text())
        if old["classes"] == classes and old["feature_version"] == F.FEATURE_VERSION:
            state["history"] = old["history"]
            # Older checkpoints have no flag; a test report is only written once training ended.
            state["finished"] = old.get("finished", (best_dir / "test_metrics.json").exists())
            start_model = str(cur_dir / "model.json")
        else:
            tqdm.write(f"  {title}: checkpoint does not match the data (classes/features changed); starting fresh")
    done = len(state["history"]["train_loss"])
    remaining = 0 if state.get("finished") else max(0, args.rounds - done)
    if remaining == 0:
        tqdm.write(f"  {title}: already trained ({done} rounds), re-checking the test set only")

    hist = state["history"]
    metric = "logloss" if K == 2 else "mlogloss"
    bar = tqdm(total=args.rounds, initial=done, desc=f"{title:<22}", unit="rnd", dynamic_ncols=True)

    def checkpoint(booster, final=False):
        n = len(hist["train_loss"])
        sub = np.random.default_rng(0).choice(len(ytr_c), min(len(ytr_c), 50000), replace=False)
        dsub = xgb.DMatrix(Xtr[sub], feature_names=F.NAMES, missing=np.nan)
        m_tr = metrics(ytr_c[sub], predict(booster, dsub), K)
        pva = predict(booster, dva)
        m_va = metrics(yva_c, pva, K)
        hist["checkpoints"].append({"round": n, "train": m_tr, "val": m_va, "time": time.time()})
        conf = confusion(yva_c, pva.argmax(1), K)
        imp = booster.get_score(importance_type="total_gain")
        save_model(booster, cur_dir, state, conf, names, imp, title)
        # Best = lowest validation loss over all rounds so far, sliced to that round.
        b = hist["best_round"]
        if b and (not (best_dir / "state.json").exists()
                  or json.loads((best_dir / "state.json").read_text())["history"]["best_round"] != b):
            best = booster[:b]
            pb = predict(best, dva)
            save_model(best, best_dir, state, confusion(yva_c, pb.argmax(1), K), names,
                       best.get_score(importance_type="total_gain"), title + f" (best @ {b})",
                       extra={"val": metrics(yva_c, pb, K)})
        bar.set_postfix_str(f"tr {hist['train_loss'][-1]:.4f} va {hist['val_loss'][-1]:.4f} "
                            f"acc {m_va['accuracy']:.3f} bal {m_va['balanced_accuracy']:.3f} best@{b}")

    def safe_checkpoint(booster):
        """Saving must never stop training: on any error, warn and let the next checkpoint retry."""
        try:
            checkpoint(booster)
        except Exception as e:
            tqdm.write(f"  {title}: checkpoint at round {len(hist['train_loss'])} failed ({type(e).__name__}: {e}); "
                       f"continuing, the next checkpoint will retry")

    class Progress(xgb.callback.TrainingCallback):
        def __init__(self):
            super().__init__()
            self.stale = 0

        def after_iteration(self, model, epoch, evals_log):
            hist["train_loss"].append(float(evals_log["train"][metric][-1]))
            v = float(evals_log["val"][metric][-1])
            hist["val_loss"].append(v)
            n = len(hist["val_loss"])
            if hist["best_val_loss"] is None or v < hist["best_val_loss"] - 1e-6:
                hist["best_val_loss"], hist["best_round"] = v, n
                self.stale = 0
            else:
                self.stale += 1
            bar.update(1)
            if n % 10 == 0:
                bar.set_postfix_str(f"tr {hist['train_loss'][-1]:.4f} va {v:.4f} best@{hist['best_round']}")
            if n % args.ckpt_every == 0:
                safe_checkpoint(model)
            return self.stale >= args.patience   # early stop

    booster = None
    if remaining:
        booster = xgb.train(params, dtr, num_boost_round=remaining, evals=[(dtr, "train"), (dva, "val")],
                            callbacks=[Progress()], xgb_model=start_model, verbose_eval=False)
        if len(hist["train_loss"]) % args.ckpt_every:
            safe_checkpoint(booster)
        state["finished"] = True   # rounds exhausted or early-stopped: --resume skips it from now on
        try:
            write_json(cur_dir / "state.json", state)
        except OSError as e:
            tqdm.write(f"  {title}: could not mark the run finished ({e}); --resume may retrain it")
    bar.close()

    # Final test evaluation with the best model.
    best = xgb.Booster(model_file=str(best_dir / "model.json"))
    dte = xgb.DMatrix(Xte, label=yte_c, feature_names=F.NAMES, missing=np.nan)
    pte = predict(best, dte)
    m_te = metrics(yte_c, pte, K)
    m_te["per_class_recall"] = {names[c]: float(((pte.argmax(1) == c) & (yte_c == c)).sum() / max((yte_c == c).sum(), 1))
                                for c in range(K)}
    m_te["support"] = {names[c]: int((yte_c == c).sum()) for c in range(K)}
    (best_dir / "test_metrics.json").write_text(json.dumps(m_te, indent=1))
    try:
        plot_confusion(best_dir / "test_confusion.png", confusion(yte_c, pte.argmax(1), K), names, title + " - TEST")
    except OSError as e:
        tqdm.write(f"  could not write the test confusion plot: {e}")
    tqdm.write(f"  {title}: test acc {m_te['accuracy']:.3f} (majority {m_te['majority_baseline']:.3f}), "
               f"balanced {m_te['balanced_accuracy']:.3f}, logloss {m_te['logloss']:.4f}, n={m_te['n']}")
    return {"hero": hero, "head": head, "classes": K, "n_train": len(ytr_c), "best_round": hist["best_round"],
            "val_loss": hist["best_val_loss"], **{f"test_{k}": v for k, v in m_te.items() if not isinstance(v, dict)}}


def replace(src, dst):
    """os.replace that waits out Windows briefly locking the target (antivirus, indexer, an open viewer)."""
    for attempt in range(40):
        try:
            os.replace(src, dst)
            return
        except OSError:   # PermissionError / EINVAL: the target is briefly locked
            if attempt == 39:
                raise
            time.sleep(0.25 + 0.05 * attempt)


def write_json(path, data):
    tmp = path.with_name(path.stem + ".tmp.json")
    tmp.write_text(json.dumps(data, indent=1))
    replace(tmp, path)


def save_model(booster, folder, state, conf, names, importance, title, extra=None):
    folder.mkdir(parents=True, exist_ok=True)
    tmp = folder / "model.tmp.json"
    booster.save_model(str(tmp))
    replace(tmp, folder / "model.json")
    write_json(folder / "state.json", dict(state, **(extra or {})))
    try:
        save_plots(folder, state["history"], conf, names, importance, title)
    except OSError as e:   # a plot locked by a viewer/antivirus must never kill training
        tqdm.write(f"  could not write plots in {folder}: {e}")


def update_summary(path, row):
    rows = []
    if path.exists():
        with open(path, newline="", encoding="utf-8") as f:
            rows = [r for r in csv.DictReader(f) if not (r["hero"] == row["hero"] and r["head"] == row["head"])]
    rows.append({k: (f"{v:.4f}" if isinstance(v, float) else v) for k, v in row.items()})
    keys = list(dict.fromkeys(k for r in rows for k in r))
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--hero", default="all", help=f"one of {list(HEROES)} or 'all'")
    p.add_argument("--head", default="all", help=f"one of {ALL_HEADS}, a comma list, or 'all'")
    p.add_argument("--rounds", type=int, default=600, help="max boosting rounds (per class tree group)")
    p.add_argument("--lr", type=float, default=0.08)
    p.add_argument("--max-depth", type=int, default=6, help="keep small: the C++ bot pays fuel per tree level")
    p.add_argument("--subsample", type=float, default=0.8)
    p.add_argument("--colsample", type=float, default=0.8)
    p.add_argument("--min-child-weight", type=float, default=5)
    p.add_argument("--l2", type=float, default=1.0)
    p.add_argument("--balance", choices=["none", "sqrt", "full"], default="none",
                   help="class re-weighting; 'none' imitates the true action frequencies")
    p.add_argument("--patience", type=int, default=60, help="early stop after this many rounds without val gain")
    p.add_argument("--ckpt-every", type=int, default=25, help="checkpoint + plots every N rounds")
    p.add_argument("--val-frac", type=float, default=0.1)
    p.add_argument("--test-frac", type=float, default=0.2)
    p.add_argument("--max-rows", type=int, default=600000, help="cap on training rows per head (0 = no cap)")
    p.add_argument("--min-rows", type=int, default=200)
    p.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--resume", action="store_true", help="continue from current/ checkpoints")
    p.add_argument("--ckpt-dir", default=str(DEFAULT_CKPT))
    args = p.parse_args()

    heroes = list(HEROES) if args.hero == "all" else args.hero.split(",")
    heads = DEFAULT_HEADS if args.head == "all" else args.head.split(",")
    for h in heroes:
        if h not in HEROES:
            sys.exit(f"unknown hero {h}; choose from {list(HEROES)}")
    for h in heads:
        if h not in ALL_HEADS:
            sys.exit(f"unknown head {h}; choose from {ALL_HEADS}")
    Path(args.ckpt_dir).mkdir(parents=True, exist_ok=True)
    failed = []
    for hero in heroes:
        data, games = load_hero(HEROES[hero])
        for head in heads:
            try:
                row = train_one(hero, head, data, games, args)
                if row:
                    update_summary(Path(args.ckpt_dir) / "summary.csv", row)
            except KeyboardInterrupt:
                raise
            except Exception as e:   # one bad model must not stop the whole run; --resume retries it
                failed.append(f"{hero}/{head}")
                tqdm.write(f"  {hero}/{head} FAILED ({type(e).__name__}: {e}); moving on")
                traceback.print_exc()

    if failed:
        print(f"\n{len(failed)} model(s) failed: {', '.join(failed)}. Rerun with --resume to retry them.")


if __name__ == "__main__":
    main()
