"""Build and upload ESP-SR weights; PlatformIO does not run IDF's flash target."""
import json
import os
import subprocess

Import("env")

project = env.subst("$PROJECT_DIR")
build = env.subst("$BUILD_DIR")
component = os.path.join(project, "managed_components", "espressif__esp-sr")
generator = os.path.join(component, "model", "movemodel.py")
config = os.path.join(project, "sdkconfig." + env.subst("$PIOENV"))
model_image = os.path.join(build, "srmodels", "srmodels.bin")
with open(os.path.join(build, "flasher_args.json"), encoding="utf-8") as handle:
    model_offset = json.load(handle)["model"]["offset"]


def pack_model(source, target, env):
    subprocess.run(
        [env.subst("$PYTHONEXE"), generator, "-d1", config,
         "-d2", component, "-d3", build], check=True,
    )
    # Must match the model partition in partitions.csv.
    if os.path.getsize(str(target[0])) > 0x600000:
        raise RuntimeError("ESP-SR weights exceed the 6 MiB model partition")


weights = env.Command(model_image, [config, generator,
    os.path.join(component, "model", "pack_model.py"),
    os.path.join(project, "dependencies.lock"),
    os.path.join(project, "scripts", "esp_sr_model.py")], pack_model)
env.Depends("$BUILD_DIR/${PROGNAME}.bin", weights)
env.Depends("upload", weights)
env.Append(FLASH_EXTRA_IMAGES=[(model_offset, model_image)])
