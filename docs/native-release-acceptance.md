# brw 原生版候选发布验收

2026-11-27，候选固定为 `5e34c7819fadeea2a0eb373231e50941161b910c`，基于合入 #158 的 `main`（`79520b2355d4`）。三项体积门槛通过。发布仍待干净 Windows、普通用户 HKCU、真实 macOS 和专用 WSL 的 SSH 记录。下表给出本次结果及证据 ZIP 内的位置。[本次证据 ZIP](history/release-134/evidence.zip) · [发布工单](https://github.com/Coh1e/burrow/issues/134) · [父规格](https://github.com/Coh1e/burrow/issues/119) · [发布验收决议](https://github.com/Coh1e/burrow/issues/118#issuecomment-7101372684)

| 门禁 | 该候选的实测结果 | ZIP 内的记录 |
| --- | --- | --- |
| Windows 构建与交付 | Windows 11 x64 开发宿主上，从 Git 归档和已核验的固定材料，在新目录用 Ninja、MinGW Makefiles 各构建并启动一次。AppContainer 无网络能力，构建前后 IPv4/IPv6 均返回 10013。干净镜像交付仍待验。 | `windows-delivery/windows-build.json`、`network.json`、两套构建日志、`developer-smoke.json` |
| 原生包与 Digby 计划 | 单包与依赖链的文本、JSON 预览符合实际后端顺序。摘要失败停止于 `broken verify`，已提交的 `extra` 保留。变更目录内容后按新依赖安装。两种预览均未改变夹具，HTTP 监听未收到请求。六字段和七字段目录、空依赖、来源限定名及无效字段均覆盖。 | `windows-behavior/ctest.txt` 的 `brw_source`、`brw_dependencies` |
| 生命周期、命令入口与 XDG | 实跑安装、更新、回退、卸载、失败保护、发布中断恢复。覆盖 cmd、PowerShell、资源管理器使用的 Shell Open 动词、GUI 控制台、参数、工作目录、标准流和退出码。覆盖同卷 D: 及跨卷 D:/C: 恢复。 | `windows-behavior/ctest.txt` 的 `brw_local_install`、`brw_recovery_*`、路径和初始化用例 |
| 环境与注册表 | 会话与隔离测试 hive 通过。真实 HKCU 待普通账户复验。外层完整性 RID 8192，原生子进程为 4096。新专用键上 `KEY_CREATE_SUB_KEY` 返回错误 5，预览在步骤 1 报 `registry-write`。状态目录未创建，专用键已清理。 | `windows-behavior/registry-native-diagnosis.json`、`token-comparison.json`、`hkcu-direct.txt` |
| Windows 离线 devkit | 用交付字节应用组合 Blueprint，devkit 搬移到带空格目录后通过 CMake 编译，程序分别输出 `C: 42`、`C++: 42`。篡改、缺模块、缺能力和超限均被拒绝。前后网络探针通过。 | `windows-devkit/summary.json`、`commands.jsonl` |
| 旧 Linux 最小版 | WSL2 上的 Ubuntu 14.04 x86-64 用户空间，GCC 4.8.4、CMake 3.10.3、glibc 2.19。独立网络命名空间仅有回环，无 g++。三个 CTest 均通过，含搜索、替换、tar 及越界拒绝。readelf 未见解释器或动态段。 | `linux/legacy.log`、`summary.json`、`reproduce.txt` |
| 真实 SSH | 当前候选待 Windows 经回环 SSH 传入材料、远端断网构建、应用及运行 ELF，并收回断线证据。 | 人工返回 `transport.json`、`remote/` 与主机信任、清理记录 |
| 共享核心及 C ABI | Windows 纯 C 核心 5/5，可选 C++23 配置中 16 项通过，另 1 项真实 HKCU 见上。现代 WSL 的纯 C 用例覆盖 ext4、ramfs、实际 9p，C++23 Release 和 Debug ASan/UBSan 各 3/3。两宿主的同一 Blueprint 主场景、3 个故障场景和 20 个边界均通过。macOS 待验。 | `windows-behavior/core-configurations.json`、两宿主的 `blueprint/`、`linux/modern.log` |

