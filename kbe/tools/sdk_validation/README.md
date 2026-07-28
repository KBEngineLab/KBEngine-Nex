# KBEngine 2.8 SDK validation

该工具把客户端 SDK 的生成、构建和端到端运行统一为一个清单驱动的验收流程。目前只接受迁移范围内的 `csharp`、`cxx`、`typescript` 和 `gdscript`，不会重新引入 Unity、UE 或旧 JavaScript SDK。

This tool provides one manifest-driven validation flow for client SDK generation, compilation, and end-to-end execution. It accepts only the migration targets `csharp`, `cxx`, `typescript`, and `gdscript`; it does not reintroduce Unity, UE, or the legacy JavaScript SDK.

## Requirements

- PowerShell 7 or later.
- A built `kbe/bin/server/kbcmd.exe` when the manifest contains `generate` stages.
- The compiler/runtime required by each selected SDK, such as VS 2022 v143, .NET, Node.js, or Godot.
- A running KBEngine server and an application-specific E2E harness when a manifest contains `run` stages.

PowerShell 7 是硬性要求，因为执行器依赖现代 .NET 的结构化参数列表、异步管道读取和进程树终止。工具不会安装依赖，也不会启动或修改数据库、防火墙与生产服务。

PowerShell 7 is required because the runner uses modern .NET structured argument lists, asynchronous pipe reads, and process-tree termination. It does not install dependencies or start and modify databases, firewalls, or production services.

## Usage

从示例复制一份项目清单并修改资产、编译器和 E2E 工程路径。清单属于具体游戏项目，不应写入引擎仓库。

Create a project manifest from the example and update the asset, compiler, and E2E project paths. A manifest belongs to a game project and should not be committed to the engine repository.

```powershell
pwsh -File kbe/tools/sdk_validation/Invoke-SdkValidation.ps1 `
  -ManifestPath D:/game/assets/sdk-validation.json
```

只运行 TypeScript 和 C++，并复用已经生成的 SDK：

Run only TypeScript and C++ while reusing SDKs that were already generated:

```powershell
pwsh -File kbe/tools/sdk_validation/Invoke-SdkValidation.ps1 `
  -ManifestPath D:/game/assets/sdk-validation.json `
  -Sdk typescript,cxx `
  -SkipGenerate
```

只运行指定故障场景，并跳过各 SDK 的基线 `run`：

Run selected failure scenarios without executing each SDK's baseline `run` stage:

```powershell
pwsh -File kbe/tools/sdk_validation/Invoke-SdkValidation.ps1 `
  -ManifestPath D:/game/assets/sdk-failure-validation.json `
  -Scenario heartbeat-timeout,repeated-close-kcp `
  -SkipGenerate
```

只验证生成和编译，不连接服务器：

Validate generation and compilation without connecting to a server:

```powershell
pwsh -File kbe/tools/sdk_validation/Invoke-SdkValidation.ps1 `
  -ManifestPath D:/game/assets/sdk-validation.json `
  -SkipRun
