# KCP ingress latency probe / KCP 入口延迟探针

This .NET 8 console tool repeatedly performs KBEngine's real UDP/KCP hello exchange against one BaseApp external UDP endpoint. It measures BaseApp ingress, IOCP dispatch, channel lookup, and hello ACK send latency without requiring game-specific Entity methods.

这个 .NET 8 控制台工具会对一个 BaseApp 外部 UDP 端点重复执行 KBEngine 的真实 UDP/KCP hello 往返。它不依赖业务 Entity 方法，可测量 BaseApp 入口、IOCP 调度、Channel 查找与 hello ACK 发送延迟。

```powershell
dotnet run --project .\kbe\src\tools\kcp_latency_probe\KcpLatencyProbe.csproj -c Release -- `
  --host 172.18.16.1 `
  --port 20015 `
  --duration 60 `
  --interval-ms 100 `
  --timeout-ms 5000 `
  --warmup 10 `
  --output-json .\kbe\src\out\performance-runs\kcp-ingress.json
```

The report uses nearest-rank P50/P95/P99/P99.9 percentiles. Only one request is outstanding. After a timeout, the probe keeps its local port, waits briefly, and drains late ACKs; replacing the port would create temporary server Channels until inactivity cleanup and could distort a loss-heavy test.

报告采用 nearest-rank 计算 P50/P95/P99/P99.9。探针始终只保留一个在途请求；发生超时后保留本地端口，短暂等待并清空迟到 ACK。更换端口会让服务端在 inactivity 清理前保留临时 Channel，进而扭曲高丢包测试。

This is an ingress scheduling metric, not end-to-end Entity RPC latency. Python-to-engine-to-client latency requires an explicit echo RPC in the selected test assets and should be reported separately.

该指标反映入口调度延迟，不等于 Entity RPC 端到端延迟。Python 到引擎再到客户端的延迟需要测试资产显式提供 echo RPC，并应单独发布。
