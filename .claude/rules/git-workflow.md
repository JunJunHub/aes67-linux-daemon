# Git 与 Worktree 工作流规则

本文件定义 aes67-linux-daemon 的提交规范和 worktree 工作流程。

## 提交信息

优先使用中文提交信息，格式：

```text
<type>(<scope>): <subject>

<body>

<footer>
```

## Type 类型

| 类型 | 描述 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档更新 |
| `style` | 代码格式，不影响功能 |
| `refactor` | 重构，不是新功能也不是修复 |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `chore` | 构建或工具相关 |
| `revert` | 回滚提交 |

## Scope 范围

Scope 表示逻辑领域，不等同于物理目录名。按以下规则确定 scope，使新增组件自动获得 scope，避免手工表随代码扩展而过期。

### 默认规则

改动落在 `daemon/` 下某模块（单个 `.cpp`/`.hpp` 或一个模块子目录）时，scope 取模块名的小写形式。新增模块无需改本表，自动适用：

| 模块 | Scope |
|------|-------|
| `daemon/session_manager.*` | `sessionmanager` |
| `daemon/http_server.*` | `http` |
| `daemon/streamer.*` | `streamer` |
| `daemon/browser.*` | `browser` |
| `daemon/sap.*` / `mdns_*.*` | `discovery` |
| `daemon/rtsp_*.*` | `rtsp` |
| `daemon/config.*` | `config` |
| `daemon/driver_*.*` | `driver` |
| `daemon/log.*` | `log` |

### 显式覆盖

下列情况优先于默认规则--多个模块合并为同一 scope，或模块名与期望 scope 不一致：

| Scope | 覆盖模块 / 说明 |
|-------|----------------|
| `oca` | `daemon/oca/` 整目录 + `daemon/oca_session_manager_bridge.*` 合并（AES70/OCA 控制协议） |
| `noise` | `daemon/noise/` 整目录（噪声分析与降噪模块） |
| `netlink` | `daemon/netlink.*`、`netlink_client.*` 合并 |

### 跨切面 scope

改动横跨多模块或不属于具体模块时使用固定词汇：

| Scope | 适用范围 |
|-------|---------|
| `webui` | `webui/` 前端（React + Vite） |
| `build` | CMake、构建系统、依赖配置、`build.sh`/`buildfake.sh` |
| `test` | `daemon/tests/`、`test/`、`wavplay_am824/` 测试代码与设施 |
| `docs` | 文档、规则、spec/plan |
| `tools` | `systemd/`、脚本与工具 |
| `dev` | 开发辅助脚本（如 `oca-dev.sh`、`oca-daemonctl.sh`） |
| `release` | 版本发布流程 |
| `chore` | 不属于上述任何类的杂项 |

### 维护要求

- 新增 `daemon/` 下模块默认自动获得 scope，无需改本表；仅当新设分组、新增跨切面 scope 或模块名与领域词不一致时才更新。
- Fork 维护规则见 `.claude/rules/fork-maintenance.md`，涉及上游同步的提交 scope 用 `upstream`。

## Worktree 固定目录

较大的功能开发、重构、实验性改动或需要隔离上下文的任务，优先使用 Git worktree。应在一个 worktree 中完成此次 spec 定义的所有开发任务，即使任务比较复杂、被拆解成多个 phase 阶段执行。全部功能验收后，再按流程合并回 master。

本项目 worktree 固定放在项目根目录的 `.claude/worktrees/` 下：

```bash
git worktree add .claude/worktrees/<目录名> -b <分支> master
```

规则：

- 不使用 `.worktree/`、`.worktrees/` 或其它临时目录。
- `.claude/worktrees/` 已写入 `.gitignore`，不得提交其中内容。
- 创建 worktree 前必须确认主工作区状态（见下方「防遗漏规则」）。
- **含 submodule 的 worktree 无法用 `git worktree move/remove` 迁移或删除**（git 已知限制）。迁移方式：确认工作区干净后手动 `rm -rf` 旧目录 → `git worktree prune` → 在新路径 `git worktree add` 重建 → `git submodule update --init --recursive`。

## 复杂度分流

任务开始前，根据工作复杂度评估执行方式，避免对小改动也开 worktree，或对大重构直接改主分支：

| 复杂度 | 判定 | 执行方式 |
|--------|------|---------|
| 低 | 单文件小改、文档或规则整理、单点 bug 修复、不动公共 API | 主工作区直接修改并提交 |
| 中 | 改动 2~5 文件、动单一模块内部、不需隔离上下文且**不与其它任务并行** | 主工作区创建分支执行 |
| 高 | 改动 >5 文件、跨模块或动公共 API、需隔离上下文、可能并行多任务 | 创建 worktree（`.claude/worktrees/`）执行 |

判据以客观信号优先：**改动文件数、是否动公共头文件 / CMake target、是否与其它在途任务并行**。任一触发"并行"或"动公共 API"即升至 worktree，不论文件数。

典型场景：noise 功能开发与 aes70-oca 开发**并行**进行时，两者各自独立 worktree，互不干扰。

## 标准流程

spec 与 plan 都是某次实现的设计依据与指南，与实现同分支推进，合并回主分支时一起进入 master。无需把 spec 单独提前提交到主分支。

中高复杂度任务的标准流程：

1. 评估复杂度并确定执行方式：主工作区分支或 worktree。
2. 编写 spec：讨论需求，编写 `docs/` 下对应模块的设计文档，先保持未提交状态供审核。
3. 提交 spec：spec 审核通过后，作为分支的第一个提交落盘，独立成笔，不与 plan 或实现代码混在同一提交。
4. 编写 plan：在 `docs/` 下编写实现计划，先保持未提交状态供审核。
5. 提交 plan：plan 审核通过后，独立成笔落盘，先于任何实现提交。
6. 实现：代码、测试、修复和阶段性提交都在该分支完成。
7. 同步主分支变化：如 master 有新提交，在该分支合并或变基 master。
8. 验证：至少运行无硬件构建（`./buildfake.sh`）和相关测试；真实硬件验证需加载 LKM。
9. 合并回主工作区：用户确认后，将该分支合并回 master，spec、plan 与实现一起进入。
10. 主工作区最终检查：合并后再次检查 `git status --short`。

低复杂度任务可直接在主工作区修改并提交，无需 spec/plan，但仍遵守提交信息规范。

## 防遗漏规则

- spec 与 plan 均须独立成笔，先于实现提交，禁止与实现代码混在同一提交，便于评审追溯"设计依据"与"实现依据"。
- spec 与 plan 在同一分支（普通分支或 worktree 分支）提交，合并回主分支时一起进入 master，无需提前单独提交到主分支。
- 创建 worktree 前必须确认主工作区状态，运行 `git status --short`，未提交文件需逐项处置；AI 助手须主动检查并报告未提交文件。
- 未提交文件只存在于当前物理目录，不会自动进入其它 worktree，也不会被 merge/rebase 带回。
- 如果合并回主分支后发现遗漏的 spec 或 plan，应单独补交 `docs` commit，并复盘分支创建前的状态检查。
- 例外：多个 worktree 需要共享同一 plan 或 spec 基线时，可提前提交到 master，但需在文档头部声明该例外原因。
