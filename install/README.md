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

WSL2 使用宿主机代理时，先将代理地址替换为实际网关，例如：

```bash
export https_proxy=http://172.18.16.1:7897
export http_proxy=http://172.18.16.1:7897
```

## macOS

```bash
./install/install_macos.sh Release
```

## 可选环境变量

- `KBE_VCPKG_REPOSITORY`：自定义 vcpkg Git 仓库地址。
- `KBE_VCPKG_REF`：首次克隆后检出的 vcpkg 分支、标签或提交；不设置时默认跟随最新分支。

脚本不会对已有 vcpkg 工作区执行 `reset --hard` 或自动拉取，避免覆盖本地修改；需要升级时请显式操作 `kbe/vcpkg`。
