# 公共 Skill 维护规则

本文件定义团队共享 Claude Code skill 的目录、格式和维护规范。

## 目录约定

团队共享 skill 固定提交到项目内：

```text
.claude/skills/<skill-name>/SKILL.md
```

索引文件固定为：

```text
.claude/skills/README.md
```

禁止把团队共享 skill 放在：

- `docs` 下的旧 skills 目录
- 个人目录，例如 `~/.claude/skills/`
- 本地私有配置目录

## Skill 命名

- 使用小写英文、数字和短横线。
- 名称应描述触发场景或动作。
- 避免项目外无法理解的缩写。

示例：

```text
release
code-review-checklist
systematic-debugging
```

## `SKILL.md` 格式

每个 skill 必须包含 YAML frontmatter：

```markdown
---
name: release
description: Use when user wants to release a new version of QFitcanAudioStreamMonitor.
---
```

规则：

1. `name` 必须与目录名一致。
2. `description` 必须以 `Use when...` 开头。
3. `description` 只描述触发条件，不总结执行流程。
4. 正文优先使用中文；保留命令、API 和标准术语英文原文。
5. 文档图表必须使用 Mermaid，并遵守 `.claude/rules/docs.md`。

## 维护流程

新增或修改公共 skill 时必须同步：

1. 更新对应 `.claude/skills/<skill-name>/SKILL.md`。
2. 更新 `.claude/skills/README.md` 索引。
3. 检查 `CLAUDE.md`、`AGENTS.md` 和 `.claude/rules/` 中是否存在旧路径引用。
4. 提交前确认入口规则不再引用旧的 docs skills 路径。

## 内容边界

适合写成 skill：

- 可复用的团队流程。
- 多次执行且需要固定步骤的任务。
- 需要 Agent 主动调用的操作手册。

不适合写成 skill：

- 一次性任务记录。
- 当前项目必须长期遵守的基础规则；这类内容应写入 `.claude/rules/`。
- 设计或实施计划；这类内容应写入 `docs/superpowers/specs/` 或 `docs/superpowers/plans/`。

## 验证要求

提交公共 skill 前至少检查：

```bash
git grep -n "docs/skills" -- CLAUDE.md AGENTS.md .claude/rules .claude/skills/README.md
git status --short
```

如果 skill 涉及实际命令或外部发布流程，必须在正文中明确 dry-run 或用户确认步骤。
