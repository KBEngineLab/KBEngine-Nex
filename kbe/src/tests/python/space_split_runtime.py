"""在真实集群中验证新增 Cell、更新 Layout、同步资源和迁移实体。"""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import json
import subprocess
import sys
import time

from space_layout_runtime import captured_error, require, wait_for


def space_command(repository, endpoint, environment, space):
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

    return command


def verify_split_boundary_move(cells, space, owner, target, state, current, cut, base, route, client, avatar, npc):
    participants = {row["cellappID"]: row["cellID"] for row in current["cells"]}
    split_index, = (index for index, axis, at in current["boundaries"] if axis == "X" and at == cut)
    positions = cells[target].evaluate(
        f"[list(KBEngine.entities[i].position) for i in {(avatar, npc)!r}]"
    )
    reals = (
        f"[(e.id, list(e.position), e.hasWitness) for i in {(avatar, npc)!r} "
        f"for e in (KBEngine.entities.get(i),) if e and e.spaceID == {space} and e.isReal()]"
    )
    version, revision = current["version"], current["boundaryRevision"]
    moves = []
    for at, host in ((cut + 10.0, owner), (cut, target)):
        started = time.monotonic()
        request = f"KBEngine.moveSpaceBoundary({space}, {version}, {revision}, {split_index}, {at!r})"
        revision += 1
        require(cells[owner].evaluate(request) == revision, "扩容后的调界未开始")
        views = wait_for(
            "扩容后的全部 Cell 确认调界",
            lambda: [cells[cid].evaluate(state) for cid in participants],
            lambda layouts: all(row and row["boundaryRevision"] == revision and not row["boundaryMovePending"]
                                for row in layouts),
        )
        bounds = [[index, axis, at if index == split_index else position]
                  for index, axis, position in current["boundaries"]]
        for row in views:
            require(row["state"] == "active" and row["version"] == version and
                    not row["boundaryMoveError"] and row["boundaries"] == bounds and
                    row["resourceRevision"] == current["resourceRevision"] and
                    row["hasNavigation"] == current["hasNavigation"] and
                    {cell["cellappID"]: cell["cellID"] for cell in row["cells"]} == participants,
                    "扩容后的调界改变了其他切分坐标、Cell 编号或资源")
        expected = {cid: [] for cid in cells}
        expected[host] = [[avatar, positions[0], True], [npc, positions[1], False]]
        wait_for(
            "静止实体按新切分坐标交接",
            lambda: {cid: console.evaluate(reals) for cid, console in cells.items()},
            lambda rows: rows == expected,
        )
        routed = wait_for("调界后 Base 路由切换", lambda: base.evaluate(route),
                          lambda row: f"cellapp[{host}]" in row[0])
        require(routed[1] == client, "调界更换了在线玩家客户端连接")
        moves.append({"revision": revision, "at": at, "target": host,
                      "elapsed_seconds": round(time.monotonic() - started, 3)})
    return moves


def verify_space_split(cells, layout, base, repository, endpoint, environment, output):
    space, owner, version = layout["space"], layout["owner"], layout["version"]
    old = {cell["cellappID"]: cell["cellID"] for cell in layout["cells"]}
    spares = set(cells) - old.keys()
    source = cells[owner]
    state = f"KBEngine.getSpaceLayout({space})"
    boundary = layout["split_x"]
    cut = boundary - 16.0
    command = space_command(repository, endpoint, environment, space)

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
    moves = verify_split_boundary_move(
        cells, space, owner, target_id, state, current, cut, base, route_query, client, avatar, npc,
    )
    target.evaluate(f"KBEngine.entities[{npc}].destroy()")
    print(f"SPACE_SPLIT_RUNTIME_PASS space={space} target={target_id}", flush=True)
    return dict(attempt=after["splitAttempt"], cell=cell_apps[target_id], target=target_id,
                original_layout_served=True, preparation_timeout_retried=True,
                resources_current=True, migration_complete=True, boundary_moves=moves)


