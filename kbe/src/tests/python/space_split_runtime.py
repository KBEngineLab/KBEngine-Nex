"""在真实集群中验证新增 Cell、更新 Layout、同步资源和迁移实体。"""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, ExitStack
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


@contextmanager
def pause_console(console, output, label, on_resume="", *, timeout=20):
    held = output / f"{label}-held"
    release = output / f"{label}-release"
    hold = f"""
from pathlib import Path
from time import monotonic, sleep
Path({str(held)!r}).touch()
_hold_until = monotonic() + {timeout}
while not Path({str(release)!r}).exists() and monotonic() < _hold_until:
    sleep(.02)
{on_resume}
"""
    with ThreadPoolExecutor(max_workers=1) as executor:
        waiting = executor.submit(console.evaluate, f"exec({hold!r})", timeout + 5)
        try:
            wait_for("等待进程暂停处理消息", held.exists, bool)
            yield
        finally:
            release.touch()
        waiting.result()


class LiveSpaceAdjustment:
    def __init__(self, cells, base, bots, space):
        self.cells = cells
        self.base = base
        self.bots = bots
        self.space = space
        self.checkpoints = []

    def prepare(self, owner, mover, position, revision):
        source = self.cells[owner]
        observers = source.evaluate(
            f"sorted(e.id for e in KBEngine.entities.values() if e.id != {mover} and "
            f"e.spaceID == {self.space} and e.isReal() and e.hasWitness)"
        )
        require(len(observers) == 1, f"调区检查需要一名留在属主 Cell 的观察者：{observers!r}")
        self.observer, = observers
        self.mover = mover
        self.split_model_scale = source.evaluate(f"KBEngine.entities[{mover}].modelScale") ^ 1
        pairs = ((mover, self.observer), (self.observer, mover))
        players = (mover, self.observer)
        source.evaluate(
            f"(setattr(KBEngine.entities[{self.observer}], 'controlledBy', None), "
            f"setattr(KBEngine.entities[{self.observer}], 'position', {position!r}))"
        )
        self.reals = (
            f"sorted((i, e.hasWitness, e.hasWitness and any(p.id == other for p in e.entitiesInView())) "
            f"for i, other in {pairs!r} for e in (KBEngine.entities.get(i),) "
            f"if e and e.spaceID == {self.space} and e.isReal())"
        )
        self.routes = (
            f"[(i, str(e.cell), str(e.client)) for i in {players!r} "
            "for e in (KBEngine.entities.get(i),) if e and e.client is not None]"
        )
        self.clients = {i: client for i, _, client in self.base.evaluate(self.routes)}
        require(self.clients.keys() == set(players), "调区前缺少两名已连接的玩家")
        # 在同一轮控制台调用中比较并保存对象；保留强引用才能发现同 ID 的 Leave 和重建。
        sample = f"""
_space_adjustment_objects = {{}}
def _space_adjustment_sample():
    global _space_adjustment_objects
    current = {{i: (e, app.entities.get(other)) for app in KBEngine.bots.values()
               for i, other in {pairs!r} for e in (app.entities.get(i),) if e and e.isPlayer()}}
    rows = []
    for i, (own, peer) in current.items():
        old_own, old_peer = _space_adjustment_objects.get(i, (own, peer))
        rows.append((i, peer is not None, own is old_own, peer is old_peer))
    if len(rows) == {len(players)} and all(row[1] for row in rows):
        _space_adjustment_objects = current
    return rows
"""
        self.bots.evaluate(f"exec({sample!r})")
        self.check("before_split", revision, owner, owner)
        # 保留独立强引用，后续采样即使发现重建也不能改变这次回归的身份基准。
        capture = f"""
import sys
_entry_object = _space_adjustment_objects[{self.observer}][1]
_entry_calls = []
_entry_previous_profile = sys.getprofile()
def _entry_profile(frame, event, arg):
    if event == 'call' and frame.f_code.co_name == 'recvDamage':
        v = frame.f_locals
        if v['self'].id == {mover} and v['attackerID'] == {self.observer}:
            _entry_calls.append((v['damage'], v['self'] is _entry_object, v['self'].modelScale))
sys.setprofile(_entry_profile)
"""
        self.bots.evaluate(f"exec({capture!r})")

    def arm_split_entry(self, candidates):
        # 目标回调发生在源端确认释放之后。让等待跨过旧的 5 秒期限，再制造普通扫描积压。
        setup = f"""
from Avatar import Avatar
from time import monotonic, sleep

_space_adjustment_enter_local = Avatar.__dict__.get('onEnteredCell')
_space_adjustment_enter_original = getattr(Avatar, 'onEnteredCell', None)
_space_adjustment_enter_count = 0
_space_backlog = []
def _space_adjustment_enter(self, *args):
    global _space_adjustment_enter_count
    if self.id == {self.mover}:
        _space_adjustment_enter_count += 1
        sleep(6)
    if _space_adjustment_enter_original is not None:
        _space_adjustment_enter_original(self, *args)
    if self.id == {self.mover}:
        self.modelScale = {self.split_model_scale}
        self.otherClients.recvDamage({self.observer}, 1, 1, 71)
        pos = (self.position.x, self.position.y, self.position.z + 200)
        for _ in range(384):
            _space_backlog.append(KBEngine.createEntity('NPC', self.spaceID, pos, (0., 0., 0.), {{}}).id)
        globals()['_space_backlog_start'] = monotonic()

Avatar.onEnteredCell = _space_adjustment_enter
"""
        for cid in sorted(candidates):
            self.cells[cid].evaluate(f"exec({setup!r})")

    def finish_split_entry(self, candidates, target, owner):
        restore = """
if _space_adjustment_enter_local is None:
    del Avatar.onEnteredCell
else:
    Avatar.onEnteredCell = _space_adjustment_enter_local
"""
        count = self.cells[target].evaluate("_space_adjustment_enter_count")
        for cid in sorted(candidates):
            self.cells[cid].evaluate(f"exec({restore!r})")
        require(count == 1, f"未按预期在扩容目标执行一次进入回调：{count}")
        self.cells[target].evaluate(f"KBEngine.entities[{self.mover}].otherClients.recvDamage({self.observer}, 1, 1, 73)")
        self.entry_calls = wait_for("原观察者按序收到进入回调 RPC 和结束标记",
                                    lambda: self.bots.evaluate("_entry_calls"),
                                    lambda rows: any(row[0] == 73 for row in rows), timeout=10)
        require(self.entry_calls == [[n, True, self.split_model_scale] for n in (71, 73)],
                f"进入回调 RPC 次数、客户端对象或属性顺序错误：{self.entry_calls}")
        # 对端必须接收到新 Real 的属性，不能靠暂留旧坐标节点通过对象身份检查。
        wait_for(
            "扩容后源端 Bot 收到新 Real 的 Ghost 快照",
            lambda: self.bots.evaluate(
                f"[app.entities[{self.mover}].modelScale for app in KBEngine.bots.values() "
                f"if {self.observer} in app.entities and app.entities[{self.observer}].isPlayer() "
                f"and {self.mover} in app.entities]"
            ),
            lambda rows: rows == [self.split_model_scale],
        )
        # 已到的普通副本证明连接在工作；未到的副本证明新玩家快照赶在普通队列排空之前。
        backlog = self.cells[target].evaluate("_space_backlog")
        elapsed = self.cells[target].evaluate("monotonic() - _space_backlog_start")
        time.sleep(max(0, 5.2 - elapsed))
        received = self.cells[owner].evaluate(f"sum(i in KBEngine.entities for i in {backlog!r})")
        require(0 < received < len(backlog), f"未观察到持续五秒的普通 Ghost 刷新积压：{received}/{len(backlog)}")
        self.backlog = {"ordinary_received": received, "ordinary_total": len(backlog)}
        self.cells[target].evaluate("exec('for i in _space_backlog:\\n    KBEngine.entities[i].destroy()')")

    def chain_handoff(self, owner, middle, last, position, output, revision):
        a, b, c = (self.cells[cid] for cid in (owner, middle, last))
        mover = f"KBEngine.entities[{self.mover}]"
        start = b.evaluate(f"list({mover}.position)")
        installed = output / "chain-installing"
        resume = output / "chain-resume-install"
        traffic_pos = (position[0], position[1], position[2] + 2000.)
        traffic = c.evaluate(f"KBEngine.createEntity('NPC', {self.space}, {traffic_pos!r}, (0., 0., 0.), {{}}).id")
        wait_for("B 已建立压力测试 NPC 的 Ghost", lambda: b.evaluate(f"{traffic} in KBEngine.entities"), bool)
        # B 没有读时用另一实体的公共属性填充同一可靠连接，C 随后才处理 FINISH_TARGET。
        fill = (f"def _fill_copy_queue():\n    entity = KBEngine.entities[{traffic}]\n"
                "    payload = 'Q' * (96 * 1024)\n"
                "    for i in range(128):\n        entity.name = payload + str(i)\n_fill_copy_queue()")
        hook = f"""
from pathlib import Path
from time import monotonic, sleep
from Avatar import Avatar
_chain_local = Avatar.__dict__.get('__setattr__')
_chain_setattr = Avatar.__setattr__
_chain_events = []
_chain_callbacks = {{name: Avatar.__dict__.get(name) for name in ('onEnteredCell', 'onWitnessed')}}
_chain_enter = getattr(Avatar, 'onEnteredCell', None)
_chain_observed = getattr(Avatar, 'onWitnessed', None)
def _chain_record_enter(self):
    if _chain_enter:
        _chain_enter(self)
    if self.id == {self.mover}:
        _chain_events.append('entered')
        self.otherClients.recvDamage({self.observer}, 1, 1, 77)
def _chain_record_observed(self, observed):
    if _chain_observed:
        _chain_observed(self, observed)
    if self.id == {self.mover}:
        _chain_events.append(observed)
Avatar.onEnteredCell = _chain_record_enter
Avatar.onWitnessed = _chain_record_observed
def _chain_hold(self, key, value):
    if self.id == {self.mover} and self.isReal() and not Path({str(installed)!r}).exists():
        Path({str(installed)!r}).touch()
        deadline = monotonic() + 20
        while not Path({str(resume)!r}).exists() and monotonic() < deadline:
            sleep(.02)
    return _chain_setattr(self, key, value)
Avatar.__setattr__ = _chain_hold
"""
        c.evaluate(f"exec({hook!r})")
        try:
            b.evaluate(f"setattr({mover}, 'position', {position!r})")
            wait_for("C 已开始接收来自 B 的交接", installed.exists, bool, timeout=12)
            # 第一次 CHECK 可能早于快照。C 仍在安装时，让 Base 的下一轮 DISPATCH 和 CHECK 先入队。
            counter = "KBEngine.getWatcher('root/network/messages/Cellapp::onCellHandoff/sentCount')"
            sent = self.base.evaluate(counter)
            wait_for("Base 已重发本轮目标检查", lambda: self.base.evaluate(counter),
                     lambda count: count >= sent + 2, timeout=6)
            # 暂停 Base 让 C 完成安装，再暂停 C；此时放行 Base 就能让 B 提交，C 仍不能排首份快照。
            with ExitStack() as slow_peer:
                with ExitStack() as gates:
                    with pause_console(self.base, output, "chain-base"):
                        resume.touch()
                        require(c.evaluate(f"{mover}.isReal()"), "C 未安装 Real")
                        gates.enter_context(pause_console(c, output, "chain-target", on_resume=fill))
                    wait_for("B 已释放交接源", lambda: b.evaluate(f"{mover}.base is None"), bool, timeout=8)
                    # 跨过占位查询周期，A、B 和 Bots 均继续处理消息。
                    time.sleep(5.5)
                    require(a.evaluate(f"any(p.id == {self.mover} for p in KBEngine.entities[{self.observer}].entitiesInView())")
                            and self.bots.evaluate(f"any(app.entities.get({self.mover}) is _entry_object "
                                                   "for app in KBEngine.bots.values())"),
                            "C 恢复前 A 上的观察者已失去原对象")
                    slow_peer.enter_context(pause_console(b, output, "chain-slow-peer"))
                calls = wait_for("B 仍暂停时 A 收到 C 的进入回调广播", lambda: self.bots.evaluate("_entry_calls"),
                                 lambda rows: any(row[0] == 77 for row in rows), timeout=8)
                self.chain_entry_calls = self.entry_calls + [[77, True, self.split_model_scale]]
                require(calls == self.chain_entry_calls, f"快端回调顺序或对象错误：{calls}")
                wait_for("新 Real 收到快端首次观察通知", lambda: c.evaluate("_chain_events"),
                         lambda rows: rows == ["entered", True], timeout=5)
                log = output / "server/logs/logger_cellapp.log"
                prefix = f"reserved send, entity={self.mover}, peer={middle}, queued="
                self.slow_peer_bytes = wait_for("B 超过普通 Ghost 发送水位",
                    lambda: max((int(line.split(prefix)[1].rstrip(".")) for line in
                                 log.read_text(encoding="utf-8", errors="replace").splitlines() if prefix in line),
                                default=0), lambda size: size > 512 * 1024, timeout=3)
            self.check("chain_a_b_c", revision, last, owner)
            value = self.split_model_scale ^ 1
            c.evaluate(f"(setattr({mover}, 'modelScale', {value}), {mover}.otherClients.recvDamage({self.observer}, 1, 1, 79))")
            calls = wait_for("C 的属性和 RPC 抵达 A 原对象", lambda: self.bots.evaluate("_entry_calls"),
                             lambda rows: any(row[0] == 79 for row in rows))
            require(calls == self.chain_entry_calls + [[79, True, value]] and c.evaluate("_chain_events") == ["entered", True],
                    f"连续交接后的回调错误：{calls}")
            c.evaluate(f"setattr({mover}, 'position', {start!r})")
            self.check("chain_return", revision, middle, owner)
        finally:
            resume.touch(exist_ok=True)
            c.evaluate(f"KBEngine.entities[{traffic}].destroy()")
            c.evaluate("exec('for name, cb in _chain_callbacks.items():\\n    if cb is None:\\n"
                       "        delattr(Avatar, name)\\n    else:\\n        setattr(Avatar, name, cb)')")
            c.evaluate("exec('if _chain_local is None:\\n    del Avatar.__setattr__\\n"
                       "else:\\n    Avatar.__setattr__ = _chain_local')")

    @contextmanager
    def check_promoted_observation(self, owner):
        cell = self.cells[owner]
        mover = f"KBEngine.entities[{self.mover}]"
        require(cell.evaluate(f"not {mover}.isReal() and {mover}.isWitnessed"),
                "需要已被本地观察者看见的目标 Ghost")
        hook = f"""
from Avatar import Avatar
_promoted_events = []
_promoted_saved = {{name: Avatar.__dict__.get(name) for name in ('onEnteredCell', 'onWitnessed')}}
_promoted_enter = getattr(Avatar, 'onEnteredCell', None)
_promoted_witness = getattr(Avatar, 'onWitnessed', None)
def _promoted_on_enter(self):
    if self.id == {self.mover}:
        _promoted_events.append(['enter', self.isWitnessed])
    if _promoted_enter:
        _promoted_enter(self)
def _promoted_on_witness(self, observed):
    if self.id == {self.mover}:
        _promoted_events.append(['observed', observed])
    if _promoted_witness:
        _promoted_witness(self, observed)
Avatar.onEnteredCell = _promoted_on_enter
Avatar.onWitnessed = _promoted_on_witness
"""
        cell.evaluate(f"exec({hook!r})")
        try:
            yield
            self.promoted_events = cell.evaluate("_promoted_events")
            require(self.promoted_events == [["enter", True], ["observed", True]],
                    f"接收时已有的观察状态没有在进入回调后派发一次：{self.promoted_events}")
        finally:
            cell.evaluate("exec('for name, cb in _promoted_saved.items():\\n    if cb is None:\\n"
                          "        delattr(Avatar, name)\\n    else:\\n        setattr(Avatar, name, cb)')")

    def check_rollback_observation(self, source, remote, avatar, boundary, y, z, output):
        player = f"KBEngine.entities[{avatar}]"
        witness = f"KBEngine.entities[{self.observer}]"
        original = remote.evaluate(f"list({witness}.position)")
        near, far = (boundary - 8., y, z), (boundary - 8., y, z + 2000.)
        hook = f"""
from Avatar import Avatar
_rollback_events = []
_rollback_local = Avatar.__dict__.get('onWitnessed')
_rollback_previous = getattr(Avatar, 'onWitnessed', None)
def _rollback_witness(self, observed):
    if self.id == {avatar}:
        _rollback_events.append(observed)
    if _rollback_previous:
        _rollback_previous(self, observed)
Avatar.onWitnessed = _rollback_witness
"""
        source.evaluate(f"exec({hook!r})")
        try:
            remote.evaluate(f"setattr({witness}, 'position', {near!r})")
            wait_for("交接前玩家已被远端观察", lambda: source.evaluate(f"{player}.isWitnessed"), bool)
            source.evaluate("_rollback_events.clear()")
            expected = []
            for label, changes in (("leave", [False]), ("enter", [True]), ("reverse", [False, True])):
                with pause_console(self.base, output, f"rollback-witness-{label}"):
                    require(source.evaluate(f"(setattr({player}, 'position', {near!r}), "
                                            f"KBEngine.requestCellHandoff({avatar}))[1]"), "交接未入队")
                    wait_for("源端已等待 Base 放行", lambda: captured_error(
                        source, f"KBEngine.requestCellHandoff({avatar})"), lambda err: err and err[0] == "ValueError")
                    for observed in changes:
                        remote.evaluate(f"setattr({witness}, 'position', {(near if observed else far)!r})")
                        wait_for("源已处理在途观察消息", lambda: source.evaluate(f"{player}.isWitnessed"),
                                 lambda value: value == observed, timeout=6)
                    require(source.evaluate("_rollback_events") == expected, "交接期间提前派发了观察回调")
                    source.evaluate(f"setattr({player}, 'position', {(boundary + 8., y, z)!r})")
                if len(changes) == 1:
                    expected += changes
                wait_for("回滚后观察状态收敛", lambda: source.evaluate("_rollback_events"),
                         lambda rows: rows == expected)
            self.rollback_events = source.evaluate("_rollback_events")
        finally:
            remote.evaluate(f"setattr({witness}, 'position', {original!r})")
            source.evaluate("exec('if _rollback_local is None:\\n    del Avatar.onWitnessed\\n"
                            "else:\\n    Avatar.onWitnessed = _rollback_local')")

    def check(self, phase, revision, mover_host, observer_host):
        assignments = {self.mover: mover_host, self.observer: observer_host}
        expected = {
            cid: [[i, True, True] for i, host in sorted(assignments.items()) if host == cid] for cid in self.cells
        }
        wait_for(
            f"{phase} 后两名玩家各有唯一 Real 且 Witness 互相可见",
            lambda: {cid: cell.evaluate(self.reals) for cid, cell in self.cells.items()},
            lambda rows: rows == expected,
        )
        wait_for(
            f"{phase} 后两名玩家的 Base 路由和连接",
            lambda: self.base.evaluate(self.routes),
            lambda rows: len(rows) == len(assignments) and all(
                f"cellapp[{assignments[i]}]" in route and client == self.clients[i] for i, route, client in rows
            ),
        )
        objects = wait_for(
            f"{phase} 后两名 Bot 互相可见",
            lambda: self.bots.evaluate("_space_adjustment_sample()"),
            lambda rows: {i for i, visible, _, _ in rows if visible} == assignments.keys(),
        )
        others = {self.mover: self.observer, self.observer: self.mover}
        changed = {i: [eid for eid, same in ((i, same_own), (others[i], same_peer)) if not same]
                   for i, _, same_own, same_peer in objects if not (same_own and same_peer)}
        # 重建会使整轮失败，但先记录结果，让其余调区操作和正常关服仍能得到验证。
        self.checkpoints.append({"phase": phase, "boundary_revision": revision, "real_cellapps": assignments,
                                 "changed_client_objects": changed})
        print(f"SPACE_ADJUSTMENT_AOI phase={phase} revision={revision} reals={assignments} "
              f"changed_client_objects={changed}", flush=True)

    def swap_hosts(self, owner, target, output, revision):
        players = ((self.mover, target), (self.observer, owner))
        original = [self.cells[host].evaluate(f"list(KBEngine.entities[{eid}].position)") for eid, host in players]
        for label, reverse in (("overlap", False), ("overlap_return", True)):
            with pause_console(self.base, output, label):
                for index, (eid, _) in enumerate(players):
                    host = players[1 - index if reverse else index][1]
                    pos = original[index if reverse else 1 - index]
                    started = self.cells[host].evaluate(
                        f"(setattr(KBEngine.entities[{eid}], 'position', {pos!r}), "
                        f"KBEngine.requestCellHandoff({eid}))[1]"
                    )
                    require(started, f"玩家 {eid} 的交接未排入队列")
                # 两个源 Cell 都可推进 Tick，而 Base 在此期间无法放行任何一名玩家。
                for eid, host in players:
                    source = self.cells[(owner if host == target else target) if reverse else host]
                    wait_for("玩家已向 Base 发起交接", lambda: captured_error(
                        source, f"KBEngine.requestCellHandoff({eid})"), lambda error: error and error[0] == "ValueError")
            self.check(label, revision, target if reverse else owner, owner if reverse else target)

    def finish(self):
        self.bots.evaluate("exec('sys.setprofile(_entry_previous_profile)\\ndel _entry_object, _space_adjustment_objects, _space_adjustment_sample')")
        return {"mover": self.mover, "observer": self.observer, "checkpoints": self.checkpoints,
                "client_connections_preserved": True, "mutual_aoi_visible_after_each_step": True,
                "held_split_entry_callback": True,
                "entered_cell_rpc": self.entry_calls,
                "slow_peer_queued_bytes_at_finish": self.slow_peer_bytes,
                "promoted_witness_events": self.promoted_events,
                "connected_ghost_backlog": self.backlog,
                "mutual_aoi_objects_preserved": not any(row["changed_client_objects"] for row in self.checkpoints)}


