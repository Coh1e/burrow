# Burrow

`brw` 是原生包和环境编排命令行。Digby 按计划执行系统操作；Blueprint 用经过校验的本地模块组合这些操作。首版候选面向 Windows 11 x64，使用 MinGW-w64 和 CMake。候选构建、已实测的平台和待验项目见[发布验收记录](docs/native-release-acceptance.md)。

## 原生包与 Blueprint

```powershell
brw source add sdk C:\fixtures\catalog.tsv
brw search tool
brw show sdk/tool
brw install sdk/tool --dry-run --json
brw install sdk/tool
brw list
```

包源目录是 UTF-8 文本。预览按依赖顺序列出操作；实际安装重新读取本地目录和安装状态，校验载荷后逐包提交。Blueprint 的预览与应用另需操作者提供可信清单摘要。[构建、目录格式及安装契约](brw/README.md) · [Blueprint 用法](brw/blueprint.md) · [Windows devkit](docs/windows-devkit.md) · [旧 Linux 最小构建](brw/minimal.md)

## 已发布的 Scoop 兼容版

`burrow.exe` v0.3.5 与用户级 Scoop 共用安装根、bucket 和安装记录。发布 ZIP 中的 `burrow.exe` 与 `scoop-lib/` 需要同级；包内附许可证及第三方声明。可从 Releases 下载，或用以下命令安装。[Releases](https://github.com/Coh1e/burrow/releases) · [兼容范围与实测](docs/coverage-report.md)

```powershell
scoop bucket add coh1e https://github.com/Coh1e/scoop-bucket
scoop install coh1e/burrow
```

构建 Scoop 兼容版需要 Windows x64、MSVC 19.30+、CMake 3.25+、Ninja 和 Windows PowerShell 5.1。在仓库根目录运行 `./scripts/fetch-deps.ps1` 和 `./scripts/build.ps1`。[命令与退出码](docs/cli-boundaries.md) · [共用状态](docs/architecture.md) · [恢复](docs/recovery-support.md) · [发布流程](docs/release-process.md)

## 许可证

Burrow 以 [MIT 许可证](LICENSE) 发布。随包分发的第三方组件仍按各自许可证分发，来源与全文见 [THIRD_PARTY.md](THIRD_PARTY.md)。
