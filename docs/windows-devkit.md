# 搬移 Windows MinGW-w64 devkit

根蓝图依次调用 `toolchain.mjs` 和 `session.mjs`。前者检查目标目录为空，从本地包源取出锁定的 w64devkit 2.11.0 并解包。后者写入会话入口 `devkit.cmd`。搬移整个目录后，入口根据自身位置选择 `w64devkit\bin`，保留调用者的工作目录。直接打开入口会启动命令提示符，也可以把要执行的命令作为参数传给它。[组合蓝图](../tests/devkit-acceptance/kit/main.mjs) · [平台锁](../tests/devkit-acceptance/kit/blueprint.windows-x86_64.lock)

## 离线复跑

在 Windows 11 x64 上预置启用 Blueprint 的原生 `brw.exe`、Python 3.11 或更高版本，以及官方自解压归档 `w64devkit-x64-2.11.0.7z.exe`。归档的 SHA-256 应为 `54c2bb9962e8a30c8270e34c5efc3d5fee579f53087fa7d435b95fbba2af7b2b`。驱动只从指定的本地文件准备包源。[上游归档](https://github.com/skeeto/w64devkit/releases/download/v2.11.0/w64devkit-x64-2.11.0.7z.exe) · [本地包源目录文件](../tests/devkit-acceptance/catalog)

在无网络能力的 Windows AppContainer 或已断开网络适配器的 Windows 测试机中，从仓库根目录运行。将 `--brw`、`--archive` 替换为实际文件路径，`--run-dir` 指定新目录。驱动在该目录下隔离包源、缓存、命令入口和项目，并保留生成的 devkit、程序及日志。[验收驱动](../tests/devkit-acceptance/run.py)

```powershell
python -B tests/devkit-acceptance/run.py --brw .local/issue123-build/brw.exe --archive .local/issue120-tools/w64devkit-x64-2.11.0.7z.exe --run-dir .local/131-new
```

## Windows 11 x64 实测

本轮在提交 `1f0bdb7ecdf2dccac56104ddfee152f03f6a81dd` 上使用上述驱动，运行目录为 `D:\Projects\burrow\.local\131-final`。预览和应用均以 0 退出，引用位置分别落在工具链和会话子蓝图。使用的清单 SHA-256 为 `358a93fcdeb87b4a2d0a279295194be25e0ef848b9af5951c32be9a69f772593`。[实跑命令和结果摘要](history/devkit-131/summary.json) · [逐次命令、退出码、标准输出及错误输出](history/devkit-131/commands.jsonl)

| 本轮位置 | 路径 |
| --- | --- |
| 应用目录，搬移后原路径消失 | `D:\Projects\burrow\.local\131-final\initial` |
| 搬移后的会话入口 | `D:\Projects\burrow\.local\131-final\relocated devkit\devkit.cmd` |
| 启动会话和执行 CMake 的工作目录 | `D:\Projects\burrow\.local\131-final\caller` |
| CMake 实际选取的工具目录 | `D:\Projects\burrow\.local\131-final\relocated devkit\w64devkit\bin` |

用交互入口核对当前目录及 GCC 的查找结果后，驱动通过同一入口运行 `cmake -S probe -B build -G "MinGW Makefiles"`。CMake 4.4.4 选择搬移目录中的 `cmake.exe`、`mingw32-make.exe`、`gcc.exe` 和 `g++.exe`。两种编译器均为 GCC 16.2.0，目标为 `x86_64-w64-mingw32`。调用者保留着指向旧目录的 GCC 搜索变量时，两个编译器报告的前端 `cc1.exe`、`cc1plus.exe` 均位于搬移后 devkit 的 `libexec` 目录。[工具路径、编译命令与程序摘要](history/devkit-131/summary.json)

| 经会话入口执行的独立构建 | 运行程序 | 标准输出 | 构建 / 运行退出码 |
| --- | --- | --- | --- |
| `cmake --build build --target c_probe --verbose` | `build\c_probe.exe` | `C: 42` | 0 / 0 |
| `cmake --build build --target cxx_probe --verbose` | `build\cxx_probe.exe` | `C++: 42` | 0 / 0 |

本轮应用前和构建后，进程令牌均为 AppContainer，未授予互联网或本地网络能力。IPv4 和 IPv6 TCP 探针均被 Windows 以 `WSAEACCES`（10013）拒绝。[隔离检查](../tests/devkit-acceptance/offline.py) · [实测记录](history/devkit-131/summary.json)

对篡改模块、缺模块、缺能力和模块超限分别执行预览和应用，均以 1 退出。报告给出对应输入、错误码和已完成步骤数 0。带有现有文件的工作区在 `main.mjs[1] > toolchain.mjs[1]` 报 `host-requirement-unmet`，原文件内容保留。这些拒绝用例及正常预览前后，工作区、包状态、缓存和入口的快照一致。[逐项诊断](history/devkit-131/commands.jsonl)
