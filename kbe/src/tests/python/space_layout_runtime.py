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


def verify_boundary_ghosts(cells, result, bots, read_client_log, read_cell_log):
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
        move((x + 1024., 0., z + 1024.))
        wait_for("离开边界的副本清理", replicas, lambda values: not any(values.values()))
        move(position)
        wait_for("重新进入边界的完整副本", replicas,
                 lambda values: all(v and v[1] == "boundary-updated" for v in values.values()))

        monster = host.evaluate(
            f"KBEngine.createEntity('Monster', {space}, {position!r}, (0., 0., 0.), "
            f"{{'name': 'boundary-broadcast', 'spawnPos': {position!r}}}).id"
        )

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

    # 用实际有 Base 的业务实体走迁移协议，不销毁在线角色，也不替换引擎方法。
    route_key = f"space_layout_test_{space_id}"
    publish_route = f"KBEngine.globalData[{route_key!r}] = KBEngine.entities[{result['owner_entity']}].cell"
    base.evaluate(f"exec({publish_route!r})")
    migrants = base.evaluate(
        "[KBEngine.createEntityLocally('SpawnPoint', "
        f"{{'createToCell': KBEngine.entities[{origin_space['id']}].cell, "
        "'spawnEntityNO': 40001003, 'position': (0, 0, 0)}).id "
        "for _ in range(2)]"
    )
    first, second = migrants
    wait_for(
        "源端迁移实体及目标引用准备",
        lambda: origin.evaluate(
            f"({route_key!r} in KBEngine.globalData, "
            f"all(KBEngine.entities.get(eid) and KBEngine.entities[eid].isReal() for eid in {migrants!r}))"
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

    # 控制器放行前，BaseApp 不处理路由切换；超时也必须退出等待，避免影响关服。
    paused = output / "space-layout-base-paused"
    release = output / "space-layout-base-release"
    hold = (
        "import pathlib, time\n"
        f"pathlib.Path({str(paused)!r}).touch()\n"
        f"_layout_release = pathlib.Path({str(release)!r})\n"
        "_layout_deadline = time.monotonic() + 20\n"
        "while not _layout_release.exists() and time.monotonic() < _layout_deadline:\n"
        "    time.sleep(0.02)\n"
        "_layout_released = _layout_release.exists()\n"
    )
    with ThreadPoolExecutor(max_workers=1) as executor:
        held = executor.submit(base.evaluate, f"(exec({hold!r}), _layout_released)[1]", 25)
        try:
            wait_for("BaseApp 暂停处理迁移确认", paused.exists, bool, timeout=5)
            require(not teleport(first), "第一次传送未发往远端")
            wait_for("目标已收到未提交的迁移",
                     lambda: entity_is_real(destination, first, space_id), bool, timeout=5)
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

    wait_for(
        "整个逻辑 Space 删除",
        lambda: [console.evaluate(f"KBEngine.getSpaceLayout({space_id})")
                 for console in cells.values()],
        lambda values: all(value is None for value in values),
    )
    require(origin.evaluate(f"KBEngine.entities.get({first}) is None"),
            "已有迁移没有提交并清理源端")
    require(entity_is_real(origin, second, origin_space["space"]), "被拒绝的迁移未保留源端 Real")
    remove_route = f"del KBEngine.globalData[{route_key!r}]"
    base.evaluate(f"exec({remove_route!r})")
    result["logical_space_removed"] = True
    result["pending_migration_protected"] = True
    result["local_creation_rejected_while_releasing"] = True
    print(f"SPACE_LAYOUT_REMOVAL_PASS space={space_id}", flush=True)
