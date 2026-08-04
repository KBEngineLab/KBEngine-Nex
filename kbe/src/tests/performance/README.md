# KBE Performance Lab

The Performance Lab runs outside the engine's hot path and records append-only JSONL observations.
性能实验室运行在引擎热路径之外，并记录追加式 JSONL 观测数据。

Run a short local sample:

```powershell
python -B -m performance.run --scenario performance/scenarios/smoke.json --duration 30 --command "<workload command>"
```

Run the formal gameplay workload with a generated local topology:
使用自动生成的本地拓扑运行正式业务压测：

```powershell
python -B -m performance.run `
  --scenario performance/scenarios/gameplay_10000.json `
  --assets-root D:\KBELAB\KBEProject\kbengine_stresstest\mmorpg\server_assets `
  --server-binary-dir D:\KBELAB\kbengine\kbengine1x\kbe\src\out\cmake\windows-ninja\bin\Release `
  --build-root D:\KBELAB\kbengine\kbengine1x\kbe\src\out\build\windows-ninja-multi `
  --tools-root D:\KBELAB\kbengine\kbengine1x\kbe\tools\server `
  --start-cluster `
  --baseapp-count 3 `
  --cellapp-count 6 `
  --bots 10000 `
  --bots-processes 4 `
  --bots-batch-size 8 `
  --bots-batch-interval 0.08 `
  --server-ready-timeout 300 `
  --workload-ready-timeout 300
```

`--server-binary-dir` generates the singleton components plus the requested BaseApp/CellApp
processes and supplies the Bots executable automatically. `--bots-batch-size` is the aggregate
batch across all Bots processes and must divide evenly; the example creates about 100 Bots/second.
Server startup waits for BaseAppMgr and CellAppMgr `root/readiness` values derived from
`onReadyForLogin`, including exact expected process counts. It does not depend on game Space logs.
`--server-binary-dir` 会自动生成单例组件和指定数量的 BaseApp/CellApp，并自动选择 Bots 可执行文件。
`--bots-batch-size` 表示全部 Bots 进程合计的单批数量且必须可整除；示例约为每秒 100 Bots。
服务端启动通过 BaseAppMgr/CellAppMgr 的 `root/readiness` 等待底层 `onReadyForLogin` 聚合结果，
同时校验精确进程数，不再依赖业务 Space 日志。

Bots write their own rotating log by default and do not connect to Logger. Add `--bots-dev` to the
runner, or `--dev` when launching `bots.exe` directly, only when centralized IDE/PyCharm log display
is required. Development mode intentionally adds serialization, network, Logger CPU, and centralized
disk IO, so it must remain disabled for publishable performance runs.
Bots 默认只写自身滚动日志且不连接 Logger。只有需要 IDE/PyCharm 集中日志显示时，才向运行器添加
`--bots-dev`，或直接启动 `bots.exe` 时添加 `--dev`。开发模式会额外产生序列化、网络、Logger CPU
和集中磁盘 IO，因此发布正式性能数据时必须关闭。

The runner writes `raw.jsonl`, `summary.json`, and `report.md` below the repository-root
`kbe/src/out/performance-runs/`, regardless of the caller's current directory. Pass
`--output-root` to override it explicitly.
运行器默认把 `raw.jsonl`、`summary.json` 和 `report.md` 写入仓库根目录下的
`kbe/src/out/performance-runs/`，与启动时的当前目录无关；需要时可用 `--output-root` 覆盖。

Process samples include CPU normalized to the machine's logical processor count, working set,
thread count, and, on Windows, private committed memory, peak working set, and handle count.
进程采样包含按整机逻辑处理器数归一化的 CPU、工作集和线程数；Windows 还会记录私有提交量、
进程启动以来的峰值工作集和句柄数。工作集包含共享驻留页，判断真实进程内存成本时应优先比较
`memory.private`，并结合 `memory.working_set` 判断当前物理内存压力。

Watcher sampling runs from the controller process. The built-in `baseline` and `stress` scenarios
carry a versioned 25-target publication set; `--watcher-target` adds asset-specific targets without
replacing or duplicating scenario targets:
Watcher 采样由控制器进程执行。内置 `baseline` 和 `stress` 场景携带版本化的 25 目标发布集；
`--watcher-target` 用于追加资产专用目标，不会替换或重复场景目标：

```powershell
python -B -m performance.run --scenario performance/scenarios/baseline.json `
  --assets-root D:\path\to\assets `
  --tools-root ../../tools/server `
  --watcher-target BOTS_TYPE=@bots:root/bots/performance
