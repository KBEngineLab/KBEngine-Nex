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

### CTest integration

默认 CMake 配置会注册当前机器具备依赖的离线 SDK 测试。测试清单由本工具目录维护，输出统一写入构建树的 `Testing/sdk-validation-<config>`，不会在源码夹具中产生 `bin`、`obj`、`x64` 或 `dist`。单独构建 `kbcmd` 会同步部署嵌入式 Python 动态库与标准库，随后可通过 `sdk` 标签运行全部已注册测试；完整服务验收仍构建 `kbe_runtime`。也可叠加 `offline`、`csharp`、`cxx`、`typescript`、`gdscript`、`release` 等标签缩小范围。

The default CMake configuration registers offline SDK tests whose dependencies are available on the current machine. This tool directory owns the test manifest, and all output is written under `Testing/sdk-validation-<config>` in the build tree, so source fixtures do not receive `bin`, `obj`, `x64`, or `dist` directories. Building `kbcmd` alone also deploys the embedded Python libraries and standard library, after which every registered test can run through the `sdk` label; full server validation still builds `kbe_runtime`. Combine labels such as `offline`, `csharp`, `cxx`, `typescript`, `gdscript`, and `release` to narrow the set. Run preset commands from `kbe/src`, where `CMakePresets.json` is located.

```powershell
cmake --build --preset windows-vs2022-kbcmd-debug
ctest --preset windows-vs2022-debug -L sdk

cmake --build --preset windows-vs2022-kbcmd-release
ctest --preset windows-vs2022-release -L sdk
```

PowerShell 7、.NET、Node.js 和 MSBuild 会自动发现，也可通过同名 CMake 缓存变量或环境变量覆盖。完整 Windows 候选门禁还需要 `KBE_TEST_ASSETS`、`KBE_SDK_TYPESCRIPT_EXECUTABLE` 与 `KBE_SDK_GODOT_EXECUTABLE`；当 assets 内存在 `kbeclient/typescript-component-e2e/node_modules/.bin/tsc.cmd` 时会自动复用该编译器。Godot 变量必须指向 console 可执行文件，而不是安装目录。

PowerShell 7, .NET, Node.js, and MSBuild are discovered automatically and can be overridden through same-named CMake cache variables or environment variables. The full Windows candidate gate also requires `KBE_TEST_ASSETS`, `KBE_SDK_TYPESCRIPT_EXECUTABLE`, and `KBE_SDK_GODOT_EXECUTABLE`; when the assets contain `kbeclient/typescript-component-e2e/node_modules/.bin/tsc.cmd`, that compiler is reused automatically. The Godot variable must point to the console executable rather than its installation directory.

```powershell
$env:KBE_TEST_ASSETS = "D:/game/assets"
$env:KBE_SDK_GODOT_EXECUTABLE = "D:/tools/Godot/Godot_console.exe"
cmake --preset windows-vs2022
ctest --preset windows-vs2022-release -L sdk -L release
```

在线四端 E2E 不会因本机恰好存在某份 assets 而自动启用。只有显式设置 `KBE_SDK_VALIDATION_MANIFEST` 时才注册 `sdk.e2e.manifest`，防止默认 CTest 修改或连接未知项目环境。清单路径不存在时，配置阶段直接失败。

Online four-SDK E2E is never enabled merely because an assets tree happens to exist locally. `sdk.e2e.manifest` is registered only when `KBE_SDK_VALIDATION_MANIFEST` is set explicitly, preventing default CTest runs from modifying or connecting to an unknown project environment. Configuration fails immediately when the manifest path does not exist.

```powershell
$env:KBE_SDK_VALIDATION_MANIFEST = "D:/game/assets/sdk-validation.json"
cmake --preset windows-vs2022
ctest --preset windows-vs2022-release -R "^sdk\.e2e\.manifest$"
```

## Command-line usage

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

执行以下命令可回归场景隔离、资源 watcher、C# 心跳与并发关闭状态，C#、C++ TCP 背压，TypeScript 根模块依赖、兼容导出身份、单调心跳、连接代次、单次断线通知、Bundle 单帧原子提交、60/70 KiB 出站长度与 WebSocket 待发送字节背压，以及 TypeScript/GDScript WebSocket 入站普通长度、扩展长度、整帧原子验证和畸形边界拒绝。PowerShell 合成产物只写入系统临时目录，C# 的 `bin/obj`、C++ 的 `x64` 和 TypeScript 的 `dist` 测试产物已按专用目录模式精确忽略。TypeScript 模块测试需要先把生成 SDK 编译到传入的第二个目录。Godot runner 需要隔离的已生成 GDScript SDK 目录；目录不是项目时会添加最小 `project.godot` 以建立 `class_name` 缓存：

