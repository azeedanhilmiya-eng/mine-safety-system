#!/usr/bin/env bash
# End-to-end rehearsal on synthetic data: proves the toolchain works before
# any real recording exists. Takes about a minute on a laptop.
#
#   ./run_smoke_test.sh
#
# It does NOT produce a usable model. The artefacts it writes are stamped as
# placeholders so they cannot be mistaken for the real thing.
set -euo pipefail

PY="${PYTHON:-python3}"
cd "$(dirname "$0")"

echo "=== 1/6 synthetic data ==="
"$PY" tools/make_synthetic.py --speakers 12 --per-class 48 --clean

echo; echo "=== 2/6 train ==="
"$PY" train.py --variants 6 --epochs 40

echo; echo "=== 3/6 quantize ==="
"$PY" quantize.py

echo; echo "=== 4/6 evaluate ==="
# Synthetic negatives are deliberately confusable, so the false-trigger budget
# is not expected to be met here; that check is for real data.
"$PY" evaluate.py || echo "(threshold budget not met -- expected on synthetic data)"

echo; echo "=== 5/6 export firmware artefacts ==="
"$PY" tools/export_c_array.py
"$PY" tools/export_test_vectors.py

echo; echo "=== 6/6 host tests for the decision logic ==="
make -C ../firmware/voice_node/test

echo; echo "smoke test complete."
echo "Artefacts in out/ and ../firmware/voice_node/ are PLACEHOLDERS."
