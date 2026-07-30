"""Create an isolated KBE configuration overlay. / 创建隔离的 KBE 配置覆盖层。"""

from __future__ import annotations

import json
import os
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


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

    destination = output_root / "config-overlay/res/server/kbengine.xml"
    destination.parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="utf-8", xml_declaration=True)
    return destination


def set_xml_value(parent: ET.Element, name: str, value: Any) -> None:
    node = parent.find(name)
    if node is None:
        node = ET.SubElement(parent, name)
    node.text = str(value)


def build_environment(repository_root: Path, assets_root: Path, run_root: Path) -> dict[str, str]:
    """Build isolated runtime paths without changing global environment.
    构造隔离运行路径，不修改全局环境。
    """
    environment = os.environ.copy()
    resource_roots = (
        run_root / "config-overlay/res",
        repository_root / "kbe/res",
        assets_root,
        assets_root / "scripts",
        assets_root / "res",
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

