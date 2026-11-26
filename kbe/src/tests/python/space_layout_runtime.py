"""在真实 CellApp 控制台验证固定区域、资源同步和逻辑 Space 销毁。"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import math
import time

NO_NAVIGATION_MAPPING = "space_layout_no_navigation"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(description, action, ready, timeout=45):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = action()
        if ready(last):
            return last
        time.sleep(0.2)
    raise TimeoutError(f"{description}超时，最后结果：{last!r}")


def captured_error(console, expression):
    # 调用正式引擎接口，只捕获负面场景的异常，避免把预期拒绝写成未处理脚本错误。
    script = (
        "try:\n"
        f"    {expression}\n"
        "    _space_layout_error = None\n"
        "except Exception as error:\n"
        "    _space_layout_error = [type(error).__name__, str(error)]\n"
    )
    return console.evaluate(f"(exec({script!r}), _space_layout_error)[1]")


def verify_fixed_layout(cells, source_space, enabled, rejection_logged):
    space_id = source_space["space"]
    owner = source_space["cid"]
    source = cells[owner]
    others = [cid for cid in cells if cid != owner]
    positions = source.evaluate(
        f"[list(e.position) for e in KBEngine.entities.values() "
        f"if e.spaceID == {space_id} and e.isReal()]"
    )
    require(positions, "布局检查缺少源端 Real 实体")
    split_x = max(position[0] for position in positions) + 256.0
    split_z = max(position[2] for position in positions) + 256.0
    if len(cells) == 4:
        specification = (
            "X", split_x, ("Z", split_z, owner, others[0]),
            ("Z", split_z, others[1], others[2]),
        )
    else:
        specification = ("X", split_x, owner, others[0])
    if not enabled:
        error = captured_error(source, f"KBEngine.setSpaceLayout({space_id}, {specification!r})")
        require(error and error[0] == "RuntimeError" and "disabled" in error[1],
                "关闭分割时固定布局接口没有明确拒绝")
        error = captured_error(source, f"KBEngine.moveSpaceBoundary({space_id}, 1, 0, 0, {split_x!r})")
        require(error and error[0] == "RuntimeError" and "must both be enabled" in error[1],
                "关闭分割时边界调整没有明确拒绝")
        require(all(cells[cid].evaluate(f"KBEngine.getSpaceLayout({space_id})") is None
                    for cid in others), "关闭分割时创建了远端区域")
        print("SPACE_LAYOUT_DISABLED_PASS", flush=True)
        return {"enabled": False, "creation_rejected": True}, None

    bad_layouts = (
        ("X", split_x, owner, owner),
        ("X", min(position[0] for position in positions) - 256.0, owner, others[0]),
    )
    for bad in bad_layouts:
        error = captured_error(source, f"KBEngine.setSpaceLayout({space_id}, {bad!r})")
        require(error and error[0] in ("ValueError", "RuntimeError"),
                "重复节点或需要移动现有 Real 的布局没有明确拒绝")
    error = captured_error(
        source, f"KBEngine.setSpaceLayout({space_id}, ('X', float('nan'), {owner}, {others[0]}))"
    )
    require(error and error[0] == "ValueError", "非法浮点边界未被拒绝")
    rejected_version = source.evaluate(
        f"KBEngine.setSpaceLayout({space_id}, ('X', {split_x!r}, {owner}, 999999))"
    )
    failed = wait_for(
        "无效目标节点拒绝", lambda: source.evaluate(f"KBEngine.getSpaceLayout({space_id})"),
        lambda result: result and result["state"] == "failed",
    )
    require(failed["version"] == rejected_version and "not ready" in failed["error"],
            "管理器没有返回准确的目标节点拒绝结果")
    require(all(cells[cid].evaluate(f"KBEngine.getSpaceLayout({space_id})") is None
                for cid in others), "被拒绝的布局留下了远端区域")
    original_mapping = source.evaluate(f"KBEngine.getSpaceGeometryMapping({space_id})")
    # 只观察引擎实际触发的脚本回调，不替换业务函数或手动调用回调。
    profile = (
        "import sys\n"
        "_space_geometry_calls = []\n"
        "def _space_geometry_profile(frame, event, arg):\n"
        f"    if (event == 'call' and frame.f_locals.get('spaceID') == {space_id}\n"
        f"            and frame.f_locals.get('mapping') == {NO_NAVIGATION_MAPPING!r}\n"
        "            and frame.f_code.co_name in ('onSpaceGeometryLoaded', 'onAllSpaceGeometryLoaded')):\n"
        "        _space_geometry_calls.append(frame.f_code.co_name)\n"
        "sys.setprofile(_space_geometry_profile)\n"
    )
    source.evaluate(f"exec({profile!r})")
    try:
        source.evaluate(
            f"KBEngine.addSpaceGeometryMapping({space_id}, None, {NO_NAVIGATION_MAPPING!r}, "
            "True, {0: 'missing.bin'})"
        )
        wait_for(
            "无导航的几何加载回调",
            lambda: source.evaluate(
                f"(KBEngine.getSpaceLayout({space_id}), _space_geometry_calls)"
            ),
            lambda value: value[0]["resourcesReady"] and not value[0]["hasNavigation"]
            and value[1] == ["onSpaceGeometryLoaded", "onAllSpaceGeometryLoaded"],
        )
    finally:
        source.evaluate("__import__('sys').setprofile(None)")
    source.evaluate(f"KBEngine.setSpaceData({space_id}, 'layout_initial', 'initial')")
    version = source.evaluate(f"KBEngine.setSpaceLayout({space_id}, {specification!r})")
    require(version > rejected_version, "重试布局没有使用新的版本")

    def layouts():
        return {cid: console.evaluate(f"KBEngine.getSpaceLayout({space_id})")
                for cid, console in cells.items()}

    confirmed = wait_for(
        "全部 Cell 布局启用", layouts,
        lambda state: all(value and value["state"] == "active"
                          and value["version"] == version and value["resourcesReady"]
                          for value in state.values()),
    )
    first = confirmed[owner]
    require(len(first["cells"]) == len(cells), "布局 Cell 数与目标进程数不一致")
    require(len({value["localCellID"] for value in confirmed.values()}) == len(cells),
            "区域编号重复")
    require(all(value["cells"] == first["cells"] and value["ownerCellAppID"] == owner
                for value in confirmed.values()), "各节点的区域布局不一致")
    require(all(not value["hasNavigation"] for value in confirmed.values()),
            "导航不可用时布局应就绪，但不能报告存在导航句柄")
    require(all(value["localCellID"] != cid for cid, value in confirmed.items()),
            "区域编号仍然使用进程编号")

    probes = [(split_x - 1, split_z - 1, owner)]
    if len(cells) == 4:
        probes.extend(((split_x - 1, split_z, others[0]),
                       (split_x, split_z - 1, others[1]), (split_x, split_z, others[2])))
    else:
        probes.append((split_x, split_z, others[0]))
    for x, z, expected_owner in probes:
        answers = [console.evaluate(f"KBEngine.cellForPosition({space_id}, {x!r}, {z!r})")
                   for console in cells.values()]
        require(all(answer == answers[0] and answer[1] == expected_owner for answer in answers),
                "边界或交汇处的区域归属不一致")
    error = captured_error(
        source, f"KBEngine.cellForPosition({space_id}, float('inf'), 0.0)"
    )
    require(error and error[0] == "ValueError", "非法查询坐标没有被拒绝")

    mappings = [console.evaluate(f"KBEngine.getSpaceGeometryMapping({space_id})")
                for console in cells.values()]
    require(all(path == NO_NAVIGATION_MAPPING for path in mappings), "各节点的几何映射不一致")
    require(all(console.evaluate(f"KBEngine.getSpaceData({space_id}, 'layout_initial')") == "initial"
                for console in cells.values()), "初始 SpaceData 未完整复制")
    owner_counts = [
        console.evaluate(
            f"sum(e.isReal() and e.spaceID == {space_id} for e in KBEngine.spaces().values())"
        ) for console in cells.values()
    ]
    require(sum(owner_counts) == 1, "远端创建了重复的 Space 业务实体")
    require(all(cells[cid].evaluate(
        f"sum(e.isReal() for e in KBEngine.entitiesForSpace({space_id}).values())") == 0
                for cid in others), "新区域不应自动创建 Real")
    for cid, console in cells.items():
        console.evaluate(f"KBEngine.setSpaceData({space_id}, 'layout_writer_{cid}', '{cid}')")
    # 先等管理器返回相应版本，再读取键，避免把合法的异步更新当作缺失数据。
    wait_for(
        "SpaceData 更新版本同步", layouts,
        lambda state: all(value and value["resourceRevision"] == 1 + len(cells)
                          for value in state.values()),
    )
    for console in cells.values():
        values = console.evaluate(
            f"[KBEngine.getSpaceData({space_id}, 'layout_writer_' + str(cid)) for cid in {list(cells)!r}]"
        )
        require(values == [str(cid) for cid in cells], "跨 Cell SpaceData 写入内容不一致")
    cells[others[0]].evaluate(f"KBEngine.delSpaceData({space_id}, 'layout_initial')")
    wait_for(
        "SpaceData 删除版本同步", layouts,
        lambda state: all(value and value["resourceRevision"] == 2 + len(cells)
                          for value in state.values()),
    )
    avatar_positions = source.evaluate(
        f"[list(e.position) for e in KBEngine.entities.values() "
        f"if type(e).__name__ == 'Avatar' and e.spaceID == {space_id} and e.isReal()]"
    )
    require(avatar_positions, "导航层检查缺少实际场景中的角色位置")
    source.evaluate(
        f"KBEngine.addSpaceGeometryMapping({space_id}, None, {original_mapping!r}, True, {{0: 'navmesh.bin'}})"
    )
    restored_revision = 3 + len(cells)
    wait_for(
        "恢复有效导航", layouts,
        lambda state: all(value and value["state"] == "active" and value["resourcesReady"]
                          and value["hasNavigation"] and value["resourceRevision"] == restored_revision
                          for value in state.values()),
    )
    source.evaluate(
        f"KBEngine.addSpaceGeometryMapping({space_id}, None, {original_mapping!r}, True, "
        "{0: 'navmesh.bin', 17: 'navmesh.bin'})"
    )
    final_resources = wait_for(
        "相同路径的导航层配置同步", layouts,
        lambda state: all(value and value["state"] == "active" and value["resourcesReady"]
                          and value["hasNavigation"] and value["resourceRevision"] == restored_revision + 1
                          for value in state.values()),
    )
    point = avatar_positions[0]
    ray_start = (point[0], point[1] + 5.0, point[2])
    ray_end = (point[0], point[1] - 5.0, point[2])
    navigation_results = [
        console.evaluate(
            f"KBEngine.raycast({space_id}, 17, {ray_start!r}, {ray_end!r})"
        ) for console in cells.values()
    ]
    require(navigation_results[0] and all(value == navigation_results[0] for value in navigation_results),
            "不同 Cell 的显式导航层查询结果不一致")
    error = captured_error(
        source, f"KBEngine.addSpaceGeometryMapping({space_id}, None, {original_mapping!r}, "
        "True, {0: 'x' * 4097})"
    )
    require(error and error[0] == "ValueError", "超长导航参数没有返回调用错误")
    error = captured_error(
        source, f"KBEngine.addSpaceGeometryMapping({space_id}, None, {original_mapping!r}, "
        "True, {1 << 64: 'navmesh.bin'})"
    )
    require(error and error[0] == "OverflowError", "无法表示的导航层编号没有返回调用错误")
    require(layouts() == final_resources, "拒绝导航参数时改变了布局或资源版本")
    require(source.evaluate(f"KBEngine.raycast({space_id}, 17, {ray_start!r}, {ray_end!r})")
            == navigation_results[0], "拒绝导航参数后旧句柄不可用")
    source.evaluate(f"KBEngine.setSpaceData({space_id}, 'layout_after_rejection', 'ok')")
    wait_for(
        "无效导航参数拒绝后仍可同步 SpaceData", layouts,
        lambda state: all(value and value["state"] == "active" and value["hasNavigation"]
                          and value["resourceRevision"] == restored_revision + 2
                          for value in state.values()),
    )

    def verify_after_logout():
        # 超过旧的空 Space 延迟清理时间后，空 Cell 仍须由管理器保留。
        # 退出客户端后再做大额写入，避免业务追击和客户端转发影响布局测试。
        retained_until = time.monotonic() + 4
        while time.monotonic() < retained_until:
            require(all(value and value["state"] == "active" for value in layouts().values()),
                    "空区域被当作整个 Space 结束而删除")
            time.sleep(0.2)
        resource_revision = verify_resource_limits(
            cells, owner, space_id, version, restored_revision + 2, original_mapping,
            ray_start, ray_end, navigation_results[0], layouts, rejection_logged,
        )
        print(f"SPACE_LAYOUT_ACTIVE_PASS space={space_id} cells={len(cells)} version={version}",
              flush=True)
        return {"empty_cells_retained": True, "resource_revision": resource_revision,
                "oversized_resource_updates_rejected": True}

    return {"enabled": True, "space": space_id, "owner": owner,
            "owner_entity": source_space["id"], "version": version,
            "cells": first["cells"], "resources_synchronized": True,
            "rejected_target_version": rejected_version,
            "ready_without_navigation": True, "invalid_navigation_rejected": True,
            "navigation_parameters_synchronized": True,
            "split_x": split_x, "split_z": split_z}, verify_after_logout


def verify_boundary_ghosts(cells, result, bots, read_client_log, read_cell_log, has_y):
    space = result["space"]
    owner = result["owner"]
    source = cells[owner]
    peers = [cid for cid in cells if cid != owner]
    x, z = result["split_x"], result["split_z"]
    position = (x + 2.0, 0.0, z + 2.0)
    host = cells[source.evaluate(
        f"KBEngine.cellForPosition({space}, {position[0]}, {position[2]})[1]"
    )]
    avatar, radius, hysteresis = source.evaluate(
        f"next((e.id, e.getViewRadius(), e.getViewHystArea()) for e in KBEngine.entities.values() "
        f"if type(e).__name__ == 'Avatar' and e.spaceID == {space} and e.hasWitness)"
    )
    ghost_id = host.evaluate(
        f"KBEngine.createEntity('NPC', {space}, {position!r}, (0., 0., 0.), "
        "{'name': 'boundary-visible', 'spawnID': 123456}).id"
    )
    # 不改变导航位置，让同一真实 Witness 的视野覆盖布局边界。
    source.evaluate(f"KBEngine.entities[{avatar}].setViewRadius(480.0, 5.0)")

    def replicas():
        return {
            cid: console.evaluate(
                f"(lambda e: (e.isReal(), e.name, e.spawnID, tuple(e.position), "
                f"tuple(e.direction), e.isWitnessed) if e else None)(KBEngine.entities.get({ghost_id}))"
            ) for cid, console in cells.items() if console is not host
        }

    def move(target):
        host.evaluate(f"setattr(KBEngine.entities[{ghost_id}], 'position', {target!r})")

    def client_replica():
        return bots.evaluate(
            f"[(e.name, list(e.position)) for app in KBEngine.bots.values() "
            f"if app.entities.get({avatar}) for e in (app.entities.get({ghost_id}),) if e]"
        )

    def set_view(view_radius, margin):
        source.evaluate(f"KBEngine.entities[{avatar}].setViewRadius({view_radius}, {margin})")

    def near(actual, expected, tolerance=0.01):
        return all(math.isclose(a, b, abs_tol=tolerance) for a, b in zip(actual, expected, strict=True))

    try:
        wait_for("边界副本进入真实 Witness", replicas,
                 lambda values: all(v and v[:3] == [False, "boundary-visible", 0]
                                    for v in values.values()) and values[owner][5])
        wait_for("客户端 Ghost 初始状态", client_replica,
                 lambda values: any(name == "boundary-visible" for name, _ in values))
        for expression in (
            f"KBEngine.entities[{ghost_id}].addTimer(1.0, 0.0, 0)",
            f"KBEngine.entities[{ghost_id}].destroy()",
            f"setattr(KBEngine.entities[{ghost_id}], 'name', 'not-authoritative')",
        ):
            error = captured_error(source, expression)
            require(error and "boundary Ghost" in error[1], f"Ghost 允许本地权威操作：{expression}")
        nearby = (position[0] + 3., 1., position[2] + 2.)
        move(nearby)
        host.evaluate(f"setattr(KBEngine.entities[{ghost_id}], 'direction', (0.0, 0.0, 1.0))")
        host.evaluate(f"setattr(KBEngine.entities[{ghost_id}], 'name', 'boundary-updated')")
        wait_for("Ghost 位姿与公开属性同步", replicas,
                 lambda values: all(v and v[1:3] == ["boundary-updated", 0] and
                                    near(v[3], nearby) and
                                    v[4] == [0, 0, 1] for v in values.values()))
        wait_for("客户端收到 Ghost 属性与位置变化", client_replica,
                 # 客户端沿用紧凑坐标协议，允许相对坐标编码产生的舍入误差。
                 lambda values: any(name == "boundary-updated" and near(pos, nearby, 0.5)
                                    for name, pos in values), timeout=12)
        # X/Z 不动，Ghost 仍需保留。只有三维 AOI 才应因高度变化而退出视野。
        raised = (nearby[0], nearby[1] + 1024., nearby[2])
        move(raised)
        wait_for("观察者所在 Cell 收到纯 Y 轴移动", replicas,
                 lambda values: values[owner] and near(values[owner][3], raised))
        visible = (f"any(e.id == {ghost_id} for e in "
                   f"KBEngine.entities[{avatar}].entitiesInView(True))")
        wait_for("纯 Y 轴移动后的 AOI", lambda: source.evaluate(visible),
                 lambda value: value == (not has_y))
        move(nearby)
        wait_for("恢复高度后客户端仍可见", client_replica,
                 lambda values: any(name == "boundary-updated" and near(pos, nearby, 0.5)
                                    for name, pos in values), timeout=12)
        move((x + 1024., 0., z + 1024.))
        wait_for("离开边界的副本清理", replicas, lambda values: not any(values.values()))
        move(position)
        wait_for("重新进入边界的完整副本", replicas,
                 lambda values: all(v and v[1] == "boundary-updated" for v in values.values()))

        monster = host.evaluate(
            f"KBEngine.createEntity('Monster', {space}, {position!r}, (0., 0., 0.), "
            f"{{'name': 'boundary-broadcast', 'spawnPos': {position!r}}}).id"
        )
        # 四区交点可能不在导航网格上。保留观察回调，只暂停测试怪物的随机走动。
        next_move = int(time.time()) + 3600
        monster_expr = f"KBEngine.entities[{monster}]"
        paused_until = host.evaluate(
            f"(setattr({monster_expr}, 'nextMoveTime', {next_move}), {monster_expr}.nextMoveTime)[1]"
        )
        require(paused_until == next_move, "测试怪物的随机走动未暂停")

        def real_observed():
            return host.evaluate(f"KBEngine.entities[{monster}].isWitnessed")

        def observation_callbacks():
            prefix = f"Monster::onWitnessed: {monster} isWitnessed="
            return [line.rsplit(prefix, 1)[1] for line in read_cell_log().splitlines()
                    if prefix in line and line.endswith(("isWitnessed=0.", "isWitnessed=1."))]

        try:
            require(not host.evaluate(
                f"any(e.isReal() and e.hasWitness and e.spaceID == {space} "
                "for e in KBEngine.entities.values())"
            ), "观察回调检查要求怪物所在 Cell 没有本地玩家")
            wait_for("广播实体进入对侧 Witness", lambda: source.evaluate(
                f"(lambda e: bool(e and not e.isReal() and e.isWitnessed))(KBEngine.entities.get({monster}))"
            ), bool)
            wait_for("Bots 创建跨 Cell 客户端实体", read_client_log,
                     lambda log: f"ClientObjectBase::createEntity: new Monster ({monster})" in log)
            wait_for("只有远端玩家时 Real 收到观察回调", observation_callbacks,
                     lambda calls: calls == ["1."],
                     timeout=5)
            require(real_observed(), "远端玩家未计入 Real 的观察状态")
            profile = (
                "import sys\n"
                "_boundary_calls = []\n"
                "def _boundary_profile(frame, event, arg):\n"
                "    if event == 'call' and frame.f_code.co_name == 'recvDamage':\n"
                "        values = frame.f_locals\n"
                f"        if values.get('attackerID') == {avatar} and values['self'].id == {monster}:\n"
                "            _boundary_calls.append(values['damage'])\n"
                "sys.setprofile(_boundary_profile)\n"
            )
            bots.evaluate(f"exec({profile!r})")
            for clients, damage in (("allClients", 31), ("otherClients", 37)):
                host.evaluate(f"KBEngine.entities[{monster}].{clients}.recvDamage({avatar}, 1, 1, {damage})")
            wait_for("Bots 收到跨 Cell 广播", lambda: bots.evaluate("_boundary_calls"),
                     lambda calls: calls == [31, 37], timeout=12)
            time.sleep(0.3)
            require(bots.evaluate("_boundary_calls") == [31, 37], "跨 Cell 广播重复发送")

            set_view(1.0, 0.0)
            wait_for("Real 收到远端离开状态", real_observed, lambda value: not value)
            set_view(480.0, 5.0)
            wait_for("Real 收到远端重新进入状态", real_observed, bool)
            # 延时期间观察状态可以变化，不能重复触发启停 AI 的回调。
            require(observation_callbacks() == ["1."], "延时内重入重复触发观察回调")

            set_view(1.0, 0.0)
            wait_for("最后一个远端观察者离开后的回调", observation_callbacks,
                     lambda calls: calls == ["1.", "0."], timeout=22)
            require(not real_observed(), "最后一个远端观察者离开后 Real 仍被标为已观察")
            set_view(480.0, 5.0)
            wait_for("延时结束后重新进入触发回调", observation_callbacks,
                     lambda calls: calls == ["1.", "0.", "1."])
        finally:
            bots.evaluate("(__import__('sys').setprofile(None), None)[1]")
            host.evaluate(f"KBEngine.entities[{monster}].destroy()")
    finally:
        host.evaluate(f"KBEngine.entities[{ghost_id}].destroy()")
        set_view(radius, hysteresis)

    wait_for("Real 销毁后的所有副本清理", replicas, lambda values: not any(values.values()))
    print(f"BOUNDARY_GHOSTS_PASS space={space} replicas={len(peers)}", flush=True)
    return {"replicas": len(peers), "aoi": True, "pose_and_public_property": True,
            "client_broadcast": True, "remote_observation": True, "cleanup": True}


def verify_ghost_source_change(cells, layout, bots, output, has_y):
    space = layout["space"]
    observer = cells[layout["owner"]]
    x, z = layout["split_x"], layout["split_z"]
    left, right = (x - 4., 0., z + 4.), (x + 4., 0., z + 4.)
    source_id, target_id, peer_id = [
        observer.evaluate(f"KBEngine.cellForPosition({space}, {px!r}, {pz!r})[1]")
        for px, pz in ((left[0], left[2]), (right[0], right[2]), (x + 4., z - 4.))
    ]
    source, target, peer = (cells[cid] for cid in (source_id, target_id, peer_id))
    require(len({source, target, peer, observer}) == 4, "Ghost 换源检查需要四个不同的 Cell")

    avatar, radius, hysteresis = observer.evaluate(
        f"next((e.id, e.getViewRadius(), e.getViewHystArea()) for e in KBEngine.entities.values() "
        f"if type(e).__name__ == 'Avatar' and e.spaceID == {space} and e.hasWitness)"
    )
    # Monster 自带 recvDamage，但旧 demo 的怪物陷阱不适用于三维 AOI；三维继续用 NPC。
    entity_type = "NPC" if has_y else "Monster"
    initial_host, initial_pos = (source, left) if has_y else (target, right)
    entity = initial_host.evaluate(
        f"KBEngine.createEntity({entity_type!r}, {space}, {initial_pos!r}, (0., 0., 0.), "
        f"{{'name': 'before-handoff', 'spawnPos': {initial_pos!r}}}).id"
    )
    lookup = f"KBEngine.entities.get({entity})"
    ghost_name = f"(lambda e: e.name if e and not e.isReal() else None)({lookup})"
    client_entity = (
        f"next(app.entities.get({entity}) for app in KBEngine.bots.values() "
        f"if app.entities.get({avatar}))"
    )
    avatar_expr = f"KBEngine.entities.get({avatar})"
    visible = f"any(e.id == {entity} for e in {avatar_expr}.entitiesInView(True))"
    client_state = (
        f"(lambda e: (e is _ghost_client_entity, getattr(e, 'name', None)))({client_entity})"
    )

    def set_view(view_radius, margin):
        observer.evaluate(f"{avatar_expr}.setViewRadius({view_radius}, {margin})")

    def setup(name):
        return f"""