## 交付文件

Windows 使用 w64devkit 2.11.0、GCC 16.2.0、CMake 4.4.4。选用 Ninja 产物交付并测量查询。两种生成器构建的 EXE 均为 4,197,597 字节，各自的 SHA-256 与构建缓存摘要见 `windows-delivery/windows-build.json`。下表使用最终文件的字节数，空安装为 EXE 与七个许可文件之和。[交付测量和检查](../tests/release-acceptance/windows_delivery.py)

| 交付项目 | 实测字节 | 门槛 |
| --- | ---: | --- |
| Windows QuickJS `brw.exe` | 4,197,597 | ≤ 8 MiB |
| Windows 空安装（含许可） | 4,301,086 | ≤ 9 MiB |
| Linux x86-64 静态最小版 | 1,189,296 | ≤ 2 MiB |
| Windows 下载 ZIP | 1,791,451 | 单列 |
| 安装后生成的一个控制台 shim | 110,779 | 单列 |

交付 ZIP 的 SHA-256 为 `d5ddcd95ae06c7609d35f13f96aa3edb9d883015eaaa2f1968c2316325724c77`，解压后 EXE 为 `49328dce03efe66a953462536c1698debab21caefd7fc55370bf2456d7673ba3`。EXE 有 13 个直接导入，两个 shim 各 2 个，三个文件的延迟导入均为空。GDB 跟踪空列表、安装、QuickJS 应用和命令入口，共观察到 25 个允许的 System32 DLL。在现有开发机上另用 Windows PowerShell 5.1 解包、启动并采样 22 个模块，记录标为 `cleanHostVerified=false`。[系统 DLL 清单](../tests/release-acceptance/windows-system-dlls.txt) · [干净 Windows 验收入口](../tests/release-acceptance/clean-windows.ps1)

## 历史证据

#125 的依赖顺序与回退保护、#127 的真实用户注册表、#131 的离线 devkit、#132 的真实 SSH、#133 的三宿主核心均已通过各自提交的验收。这些结果可复用其夹具和方法。本轮按候选修订核对独立记录。[依赖安装](https://github.com/Coh1e/burrow/issues/125) · [普通用户注册表](https://github.com/Coh1e/burrow/issues/127#issuecomment-7205568029) · [devkit 记录](windows-devkit.md) · [真实 SSH 记录](ssh-build-acceptance.md) · [三宿主 CTest](../brw/README.md#verification-on-native-hosts) · [旧 Linux 基线](../brw/minimal.md)

