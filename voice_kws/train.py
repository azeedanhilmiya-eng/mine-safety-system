#!/usr/bin/env python3
"""Train the keyword spotter.

    python train.py                       # real data under data/raw
    python train.py --variants 12         # heavier augmentation
    python train.py --test-speakers li,wang   # pin held-out speakers

Writes out/kws_float.keras plus out/report/dataset.txt so the split that
produced the numbers is recorded next to them.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf
from tensorflow import keras

from kws import config as C, dataset as ds
from kws.model import describe, ds_cnn

OUT = Path(__file__).resolve().parent / "out"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", type=Path, default=ds.RAW_DIR)
    ap.add_argument("--noise", type=Path, default=ds.NOISE_DIR)
    ap.add_argument("--variants", type=int, default=8,
                    help="augmented copies per training clip (default 8)")
    ap.add_argument("--epochs", type=int, default=C.TRAIN["epochs"])
    ap.add_argument("--batch-size", type=int, default=C.TRAIN["batch_size"])
    ap.add_argument("--seed", type=int, default=C.TRAIN["seed"])
    ap.add_argument("--test-speakers", default="",
                    help="comma-separated speakers forced into the test split")
    ap.add_argument("--val-speakers", default="")
    ap.add_argument("--cache", type=Path, default=None,
                    help="npz feature cache; reused if it already exists")
    ap.add_argument("--out", type=Path, default=OUT)
    args = ap.parse_args()

    keras.utils.set_random_seed(args.seed)

    overrides = {s: "test" for s in filter(None, args.test_speakers.split(","))}
    overrides |= {s: "val" for s in filter(None, args.val_speakers.split(","))}

    print("[data] building features ...")
    data = ds.build_all(variants=args.variants, seed=args.seed,
                        raw_dir=args.raw, noise_dir=args.noise,
                        overrides=overrides, cache=args.cache)

    clips = ds.scan(args.raw)
    summary = ds.summarise(clips, data["assignment"])
    print(summary)

    (x_tr, y_tr), (x_va, y_va), (x_te, y_te) = data["train"], data["val"], data["test"]
    print(f"[data] train {x_tr.shape[0]} (augmented)  val {x_va.shape[0]}  "
          f"test {x_te.shape[0]}")
    for name, y in (("train", y_tr), ("val", y_va), ("test", y_te)):
        if y.size == 0:
            print(f"[warn] {name} split is EMPTY -- check speaker assignment")
    if x_va.shape[0] == 0:
        raise SystemExit("validation split is empty; pin a speaker with --val-speakers")

    model = ds_cnn()
    print("[model]", describe(model))
    model.compile(
        optimizer=keras.optimizers.Adam(C.TRAIN["lr"]),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )

    args.out.mkdir(parents=True, exist_ok=True)
    ckpt = args.out / "kws_float.keras"
    callbacks = [
        keras.callbacks.ModelCheckpoint(str(ckpt), monitor="val_accuracy",
                                        save_best_only=True, mode="max"),
        keras.callbacks.ReduceLROnPlateau(monitor="val_loss", factor=0.5,
                                          patience=5, min_lr=C.TRAIN["lr_min"]),
        keras.callbacks.EarlyStopping(monitor="val_accuracy", mode="max",
                                      patience=C.TRAIN["patience"],
                                      restore_best_weights=True),
    ]

    # Features are int8 on disk to keep the cache small; the model trains in
    # float on exactly those values, which is also what the int8 interpreter
    # will see after quantisation.
    model.fit(
        x_tr.astype(np.float32), y_tr,
        validation_data=(x_va.astype(np.float32), y_va),
        epochs=args.epochs, batch_size=args.batch_size,
        class_weight=ds.class_weights(y_tr),
        callbacks=callbacks, verbose=2,
    )

    report = args.out / "report"
    report.mkdir(parents=True, exist_ok=True)
    synthetic = (args.raw.parent / "SYNTHETIC").exists()
    if synthetic:
        print("\n[PROVENANCE] training on SYNTHETIC data -- the resulting model "
              "is a placeholder,\n              not something to demo or report.\n")
    (report / "provenance.json").write_text(
        json.dumps({"synthetic": synthetic,
                    "raw_dir": str(args.raw),
                    "speakers": sorted({c.speaker for c in clips}),
                    "clips": len(clips)}, indent=2), encoding="utf-8")
    (report / "dataset.txt").write_text(summary + "\n", encoding="utf-8")
    (report / "split.json").write_text(json.dumps(data["assignment"], indent=2,
                                                  ensure_ascii=False), encoding="utf-8")

    if x_te.shape[0]:
        loss, acc = model.evaluate(x_te.astype(np.float32), y_te, verbose=0)
        print(f"[float] held-out test accuracy {acc:.4f}")
        (report / "float_test.json").write_text(
            json.dumps({"loss": float(loss), "accuracy": float(acc),
                        "n": int(x_te.shape[0])}, indent=2), encoding="utf-8")

    np.savez_compressed(args.out / "features.npz",
                        train_x=x_tr, train_y=y_tr, val_x=x_va, val_y=y_va,
                        test_x=x_te, test_y=y_te)
    print(f"[done] {ckpt}")
    print(f"[done] {args.out / 'features.npz'}  (quantize.py reads this)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
