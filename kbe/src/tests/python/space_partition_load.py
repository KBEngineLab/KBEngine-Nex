"""在同一 demo Space 中比较均匀分布和四区交汇处的负载。"""

from collections import defaultdict
from dataclasses import asdict
import json
import time

from performance.process_metrics import ProcessGroupCollector
from performance.watcher_metrics import WatcherCollector, WatcherTarget
from space_layout_runtime import require, wait_for


TICK_PATH = "root/cprofiles/default/gameTick/latency"
GHOST_PATH = "root/network/messages/Cellapp::onBoundaryGhost"
MOVE_INTERVAL = 0.2
CONSOLE_BATCH = 12


def batches(rows):
    # 控制台按 1024 字节分段读取，短命令可以避免把回车与换行拆到两段。
    for offset in range(0, len(rows), CONSOLE_BATCH):
        yield rows[offset:offset + CONSOLE_BATCH]


def stop_demo_bot_actions(console):
    # 保留客户端连接及实体同步，避免 demo 随机攻击和传送改变测试分布。
    setup = """
from Avatar import PlayerAvatar
PlayerAvatar.updateTest = lambda self: None
PlayerAvatar.testAttackTarget = lambda self: None
PlayerAvatar.testTeleport = lambda self: None
def _partition_stop_actions():
    for app in KBEngine.bots.values():
        for entity in app.entities.values():
            if isinstance(entity, PlayerAvatar):
                entity.testType = 1
                entity.cancelController("Movement")
_partition_stop_actions()
del _partition_stop_actions
"""
    console.evaluate(f"exec({setup!r})")


def sample_distribution(collector, processes, cells, infos, space, duration):
    targets = {
        cid: {
            name: WatcherTarget("CELLAPP_TYPE", infos[cid]["host"], infos[cid]["internal_port"], path)
            for name, path in (
                ("tick", TICK_PATH),
                ("network", "root/network"),
                ("tcp", "root/network/poller"),
                ("aoi", "root/spaces/aoiRelations"),
                ("witness", "root/witness/queues"),
                ("ghost_message", GHOST_PATH),
            )
        }
        for cid in cells
    }
    # Tick 分位数来自引擎最近窗口，保留每次窗口观测，避免把多个分位数再算作 Tick 分布。
    window = max(collector.query(target["tick"])["windowMaxAgeSeconds"] for target in targets.values())
    time.sleep(max(12, window + 1))
    started = time.monotonic()
    samples = []
    while True:
        sample_time = time.monotonic()
        sample = {"seconds": sample_time - started, "cells": {}, "processes": {
            name: asdict(value) for name, value in processes.sample().items()
        }}
        for cid, console in cells.items():
            metrics = {name: collector.query(target) for name, target in targets[cid].items()}
            require(metrics["tick"]["count"] > 0 and "p99Micros" in metrics["tick"],
                    f"CellApp {cid} 缺少 Tick 分位数")
            require("sentCount" in metrics["ghost_message"], f"CellApp {cid} 缺少 Ghost 消息计数")
            counts = console.evaluate(
                f"(lambda rows: [sum(real for _, real in rows), sum(not real for _, real in rows), "
                "sum(not real and i in _partition_load_ids for i, real in rows)])"
                f"([(e.id, e.isReal()) for e in KBEngine.entities.values() if e.spaceID == {space}])"
            )
            sample["cells"][cid] = {**metrics, "real": counts[0], "ghost": counts[1],
                                    "player_ghosts": counts[2]}
        samples.append(sample)
        if sample_time - started >= duration:
            return samples
        time.sleep(max(0, min(sample_time + 2, started + duration) - time.monotonic()))