from {entity_type} import {entity_type} as _ghost_class
_ghost_previous_enter = _ghost_class.__dict__.get('onEnteredCell')
_ghost_inherited_enter = getattr(_ghost_class, 'onEnteredCell', None)
def _ghost_enter(self):
    if self.id == {entity}:
        self.name = {name!r}
    if _ghost_inherited_enter is not None:
        _ghost_inherited_enter(self)
_ghost_class.onEnteredCell = _ghost_enter
"""
    restore = """
if _ghost_previous_enter is None:
    del _ghost_class.onEnteredCell
else:
    _ghost_class.onEnteredCell = _ghost_previous_enter
"""
    profile = f"""
import sys
_ghost_rpc_calls = []
_ghost_scale_calls = []
def _ghost_profile(frame, event, arg):
    method = frame.f_code.co_name
    if event != 'call' or method not in ('recvDamage', 'set_modelScale'):
        return
    values = frame.f_locals
    receiver = values['self']
    if receiver.id != {entity}:
        return
    same_object = receiver is _ghost_client_entity
    if method == 'recvDamage' and values['attackerID'] == {avatar}:
        _ghost_rpc_calls.append((values['damage'], same_object, receiver.name))
    elif method == 'set_modelScale':
        _ghost_scale_calls.append((values['oldValue'], receiver.modelScale, same_object, receiver.name))
