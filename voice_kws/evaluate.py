#!/usr/bin/env python3
"""Evaluate the INT8 model under the rule the board actually runs.

    python evaluate.py
    python evaluate.py --max-false-trigger-rate 0.005 --placements 5

The headline numbers come from the three-window vote in kws/postprocess.py,
the Python mirror of the firmware's kws_postprocess.c. A single-window score is
reported alongside as a diagnostic, but it is not what the device does: the
vote takes the highest confidence among the windows that agreed, which is never
below a single window's, so a threshold picked on single windows fires more
readily on the board than its own numbers suggest.

Writes out/report/: confusion matrices (txt + png), the threshold sweep
(csv + png) and threshold.json, which tools/export_c_array.py bakes into the
firmware header.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf

from kws import config as C, dataset as ds, metrics, postprocess as pp
from quantize import int8_predict, int8_predict_raw

OUT = Path(__file__).resolve().parent / "out"
REPORT = OUT / "report"


def load_test_clips(raw_dir: Path) -> list[ds.Clip]:
    split_path = REPORT / "split.json"
    if not split_path.exists():
        raise SystemExit(f"{split_path} not found -- run train.py first")
    assignment = json.loads(split_path.read_text())

    clips = ds.scan(raw_dir)
    missing = {c.speaker for c in clips} - set(assignment)
    if missing:
        raise SystemExit(
            f"speakers {sorted(missing)} are not in split.json. New recordings "
            "were added since training; retrain before evaluating, or the test "
            "set no longer means what it says.")
    return [c for c in clips if assignment[c.speaker] == "test"]


def vote_outcomes(interpreter, x_windows: np.ndarray,
                  scale: float, zero_point: int) -> tuple[np.ndarray, np.ndarray]:
    """Run every window and vote. Returns (labels, confidences)."""
    n, w = x_windows.shape[0], x_windows.shape[1]
    flat = x_windows.reshape(n * w, *C.FEATURE_SHAPE)
    raw = int8_predict_raw(interpreter, flat).reshape(n, w, C.NUM_CLASSES)

    labels = np.zeros(n, dtype=np.int64)
    confs = np.zeros(n, dtype=np.float64)
    for i in range(n):
        # threshold 0 here: the accept/reject decision is swept separately.
        r = pp.vote(raw[i], threshold=0.0, scale=scale, zero_point=zero_point)
        labels[i], confs[i] = r.label, r.confidence
    return labels, confs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", type=Path, default=OUT / "kws_int8.tflite")
    ap.add_argument("--features", type=Path, default=OUT / "features.npz")
    ap.add_argument("--raw", type=Path, default=ds.RAW_DIR)
    ap.add_argument("--placements", type=int, default=3,
                    help="random button-press timings evaluated per test clip")
    ap.add_argument("--seed", type=int, default=C.TRAIN["seed"])
    ap.add_argument("--max-false-trigger-rate", type=float, default=0.01)
    ap.add_argument("--min-keyword-recall", type=float, default=0.60,
                    help="a threshold that recognises less than this is not a "
                         "usable operating point, however quiet it is")
    args = ap.parse_args()

    interpreter = tf.lite.Interpreter(model_path=str(args.model))
    interpreter.allocate_tensors()
    out_det = interpreter.get_output_details()[0]
    o_scale, o_zero = float(out_det["quantization"][0]), int(out_det["quantization"][1])

    # ---- the rule the board runs -------------------------------------------
    clips = load_test_clips(args.raw)
    if not clips:
        raise SystemExit("test split is empty; pin held-out speakers with "
                         "train.py --test-speakers")
    print(f"[vote] {len(clips)} test clips x {args.placements} button timings")
    x_win, y_win = ds.build_vote_windows(clips, seed=args.seed,
                                         placements=args.placements)
    labels, confs = vote_outcomes(interpreter, x_win, o_scale, o_zero)

    sweep = metrics.sweep_decisions(y_win, labels, confs)
    threshold, budget_met = metrics.pick_threshold(
        sweep, args.max_false_trigger_rate, args.min_keyword_recall)

    pred = metrics.decisions_to_pred(labels, confs, threshold)
    cm = metrics.confusion(y_win, pred)
    ft, neg, recall = metrics.rates_from_pred(y_win, pred)
    disagreed = int((labels < 0).sum())

    warning = ""
    if not budget_met:
        best_ft = min(r["false_trigger_rate"] for r in sweep)
        quiet = [r for r in sweep
                 if r["false_trigger_rate"] <= args.max_false_trigger_rate]
        if not quiet:
            why = (f"    Too many false triggers: budget "
                   f"{args.max_false_trigger_rate:.3f}, best achievable "
                   f"{best_ft:.3f}.\n"
                   "    Fix it with data, not with this number: add confusable "
                   "words and machine\n"
                   "    noise to the `unknown` class, then retrain.\n")
        else:
            why = (f"    Quiet enough, but deaf: the best recall within the "
                   f"false-trigger budget is\n"
                   f"    {max(r['keyword_recall'] for r in quiet):.3f}, below "
                   f"the required {args.min_keyword_recall:.3f}.\n"
                   "    A model that rarely fires trivially passes a "
                   "false-trigger budget; it is not\n"
                   "    a working keyword spotter. Retrain before reporting "
                   "anything from it.\n")
        warning = "\n*** NO USABLE OPERATING POINT ***\n" + why

    text = (warning
            + f"three-window vote (what the board runs)\n"
              f"threshold = {threshold:.2f}  "
              f"(false-trigger budget {args.max_false_trigger_rate:.3f}, "
              f"min recall {args.min_keyword_recall:.2f})\n\n"
            + metrics.format_confusion(cm)
            + f"\n\nkeyword recall      : {recall:.4f}"
              f"\nfalse triggers      : {ft} / {neg} negative captures"
              f"\nfalse-trigger rate  : {ft / neg if neg else 0:.4f}"
              f"\nwindows disagreed   : {disagreed} / {len(labels)} captures\n")

    # ---- single-window diagnostic ------------------------------------------
    z = np.load(args.features)
    x_te, y_te = z["test_x"], z["test_y"]
    if x_te.shape[0]:
        probs = int8_predict(interpreter, x_te)
        single = metrics.apply_threshold(probs, threshold)
        s_ft, s_neg, s_recall = metrics.rates_from_pred(y_te, single)
        text += (f"\nsingle-window diagnostic at the same threshold "
                 f"(NOT the device rule):\n"
                 f"  keyword recall {s_recall:.4f}, false triggers "
                 f"{s_ft}/{s_neg}"
                 f" ({s_ft / s_neg if s_neg else 0:.4f})\n")

    print(text)

    REPORT.mkdir(parents=True, exist_ok=True)
    (REPORT / "confusion.txt").write_text(text, encoding="utf-8")
    metrics.write_csv(sweep, REPORT / "threshold_sweep.csv")
    (REPORT / "threshold.json").write_text(json.dumps({
        "decision_rule": "three_window_vote",
        "threshold": threshold,
        "budget_met": budget_met,
        "keyword_recall": recall,
        "false_triggers": ft,
        "negatives": neg,
        "windows_disagreed": disagreed,
        "captures": int(len(labels)),
        "placements_per_clip": args.placements,
        "max_false_trigger_rate": args.max_false_trigger_rate,
        "min_keyword_recall": args.min_keyword_recall,
    }, indent=2), encoding="utf-8")

    drew = metrics.plot_confusion(
        cm, REPORT / "confusion.png",
        f"INT8, three-window vote (threshold {threshold:.2f})")
    drew &= metrics.plot_sweep(sweep, REPORT / "threshold_sweep.png", threshold)
    if not drew:
        print("[note] matplotlib missing, CSV/TXT written without plots")

    print(f"[done] {REPORT}")
    return 0 if budget_met else 2


if __name__ == "__main__":
    raise SystemExit(main())
