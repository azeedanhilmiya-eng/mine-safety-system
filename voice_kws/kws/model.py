"""DS-CNN keyword spotter sized for an ESP32-S3.

Depthwise-separable convolutions give most of a plain CNN's accuracy at a
fraction of the multiply-accumulates, which is why every microcontroller KWS
reference design uses them.  The shape here is deliberately conservative:

    input            49 x 40 x 1      int8 features from the micro frontend
    conv 64 @10x4    stride (2,4)  -> 25 x 10 x 64
    4 x [dw 3x3 + pw 1x1 @64]      -> 25 x 10 x 64
    global average pool            -> 64
    dense                          -> num_classes

~22k parameters, ~5.3M MACs, about 30 KB once quantised to int8.
"""

from __future__ import annotations

from tensorflow import keras
from tensorflow.keras import layers

from . import config as C


def ds_cnn(num_classes: int = C.NUM_CLASSES, cfg: dict | None = None) -> keras.Model:
    cfg = {**C.MODEL, **(cfg or {})}

    inp = layers.Input(shape=C.FEATURE_SHAPE, name="features")
    x = layers.Conv2D(cfg["first_conv_filters"], cfg["first_conv_kernel"],
                      strides=cfg["first_conv_stride"], padding="same",
                      use_bias=False, name="conv1")(inp)
    x = layers.BatchNormalization(momentum=cfg["bn_momentum"],
                                 name="conv1_bn")(x)
    x = layers.ReLU(name="conv1_relu")(x)

    for i in range(cfg["ds_blocks"]):
        x = layers.DepthwiseConv2D((3, 3), padding="same", use_bias=False,
                                   name=f"dw{i}")(x)
        x = layers.BatchNormalization(momentum=cfg["bn_momentum"],
                                     name=f"dw{i}_bn")(x)
        x = layers.ReLU(name=f"dw{i}_relu")(x)
        x = layers.Conv2D(cfg["ds_filters"], (1, 1), padding="same",
                          use_bias=False, name=f"pw{i}")(x)
        x = layers.BatchNormalization(momentum=cfg["bn_momentum"],
                                     name=f"pw{i}_bn")(x)
        x = layers.ReLU(name=f"pw{i}_relu")(x)

    x = layers.GlobalAveragePooling2D(name="gap")(x)
    x = layers.Dropout(cfg["dropout"], name="dropout")(x)
    out = layers.Dense(num_classes, activation="softmax", name="logits")(x)

    return keras.Model(inp, out, name="ds_cnn_s")


def macs(model: keras.Model) -> int:
    """Rough multiply-accumulate count, enough to sanity-check the budget."""
    total = 0
    for layer in model.layers:
        if isinstance(layer, layers.DepthwiseConv2D):
            h, w, ch = layer.output_shape[1:]
            kh, kw = layer.kernel_size
            total += h * w * ch * kh * kw
        elif isinstance(layer, layers.Conv2D):
            h, w, co = layer.output_shape[1:]
            kh, kw = layer.kernel_size
            ci = layer.input_shape[-1]
            total += h * w * co * kh * kw * ci
        elif isinstance(layer, layers.Dense):
            total += layer.input_shape[-1] * layer.units
    return total


def describe(model: keras.Model) -> str:
    return (f"{model.name}: {model.count_params():,} params, "
            f"{macs(model) / 1e6:.2f} M MACs, input {C.FEATURE_SHAPE}")
