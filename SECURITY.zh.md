# 安全说明

## 本仓库构建什么

`uia_agent.exe` 是纯命令行工具：从命令行读取参数，对 Windows 桌面 UI 执行操作（枚举/快照/查找/点击/设值/滚动/拖动/滑动/截屏/窗口状态），向 stdout 输出一行 UTF-8 JSON。它**无网络访问、无持久化、无自更新**，以调用它的进程的权限运行。

## 供应链完整性

- 发布流水线（`.github/workflows/release.yml`）在 GitHub 托管的 Windows runner 上从已提交源码构建 exe，然后：
  1. `scripts/pack.ps1` 将 exe 编码为 `assets/uia_agent.exe.b64`，并把 SHA-256 写入 `assets/uia_agent.exe.sha256`
  2. `scripts/verify.ps1` 把 b64 还原并断言哈希一致
  3. 两个资产文件提交回 `master`
  4. exe 挂到对应的 GitHub release
- 任何人都可独立审计：下载 release 的 exe，计算 `Get-FileHash -Algorithm SHA256`，与仓库同 tag 下的 `assets/uia_agent.exe.sha256` 对比。

## 能力与风险

| 能力 | 风险 |
|---|---|
| list / snapshot / find / get_text / screenshot | 只读：枚举 UI 结构与像素 |
| minimize / maximize / restore / topmost / close | 修改目标窗口状态 |
| click / set_text / scroll / drag / swipe | **模拟输入**：向桌面会话注入鼠标/键盘事件 |

高风险操作的闸门由消费方插件（见 `dsh-uia-agent` 的 SECURITY）通过用户审批控制——本仓库的 exe **自身不强制权限**，权限由调用方负责。

## 责任划分

- **本仓库** 负责：自动化操作的正确与安全实现、可复现构建、提交 sha256 资产。
- **消费方插件**（`cnyc6n/dsh-uia-agent`）负责：权限分档、将高风险调用路由到用户审批。
- **用户** 决定是否批准提权（workspace-write / danger-full-access）。批准 `click`/`set_text`/`drag` 意味着接受将真实输入注入桌面会话。

## 边界 / 限制

- Windows UIPI 会拦截非提升进程访问 elevated（管理员）窗口；本工具检测并报告 `elevated_requires_admin`，而不是返回误导性的空树。
- 以管理员运行 exe 会获得与 elevated 窗口交互的能力；仅在信任调用方时这样做。

## 报告

安全相关问题请通过 private advisory 或标记 `security` 的 GitHub issue 联系维护者；修复发布前请勿公开披露利用细节。