sys.setprofile(_ghost_profile)
"""

    def change_source(old_host, new_host, position, old_name, new_name, messages=None, marker=None):
        paused = output / f"ghost-observer-paused-{new_name}"
        release = output / f"ghost-observer-release-{new_name}"
        pause = f"""
from pathlib import Path
from time import monotonic, sleep
Path({str(paused)!r}).touch()
deadline = monotonic() + 12
while not Path({str(release)!r}).exists() and monotonic() < deadline:
    sleep(0.01)
"""
        wait_for("迁移目标已收到待提升的 Ghost", lambda: new_host.evaluate(ghost_name),
                 lambda name: name == old_name)
        new_host.evaluate(f"exec({setup(new_name)!r})")
        try:
            # 暂停观察者，避免旧 Leave 先发出，也让新快照和随后发出的消息一起排队。
            with ThreadPoolExecutor(max_workers=1) as executor:
                held = executor.submit(observer.evaluate, f"exec({pause!r})", 14)
                try:
                    wait_for("观察者暂停处理消息", paused.exists, bool, timeout=5)
                    old_host.evaluate(f"setattr({lookup}, 'position', {position!r})")
                    wait_for("第四个 Cell 收到新 Real 的 Ghost 快照", lambda: peer.evaluate(ghost_name),
                             lambda name: name == new_name, timeout=8)
                    if messages is not None:
                        new_host.evaluate(f"({messages}, setattr({lookup}, 'uid', {marker}))")
                        wait_for("第四个 Cell 收到新来源的同步标记", lambda: peer.evaluate(f"{lookup}.uid"),
                                 lambda uid: uid == marker, timeout=2)
                        require(bots.evaluate(client_state) == [True, old_name],
                                "恢复观察者处理前客户端没有保留旧快照")
                finally:
                    release.touch()
                held.result()

            wait_for("观察者使用新 Ghost 快照", lambda: observer.evaluate(ghost_name),
                     lambda name: name == new_name, timeout=8)
            wait_for("外圈换源后客户端保留原对象并收到新属性", lambda: bots.evaluate(client_state),
                     lambda state: state == [True, new_name], timeout=8)
            require(observer.evaluate(visible), "换源后 Witness 丢失了外圈的实体")
        finally:
            new_host.evaluate(f"exec({restore!r})")

    try:
        if not has_y:
            initial_host.evaluate(f"setattr({lookup}, 'nextMoveTime', {int(time.time()) + 3600})")
        observer.evaluate(f"setattr({avatar_expr}, 'controlledBy', None)")
        set_view(32., 10.)
        observer.evaluate(f"setattr({avatar_expr}, 'position', {(x - 4., 0., z - 26.)!r})")
        wait_for("迁移前观察者收到旧 Ghost", lambda: observer.evaluate(ghost_name),
                 lambda name: name == "before-handoff")
        wait_for("客户端收到实体初始属性",
                 lambda: bots.evaluate(f"getattr(({client_entity}), 'name', None)"),
                 lambda name: name == "before-handoff")
        bots.evaluate(f"exec('_ghost_client_entity = {client_entity}')")
        try:
            if not has_y:
                bots.evaluate(f"exec({profile!r})")
            # 实体先进入内圈，再留在外圈。换源前后 Z 距离都为 40，不能重新从内圈进入。
            observer.evaluate(f"setattr({avatar_expr}, 'position', {(x - 4., 0., z - 36.)!r})")
            require(observer.evaluate(visible) and bots.evaluate(client_state) == [True, "before-handoff"],
                    "换源前滞回外圈没有保留已可见的实体")
            if not has_y:
                change_source(target, source, left, "before-handoff", "rpc-handoff",
                              f"[{lookup}.otherClients.recvDamage({avatar}, 1, 1, damage) "
                              "for damage in (41, 43)]", marker=739)
                calls = wait_for("客户端执行 Ghost 换源广播", lambda: bots.evaluate("_ghost_rpc_calls"),
                                 lambda calls: len(calls) >= 2, timeout=8)
                require(calls == [[41, True, "rpc-handoff"], [43, True, "rpc-handoff"]],
                        f"Ghost 换源 RPC 未按顺序在原客户端对象上读到新快照：{calls!r}")

                # 单独测试属性先到，避免 RPC 先发出快照后掩盖增量路径的问题。
                bots.evaluate("_ghost_scale_calls.clear()")
                change_source(source, target, right, "rpc-handoff", "after-handoff",
                              f"setattr({lookup}, 'modelScale', 37)", marker=743)
                # 再发一次常规增量；收到它时，之前同一连接上的回调应该已经全部执行。
                target.evaluate(f"setattr({lookup}, 'modelScale', 39)")
                calls = wait_for("客户端执行 Ghost 属性回调", lambda: bots.evaluate("_ghost_scale_calls"),
                                 lambda calls: any(current == 39 for _, current, _, _ in calls), timeout=8)
                require(calls == [[30, 37, True, "after-handoff"], [37, 39, True, "after-handoff"]],
                        f"Ghost 换源属性回调重复或发生在快照前：{calls!r}")
            else:
                change_source(source, target, right, "before-handoff", "after-handoff")

            destination = (x + 4., 0., z - 4.)
            verify_observer_ghost_refresh(observer, peer, avatar, ghost_name, bots, client_entity, destination,
                                          output, (target, lookup) if not has_y else None)

            # 保留边界 Ghost，只让它真正离开外圈；重新到达外圈不能代替进入内圈。
            peer.evaluate(f"(setattr({avatar_expr}, 'controlledBy', None), "
                          f"setattr({avatar_expr}, 'position', {destination!r}))")
            remote_position = f"(lambda e: tuple(e.position) if e else None)({lookup})"
            for distance, description in ((54., "越过外圈"), (40., "只回到外圈")):
                position = (right[0], right[1], destination[2] + distance)
                target.evaluate(f"setattr({lookup}, 'position', {position!r})")
                wait_for(f"目标观察者所在 Cell 收到实体{description}的位置",
                         lambda: peer.evaluate(remote_position),
                         lambda value: value is not None and math.dist(value, position) < 0.01, timeout=8)
                require(not peer.evaluate(visible), f"实体{description}后仍在 Witness 视野内")
                wait_for(f"实体{description}后不在客户端视野", lambda: bots.evaluate(client_state),
                         lambda state: state == [False, None], timeout=8)
        finally:
            if not has_y:
                bots.evaluate("(__import__('sys').setprofile(None), None)[1]")
            bots.evaluate("exec('del _ghost_client_entity')")
    finally:
        for console in (source, target):
            console.evaluate(f"(lambda e: e.destroy() if e and e.isReal() else None)({lookup})")
        for console in (observer, peer):
            console.evaluate(f"(lambda e: (setattr(e, 'controlledBy', e.base), "
                             f"e.setViewRadius({radius}, {hysteresis})) if e and e.isReal() "
                             f"else None)({avatar_expr})")
    print(f"GHOST_SOURCE_CHANGE_PASS space={space} entity={entity}", flush=True)
    return {"entity": entity, "same_client_entity": True, "snapshot_property": "after-handoff",
            "retained_in_hysteresis": True, "outer_exit_and_reentry": True,
            "observer_handoff_refreshed": True, "snapshot_before_rpc": True if not has_y else None,
            "snapshot_before_property_callback": True if not has_y else None,
            "observer_handoff_property_once": True if not has_y else None}


def verify_observer_ghost_refresh(source, target, avatar, ghost_name, bots, client_entity, destination,
                                  output, ghost_real):
    require(target.evaluate(ghost_name) == "after-handoff", "玩家目标 Cell 缺少最新 Ghost")
    avatar_expr = f"KBEngine.entities.get({avatar})"
    player_with_witness = f"(lambda e: bool(e and e.isReal() and e.hasWitness))({avatar_expr})"
    client_state = (
        f"(lambda e: (e is _ghost_client_entity, getattr(e, 'name', None)))({client_entity})"
    )
    # 只让 Bot 缓存停在旧值，模拟源 Witness 没来得及发出刷新；迁移和补发仍由真实集群完成。
    bots.evaluate("setattr(_ghost_client_entity, 'name', 'before-handoff')")
    require(bots.evaluate(client_state) == [True, "before-handoff"], "迁移前客户端没有保留旧属性")
    source.evaluate(f"setattr({avatar_expr}, 'controlledBy', None)")
    try:
        move = f"setattr({avatar_expr}, 'position', {destination!r})"
        if ghost_real is None:
            source.evaluate(move)
        else:
            verify_observer_handoff_messages(source, target, avatar, move, player_with_witness,
                                             bots, output, *ghost_real)
        wait_for("玩家迁入目标 Cell 并恢复 Witness", lambda: target.evaluate(player_with_witness), bool)
        require(not source.evaluate(player_with_witness), "源 Cell 仍保留玩家 Real 和 Witness")
        wait_for("观察者迁移后保留客户端对象并刷新 Ghost 属性", lambda: bots.evaluate(client_state),
                 lambda state: state == [True, "after-handoff"], timeout=8)
    finally:
        for console in (source, target):
            console.evaluate(f"(lambda e: setattr(e, 'controlledBy', e.base) if e and e.isReal() "
                             f"else None)({avatar_expr})")
    print(f"OBSERVER_GHOST_REFRESH_PASS player={avatar}", flush=True)


def verify_observer_handoff_messages(source, target, avatar, move, player_with_witness, bots, output, real, lookup):
    target_entered = output / "observer-handoff-target-entered"
    target_resume = output / "observer-handoff-target-resume"
    source_paused = output / "observer-handoff-source-paused"
    source_resume = output / "observer-handoff-source-resume"
    finished = output / "observer-handoff-finished"
    hook = f"""
