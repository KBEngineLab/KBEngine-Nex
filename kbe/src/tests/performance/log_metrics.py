"""Incremental error-pattern collector. / 增量错误模式采集器。"""

from __future__ import annotations

import re
from pathlib import Path


DEFAULT_PATTERNS = {
    "error": re.compile(r"\bERROR\b|\[ERROR\]", re.IGNORECASE),
    "warning": re.compile(r"\bWARN(?:ING)?\b|\[WARNING\]", re.IGNORECASE),
    "not_found_msgid": re.compile(r"not found msgID", re.IGNORECASE),
    "resource_unavailable": re.compile(r"REASON_RESOURCE_UNAVAILABLE", re.IGNORECASE),
    "abnormal_exit": re.compile(r"Abnormal exit|abnormal exit", re.IGNORECASE),
}


class IncrementalLogCollector:
    READ_CHUNK_CHARS = 1024 * 1024

    def __init__(self, root: Path):
        self.root = root
        self._offsets: dict[Path, int] = {}
        self._counts = {name: 0 for name in DEFAULT_PATTERNS}

    def sample(self) -> dict[str, int]:
        # 每条新日志只在一个采样周期内计数；报告会累加这些增量。
        # Count each new log line in exactly one interval; the report aggregates these deltas.
        self._counts = {name: 0 for name in DEFAULT_PATTERNS}
        if not self.root.exists():
            return dict(self._counts)
        for path in self.root.rglob("*.log"):
            self._read_new(path)
        return dict(self._counts)

    def _read_new(self, path: Path) -> None:
        try:
            size = path.stat().st_size
            offset = self._offsets.get(path, 0)
            if size < offset:
                offset = 0
            with path.open("r", encoding="utf-8", errors="replace") as stream:
                stream.seek(offset)
                carry = ""
                while True:
                    chunk = stream.read(self.READ_CHUNK_CHARS)
                    if not chunk:
                        break
                    text = carry + chunk
                    newline = max(text.rfind("\n"), text.rfind("\r"))
                    if newline < 0:
                        carry = text
                        continue
                    self._count_text(text[: newline + 1])
                    carry = text[newline + 1 :]
                if carry:
                    self._count_text(carry)
                self._offsets[path] = stream.tell()
        except OSError:
            return

    def _count_text(self, text: str) -> None:
        for name, pattern in DEFAULT_PATTERNS.items():
            self._counts[name] += len(pattern.findall(text))

