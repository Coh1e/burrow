# 审查标准

## 行为测试

每条新测试都应能指出它验证的需求，以及哪一项行为回退会让它失败。写测试时先把行为改坏一次，看着它红，再改回来。测试部分失败时，核对命令返回值、已提交的步骤与仍留在磁盘上的状态。新增的 `tests/*.ps1` 在文件头用 `# Red when: <改动>` 写明失败条件。

## 构建与宿主

- 公共 C 头须能由独立的 C 调用方编译。把专用依赖的头文件留在使用它们的实现或目标中。
- Windows 构建与测试脚本须在 Windows PowerShell 5.1 中运行。原生命令的退出码决定成败，诊断可写到 stderr。含非 ASCII 字符的 PowerShell 脚本使用 UTF-8 BOM。
- 文件内使用一致的行尾。提交前检查源码编码和 Markdown 相对链接。[源码检查](scripts/check-source-hygiene.ps1) · [链接检查](scripts/check-doc-links.ps1)

pre-commit 钩子可对暂存文件执行相同检查。推送与 PR 的 CI 在仓库检查之外构建并测试原生目标。[安装钩子](scripts/install-hooks.ps1) · [CI 工作流](.github/workflows/ci.yml)

## 验收记录

引用耗时、体积与测试数量时，记录产生数值的命令、脚本和修订，区分本轮与历史结果。记录宿主和工具链，性能测量还须标明工作负载与缓存条件。候选构建的记录、产物摘要及未通过的门禁见[原生发布验收](docs/native-release-acceptance.md)。

审查相对链接时，核对目标段落是否支持所写的结论。