from pathlib import Path
from time import monotonic, sleep
from Avatar import Avatar as _observer_avatar_class
_observer_saved_slot = _observer_avatar_class.__dict__.get('__setattr__')
_observer_setattr = _observer_avatar_class.__setattr__
_observer_saved_enter = _observer_avatar_class.__dict__.get('onEnteredCell')
_observer_enter = getattr(_observer_avatar_class, 'onEnteredCell', None)
def _observer_hold(self, key, value):
    entered = Path({str(target_entered)!r})
    if self.id == {avatar} and self.isReal() and not entered.exists():
        entered.touch()
        deadline = monotonic() + 12
        while not Path({str(target_resume)!r}).exists() and monotonic() < deadline:
            sleep(0.01)
    return _observer_setattr(self, key, value)
def _observer_finished(self):
    if _observer_enter is not None:
        _observer_enter(self)
    if self.id == {avatar}:
        Path({str(finished)!r}).touch()
_observer_avatar_class.__setattr__ = _observer_hold
_observer_avatar_class.onEnteredCell = _observer_finished
"""
    restore = """
for name, callback in (('__setattr__', _observer_saved_slot), ('onEnteredCell', _observer_saved_enter)):
    if callback is None:
        delattr(_observer_avatar_class, name)
    else:
        setattr(_observer_avatar_class, name, callback)
