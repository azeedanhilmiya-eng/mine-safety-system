# Everything about this firmware that can be checked without an ESP32.
#
#   make            type-check both sketches, then run the unit tests
#   make sketches   type-check only
#   make units      unit tests only

PY ?= python3

all: sketches units

sketches:
	@$(PY) check_sketch.py ../surface_node.ino
	@$(PY) check_sketch.py ../voice_node/voice_node.ino
	@$(PY) check_sketch.py ../underground_node1_mine1.ino
	@$(PY) check_sketch.py ../underground_node2_mine2.ino

units:
	@$(MAKE) --no-print-directory -C ../voice_node/test

.PHONY: all sketches units
