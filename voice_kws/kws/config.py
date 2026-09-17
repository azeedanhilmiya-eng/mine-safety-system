"""Single source of truth for every parameter shared by training and firmware.

Nothing in this file may be changed without regenerating BOTH the model
(`quantize.py`) and the firmware header (`tools/export_c_array.py`).  A silent
mismatch between the training frontend and the device frontend is the number
one cause of "95% on the PC, 50% on the board".
"""

# ---------------------------------------------------------------- audio ----
SAMPLE_RATE = 16000
CLIP_MS = 1000
CLIP_SAMPLES = SAMPLE_RATE * CLIP_MS // 1000          # 16000

# ------------------------------------------------------------- frontend ----
# These values mirror tflite-micro's micro_speech frontend configuration
# (micro_features_generator.cc) so that the device can run the stock generator
# unmodified.  Only num_channels / window_size / window_step differ from the
# library defaults, and those three are set identically on both sides.
FRONTEND = dict(
    sample_rate=SAMPLE_RATE,
    window_size=30,               # ms
    window_step=20,               # ms
    num_channels=40,
    lower_band_limit=125.0,
    upper_band_limit=7500.0,
    smoothing_bits=10,            # noise reduction block
    even_smoothing=0.025,
    odd_smoothing=0.06,
    min_signal_remaining=0.05,
    enable_pcan=True,             # per-channel automatic gain control
    pcan_strength=0.95,
    pcan_offset=80.0,
    gain_bits=21,
    enable_log=True,
    scale_shift=6,
)

NUM_FRAMES = 49                   # (1000 - 30) // 20 + 1
NUM_CHANNELS = FRONTEND["num_channels"]
FEATURE_SHAPE = (NUM_FRAMES, NUM_CHANNELS, 1)
FEATURE_BYTES = NUM_FRAMES * NUM_CHANNELS

# uint16 frontend output -> uint8 -> int8, byte-identical to micro_speech's
#   value = ((v * 256) + 333) / 666 ; clamp 0..255 ; then -128
VALUE_SCALE = 256
VALUE_DIV = 666                   # int(25.6 * 26.0 + 0.5)

# --------------------------------------------------------------- labels ----
# Order is frozen.  New keywords are APPENDED, never inserted, because the
# index travels over LoRa as the command id and the surface gateway maps it to
# Chinese text.
LABELS = ["silence", "unknown", "help", "evacuate"]
LABELS_ZH = ["静音", "未知", "救命", "撤离"]
NUM_CLASSES = len(LABELS)

# Classes that must never raise an alarm, whatever the confidence.
NON_COMMAND_LABELS = ("silence", "unknown")
NON_COMMAND_IDS = tuple(LABELS.index(x) for x in NON_COMMAND_LABELS)

# ---------------------------------------------------------------- model ----
MODEL = dict(
    first_conv_filters=64,
    first_conv_kernel=(10, 4),
    first_conv_stride=(2, 4),     # 49x40 -> 25x10, keeps MACs near 5.3M
    ds_blocks=4,
    ds_filters=64,
    dropout=0.3,
    # An epoch here is a couple of dozen steps. At Keras' default momentum of
    # 0.99 the BatchNorm moving statistics lag far behind the batch statistics
    # the layers actually trained with, and validation accuracy sits at the
    # majority-class rate while training accuracy climbs. 0.9 tracks closely
    # enough for a dataset this size.
    bn_momentum=0.9,
)

# ---------------------------------------------------------------- train ----
TRAIN = dict(
    batch_size=64,
    epochs=60,
    lr=1e-3,
    lr_min=1e-5,
    patience=12,
    seed=1337,
)

# ------------------------------------------------------------ inference ----
# Chosen by evaluate.py's threshold sweep; firmware reads it from the header.
DEFAULT_CONF_THRESHOLD = 0.80

# Push-to-talk capture is longer than one window so three overlapping windows
# can vote; offsets are in milliseconds from the start of the capture.
PTT_CAPTURE_MS = 1400
WINDOW_OFFSETS_MS = (0, 200, 400)
