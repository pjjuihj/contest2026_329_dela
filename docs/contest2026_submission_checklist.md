# 2026 openvela AI 硬件赛道验收清单

更新时间：2026-08-29（Asia/Shanghai）
项目：VelaWear Agent；硬件：SF32LB52 黄山派；当前权威构建环境：VM `192.168.175.128`。

## 结论摘要

新镜像已在当前 VM 的完整 openvela 工程中构建，并通过 1 Mbps 完整镜像方式烧录到黄山派；冷启动、独立 WinRT 广播、GATT/CCCD/通知和 UART 关联均已复验通过。当前仍不能宣称作品已完成：板端没有有效网络/LLM 凭据，真实对话、Skill 执行、主动场景、可见 UI/扬声器物理反馈和提交材料仍未闭环。

2026-08-29 新镜像证据：`docs/evidence/velawear_ble_hil_2026-08-29.log`。

## 2026-08-28 真机复验记录

- 修复 packages/ai_agent/src/agent_main.c：板级 CONFIG_MQ_MAXMSGSIZE=32 时跳过 4096 字节 QuickApp mqueue，避免 mq_msgqalloc.c 断言；修复版启动日志确认没有该断言。
- 同一文件的 network watcher 已改为可取消、可 join 的线程；退出回归确认 watcher、Cron、Heartbeat 均停止，`Shutdown complete` 出现且无 `Assertion failed`。正式日志为 docs/evidence/velawear_ai_agent_shutdown_clean_2026-08-28.log。
- 板端 ai_agent 已完成初始化：AI Agent ready、vela>、36 个内置工具注册、10 个内置 Skill 安装、VelaWear Wellness Skill 安装。
- /skill、/remind 命令已送入 CLI；因网络断开，agent_loop 未启动，未形成实际 Agent 回复或物理提醒。
- 运行证据：docs/evidence/velawear_ai_agent_runtime_2026-08-28.log。历史断言日志仍保留在 docs/evidence/velawear_ai_agent_shutdown_assert_2026-08-28.log，作为修复前回归记录；最新干净退出以正式日志为准。

## 必做项状态

| 比赛要求 | 当前状态 | 证据或剩余工作 |
| --- | --- | --- |
| 基于 openvela + ai_agent | 代码/构建/板端启动通过 | .config 含 CONFIG_EXAMPLES_AI_AGENT_VELA=y；修复版 ai_agent 冷启动进入 AI Agent ready 与 vela>，最终 ELF 有 ai_agent_main、skill_loader_init、velawear_main、XiaoZhi 入口和 velawear_skill_install。 |
| 在指定硬件上编译并烧录 | 已烧录、校验并冷启动 | 当前 nuttx.bin（5,994,568 bytes，SHA-256 `71be9a043790408bdf5121dcd0ad09492d2483cfb5d55894f2c2b03350251ad9`）以 1,000,000 波特率写入 `0x12010000`，`--verify` 通过并软复位；串口看到 SFBL、nsh>、VelaWear 初始化和 Agent started successfully。 |
| 配置 LLM 后端、基础对话 | 未完成 | 板端 config_show 显示 API key、LLM host/path 均未设置，网络为 disconnected / 0.0.0.0；XiaoZhi 连 api.tenclass.net:443 返回 -0x0052。需接入可用网络、配置真实后端/团队服务凭据，并完成一轮基础对话。 |
| 至少一个交互渠道 | CLI 板端启动通过，真实对话未完成 | 板端已从 nsh> 启动 Agent，XiaoZhi worker 运行并等待 PAN peer；云端仍未连接，尚无有效问答或语音闭环。 |
| 至少一个自定义 Skill | 文件安装通过，执行演示未完成 | 板端日志确认 /data/agent/skills/velawear-wellness.md 已安装，/skill 请求已送入 CLI；因无网络且 Agent loop 未启动，尚未得到 Skill 列表回复或实际执行结果，仍需联网后完成演示。 |
| 至少一个“主动 + 执行”场景 | 软件路径已实现，板端执行未闭环 | Wellness Skill 仍走 get_current_time → cron_add → cron_list；板端 /remind 已送出但无 Agent loop 回复，尚未证明主动触发、cron_list 确认和 LCD/声音/震动反馈。 |
| 场景说明 | 已有 README/docs，需最终校对 | 必须把用户故事、完整功能清单、使用的 Agent 能力、限制条件统一到最终作品介绍中。 |
| 团队自建并维护服务端 | 未完成/未证明 | 当前 XiaoZhi 仍连接 api.tenclass.net，且板端无有效网络和凭据；比赛要求团队自建并维护服务端，需补齐服务端、凭据和联调证据。 |
| 语音唤醒词 | 条件项未完成 | 当前实现是按键/CLI 手动触发；若最终作品宣称语音唤醒，需使用官方要求的 你好，openvela / Hello, openvela，并录制可复现证据。 |
| AI Coding 日志 | 已整理并提交 | 现有队伍 JSONL 已通过此前官方校验（3820 个事件、`ALL OK`），`logs/your-github-login/` 示例目录已移除；2026-08-29 HIL 证据已单独提交。 |
| Apache 2.0 许可 | 已补充根许可证，待最终审阅 | 正式仓根目录已加入 Apache 2.0 `LICENSE`；提交前确认新增/改动文件的版权头和第三方来源说明。 |
| 代码源与实际构建源一致 | 常规源文件一致，新增运行时修复已纳入构建 | 正式仓 `app/velawear_agent` 与实际 CMake 演示链接目标的 44 个非备份源文件逐文件哈希一致；本次 Ninja 生成了新镜像。提交前排除 `.orig/.rej/.before-*/.bak`。 |
| 独立作品介绍文档 | 已补充，待最终校对 | 已加入 `docs/submission/velawear_project_introduction.pdf` 及 Markdown 源稿；内容基于已验证证据，提交前仍需结合最终实机演示校对。 |
| 不超过 5 分钟演示视频 | 未发现 | 仓库中没有 `.mp4/.mov/.mkv/.avi`；需录制并提交。 |
| GitHub fork/PR/review/合入 | 日志、源码和文档已提交并推送，PR #6 待检查合入 | HIL 日志提交 `22115e6`、源码提交 `cf62211`、证据与文档提交 `0f13ccb` 已推到 fork 分支 `codex/velawear-hil-2026-08-29`；已用合并提交 `7de3189` 解决官方基线冲突，PR #6 待完成检查、自审和合入。 |
| vendor_sifli 板级修改提交 | 未完成 | Agent/VelaWear defconfig 修改仍在 VM 的 vendor 工作树；需按指南在对应公共仓 fork + PR，或按组委会要求提交板级补丁。 |