def verify_space_merge(cells, layout, split, base, repository, endpoint, environment, output):
    space, owner = layout["space"], layout["owner"]
    state = f"KBEngine.getSpaceLayout({space})"
    command = space_command(repository, endpoint, environment, space)
    before = command("show")
    original = {cell["cellappID"]: cell["cellID"] for cell in layout["cells"]}
    peer, = original.keys() - {owner}
    source_id, retiring = split["target"], split["cell"]
    source, target = cells[source_id], cells[owner]
    keeping = original[owner]
    cut = next(row["bounds"][0] for row in before["cells"] if row["cell"] == retiring)
    avatar, y, z = source.evaluate(
        f"next((e.id, e.position.y, e.position.z) for e in KBEngine.entities.values() "
        f"if type(e).__name__ == 'Avatar' and e.spaceID == {space} and e.isReal())"
    )
    route = f"(lambda e: (str(e.cell), str(e.client)))(KBEngine.entities[{avatar}])"
    _, client = base.evaluate(route)

    def create_at(x):
        return f"KBEngine.createEntity('NPC', {space}, {(x, y, z)!r}, (0., 0., 0.), {{}}).id"

    def has_real(console, entity):
        return console.evaluate(f"bool(KBEngine.entities.get({entity}) and KBEngine.entities[{entity}].isReal())")

    def merge(retire, keep, error=None):
        return command("merge", "--retire", str(retire), "--keep", str(keep), error=error)

    merge(keeping, retiring, error="must keep the Space owner")
    merge(retiring, original[peer], error="sibling leaf Cells")
    require(command("show") == before, "非法合并改变了原 Layout")
    npc = source.evaluate(create_at(cut + 1.0))
    entered = output / "merge-target-entered"
    proceed = output / "merge-target-proceed"
    setup = f"""
from pathlib import Path
from time import monotonic, sleep
from NPC import NPC

_merge_previous_enter = NPC.__dict__.get('onEnteredCell')
_merge_enter_count = 0
def _merge_enter(self, *args):
    global _merge_enter_count
    if self.id == {npc}:
        _merge_enter_count += 1
        Path({str(entered)!r}).touch()
        deadline = monotonic() + 20
        while not Path({str(proceed)!r}).exists() and monotonic() < deadline:
            sleep(.02)
    if _merge_previous_enter:
        _merge_previous_enter(self, *args)

NPC.onEnteredCell = _merge_enter
"""
    target.evaluate(f"exec({setup!r})")
    try:
        request = merge(retiring, keeping)
        require(request["mergeState"] == "preparing" and request["cells"] == before["cells"],
                "准备合并时提前发布了新 Layout")
        wait_for("目标派发迁入回调前已释放源端", entered.exists, bool, timeout=20)
        wait_for("退役 Cell 已释放原 NPC", lambda: has_real(source, npc), lambda real: not real)
        draining = command("show")
        require(draining["mergeState"] == "draining" and draining["boundaryRevision"] == before["boundaryRevision"]
                and len(draining["cells"]) == 3 and source.evaluate(state) is not None,
                "目标仍在处理交接时提前发布了新 Layout")
        error = captured_error(source, create_at(cut + 1.0))
        require(error and error[0] == "RuntimeError", "退役 Cell 仍在接收新建实体")
        merge(retiring, keeping, error="another layout adjustment")
    finally:
        proceed.touch()

    def wait_merge_done(revision, count):
        result = wait_for("等待 Cell 合并及回收结束", lambda: command("show"),
                          lambda row: row["mergeState"] == "complete")
        require(result["version"] == before["version"] and result["boundaryRevision"] == revision and
                len(result["cells"]) == count, "合并修改了 Space 版本或留下多余 Cell")
        return result

    merged = wait_merge_done(before["boundaryRevision"] + 1, 2)
    require({item["cellapp"]: item["cell"] for item in merged["cells"]} == original and source.evaluate(state) is None,
            "旧 Cell 未真正回收，或其他 Cell 编号被改变")
    require(not source.evaluate(f"any(e.spaceID == {space} for e in KBEngine.entities.values())"),
            "退役 Cell 留下了本次 Space 的实体副本")
    require(has_real(target, npc) and has_real(target, avatar) and target.evaluate("_merge_enter_count") == 1,
            "迁出没有保留 Real，或重复派发进入回调")
    routed = base.evaluate(route)
    require(f"cellapp[{owner}]" in routed[0] and routed[1] == client, "合并修改了玩家连接或没有切换路由")
    target.evaluate(f"KBEngine.entities[{npc}].destroy()")
    target.evaluate("exec('if _merge_previous_enter is None:\\n    del NPC.onEnteredCell\\n"
                    "else:\\n    NPC.onEnteredCell = _merge_previous_enter')")

    # 原右侧范围没有上界；这个 NPC 超出保留 Cell 原来的 Ghost 距离。
    far_x = layout["split_x"] + 10000
    far_source = cells[peer]
    far_npc = far_source.evaluate(create_at(far_x))
    require(not target.evaluate(f"{far_npc} in KBEngine.entities"), "远处 NPC 已有 Ghost，无法验证退役复制")

    def set_far_name(value):
        far_source.evaluate(f"setattr(KBEngine.entities[{far_npc}], 'name', {value})")

    def wait_merge_failed(request):
        failed = wait_for("超限 Ghost 取消合并", lambda: command("show"),
                          lambda row: row["mergeState"] == "failed")
        require(failed["state"] == "active" and failed["version"] == merged["version"] and
                failed["boundaryRevision"] == merged["boundaryRevision"] and failed["cells"] == merged["cells"] and
                failed["mergeAttempt"] == request["mergeAttempt"] and
                f"entity {far_npc}: target Ghost snapshot exceeds 256 KiB" in failed["mergeError"],
                "拒绝超限 Ghost 后修改了原 Layout，或没有报告实体和上限")
        require(has_real(far_source, far_npc) and not has_real(target, far_npc), "合并失败后实体归属错误")
        created = far_source.evaluate(create_at(far_x))
        far_source.evaluate(f"KBEngine.entities[{created}].destroy()")

    set_far_name("'N' * (270 * 1024)")
    wait_merge_failed(merge(original[peer], keeping))
    set_far_name("'merge-probe'")

    boundary = next(row["bounds"][0] for row in merged["cells"] if row["cellapp"] == peer)
    target.evaluate(f"setattr(KBEngine.entities[{avatar}], 'controlledBy', None)")
    target.evaluate(f"setattr(KBEngine.entities[{avatar}], 'position', {(boundary + 8, y, z)!r})")
    wait_for("玩家迁入待退役 Cell", lambda: base.evaluate(route), lambda row: f"cellapp[{peer}]" in row[0])
    wait_for("玩家交接完成且原 Cell 已有 Ghost",
             lambda: (has_real(far_source, avatar), target.evaluate(
                 f"bool(KBEngine.entities.get({avatar}) and not KBEngine.entities[{avatar}].isReal())")),
             all)

    @contextmanager
    def pause(console, label):
        held = output / f"merge-oversize-{label}-held"
        release = output / f"merge-oversize-{label}-release"
        hold = f"""
from pathlib import Path
from time import monotonic, sleep
Path({str(held)!r}).touch()
_merge_wait_until = monotonic() + 20
while not Path({str(release)!r}).exists() and monotonic() < _merge_wait_until:
    sleep(.02)
"""
        with ThreadPoolExecutor(max_workers=1) as executor:
            waiting = executor.submit(console.evaluate, f"exec({hold!r})", 25)
            try:
                wait_for("等待进程暂停处理消息", held.exists, bool)
                yield
            finally:
                release.touch()
            waiting.result()

    manager_log = output / "server/logs/logger_cellappmgr.log"

    def manager_logged(text):
        return text in manager_log.read_text(encoding="utf-8", errors="replace")

    with pause(base, "base"):
        # 等退役端已检查远处 NPC，再增大属性，并在 BaseApp 暂停期间发起玩家越界。
        with pause(target, "peer"):
            request = merge(original[peer], keeping)
            ready_line = f"merge ready, space={space}, attempt={request['mergeAttempt']}, cellapp={peer}."
            wait_for("退役端完成快照检查", lambda: manager_logged(ready_line), bool, timeout=8)
            far_source.evaluate(f"setattr(KBEngine.entities[{avatar}], 'position', {(boundary - 8, y, z)!r})")
            set_far_name("'N' * (270 * 1024)")
            require(command("show")["mergeState"] == "preparing", "保留端暂停时意外开始排空")
        cancelling = wait_for("排空取消等待在途玩家交接", lambda: command("show"),
                              lambda row: row["mergeState"] == "cancelling", timeout=8)
        require(cancelling["boundaryRevision"] == merged["boundaryRevision"] and has_real(far_source, avatar),
                "交接确认前改变了 Layout 或丢失了玩家 Real")
        error = captured_error(far_source, create_at(far_x))
        require(error and error[0] == "RuntimeError" and command("show")["mergeState"] == "cancelling",
                "在途交接结束前恢复了退役 Cell 创建或报告了失败")
    wait_merge_failed(request)
    routed = base.evaluate(route)
    require(f"cellapp[{owner}]" in routed[0] and routed[1] == client and has_real(target, avatar) and
            not has_real(far_source, avatar), "取消合并时没有正确完成在途玩家交接")
    target.evaluate(f"(lambda e: setattr(e, 'controlledBy', e.base))(KBEngine.entities[{avatar}])")

    set_far_name("'merge-probe'")
    merge(original[peer], keeping)
    single = wait_merge_done(before["boundaryRevision"] + 2, 1)
    require(single["cells"] == [{"cell": keeping, "cellapp": owner, "bounds": [None] * 4}] and
            has_real(target, far_npc) and cells[peer].evaluate(state) is None,
            "最后两个 Cell 合并后没有保留全部范围或远处实体")
    target.evaluate(f"KBEngine.entities[{far_npc}].destroy()")

    # 再切分原区域，确认回收后的编号不会被重新分配。
    command("split", "--cell", str(keeping), "--axis", "x", "--at", str(layout["split_x"] + 1))
    added = wait_for("缩容后再次扩容", lambda: command("show"),
                     lambda row: row["splitState"] == "active" and row["migrationState"] == "complete")
    new_cell, = (item["cell"] for item in added["cells"] if item["cell"] != keeping)
    require(new_cell > retiring, "缩容后扩容复用了已退役的 Cell 编号")
    print(f"SPACE_MERGE_RUNTIME_PASS space={space} retired={retiring} new={new_cell}", flush=True)
    return {"retired": retiring, "kept": keeping, "reexpanded": new_cell, "target_callback_once": True,
            "refused_creation_during_drain": True, "moved_npc_outside_ghost_distance": True,
            "oversized_ghost_rejected_before_drain": True, "oversized_ghost_cancelled_during_drain": True,
            "inflight_player_handoff_completed_during_cancellation": True,
            "creation_and_merge_restored_after_cancellation": True}