Run the following commands to verify scenario isolation, resource watchers, C# heartbeats and concurrent close state, C# and C++ TCP backpressure, TypeScript root-module dependencies, compatibility export identity, monotonic heartbeat state, connection generations, single disconnect notification, atomic single-frame Bundle submission, 60/70 KiB outbound lengths, and WebSocket pending-byte backpressure, plus TypeScript/GDScript inbound WebSocket normal lengths, extended lengths, whole-frame atomic validation, and malformed-boundary rejection. PowerShell fixtures write only to the system temporary directory, while dedicated C# `bin/obj`, C++ `x64`, and TypeScript `dist` outputs are narrowly ignored. Compile the generated TypeScript SDK into the second path before running the module test. The Godot runner requires an isolated generated GDScript SDK directory; when it is not already a project, the runner adds a minimal `project.godot` to build the `class_name` cache:

```powershell
pwsh -File kbe/tools/sdk_validation/tests/Test-SdkValidation.ps1
pwsh -File kbe/tools/sdk_validation/tests/Test-CxxSdkBoundaries.ps1
pwsh -File kbe/tools/sdk_validation/tests/Test-CsharpSdkWarnings.ps1
pwsh -File kbe/tools/sdk_validation/tests/Test-SdkReleaseArtifacts.ps1
python -B -m unittest kbe/tools/sdk_validation/tests/Test-SdkResourceRelease.py -v
dotnet run --project kbe/tools/sdk_validation/tests/csharp_heartbeat/CsharpHeartbeatStateTest.csproj -c Release
dotnet run --project kbe/tools/sdk_validation/tests/csharp_tcp_send/CsharpTcpSendQueueTest.csproj -c Release
& "E:/ProgramFiles/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" kbe/tools/sdk_validation/tests/cxx_tcp_receive/CxxTcpReceiveQueueTest.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal
& kbe/tools/sdk_validation/tests/cxx_tcp_receive/x64/Release/CxxTcpReceiveQueueTest.exe
tsc -p kbe/tools/sdk_validation/tests/typescript_websocket/tsconfig.json
node kbe/tools/sdk_validation/tests/typescript_websocket/dist/tools/sdk_validation/tests/typescript_websocket/Program.js
node kbe/tools/sdk_validation/tests/typescript_modules/Program.js D:/generated/typescript D:/generated/typescript/dist
pwsh -File kbe/tools/sdk_validation/tests/gdscript_websocket/Invoke-GdscriptWebSocketTest.ps1 -SdkPath D:/generated/gdscript -GodotPath D:/ProgramFiles/Godot/Godot_v4.7-stable_win64/Godot_v4.7-stable_win64_console.exe
```

The full schema is available in `sdk-validation.schema.json`.

## Release artifacts

发布目录必须是新目录或空目录，生成器不会删除调用方已有内容。以下命令从同一份 assets clean-generate 四端 SDK，拒绝构建缓存、本地二进制和未知文件类型，并生成不含本机绝对路径的 `sdk-release-manifest.json`。manifest 记录生成器 SHA-256、四端一致的版本/协议/EntityDef 摘要、每个文件与每棵 SDK tree 的 SHA-256，以及整套发布产物的根摘要；自动化测试还会在独立目录重复生成并验证根摘要稳定。

The release directory must be new or empty; the generator never deletes caller-owned content. This command clean-generates all four SDKs from one assets tree, rejects build caches, local binaries, and unknown file types, and writes `sdk-release-manifest.json` without local absolute paths. The manifest records the generator SHA-256, cross-SDK version/protocol/EntityDef metadata, SHA-256 values for every file and SDK tree, and one root digest for the complete release set; the automated test repeats generation in an independent directory and requires the root digest to remain stable.

```powershell
pwsh -File kbe/tools/sdk_validation/New-SdkReleaseArtifacts.ps1 `
  -AssetsPath D:/game/assets `
  -OutputPath D:/release/kbe-sdk-2.8.2
```

Windows 最终候选门禁会在生成上述不可变目录后，于隔离且自动清理的工作目录完成 C# warning-as-error、C++ v143 `/W4 /WX`、TypeScript 4.9 全量编译/模块回归/Map 契约与 AST 零循环依赖、Godot headless parse。它不会启动服务器或修改项目 assets；在线四端 E2E 仍由项目清单负责。TypeScript SDK 仍保留部分历史隐式类型，完整 strict 模式迁移不属于当前 Windows 候选门禁。

The Windows release-candidate gate generates the immutable directory above, then uses isolated, automatically cleaned work directories for C# warning-as-error, C++ v143 `/W4 /WX`, TypeScript 4.9 full compilation, module regression, Map-contract and AST-based zero-cycle checks, and Godot headless parsing. It does not start servers or modify project assets; the project manifest remains responsible for online four-SDK E2E validation. The TypeScript SDK still contains some legacy implicit typing, so full strict-mode migration is outside the current Windows candidate gate.

```powershell
pwsh -File kbe/tools/sdk_validation/Test-WindowsReleaseCandidate.ps1 `
  -AssetsPath D:/game/assets `
  -OutputPath D:/release/kbe-sdk-2.8.2
```

## Release compatibility gate