def verify_partition_load(cells, base, bot_consoles, infos, processes, source, player_ids, args, token, output,
                          *, base_info):
    space, owner = source["space"], source["cid"]
    enabled = args.space_partitioning == "true"
    hosts = [owner, *(cid for cid in cells if cid != owner)]
    player_ids = tuple(sorted(player_ids))
    for console in (base, *bot_consoles, *cells.values()):
        console.evaluate("exec('_partition_load_ids = set()')")
        for batch in batches(player_ids):
            console.evaluate(f"_partition_load_ids.update({batch!r})")
    state = f"KBEngine.getSpaceLayout({space})"
    # 切分线离出生点足够远，均匀分布时各组又能离开 Ghost 同步范围。
    x, y, z = cells[owner].evaluate(f"__import__('d_spaces').datas[{source['utype']}]['spawnPos']")
    x, z = round(x) + 1024, round(z) + 1024
    if enabled:
        spec = ("X", x, ("Z", z, hosts[0], hosts[1]), ("Z", z, hosts[2], hosts[3]))
        version = cells[owner].evaluate(f"KBEngine.setSpaceLayout({space}, {spec!r})")
        wait_for("性能场景四 Cell 就绪", lambda: [console.evaluate(state) for console in cells.values()],
                 lambda rows: all(row and row["state"] == "active" and row["version"] == version for row in rows))

    def routes():
        return base.evaluate(
            "[(i, str(e.cell), str(e.client), e.client is not None) for i in _partition_load_ids "
            "for e in (KBEngine.entities.get(i),) if e]"
        )

    clients = {eid: client for eid, _, client, connected in routes() if connected}
    require(set(clients) == set(player_ids), "性能场景缺少已连接玩家")

    def visible_players():
        query = (
            "[(player.id, sorted(eid for eid in _partition_load_ids if eid != player.id and "
            "app.entities.get(eid) is not None)) for app in KBEngine.bots.values() "
            "for player in app.entities.values() if player.id in _partition_load_ids and player.isPlayer()]"
        )
        return [row for console in bot_consoles for row in console.evaluate(query)]

    def inspect_server_aoi(affected):
        evidence = {}
        for cid, console in cells.items():
            for batch in batches(tuple(affected)):
                evidence.update(console.evaluate(
                    f"{{e.id: {{'cellapp': {cid}, 'position': list(e.position), 'radius': e.getViewRadius(), "
                    "'witness': sorted(p.id for p in e.entitiesInView(True) if p.id in _partition_load_ids), "
                    "'local': sorted(p.id for p in KBEngine.entities.values() if p.id in _partition_load_ids "
                    f"and p.spaceID == {space})}} for i in {batch!r} "
                    "for e in (KBEngine.entities.get(i),) if e and e.isReal() and e.hasWitness}"
                ))
        return evidence

    def check_player_aoi(expected, actual):
        missing, unexpected = {}, {}
        for eid in player_ids:
            wanted, seen = set(expected[eid]), set(actual.get(eid, ()))
            if wanted - seen:
                missing[eid] = sorted(wanted - seen)
            if seen - wanted:
                unexpected[eid] = sorted(seen - wanted)
        result = {"missing_player_aoi": missing, "unexpected_player_aoi": unexpected,
                  "missing_observers": sorted(set(player_ids) - actual.keys())}
        if missing or unexpected or result["missing_observers"]:
            results["player_aoi_passed"] = False
            result["server_aoi"] = inspect_server_aoi(missing.keys() | unexpected.keys())
        return result

    def local_players():
        return {
            cid: console.evaluate(
                f"sorted((e.id, e.hasWitness, list(e.position)) for e in KBEngine.entities.values() "
                f"if e.spaceID == {space} and e.id in _partition_load_ids and e.isReal())"
            )
            for cid, console in cells.items()
        }

    def positions(distance, flip_x=False):
        placed = {}
        for index, eid in enumerate(player_ids):
            quadrant, slot = index % 4, index // 4
            sign_x = -1 if quadrant < 2 else 1
            if flip_x:
                sign_x = -sign_x
                quadrant ^= 2
            sign_z = -1 if quadrant % 2 == 0 else 1
            placed[eid] = (
                hosts[quadrant] if enabled else owner,
                (x + sign_x * (distance + slot % 8), y, z + sign_z * (distance + slot // 8)),
            )
        return placed

    def place(placed):
        started = time.monotonic()
        before = local_players()
        require(sorted(eid for rows in before.values() for eid, _, _ in rows) == list(player_ids),
                "移动前玩家 Real 重复或缺失")
        expected = {cid: [eid for eid, (host, _) in placed.items() if host == cid] for cid in cells}
        for cid, rows in before.items():
            points = [(eid, placed[eid][1]) for eid, _, _ in rows]
            for batch in batches(points):
                cells[cid].evaluate(
                    f"[(setattr(e, 'controlledBy', None), setattr(e, 'position', point)) "
                    f"for i, point in {batch!r} for e in (KBEngine.entities[i],)]"
                )

        def arrived(current):
            return all([eid for eid, _, _ in rows] == expected[cid] and all(
                witness and all(abs(a - b) < 0.001 for a, b in zip(position, placed[eid][1]))
                for eid, witness, position in rows
            ) for cid, rows in current.items())

        wait_for("玩家 Real 到达指定 Cell", local_players, arrived)
        wait_for("越界玩家 Base 路由和客户端连接正常", routes,
                 lambda rows: len(rows) == len(placed) and all(
                     f"cellapp[{placed[eid][0]}]" in cell and clients[eid] == client and connected
                     for eid, cell, client, connected in rows))
        return round(time.monotonic() - started, 3)

    results = {"bots": len(player_ids), "bot_processes": len(bot_consoles),
               "space": space, "enabled": enabled, "intersection": [x, y, z],
               "player_aoi_passed": True, "tcp_drained": True,
               "position_updates_per_second": 1 / MOVE_INTERVAL, "phases": {}}
    sampler = ProcessGroupCollector(processes)
    collector = WatcherCollector(args.repository_root / "kbe/tools/server", admin_token=token)
    try:
        for label, distance in (("uniform", 1024), ("hotspot", 8)):
            placed = positions(distance)
            place(placed)
            expected_aoi = {
                eid: [other for other in player_ids if other != eid and (
                    label == "hotspot" or abs(placed[eid][1][0] - placed[other][1][0]) < 16
                    and abs(placed[eid][1][2] - placed[other][1][2]) < 16
                )] for eid in player_ids
            }
            groups = defaultdict(dict)
            for eid, (cid, point) in placed.items():
                groups[cid][eid] = point
            timers = {}
            try:
                for cid, group in groups.items():
                    # 定时器在 CellApp 内批量更新位置，采样器不用逐 Tick 发送控制台命令。
                    driver = next(iter(group))
                    cells[cid].evaluate("exec('_partition_load_points = {}')")
                    for batch in batches(list(group.items())):
                        cells[cid].evaluate(f"_partition_load_points.update({batch!r})")
                    setup = f"""
from Avatar import Avatar
_partition_load_previous_timer = Avatar.onTimer
_partition_load_steps = 0
def _partition_load_move(self, tid, user_arg):
    global _partition_load_steps
    if self.id != {driver} or tid != _partition_load_timer:
        return _partition_load_previous_timer(self, tid, user_arg)
    _partition_load_steps += 1
    offset = 0.5 if _partition_load_steps % 2 else -0.5
    for eid, (x, y, z) in _partition_load_points.items():
        entity = KBEngine.entities[eid]
        entity.position = (x + offset, y, z)
_partition_load_timer = KBEngine.entities[{driver}].addTimer({MOVE_INTERVAL}, {MOVE_INTERVAL}, 0)
Avatar.onTimer = _partition_load_move
"""
                    timer = cells[cid].evaluate(f"(exec({setup!r}), _partition_load_timer)[1]")
                    timers[cid] = (driver, timer)
                    require(timer, f"CellApp {cid} 未启动位置更新定时器")
                samples = sample_distribution(collector, sampler, cells, infos, space, args.partition_load_seconds)
                steps = {cid: cells[cid].evaluate("_partition_load_steps") for cid in timers}
                require(all(value > 0 for value in steps.values()), "性能场景未更新玩家坐标")
            finally:
                for cid, (driver, timer) in timers.items():
                    cells[cid].evaluate(
                        f"(setattr(Avatar, 'onTimer', _partition_load_previous_timer), "
                        f"KBEngine.entities[{driver}].delTimer({timer}) if {bool(timer)!r} else None)"
                    )
            expected_ghosts = len(player_ids) * 3 if enabled and label == "hotspot" else 0
            require(all(sum(cell["player_ghosts"] for cell in row["cells"].values()) == expected_ghosts
                        for row in samples), "采样期间玩家 Ghost 数与预期分布不符")
            phase = {
                "samples": samples, "movement_steps_with_warmup": steps,
                **check_player_aoi(expected_aoi, dict(visible_players())),
            }
            results["phases"][label] = phase
            print(f"SPACE_PARTITION_LOAD_SAMPLED distribution={label} bots={len(player_ids)} "
                  f"aoi_missing={len(phase['missing_player_aoi'])} "
                  f"aoi_unexpected={len(phase['unexpected_player_aoi'])}", flush=True)

        if enabled and args.partition_crossings:
            crossings, memory = [], []
            base_targets = {
                name: WatcherTarget("BASEAPP_TYPE", base_info["host"], base_info["internal_port"], path)
                for name, path in (
                    ("tcp", "root/network/poller"),
                    ("bundle_pool", "root/objectPools/Bundle"),
                    ("packet_pool", "root/objectPools/TCPPacket"),
                    ("stream_pool", "root/objectPools/MemoryStream"),
                )
            }
            started = time.monotonic()
            for round_number in range(1, args.partition_crossings + 1):
                crossings.append(place(positions(8, flip_x=round_number % 2 == 1)))
                if round_number % 10 == 1 or round_number == args.partition_crossings:
                    memory.append({
                        "round": round_number,
                        "processes": {name: asdict(value) for name, value in sampler.sample().items()},
                        "baseapp": {name: collector.query(target) for name, target in base_targets.items()},
                    })
                if round_number % 100 == 0:
                    print(f"SPACE_PARTITION_CROSSINGS completed={round_number} "
                          f"total={args.partition_crossings}", flush=True)
            crossing_seconds = time.monotonic() - started
            # 单个 Bots 进程跑满一核时，客户端读完积压可能比实体交接慢。
            # 记录排空耗时，再核对 AOI，区分消息迟到和真正漏发。
            before_drain = dict(visible_players())
            drain_started = time.monotonic()
            drain_samples = []
            while True:
                backlog = collector.query(base_targets["tcp"])["tcpSendBacklogBytes"]
                elapsed = time.monotonic() - drain_started
                drain_samples.append({"seconds": round(elapsed, 3), "bytes": backlog})
                if backlog == 0 or elapsed >= 180:
                    break
                time.sleep(1)
            results["tcp_drained"] = backlog == 0
            deadline = time.monotonic() + 12
            seen = dict(visible_players())
            while (set(seen) != set(player_ids) or any(len(visible) != len(player_ids) - 1
                                                      for visible in seen.values())) and time.monotonic() < deadline:
                time.sleep(0.2)
                seen = dict(visible_players())
            final_aoi = check_player_aoi(expected_aoi, seen)
            drained_processes = {name: asdict(value) for name, value in sampler.sample().items()}
            place(positions(1024))
            wait_for(
                "连续越界后远端玩家 Ghost 已回收",
                lambda: [console.evaluate(
                    "any(e.id in _partition_load_ids and not e.isReal() for e in KBEngine.entities.values())"
                ) for console in cells.values()],
                lambda present: not any(present),
            )
            results["crossings"] = {"round_seconds": crossings,
                                    "elapsed_seconds": round(crossing_seconds, 3),
                                    "process_samples": memory, "tcp_drain_samples": drain_samples,
                                    "processes_after_drain": drained_processes, "ghosts_released": True,
                                    "missing_before_drain": sum(len(set(expected_aoi[eid]) -
                                                                    set(before_drain.get(eid, ())))
                                                                for eid in player_ids),
                                    **final_aoi}
    finally:
        collector.close()
    path = output / "space-partition-load.json"
    path.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return path.name, results["player_aoi_passed"] and results["tcp_drained"]
