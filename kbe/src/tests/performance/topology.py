"""Parameterized local KBEngine topology. / 参数化的本地 KBEngine 拓扑。"""

from __future__ import annotations

from pathlib import Path


SINGLETON_COMPONENTS = (
    ("machine", 1000),
    ("logger", 2000),
    ("interfaces", 3000),
    ("dbmgr", 4000),
    ("baseappmgr", 5000),
    ("cellappmgr", 6000),
)


def build_local_cluster_components(
    binary_dir: Path,
    baseapp_count: int,
    cellapp_count: int,
) -> tuple[str, dict[str, list[int]]]:
    """Build a deterministic single-machine topology and Watcher ownership map.
    构造确定性的单机拓扑和 Watcher 组件归属表。
    """
    binary_dir = binary_dir.resolve()
    if baseapp_count < 1 or cellapp_count < 1:
        raise ValueError("baseapp and cellapp counts must be positive")

    declarations: list[tuple[str, int]] = list(SINGLETON_COMPONENTS)
    baseapp_ids = list(range(7001, 7001 + baseapp_count))
    cellapp_ids = list(range(8001, 8001 + cellapp_count))
    declarations.extend(("baseapp", component_id) for component_id in baseapp_ids)
    declarations.extend(("cellapp", component_id) for component_id in cellapp_ids)
    declarations.append(("loginapp", 9000))

    specs: list[str] = []
    for global_order, (name, component_id) in enumerate(declarations, start=1):
        executable = binary_dir / f"{name}.exe"
        if not executable.is_file():
            raise FileNotFoundError(f"server component is missing: {executable}")
        specs.append(f"{name}::{executable}::{component_id}::{global_order}")

    return "|".join(specs), {"baseapp": baseapp_ids, "cellapp": cellapp_ids}


def partition_batch_size(batch_size: int, process_count: int) -> int:
    """Convert an aggregate Bots batch into an equal per-process batch.
    将总 Bots 批次转换为各进程相等的批次。
    """
    if batch_size < 1 or process_count < 1:
        raise ValueError("batch size and process count must be positive")
    if batch_size % process_count != 0:
        raise ValueError("aggregate bots batch size must be divisible by bots process count")
    return batch_size // process_count
