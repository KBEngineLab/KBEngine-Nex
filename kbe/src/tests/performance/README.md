# KBE Performance Lab

The Performance Lab runs outside the engine's hot path and records append-only JSONL observations.
性能实验室运行在引擎热路径之外，并记录追加式 JSONL 观测数据。

Run a short local sample:

```powershell
python -B -m performance.run --scenario performance/scenarios/smoke.json --duration 30 --command "<workload command>"
```

The runner writes `raw.jsonl`, `summary.json`, and `report.md` below `kbe/src/out/performance-runs/`.
运行器会在 `kbe/src/out/performance-runs/` 下生成 `raw.jsonl`、`summary.json` 和 `report.md`。

Watcher sampling is opt-in and runs from the controller process:
Watcher 采样默认关闭，并且由控制器进程执行：

```powershell
python -B -m performance.run --scenario performance/scenarios/baseline.json `
  --assets-root D:\path\to\assets `
  --tools-root ../../tools/server `
  --watcher-target BOTS_TYPE=127.0.0.1:11000:root/bots/network/poller
```

The scenario `bots` value sets `bots/defaultAddBots/totalCount` in the isolated overlay.
场景中的 `bots` 值会写入隔离覆盖层的 `bots/defaultAddBots/totalCount`。

Database scenarios are intentionally excluded from this phase.
本阶段明确不包含数据库场景。