## 可复现构建证据

构建根目录：`/home/cmj/桌面/1/contest2026_329_dela/openvela`  
构建命令：ninja -C cmake_out/lckfb_huangshan_pi -j4
结果：后台构建 status=0；`nuttx`、`first_link`、`second_link` 和 `final_nuttx` 均生成，未出现编译错误。`ninja -t commands` 已确认修复后的 packages/ai_agent/src/agent_main.c 进入实际编译命令。

- nuttx.bin：5,994,568 bytes；SHA-256：71be9a043790408bdf5121dcd0ad09492d2483cfb5d55894f2c2b03350251ad9
- final_nuttx：37,851,268 bytes；SHA-256：b229c3ee3778510990963ef2e770bc36f066e21d90f43d4595ecbe86d47cf45c
- System.map：339,604 bytes；SHA-256：b7436b1997528284aa643c4b77077ab5a5a0137f553fb1a1a58fd137086793a0
- final_nuttx 中已找到：ai_agent_main、skill_loader_init、velawear_main、velawear_skill_install、xiaozhi_*、bt_id_get_mc、popen、pclose、system。

## 硬件复验顺序

1. 当前镜像已完成 1 Mbps 完整镜像烧录、校验和冷启动；新镜像 HIL 记录见 `docs/evidence/velawear_ble_hil_2026-08-29.log`。
2. 板端运行日志保存在 docs/evidence/velawear_ai_agent_runtime_2026-08-28.log；干净退出回归保存在 docs/evidence/velawear_ai_agent_shutdown_clean_2026-08-28.log。新增 HIL 已确认 RF、GATT、CCCD 和通知，但上述证据仍不等同于可见 UI 或可听声音证据。
3. 先给板端配置可用网络与真实 LLM/团队服务凭据，使 Agent loop 启动；再执行 /skill、Wellness reminder，并保存 cron_add、cron_list、主动触发和设备反馈的连续证据。
4. 另行用独立扫描器验证 BLE 广播、连接、CCCD 和特征值往返；用相机/录音设备验证 LCD、触摸、扬声器与语音链路。
5. 提交前清理 .orig/.rej/.before-*/.bak 残留，制作介绍文档/视频，完成 fork/PR/self-review/merge。
