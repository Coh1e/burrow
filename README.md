# Burrow

`brw` 是 Burrow 的原生命令行，用 Digby 执行系统操作，用 Blueprint 组合安装和环境配置步骤。

## 安装

原生候选包解压后包含 `brw.exe` 和许可文件。已发布的 Scoop 兼容版 v0.3.5 提供 `burrow.exe` 和同级的 `scoop-lib/`，可通过 Scoop 安装。[发布版本](https://github.com/Coh1e/burrow/releases)

```powershell
scoop bucket add coh1e https://github.com/Coh1e/scoop-bucket
scoop install coh1e/burrow
```

## 构建与测试

原生 Windows 配置使用 MinGW-w64 与 CMake。从仓库根目录预置固定依赖，构建后运行 CTest。[原生构建命令](brw/README.md)

构建 Scoop 兼容版需要 Windows x64、MSVC、Ninja 和 Windows PowerShell 5.1。运行 `./scripts/fetch-deps.ps1` 和 `./scripts/build.ps1`。[兼容版发布流程](docs/release-process.md)

## 常用命令

```powershell
brw source add sdk C:\fixtures\catalog.tsv
brw search tool
brw show sdk/tool
brw install sdk/tool --dry-run --json
brw install sdk/tool
brw list
```

## 能力边界

原生首版候选面向 Windows 11 x64。预览按依赖顺序列出操作，实际安装重新读取包源和安装状态，校验载荷后逐包提交。Blueprint 需要可信的清单摘要。Linux 最小版提供文件、文本和 tar 操作；macOS 用于验收共享核心。[候选发布验收](docs/native-release-acceptance.md) · [Scoop 兼容版范围](docs/coverage-report.md)

## 文档索引

- 原生包、蓝图与工具链。[包源格式](brw/README.md) · [Blueprint](brw/blueprint.md) · [Windows devkit](docs/windows-devkit.md) · [旧 Linux 最小版](brw/minimal.md)
- Scoop 兼容版。[命令与退出码](docs/cli-boundaries.md) · [共用状态](docs/architecture.md) · [恢复](docs/recovery-support.md)

## 许可证

Burrow 以 [MIT 许可证](LICENSE) 发布。随包分发的第三方组件仍按各自许可证分发，来源与全文见 [THIRD_PARTY.md](THIRD_PARTY.md)。
