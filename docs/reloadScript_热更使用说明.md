# reloadScript 热更使用说明

> 最后更新: 2026-05-28

## 一、概述

`KBEngine.reloadScript()` 用于在进程运行期间重新加载 Python 脚本逻辑，尽量让在线 Entity、EntityComponent 和 Timer 回调切换到新的脚本实现。

热更的目标是修复脚本逻辑问题，减少开发和紧急修复时的重启成本。它不是完整发布系统，也不适合承载数据结构、实体定义、持久化 schema 的线上变更。

当前默认语义:

- `KBEngine.reloadScript()` 等价于 `KBEngine.reloadScript(False)`。
- `KBEngine.reloadScript(False)` 只热更逻辑层。
- `KBEngine.reloadScript(True)` 仅建议开发环境使用，用于完整 reload。
- 生产环境下即使传入 `True`，底层也会强制降级为 `False`。
- 生产环境也会调用 Entity 的 `onReload(fullReload)` 回调。

## 二、基本用法

在目标进程的 telnet/控制台中执行:

```python
KBEngine.reloadScript()
```

或显式写成:

```python
KBEngine.reloadScript(False)
```

开发环境如果确实需要完整 reload，可以使用:

```python
KBEngine.reloadScript(True)
```

生产环境不建议使用 `True`。即使使用，也会被强制改为逻辑热更，并打印 warning。

## 三、适用范围

适合热更:

- 修改普通 Python 方法实现。
- 修改 `base/`、`cell/` 当前组件目录下已经加载过的 helper 模块。
- 修改 `interfaces/` 下的 mixin/interface 方法。
- 修改在线 Entity 或 EntityComponent 的方法逻辑。
- 修改 Timer 回调指向的模块函数或绑定方法实现。

不适合热更:

- 新增、删除、修改 `.def` 中的属性、方法、组件结构。
- 修改 `types.xml` 或持久化数据结构。
- 新增或删除插件。
- 新增一个当前进程从未 import 过的普通模块并期待 reload 主动 import。
- 修改需要所有进程严格同步切换的协议或跨进程接口。
- 删除 Python 方法并要求旧在线对象立刻失去该方法。

## 四、加载规则

`reloadScript(False)` 会按文件版本戳过滤脚本文件。只有检测到变化的文件才会真正执行 Python reload。

版本戳判断规则:

- Windows 使用文件最后写入时间和文件大小。
- 非 Windows 使用 `st_mtime` 和文件大小。
- 同一个物理文件如果被 `interfaces.Teleport` 和 `Teleport` 两个模块名加载，只会真正 reload 一次，另一个模块名只做旧 class patch。

当前组件只扫描自己的脚本目录:

- `baseapp` 只处理 `base/`。
- `cellapp` 只处理 `cell/`。
- `client/bots` 不处理 `client/` 热更。

这样可以避免 cell 进程误加载 base 脚本，或 base 进程误加载 cell 脚本。

## 五、在线对象更新

当检测到脚本文件变化，并且 reload 成功后:

- 在线 Entity 会更新 `__class__`，方法查找会进入新类。
- 在线 EntityComponent 会更新 `__class__`，并重新绑定组件描述和 owner 上的组件属性描述。
- EntityCall 会刷新内部缓存。
- Timer 不会重新创建，不会改变剩余触发时间，只会尝试刷新保存的 Python 回调对象。

当没有任何 changed 文件时:

- 不刷新 Entity。
- 不刷新 EntityComponent。
- 不刷新 Timer。
- 不调用 `onInit(1)`。
- 只打印 no changed 日志。

当 reload 过程中出现脚本 reload 失败:

- 本轮会中止。
- 不继续刷新在线 Entity、Component、Timer。
- 不调用 `onInit(1)`。
- 日志会打印失败原因和 abort 信息。

## 六、onReload 回调

开发环境下，Entity 可以选择实现:

```python
def onReload(self, fullReload):
    pass
```

特点:

- 这是可选回调，不实现也不会报错。
- 开发和生产环境都会触发。
- 参数 `fullReload=True` 表示完整 reload，`fullReload=False` 表示逻辑 reload。
- 生产环境下 `fullReload` 会被强制降级为 `False`，因此生产收到的通常是 `False`。
- 回调发生在 Entity、Component、EntityCall 刷新之后。

建议用途:

- 开发期刷新临时缓存。
- 打印调试信息。
- 验证当前对象已经切到新逻辑。

不建议用途:

- 做线上数据迁移。
- 修改持久化结构。
- 依赖所有 Entity 必定收到回调。

## 七、Timer 热更

Timer 热更会尝试重新定位回调:

- 绑定方法: 通过原 owner 和方法名重新获取，例如 `entity.onTimer`。
- 模块函数: 通过 `__module__ + __qualname__` 重新获取。

不能稳定热更的 Timer:

- 局部函数。
- lambda。
- 闭包。
- 动态生成且没有稳定模块路径的 callable。

这些 Timer 会保留旧回调，并在日志中计入 `timersKeptOld`。同时会打印具体 callback 路径:

