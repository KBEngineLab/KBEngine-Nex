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

Watcher sampling is opt-in and runs from the controller process:
Watcher 采样默认关闭，并且由控制器进程执行：

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