另对根目录的 Scoop 兼容版 `burrow.exe` 运行完整 CTest，首轮 75/93 通过。18 项失败中，换用独立夹具和已预置的工具后有 3 项通过，9 项仍失败，6 项涉及真实用户环境或入口，未扩大权限重跑。剩余失败包含旧实现对目录联接目标格式的处理和子进程启动被拒绝。逐项归因在 `legacy-windows/failure-review.json`。父规格将旧 CLI 和 Scoop 兼容性列在原生首版范围之外。相关旧实现与这 18 项脚本和先前通过 93/93 的 CI 修订一致。[父规格](https://github.com/Coh1e/burrow/issues/119) · [先前兼容版 CI](https://github.com/Coh1e/burrow/actions/runs/45947543745/job/140335595997)

## 候选宿主复验

本地交接包 `134-handoff-5e34c7819fad.zip` 含可离线克隆的 `candidate.bundle`、四份源码依赖、固定 SDK、Windows 交付 ZIP、普通用户注册表测试程序和逐项命令。包内 `README.md` 列明每项应返回的文件，`checksums.json` 列出材料摘要。两份文件也存于证据 ZIP 的 `handoff/`。使用新目录，核对完整提交和摘要后运行。

交接包共 89,704,821 字节，SHA-256 为 `c7af554f0d946fd02cd61ca2abd5bc07b2cb30e4a551e0c052e858b73595a02f`。已验证可从包内 Git bundle 离线检出上述完整提交。

- **干净 Windows 11 x64**。在新镜像上预置材料并断开网卡，用 `windows_build.py` 生成双构建记录。将该次交付 ZIP 和已核实的 ZIP/EXE 摘要交给无开发工具的干净 Windows。用系统 PowerShell 5.1 运行 `clean-windows.ps1 -PristineImage`，返回构建日志、镜像来源和 `clean-windows.json`。须看到空列表、QuickJS 应用成功、模块采样通过及 `cleanHostVerified=true`。[构建驱动](../tests/release-acceptance/windows_build.py) · [启动和 DLL 验收入口](../tests/release-acceptance/clean-windows.ps1)
- **Windows 普通用户**。在子进程保持中等完整性的普通账户会话，用包内测试程序运行 `brw-settings.ps1 -UserRegistry`。用例只操作随机测试子树，并在结束时清理。返回退出码、`hkcu.log` 和清理结果。须核对真实 REG_SZ、REG_DWORD、用户环境与 PATH、拒绝未授权的写入及恢复时保留用户改值。[真实 HKCU 用例](../tests/brw-settings.ps1)

- **macOS**。预置 Apple Clang、CMake、Ninja、Git 和 Python 3 后，运行 `sh tests/release-acceptance/macos.sh "$candidate" .local/mac-134`。返回 `.local/mac-134/macos.log` 与其 `blueprint/` 目录。须通过纯 C、可选 C++23 的 C ABI、共享 Blueprint、两种 APFS 大小写模式。[macOS 验收入口](../tests/release-acceptance/macos.sh)
- **真实 SSH**。在操作者授权的专用 WSL 仅开启回环监听，通过本地 WSL 控制台取得主机公钥，在 Windows 运行现有入口。使用 #144 的固定清单摘要和新的证据目录。返回 `transport.json`、`remote/`、主机信任来源及撤销凭据、关闭端口的记录。须核对传输两端摘要、独立网络命名空间、Linux ELF 的输出和断线后的一次启动。[Windows 命令、主机准备与通过条件](ssh-build-acceptance.md#在-windows-复跑)

## 查询基线

在 Windows 使用最终交付的 `brw.exe` 和对应的 `CMakeCache.txt`。驱动在独立用户目录通过 CLI 登记包源、安装夹具；每次查询启动独立进程，计时包含进程启动和 JSON 输出捕获。每个工作负载分别记录首次和后续样本，保留全部时间，再计算最近秩 p50、p95。夹具刚写入，系统缓存沿用正常状态。[测量驱动](../tests/release-acceptance/query_baseline.py)

本次在 Windows 11 x64、Ryzen 7 8845HS、NTFS、KIOXIA-EXCERIA G2 NVMe SSD 上完成 15 个工作负载，共 315 个独立查询进程。每项单列首次样本，再测 20 次；`queries.json` 保存每次耗时、完整参数、构建选项、EXE 和缓存摘要。下表节选最大夹具，单位为毫秒，本轮仅建立基线。[完整数据](history/release-134/evidence.zip)

| 命令与夹具 | 首次 | 重复 p50 | 重复 p95 |
| --- | ---: | ---: | ---: |
| search 命中，2,048 行 | 21.63 | 18.06 | 19.89 |
| search 未命中，2,048 行 | 24.51 | 18.07 | 21.11 |
| show 命中，2,048 行 | 26.15 | 17.09 | 22.64 |
| show 未命中，2,048 行 | 15.66 | 16.09 | 18.84 |
| list，已安装 64 包 | 59.54 | 59.15 | 77.03 |

将 `$python` 设为可运行的 Python 3.11+ 解释器。下例从存储管理接口取得夹具所在盘的型号和总线；受限进程也可以直接使用同一宿主已核实的磁盘记录。

```powershell
$disk = Get-Partition -DriveLetter (Get-Location).Drive.Name | Get-Disk
& $python -B tests/release-acceptance/query_baseline.py --brw $releaseExe --cache $cmakeCache --build-record $queryBuildJson --storage "$($disk.FriendlyName) ($($disk.BusType))" --evidence .local/query-baseline.json
```