def verify_split_boundary_move(cells, space, owner, target, state, current, cut, base, route, client, avatar, npc, live):
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
        if live is not None:
            live.check("move_boundary", revision, host, owner)
    return moves


def verify_space_split(cells, layout, base, repository, endpoint, environment, output, *, live=None):
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
    if live is not None:
        live.prepare(owner, avatar, (cut - 5.0, y, z), revision=1)
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

    if live is not None:
        live.arm_split_entry(spares)
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
                f"and KBEngine.getSpaceData({space}, 'split_preparing') == 'still-active')({state})", timeout=15
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
    current = target.evaluate(state, timeout=15)
    require(current["hasNavigation"] and current["boundaryRevision"] == 2, "新增 Cell 的导航或边界不正确")
    wait_for("静止 NPC 和在线玩家迁到新增 Cell",
             lambda: target.evaluate(
                 f"all(KBEngine.entities.get(i) and KBEngine.entities[i].isReal() for i in {(avatar, npc)!r})",
                 timeout=15),
             bool)
    routed = wait_for("在线玩家 Base 路由迁到新增 Cell", lambda: base.evaluate(route_query),
                      lambda value: f"cellapp[{target_id}]" in value[0])
    require(routed[1] == client, "扩容更换了在线玩家客户端连接")
    wait_for("源端和目标均完成迁移", lambda: command("show"), lambda row: row["migrationState"] == "complete")
    require(source.evaluate(f"not any(KBEngine.entities.get(i) and KBEngine.entities[i].isReal() "
                            f"for i in {(avatar, npc)!r})"), "源端留下重复 Real")
    if live is not None:
        live.finish_split_entry(spares, target_id, owner)
        live.check("split", current["boundaryRevision"], target_id, owner)
        last, = old.keys() - {owner}
        live.chain_handoff(owner, target_id, last, (boundary + 4., y, z), output, current["boundaryRevision"])
    with ExitStack() as probes:
        if live is not None:
            probes.enter_context(live.check_promoted_observation(owner))
        moves = verify_split_boundary_move(
            cells, space, owner, target_id, state, current, cut, base, route_query, client, avatar, npc, live,
        )
    if live is not None:
        live.swap_hosts(owner, target_id, output, moves[-1]["revision"])
    target.evaluate(f"KBEngine.entities[{npc}].destroy()")
    print(f"SPACE_SPLIT_RUNTIME_PASS space={space} target={target_id}", flush=True)
    return dict(attempt=after["splitAttempt"], cell=cell_apps[target_id], target=target_id,
                original_layout_served=True, preparation_timeout_retried=True,
                resources_current=True, migration_complete=True, boundary_moves=moves)


