"""验证真实双 Cell 调整边界时的实体交接。"""

from space_layout_runtime import captured_error, require, wait_for


def verify_boundary_move(cells, layout, base, bots, output):
    space, owner, version = layout["space"], layout["owner"], layout["version"]
    target_id = next(cid for cid in cells if cid != owner)
    source, target = cells[owner], cells[target_id]
    state = f"KBEngine.getSpaceLayout({space})"
    before = source.evaluate(state)
    require(before["boundaryRevision"] == 0 and
            len(before["boundaries"]) == 1 and before["boundaries"][0][:2] == [0, "X"],
            "初始边界及修订号不正确")
    boundary = before["boundaries"][0][2]
    shifted = boundary - 16.0

    def move(revision, x, index=0):
        return f"KBEngine.moveSpaceBoundary({space}, {version}, {revision}, {index}, {x!r})"

    def cell_ids(value):
        return [(cell["cellID"], cell["cellappID"]) for cell in value["cells"]]

    for expression in (move(1, shifted), move(0, shifted, 1), move(0, boundary),
                       f"KBEngine.moveSpaceBoundary({space}, {version}, 0, 0, float('nan'))"):
        error = captured_error(source, expression)
        require(error and error[0] == "ValueError", f"非法边界修改没有被拒绝：{expression}")
    error = captured_error(target, move(0, shifted))
    require(error and error[0] == "RuntimeError", "非属主可以提交边界修改")
    require(source.evaluate(state) == before, "请求被拒后改变了原布局")

    avatar, y, z = source.evaluate(
        f"next((e.id, e.position.y, e.position.z) for e in KBEngine.entities.values() "
        f"if type(e).__name__ == 'Avatar' and e.spaceID == {space} and e.hasWitness)"
    )
    avatar_position = (boundary - 10.0, y, z)
    npc_position = (boundary - 8.0, y, z)
    source.evaluate(f"setattr(KBEngine.entities[{avatar}], 'controlledBy', None)")
    source.evaluate(f"setattr(KBEngine.entities[{avatar}], 'position', {avatar_position!r})")

    def create(console, position):
        return console.evaluate(f"KBEngine.createEntity('NPC', {space}, {position!r}, (0., 0., 0.), {{}}).id")

    npc = create(source, npc_position)
    crossing = create(source, (boundary - 4.0, y, z))
    sentinel = create(target, (boundary + 3.0, y, z))
    route_query = (
        f"(lambda e: (str(e.cell), str(e.client), e.client is not None))(KBEngine.entities[{avatar}])"
    )
    _, client, connected = base.evaluate(route_query)
    require(connected, "缺少在线玩家连接")
    observed = (
        f"next(app.entities.get({sentinel}) for app in KBEngine.bots.values() if app.entities.get({avatar}))"
    )
    wait_for("调整前客户端看到对侧 NPC", lambda: bots.evaluate(f"bool({observed})"), bool)
    bots.evaluate(f"exec('_boundary_client_npc = {observed}')")
    source.evaluate(f"exec('_boundary_server_npc = KBEngine.entities[{sentinel}]')")
    present = f"(lambda e: e.isReal() if e else None)(KBEngine.entities.get({crossing}))"
    wait_for("交接探针收到 Ghost", lambda: target.evaluate(present), lambda real: real is False)

    restoring = output / "boundary-handoff-restoring"
    proceed = output / "boundary-handoff-proceed"
    source_setup = f"""
from NPC import NPC

def _boundary_test_event(self):
    pass

NPC._boundary_test_event = _boundary_test_event
_boundary_crossing_source = KBEngine.entities[{crossing}]
_boundary_crossing_source.registerEvent('_boundary_test_event', _boundary_crossing_source._boundary_test_event)
"""
    target_setup = f"""
from pathlib import Path
from time import monotonic, sleep
from NPC import NPC

def _boundary_test_event(self):
    return None

def _boundary_test_rebind(self):
    if self.id == {crossing}:
        Path({str(restoring)!r}).touch()
        deadline = monotonic() + 20
        while not Path({str(proceed)!r}).exists():
            if monotonic() >= deadline:
                raise TimeoutError('Boundary test handoff pause timed out')
            sleep(0.01)
    return _boundary_test_event.__get__(self, type(self))

NPC._boundary_test_event = property(_boundary_test_rebind)
"""
    source.evaluate(f"exec({source_setup!r})")
    target.evaluate(f"exec({target_setup!r})")
    source.evaluate(f"setattr(KBEngine.entities[{crossing}], 'position', {(boundary + 4.0, y, z)!r})")
    try:
        # 目标正恢复快照、尚未确认。源端要能先应用新边界，且不能误报全部完成。
        wait_for("目标暂停交接快照恢复", restoring.exists, bool, timeout=10)
        require(source.evaluate(present) is False, "探针没有在交接中转为源 Ghost")
        require(source.evaluate(move(0, shifted)) == 1, "未返回待确认的修订号")
        wait_for("源 Cell 先应用边界", lambda: source.evaluate(state),
                 lambda value: value["boundaryRevision"] == 1 and value["boundaryMovePending"],
                 timeout=8)
        error = captured_error(source, move(1, shifted - 1.0))
        require(error and error[0] == "RuntimeError", "上一轮未确认时仍可发起新修改")
    finally:
        proceed.touch()

    def confirm(revision, position):
        values = wait_for(
            "全部 Cell 确认边界",
            lambda: [console.evaluate(state) for console in (source, target)],
            lambda layouts: all(value["boundaryRevision"] == revision and not value["boundaryMovePending"]
                                for value in layouts),
        )
        for value in values:
            require(value["state"] == "active" and not value["boundaryMoveError"] and
                    value["version"] == version and value["resourceRevision"] == before["resourceRevision"] and
                    value["hasNavigation"] == before["hasNavigation"] and cell_ids(value) == cell_ids(before) and
                    value["boundaries"] == [[0, "X", position]],
                    "调界时改变了布局身份、资源或 Cell 编号")
        for console in (source, target):
            require(console.evaluate(f"KBEngine.cellForPosition({space}, {position!r}, {z!r})")[1] == target_id,
                    "分界线本身没有归右侧 Cell")

    def reals(console):
        return console.evaluate(f"{{e.id: (list(e.position), e.hasWitness) for e in KBEngine.entities.values() "
                                f"if e.spaceID == {space} and e.isReal() and e.id in ({avatar}, {npc}, {crossing})}}")

    def check_migration(host_id, position):
        host = cells[host_id]
        other = target if host is source else source

        def is_migrated(values):
            local, remote = values
            # 只比较同一实体，其他实体可能在不同 Tick 完成迁移。
            return all(str(eid) in local and str(eid) not in remote for eid in (avatar, npc))

        local, _ = wait_for("静止玩家和 NPC 按新边界迁移",
                            lambda: (reals(host), reals(other)), is_migrated)
        require(local[str(avatar)] == [list(avatar_position), True] and
                local[str(npc)][0] == list(npc_position) and
                host.evaluate(f"KBEngine.cellForPosition({space}, {position[0]!r}, {position[2]!r})")[1] == host_id,
                "迁移改变了静止实体的坐标、Space 或 Witness")
        current = wait_for("BaseApp 更新玩家路由", lambda: base.evaluate(route_query),
                           lambda value: f"cellapp[{host_id}]" in value[0])
        require(current[1:] == [client, True], "移动边界时替换了玩家连接")

    confirm(1, shifted)
    check_migration(target_id, avatar_position)
    # 源端可以再收到普通边界 Ghost，但不能留下原来等待释放的迁移对象。
    released = (f"(lambda e: e is None or (e is not _boundary_crossing_source and not e.isReal()))"
                f"(KBEngine.entities.get({crossing}))")
    wait_for("原交接正常释放源端", lambda: source.evaluate(released), bool)
    require(target.evaluate(present) is True, "新边界打断了原交接")
    require(source.evaluate(f"KBEngine.entities.get({sentinel}) is _boundary_server_npc"),
            "移动边界时重建了仍在同步范围内的 Ghost")
    require(bots.evaluate(f"({observed}) is _boundary_client_npc"), "移动边界时重建了客户端 AOI 实体")
    require(source.evaluate(move(1, boundary)) == 2, "第二次边界修改未开始")
    confirm(2, boundary)
    check_migration(owner, avatar_position)
    source.evaluate(f"(lambda e: setattr(e, 'controlledBy', e.base))(KBEngine.entities[{avatar}])")
    require(bots.evaluate(f"({observed}) is _boundary_client_npc"), "移回边界时客户端丢失原 AOI 实体")
    # 身份检查需要保留对象，结束后释放引用，让引擎照常检查关服对象泄漏。
    source.evaluate("exec('del _boundary_crossing_source, _boundary_server_npc')")
    bots.evaluate("exec('del _boundary_client_npc')")
    print(f"SPACE_BOUNDARY_MOVE_PASS space={space} version={version} revision=2", flush=True)
    return {"revision": 2, "player": avatar, "npc": npc, "in_flight_handoff": crossing,
            "retained_aoi_entity": sentinel}
