from __future__ import annotations

import argparse
import logging
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path

from mine_voice.app import MineVoiceApp
from mine_voice.config import load_config, resolve_path


def configure_logging(config: dict, project_dir: Path, verbose: bool) -> None:
    log_path = resolve_path(project_dir, config["runtime"]["log_file"])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    formatter = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    handlers: list[logging.Handler] = [logging.StreamHandler()]
    handlers.append(RotatingFileHandler(log_path, maxBytes=2_000_000, backupCount=3, encoding="utf-8"))
    for handler in handlers:
        handler.setFormatter(formatter)
    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO, handlers=handlers)


def main() -> None:
    parser = argparse.ArgumentParser(description="树莓派 M2 离线语音转写与 LoRa 发送节点")
    parser.add_argument("--config", default="config.json", help="JSON 配置文件")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    config, project_dir = load_config(args.config)
    configure_logging(config, project_dir, args.verbose)
    logger = logging.getLogger(__name__)
    while True:
        try:
            app = MineVoiceApp(config, project_dir)
            app.run()
        except KeyboardInterrupt:
            logger.info("收到 Ctrl+C，程序退出")
            return
        except FileNotFoundError as exc:
            logger.warning("%s；15 秒后自动检查", exc)
            time.sleep(15)
        except Exception:
            logger.exception("节点启动或运行失败，15 秒后自动重试")
            time.sleep(15)


if __name__ == "__main__":
    main()