```

`@bots` discovers the ephemeral internal endpoint from logs owned by the current run. A fixed
`HOST:PORT` remains available when the endpoint is managed externally.
`@bots` 会从本轮自有日志中发现临时内部端点；由外部环境固定端点时仍可使用 `HOST:PORT`。

Scenarios may lower the steady-state frequency of an expensive target without changing readiness:
场景可单独降低大目录的稳态采样频率，而不改变 readiness 查询：

```json
{
  "watcher_intervals": {
    "BOTS_TYPE:root/bots/performance": 5.0
  }
}
```

Keys use `COMPONENT_TYPE:PATH`; values are seconds and must be at least `0.1`. Unknown keys fail
before the cluster starts. Each target keeps an independent monotonic deadline, failed queries wait
for their next period, and missed periods are not replayed as a burst. Targets slower than the base
sampling interval receive deterministic phase offsets across their available slots. For example,
five-second targets are spread over five one-second slots instead of interrupting every component on
the same tick. The offset is published as `sampling/configuredPhaseOffsetMs`.
键格式为 `COMPONENT_TYPE:PATH`，值为秒且不得小于 `0.1`。未知目标会在集群启动前失败。
每个目标使用独立单调时钟截止点；失败查询等待下一周期，错过的周期不会集中补发。慢于基础
采样周期的目标会确定性分散到可用槽位，例如五秒目标分散到五个一秒槽，而不是在同一 Tick
打断所有组件；具体偏移通过 `sampling/configuredPhaseOffsetMs` 发布。

The standard targets cover Bots performance; BaseApp root, stats, channels, poller, KCP, Tick health,
client input, gameTick, scriptCall, and onTimer; and CellApp root, stats, channels, poller, Tick health,
client input, gameTick, scriptCall, onTimer, clientUpdate, and Witness. Seven additional `/latency` targets publish the exact recent distributions
for the BaseApp/CellApp profiles. Readiness queries only the targets that contain a configured
readiness metric. Other targets begin sampling in the steady-state window, avoiding unnecessary startup load.
标准目标覆盖 Bots performance；BaseApp root、stats、channels、poller、KCP、Tick 健康度、客户端输入、
gameTick、scriptCall、onTimer；以及 CellApp root、stats、channels、poller、Tick 健康度、客户端输入、
gameTick、scriptCall、onTimer、clientUpdate 和 Witness；另有 7 个 `/latency` 子目标发布 BaseApp/CellApp Profile 的真实近期分布。
readiness 只查询能够覆盖已配置就绪指标的目标，其余目标在稳态窗口开始采样，避免给启动阶段增加无关控制面负载。

Watcher protocol responses do not carry a request ID. The collector therefore reuses one connection
per `(component_type, host, port, path)`, not merely per endpoint. This prevents a delayed or chunked
response for one path from being consumed by the next path query on the same component.
Watcher 协议响应不携带 request ID，因此采集器按 `(component_type, host, port, path)` 独立复用连接，
而不是只按端点复用。这样可防止某一路径的延迟或分块响应被同组件的下一路径查询误收。

The standard publication set includes:
标准发布数据集包括：

- `process.cluster:*`: server CPU, private/working-set memory, peak memory, threads, and handles;
- `process.workload:*`: Bots process resources when an external workload is owned;
- `process.controller:*`: the sampler/controller's own CPU, memory, threads, and handles;
- `watcher.*.<path>/*`: engine metrics returned by each target;
- `watcher.*.<path>/sampling/*`: configured interval and phase offset, actual interval, response
  value count, protocol-neutral estimated response bytes, and connection reuse;
- request latency `mean`, `p50`, `p95`, `p99`, `p99.9`, `max`, and `total`, plus operation-level
  success/error/due/skipped counters.

`responseBytesEstimated` is a JSON-equivalent estimate, not an on-wire protocol byte count. The
controller process runs outside engine threads, so its resource samples measure observability cost
separately from server workload cost.
`responseBytesEstimated` 是 JSON 等价估算值，不是线协议字节数。控制器运行在引擎线程之外，
其资源样本用于与服务端工作负载成本分开发布。

`--start-cluster` starts the existing nine-component integration controller, waits for its
readiness marker, samples only the published child PIDs, and requests graceful cleanup through
an owned stop file. It requires `--cluster-components` and `--cluster-binary-root`.
`--start-cluster` 会启动现有九组件集成控制器、等待就绪标记、仅采样其发布的子进程 PID，
并通过本轮停止文件执行优雅清理；同时必须提供 `--cluster-components` 和
`--cluster-binary-root`。

The reported request latency currently measures Watcher control-plane round trips. It is not a
gameplay RPC latency or success-rate claim. Gameplay SLAs require an asset-specific correlated
request/response producer that emits the same JSONL request contract.
当前报告的请求延迟是 Watcher 控制面往返时间，不代表游戏业务 RPC 的延迟或成功率。
玩法 SLA 仍需要资产专用的关联请求/响应生产器，并输出相同 JSONL 请求契约。

`root/network/messageProcessing` publishes sampled synchronous handler time for client movement,
Python methods, Cell migration, Watcher control, and other inbound messages. Client movement is sampled
1/64, Python methods 1/8, migration and Watcher control in full, and other messages 1/256. Cell AOI
Enter/Leave encoding time is sampled independently at 1/32 under `root/witness/messages` because it is
not entered through an inbound message handler. Totals, averages, maxima, and slow-sample counts are
sample statistics and must not be multiplied into exact request totals.
`root/network/messageProcessing` 发布客户端移动、Python 方法、跨 Cell 迁移、Watcher 控制和其他入站
消息的同步 handler 采样耗时；采样率依次为 1/64、1/8、完整、完整和 1/256。Cell AOI Enter/Leave
编码不经过入站 handler，因此在 `root/witness/messages` 下独立按 1/32 采样。累计、平均、最大值及
慢样本数均属于采样统计，不能直接外推为精确请求总量。

`root/scriptStages` splits BaseApp/CellApp RPC work into method lookup, Python attribute lookup,
argument decoding, Python execution, and cleanup. CellApp additionally publishes migration
serialization, cross-Cell forwarding, target deserialize/create, and callback time. RPC stages use
the same deterministic 1/8 sampling policy; migration stages are sampled in full. Eight fixed slow
slots retain only observations at or above 1 ms and publish handler name, stage, and duration without
growing with uptime.
`root/scriptStages` 将 BaseApp/CellApp RPC 拆为方法查找、Python 属性查找、参数解包、Python 执行和
清理阶段；CellApp 还发布迁移序列化、跨 Cell 转发、目标端反序列化/创建及回调耗时。RPC 阶段按
确定性的 1/8 采样，迁移阶段全量采样。8 个固定慢样本槽只保留不低于 1 ms 的 handler 名称、阶段和
耗时，内存不会随运行时间增长。
慢样本的名称和阶段以 `kind=label` 写入 JSONL，并在 `summary.json`/`report.md` 的 Labels 中保留
每个组件槽位的最新非空值；它们不会进入数值直方图或质量门计算。

For cprofile targets, the runner preserves raw stamp counters, converts time counters to microseconds
using `root/stats/stampsPerSecond`, and derives interval call count, calls/second, total/self time, and
mean total/self time. The dedicated `/latency` targets are different: selected BaseApp/CellApp profiles
retain exact outermost-call durations from the last 10 seconds, capped at the newest 10,000 samples,
and publish count, mean, P50, P95, P99, and max. P99.9 is omitted unless all 10,000 slots are populated.
At high throughput this is a newest-10,000-call window shorter than 10 seconds; nested scopes sharing
one `ProfileVal` are represented by their outermost duration.
对于 cprofile 目标，运行器保留原始 stamp 计数，使用 `root/stats/stampsPerSecond` 换算微秒，并派生
间隔调用数、每秒调用数、总/自耗时及平均总/自耗时。专用 `/latency` 目标口径不同：指定的
BaseApp/CellApp Profile 精确保留最近 10 秒、最多最新 10,000 个最外层调用时长，并发布 Count、Mean、
P50、P95、P99 和 Max；只有 10,000 个槽位全部有样本时才发布 P99.9。高吞吐时它代表短于 10 秒的
“最新 10,000 次”窗口；共享同一 `ProfileVal` 的嵌套作用域按最外层总时长记录。

The scenario `bots` value sets `bots/defaultAddBots/totalCount` in the isolated overlay.
场景中的 `bots` 值会写入隔离覆盖层的 `bots/defaultAddBots/totalCount`。

Scenario readiness rules can compare a Watcher metric with `$bots`; the measurement window starts
only after all rules match and `warmup_seconds` has elapsed.
场景就绪规则可将 Watcher 指标与 `$bots` 比较；全部满足并完成 `warmup_seconds` 预热后才开始计时。

A readiness rule may also require a numeric lower bound with `{"min": 1}`. The
`python_latency.json` scenario uses this form for `pythonLatency/control/successes`, so an enabled
probe with no completed transaction cannot produce an empty PASS.
就绪规则也可使用 `{"min": 1}` 声明数值下限。`python_latency.json` 用它约束
`pythonLatency/control/successes`，因此探针虽启用但没有完成事务时不能空载通过。

`python_latency.json` is an opt-in end-to-end Python probe. It samples one in every 50 Bots every
two seconds (about 20 transactions/second at 2,000 Bots), permits one in-flight request per sampled
Bot, and publishes round-trip plus Client-to-Base, Base-to-Cell, Cell-to-Base, and Base-to-Client
P50/P95/P99 distributions. The five native windows allocate about 240 KiB only when
`KBE_PERF_PYTHON_CHAIN=1`; with the flag absent, no probe callback, RPC, or window allocation occurs.
P99.9 remains explicitly unavailable unless the retained window has enough samples.
`python_latency.json` 是按需启用的 Python 全链路探针。它每两秒抽取 1/50 Bots（2,000 Bots 时约
20 transaction/s），每个被抽样 Bot 最多保留一个在途请求，并发布 RoundTrip、Client-to-Base、
Base-to-Cell、Cell-to-Base、Base-to-Client 的 P50/P95/P99。五个原生窗口仅在
`KBE_PERF_PYTHON_CHAIN=1` 时分配约 240 KiB；未设置开关时不创建探针定时器、不发送 RPC、也不分配窗口。
保留窗口样本不足时，P99.9 通过 `p999Available=0` 明确标记为不可用。

The built-in scenarios use the versioned `network-baseline` fixture. It creates one empty Space and
places each Avatar outside every other Avatar's AOI, so startup cost, NPCs, database character writes,
and gameplay timers do not contaminate the engine connection baseline. Asset-specific load belongs in
a separate scenario and must not be compared with this baseline.
内置场景使用版本化的 `network-baseline` fixture：仅创建一个空 Space，并让各 Avatar 互不进入 AOI，
避免启动逻辑、NPC、角色数据库写入和玩法定时器污染引擎连接基线。业务资产负载应使用独立场景，
不可与本基线直接比较。

Readiness timeout is a reportable failure. The runner records the last complete Watcher snapshot,
one process snapshot, and the closed-run log counters, writes `summary.json` plus `report.md`, then exits
with a non-zero status.
就绪超时属于可报告失败：运行器会记录最后一份完整 Watcher 快照、一次进程快照和闭合后的日志计数，
生成 `summary.json` 与 `report.md`，随后以非零状态退出。

Database scenarios are intentionally excluded from this phase.
本阶段明确不包含数据库场景。

For a long soak, use a 10-second sample interval and keep Python probes disabled when measuring
the connection/memory baseline. Compare the first and last five minutes of
`process.workload.memory.private`, `process.workload.memory.working_set`, `handles.active`,
`kcpDynamicAllocatedBytes`, `contextsOutstandingBytes`, and `contextsCachedBytes` instead of
using the full-window maximum as a leak claim.
长时间 soak 建议使用 10 秒采样间隔；测量连接和内存基线时关闭 Python 探针。应比较
`process.workload.memory.private`、`process.workload.memory.working_set`、`handles.active`、
`kcpDynamicAllocatedBytes`、`contextsOutstandingBytes` 和 `contextsCachedBytes` 的前后五分钟均值及斜率，
不能把全窗口最大值直接当作泄漏结论。

`quality.status=SLOW` can be caused by an occasional Watcher control-plane spike while readiness,
network errors, and process health remain valid. Treat it separately from gameplay RPC SLAs; the
Watcher round trip is an external diagnostic measurement, not a business request latency.
`quality.status=SLOW` 可能只是 Watcher 控制面偶发尖峰，而 readiness、网络错误和进程健康仍然有效。
必须与业务 RPC SLA 分开解释；Watcher 往返是外部诊断指标，不是业务请求延迟。

## Windows CPU profile

`windows_cpu_profile.ps1` records a low-frequency CPU ETL with Visual Studio DiagnosticsHub and
exports both a raw xperf module report and a machine-readable `cpu-summary.json`. It does not need
the elevated WPR system-profile policy. Use exact PIDs when several test clusters may coexist:

```powershell
$baseappPids = @(Get-Process baseapp | Select-Object -ExpandProperty Id)
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\kbe\src\tests\performance\windows_cpu_profile.ps1 `
  -ProcessId $baseappPids `
  -DurationSeconds 60 `
  -OutputDirectory .\kbe\src\out\performance-runs\cpu-profile `
  -IncludeStacks `
  -DownloadMicrosoftSymbols
```

The module report covers the whole machine so Bots, server components, security software, and
diagnostic tools remain separate process groups. `target_modules` in the JSON summary contains only
the selected PIDs. Stack export is opt-in because symbol download and HTML generation are slower and
larger than module aggregation. Visual Studio DiagnosticsHub and the Windows Performance Toolkit
(`xperf.exe`) must be installed; the script never stops security software or network filter drivers.

`windows_cpu_profile.ps1` 使用 Visual Studio DiagnosticsHub 低频采集 CPU ETL，并同时导出 xperf
模块报告与机器可读的 `cpu-summary.json`，不依赖需要管理员权限的 WPR 系统性能策略。模块报告覆盖
整机，使 Bots、服务端、安全软件和诊断工具保持独立进程分组；JSON 的 `target_modules` 只汇总指定
PID。调用栈和微软符号下载默认关闭，因为其耗时和产物体积明显高于模块汇总。脚本只采集和报告，
不会停止安全软件或网络过滤驱动。