"""
    hold_source = f"""
from pathlib import Path
from time import monotonic, sleep
Path({str(source_paused)!r}).touch()
deadline = monotonic() + 12
while not Path({str(source_resume)!r}).exists() and monotonic() < deadline:
    sleep(0.01)
_observer_released = Path({str(source_resume)!r}).exists()
"""
    send_rpc = f"{lookup}.otherClients.recvDamage({avatar}, 1, 1, {{}})"
    bots.evaluate("(_ghost_scale_calls.clear(), _ghost_rpc_calls.clear())")
    target.evaluate(f"exec({hook!r})")
    try:
        source.evaluate(move)
        wait_for("目标开始安装玩家快照", target_entered.exists, bool, timeout=8)
        with ThreadPoolExecutor(max_workers=1) as executor:
            held = executor.submit(source.evaluate, f"(exec({hold_source!r}), _observer_released)[1]", 14)
            try:
                # 先让源端停在释放请求之前，再恢复目标；保持 BaseApp 和 Bot 正常运行。
                wait_for("源 Cell 暂停释放", source_paused.exists, bool, timeout=5)
                target_resume.touch()
                wait_for("目标 Witness 已安装", lambda: target.evaluate(player_with_witness), bool, timeout=8)
                require(not finished.exists(), "测试未进入已安装但未释放的交接窗口")
                real.evaluate(f"(setattr({lookup}, 'modelScale', 47), {send_rpc.format(47)}, "
                              f"setattr({lookup}, 'uid', 747))")
                wait_for("目标 Ghost 已应用属性增量和 RPC 后的标记",
                         lambda: target.evaluate(f"({lookup}.modelScale, {lookup}.uid)"),
                         lambda state: state == [47, 747], timeout=5)
                early = bots.evaluate("(_ghost_scale_calls, _ghost_rpc_calls)")
            finally:
                source_resume.touch()
            require(held.result(), "源 Cell 在测试放行前已结束等待")
        wait_for("源端释放后目标完成交接", finished.exists, bool, timeout=8)
        # 最后一条 RPC 是同一可靠流上的标记，收到后之前的回调都已执行。
        real.evaluate(send_rpc.format(53))
        calls = wait_for("客户端执行交接窗口内和交接后的 Ghost RPC",
                         lambda: bots.evaluate("(_ghost_scale_calls, _ghost_rpc_calls)"),
                         lambda calls: any(damage == 53 for damage, _, _ in calls[1]), timeout=8)
        require(calls == [[[39, 47, True, "after-handoff"]],
                          [[47, True, "after-handoff"], [53, True, "after-handoff"]]],
                f"观察者跨 Cell 的 Ghost 回调重复、丢失或使用旧属性：{calls!r}")
        require(early == [[], []], f"源端释放前客户端已执行目标 Cell 的回调：{early!r}")
    finally:
        target_resume.touch(exist_ok=True)
        target.evaluate(f"exec({restore!r})")


def verify_resource_limits(cells, owner, space_id, version, revision, mapping,
                           ray_start, ray_end, navigation, layouts, rejection_logged):
    source = cells[owner]
    replica = next(cid for cid in cells if cid != owner)
    keys = [f"layout_test_size_{index}" for index in range(32)]

    def wait_revision(description, expected):
        return wait_for(
            description, layouts,
            lambda state: all(value and value["state"] == "active"
                              and not value["error"] and value["version"] == version
                              and value["resourceRevision"] == expected and value["resourcesReady"]
                              and value["hasNavigation"] for value in state.values()),
        )

    # 31 条 64000 字节的值留下足够空间容纳新的加载参数或第 32 条数据，但不能同时容纳两者。
    # 每批等所有 Cell 收到后再继续，避免测试流量本身影响正常 Tick。
    for start in range(0, 31, 4):
        batch = keys[start:min(start + 4, 31)]
        source.evaluate(
            f"[KBEngine.setSpaceData({space_id}, key, 'x' * 64000) for key in {batch!r}]"
        )
        revision += len(batch)
        wait_revision("大额 SpaceData 同步", revision)
    require(not any(rejection_logged(cid, "addSpaceGeometryMapping", space_id, version, revision + 1, "")
                    for cid in cells), "测试前已存在同版本的拒绝记录")
    # 同一轮脚本连续发两条请求，几何校验仍看到第 31 条数据。管理器先接受第 32 条，再拒绝几何更新。
    source.evaluate(
        f"(KBEngine.setSpaceData({space_id}, {keys[-1]!r}, 'x' * 64000), "
        f"KBEngine.addSpaceGeometryMapping({space_id}, None, {mapping!r}, "
        "True, {layer: 'x' * 4000 for layer in range(16)}))"
    )
    revision += 1
    wait_for("属主收到几何更新超限拒绝",
             lambda: rejection_logged(owner, "addSpaceGeometryMapping", space_id, version, revision, ""),
             bool)
    committed = wait_revision("只接受第 32 条 SpaceData", revision)
    require(all(not rejection_logged(cid, "addSpaceGeometryMapping", space_id, version, revision, "")
                for cid in cells if cid != owner), "拒绝消息发给了未提交请求的 Cell")
    require(all(console.evaluate(f"len(KBEngine.getSpaceData({space_id}, {keys[-1]!r}))") == 64000
                for console in cells.values()), "几何更新被拒时丢失了已提交的 SpaceData")
    key = "layout_after_rejection"
    cells[replica].evaluate(f"KBEngine.setSpaceData({space_id}, {key!r}, 'x' * 64000)")
    wait_for("副本收到 SpaceData 总大小超限拒绝",
             lambda: rejection_logged(replica, "setSpaceData", space_id, version, revision, key), bool)
    require(layouts() == committed, "资源更新被拒后，布局状态或资源版本发生变化")
    for console in cells.values():
        require(console.evaluate(f"KBEngine.getSpaceData({space_id}, {key!r})") == "ok",
                "被拒绝的 SpaceData 覆盖了已提交数据")
        require(console.evaluate(f"KBEngine.raycast({space_id}, 17, {ray_start!r}, {ray_end!r})")
                == navigation, "拒绝更新后旧导航层不可用")
    cells[replica].evaluate(f"KBEngine.setSpaceData({space_id}, {key!r}, 'updated')")
    revision += 1
    wait_revision("拒绝超限更新后仍可提交", revision)
    require(all(console.evaluate(f"KBEngine.getSpaceData({space_id}, {key!r})") == "updated"
                for console in cells.values()), "合法更新未同步到全部 Cell")
    source.evaluate(f"[KBEngine.delSpaceData({space_id}, key) for key in {keys!r}]")
    revision += len(keys)
    wait_revision("清理测试用 SpaceData", revision)
    print(f"SPACE_LAYOUT_RESOURCE_LIMIT_PASS space={space_id} revision={revision}", flush=True)
    return revision


def verify_rejected_layout_removal(cells, space):
    # 同一轮脚本先申请再删除，确保返回的 ABORT 不能在删除请求前被处理。
    owner, space_id = space["cid"], space["space"]
    source = cells[owner]
    positions = source.evaluate(
        f"[e.position[0] for e in KBEngine.entitiesForSpace({space_id}).values() if e.isReal()]"
    )
    boundary = max(positions) + 256.0
    wait_for("待删除 Space 导航加载", lambda: source.evaluate(f"KBEngine.getSpaceLayout({space_id})"),
             lambda state: state["resourcesReady"])
    version, immediate = source.evaluate(
        f"(KBEngine.setSpaceLayout({space_id}, ('X', {boundary!r}, {owner}, 999999)), "
        f"KBEngine.entities[{space['id']}].destroySpace(), KBEngine.getSpaceLayout({space_id}))[::2]"
    )
    require(immediate["version"] == version and immediate["state"] == "preparing",
            "删除请求在管理器确认前抢占了布局状态")
    wait_for(
        "布局被拒后的本地 Space 删除",
        lambda: [console.evaluate(f"KBEngine.getSpaceLayout({space_id})") for console in cells.values()],
        lambda values: all(value is None for value in values),
    )
    print(f"SPACE_LAYOUT_REJECTED_REMOVAL_PASS space={space_id}", flush=True)


def verify_layout_removal(cells, result, base, origin_space, output):
    if not result.get("enabled"):
        return
    space_id = result["space"]
    destination = cells[result["owner"]]
    origin = cells[origin_space["cid"]]
    require(origin is not destination, "迁移期间销毁检查需要两个不同的 CellApp")
    mapping = destination.evaluate(f"KBEngine.getSpaceGeometryMapping({space_id})")

    # 用有 Base 的实体验证在途提交，再用无 Base 的实体检查关闭期间拒绝迁入。
    route_key = f"space_layout_test_{space_id}"
    publish_route = f"KBEngine.globalData[{route_key!r}] = KBEngine.entities[{result['owner_entity']}].cell"
    base.evaluate(f"exec({publish_route!r})")
    first = base.evaluate(
        "KBEngine.createEntityLocally('SpawnPoint', "
        f"{{'createToCell': KBEngine.entities[{origin_space['id']}].cell, "
        "'spawnEntityNO': 40001003, 'position': (0, 0, 0)}).id"
    )
    second = origin.evaluate(f"KBEngine.createEntity('NPC', {origin_space['space']}, (1., 1., 12000.), "
                             "(0., 0., 0.), {'uid': 1001}).id")
    wait_for(
        "源端迁移实体及目标引用准备",
        lambda: origin.evaluate(
            f"({route_key!r} in KBEngine.globalData, "
            f"all(KBEngine.entities.get(eid) and KBEngine.entities[eid].isReal() for eid in {(first, second)!r}))"
        ),
        lambda state: all(state),
    )

    def entity_is_real(console, eid, expected_space):
        return console.evaluate(
            f"(lambda e: bool(e and e.isReal() and e.spaceID == {expected_space}))"
            f"(KBEngine.entities.get({eid}))"
        )

    def teleport(eid):
        return origin.evaluate(
            f"(KBEngine.entities[{eid}].teleport(KBEngine.globalData[{route_key!r}], "
            f"(0, 0, 0), (0, 0, 0)), KBEngine.entities[{eid}].isReal())[1]"
        )

    # 目标安装后暂缓发出迁移结束消息，等控制器暂停 BaseApp；开始登记不会被这次暂停拦住。
    installed = output / "space-layout-target-installed"
    paused = output / "space-layout-base-paused"
    release = output / "space-layout-base-release"
    hook = f"""
