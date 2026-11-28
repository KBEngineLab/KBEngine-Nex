"""Drive the same two-Cell, four-lane TCP movement test as the NexNet runner."""

import asyncio
import json
from pathlib import Path
import random
import time


LANES = 4
PLAYERS_PER_LANE = 24
MOVERS_PER_LANE = 20
WAVES = 6


class BotWave:
    def __init__(self, players):
        import KBEngine

        ids = set(players)
        self.clients = {
            player.id: app
            for app in KBEngine.bots.values()
            for player in app.entities.values()
            if player.isPlayer() and player.id in ids
        }
        if set(self.clients) != ids:
            raise RuntimeError("The TCP bot process does not own all benchmark clients")
        self.sent = {}

    def view(self):
        return {
            player: sorted(eid for eid in self.clients if eid != player and app.entities.get(eid) is not None)
            for player, app in self.clients.items()
        }

    def place(self, positions):
        for eid, point in positions:
            player = self.clients[eid].entities.get(eid)
            player.testType = 1
            player.cancelController("Movement")
            self.clients[eid].stageMeasuredPosition(*point)

    def arm(self, positions, order, window_ms):
        self.sent = {}
        observers = {}
        for mover, observer, point in positions:
            x, _, z = point
            self.clients[observer].watchMeasuredPosition(mover, x, z)
            observers[mover] = (observer, point)
        self.observers = observers
        loop = asyncio.get_event_loop()
        start = loop.time() + 0.05
        for index, mover in enumerate(order):
            loop.call_at(start + index * window_ms / (1000 * (len(order) - 1)), self.send, mover)

    def send(self, mover):
        _, point = self.observers[mover]
        self.sent[mover] = self.clients[mover].sendMeasuredPosition(*point)

    def read(self):
        observed = {
            observer: dict(self.clients[observer].readMeasuredPositions())
            for observer in {observer for observer, _ in self.observers.values()}
        }
        return [
            [mover, observer, self.sent.get(mover, 0), observed[observer].get(mover, 0)]
            for mover, (observer, _) in self.observers.items()
        ]

    def diagnosed_positions(self, movers):
        result = {}
        for mover in movers:
            observer, _ = self.observers[mover]
            remote = self.clients[observer].entities.get(mover)
            local = self.clients[mover].entities.get(mover)
            result[mover] = [list(local.position) if local is not None else None,
                             list(remote.position) if remote is not None else None]
        return result


