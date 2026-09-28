#!/usr/bin/env python3
"""节点2 的离线语音识别前端：按键说话 → sherpa-onnx 识别 → 文字逐行打到标准输出。

与传输层 node2.py 通过管道配合（node2.py 从 stdin 一行一句读）：

    python3 asr_front.py | python3 node2.py --state ASR_READY

约定：
- **识别结果只走 stdout**（一行一句，UTF-8），日志一律走 stderr，避免污染管道；
- 触发方式：TRIGGER_MODE=button（默认，按住按键说话，松手识别）或 vad（常开麦克风自动切句）。

环境变量：
    AUDIO_DEVICE / MIC_GAIN / ASR_MODEL_DIR / ASR_MODEL_KIND / TRIGGER_MODE /
    BUTTON_GPIO / RECORD_MAX_MS / RELEASE_TAIL_MS /
    VAD_TRIGGER / VAD_NOISE_MULT / VAD_TRIGGER_MIN / VAD_SILENCE_MS / VAD_MAX_MS
"""

import array
import math
import os
import select
import subprocess
import sys
import time

import sherpa_onnx

# ---------------- 配置 ----------------
MODEL_DIR = os.environ.get("ASR_MODEL_DIR", os.path.expanduser("~/models/paraformer-zh"))
MODEL_KIND = os.environ.get("ASR_MODEL_KIND", "paraformer")
AUDIO_DEVICE = os.environ.get("AUDIO_DEVICE", "plughw:CARD=Newmine,DEV=0")
SAMPLE_RATE = 16000
FRAME_MS = 100

TRIGGER_MODE = os.environ.get("TRIGGER_MODE", "button")
BUTTON_GPIO = int(os.environ.get("BUTTON_GPIO", "17"))
RECORD_MAX_MS = int(os.environ.get("RECORD_MAX_MS", "8000"))
RELEASE_TAIL_MS = int(os.environ.get("RELEASE_TAIL_MS", "200"))

VAD_TRIGGER = float(os.environ.get("VAD_TRIGGER", "0"))
VAD_NOISE_MULT = float(os.environ.get("VAD_NOISE_MULT", "3.0"))
VAD_TRIGGER_MIN = float(os.environ.get("VAD_TRIGGER_MIN", "0.010"))
VAD_SILENCE_MS = int(os.environ.get("VAD_SILENCE_MS", "700"))
VAD_MAX_MS = int(os.environ.get("VAD_MAX_MS", "8000"))

MIC_GAIN = float(os.environ.get("MIC_GAIN", "1.0"))


def log(msg):
    print("[ASR] %s" % msg, file=sys.stderr, flush=True)


def emit(text):
    """把一句识别结果交给 node2.py（stdout，一行一句）"""
    print(text, flush=True)


# ---------------- 音频采集 ----------------
class AudioSource:
    """调用 arecord 采集 16 kHz 单声道 PCM；读不满会超时报错，不会无声卡死"""

    def __init__(self, device, gain=1.0):
        self.device = device
        self.gain = gain
        self.proc = subprocess.Popen(
            ["arecord", "-q", "-D", device, "-f", "S16_LE", "-r", str(SAMPLE_RATE),
             "-c", "1", "-t", "raw"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)

    def _stderr_text(self):
        try:
            return self.proc.stderr.read().decode("utf-8", "ignore").strip()
        except Exception:
            return ""

    def read_frames(self, samples, timeout_s=5.0):
        nbytes = samples * 2
        buf = b""
        deadline = time.time() + timeout_s
        while len(buf) < nbytes:
            remain = deadline - time.time()
            if remain <= 0:
                raise RuntimeError("录音超时（%s）：%s"
                                   % (self.device, self._stderr_text() or "无错误输出"))
            ready, _, _ = select.select([self.proc.stdout], [], [], remain)
            if not ready:
                continue
            chunk = self.proc.stdout.read(nbytes - len(buf))
            if not chunk:
                raise RuntimeError("录音进程结束（%s）：%s"
                                   % (self.device, self._stderr_text() or "无错误输出"))
            buf += chunk
        pcm = array.array("h")
        pcm.frombytes(buf)
        vals = [v / 32768.0 for v in pcm]
        if self.gain != 1.0:
            vals = [max(-1.0, min(1.0, v * self.gain)) for v in vals]
        return vals

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=2)
        except Exception:
            pass


def rms(samples):
    if not samples:
        return 0.0
    return math.sqrt(sum(v * v for v in samples) / len(samples))


# ---------------- 识别 ----------------
def build_recognizer():
    tokens = os.path.join(MODEL_DIR, "tokens.txt")
    if MODEL_KIND == "sense_voice":
        model = os.path.join(MODEL_DIR, "model.int8.onnx")
        if not os.path.isfile(model):
            model = os.path.join(MODEL_DIR, "model.onnx")
        log("加载 SenseVoice 模型: %s" % model)
        return sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=model, tokens=tokens, num_threads=2, use_itn=True, debug=False)

    model = os.path.join(MODEL_DIR, "model.int8.onnx")
    if not os.path.isfile(model):
        model = os.path.join(MODEL_DIR, "model.onnx")
    log("加载 Paraformer 模型: %s" % model)
    return sherpa_onnx.OfflineRecognizer.from_paraformer(
        paraformer=model, tokens=tokens, num_threads=2,
        sample_rate=SAMPLE_RATE, feature_dim=80,
        decoding_method="greedy_search", debug=False)