```

## Manifest contract

- `${RepoRoot}` and `${ManifestDir}` are reserved built-in variables.
- Custom scalar variables are declared under `variables` and may reference other variables.
- Manifest-level environment variables apply to every command; stage-level values override them.
- Every stage uses a `file` plus an `arguments` array. Commands are not evaluated as PowerShell source.
- `scenarios` is an optional array of named run commands. Names are case-insensitive and limited to letters, digits, `.`, `_`, and `-`, producing stable and safe log filenames.
- A scenario may define a `verify` command. It runs after the main command even when that command fails or times out, and receives its own `scenario-<name>-verify` log and summary entry.
- `-Scenario` selects named scenarios across the selected SDKs and skips the baseline `run`; `-SkipRun` skips both the baseline and every scenario.
- A stage fails on timeout, a nonzero exit code, a missing `requiredPatterns` regular expression, or a matching `forbiddenPatterns` regular expression.
- If generation or build fails, later stages for that SDK are marked `blocked`; other SDKs continue so the final report captures independent failures.
- Scenarios run after successful generation and build and are independent. A failed scenario does not block the remaining scenarios for that SDK, preserving the complete failure matrix in one report.
- Missing stages and stages disabled by `-SkipGenerate`, `-SkipBuild`, or `-SkipRun` are marked `skipped`.
- Successful stages keep full output in their log and print only the PASS summary; `-ShowOutput` enables full terminal output, while failed stages automatically print the last 80 lines.

每次执行会为各阶段保存 UTF-8 日志，并生成 `summary.json`。默认输出目录是清单旁的 `sdk-validation-results`，也可以通过清单的 `resultsDirectory` 或命令行 `-ResultsPath` 覆盖。构建产物和验收日志不应提交到 Git。

Each run saves a UTF-8 log for every stage and writes `summary.json`. The default output directory is `sdk-validation-results` next to the manifest; override it through manifest `resultsDirectory` or command-line `-ResultsPath`. Build output and validation logs should not be committed to Git.

## Failure recovery contract

`sdk-failure-matrix.example.json` 定义推荐的四端恢复矩阵。命令仍由具体项目的 E2E 夹具提供；只需调整路径，不要把游戏业务相关的故障注入移入引擎。每个场景的 `verify` 使用 `Wait-SdkResourceRelease.py` 读取 BaseApp 内部 watcher 端口；该动态端口应从 Machine 组件信息获取并写入项目清单的 `BaseappWatcherPort`。

`sdk-failure-matrix.example.json` defines the recommended four-SDK recovery matrix. The commands remain project-owned E2E harnesses; update their paths without moving game-specific fault injection into the engine. Each scenario's `verify` uses `Wait-SdkResourceRelease.py` against the BaseApp internal watcher port; obtain that dynamic port from Machine component information and set `BaseappWatcherPort` in the project manifest.

- `E2E_PASS ...` proves the expected disconnect count, relogin, Space reconstruction, Entity Component reconstruction, and post-recovery RPC.
- `RESOURCE_PASS client_connections=0 server_channels=0 kcp_timers=0` is emitted by the independent verifier after external Channels, KCP control blocks, and KCP update timers all reach zero.

Keep `E2E_FAIL` and `RESOURCE_FAIL` in `forbiddenPatterns`. This prevents a successful process exit from hiding a protocol, state-rebuild, or resource-lifecycle failure.

必须把 `E2E_FAIL` 与 `RESOURCE_FAIL` 保留在 `forbiddenPatterns` 中，避免进程以成功状态退出时掩盖协议、状态重建或资源生命周期错误。

执行以下命令可回归场景隔离、资源 watcher、C# 心跳与并发关闭状态，以及 TCP 发送/接收队列的容量、等待唤醒、回绕、并发、故障重启和 KCP 原子背压。PowerShell 合成产物只写入系统临时目录，C# 测试工程的 `bin/obj` 已按专用目录模式精确忽略：

Run the following commands to verify scenario isolation, resource watchers, C# heartbeats and concurrent close state, plus TCP send/receive queue capacity, wait/wake behavior, wraparound, concurrency, failure restart, and atomic KCP backpressure. PowerShell fixtures write only to the system temporary directory, while the dedicated C# test projects' `bin/obj` paths are narrowly ignored:

```powershell
pwsh -File kbe/tools/sdk_validation/tests/Test-SdkValidation.ps1
python -B -m unittest kbe/tools/sdk_validation/tests/Test-SdkResourceRelease.py -v
dotnet run --project kbe/tools/sdk_validation/tests/csharp_heartbeat/CsharpHeartbeatStateTest.csproj -c Release
dotnet run --project kbe/tools/sdk_validation/tests/csharp_tcp_send/CsharpTcpSendQueueTest.csproj -c Release
```

The full schema is available in `sdk-validation.schema.json`.
