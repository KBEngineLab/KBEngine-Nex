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
- A stage fails on timeout, a nonzero exit code, a missing `requiredPatterns` regular expression, or a matching `forbiddenPatterns` regular expression.
- If one stage fails, later stages for that SDK are marked `blocked`; other SDKs continue so the final report captures independent failures.
- Missing stages and stages disabled by `-SkipGenerate`, `-SkipBuild`, or `-SkipRun` are marked `skipped`.
- Successful stages keep full output in their log and print only the PASS summary; `-ShowOutput` enables full terminal output, while failed stages automatically print the last 80 lines.

每次执行会为各阶段保存 UTF-8 日志，并生成 `summary.json`。默认输出目录是清单旁的 `sdk-validation-results`，也可以通过清单的 `resultsDirectory` 或命令行 `-ResultsPath` 覆盖。构建产物和验收日志不应提交到 Git。

Each run saves a UTF-8 log for every stage and writes `summary.json`. The default output directory is `sdk-validation-results` next to the manifest; override it through manifest `resultsDirectory` or command-line `-ResultsPath`. Build output and validation logs should not be committed to Git.

The full schema is available in `sdk-validation.schema.json`.
