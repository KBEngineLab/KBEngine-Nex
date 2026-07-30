"""Create an isolated KBE configuration overlay. / 创建隔离的 KBE 配置覆盖层。"""

from __future__ import annotations

import json
import os
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


FIXTURES_ROOT = Path(__file__).resolve().parent / "fixtures"


def find_server_config(assets_root: Path) -> Path:
    for candidate in (assets_root / "res/server/kbengine.xml", assets_root / "kbengine.xml"):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"assets do not contain kbengine.xml: {assets_root}")


def create_config_overlay(
    assets_root: Path,
    output_root: Path,
    bots: int,
    tick_time: float = 0.1,
    tick_count: int | None = None,
    external_receive_messages: int | None = None,
    external_receive_bytes: int | None = None,
) -> Path:
    """Write only the per-run XML override; source assets remain read-only.
    只写入本次运行的 XML 覆盖文件，源资产保持只读。
    """
    if bots < 1:
        raise ValueError("bots must be positive")
    if tick_time <= 0:
        raise ValueError("bots tick time must be positive")
    if tick_count is None:
        tick_count = max(1, min(50, bots // 10))
    if tick_count < 1:
        raise ValueError("bots tick count must be positive")

    tree = ET.parse(find_server_config(assets_root))
    root = tree.getroot()
    bots_node = root.find("./bots")
    if bots_node is None:
        bots_node = ET.SubElement(root, "bots")
    default_add = bots_node.find("defaultAddBots")
    if default_add is None:
        default_add = ET.SubElement(bots_node, "defaultAddBots")
    set_xml_value(default_add, "totalCount", bots)
    set_xml_value(default_add, "tickTime", tick_time)
    set_xml_value(default_add, "tickCount", tick_count)

    if external_receive_messages is not None or external_receive_bytes is not None:
        channel_common = root.find("./channelCommon")
        if channel_common is None:
            channel_common = ET.SubElement(root, "channelCommon")
        window_overflow = channel_common.find("windowOverflow")
        if window_overflow is None:
            window_overflow = ET.SubElement(channel_common, "windowOverflow")
        receive = window_overflow.find("receive")
        if receive is None:
            receive = ET.SubElement(window_overflow, "receive")
        if external_receive_messages is not None:
            if external_receive_messages < 1:
                raise ValueError("external receive message limit must be positive")
            messages = receive.find("messages")
            if messages is None:
                messages = ET.SubElement(receive, "messages")
            set_xml_value(messages, "external", external_receive_messages)
        if external_receive_bytes is not None:
            if external_receive_bytes < 1:
                raise ValueError("external receive byte limit must be positive")
            receive_bytes = receive.find("bytes")
            if receive_bytes is None:
                receive_bytes = ET.SubElement(receive, "bytes")
            set_xml_value(receive_bytes, "external", external_receive_bytes)

    destination = output_root / "config-overlay/res/server/kbengine.xml"
    destination.parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="utf-8", xml_declaration=True)
    return destination


def resolve_bots_schedule(scenario: dict[str, Any], bots: int) -> tuple[float, int]:
    """Translate the declared connection rate into an exact Bots add schedule.
    将场景声明的连接速率转换为精确的 Bots 添加计划。
    """
    tick_time = float(scenario.get("bots_tick_time", 0.1))
    if tick_time <= 0:
        raise ValueError("bots_tick_time must be positive")
    if bots < 1:
        raise ValueError("bots must be positive")
    if "bots_tick_count" in scenario:
        tick_count = int(scenario["bots_tick_count"])
        if tick_count < 1:
            raise ValueError("bots_tick_count must be positive")
        return tick_time, tick_count
    if "connect_rate_per_second" not in scenario:
        return tick_time, max(1, min(50, bots // 10))
    connect_rate = float(scenario["connect_rate_per_second"])
    if connect_rate <= 0:
        raise ValueError("connect_rate_per_second must be positive")
    # 调整周期而不是舍入后接受速率漂移；例如 25/s 转换为每 0.08s 添加 2 个。
    # Adjust the period instead of accepting rounding drift; for example, 25/s becomes two clients every 0.08s.
    tick_count = max(1, round(connect_rate * tick_time))
    return tick_count / connect_rate, tick_count


def set_xml_value(parent: ET.Element, name: str, value: Any) -> None:
    node = parent.find(name)
    if node is None:
        node = ET.SubElement(parent, name)
    node.text = str(value)


def resolve_fixture_root(name: str | None) -> Path | None:
    """Resolve only a versioned fixture owned by the Performance Lab.
    仅解析 Performance Lab 自有的版本化 fixture，避免场景值逃逸到任意路径。
    """
    if not name:
        return None
    if Path(name).name != name:
        raise ValueError(f"invalid performance fixture name: {name}")
    fixture_root = (FIXTURES_ROOT / name).resolve()
    if fixture_root.parent != FIXTURES_ROOT.resolve() or not (fixture_root / "entities.xml").is_file():
        raise FileNotFoundError(f"performance fixture does not exist: {name}")
    return fixture_root


def build_environment(
    repository_root: Path,
    assets_root: Path,
    run_root: Path,
    fixture_root: Path | None = None,
) -> dict[str, str]:
    """Build isolated runtime paths without changing global environment.
    构造隔离运行路径，不修改全局环境。
    """
    environment = os.environ.copy()
    resource_roots = [run_root / "config-overlay/res"]
    # fixture 必须位于业务资产之前，才能稳定覆盖 entities.xml 与脚本；引擎公共资源仍保持最高的非配置优先级。
    # The fixture precedes game assets so its entity model is deterministic while shared engine resources remain available.
    if fixture_root is not None:
        resource_roots.append(fixture_root.resolve())
    resource_roots.extend(
        (
            repository_root / "kbe/res",
            assets_root,
            assets_root / "scripts",
            assets_root / "res",
        )
    )
    environment.update(
        {
            "KBE_ROOT": str(repository_root),
            "KBE_RES_PATH": os.pathsep.join(str(path) for path in resource_roots),
            "KBE_BIN_PATH": str(repository_root / "kbe/src/out/cmake/bin/Release"),
        }
    )
    return environment


def write_scenario_metadata(output_root: Path, scenario: dict[str, Any], bots: int) -> None:
    metadata = dict(scenario)
    metadata["configured_bots"] = bots
    (output_root / "scenario.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

