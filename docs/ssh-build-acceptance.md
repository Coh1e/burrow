# Windows 经 SSH 在专用 WSL 中离线构建和应用 Blueprint

2026-11-27，Windows 使用系统 OpenSSH，将修订 `adb79f94c5a7e9b7311f2f00b7eb3a3bcb508c3c` 的源码、四份固定依赖和跨平台 Blueprint 传入专用 Ubuntu 24.04.5 LTS WSL2。远端从空目录构建启用 QuickJS 的 `brw`，预览、应用、重跑同一份 Blueprint，并执行了 Release 归档内的 ELF。另一次命令在远端确认启动后断线，客户端报告结果未知，远端仅记录一次启动。[#132 验收条件](https://github.com/Coh1e/burrow/issues/132) · [本轮原始记录 ZIP](history/ssh-132/evidence.zip)

## 已有证据

#117 的原型在 WSL 内用预置材料完成了离线演练。#144 在三个宿主上运行了相同的 Blueprint，核对了 Debug、Release 和下游测试的参数与输出。原样例的 `Release.zip` 只含模式标记。本轮复用其清单、子蓝图和断言，在 Linux 的工具锁中选入能编译 C 程序的 builder，沿用同一个具名输出交给消费者。[#117 原型记录](https://github.com/Coh1e/burrow/issues/117#issuecomment-7100484116) · [#144 三宿主记录](blueprint-acceptance.md) · [Linux 构建工具](../tests/ssh-acceptance/linux_builder.py)

## 实跑结果

| 验收项 | 观察结果 | ZIP 内记录 |
| --- | --- | --- |
| 身份与传输 | 在首次 SSH 登录前，经 Windows 本地 `wsl.exe` 读取专用实例的 ED25519 公钥。空信任文件触发主机校验失败。Windows OpenSSH 9.5p2 使用固定公钥经回环端口发送 16,117,760 字节。两端的 SHA-256 一致。 | `host-setup.json`、`transport.json` |
| 离线与构建 | SSH 会话与构建进程的网络命名空间不同。构建进程只有 `lo`，IPv4、IPv6 的全部路由表均为空。远端校验传输包、清单和每个依赖，在新目录用 GCC 13.3、CMake 3.28.3 和 GNU Make 4.3 构建 Release `brw`，退出码为 0。 | `remote/network.txt`、`remote/toolchain.txt`、`remote/*.sha256`、`remote/build.log` |
| 应用与产物 | 远端新构建的 `brw` 预览、应用并重跑。消费者选中第二次构建的具名输出 `Release.zip`。从中提取的 `bin/probe` 带有 ELF 文件头，执行后输出一行 `brw Linux Release`，退出码为 0。 | `remote/linux/commands.jsonl`、`remote/linux/*-observations.json`、`remote/linux/linux-product.json` |
| 断线与退出码 | 远端将启动标记写入文件并回执后，Windows 终止该 SSH 客户端。结果为 `unknown`，远端退出码为 `null`。另开只读连接查到启动次数为 1。对照命令正常传回退出码 255，结果为 `completed`。 | `transport.json` 的 `disconnect`、`knownFailure` |

| 材料或产物 | 本轮 SHA-256 |
| --- | --- |
| Windows 发送、WSL 接收的 tar | `19015b535301784aa0e89728822bf2b983c060d4e39a7a9816d916cb701e9871` |
| #144 原始 Blueprint 清单 | `6fb5571a9c159a52f18e444e8784c9ff10f011ace54ab60f45e4b4dae05dd9c6` |
| 远端构建并用于应用的 `brw` | `6d56e9ea28fdf3c1adc5a1768220eb97cd3fb08b6356701f0ebec9ae0dab660d` |
| 本轮 Linux 平台锁 | `0c38865b79be58cae9656b334ced5ce405a03972604e5e54d1b95dd14a5c15e5` |
| 消费者收到的 `Release.zip` | `de52155299e484924af0279a8ef296a9bd221de59dbac894f7c1569d3167e0d5` |
| 从归档取出并执行的 `bin/probe` | `48671546c0a95e3a63e8b242cea0d54ab449ffb5a5657346c65c65110651f25a` |

## 在 Windows 复跑

1. 从仓库根目录运行。准备 Git、Python 3.12 或更新版、Windows OpenSSH 客户端，以及 `.local/deps/sources/` 下的四份原始归档。驱动从当前提交用 `git archive` 取源码，并在 Windows 核验依赖摘要。[固定依赖的文件名、下载地址及 SHA-256](../scripts/blueprint-sources.txt)
2. 准备经操作者授权的专用 WSL2，只在 `127.0.0.1` 开启临时 SSH。提前安装 OpenSSH 服务端、GCC 和 libc 开发文件、CMake 3.25+、GNU Make、Git、Python 3、GNU tar、gzip、xz、coreutils、iproute2 和 util-linux。测试账号需要能运行 `unshare -Urn`。本轮实例关闭了 Windows 驱动器自动挂载和互操作。
3. 经本地 WSL 控制台读取主机 ED25519 公钥并核对指纹。准备含本轮唯一端点的 `known_hosts`，非标准端口的条目格式为 `[127.0.0.1]:端口 ssh-ed25519 公钥`。在 PowerShell 中将下列变量设为该端口、测试账号、临时客户端身份文件和信任文件，指定新的证据目录后执行。[Windows 验收入口](../tests/ssh-acceptance/run.py) · [远端构建步骤](../tests/ssh-acceptance/build.sh)

```powershell
python -B tests/ssh-acceptance/run.py `
  --port $wslPort --user $wslUser --identity $testIdentity --known-hosts $trustedHosts `
  --manifest-sha256 6fb5571a9c159a52f18e444e8784c9ff10f011ace54ab60f45e4b4dae05dd9c6 `
  --evidence .local/132-ssh-rerun
```

驱动通过 SSH 取回证据。复跑结束后应关闭临时监听、撤销凭据并停止专用 WSL。本轮已验证服务停止、临时密钥已清理，Windows 回环端口拒绝连接。专用发行版保留登记，处于停止状态。详见 ZIP 内的 `host-cleanup.json`。
