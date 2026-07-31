# 团队共享 Skill 索引

本目录下 skill 为团队共享，与所在分支一同入库。调用方式：用户输入 `/<skill-name>`
或 Agent 按触发条件主动调用。

| Skill | 说明 |
|-------|------|
| [noise-verify](noise-verify/SKILL.md) | 噪声模块准确性与性能端到端验证：构建 daemon、64 路 ESC-50 并发检测、三路降噪对比、并发降噪容量测试，更新检测报告 |

## 维护规则

- 新增/修改 skill 必须同步更新本索引。
- skill 格式与维护规范见 `.claude/rules/skills.md`。
- 禁止把团队共享 skill 放在 `docs` 旧 skills 目录或个人目录。
