"""验证真实 Cell 间交接期间的一次性销毁定时器。"""

from concurrent.futures import ThreadPoolExecutor

from space_layout_runtime import require, wait_for


def verify_handoff_destroy_timer(cells, layout, output):
    space, owner = layout["space"], layout["owner"]
    source = cells[owner]
    x, z = layout["split_x"], layout["split_z"] - 16.0
    left, right = (x - 4.0, 1.0, z), (x + 4.0, 1.0, z)
    target_id = source.evaluate(f"KBEngine.cellForPosition({space}, {right[0]}, {z})[1]")
    require(target_id in cells and target_id != owner, "测试坐标未跨 Cell")
    target = cells[target_id]
    entity_id = source.evaluate(
        f"KBEngine.createEntity('NPC', {space}, {left!r}, (0., 0., 0.), {{}}).id"
    )
    state = (
        f"KBEngine.entities[{entity_id}].isReal() if {entity_id} in KBEngine.entities else None"
    )
    wait_for("目标收到边界 Ghost", lambda: target.evaluate(state), lambda real: real is False)

    restoring = output / "handoff-restoring"
    source_paused = str(output / "handoff-source-paused")
    release = output / "handoff-release-source"
    gate = """
from pathlib import Path
from time import monotonic, sleep

def _handoff_wait(path):
    deadline = monotonic() + 20
    while not Path(path).exists():
        if monotonic() >= deadline:
            raise TimeoutError('Cell handoff test synchronization timed out')
        sleep(0.01)
"""
    source_setup = gate + f"""
from NPC import NPC

def _handoff_test_event(self):
    pass

NPC._handoff_test_event = _handoff_test_event
KBEngine.entities[{entity_id}].registerEvent(
    '_handoff_test_event', KBEngine.entities[{entity_id}]._handoff_test_event)
"""
    target_setup = gate + f"""
from NPC import NPC
_handoff_test_calls = []
_handoff_timer = NPC.onTimer
_handoff_entered = getattr(NPC, 'onEnteredCell', None)
_handoff_destroyed = getattr(NPC, 'onDestroy', None)

def _handoff_test_event(self):
    pass

def _handoff_rebind(self):
    if self.id == {entity_id}:
        Path({str(restoring)!r}).touch()
        _handoff_wait({source_paused!r})
    return _handoff_test_event.__get__(self, type(self))

def _handoff_on_timer(self, tid, arg):
    if self.id != {entity_id} or arg != 482115:
        return _handoff_timer(self, tid, arg)
    _handoff_test_calls.append(['timer', self.isReal(), self.destroy() is None, self.destroy() is None])

def _handoff_on_entered(self):
    if self.id == {entity_id}:
        _handoff_test_calls.append(['entered', self.isReal()])
    elif _handoff_entered is not None:
        _handoff_entered(self)

def _handoff_on_destroy(self):
    if self.id == {entity_id}:
        _handoff_test_calls.append(['destroyed', self.isReal()])
    if _handoff_destroyed is not None:
        _handoff_destroyed(self)

NPC._handoff_test_event = property(_handoff_rebind)
NPC.onTimer = _handoff_on_timer
NPC.onEnteredCell = _handoff_on_entered
NPC.onDestroy = _handoff_on_destroy
"""
    source.evaluate(f"exec({source_setup!r})")
    target.evaluate(f"exec({target_setup!r})")
    move = f"""
KBEngine.entities[{entity_id}].addTimer(2.5, 0, 482115)
KBEngine.entities[{entity_id}].position = {right!r}
"""
    source.evaluate(f"exec({move!r})")
    # 目标恢复实体事件时还未确认安装。此时暂停源端，保证定时器先于释放确认到期。
    wait_for("目标正在恢复迁移快照", restoring.exists, bool)
    pause = f"Path({source_paused!r}).touch()\n_handoff_wait({str(release)!r})"

    def events():
        return target.evaluate("_handoff_test_calls")

    timer = ["timer", True, True, True]
    with ThreadPoolExecutor(max_workers=1) as executor:
        paused = executor.submit(source.evaluate, f"exec({pause!r})", 22)
        try:
            before = wait_for("目标执行一次性定时器", events, bool, timeout=12)
            require(before == [timer] and target.evaluate(state) is True,
                    "定时器没有在等待源释放的目标 Real 上执行，或提前销毁了实体")
        finally:
            release.touch()
        paused.result()

    wait_for("交接完成后销毁目标", lambda: target.evaluate(state), lambda real: real is None, timeout=15)
    expected = [timer, ["entered", True], ["destroyed", True]]
    require(events() == expected and source.evaluate(state) is None,
            "销毁回调没有按顺序执行一次，或源端仍保留实体")
    return {"entity": entity_id, "events": expected, "source_released": True}
