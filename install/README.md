# 安装与构建

这些脚本统一使用 `kbe/src/CMakePresets.json`，不会再调用旧的 Visual Studio 工程作为构建入口。vcpkg 默认安装到仓库内的 `kbe/vcpkg`，依赖安装目录仍由 CMake manifest 管理，位于 `kbe/src/vcpkg_installed` 和 `kbe/src/python-runtime/vcpkg_installed`。

## Windows

推荐在 **Developer PowerShell for VS 2022** 中运行：

```powershell
.\install\install_windows.ps1 -Configuration Release
```

也可以使用 BAT 入口：

```bat
install\install_windows.bat Release
```

只生成 Ninja 工程但不编译：

```powershell
.\install\install_windows.ps1 -SkipBuild
```

生成可在 Visual Studio 中打开的 VS2022 工程：

```powershell
.\install\generate_vs_project.ps1
```

工程目录：`kbe/src/out/build/windows-vs2022`。

如果当前脚本选择 VS 2026，就会导出到：

```text
kbe/src/out/build/windows-vs2026
```

也可以直接显式指定：

```powershell
.\install\generate_vs_project.ps1 -VisualStudioVersion 2026
```

## Linux / WSL2

```bash
./install/install_linux.sh Release
```

也可以显式使用 POSIX shell：`sh install/install_linux.sh Release`。

脚本会在配置 CMake 前识别 `apt-get`、`dnf`、`yum`、`zypper`、`pacman` 或 `apk`。依赖检测沿用 Nex 已验证的候选包机制：先检查候选包是否已经安装，仅在全部候选都缺失时按顺序尝试对应发行版包名。非 root 用户需要可用的 `sudo`。对于只读 CI 镜像或已经预配依赖的环境，可以设置 `KBE_SKIP_SYSTEM_DEPS=1` 跳过安装，但脚本仍会验证所有必需工具。

Arch Linux 为避免部分升级，更新软件包索引时会执行 `pacman -Syu`。由外部流程统一维护系统版本时，请设置 `KBE_SKIP_PACKAGE_UPDATE=1`。

Linux 构建要求 CMake 3.25 或更高版本。如果系统软件源提供的版本过旧，脚本会通过仓库内 vcpkg 获取受管的 CMake，不会替换系统 CMake。

WSL2 使用宿主机代理时，先将代理地址替换为实际网关，例如：

```bash
export https_proxy=http://172.18.16.1:7897
export http_proxy=http://172.18.16.1:7897
```

## macOS

```bash
./install/install_macos.sh Release
```

也可以显式使用 POSIX shell：`sh install/install_macos.sh Release`。

macOS 需要先安装 Xcode Command Line Tools 和 Homebrew。脚本会先检查各 Homebrew formula，仅安装缺失的 CMake、Ninja、Autotools、`pkg-config`、`libtirpc` 等宿主依赖，并自动配置 keg-only 工具的搜索路径。Xcode Command Line Tools 需要 Apple 的交互式安装流程，缺失时脚本会给出 `xcode-select --install` 提示。

## 可选环境变量

- `KBE_VCPKG_REPOSITORY`：自定义 vcpkg Git 仓库地址。
- `KBE_VCPKG_REF`：首次克隆后检出的 vcpkg 分支、标签或提交；不设置时默认跟随最新分支。
- `KBE_SKIP_SYSTEM_DEPS=1`：跳过 Linux 包管理器或 Homebrew 安装，仅验证现有环境。
- `KBE_SKIP_PACKAGE_UPDATE=1`：Linux 下跳过软件包索引更新；仅建议已提前更新的软件源或 CI 镜像使用。

脚本不会对已有 vcpkg 工作区执行 `reset --hard` 或自动拉取，避免覆盖本地修改；需要升级时请显式操作 `kbe/vcpkg`。

## GitHub Actions

在 GitHub 的 **Actions > Build and release > Run workflow** 中手动运行时，`publish_release` 默认为关闭。保持关闭只会执行三平台构建、CTest 和运行库检查，不会创建标签、产物包或 GitHub Release。需要正式发布时再启用 `publish_release` 并填写 `tag_name`。
