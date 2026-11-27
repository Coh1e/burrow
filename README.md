# Burrow

`brw` 是 Burrow 的原生命令行，用 Digby 执行系统操作，用 Blueprint 组合安装和环境配置步骤。

## 安装

Windows 11 x64 原生版处于候选验收阶段。如果已取得候选 ZIP，请核对摘要，解压后在该目录运行 `./brw.exe list`。包内包含可执行文件和许可文件。[候选摘要与验收记录](docs/native-release-acceptance.md)

## 构建与测试

在 Windows 上将 MinGW-w64 与 CMake 3.25 或更高版本加入 PATH。在仓库根目录打开 Windows PowerShell 5.1，预置固定依赖并构建、测试。

```powershell
./scripts/fetch-deps.ps1
cmake -S . -B .local/brw-build -G "MinGW Makefiles"
cmake --build .local/brw-build --parallel
ctest --test-dir .local/brw-build --output-on-failure
./.local/brw-build/brw.exe list
```

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

原生首版候选面向 Windows 11 x64。预览按依赖顺序列出操作，实际安装重新读取包源和安装状态，校验载荷后逐包提交。Blueprint 需要可信的清单摘要。Linux 最小版提供文件、文本和 tar 操作。Windows、Linux 和 macOS 共用 Digby 与 Blueprint 行为测试。[三宿主验收记录](brw/README.md#verification-on-native-hosts)

## 文档索引

- [原生包与包源格式](brw/README.md) · [用户环境与注册表](brw/settings.md)
- [Blueprint](brw/blueprint.md) · [Windows devkit](docs/windows-devkit.md)
- [旧 Linux 最小版](brw/minimal.md) · [经 SSH 在 WSL 中离线构建](docs/ssh-build-acceptance.md)

## 许可证

Burrow 采用 MIT 许可证。候选 ZIP 的 `licenses/` 目录包含第三方组件的许可全文。[项目许可](LICENSE) · [第三方来源与许可](THIRD_PARTY.md)
