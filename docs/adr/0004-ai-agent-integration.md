# ADR-0004: ai_agent 集成方式

## 状态
已接受

## 背景
VelaWear 需要 LLM 能力，需要决定如何集成 ai_agent 框架。

## 决策
采用**内嵌模式**，VelaWear Agent 直接调用 velaclaw_client，自主管理 LLM 交互。

## 原因
1. **控制权**：完全控制决策流程，不依赖 ai_agent 的消息总线
2. **灵活性**：可以自定义 Skill 和 Tool，不受框架约束
3. **性能**：减少进程间通信开销
4. **调试**：更容易调试和追踪问题

## 替代方案
- **独立进程模式**：与 ai_agent 通过消息总线通信，但增加复杂度
- **扩展模式**：作为 ai_agent 的 Skill/Tool，但受框架约束

## 影响
- 需要直接使用 velaclaw_client API
- 需要自己管理 LLM 会话和上下文
- 需要自己实现 Skill 加载和执行
