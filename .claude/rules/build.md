# 构建规则

本文件定义 aes67-linux-daemon 的 CMake 构建规范与 clangd 编译数据库约定。

## 构建方式

本项目默认 **in-source 构建**：CMake 配置与编译在 `daemon/` 目录内进行（生成的 `Makefile`/`CMakeFiles/` 已被 `.gitignore` 忽略）。`build.sh`/`buildfake.sh` 已封装此流程。

也可 out-of-source 构建（如需隔离构建产物），`.gitignore` 允许 `build/`、`build-*/` 目录。

### 标准构建

```bash
# 完整构建（含 LKM、WebUI，需 linux-headers）
./build.sh

# 无硬件构建（CI 路径，FAKE_DRIVER=ON）
./buildfake.sh

# 手动 in-source 构建（daemon/ 内）
cd daemon
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DFAKE_DRIVER=ON -DWITH_OCA=ON .
make -j$(nproc)
```

### out-of-source 构建（推荐用于 worktree）

```bash
cd daemon
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DFAKE_DRIVER=ON -DWITH_OCA=ON .
cmake --build build -j$(nproc)
```

## CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ENABLE_TESTS` | OFF | 构建 Boost.Test 套件（target `daemon-test`） |
| `WITH_AVAHI` | OFF | mDNS 支持（需 Avahi 开发库） |
| `FAKE_DRIVER` | OFF | 用 fake driver 替代 RAVENNA（无硬件验证用） |
| `WITH_SYSTEMD` | OFF | systemd notify + watchdog 支持 |
| `WITH_STREAMER` | ON | HTTP Streamer 支持 |
| `WITH_OCA` | OFF | AES70/OCA 控制协议（本 fork 特性，`feature/aes70-oca` 分支） |
| `WITH_NOISE` | OFF | 噪声分析与降噪模块（本 fork 特性，`feature/noise` 分支；代码隔离在 `daemon/noise/`） |

无硬件验证统一用 `FAKE_DRIVER=ON -DWITH_AVAHI=OFF -DWITH_STREAMER=OFF`（即 `buildfake.sh` 默认）。

## compile_commands.json

运行 CMake 配置时**必须**启用 `compile_commands.json` 生成，确保 clangd LSP 正常工作：

```bash
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ...
```

`compile_commands.json` 已写入 `.gitignore`，不会提交，也不会随 worktree checkout 带过去。

## clangd 编译数据库定位

本项目根目录**无 `.clangd` 配置、无 `compile_commands.json` 符号链接**。clangd 默认沿目录树向上查找最近的 `compile_commands.json`。

由于本项目是 in-source 构建在 `daemon/` 内，`daemon/compile_commands.json` 会被 clangd 命中。若改用 out-of-source 构建（`daemon/build/`），clangd 会找到 `daemon/build/compile_commands.json`。

### 潜在问题：多 worktree 串扰

主工作区与 worktree 若都用 in-source 构建，各自 `daemon/compile_commands.json` 中的绝对路径指向**各自工作区**的源码，互不影响--这是 in-source 构建相对符号链接方式的优势。

但若某 worktree 跳过 CMake 配置，clangd 会沿目录树向上回退，可能命中父级或主工作区的编译数据库，导致路径不匹配的错误诊断。

## worktree 中的编译数据库

worktree 必须各自独立生成编译数据库，否则 clangd 可能回退到错误路径：

1. worktree checkout 后，`daemon/compile_commands.json` 不会自动生成。
2. 在 worktree 中首次编码或 clangd 诊断相关操作前，必须先在该 worktree 运行一次 CMake 配置：

   ```bash
   cd daemon
   cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DFAKE_DRIVER=ON .
   ```

3. 配置成功后，`daemon/compile_commands.json` 中的绝对路径会正确指向该 worktree 的源码，clangd 即可正常解析。
4. 禁止依赖主工作区或其他 worktree 的编译数据库为本 worktree 提供诊断--路径不匹配会得到错误信息。

多个 worktree 同时存在时，各自独立的 `daemon/compile_commands.json` 完全隔离，可同时开多个编辑器或 Agent 会话互不干扰。详见 `.claude/rules/git-workflow.md` 的「Worktree 固定目录」。

## 开发脚本与 worktree 构建

各特性分支在仓库根维护各自的「编译+运行」一体化开发脚本，封装 out-of-source 构建（产物进 `daemon/build/`，自动带 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`）、临时配置生成、后台启停与状态查看。脚本与所在分支一同入库。

| 脚本 | 所在分支 | 构建特性 | 控制平面 | 用途 |
|------|---------|---------|---------|------|
| `oca-dev.sh` / `oca-daemonctl.sh` | `feature/aes70-oca` | `WITH_OCA=ON` | OCA（OCP.1 65037） + HTTP | AES70/OCA 开发与控制器验证 |
| `noise-dev.sh` / `noise-daemonctl.sh` | `feature/noise` | 不含 `WITH_OCA`（分支无该选项），后续 `WITH_NOISE` | 仅 HTTP | 噪声分析与降噪模块开发 |

- `*-dev.sh`：面向 FAKE_DRIVER，不加载内核模块、不启 ptp4l，后台跑 daemon 验证控制平面（`build`/`run`/`run-real`/`stop`/`status`/`clean`）。**编译优先用此脚本做 out-of-source 构建**。
- `*-daemonctl.sh`：面向真实硬件，加载 LKM + 起 ptp4l + 等 PTP 锁；`*-dev.sh run-real` 会委托它。OCA 版带 `--oca`，noise 版的 `--oca` 保留但不生效（分支无 OCA 代码，`oca_enabled` 被忽略）。
- 两套脚本的运行时 PID/日志文件按脚本前缀隔离（`/tmp/aes67-noise.*` vs `/tmp/aes67-dev.*`），主工作区与 noise worktree 可并行各自跑实例而不冲突。
- 跨分支复用：脚本以 `feature/aes70-oca` 为源头；noise worktree 复制并适配为 `noise-*` 命名，去掉本分支不存在的 CMake 选项/target（如 `WITH_OCA`、`oca-test`/`oca-probe`），不做软链（软链会让 `$TOPDIR` 指向错误分支的源码）。

## 注意事项

- AI 助手执行 CMake 配置时必须带上 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 参数。
- in-source 构建的 `daemon/CMakeFiles/`、`daemon/Makefile` 等已被 `.gitignore` 忽略，不要提交。
- 如果 `daemon/` 内构建产物被清理（如 `cleanup.sh`），需重新运行配置步骤以生成 `compile_commands.json`。
- `cleanup.sh` 是**破坏性**操作（`rm -rf` 子模块和构建产物），执行前务必确认。