def recognize(recognizer, samples):
    stream = recognizer.create_stream()
    stream.accept_waveform(SAMPLE_RATE, samples)
    recognizer.decode_stream(stream)
    return stream.result.text.strip()


# ---------------- 按键 ----------------
class Button:
    """轻触按键：一脚接 BUTTON_GPIO，另一脚接 GND，内部上拉，按下读到 0"""

    def __init__(self, pin):
        import lgpio
        self.lgpio = lgpio
        self.pin = pin
        self.handle = lgpio.gpiochip_open(0)
        lgpio.gpio_claim_input(self.handle, pin, lgpio.SET_PULL_UP)

    def pressed(self):
        return self.lgpio.gpio_read(self.handle, self.pin) == 0

    def close(self):
        try:
            self.lgpio.gpiochip_close(self.handle)
        except Exception:
            pass


def collect_while_pressed(source, button):
    frame_samples = SAMPLE_RATE * FRAME_MS // 1000
    frames = []
    deadline = time.time() + RECORD_MAX_MS / 1000.0
    while time.time() < deadline:
        frames.append(source.read_frames(frame_samples))
        if not button.pressed():
            for _ in range(max(1, RELEASE_TAIL_MS // FRAME_MS)):
                try:
                    frames.append(source.read_frames(frame_samples))
                except Exception:
                    break
            break
    return frames


# ---------------- 主流程 ----------------
def handle_frames(recognizer, frames, reason):
    total_ms = len(frames) * FRAME_MS
    if total_ms < 300:
        log("录音太短（%.1f 秒），忽略" % (total_ms / 1000.0))
        return
    flat = [v for f in frames for v in f]
    t0 = time.time()
    text = recognize(recognizer, flat)
    cost = time.time() - t0
    if not text:
        log("未识别到内容 (%.2fs)" % cost)
        return
    log("识别结果 (%.2fs, %s): %s" % (cost, reason, text))
    emit(text)


def main():
    log("启动：模式=%s，模型=%s" % (TRIGGER_MODE, MODEL_DIR))
    recognizer = build_recognizer()
    frame_samples = SAMPLE_RATE * FRAME_MS // 1000

    if TRIGGER_MODE == "vad":
        source = AudioSource(AUDIO_DEVICE, MIC_GAIN)
        if VAD_TRIGGER > 0:
            trigger = VAD_TRIGGER
            log("VAD 阈值（手动）= %.4f" % trigger)
        else:
            log("校准本底噪声：请保持安静 1.5 秒 ...")
            try:
                levels = [rms(source.read_frames(frame_samples)) for _ in range(15)]
                noise = sorted(levels)[len(levels) // 2]
            except Exception as exc:
                log("校准失败：%s" % exc)
                noise = 0.0
            trigger = max(VAD_TRIGGER_MIN, noise * VAD_NOISE_MULT)
            log("本底噪声 RMS=%.4f → 阈值=%.4f（增益 %.1fx）" % (noise, trigger, MIC_GAIN))

        pre_roll, utterance = [], []
        in_speech, silence_ms = False, 0
        log("开始监听（VAD 自动切句）")
        while True:
            try:
                frame = source.read_frames(frame_samples)
            except Exception as exc:
                log("录音异常：%s" % exc)
                time.sleep(1)
                source = AudioSource(AUDIO_DEVICE, MIC_GAIN)
                continue
            level = rms(frame)
            if not in_speech:
                pre_roll.append(frame)
                if len(pre_roll) > 3:
                    pre_roll.pop(0)
                if level >= trigger:
                    in_speech, silence_ms = True, 0
                    utterance = list(pre_roll)
                    pre_roll = []
                    log("检测到说话 (RMS=%.3f)" % level)
            else:
                utterance.append(frame)
                silence_ms = 0 if level >= trigger * 0.6 else silence_ms + FRAME_MS
                if silence_ms >= VAD_SILENCE_MS or len(utterance) * FRAME_MS >= VAD_MAX_MS:
                    frames, utterance, in_speech = utterance, [], False
                    handle_frames(recognizer, frames, "VAD")
        return

    button = Button(BUTTON_GPIO)
    log("按键模式：按住 GPIO%d 说话，松手识别（最长 %.1f 秒）" % (BUTTON_GPIO, RECORD_MAX_MS / 1000.0))
    while True:
        if not button.pressed():
            time.sleep(0.02)
            continue
        time.sleep(0.03)
        if not button.pressed():
            continue
        log("按键按下，开始录音")
        source = AudioSource(AUDIO_DEVICE, MIC_GAIN)
        try:
            frames = collect_while_pressed(source, button)
        except Exception as exc:
            log("录音异常：%s" % exc)
            frames = []
        finally:
            source.close()
        log("松手，开始识别（%.1f 秒音频）" % (len(frames) * FRAME_MS / 1000.0))
        handle_frames(recognizer, frames, "按键")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
