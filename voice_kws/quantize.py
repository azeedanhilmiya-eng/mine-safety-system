#!/usr/bin/env python3
"""Full-integer INT8 quantisation, plus the C array the firmware compiles in.

    python quantize.py

The representative dataset is drawn from the *training* features, covering all
classes and all noise levels.  Using clean clips only is the classic mistake:
the activation ranges then come out too narrow and the board falls apart the
moment a fan starts up.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf
from tensorflow import keras

from kws import config as C
from kws.metrics import confusion, format_confusion

OUT = Path(__file__).resolve().parent / "out"


def representative_dataset(x: np.ndarray, y: np.ndarray, n: int, seed: int):
    """Stratified sample so every class contributes to the activation ranges."""
    rng = np.random.default_rng(seed)
    per_class = max(n // C.NUM_CLASSES, 1)
    picks = []
    for cls in range(C.NUM_CLASSES):
        idx = np.flatnonzero(y == cls)
        if idx.size == 0:
            continue
        picks.append(rng.choice(idx, size=min(per_class, idx.size), replace=False))
    sel = np.concatenate(picks) if picks else np.arange(min(n, x.shape[0]))
    rng.shuffle(sel)

    def gen():
        for i in sel:
            yield [x[i].reshape(1, *C.FEATURE_SHAPE).astype(np.float32)]

    return gen


def int8_predict(interpreter: tf.lite.Interpreter, x: np.ndarray) -> np.ndarray:
    """Run the int8 model over [N,49,40,1] int8 features, returning probabilities."""
    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    o_scale, o_zero = out["quantization"]

    probs = np.zeros((x.shape[0], C.NUM_CLASSES), dtype=np.float32)
    for i in range(x.shape[0]):
        interpreter.set_tensor(inp["index"], x[i].reshape(1, *C.FEATURE_SHAPE)
                               .astype(inp["dtype"]))
        interpreter.invoke()
        q = interpreter.get_tensor(out["index"]).astype(np.float32)[0]
        probs[i] = (q - o_zero) * o_scale
    return probs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", type=Path, default=OUT / "kws_float.keras")
    ap.add_argument("--features", type=Path, default=OUT / "features.npz")
    ap.add_argument("--out", type=Path, default=OUT / "kws_int8.tflite")
    ap.add_argument("--rep-size", type=int, default=400)
    ap.add_argument("--seed", type=int, default=C.TRAIN["seed"])
    ap.add_argument("--max-drop", type=float, default=0.02,
                    help="fail if int8 accuracy drops more than this vs float")
    args = ap.parse_args()

    model = keras.models.load_model(args.model)
    z = np.load(args.features)
    x_tr, y_tr = z["train_x"], z["train_y"]
    x_te, y_te = z["test_x"], z["test_y"]

    print(f"[quant] representative sample: {args.rep_size} of {x_tr.shape[0]}")
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = representative_dataset(x_tr, y_tr,
                                                         args.rep_size, args.seed)
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tflite = conv.convert()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(tflite)
    print(f"[quant] {args.out}  {len(tflite) / 1024:.1f} KB")

    interpreter = tf.lite.Interpreter(model_content=tflite)
    interpreter.allocate_tensors()
    inp, out = interpreter.get_input_details()[0], interpreter.get_output_details()[0]
    print(f"[quant] input  {inp['dtype'].__name__} scale={inp['quantization'][0]:.6g} "
          f"zero={inp['quantization'][1]}")
    print(f"[quant] output {out['dtype'].__name__} scale={out['quantization'][0]:.6g} "
          f"zero={out['quantization'][1]}")

    meta = {
        "tflite_bytes": len(tflite),
        "input_scale": float(inp["quantization"][0]),
        "input_zero_point": int(inp["quantization"][1]),
        "output_scale": float(out["quantization"][0]),
        "output_zero_point": int(out["quantization"][1]),
        "labels": C.LABELS,
    }

    if x_te.shape[0]:
        float_pred = model.predict(x_te.astype(np.float32), verbose=0).argmax(1)
        float_acc = float((float_pred == y_te).mean())

        int8_probs = int8_predict(interpreter, x_te)
        int8_pred = int8_probs.argmax(1)
        int8_acc = float((int8_pred == y_te).mean())

        drop = float_acc - int8_acc
        print(f"[quant] float {float_acc:.4f} -> int8 {int8_acc:.4f}  "
              f"(drop {drop * 100:+.2f} pp)")
        print(format_confusion(confusion(y_te, int8_pred)))

        meta |= {"float_accuracy": float_acc, "int8_accuracy": int8_acc,
                 "accuracy_drop": drop}
        np.save(OUT / "test_probs_int8.npy", int8_probs)

        if drop > args.max_drop:
            print(f"\n[FAIL] int8 lost {drop * 100:.2f} pp, budget is "
                  f"{args.max_drop * 100:.2f} pp.\n"
                  "       The representative dataset is probably not covering the "
                  "noise conditions.\n"
                  "       Raise --rep-size, or check that augmented clips are in "
                  "the training features.")
            (OUT / "report").mkdir(parents=True, exist_ok=True)
            (OUT / "report" / "quantization.json").write_text(
                json.dumps(meta, indent=2), encoding="utf-8")
            return 1

    (OUT / "report").mkdir(parents=True, exist_ok=True)
    (OUT / "report" / "quantization.json").write_text(json.dumps(meta, indent=2),
                                                      encoding="utf-8")
    print("[done] run evaluate.py next, then tools/export_c_array.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
