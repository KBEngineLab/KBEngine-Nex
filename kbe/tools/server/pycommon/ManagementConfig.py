# -*- coding: utf-8 -*-

import os
import xml.etree.ElementTree as ET
from pathlib import Path


def _resource_roots():
	"""Return resource roots in the same precedence order used by the engine.
	按引擎一致的优先级返回资源目录，后出现的配置用于覆盖 defaults。
	"""
	roots = []
	for item in os.environ.get("KBE_RES_PATH", "").split(os.pathsep):
		item = item.strip()
		if item:
			roots.append(Path(item))

	engine_res = Path(__file__).resolve().parents[3] / "res"
	if engine_res not in roots:
		roots.append(engine_res)
	return roots


def _read_token(roots, names):
	for root in roots:
		for name in names:
			candidate = root / name
			if not candidate.is_file():
				continue
			try:
				tree = ET.parse(str(candidate))
			except (OSError, ET.ParseError):
				continue

			node = tree.getroot().find("./management/adminToken")
			if node is not None:
				return (node.text or "").strip()
	return None


def resolve_admin_token():
	"""Resolve management.adminToken using defaults followed by project override.
	按 defaults 后项目配置的顺序解析 management.adminToken。
	"""
	roots = _resource_roots()
	token = _read_token(roots, ("server/kbengine_defaults.xml", "kbengine_defaults.xml"))
	override = _read_token(roots, ("server/kbengine.xml", "kbengine.xml"))
	return (token or "") if override is None else override
