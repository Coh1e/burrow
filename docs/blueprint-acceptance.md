# Digby 与蓝图首版验收清单

本清单用于验证 [ADR 0010](adr/0010-blueprint-composition.md)。使用同一份根蓝图和同一个构建子蓝图，通过产品入口在 Windows、Linux 和 macOS 上各运行一次完整流程。以下名称仅用于说明样例；实际运行时记录所用蓝图、命令和版本。

## 主场景

| 步骤 | 验收操作与通过条件 |
| --- | --- |
| 准备 | 各平台使用独立的临时项目目录。为工具和它的直接、间接依赖准备各平台的锁定记录，并让测试环境中的默认工具入口指向另一版本。子蓝图旁放置构建脚本，从另一个目录启动 Digby。 |
| 预览 | 核对当前平台选中的工具版本、来源和产物摘要。至少设置一个平台专属步骤，核对其他平台跳过它的原因。两次构建按 Debug、Release 排列，各自显示参数及引用位置。测试输入指向第二次调用的具名输出。运行时才确定的归档路径显示来源；预览前后，项目产物、安装状态和默认入口一致。 |
| 应用 | 两次调用使用同一个子蓝图，各收到自己的构建模式。脚本从子蓝图目录定位，在调用方指定的项目目录执行。让构建工具回报实际版本、工作目录和收到的参数，核对含空格的参数仍为一个参数。两份产物各带可辨认的构建模式，测试读到 Release 标记。默认工具入口仍指向原版本。 |
| 重跑 | 再次应用同一份蓝图。核对已满足的目标状态跳过变更，Debug、Release 和测试动作各再次运行。预览与执行报告中，同一次调用的引用路径和步骤位置一致。 |

## 三个独立故障用例

每个用例从主场景的初始状态单独开始，先预览，再应用，并检查留下的文件和命令入口。

| 注入条件 | 预览与执行位置 | 最终状态 |
| --- | --- | --- |
| 删除当前平台的一条必需锁定记录 | 预览和应用都在规划时指出软件、平台和缺失记录。 | 首次写入前停止。锁定记录未被自动补齐，构建和测试均未启动。 |
| 让首个外部只读检查返回 1，只声明 3 为“未满足” | 预览与应用在同一引用路径报告检查错误，保留退出码。 | 对应变更和后续构建均未执行。用返回 3 的对照运行确认变更会执行，并以复查返回 0 确认目标已满足。 |
| 让 Release 构建写入一个标记后以失败退出 | 预览仍列出两次构建和测试；应用报告第二次调用失败，指出外部命令已运行、可能已留下副作用。 | Debug 产物保留，检查 Release 标记的实际状态，测试未启动。修正后重新应用，从头按序执行，最终测试取得 Release 输出。 |

## 其余约定的边界检查

- [x] 构造循环引用、缺少必填参数、必需平台能力缺失三个变体，分别在首次写入前得到完整引用链、引用位置与参数名、具体缺项。
- [x] 让已选步骤引用被平台条件跳过的调用输出。规划报告两处位置及输出名。再让产出调用正常启动但缺少必需输出，核对失败定位到产出调用，后续步骤停止。
- [x] 让前序命令改写后续检查的文件。预览保留顺序并注明“执行时检查”及原因，应用根据命令运行后的文件内容决定是否变更。
- [x] 发布一个符合范围的新版本，普通应用仍使用锁定记录；显式更新才重新选版。改变蓝图要求使旧记录失效时，首次写入前报告冲突。已有同名依赖仅在匹配锁定记录时复用。
- [x] 将两个版本并存与切换用户默认入口分别预览、应用；只有显式声明切换入口的蓝图才改变默认版本。
- [x] 宿主条件缺失时核对检测值与要求值。显式加入修复步骤后，核对预览、修复和复查顺序，复查通过后才启动依赖步骤。
- [x] 直接启动外部程序时，普通参数中的管道符按字面传入；显式指定解释器时，由解释器处理管道。检查程序无法启动、变更后复查仍未满足时，分别报告失败并停止。

## 结果记录

每个平台保留系统与架构、构建版本、蓝图和锁定记录修订、实际命令、退出码、预览及执行报告。用动作调用记录、产物内容或摘要、默认入口指向核对结果。失败项记录首个偏差及位置，未运行项记录原因。

2026-11-26，三个宿主均在干净的提交 `86c9466f60ceadcc5cf4d6348edbc762022ad51b` 上运行了同一份根蓝图和构建子蓝图。`blueprint.manifest` 的 SHA-256 均为 `6fb5571a9c159a52f18e444e8784c9ff10f011ace54ab60f45e4b4dae05dd9c6`。表内的锁摘要取 SHA-256 的前 12 位，完整值和每条锁定记录见各平台压缩包。[本轮蓝图清单](../tests/blueprint-acceptance/kit/blueprint.manifest)