def verify_space_merge(cells, layout, split, base, repository, endpoint, environment, output,
                       *, expected_server_errors, live=None):
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
    if live is not None:
        # 先让观察 Cell 确认准备，再阻塞它的读取。退役端和接收端仍可完成拒绝和回滚。
        traffic = target.evaluate(
            f"KBEngine.createEntity('NPC', {space}, {(cut - 1., y, z + 2000.)!r}, (0., 0., 0.), {{}}).id")
        slow = cells[peer]
        wait_for("观察端已收到压测实体", lambda: slow.evaluate(f"{traffic} in KBEngine.entities"), bool)
        wait_for("退役玩家在观察端已有副本", lambda: slow.evaluate(f"{avatar} in KBEngine.entities"), bool)
        fill = (f"for i in range(128):\n"
                f"    KBEngine.entities[{traffic}].name = 'Q' * (96 * 1024) + str(i)")
        try:
            with ExitStack() as gate:
                with pause_console(source, output, "merge-budget-preparing"):
                    attempt = merge(retiring, keeping)["mergeAttempt"]
                    ready = f"merge ready, space={space}, attempt={attempt}, cellapp={peer}."
                    manager_log = output / "server/logs/logger_cellappmgr.log"
                    wait_for("观察端已完成准备", lambda: ready in manager_log.read_text(
                        encoding="utf-8", errors="replace"), bool, timeout=8)
                    gate.enter_context(pause_console(slow, output, "merge-budget-slow"))
                    target.evaluate(f"exec({fill!r})")
                wait_for("观察连接积压时有界取消缩容", lambda: command("show"),
                         lambda row: row["mergeState"] == "cancelling", timeout=15)
            failed = wait_for("各参与 Cell 完成取消", lambda: command("show"),
                              lambda row: row["mergeState"] == "failed", timeout=10)
            require(failed["mergeAttempt"] == attempt and failed["cells"] == before["cells"] and
                    failed["boundaryRevision"] == before["boundaryRevision"] and
                    failed["mergeError"] == f"entity {avatar}: target repeatedly rejected handoff before admission"
                    and has_real(source, avatar) and base.evaluate(route)[1] == client,
                    f"预算拒绝后未恢复本轮布局和玩家：{failed}")
            log = (output / "server/logs/logger_cellapp.log").read_text(encoding="utf-8", errors="replace")
            require(log.count(f"copy budget exhausted, entity={avatar},") == 3, "未观察到三次真实预算拒绝")
        finally:
            target.evaluate(f"KBEngine.entities[{traffic}].destroy()")

        # 第三个 Cell 持有仍可见的 Ghost；源和目标可以在它停读时独立完成权威交接。
        witness = f"KBEngine.entities[{live.observer}]"
        observer_pos = target.evaluate(f"list({witness}.position)")
        peer_x = next(row["bounds"][0] for row in before["cells"] if row["cellapp"] == peer)
        target.evaluate(f"setattr({witness}, 'position', {(peer_x + 4., y, z)!r})")
        live.check("before_merge_fence", before["boundaryRevision"], source_id, peer)
        @contextmanager
        def start_with_paused_observer(label, timeout):
            with ExitStack() as gate:
                with pause_console(source, output, label + "-preparing"):
                    request = merge(retiring, keeping)
                    ready = f"merge ready, space={space}, attempt={request['mergeAttempt']}, cellapp={peer}."
                    wait_for("第三个 Cell 确认缩容准备", lambda: ready in manager_log.read_text(
                        encoding="utf-8", errors="replace"), bool)
                    gate.enter_context(pause_console(slow, output, label + "-observer", timeout=timeout))
                yield request

        with start_with_paused_observer("merge-fence-timeout", 55) as request:
            wait_for("观察 Cell 暂停期间玩家已提交交接", lambda: base.evaluate(route),
                     lambda row: f"cellapp[{owner}]" in row[0] and row[1] == client, timeout=12)
            cancelled = wait_for("观察 Cell 未消费换源时取消本轮缩容", lambda: command("show"),
                                 lambda row: row["mergeState"] == "cancelling", timeout=40)
            require(cancelled["mergeAttempt"] == request["mergeAttempt"] and cancelled["cells"] == before["cells"]
                    and cancelled["boundaryRevision"] == before["boundaryRevision"] and
                    cancelled["mergeError"] == f"timed out waiting for Ghost updates on CellApp {peer}",
                    f"观察 Cell 未应答时提前发布或错误取消：{cancelled}")
        wait_for("慢观察端恢复后完成缩容取消", lambda: command("show"), lambda row: row["mergeState"] == "failed")
        live.check("merge_fence_cancelled", before["boundaryRevision"], source_id, peer)
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
    with ExitStack() as gates:
        request = (gates.enter_context(start_with_paused_observer("merge-fence-resume", 35))
                   if live is not None else merge(retiring, keeping))
        try:
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
        if live is not None:
            fence = f"sent Ghost fence, space={space}, attempt={request['mergeAttempt']}, peer={peer}."
            log_path = output / "server/logs/logger_cellapp.log"
            wait_for("旧源已向慢观察端发出最后的换源及其确认标记",
                     lambda: fence in log_path.read_text(encoding="utf-8", errors="replace"), bool, timeout=10)
            require(command("show")["boundaryRevision"] == before["boundaryRevision"],
                    "慢观察端恢复前已发布缩容布局")

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
    live_result = None
    if live is not None:
        live.check("merge", merged["boundaryRevision"], owner, peer)
        # 进入新布局后用一次实际 RPC 验证第三个 Cell 上的原对象已经绑定新的权威副本。
        target.evaluate(f"KBEngine.entities[{avatar}].otherClients.recvDamage({live.observer}, 1, 1, 83)")
        calls = wait_for("缩容后原观察对象接收新 Real 的广播", lambda: live.bots.evaluate("_entry_calls"),
                         lambda rows: any(row[0] == 83 for row in rows))
        require([row[1] for row in calls if row[0] == 83] == [True], f"换源后客户端对象或广播次数错误：{calls}")
        slow.evaluate(f"setattr({witness}, 'position', {observer_pos!r})")
        live.check("merge_observer_return", merged["boundaryRevision"], owner, owner)
        live_result = live.finish()
        live_result["ghost_fence_timeout_peer"] = peer
        live_result["post_merge_rpc_on_original_object"] = True
        live_result["retired_cell_empty"] = True
    target.evaluate(f"KBEngine.entities[{npc}].destroy()")
    target.evaluate("exec('if _merge_previous_enter is None:\\n    del NPC.onEnteredCell\\n"
                    "else:\\n    NPC.onEnteredCell = _merge_previous_enter')")

    # 原右侧范围没有上界；在远处验证合并不会依赖已有目标 Ghost。
    far_x = layout["split_x"] + 10000
    far_source = cells[peer]

    def wait_merge_failed(request, blocked, reason, timeout=45):
        failed = wait_for("快照超限后取消合并", lambda: command("show"),
                          lambda row: row["mergeState"] == "failed", timeout=timeout)
        require(failed["state"] == "active" and failed["version"] == merged["version"] and
                failed["boundaryRevision"] == merged["boundaryRevision"] and failed["cells"] == merged["cells"] and
                failed["mergeAttempt"] == request["mergeAttempt"] and
                failed["mergeError"] == f"entity {blocked}: {reason}",
                "拒绝超限快照后修改了原 Layout，或没有报告实体和原因")
        require(has_real(far_source, blocked) and not has_real(target, blocked), "合并失败后实体归属错误")
        created = far_source.evaluate(create_at(far_x))
        far_source.evaluate(f"KBEngine.entities[{created}].destroy()")

    boundary = next(row["bounds"][0] for row in merged["cells"] if row["cellapp"] == peer)
    target.evaluate(f"setattr(KBEngine.entities[{avatar}], 'controlledBy', None)")
    target.evaluate(f"setattr(KBEngine.entities[{avatar}], 'position', {(boundary + 8, y, z)!r})")
    wait_for("玩家迁入待退役 Cell", lambda: base.evaluate(route), lambda row: f"cellapp[{peer}]" in row[0])
    wait_for("玩家交接完成且原 Cell 已有 Ghost",
             lambda: (has_real(far_source, avatar), target.evaluate(
                 f"bool(KBEngine.entities.get({avatar}) and not KBEngine.entities[{avatar}].isReal())")),
             all)

    if live is not None:
        live.check_rollback_observation(far_source, target, avatar, boundary, y, z, output)

    # 先移出覆盖带，确保预检要序列化新 Ghost，但不会读到两个 Cell 私有字段。
    player = f"KBEngine.entities[{avatar}]"
    far_source.evaluate(f"setattr({player}, 'position', {(far_x, y, z)!r})")
    wait_for("保留 Cell 已回收旧 Avatar Ghost", lambda: target.evaluate(f"{avatar} not in KBEngine.entities"), bool)
    original_source = far_source.evaluate(f"[id({player}), {player}.hasWitness]")
    original_route = base.evaluate(route)
    # 属性赋值也会单独序列化；让每个字段低于上限，只在完整交接快照中合计超限。
    payload_bytes = 5_200_000
    cast_key = "merge-overflow"
    install = (f"_merge_saved_private = ({player}.dialogSession, {player}.pendingSkillCasts)\n"
               f"{player}.dialogSession = b'M' * {payload_bytes}\n"
               f"{player}.pendingSkillCasts = {{{cast_key!r}: b'S' * {payload_bytes}}}")
    restore = (f"{player}.dialogSession, {player}.pendingSkillCasts = _merge_saved_private\n"
               "del _merge_saved_private")
    far_source.evaluate(f"exec({install!r})")
    try:
        write_sizes = far_source.evaluate(
            f"[len(__import__('pickle').dumps(v)) for v in ({player}.dialogSession, {player}.pendingSkillCasts)]"
        )
        request = merge(original[peer], keeping)
        wait_merge_failed(request, avatar, "full handoff snapshot exceeds MemoryStream size limit", timeout=12)
        require(far_source.evaluate(f"[id({player}), {player}.hasWitness]") == original_source and
                original_source[1] and base.evaluate(route) == original_route and original_route[1] == client and
                far_source.evaluate(f"[len({player}.dialogSession), len({player}.pendingSkillCasts[{cast_key!r}])]") ==
                [payload_bytes, payload_bytes],
                "完整快照溢出后更换了源对象、路由或客户端，或破坏了私有状态")
        log_dir = output / "server/logs"
        centralized_log = log_dir / "logger_cellapp.log"

        def overflow_diagnostics():
            return [line.strip() for line in centralized_log.read_text(encoding="utf-8", errors="replace").splitlines()
                    if line.lstrip().startswith("ERROR") and f" {peer}  [" in line
                    and " - MemoryStream write overflowed! writePos:" in line
                    and any(line.endswith(f", writeSize:{size}.") for size in write_sizes)]

        diagnostics = wait_for("源端记录了完整快照的写入溢出", overflow_diagnostics, bool, timeout=5)
        require(len(diagnostics) == 1, "完整快照超限后仍重复尝试迁出")
        # log4cxx 还可能把同一条诊断写进源进程自己的文件；其他进程和错误不在例外范围内。
        local_log = log_dir / f"cellapp.{peer}.log"
        copies = []
        if local_log.is_file():
            message = diagnostics[0].split(" - ", 1)[1]
            copies = [line.strip() for line in local_log.read_text(encoding="utf-8", errors="replace").splitlines()
                      if line.lstrip().startswith("ERROR root [") and line.partition(" - ")[2].strip() == message]
            require(len(copies) <= 1, "源进程重复记录了完整快照溢出")
        expected_server_errors.update(diagnostics + copies)
        print(f"SPACE_MERGE_FULL_HANDOFF_OVERFLOW_PASS space={space} entity={avatar}", flush=True)
    finally:
        far_source.evaluate(f"exec({restore!r})")
        far_source.evaluate(f"setattr({player}, 'position', {(boundary + 8, y, z)!r})")
    wait_for("取消合并后原 Cell 再次收到 Avatar Ghost",
             lambda: target.evaluate(f"bool(KBEngine.entities.get({avatar}) and not {player}.isReal())"), bool)

    # 完整快照负例结束后再建 NPC，避免在途交接先改变它的归属。
    far_npc = far_source.evaluate(create_at(far_x))
    require(not target.evaluate(f"{far_npc} in KBEngine.entities"), "远处 NPC 已有 Ghost，无法验证退役复制")

    def set_far_name(value):
        far_source.evaluate(f"setattr(KBEngine.entities[{far_npc}], 'name', {value})")

    ghost_limit = "target Ghost snapshot exceeds 256 KiB"
    set_far_name("'N' * (270 * 1024)")
    wait_merge_failed(merge(original[peer], keeping), far_npc, ghost_limit)
    set_far_name("'merge-probe'")

    manager_log = output / "server/logs/logger_cellappmgr.log"

    def manager_logged(text):
        return text in manager_log.read_text(encoding="utf-8", errors="replace")

    with pause_console(base, output, "merge-oversize-base"):
        # 等退役端已检查远处 NPC，再增大属性，并在 BaseApp 暂停期间发起玩家越界。
        with pause_console(target, output, "merge-oversize-peer"):
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
    wait_merge_failed(request, far_npc, ghost_limit)
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
    result = {"retired": retiring, "kept": keeping, "reexpanded": new_cell, "target_callback_once": True,
              "refused_creation_during_drain": True, "moved_npc_outside_ghost_distance": True,
              "oversized_ghost_rejected_before_drain": True, "oversized_ghost_cancelled_during_drain": True,
              "oversized_full_handoff_cancelled_during_drain": True,
              "inflight_player_handoff_completed_during_cancellation": True,
              "creation_and_merge_restored_after_cancellation": True}
    if live_result is not None:
        live_result["merge_budget_rejections_before_cancel"] = 3
        live_result["source_rollback_witness_events"] = live.rollback_events
        result["live_adjustment"] = live_result
    return result