def verify_boundary_compare(cells, base, bots, source, player_ids, window_ms, output):
    from space_layout_runtime import require, wait_for
    from space_partition_load import batches

    space, owner = source["space"], source["cid"]
    target, = set(cells) - {owner}
    west = cells[owner]
    players = sorted(player_ids)
    require(len(players) == LANES * PLAYERS_PER_LANE, "跨引擎对照人数不正确")
    sx, y, sz = west.evaluate(f"__import__('d_spaces').datas[{source['utype']}]['spawnPos']")
    x, z = round(sx) + 1024, round(sz) + 1024
    version = west.evaluate(f"KBEngine.setSpaceLayout({space}, {('X', x, owner, target)!r})")
    wait_for("对照布局应用", lambda: [cell.evaluate(f"KBEngine.getSpaceLayout({space})") for cell in cells.values()],
             lambda rows: all(row and row["state"] == "active" and row["version"] == version for row in rows))
    rows = []
    expected_view = {}
    for lane in range(LANES):
        group = players[lane * PLAYERS_PER_LANE:(lane + 1) * PLAYERS_PER_LANE]
        for index, eid in enumerate(group):
            point = (x - 8, y, z + lane * 160 + index)
            rows.append((eid, group[MOVERS_PER_LANE], point, index < MOVERS_PER_LANE))
            expected_view[str(eid)] = [other for other in group if other != eid]

    placement = """
def _compare_place(rows):
    for eid, point in rows:
        e = KBEngine.entities[eid]
        e.controlledBy = None
        e.cancelController("Movement")
        e.position = point
        e.topSpeed = 1000
        e.setViewRadius(80, 5)
"""
    probe_file = str(Path(__file__).resolve())
    setup = f"from runpy import run_path\n_compare_bot = run_path({probe_file!r})['BotWave']({players!r})"
    bots.evaluate(f"exec({setup!r})")
    initial_positions = [(eid, point) for eid, _, point, _ in rows]
    for batch in batches(initial_positions):
        bots.evaluate(f"_compare_bot.place({batch!r})")
    time.sleep(0.4)
    west.evaluate(f"exec({placement!r})")
    for batch in batches(initial_positions):
        west.evaluate(f"_compare_place({batch!r})")
    wait_for("每条线路仅看到本组其余 23 名玩家", lambda: bots.evaluate("_compare_bot.view()"),
             lambda view: view == expected_view)
    for batch in batches(players):
        west.evaluate(f"[setattr(KBEngine.entities[i], 'controlledBy', KBEngine.entities[i].base) for i in {batch!r}]")

    base.evaluate("exec('_compare_ids = set()')")
    for cell in cells.values():
        cell.evaluate("exec('_compare_ids = set()')")
    for batch in batches(players):
        for console in (base, *cells.values()):
            console.evaluate(f"_compare_ids.update({batch!r})")

    def routes():
        return {eid: (route, connection) for eid, route, connection, connected in base.evaluate(
            "[(e.id, str(e.cell), str(e.client), e.client is not None) for e in KBEngine.entities.values() "
            "if e.id in _compare_ids]") if connected}

    def reals():
        return [
            (eid, cid, point)
            for cid, cell in cells.items()
            for eid, point in cell.evaluate(
                "[(e.id, list(e.position)) for e in KBEngine.entities.values() "
                "if e.id in _compare_ids and e.isReal() and e.hasWitness]")
        ]

    def verify_owners(positions, east):
        expected = {eid: (target if east and moves else owner) for eid, _, _, moves in rows}
        wait_for("Real 角色和权威位置到达目标 Cell", reals,
                 lambda actual: sorted(eid for eid, _, _ in actual) == players and all(
            cid == expected[eid] and abs(point[0] - positions[eid][0]) < 0.01
            and abs(point[2] - positions[eid][2]) < 0.01 for eid, cid, point in actual))
        wait_for("Base 路由指向目标且仍使用原连接", routes, lambda actual: actual.keys() == expected.keys()
                 and all(f"cellapp[{expected[eid]}]" in route and connection == initial_routes[eid][1]
                         for eid, (route, connection) in actual.items()))

    initial_routes = routes()
    require(set(initial_routes) == set(players), "对照开始前存在未连接角色")
    verify_owners({eid: point for eid, _, point, _ in rows}, False)
    time.sleep(10)  # 和 NexNet 一样先等初始同步排空，不跑预热迁移。
    wave_path = output / "boundary-observer-samples.json"
    wave_results = []
    for wave in range(1, WAVES + 1):
        east = wave % 2 == 1
        destinations = {eid: (x + 8 if east and moves else point[0], point[1], point[2])
                        for eid, _, point, moves in rows}
        moves = [(eid, observer, destinations[eid]) for eid, observer, _, moves in rows if moves]
        order = [entry[0] for entry in moves]
        random.Random(wave).shuffle(order)
        # 注入数据使用有界短命令；进程内计时与发包，不把控制台往返算进样本。
        bots.evaluate("exec('_compare_moves = []\\n_compare_order = []')")
        for batch in batches(moves):
            bots.evaluate(f"_compare_moves.extend({batch!r})")
        for batch in batches(order):
            bots.evaluate(f"_compare_order.extend({batch!r})")
        bots.evaluate(f"_compare_bot.arm(_compare_moves, _compare_order, {window_ms})")
        time.sleep(window_ms / 1000 + 0.30)
        deadline = time.monotonic() + 5
        while True:
            raw = bots.evaluate("_compare_bot.read()")
            if all(sent and received >= sent for _, _, sent, received in raw) or time.monotonic() >= deadline:
                break
            time.sleep(0.1)
        valid = [received - sent for _, _, sent, received in raw if sent and received >= sent]
        sent_times = [sent for _, _, sent, _ in raw if sent]
        wave_results.append({
            "wave": wave, "sent_spread_ms": (max(sent_times) - min(sent_times)) / 1e6 if sent_times else None,
            "missing": len(raw) - len(valid), "samples": raw,
        })
        if len(valid) != len(moves):
            examples = [eid for eid, _, sent, received in raw if not sent or received < sent][:8]
            wave_results[-1]["sample_diagnostics"] = {
                "expected": {eid: destinations[eid] for eid in examples},
                "real": [row for row in reals() if row[0] in examples],
                "client": bots.evaluate(f"_compare_bot.diagnosed_positions({examples!r})"),
            }
        wave_path.write_text(json.dumps({
            "players": len(players), "crossers": len(moves), "window_ms": window_ms,
            "position_radius": 80, "space": space, "source_cellapp": owner, "target_cellapp": target,
            "waves": wave_results,
        }, indent=2), encoding="utf-8")
        require(len(valid) == len(moves), f"第 {wave} 波客户端样本缺失，详见 {wave_path}")
        verify_owners(destinations, east)
        wait_for("越界后保持相同 AOI 关系", lambda: bots.evaluate("_compare_bot.view()"),
                 lambda view: view == expected_view)
        print(f"BOUNDARY_COMPARE wave={wave} received={len(valid)} path={wave_path}", flush=True)
    bots.evaluate("exec('del _compare_bot, _compare_moves, _compare_order')")
    return wave_path.name, True
