"""在真实集群中验证新增 Cell、更新 Layout、同步资源和迁移实体。"""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import json
import subprocess
import sys

from space_layout_runtime import require, wait_for


def verify_space_split(cells, layout, base, repository, endpoint, environment, output):
    space, owner, version = layout["space"], layout["owner"], layout["version"]
    old = {cell["cellappID"]: cell["cellID"] for cell in layout["cells"]}
    spares = set(cells) - old.keys()
    source = cells[owner]
    state = f"KBEngine.getSpaceLayout({space})"
    boundary = layout["split_x"]
    cut = boundary - 16.0
    tool = repository / "kbe/tools/server/pycluster/cluster_controller.py"

    def command(action, *options, error=None):
        result = subprocess.run(
            [sys.executable, str(tool), "space", action, "--manager", endpoint, "--space", str(space), *options],
            env={**environment, "PYTHONIOENCODING": "utf-8"},
            capture_output=True, encoding="utf-8", timeout=10,
        )
        if error:
            require(result.returncode == 1 and error in result.stderr, f"管理命令未明确拒绝：{result.stderr}")
            return None
        require(result.returncode == 0, f"管理命令失败：{result.stderr}")
        return json.loads(result.stdout)

    options = ("--cell", str(old[owner]), "--axis", "x", "--at")
    command("split", *options, str(boundary), error="invalid Cell")
    require(source.evaluate(f"KBEngine.moveSpaceBoundary({space}, {version}, 0, 0, {boundary + 1!r})") == 1,
            "扩容前调界未开始")
    wait_for("扩容前调界确认", lambda: source.evaluate(state),
             lambda row: row["boundaryRevision"] == 1 and not row["boundaryMovePending"])
    avatar, y, z = source.evaluate(
        f"next((e.id, e.position.y, e.position.z) for e in KBEngine.entities.values() "
        f"if type(e).__name__ == 'Avatar' and e.spaceID == {space} and e.isReal())"
    )
    source.evaluate(f"setattr(KBEngine.entities[{avatar}], 'controlledBy', None)")
    source.evaluate(f"setattr(KBEngine.entities[{avatar}], 'position', {(cut + 5.0, y, z)!r})")
    npc = source.evaluate(
        f"KBEngine.createEntity('NPC', {space}, {(cut + 4.0, y, z)!r}, (0., 0., 0.), {{}}).id"
    )
    route_query = f"(lambda e: (str(e.cell), str(e.client)))(KBEngine.entities[{avatar}])"
    _, client = base.evaluate(route_query)

    @contextmanager
    def pause_candidates(label, seconds):
        release = output / f"split-{label}-release"
        marks = {cid: output / f"split-{label}-{cid}-paused" for cid in spares}
        with ThreadPoolExecutor(max_workers=len(spares)) as executor:
            held = []
            try:
                for cid, mark in marks.items():
                    script = (
                        "from pathlib import Path\nfrom time import monotonic, sleep\n"
                        f"Path({str(mark)!r}).touch()\n"
                        f"_split_release = Path({str(release)!r})\n"
                        f"_split_deadline = monotonic() + {seconds}\n"
                        "while not _split_release.exists() and monotonic() < _split_deadline:\n"
                        "    sleep(.02)\n"
                    )
                    held.append(executor.submit(cells[cid].evaluate, f"exec({script!r})", seconds + 5))
                wait_for("候选 CellApp 暂停准备", lambda: all(mark.exists() for mark in marks.values()), bool)
                yield
            finally:
                release.touch()
            for future in held:
                future.result()

    with pause_candidates("timeout", 40):
        attempt = command("split", *options, str(cut))["splitAttempt"]
        cancelled = wait_for("准备超时，管理器取消扩容", lambda: command("show"),
                             lambda row: row["splitState"] == "failed", timeout=37)
        require(cancelled["state"] == "active" and cancelled["boundaryRevision"] == 1 and
                len(cancelled["cells"]) == len(old) and "timed out" in cancelled["splitError"],
                "准备超时影响了原布局")
    wait_for("候选清理已取消的临时 Space",
             lambda: all(cells[cid].evaluate(state) is None for cid in spares), bool)

    with pause_candidates("prepare", 20):
        request = command("split", *options, str(cut))
        require(request["splitState"] == "preparing" and request["boundaryRevision"] == 1 and
                request["splitAttempt"] == attempt + 1 and len(request["cells"]) == len(old),
                "候选未就绪时已经改变布局，或重试未分配新编号")
        command("split", *options, str(cut), error="another layout adjustment")
        require(source.evaluate(f"all(KBEngine.entities[i].isReal() for i in {(avatar, npc)!r})"),
                "准备时原 Cell 丢失 Real")
        revision = source.evaluate(state)["resourceRevision"] + 1
        source.evaluate(f"KBEngine.setSpaceData({space}, 'split_preparing', 'still-active')")
        def has_current_data(console):
            return console.evaluate(
                f"(lambda row: row and row['state'] == 'active' and row['resourceRevision'] >= {revision} "
                f"and KBEngine.getSpaceData({space}, 'split_preparing') == 'still-active')({state})"
            )

        wait_for("原布局继续同步 SpaceData", lambda: all(has_current_data(cells[cid]) for cid in old), bool)
        require(command("show")["splitState"] == "preparing", "待准备布局提前发布")

    after = wait_for("管理器确认新增 Cell", lambda: command("show"),
                     lambda row: row["splitState"] == "active" and len(row["cells"]) == len(old) + 1)
    cell_apps = {row["cellapp"]: row["cell"] for row in after["cells"]}
    target_id, = cell_apps.keys() - old.keys()
    require(target_id in spares and {cid: cell_apps[cid] for cid in old} == old and
            after["version"] == version and after["boundaryRevision"] == 2,
            "扩容修改了已有编号、布局版本或错误地复用了边界修订号")
    target = cells[target_id]
    wait_for("新增 Cell 同步导航和准备期间的 SpaceData", lambda: has_current_data(target), bool)
    current = target.evaluate(state)
    require(current["hasNavigation"] and current["boundaryRevision"] == 2, "新增 Cell 的导航或边界不正确")
    wait_for("静止 NPC 和在线玩家迁到新增 Cell",
             lambda: target.evaluate(
                 f"all(KBEngine.entities.get(i) and KBEngine.entities[i].isReal() for i in {(avatar, npc)!r})"),
             bool)
    routed = wait_for("在线玩家 Base 路由迁到新增 Cell", lambda: base.evaluate(route_query),
                      lambda value: f"cellapp[{target_id}]" in value[0])
    require(routed[1] == client, "扩容更换了在线玩家客户端连接")
    wait_for("源端和目标均完成迁移", lambda: command("show"), lambda row: row["migrationState"] == "complete")
    require(source.evaluate(f"not any(KBEngine.entities.get(i) and KBEngine.entities[i].isReal() "
                            f"for i in {(avatar, npc)!r})"), "源端留下重复 Real")
    target.evaluate(f"KBEngine.entities[{npc}].destroy()")
    print(f"SPACE_SPLIT_RUNTIME_PASS space={space} target={target_id}", flush=True)
    return dict(attempt=after["splitAttempt"], cell=cell_apps[target_id], target=target_id,
                original_layout_served=True, preparation_timeout_retried=True,
                resources_current=True, migration_complete=True)