| 宿主与编译器 | 平台锁摘要 | 预览、应用、重跑 | 独立故障 | 边界用例 | 原始记录 |
| --- | --- | --- | --- | --- | --- |
| Windows 11 build 26300，x64，MinGW-w64 GCC 16.2 | `f34e452b9dc9` | 均通过 | 3/3 | 20/20 | [Windows ZIP](history/blueprint-144/windows.zip) |
| Ubuntu 26.04 LTS，WSL2 x86_64，GCC 15.2 | `94c75bd9c232` | 均通过 | 3/3 | 20/20 | [Linux ZIP](history/blueprint-144/linux.zip) |
| macOS 15.8.2，arm64，Apple Clang 17.0 | `4b1a483345bf` | 均通过 | 3/3 | 20/20 | [macOS ZIP](history/blueprint-144/macos.zip) |

三个平台的主场景预览、应用和重跑均以 0 退出。预览前后的快照一致。首次应用执行一次状态变更，重跑跳过了变更。两次应用都按序运行 Debug、Release 和测试。构建使用锁定的版本 2，`argument with spaces` 在子进程中是一个参数，默认入口始终返回 1。测试两次都读取 `main.mjs[7]` 的具名输出 `archive`，解压 `Release.zip` 取得 `Release` 标记。[主场景断言](../tests/blueprint-acceptance/scenarios.py)

| 本轮故障注入 | 预览 / 应用退出码 | 三个平台分别观测到的停止位置和状态 |
| --- | --- | --- |
| 删除间接依赖 `base` 的当前平台锁定行 | 1 / 1 | `lock-missing` 点名 `base` 及宿主，完成步数为 0。缺行保留，项目与安装状态快照一致。 |
| 第一个检查返回 1，仅把 3 声明为未满足 | 1 / 1 | 两次都在 `main.mjs[1]` 报 `check-failed` 并保留检查退出码 1，动作日志为空。对照预览报告 3，应用变更后复查返回 0。 |
| Release 写标记后失败 | 0 / 1 | `main.mjs[7] > build/job.mjs[1]` 报 `child-failed`，标明命令已启动且可能有副作用。Debug 归档和 `Release.partial` 保留，测试启动次数为 0。移除故障后重新应用，Debug 和 Release 均重跑，测试读取 Release。 |

本轮清单的全部验收项都已执行，未出现预期外的偏差。macOS 使用 GitHub Actions 的 `macos-15` runner，原生核心测试以及区分、不区分大小写的两种 APFS 检查均通过。[macOS 运行日志](https://github.com/Coh1e/burrow/actions/runs/45821984504/job/139916305738)

### 复跑与核对

在仓库根目录，用启用了 `BRW_BLUEPRINT=ON` 和 `BUILD_TESTING=ON` 的本机构建运行下列命令。`--build-dir` 指向已经构建全部目标的目录，`--evidence` 指向一个新目录。脚本为主场景和每个故障分别准备工具源、平台锁、默认入口和项目，从独立的启动目录调用 `brw`，最后执行 CTest 的 `blueprint_cli` 边界检查。[验收入口](../tests/blueprint-acceptance/run.py)

本轮的三个入口命令均以 0 退出。复跑时请指定新的证据目录。

```text
# Windows，PowerShell
.local/tools/python/cpython-3.13.7-windows-x86_64-none/python.exe tests/blueprint-acceptance/run.py --build-dir .local/issue123-build --evidence .local/144-evidence-86c9466/windows

# Linux，Ubuntu
python3 tests/blueprint-acceptance/run.py --build-dir .local/issue142-ubuntu --evidence .local/144-evidence-86c9466/linux

# macOS，GitHub Actions
python3 tests/blueprint-acceptance/run.py --build-dir .local/brw-blueprint --evidence .local/blueprint-evidence
```

每份平台记录中的 `summary.json` 给出系统、编译器、Git 修订、可执行文件 SHA-256 和各场景结论。按以下位置可以复核单条断言。[场景及断言](../tests/blueprint-acceptance/scenarios.py)

| 核对内容 | 平台记录内的位置 |
| --- | --- |
| 实际 CLI、启动目录、退出码、完整标准输出及错误输出 | 各场景的 `commands.jsonl`，按 `label` 查找 `preview`、`apply`、`rerun` 或故障对照 |
| 蓝图、当前平台锁及锁定的依赖链 | `main/manifest.json`、`main/initial-lock.json` 和 `missing_lock/incomplete-lock.json` |
| 预览无写入、默认入口及最终文件状态 | `main/preview-filesystem.json` 的前后快照、`main/*-entry.json`、各场景的 `final-state.json` |
| 实际工具版本、工作目录、脚本摘要、参数、动作次数及归档标记 | `main/apply-observations.json`、`main/rerun-observations.json` |
| 故障停止位置、留下的文件及恢复结果 | 故障场景的 `commands.jsonl` 和 `after-failure.json`，以及 `check_error/after-control.json`、`release_failure/after-repair.json` |
| 逐项边界结果及对应命令 | `boundary/passed.txt` 和 `boundary/boundary-cli.txt`，用同一个用例名匹配 |
