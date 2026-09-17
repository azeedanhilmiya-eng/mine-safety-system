#!/usr/bin/env python3
"""Evaluate the INT8 model and pick the operating threshold.

    python evaluate.py
    python evaluate.py --max-false-trigger-rate 0.005   # stricter

Produces out/report/: confusion matrix (txt + png), threshold sweep (csv + png)
and threshold.json, which tools/export_c_array.py bakes into the firmware
header so the board and the report use the same number.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf

from kws import config as C, metrics
from quantize import int8_predict

OUT = Path(__file__).resolve().parent / "out"
REPORT = OUT / "report"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", type=Path, default=OUT / "kws_int8.tflite")
    ap.add_argument("--features", type=Path, default=OUT / "features.npz")
    ap.add_argument("--max-false-trigger-rate", type=float, default=0.01)
    ap.add_argument("--min-keyword-recall", type=float, default=0.60,
                    help="a threshold that recognises less than this is not a "
                         "usable operating point, however quiet it is")
    args = ap.parse_args()

    z = np.load(args.features)
    x_te, y_te = z["test_x"], z["test_y"]
    if x_te.shape[0] == 0:
        raise SystemExit("test split is empty; pin held-out speakers with "
                         "train.py --test-speakers")

    interpreter = tf.lite.Interpreter(model_path=str(args.model))
    interpreter.allocate_tensors()
    probs = int8_predict(interpreter, x_te)

    sweep = metrics.threshold_sweep(y_te, probs)
    threshold, budget_met = metrics.pick_threshold(
        sweep, args.max_false_trigger_rate, args.min_keyword_recall)

    pred = metrics.apply_threshold(probs, threshold)
    cm = metrics.confusion(y_te, pred)
    ft, neg = metrics.false_trigger_rate(y_te, probs, threshold)
    recall = metrics.keyword_recall(y_te, probs, threshold)

    warning = ""
    if not budget_met:
        best_ft = min(r["false_trigger_rate"] for r in sweep)
        best_recall = max(r["keyword_recall"] for r in sweep)
        quiet_enough = [r for r in sweep
                        if r["false_trigger_rate"] <= args.max_false_trigger_rate]
        if not quiet_enough:
            why = (f"    Too many false triggers: budget "
                   f"{args.max_false_trigger_rate:.3f}, best achievable "
                   f"{best_ft:.3f}.\n"
                   "    Fix it with data, not with this number: add confusable "
                   "words and machine\n"
                   "    noise to the `unknown` class, then retrain.\n")
        else:
            quiet_recall = max(r["keyword_recall"] for r in quiet_enough)
            why = (f"    Quiet enough, but deaf: the best recall within the "
                   f"false-trigger budget is\n"
                   f"    {quiet_recall:.3f}, below the required "
                   f"{args.min_keyword_recall:.3f} (best at any threshold: "
                   f"{best_recall:.3f}).\n"
                   "    A model that rarely fires trivially passes a "
                   "false-trigger budget; it is not\n"
                   "    a working keyword spotter. Retrain before reporting "
                   "anything from it.\n")
        warning = ("\n*** NO USABLE OPERATING POINT ***\n" + why)

    REPORT.mkdir(parents=True, exist_ok=True)
    text = (warning + f"threshold = {threshold:.2f}  "
            f"(false-trigger budget {args.max_false_trigger_rate:.3f})\n\n"
            + metrics.format_confusion(cm)
            + f"\n\nkeyword recall      : {recall:.4f}"
              f"\nfalse triggers      : {ft} / {neg} negative clips"
              f"\nfalse-trigger rate  : {ft / neg if neg else 0:.4f}\n")
    print(text)
    (REPORT / "confusion.txt").write_text(text, encoding="utf-8")

    metrics.write_csv(sweep, REPORT / "threshold_sweep.csv")
    (REPORT / "threshold.json").write_text(json.dumps({
        "threshold": threshold,
        "budget_met": budget_met,
        "keyword_recall": recall,
        "false_triggers": ft,
        "negatives": neg,
        "max_false_trigger_rate": args.max_false_trigger_rate,
    }, indent=2), encoding="utf-8")

    drew = metrics.plot_confusion(cm, REPORT / "confusion.png",
                                  f"INT8 confusion (threshold {threshold:.2f})")
    drew &= metrics.plot_sweep(sweep, REPORT / "threshold_sweep.png", threshold)
    if not drew:
        print("[note] matplotlib missing, CSV/TXT written without plots")

    print(f"[done] {REPORT}")
    return 0 if budget_met else 2


if __name__ == "__main__":
    raise SystemExit(main())
