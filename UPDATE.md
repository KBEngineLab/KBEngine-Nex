
# 更新日志

## 2.8.2
- [feat] 官网重构，更简洁，更直观

- [feat] SDK 异常处理，避免被上层错误导致stream错乱 [Issue #189](https://github.com/KBEngineLab/KBEngine-Nex/issues/189)

- [feat] KBEX增加插件管理功能
  - 插件管理面板：支持 GitHub / Gitee 双仓库源切换
  - 插件商城浏览：左侧插件列表（名称、作者、tags，状态靠右），右侧详情（描述、版本选择、README）
  - 插件安装：下载 zip 并解压到 assets/plugins/{id}/，自动注册到 plugins.xml
  - 插件卸载：删除插件目录并从 plugins.xml 移除
  - 插件启用/禁用：通过 plugins.xml 控制加载
  - 安装时支持版本选择

- [feat] 背包插件 [KBEngineNex-Plugin-Bag](https://github.com/KBEngineLab/KBEngineNex-Plugin-Bag)
  - 将背包从"独立实体"改为"组件 + 独立数据表"模式，挂载在 Avatar 上的 BagComponent 组件。
  - 背包数据落在独立数据库表 `kbe_plugin_bag_items`，不走 KBE 自动持久化，容量/堆叠/绑定/过期/锁定等业务点均保留结构。
  - 写操作串行队列：同一玩家的 addItem / removeItem / splitItem / swapItem / moveItem / mergeItem / sortItems / clear / transferItems 按 ownerDBID 维度串行执行，避免 SQL 异步回调乱序导致数据错乱。
  - BagManager ：周期性扫描卡住的队列，防止某条回调丢失后队列永久阻塞。
  - 客户端只读：客户端可调用 requestBagList / requestBagPage 查询背包，以及 splitItem / swapItem / moveItem / mergeItem / sortItems 等整理操作（服务端校验归属与规则），addItem / removeItem / clear 等高危写入口不暴露给客户端。
  - 离线/后台操作：提供 `getBagForEntityID(databaseID)` API，支持离线发奖、商城补单、GM 工具等不持有 Avatar 实例的场景。
  - 操作日志：三级可配日志（L1=增删清交易, L2=+拆分合并, L3=全量），支持数据库和文件双输出，文件日志支持日期+大小滚动分割。

- [feat] 数据库压测插件 [KBEngineNex-Plugin-DbStress](https://github.com/KBEngineLab/KBEngineNex-Plugin-DbStress)
  - 基于 executeRawDatabaseCommand 实现，覆盖增删改查全场景，支持 MySQL / PostgreSQL / MongoDB 三种数据库。
  - 五阶段流程：SETUP（建表预填）→ WARMUP（预热不计指标）→ STRESS（正式施压）→ REPORT（输出报告到 `scripts/logs/dbstress_*.log`）→ CLEANUP（清理压测表）。
  - 可配参数：duration（压测时长）、concurrency（并发数=threadID 数）、preloadRows（预填行数）、opWeights（INSERT/POINT_SELECT/RANGE_SELECT/UPDATE/DELETE 权重，默认 80:20 OLTP 读写比）。
  - concurrency 与 numConnections 关系：concurrency 超过 dbmgr 线程池大小时请求在内部排队，报告中提供 TPS / P50 / P95 / P99 延迟分布和按操作类型分组的统计。
  - 支持通过 dbInterface 参数指定 kbengine.xml 中配置的数据库接口名称，同一时间只允许一个压测实例运行。

- [feat] KBEX 客户端实体创建快捷入口 [Issue #181](https://github.com/KBEngineLab/KBEngine-Nex/issues/181)
  - KBEX 新增一键创建 Client Entity 的快捷入口：选择客户端实体路径后，根据 hasClient 标记和挂载的组件自动生成客户端实体模板。

- [feat] KBEX 快速热更入口：工具栏刷新按钮 + 自动热更（可配置检测目录与间隔） [Issue #183](https://github.com/KBEngineLab/KBEngine-Nex/issues/183)
  - 工具栏新增刷新按钮，一键触发热更。
  - 支持配置自动检测目录和检测间隔，文件变更时自动执行热更，无需手动操作。

- [feat] 为 cellapp teleport 接入 onTeleport 回调，支持脚本层拒绝传送 [Issue #178](https://github.com/KBEngineLab/KBEngine-Nex/issues/178)
  - cellapp Entity::onTeleport() 从预留的死方法改为实际生效，在 Entity::teleport() 所有前置检查通过后、分支执行前调用，只触发一次。
  - onTeleport() 返回 bool，脚本层返回 True/None 允许传送（默认兼容旧工程），返回 False 则拒绝并触发 onTeleportFailure。entity 和所有 cell 组件都会被询问，任一返回 False 即中断传送。

- [feat] createCellEntity 支持 CELL_VIA_BASE 类型（baseCall.cell） [Issue #182](https://github.com/KBEngineLab/KBEngine-Nex/issues/182)
  - 允许 createCellEntity 接收 baseEntityCall.cell 作为参数，不再强制要求直接 CellEntityCall。
  - 使用方式：`avatar.createCellEntity(spaceBaseCall.cell)`

- [feat] PostgreSQL 支持 [Issue #21](https://github.com/KBEngineLab/KBEngine-Nex/issues/21)
  - 新增完整 `db_postgresql` 后端库，实现 DBInterface、EntityTable、KBETable 全部接口，语义贴近 MySQL 实现。
  - 黑名单支持：PostgreSQL 专属危险命令黑名单（drop, truncate, alter, rename, grant, revoke, copy, vacuum 等）。

- [feat] executeRawDatabaseCommand 命令黑名单 [Issue #179](https://github.com/KBEngineLab/KBEngine-Nex/issues/179)
  - 配置驱动：`kbengine_defaults.xml` 新增 `<rawDatabaseCommandBlacklist>` 配置段，支持全局 `<enable>` 开关和按数据库类型（mysql / mongodb / postgresql）分别配置危险命令列表。
  - 匹配规则：多个词用英文逗号分隔，检查时不区分大小写，各数据库只读取自己类型的配置。
  - 拦截点：在 DBInterface 层统一拦截，命中黑名单时打 WARNING 日志并拒绝执行。
  - MySQL 默认拦截：delete, drop, truncate, alter, rename, replace, grant, revoke, flush, lock, unlock, shutdown 等。
  - MongoDB 默认拦截：drop, dropDatabase, deleteMany, remove, findAndModify, renameCollection, eval, mapReduce 等。
  - PostgreSQL 默认拦截：delete, drop, truncate, alter, rename, grant, revoke, copy, vacuum, reindex 等。

- [feat] 新增添加额外配置参数 [@Smile010110](https://github.com/Smile010110) [Pull request #176](https://github.com/KBEngineLab/KBEngine-Nex/pull/176)
  - 在 `kbengine.xml` 根节点下新增 `<customCfg>` 段，支持业务自定义键值配置（int / bool / float / string / dict / list 类型）。
  - Python 层通过 `KBEngine.getCustomCfg("key", default)` 读取，无需手动解析 XML。
  - 示例：
    ```xml
    <customCfg>
      <param name="battle.maxPlayers" type="int" desc="max players per battle">1000</param>
      <param name="battle.enableRank" type="bool" desc="enable battle rank">true</param>
      <param name="battle.speedScale" type="float" desc="battle speed scale">1.0</param>
      <param name="battle.welcome" type="string" desc="welcome text">hello</param>
      <param name="battle.dropRates" type="dict" desc="drop rate config">{"gold": 1.2, "item": 0.05}</param>
      <param name="battle.spawnPoints" type="list" desc="spawn point ids">[1, 2, 3]</param>
    </customCfg>
    ```

- [feat] GitHub Actions 自动化构建发布 [Issue #109](https://github.com/KBEngineLab/KBEngine-Nex/issues/109)

- [feat] 重构 Python async 调度 [Issue #166](https://github.com/KBEngineLab/KBEngine-Nex/issues/166)
  - 移除 py 层独立线程：旧方案使用单独 Python 线程跑 asyncio loop，在新方案中完全移除，消除线程间 GIL 争抢。
  - C++ 底层 dispatcher timer 驱动：EntityApp / PythonApp 类在主线程安装 `AsyncioHelper::installTimer()`，创建 `AsyncioTimerHandler` 周期性推进 asyncio 事件循环。
  - 不受 gameUpdateHertz 量化：asyncio timer 基于 `asyncioRepeatOffset` 配置独立调度（秒级精度转微秒级 dispatcher timer），不再与 game tick 频率耦合。
  - 协程任务管理：`submitCoroutine()` 将 awaitable 包装为 asyncio Task，每次 timer 触发最多连续推进 64 轮，避免 ready 队列过长时占满主循环。

- [feat] 提供当前 cellapp 下所有 space 的 py 层接口：KBEngine.spaces() [Issue #170](https://github.com/KBEngineLab/KBEngine-Nex/issues/170)
  - 返回当前 cellapp 上所有 Space 的 dict：`{spaceID: spaceEntity}`。

- [feat] 增加方法获取指定 space 的所有实体：KBEngine.entitiesForSpace(spaceid) [Issue #76](https://github.com/KBEngineLab/KBEngine-Nex/issues/76)
  - 传入 `spaceID`，返回该 Space 内所有实体的 dict。

- [feat] 按需启动服务配置，方便分布式/k8s 启动配置管理 [Issue #171](https://github.com/KBEngineLab/KBEngine-Nex/issues/171)
  - 支持多套配置文件：`kbengine.xml` / `kbengine_dev.xml` / `kbengine_prod.xml`。
  - 启动时通过 `--prop=prod/dev` 选择配置，或 `--location=/path/to/kbengine.xml` 直接指定配置文件路径。
  - 方便在 k8s 等容器编排环境中按环境注入不同配置。

- [feat] io_uring 与 kqueue completion adapter，底层完全 completion 化 [Issue #173](https://github.com/KBEngineLab/KBEngine-Nex/issues/173)
  - io_uring（Linux）：新增 `PollerIoUring` 实现，通过 `syscall(__NR_io_uring_setup)` 和 `io_uring_enter` 系统调用驱动异步 IO。
  - kqueue（macOS / DragonFly BSD）：新增 `PollerKqueue` 实现，基于 `kqueue()` + EVFILT_READ/EVFILT_WRITE 事件。
  - 统一 completion 模型：与已有 IOCP（Windows）一起，三大平台全部从 readiness 模型切换为 completion 模型。

- [feat] 【KBEX】app 启动失败后自动读取原始日志并输出 [Issue #158](https://github.com/KBEngineLab/KBEngine-Nex/issues/158)
  - 当 app 启动失败导致 logger 无法转发日志时，KBEX 自动回退到直接读取 app 的原始日志文件并输出到控制台，方便快速定位启动失败原因。

- [feat] registerReadFileDescriptor 等接口废弃，新增 registerAcceptFileDescriptor 等 completion API [Issue #172](https://github.com/KBEngineLab/KBEngine-Nex/issues/172)
  - 废弃接口：`registerReadFileDescriptor / deregisterReadFileDescriptor` 调用时直接抛出 `RuntimeError`，提示迁移到新 API。
  - 新增 listener accept API：`registerAcceptFileDescriptor(fd, onAccept)` — 注册 listener fd 的 accept completion。
  - 新增 data read API：`registerReadDataFileDescriptor(fd, onRead)` — 注册已连接 TCP fd 的读完成回调。
  - 新增 write API：`writeFileDescriptor(fd, data, onWriteComplete)` — 写操作完成后回调。
  - 安全校验：注册时检查当前 poller 是否支持 completion 模型，不支持则拒绝并报错；同一 fd 的读侧禁止重复注册（防止 listener 被当成 TCP data fd 消费）。

- [feat] 增强 topSpeed 超速检测并通知脚本层 [Issue #168](https://github.com/KBEngineLab/KBEngine-Nex/issues/168)
  - 单包检测（原有）：逐包比较移动距离与 topSpeed 阈值
  - 同帧累积（新增）：同一 game tick 内多次位置更新的移动量累加后检测，防止客户端将超速拆成多个小包绕过单包检查
  - 滑动窗口（新增）：统计 gameUpdateHertz 帧窗口内的总移动距离，若超出 topSpeed × 窗口帧数则触发回调，防御跨帧渐进式加速
  - 新增 Python 回调 onMoveOverTopSpeed(clientX, clientY, clientZ, xzDist, yDist)：
    - 单包/同帧超速时，在重置客户端位置之前回调
    - 滑动窗口超速时，在窗口满时回调

- [fix] 修复 urlopen 传入 None 时可能崩溃的问题 [Issue #167](https://github.com/KBEngineLab/KBEngine-Nex/issues/167)

- [fix] log4cxx 初始化前自动创建日志目录
  - 新增 `ensureLog4cxxAppenderDirectories()` 函数，在 log4cxx 初始化前遍历 `log4j.appender.*.File` 配置项，自动创建目标目录。
  - 解决 Linux 下首次启动时日志目录尚不存在导致 log4cxx 初始化失败的问题。

- [perf] openssl 彻底 vcpkg 引入
  - 统一通过 vcpkg 管理 OpenSSL 依赖，不再依赖系统自带的 OpenSSL 或 VS 内置 OpenSSL。

- [perf] Debug 下编译 Release Python [Issue #175](https://github.com/KBEngineLab/KBEngine-Nex/issues/175)
  - 解决 KBE 以 Debug 模式编译时链接 Python Debug ABI 导致第三方包无法使用的问题。

- [perf] db_mongodb 功能优化 [Issue #177](https://github.com/KBEngineLab/KBEngine-Nex/issues/177)
  - Entity 表
    - 补了 `EntityTableMongodb::removeEntity()`：按 `id` 删除实体文档。
    - 补了 `entityShouldAutoLoad()`：更新 `sm_autoLoad`。
    - 补了 `queryAutoLoadEntities()`：查询自动加载实体 DBID。
    - 修了 `writeTable()`：插入时 `sm_autoLoad` 默认写 `0`，再统一走 `entityShouldAutoLoad()`；`insert/update` 失败时返回 `0`，避免调用方误判成功。
  - KBE 表
    - `KBEEntityLogTableMongodb`：补 entity log 写入、查询、按 entityType 删除、按 baseapp 删除。修了 `eraseEntityLog()` 成功返回值反了的问题。同步时不再粗暴清空全部日志，改为参考 MySQL 的过期/无效 serverGroup 清理逻辑。
    - `KBEServerLogTableMongodb`：补 serverlog collection 创建、唯一索引。补 server 心跳 upsert。补查询当前 server、所有 server、超时 server、清理 server。补 `isShareDB()` 和所有 server 的共享 DB 状态检查。
    - `KBEAccountTableMongodb`：补账号表索引（accountName、email、entityDBID）。查询账号支持 accountName 或 email。补完整账号信息查询。修 `setFlagsDeadline()` 没有 `$set` 的问题。修 `updatePassword()` 错用 entityDBID 匹配账号名的问题。补 `updateCount()`（更新 lasttime 并递增 numlogin）。bindata 改为 BSON binary 存储和读取。
    - `KBEEmailVerificationTableMongodb`：补验证码 collection 创建、`code` 唯一索引。补过期验证码清理。补 `queryAccount()`、`logAccount()`、`delAccount()`。补邮箱激活账号、绑定邮箱、重置密码流程。
  - 辅助修正：增加 BSON 读取 helper（int32/int64/bool/utf8/binary、find_one()、账号名/邮箱 $or 查询等），避免重复写不稳定的 iterator 逻辑。修了几处 BSON iterator 复用可能导致字段漏读的问题。保持 MongoDB 实现尽量贴近 MySQL 语义。


## 2.8.1
- [fix] 修复使用navigateToDetour时，结束移动后navigateToDetour不被同步导致entity乱跳跃的的bug
- [fix] 修复C++ SDK ObjectPool的一处BUG
- [fix] 修复C++ SDK tcp下时序错乱的BUG
- [fix] 修复 MySQL 写入和删除实体时，收集子表 DBID 使用父表名查找的问题，避免多子表场景下子表记录同步异常。
- [fix] 修复 KBEngine.MemoryStream 销毁时没有减少 PyGC tracing 计数导致泄露的BUG。 [Issue #131](https://github.com/KBEngineLab/KBEngine-Nex/issues/131)
- [feat] raycast完善，优化导航射线检测，支持斜、垂直射线（未来考虑拆分成可走区域探测和空间射线两个方法） [Issue #79](https://github.com/KBEngineLab/KBEngine-Nex/issues/79)
- [feat] 新增 completionBudget 配置，限制每 tick 处理的完成事件数量和耗时，避免网络完成事件过多占满主循环。（目前仅win iocp）
- [feat] rider支持，目前仅支持windows [Issue #135](https://github.com/KBEngineLab/KBEngine-Nex/issues/135)
- [feat] cmake 编译器切换到ninja ，加速编译 [Issue #142](https://github.com/KBEngineLab/KBEngine-Nex/issues/142)
- [feat] Apple Silicon支持 [Issue #37](https://github.com/KBEngineLab/KBEngine-Nex/issues/37)
- [feat] macos io多路复用 kqueue [Issue #139](https://github.com/KBEngineLab/KBEngine-Nex/issues/139)
- [feat] kbex macos支持 [Issue #141](https://github.com/KBEngineLab/KBEngine-Nex/issues/141)
- [feat] windows select替换为IOCP [Issue #136](https://github.com/KBEngineLab/KBEngine-Nex/issues/136)
- [feat] win server 异常退出时生成dump信息 [Issue #155](https://github.com/KBEngineLab/KBEngine-Nex/issues/155)
- [feat] kbex 进程管理优化 https://www.kbelab.com/kbenginex/home.html [Issue #122](https://github.com/KBEngineLab/KBEngine-Nex/issues/122)
- [feat] kbex 可视化功能添加 https://www.kbelab.com/kbenginex/home.html [Issue #123](https://github.com/KBEngineLab/KBEngine-Nex/issues/123)
- [feat] 兔子世界案例迁移到Nex https://github.com/KBEngineLab/demo_kbengine_unity_rabbit
- [perf] 升级TS SDK Event 区分out/in事件注册与触发 [Issue #126](https://github.com/KBEngineLab/KBEngine-Nex/issues/126)
- [perf] 升级c++ SDK Event 区分out/in事件注册与触发 [Issue #126](https://github.com/KBEngineLab/KBEngine-Nex/issues/126)
- [perf] 给 c++层的 Python Map 增加 clear， pop 成员函数 [Issue #129](https://github.com/KBEngineLab/KBEngine-Nex/issues/129)
- [perf] c++升级到兼容17/20 [Issue #132](https://github.com/KBEngineLab/KBEngine-Nex/issues/132)
- [perf] 优化掉所有的Warning [Issue #133](https://github.com/KBEngineLab/KBEngine-Nex/issues/133)
- [perf] c++ sdk 移除libhv依赖
- [perf] python async调度迁移到c++底层 [Issue #137](https://github.com/KBEngineLab/KBEngine-Nex/issues/137)
- [perf] 优化kcp 客户端断开连接后的重试ERROR日志，变更为WARNING并更友好的提示
- [perf] 优化kcp断连后的ERROR，ikcp_send外部客户端发送窗口满不再每次 ERROR_MSG（内部通道保留）
- [perf] KBEngine.urlopen 超时配置 ，支持timeout、connectTimeout、lowSpeedTime/lowSpeedLimit [Issue #128](https://github.com/KBEngineLab/KBEngine-Nex/issues/128)
- [perf] Docker镜像下线，由于KBE已经全平台支持，所以不再提供Docker镜像，请小伙伴们自行使用dockerfile [Issue #164](https://github.com/KBEngineLab/KBEngine-Nex/issues/164)

## 2.7.4
- [feat] NavMesh Generator 添加了一个坐标配置管理器
    1.支持选点添加或更新到配置
    2.支持导入导出json、python
    3.支持下载集合或单个地图配置
    4.支持浏览器本地存储（30s自动存储
- [fix] 修复navigation补帧触发导致脱离生命周期出现的bug
- [fix] c# sdk event中补全一些参数和注释 [Issue #125](https://github.com/KBEngineLab/KBEngine-Nex/issues/125)
- [perf] KBEDebug.py升级，打印文件、行号，支持跳转 [Issue #124](https://github.com/KBEngineLab/KBEngine-Nex/issues/124)

## 2.7.3

- [fix] entity添加isOnNavigate，使用navigateToDetour时，设置isOnGround为true的同时也能下发y轴
- [fix] 修复navigate不绕过障碍物的bug
- [fix] 修复navigate持续触发时卡顿的bug
- [fix] 修复bots不触发心跳导致被卸载的bug
- [fix] 【KBEX】 启动多个pycharm时，进程管理有问题  [Issue #111](https://github.com/KBEngineLab/KBEngine-Nex/issues/111)
- [feat] bots接入logger进程，用于单元测试 [#119](https://github.com/KBEngineLab/KBEngine-Nex/issues/119)
- [perf] vcpkg 依赖固定版本及安装脚本优化 [#115](https://github.com/KBEngineLab/KBEngine-Nex/issues/115)
- [perf] 【KBEX】优化调用链不清晰时的补全逻辑
- [feat] 【KBEX】添加Bots快捷启动入口

## 2.7.2

- [fix] 修复EntityComponent里的方法回调，需要owner实现该方法后，EntityComponent的回调才会被调用的BUG [Issue #107](https://github.com/KBEngineLab/KBEngine-Nex/issues/107)
- [fix] mysql9中md5内置方法被移除 [Issue #108](https://github.com/KBEngineLab/KBEngine-Nex/issues/108)
- [update] mongodbc 升级到2.2.2 [Issue #113](https://github.com/KBEngineLab/KBEngine-Nex/issues/113)

## 2.7.1

- [update] kbex 调试下，entity.xxx 不支持输出的问题 [Issue #102](https://github.com/KBEngineLab/KBEngine-Nex/issues/102)
- [fix] 同时启动多个不同类型数据库，watcher冲突的bug [Issue #103](https://github.com/KBEngineLab/KBEngine-Nex/issues/103)
- [update] mongodb authSource应该验证对应的数据库，而不是admin [Issue #105](https://github.com/KBEngineLab/KBEngine-Nex/issues/105)
- [update] kbex 增加entity快捷操作功能 [Issue #86](https://github.com/KBEngineLab/KBEngine-Nex/issues/86)

## 2.7.0

由于本次更新中，navmesh属于底层破坏性更新，所以直接调整为一个大版本更新

- [feat] 新增navigateToDetour方法，用于使用Detour导航，原navigate方法不变（points导航） [Issue #96](https://github.com/KBEngineLab/KBEngine-Nex/issues/96)
  - Detour导航可以在服务端贴合navmesh高度，在多层建筑中非常有用
- [feat] recastnavigation升级，并改为由vcpkg导入 [Issue #74](https://github.com/KBEngineLab/KBEngine-Nex/issues/74)
  - 重要：navmesh升级后，为了保持多客户端兼容和未来插件升级兼容，由之前的左手坐标系转换为官方支持的-z右手坐标系（Recast Navigation / Three.js），KBE层是+Z的右手坐标
  - 客户端侧所有的坐标同步都要做对应手系的翻转
  - xyz分别为roll、pitch、yaw
  - unity:
    - 位置：x = -x ,y = y ,z = z
    - 朝向：yaw = -z
  - Cocos Creator:
    - 位置：-x = x ，y = z ，z = y 
    - 朝向：yaw + 180
  - Godot
    - 位置：-x = x ，y = z ，z = y 
    - 朝向：yaw + 180
  - UE
    - 位置：x = x * 100，y = z * 100 ，z = y * 100
    - 朝向：yaw + 90
- [feat] navmesh 周边工具，一个web端的navmesh生成工具（https://navmesh.kbelab.com/） [Issue #58](https://github.com/KBEngineLab/KBEngine-Nex/issues/58)
- [feat] mongodb接入 [Issue #59](https://github.com/KBEngineLab/KBEngine-Nex/issues/59)
- [feat] 原生c++ sdk [Issue #60](https://github.com/KBEngineLab/KBEngine-Nex/issues/60)
- [feat] 原生cxx ue5 demo+原生cxx demo [Issue #67](https://github.com/KBEngineLab/KBEngine-Nex/issues/67)
- [feat] 文档完善 docker使用教程，云服务器部署教程，kbex docker教程 [Issue #64](https://github.com/KBEngineLab/KBEngine-Nex/issues/64)
- [feat] WebConsole 全新重构 [Issue #44](https://github.com/KBEngineLab/KBEngine-Nex/issues/44)
- [feat] csharp sdk ，websocket 端口和域名映射支持 [Issue #50](https://github.com/KBEngineLab/KBEngine-Nex/issues/50)
- [feat] ts sdk ，websocket 端口和域名映射支持 [Issue #51](https://github.com/KBEngineLab/KBEngine-Nex/issues/51)
- [feat] webconsole 新增用户时配置权限 [Issue #62](https://github.com/KBEngineLab/KBEngine-Nex/issues/62)
- [feat] kbex 添加日志直连功能，用于外部启动引擎时连接日志 [Issue #61](https://github.com/KBEngineLab/KBEngine-Nex/issues/61)
- [feat] kbex 插件更优的docker支持 [Issue #55](https://github.com/KBEngineLab/KBEngine-Nex/issues/55)
- [feat] dockerfile 以及基础镜像 [Issue #56](https://github.com/KBEngineLab/KBEngine-Nex/issues/56)
- [fix] webconsole 创建用户时，设置用户扩展数据报错 [Issue #53](https://github.com/KBEngineLab/KBEngine-Nex/issues/53)
- [fix] webconsole py控制台无法多行输入的bug [Issue #52](https://github.com/KBEngineLab/KBEngine-Nex/issues/52)
- [fix] kbex 调试模式异常输出的bug [Issue #63](https://github.com/KBEngineLab/KBEngine-Nex/issues/63)
- [fix] 修复ts sdk里event Fire没有立即触发导致的延迟
- [delete] 删除底层redis持久化实现 [Issue #71](https://github.com/KBEngineLab/KBEngine-Nex/issues/71)
- [update] 基础demo 全面升级，适配服务端navmesh （unity、cocos、godot、ue5）
- [update] assets移除一些历史spaces配置，所有基础demo统一在一个space配置（kbengine_all_demo）下实现，加速服务端启动

## v2.6.3

- [feat] 系统回调支持asyncio [Issue #1](https://github.com/KBEngineLab/KBEngine-Nex/issues/1)
- [feat] 新增 ts sdk  [Issue #6](https://github.com/KBEngineLab/KBEngine-Nex/issues/6)
- [feat] 添加原生C# SDK，支持unity和GODOT [Issue #15](https://github.com/KBEngineLab/KBEngine-Nex/issues/15) [Issue #6](https://github.com/KBEngineLab/KBEngine-Nex/issues/6)
- [feat] 原生C# SDK支持unity websocket [Issue #29](https://github.com/KBEngineLab/KBEngine-Nex/issues/29)
- [feat] vcpkg 支持，为未来支持arm处理器编译做准备 [Issue #34](https://github.com/KBEngineLab/KBEngine-Nex/issues/34)
- [feat] makefile迁移到cmake [Issue #13](https://github.com/KBEngineLab/KBEngine-Nex/issues/13)
- [feat] 升级三方依赖（ "fmt",log4cxx","zlib","hiredis","expat","apr","apr-util", "curl"） [Issue #32](https://github.com/KBEngineLab/KBEngine-Nex/issues/32)
- [feat] ubuntu-24.x /25.x支持 [Issue #33](https://github.com/KBEngineLab/KBEngine-Nex/issues/33)
- [feat] Linux/windows 一键编译脚本支持 [Issue #25](https://github.com/KBEngineLab/KBEngine-Nex/issues/25)
- [feat] 数据库创建table时，给字段添加上注释 [Issue #23](https://github.com/KBEngineLab/KBEngine-Nex/issues/23)
- [feat] linux arm支持 [Issue #38](https://github.com/KBEngineLab/KBEngine-Nex/issues/38)
- [feat] linux下cmake编译，Hybrid默认启用ASan，Release、Evaluation默认启用FORTIFY_SOURCE，可根据实际情况开关
- [feat] base、cell、interfaces asyncioRepeatOffset配置支持
- [fix] 添加mysqlclient缺失的2个dll
- [fix] 修复sync_item_to_db时， utf8mb4 中每次启动都重复同步一次UNICODE字段的BUG


## v2.6.2

- [feat] 添加dockerfile
- [feat] python 3.7.x升级 -> 3.13.5 [Issue #2](https://github.com/KBEngineLab/KBEngine-Nex/issues/2)
- [feat] 引擎支持python venv虚拟环境
- [feat] ssl / rsa / caching_sha2_password 支持 [Issue #4](https://github.com/KBEngineLab/KBEngine-Nex/issues/4)
- [feat] linux 并行构建支持 [Issue #8](https://github.com/KBEngineLab/KBEngine-Nex/issues/8)
- [feat] UE5 SDK支持 [Issue #6](https://github.com/KBEngineLab/KBEngine-Nex/issues/6)
- [feat] 升级mysqlclient到8.x
- [fix] 调整server_assets文件结构