C# 发布构建必须保持零警告。两份示例清单都拒绝 C# 编译器、SDK 与 MSBuild 的中英文 warning 标记；`Test-CsharpSdkWarnings.ps1` 使用真实 `kbcmd` 和仓库自带 Python assets 生成完整 SDK，再以 `.NET 8 Release` 和 `TreatWarningsAsErrors` 编译，并验证外部销毁与 worker 自触发销毁都能协作结束线程。该门禁禁止通过恢复过时 RNG、`Thread.Abort` 或无效占位事件来换取表面兼容。

C# release builds must remain warning-free. Both example manifests reject localized C# compiler, SDK, and MSBuild warning markers. `Test-CsharpSdkWarnings.ps1` generates a complete SDK with real `kbcmd` and the repository Python assets, compiles it under `.NET 8 Release` with `TreatWarningsAsErrors`, and verifies cooperative shutdown initiated externally and by the worker itself. The gate prevents obsolete RNG APIs, `Thread.Abort`, or inert placeholder events from returning as superficial compatibility fixes.

C++ 发布构建必须保持 `/W4` 零警告。两份示例清单都在 C++ `build.forbiddenPatterns` 中拒绝 MSVC 编译器、链接器与 MSBuild 的中英文 warning 标记，因此即使构建工具错误地返回 `0`，后续运行和故障场景仍会被阻断。`Test-CxxSdkBoundaries.ps1` 还会以 `/W4 /WX` 验证 `uint16/uint32` 受检转换，并通过真实 `kbcmd` 生成证明错误码 `65535` 保持不变、`65536` 与 `-1` 在写入生成表前被拒绝。

C++ release builds must remain warning-free under `/W4`. Both example manifests reject localized MSVC compiler, linker, and MSBuild warning markers through the C++ `build.forbiddenPatterns`, so later runs and failure scenarios remain blocked even if a build tool incorrectly exits with `0`. `Test-CxxSdkBoundaries.ps1` also verifies checked `uint16/uint32` conversions under `/W4 /WX` and uses real `kbcmd` generation to prove error ID `65535` is preserved while `65536` and `-1` are rejected before reaching the generated table.

最终发布矩阵不能只检查进程退出码或宽泛的 `E2E_PASS`。`sdk-validation.example.json` 要求 C# 与 C++ 同时证明服务端 Entity Component 初始流和实体 RPC，TypeScript 还必须证明 WebSocket 断线后的 BaseApp 重登录、Space/组件重建和恢复后 RPC，GDScript 必须通过 Godot headless 的真实 KCP 登录、组件初始流和实体 RPC。项目夹具可以输出更多诊断字段，但不能删除这些稳定标记。

The final release matrix must not rely only on process exit codes or a broad `E2E_PASS`. `sdk-validation.example.json` requires C# and C++ to prove both the server-provided Entity Component initial stream and an entity RPC. TypeScript must additionally prove BaseApp relogin after WebSocket loss, Space and component reconstruction, and a post-recovery RPC. GDScript must complete a real KCP login, component initial stream, and entity RPC under Godot headless. Project fixtures may emit additional diagnostics, but they must retain these stable markers.

四端必须由同一个 `kbcmd.exe`、同一份 EntityDef 和同一次验收运行生成，不能混用历史生成目录。EntityDef 变更后应清空或更换编译输出目录，尤其是 TypeScript 的 `dist`；否则已删除的方法仍可能残留在旧 JavaScript 中并产生假失败或假通过。生成与运行日志应记录 SDK 版本、协议摘要和 EntityDef 摘要，以便确认握手双方使用同一产物。

All four SDKs must be generated by the same `kbcmd.exe`, from the same EntityDef, in the same validation run. Do not mix historical output directories. After an EntityDef change, clear or replace compiler output directories, especially TypeScript `dist`; otherwise removed methods can remain in stale JavaScript and produce false failures or false passes. Generation and runtime logs should retain the SDK version, protocol digest, and EntityDef digest so both handshake sides can be tied to the same artifact set.

项目资产进入矩阵前必须迁移旧 FileDescriptor readiness API。监听 socket 使用 `registerAcceptFileDescriptor(fd, onAccept)`，已连接 socket 使用 `registerReadDataFileDescriptor(fd, onRead)`；`registerReadFileDescriptor` 已按 Nex 契约明确报错，验收工具不得通过恢复旧 API 或忽略 Interfaces 脚本异常来绕过该检查。`ReloginBaseapp` 场景还要求业务脚本在重登录窗口内保留 Proxy；若 `onClientDeath()` 立即销毁 Base/Cell，服务端正确结果是 `SERVER_ERR_ILLEGAL_LOGIN`，不应归因于 SDK。

Project assets must migrate legacy FileDescriptor readiness calls before entering the matrix. Listener sockets use `registerAcceptFileDescriptor(fd, onAccept)`, while connected sockets use `registerReadDataFileDescriptor(fd, onRead)`. `registerReadFileDescriptor` intentionally fails under the Nex contract; validation must not bypass this check by restoring the legacy API or ignoring Interfaces script errors. `ReloginBaseapp` scenarios also require game logic to retain the Proxy during the relogin window. If `onClientDeath()` immediately destroys the Base and Cell entities, `SERVER_ERR_ILLEGAL_LOGIN` is the correct server outcome and is not an SDK defect.