```text
timer kept old callback: Avatar.onFooTimer
```

建议 Timer 回调使用 Entity/Component 方法或模块顶层函数。

## 八、日志说明

一次正常热更可能出现以下日志:

```text
cellapp::reloadScript: begin, fullReload=false.
EntityDef::reload: changed script files count=1, skippedFiles=42.
EntityDef::reload: changed script file: D:/.../cell/interfaces/Teleport.py (interfaces.Teleport)
cellapp::reloadScript: EntityDef::reload done, ok=true, changedFiles=1, skippedFiles=42, reloadedModules=1, duplicateModulePatches=1, staleAttrsKept=0.
cellapp::reloadScriptEntitiesAndNotify: entities reloaded, count=36.
cellapp::reloadScript: fullReload=false, componentsReloaded=3, timersRefreshed=2, timersKeptOld=0
```

关键字段:

| 字段 | 含义 |
| --- | --- |
| `changedFiles` | 本轮检测到变化并参与 reload 的物理文件数量 |
| `skippedFiles` | 检查过但没有变化的物理文件数量 |
| `reloadedModules` | 实际执行 Python reload 的模块数量 |
| `duplicateModulePatches` | 同一文件多模块名加载时，跳过二次 reload、只 patch 旧 class 的次数 |
| `staleAttrsKept` | 新类已删除但旧 class 上保留的属性数量 |
| `timersRefreshed` | 成功切到新回调的 Timer 数量 |
| `timersKeptOld` | 仍保留旧回调的 Timer 数量 |

无文件变化时:

```text
EntityDef::reload: no changed script files, skippedFiles=42.
cellapp::reloadScript: no changed script files, skip entity/component/timer/onInit refresh.
```

reload 失败时:

```text
EntityDef::reloadDependencyScriptModules: reload module(...) failed.
cellapp::reloadScript: aborted because EntityDef::reload failed.
```

## 九、删除方法的注意事项

为了降低热更破坏性，旧 class 上已有但新 class 已删除的属性不会被强制删除。

如果删除了方法，日志会提示:

```text
EntityDef::patchOldTypeFromNewType: stale attr kept on old class, attr=oldMethod. Deleted script attributes are not removed during safe reload.
```

含义:

- 新逻辑中的新增和修改方法会同步。
- 被删除的方法可能仍能在旧在线对象的旧 class 链上找到。
- 如果必须彻底删除旧方法，建议重启相关进程。

## 十、生产环境建议

生产环境只建议使用:

```python
KBEngine.reloadScript()
```

或:

```python
KBEngine.reloadScript(False)
```

生产热更应限制为纯逻辑修复:

- 不改 `.def`。
- 不改 `types.xml`。
- 不改持久化字段。
- 不改跨进程协议。
- `onReload(False)` 必须保持轻量、幂等，不做数据结构迁移。
- 不新增未加载模块作为修复入口。

推荐流程:

1. 在开发环境验证 reload 成功。
2. 在 staging/灰度环境执行同样命令。
3. 确认 `changedFiles` 和预期一致。
4. 确认没有 reload failed。
5. 确认 `timersKeptOld` 是否可接受。
6. 生产环境逐进程执行，观察日志。
7. 热修后安排滚动重启，让进程回到干净状态。

## 十一、常见问题

**Q: 为什么我改了一个文件，日志里以前出现两个 changed？**

因为同一个文件可能被不同模块名加载，例如 `interfaces.Teleport` 和 `Teleport`。现在日志按物理文件去重，并且同一个文件只真正 reload 一次。

**Q: 为什么 reload 后没有任何动作？**

如果日志显示 `no changed script files`，说明没有检测到脚本文件变化。本轮会跳过 Entity、Component、Timer 和 `onInit` 刷新。

**Q: 为什么 Timer 还是旧逻辑？**

看日志中的 `timersKeptOld` 和 `timer kept old callback`。如果回调是 lambda、闭包或局部函数，热更无法稳定重新定位，建议改为 Entity 方法或模块顶层函数。

**Q: 为什么删除方法后旧对象还能调用？**

这是安全策略。热更只同步新增和修改，不强制删除旧 class 上的属性。日志会用 `stale attr kept` 提醒。

**Q: 生产能不能传 `reloadScript(True)`？**

不建议。生产环境底层会强制降级为 `False`，只做逻辑热更。

**Q: 新增一个 Python 文件后 reload 会自动加载吗？**

不会主动 import 当前进程从未加载过的普通模块。需要已有脚本逻辑 import 到它，或重启进程。

## 十二、推荐写法

推荐:

```python
# 模块顶层函数，Timer 可稳定重新定位
def tick(timerID):
    pass

class Avatar(KBEngine.Entity):
    def onTimer(self, timerID):
        pass
```

不推荐:

```python
def start(self):
    def localTimer(timerID):
        pass

    self.addTimer(1.0, 1.0, localTimer)
```

不推荐将热更用于数据迁移:

```python
def onReload(self, fullReload):
    # 不建议在这里改持久化结构或批量迁移线上数据
    pass
```
