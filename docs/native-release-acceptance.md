# brw 原生版候选发布验收

发布维护者以同一个 Git 提交核对下表。Windows 交付件是启用 QuickJS 的 `brw.exe`，安装占用包括许可和运行所需文件；下载 ZIP 和用户安装软件后生成的 shim 单列。[发布工单](https://github.com/Coh1e/burrow/issues/134) · [父规格的测试决策](https://github.com/Coh1e/burrow/issues/119) · [发布验收决议](https://github.com/Coh1e/burrow/issues/118#issuecomment-7101372684)

| 门禁 | 要核对的证据 |
| --- | --- |
| Windows 交付 | Windows 11 x64 的 Ninja 与 MinGW Makefiles 断网清洁构建、预置材料摘要、无开发工具的干净 Windows 启动、PE 直接与延迟导入、运行时 DLL、交付件及许可的字节数 |
| 原生包与计划 | 文本和 JSON 预览、单包与依赖链的实际操作跟踪、失败停止位置、预览无副作用、输入改变后重新规划；生命周期、回退、依赖与恢复 |
| Windows 入口与目录 | cmd、PowerShell、资源管理器的入口行为，参数、工作目录、标准流、GUI、XDG 和跨卷恢复 |
| 环境与注册表 | 普通用户的专用测试键、会话范围、用户环境、未授权值拒绝与恢复时保留用户改值 |
| Blueprint 与 devkit | 离线应用、篡改和缺项拒绝、搬移后通过 CMake 构建并运行 C 和 C++ |
| 旧 Linux | GCC 4.8.4、CMake 3.10.3、libc 2.19，无 g++ 且无外网；最小版操作、静态 ELF 及交付字节数 |
| 真实 SSH | Windows 核对主机身份后传入同一份 Blueprint、源码和固定依赖；远端在隔离网络中构建、应用并运行 Linux 产物；断线报告未知 |
| 共用核心与 C ABI | Windows、Linux、macOS 跑相同核心测试，核对纯 C 与可选 C++23 两种配置 |

## 历史证据

#125 的依赖顺序与回退保护、#127 的真实用户注册表、#131 的离线 devkit、#132 的真实 SSH、#133 的三宿主核心均已通过各自提交的验收。这些结果可复用其夹具和方法。本轮按候选修订核对独立记录。[依赖安装](https://github.com/Coh1e/burrow/issues/125) · [普通用户注册表](https://github.com/Coh1e/burrow/issues/127#issuecomment-7205568029) · [devkit 记录](windows-devkit.md) · [真实 SSH 记录](ssh-build-acceptance.md) · [三宿主 CTest](../brw/README.md#verification-on-native-hosts) · [旧 Linux 基线](../brw/minimal.md)

## 候选宿主复验

在候选源码根目录运行。先用 `git rev-parse HEAD` 核对交接单的完整提交，将随包四份固定归档置于 `.local/deps/sources/`。每项使用新输出目录。记录本次使用的操作系统、架构和工具版本。

- **macOS**。预置 Apple Clang、CMake、Ninja、Git 和 Python 3 后，运行 `sh tests/release-acceptance/macos.sh "$candidate" .local/mac-134`。返回 `.local/mac-134/macos.log` 与其 `blueprint/` 目录。须通过纯 C、可选 C++23 的 C ABI、共享 Blueprint、两种 APFS 大小写模式。[macOS 验收入口](../tests/release-acceptance/macos.sh)
- **真实 SSH**。在操作者授权的专用 WSL 仅开启回环监听，通过本地 WSL 控制台取得主机公钥，在 Windows 运行现有入口。使用 #144 的固定清单摘要和新的证据目录。返回 `transport.json`、`remote/`、主机信任来源及撤销凭据、关闭端口的记录。须核对传输两端摘要、独立网络命名空间、Linux ELF 的输出和断线后的一次启动。[Windows 命令、主机准备与通过条件](ssh-build-acceptance.md#在-windows-复跑)

## 查询基线

在 Windows 使用最终交付的 `brw.exe` 和对应的 `CMakeCache.txt`。驱动在独立用户目录通过 CLI 登记包源、安装夹具；每次查询启动独立进程，计时包含进程启动和 JSON 输出捕获。每个工作负载分别记录首次和后续样本，保留全部时间，再计算最近秩 p50、p95。夹具刚写入，系统缓存沿用正常状态。[测量驱动](../tests/release-acceptance/query_baseline.py)

将 `$python` 设为可运行的 Python 3.11+ 解释器。下例从存储管理接口取得夹具所在盘的型号和总线；受限进程也可以直接使用同一宿主已核实的磁盘记录。

```powershell
$disk = Get-Partition -DriveLetter (Get-Location).Drive.Name | Get-Disk
& $python -B tests/release-acceptance/query_baseline.py --brw $releaseExe --cache $cmakeCache --build-record $queryBuildJson --storage "$($disk.FriendlyName) ($($disk.BusType))" --evidence .local/query-baseline.json
```
