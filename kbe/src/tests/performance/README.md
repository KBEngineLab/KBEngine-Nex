# KBE Performance Lab

The Performance Lab runs outside the engine's hot path and records append-only JSONL observations.
性能实验室运行在引擎热路径之外，并记录追加式 JSONL 观测数据。

Run a short local sample:

```powershell
python -B -m performance.run --scenario performance/scenarios/smoke.json --duration 30 --command "<workload command>"
```

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
carry a versioned 18-target publication set; `--watcher-target` adds asset-specific targets without
replacing or duplicating scenario targets:
Watcher 采样由控制器进程执行。内置 `baseline` 和 `stress` 场景携带版本化的 18 目标发布集；
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
for their next period, and missed periods are not replayed as a burst.
键格式为 `COMPONENT_TYPE:PATH`，值为秒且不得小于 `0.1`。未知目标会在集群启动前失败。
每个目标使用独立单调时钟截止点；失败查询等待下一周期，错过的周期不会集中补发。

The standard targets cover Bots performance; BaseApp root, stats, channels, poller, KCP, gameTick,
scriptCall, and onTimer; and CellApp root, stats, channels, poller, gameTick, scriptCall, onTimer,
clientUpdate, and Witness. Readiness queries only the targets that contain a configured readiness
metric. Other targets begin sampling in the steady-state window, avoiding unnecessary startup load.
标准目标覆盖 Bots performance；BaseApp root、stats、channels、poller、KCP、gameTick、
scriptCall、onTimer；以及 CellApp root、stats、channels、poller、gameTick、scriptCall、onTimer、
clientUpdate 和 Witness。readiness 只查询能够覆盖已配置就绪指标的目标，其余目标在稳态窗口开始采样，
避免给启动阶段增加无关控制面负载。

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
- `watcher.*.<path>/sampling/*`: configured and actual interval, response value count,
  protocol-neutral estimated response bytes, and connection reuse;
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

For cprofile targets, the runner preserves raw stamp counters, converts time counters to microseconds
using `root/stats/stampsPerSecond`, and derives window call count, calls/second, total/self time, and
mean total/self time. `ProfileVal` does not retain an invocation distribution, so these values are not
Python P50/P95/P99/P99.9. A real percentile requires a bounded low-overhead histogram in the profiled
hot path; P99.9 should only be published when a window contains at least 10,000 samples.
对于 cprofile 目标，运行器保留原始 stamp 计数，使用 `root/stats/stampsPerSecond` 换算微秒，并派生
窗口调用数、每秒调用数、总/自耗时及平均总/自耗时。`ProfileVal` 不保存单次调用分布，因此这些值不是
Python P50/P95/P99/P99.9。真实分位数需要在被测热路径加入有界低开销直方图；窗口样本不足 10,000 时
不发布 P99.9。

The scenario `bots` value sets `bots/defaultAddBots/totalCount` in the isolated overlay.
场景中的 `bots` 值会写入隔离覆盖层的 `bots/defaultAddBots/totalCount`。

Scenario readiness rules can compare a Watcher metric with `$bots`; the measurement window starts
only after all rules match and `warmup_seconds` has elapsed.
场景就绪规则可将 Watcher 指标与 `$bots` 比较；全部满足并完成 `warmup_seconds` 预热后才开始计时。

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