import pathlib, time
from SpawnPoint import SpawnPoint
_layout_previous = SpawnPoint.__dict__.get('onTeleportSuccess')
_layout_installed = pathlib.Path({str(installed)!r})
_layout_paused = pathlib.Path({str(paused)!r})
def _layout_on_teleport(self, nearby):
    if self.id == {first}:
        _layout_installed.touch()
        deadline = time.monotonic() + 12
        while not _layout_paused.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
    if _layout_previous:
        _layout_previous(self, nearby)
SpawnPoint.onTeleportSuccess = _layout_on_teleport
"""
    destination.evaluate(f"exec({hook!r})")
    hold = (
        "import pathlib, time\n"
        f"pathlib.Path({str(paused)!r}).touch()\n"
        f"_layout_release = pathlib.Path({str(release)!r})\n"
        "_layout_deadline = time.monotonic() + 20\n"
        "while not _layout_release.exists() and time.monotonic() < _layout_deadline:\n"
        "    time.sleep(0.02)\n"
        "_layout_released = _layout_release.exists()\n"
    )
    try:
        teleport(first)
        wait_for("目标已安装第一轮快照", installed.exists, bool, timeout=8)
        with ThreadPoolExecutor(max_workers=1) as executor:
            held = executor.submit(base.evaluate, f"(exec({hold!r}), _layout_released)[1]", 25)
            try:
                wait_for("BaseApp 暂停处理迁移确认", paused.exists, bool, timeout=5)
                require(entity_is_real(destination, first, space_id), "目标没有已安装的迁移实体")
                error = captured_error(
                    destination, f"(KBEngine.entities[{result['owner_entity']}].destroySpace(), "
                    f"KBEngine.addSpaceGeometryMapping({space_id}, None, {mapping!r}, True))"
                )
                require(error and error[0] == "RuntimeError", "删除请求后仍接受新的导航更新")
                wait_for("已收到 REMOVE 但未销毁",
                         lambda: destination.evaluate(f"KBEngine.getSpaceLayout({space_id})"),
                         lambda value: value and value["state"] == "releasing", timeout=5)
                require(not destination.evaluate(f"KBEngine.getSpaceLayout({space_id})['acceptsEntities']"),
                        "等待迁移完成时未公布 Space 已停止接收实体")
                error = captured_error(
                    destination, f"KBEngine.createEntity('SpawnPoint', {space_id}, "
                    "(0.0, 0.0, 0.0), (0.0, 0.0, 0.0), {'spawnEntityNO': 40001003})"
                )
                require(error and error[0] == "RuntimeError", "等待迁移完成时仍允许脚本创建 Real")
                require(not teleport(second), "第二次传送未发往远端")
                wait_for("删除中的 Space 拒绝新迁入并恢复源实体",
                         lambda: entity_is_real(origin, second, origin_space["space"]), bool, timeout=5)
                require(entity_is_real(destination, first, space_id),
                        "迁移尚未提交时目标实体已被运行期删除")
            finally:
                release.touch()
            require(held.result(), "BaseApp 在测试放行前已结束等待")
    finally:
        paused.touch(exist_ok=True)
        destination.evaluate(
            "(delattr(SpawnPoint, 'onTeleportSuccess') if _layout_previous is None else "
            "setattr(SpawnPoint, 'onTeleportSuccess', _layout_previous))"
        )

    wait_for(
        "整个逻辑 Space 删除",
        lambda: [console.evaluate(f"KBEngine.getSpaceLayout({space_id})")
                 for console in cells.values()],
        lambda values: all(value is None for value in values),
    )
    require(origin.evaluate(f"KBEngine.entities.get({first}) is None"),
            "已有迁移没有提交并清理源端")
    require(entity_is_real(origin, second, origin_space["space"]), "被拒绝的迁移未保留源端 Real")
    origin.evaluate(f"KBEngine.entities[{second}].destroy()")
    remove_route = f"del KBEngine.globalData[{route_key!r}]"
    base.evaluate(f"exec({remove_route!r})")
    result["logical_space_removed"] = True
    result["pending_migration_protected"] = True
    result["local_creation_rejected_while_releasing"] = True
    print(f"SPACE_LAYOUT_REMOVAL_PASS space={space_id}", flush=True)
