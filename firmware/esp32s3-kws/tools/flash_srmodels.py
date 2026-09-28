# PlatformIO 后置动作: 把中文模型包 srmodels.bin 写入 model 分区
Import("env")
import os

MODEL_OFFSET = "0x710000"   # 取自 partitions_esp_sr_16_custom.csv，6MB model 分区


def _find_esptool(env):
    for pkg in ("tool-esptoolpy", "tool-esptool"):
        try:
            base = env.PioPlatform().get_package_dir(pkg)
        except Exception:
            continue
        if not base:
            continue
        for rel in ("esptool.py", "esptool/__init__.py"):
            cand = os.path.join(base, rel)
            if os.path.isfile(cand) and rel == "esptool.py":
                return cand
        for root, _dirs, files in os.walk(base):
            if "esptool.py" in files:
                return os.path.join(root, "esptool.py")
    return None


def flash_srmodels(source, target, env):
    bin_path = os.path.join(env.subst("$PROJECT_DIR"), "srmodels.bin")
    if not os.path.isfile(bin_path):
        print("[srmodels] 未找到 %s, 跳过模型烧录" % bin_path)
        return
    port = env.subst("$UPLOAD_PORT")
    if not port:
        print("[srmodels] 未检测到串口, 跳过模型烧录")
        return
    esptool = _find_esptool(env)
    if not esptool:
        print("[srmodels] 找不到 esptool, 请手动烧录: "
              "esptool.py write_flash %s srmodels.bin" % MODEL_OFFSET)
        return
    cmd = '"%s" "%s" --chip esp32s3 --port "%s" --baud 921600 write_flash %s "%s"' % (
        env.subst("$PYTHONEXE"), esptool, port, MODEL_OFFSET, bin_path)
    print("[srmodels] 烧录中文模型包 -> %s (%s)" % (MODEL_OFFSET, bin_path))
    env.Execute(cmd)


env.AddPostAction("upload", flash_srmodels)
