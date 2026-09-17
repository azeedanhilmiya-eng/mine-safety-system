"""Evaluation helpers that produce exactly the numbers the report asks for.

The competition document wants, per keyword: recall, overall accuracy, a
confusion matrix, false-trigger counts, and the operating threshold that
justifies them.  Everything here writes both a human-readable table and a CSV
so the figures in the PDF and the figures in the repo can never disagree.
"""

from __future__ import annotations

import csv
from pathlib import Path

import numpy as np

from . import config as C


def confusion(y_true: np.ndarray, y_pred: np.ndarray,
              n: int = C.NUM_CLASSES) -> np.ndarray:
    m = np.zeros((n, n), dtype=np.int64)
    for t, p in zip(y_true, y_pred):
        m[t, p] += 1
    return m


def format_confusion(m: np.ndarray, labels: list[str] | None = None) -> str:
    labels = labels or C.LABELS
    w = max(len(x) for x in labels) + 2
    head = " " * w + "".join(f"{x:>10}" for x in labels) + "     recall"
    rows = [head]
    for i, label in enumerate(labels):
        total = m[i].sum()
        recall = m[i, i] / total if total else float("nan")
        rows.append(f"{label:<{w}}" + "".join(f"{v:>10}" for v in m[i])
                    + f"{recall:>11.3f}")
    rows.append("")
    rows.append(f"overall accuracy: {np.trace(m) / max(m.sum(), 1):.4f}  "
                f"(n={m.sum()})")
    return "\n".join(rows)


def apply_threshold(probs: np.ndarray, threshold: float) -> np.ndarray:
    """Argmax, but anything below `threshold` collapses to `unknown`.

    This is the same rule the firmware applies, so PC numbers and board
    numbers describe the same decision function.
    """
    unknown = C.LABELS.index("unknown")
    pred = probs.argmax(axis=1)
    conf = probs.max(axis=1)
    pred = np.where(conf < threshold, unknown, pred)
    # silence/unknown never raise an alarm regardless of confidence
    return pred


def false_trigger_rate(y_true: np.ndarray, probs: np.ndarray,
                       threshold: float) -> tuple[int, int]:
    """(false triggers, negative clips): a non-command clip predicted as a command."""
    pred = apply_threshold(probs, threshold)
    negatives = np.isin(y_true, C.NON_COMMAND_IDS)
    fired = ~np.isin(pred, C.NON_COMMAND_IDS)
    return int((negatives & fired).sum()), int(negatives.sum())


def keyword_recall(y_true: np.ndarray, probs: np.ndarray,
                   threshold: float) -> float:
    pred = apply_threshold(probs, threshold)
    is_cmd = ~np.isin(y_true, C.NON_COMMAND_IDS)
    if not is_cmd.any():
        return float("nan")
    return float((pred[is_cmd] == y_true[is_cmd]).mean())


def threshold_sweep(y_true: np.ndarray, probs: np.ndarray,
                    lo: float = 0.30, hi: float = 0.99,
                    step: float = 0.01) -> list[dict]:
    """Recall vs false-trigger rate across thresholds -- the curve for the report."""
    rows = []
    for t in np.arange(lo, hi + 1e-9, step):
        ft, neg = false_trigger_rate(y_true, probs, float(t))
        rows.append({
            "threshold": round(float(t), 3),
            "keyword_recall": round(keyword_recall(y_true, probs, float(t)), 4),
            "false_triggers": ft,
            "negatives": neg,
            "false_trigger_rate": round(ft / neg, 4) if neg else 0.0,
        })
    return rows


def pick_threshold(sweep: list[dict],
                   max_false_trigger_rate: float = 0.01,
                   min_keyword_recall: float = 0.60) -> tuple[float, bool]:
    """Pick an operating threshold; returns (threshold, budget_met).

    Lowest threshold that is quiet enough, not highest: among the thresholds
    within budget the smallest one keeps the most recall, and recall is what
    answers someone shouting for help.

    When nothing meets the budget this returns the strictest row with
    budget_met=False.  That case means the model cannot separate commands from
    negatives at any threshold, and no amount of threshold tuning will fix it
    -- it wants more negative samples (confusable words and machine noise),
    not a different number here.  Callers must surface it rather than quietly
    reporting the fallback.
    """
    ok = [r for r in sweep
          if r["false_trigger_rate"] <= max_false_trigger_rate
          and r["keyword_recall"] >= min_keyword_recall]
    if not ok:
        # A model that never fires has a perfect false-trigger rate, so the
        # budget alone would wave it through. Recall is what stops that.
        quiet = [r for r in sweep
                 if r["false_trigger_rate"] <= max_false_trigger_rate]
        fallback = (min(quiet, key=lambda r: r["threshold"]) if quiet
                    else max(sweep, key=lambda r: r["threshold"]))
        return float(fallback["threshold"]), False
    return float(min(ok, key=lambda r: r["threshold"])["threshold"]), True


def write_csv(rows: list[dict], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def plot_confusion(m: np.ndarray, path: Path, title: str = "Confusion matrix") -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False

    norm = m / np.maximum(m.sum(axis=1, keepdims=True), 1)
    fig, ax = plt.subplots(figsize=(5.2, 4.6), dpi=160)
    ax.imshow(norm, cmap="Blues", vmin=0, vmax=1)
    ax.set_xticks(range(len(C.LABELS)), C.LABELS, rotation=30, ha="right")
    ax.set_yticks(range(len(C.LABELS)), C.LABELS)
    ax.set_xlabel("predicted")
    ax.set_ylabel("true")
    ax.set_title(title)
    for i in range(m.shape[0]):
        for j in range(m.shape[1]):
            ax.text(j, i, f"{m[i, j]}\n{norm[i, j]:.2f}", ha="center", va="center",
                    fontsize=8, color="white" if norm[i, j] > 0.5 else "black")
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path)
    plt.close(fig)
    return True


def plot_sweep(rows: list[dict], path: Path, chosen: float | None = None) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False

    t = [r["threshold"] for r in rows]
    fig, ax = plt.subplots(figsize=(5.6, 3.8), dpi=160)
    ax.plot(t, [r["keyword_recall"] for r in rows], label="keyword recall")
    ax.plot(t, [r["false_trigger_rate"] for r in rows], label="false-trigger rate")
    if chosen is not None:
        ax.axvline(chosen, ls="--", lw=1, color="gray")
        ax.annotate(f"chosen {chosen:.2f}", (chosen, 0.5), rotation=90,
                    fontsize=8, va="center", ha="right", color="gray")
    ax.set_xlabel("confidence threshold")
    ax.set_ylim(0, 1.02)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path)
    plt.close(fig)
    return True
