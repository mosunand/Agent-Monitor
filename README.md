# Agent Monitor — 通用 AI Agent 用量监控面板

一个纯本地的桌面工具：只读解析各 AI coding agent 写在本地磁盘的会话数据，把
**token 消耗**（输入 / 缓存 / 输出 / 推理）、**速度（TPS）**、**按模型 / 按 agent /
按工具的用量** 整理成可查、可追溯的界面。

**核心承诺：全程只读、全程离线。** 不修改任何 agent 的数据文件，不发起任何
网络请求——它是观测器，不是代理。

- 技术栈：C++17 / Qt 6.8 Widgets，无第三方运行时依赖
- 支持 7 个 agent（ZCode / Codex / Claude Code / Gemini CLI / Qwen Code / OpenCode / pi）
- 双主题（深 / 浅）+ 毛玻璃氛围背景，全自绘图表
- 数据层 44 项无头自检（`--selftest`）+ 11 页自动截图巡检（`--screenshot`）

---

## 目录

1. [安装与卸载](#安装与卸载)
2. [功能详解（六个页面）](#功能详解)
3. [支持的 agent](#支持的-agent)
4. [数据口径与统计规则](#数据口径与统计规则)
5. [数据存储与隐私](#数据存储与隐私)
6. [性能特性（实测）](#性能特性实测)
7. [常见问题](#常见问题)
8. [开发者指南](#开发者指南)
9. [已知限制](#已知限制)

---

## 安装与卸载

### 系统要求

- Windows 10（1809 及以上）或 Windows 11，**64 位**
- 磁盘空间：安装约 40 MB；运行期自建缓存库随数据量增长（每万次模型响应约几 MB）
- 不需要安装任何运行库——Qt、MinGW 运行库、数据库驱动全部随包携带

### 安装

双击 `AgentMonitor-Setup.exe`，中文向导一路下一步即可。默认装到
`C:\Program Files\Agent Monitor`，为所有用户安装（需要管理员权限确认一次 UAC）。

> **SmartScreen 提示**：安装包未做代码签名，首次运行会弹"Windows 已保护你的
> 电脑"蓝框——点 **更多信息 → 仍要运行** 即可。这是无签名程序的正常提示，
> 与包本身无关；彻底消除需要购买代码签名证书。

快捷方式：开始菜单（必建）+ 桌面图标（向导里可选）。安装完成可勾选"立即运行"。

### 卸载

开始菜单 → Agent Monitor → 卸载，或"设置 → 应用"里卸载。卸载只删除程序文件，
**不会**动你的用户数据（见[数据存储与隐私](#数据存储与隐私)）；想彻底清掉，
再删 `%LOCALAPPDATA%\zhipucode\agent-monitor` 目录即可。

---

## 功能详解

顶部导航条：`◉ 实时监控 · ◎ 使用统计 · ▤ 会话 · ◇ 模型用量 · ≣ 原始数据 · ▦ 数据源`
（快捷键 **Ctrl+1 … Ctrl+6**）。右侧依次是：健康状态信息（各 agent 会话数与扫描
耗时，悬停看明细）、实时速度胶囊（有新响应时浮现）、设置 ⚙、主题 🌙、氛围光晕 霜。

### ◉ 实时监控

主仪表盘，默认时间窗 24 小时（可切 今天 / 24h / 7d / 30d / 全部）。

- **11 张 KPI 卡**：总 token（今日）与总 token（所选窗口）两张大卡——含生成
  占比进度条和 Top 5 每模型彩色明细；模型请求 / 平均流式时长 / 输入（带缓存
  命中进度条）/ 输出 / 推理占比 / 缓存命中率 / 活跃会话 / 错误·中止率；底部
  一张全宽"平均 Token 速度"卡（加权 Σtoken ÷ Σ秒）。
- **3 张图表**（自绘，悬停有十字线 + tooltip）：模型请求柱状图、token 构成
  三线图（输入/输出/推理）、速度随时间折线（逐点按速度阈值着色：<20 红 /
  20–100 黄 / >100 绿）。
- **最近请求速度表**：窗口内最近 60 次可测流式响应，附模型大类筛选（"全部
  模型 / GLM / DeepSeek / …"，只列实际出现过的大类），页脚汇总加权均速。
- **实时活动流**：新完成的模型响应 2 秒内推入（新行闪烁 + 顶部实时胶囊联动），
  保留最近 60 条。
- **算力分布三张表**：按模型（含缓存/均时长/速度）、按 agent（含**占比列**与
  数值比例条——条长即占比）、按工具（按工具自身发生时间过滤，长会话不会把
  全部历史工具调用计入每个窗口）。

### ◎ 使用统计

累计口径的"总账"，数据来自程序自有的二进制归档——**即使你删掉 agent 的原始
数据，这里的历史依然保留**。右上角全局筛选（agent + 模型大类）作用于整页。

- **5 张统计卡**：累计 Token / 峰值 Token（单日最高）/ 最长聊天时长 / 当前
  连续天数 / 最长连续天数。
- **Token 活动热力图**：GitHub 风格的一年日历（每日 / 每周 / 累计三档）。
  亮度按**分位定级**：活跃日中最小的 25% 为最暗档、前 25% 直接满格——与
  绝对量级无关，因此不会出现"只有接近历史峰值那几天才全亮"的情况。悬停看
  当日 token 数。
- **每日 Token 趋势图**：近 7 日 / 近 30 日，按模型多线（日历天补零，没有
  数据的日子也占位）。
- **模型用量环形图** + 图例（模型 / 用量 / 占比，颜色与趋势图一致，进页有
  扫入动画，悬停扇区看明细）。
- **按 agent 分布表**：各 agent 的累计请求数与四类 token、占比列与比例条，
  与全局筛选联动。

### ▤ 会话

左侧会话列表（跨全部 agent）：按 agent 过滤、按"最近更新 / token 最多"排序、
关键字搜索（标题 / 目录 / 模型 / 会话 id）；每行两行布局，agent 名用自己的
品牌色，右侧完整显示总 token 数。右侧详情四个标签页，**均为懒加载**——只有
你切到某个标签，它才去解析对应内容，点开会话本体只要 2ms 级：

- **概览**：元数据表（agent / 会话 id / 模型 / 目录 / 上下文窗口 / 各类 token /
  缓存命中率 / 文件路径）+ 逐响应 TPS 曲线（逐点阈值着色）。
- **对话**：完整对话重放——用户 / 助手（带模型名）/ 推理 / 工具调用 / 工具输出
  五种角色分色排版；代码块、工具参数与输出进"等宽面板"，推理块带微染底色，
  出错的工具输出整块染红并标 ✗。超过 400 条显示前 400 条。
- **明细**：逐响应 token 表（虚拟化表格，几千条响应也零成本滚动）。
- **JSON**：该会话每条模型响应的统一模型原始记录（语法着色，与"原始数据"
  页的记录对话框同一套配色）。

### ◇ 模型用量

按模型（含 effort 变体）聚合的全量视图：每日生成 token 趋势（Top 5 模型）+
汇总表（请求数 / 四类 token / 总量与**占比** / 均时长 / 加权速度 / 最近使用，
token 列带比例条）。

### ≣ 原始数据

统一响应记录浏览器：按 agent 与模型关键字（LIKE）过滤，默认最近 400 条；
点击任意行弹出**记录对话框**——顶部摘要（agent 色点、模型、时间、时长、
速度、各类 token）+ 语法着色 JSON + 「复制 JSON」按钮。

### ▦ 数据源

各 agent 的探测状态（胶囊徽章：已检测 / 未检测）、数据目录、会话文件数、
最近活动；附数据口径说明与"如何新增一个 agent 适配器"的指南。

### 全局操作与设置

| 操作 | 方式 |
|---|---|
| 切换深 / 浅主题 | 顶栏 🌙，或快捷键 **T** |
| 氛围光晕（Aurora）开关 | 顶栏 "霜" |
| 历史归档保留天数（30–3650 天） | 顶栏 ⚙ ——拖动数值时实时显示"生效后保留哪天之后的数据" |
| 窗口位置与大小 | 自动记忆 |

---

## 支持的 agent

| Agent | 状态 | 数据来源 | 验证方式 |
|---|---|---|---|
| **ZCode** | ✅ 完整支持 | `~/.zcode/cli/db/db.sqlite`（只读，`ZCODE_DB` 可改） | 本机实测（含对话重放：按 ZCode 自己的 UI 显示语义过滤，注入的隐藏消息不显示） |
| **Codex**（OpenAI，CLI / Desktop / IDE 扩展） | ✅ | `~/.codex/sessions/**/*.jsonl` + `archived_sessions`（`CODEX_HOME` 可改） | 本机实测 |
| **Claude Code**（Anthropic） | ✅ | `~/.claude/projects/<munged-cwd>/*.jsonl`（`CLAUDE_CONFIG_DIR` 可改） | 本机实测 |
| **Gemini CLI** | ⚠️ 按源码格式实现 | `~/.gemini/tmp/*/chats/`（新旧两代格式） | 文档比对 |
| **Qwen Code（千问）** | ⚠️ | `~/.qwen/tmp/*/chats/`（`QWEN_HOME` 可改） | 文档比对 |
| **OpenCode** | ⚠️ v2 SQLite | `%APPDATA%/opencode/opencode.db` 等候选路径（`OPENCODE_DB` 可改） | 源码比对 |
| **pi**（badlogic） | ⚠️ | `~/.pi/agent/sessions/**/*.jsonl`（`PI_CODING_AGENT_DIR` 可改） | 官方文档比对 |
| DeepSeek Harness (dsh) | 🗺️ 路线图 | `~/.dsh/sessions`（zstd 压缩，需引入依赖） | — |

⚠️ 标注的适配器按官方文档 / 源码格式实现、防御式解析：格式变化时自动降级为
空，不影响其他 agent；装了对应 agent 后会被自动探测并纳入统计。

第三方模型（如 Claude Code 路由到 GLM / DeepSeek）会以模型名出现在该 agent
的记录里，按模型维度自然覆盖。

---

## 数据口径与统计规则

- **一条记录 = 一次完成的模型响应**，带五类 token：输入 / 缓存读 / 缓存写 /
  输出 / 推理。总 token = 输入 + 输出 + 推理（缓存不计入总量）。
- **TPS =（输出 + 推理）÷ 流式秒数**，分层着色 <20 红 / 20–100 黄 / >100 绿；
  "加权平均" = Σ生成 token ÷ Σ可测秒数（不是逐条平均）。
- **时长可测性**：Claude Code / ZCode / OpenCode 有精确的流式时长；Codex 与
  pi 不落每 token 时间戳，速度用相邻响应时间差近似，界面标 **≈**；相邻响应
  间隔超过 120 秒（用户暂停 / 长工具）视为不可测，不计入速度统计；100ms
  以下的单块响应也不参与（时间粒度无意义）。
- **错误口径**：Codex 的 turn_aborted、Claude Code 的 API 错误行、ZCode 的
  error/cancelled 各记一条零 token 错误记录（已消耗的 token 保留在统计里，
  只有速度统计把它排除），错误率 = 错误 ÷ 总请求。
- **时间与"今天"**：时间戳按本地时区处理，"今天"按本地零点切；所有日聚合
  对齐本地午夜。
- **模型大类**：模型名第一个 `-` 前的小写段（`GLM-5.3-Flash` → `glm`），
  大类筛选覆盖其下所有具体型号。
- **最长聊天时长**：ZCode 用官方口径（会话内已完成 turn 的时长之和），其他
  agent 用会话内流式时长之和。

---

## 数据存储与隐私

**这个程序看什么、写什么，全部列在下面：**

### 读（只读，绝不写入）

- 各 agent 的数据目录（见上表）——打开文件、解析、关掉，全程只读；对
  正在被 agent 写入的文件用数据库忙碌等待与跳过策略，不会锁住对方。

### 写（只有三处，全是自己的数据）

| 位置 | 内容 | 可否删除 |
|---|---|---|
| `%LOCALAPPDATA%\zhipucode\agent-monitor\cache.sqlite` | 解析缓存（WAL）。删了会自动重建（下次全量重扫） | ✅ 随时 |
| `<exe 目录>\usage-archive.bin` | 自有历史归档：按 (agent × 会话 × 模型 × 天) 的聚合。**删掉 agent 原始数据后历史统计仍在**。可用 `AGENT_MONITOR_ARCHIVE` 环境变量改路径；保留期在 ⚙ 设置（默认 730 天） | ✅ 随时（自动重建） |
| 注册表 `HKCU\Software\zhipucode\agent-monitor` | 主题 / 毛玻璃开关 / 归档保留天数 / 窗口几何 | ✅ 随时 |

### 网络

**零网络请求。** 程序没有任何网络代码——不联网、不上报、不检查更新。
你在哪个 agent 用了什么模型，只有你自己这台电脑上的这个程序知道。

### 缓存与归档的关系

缓存库是"加速层"（把解析过的会话存成查询友好的结构，源文件变了才重解析）；
归档是"历史层"（哪怕源数据被 agent 的保留策略清掉，累计统计、热力图、连续
天数依然完整）。两者都可随时删除，程序会自动重建。

---

## 性能特性（实测）

| 项 | 实测值 |
|---|---|
| 空闲轮询（2 秒一拍，无新数据） | 14–21 ms，归档合并自动跳过 |
| 有新数据的一拍 | 目录遍历 ~70 ms + 归档合并 ~23 ms |
| 会话增量扫描 | 只重解析 mtime/size 变了的文件，未变的零成本跳过 |
| 点开会话（概览） | ~2 ms（千级响应会话实测）；明细 / 对话 / JSON 懒加载 |
| 明细表滚动 | 模型/视图虚拟化，只画可见行 |
| 最小化时 | 轮询自动暂停（恢复后下一拍续上） |
| GUI 线程 | 零磁盘 IO——扫描、查询、解析全部在池线程 |

---

## 常见问题

**Q：打开后全是"未检测"？**
那台机器没装对应 agent（或数据目录不存在）。装了 agent 并跑过至少一次，数据
目录生成后就会被自动探测。数据目录位置可用环境变量覆盖（见 agent 表）。

**Q：速度/时长和别的工具对不上？**
口径不同。本工具只统计**可测的流式时长**（见上文规则）；比如带 ≈ 的 Codex
速度是响应间隔近似值，和逐 token 计时天然有差异。

**Q：顶栏红色"扫描失败"徽章？**
某个 agent 的数据目录暂时打不开（典型：该 agent 正在做数据库独占操作）。
下一拍（2 秒后）会自动重试；持续红色才需要排查（看"数据源"页的备注列）。

**Q：删除了 agent 的会话数据，统计会消失吗？**
使用统计页不会——它读的是自有归档。实时监控页（读缓存/源数据）对应的记录
会在下次扫描时清掉，这是预期行为。

**Q：Claude Code 的会话越来越少了？**
Claude Code 默认 30 天保留期会自动清理旧会话文件。本工具的缓存会同步清掉，
但归档里的历史统计保留。

**Q：安装版会写 Program Files 吗？**
程序文件之外不写安装目录；用户数据全在 `%LOCALAPPDATA%` 和 HKCU。唯一的
例外见[已知限制](#已知限制)第 1 条。

**Q：多开安全吗？**
可以多开（只读观测），但两个实例会各自扫描与写缓存，没必要。

---

## 开发者指南

### 构建与验证

```bash
cmake -B build -G Ninja          # 需要 Qt 6.8+（Widgets/Sql/Concurrent）、CMake≥3.21、Ninja
cmake --build build
./build/bin/agent-monitor.exe              # 主界面
./build/bin/agent-monitor.exe --selftest   # 无头数据层自检（44 项断言，exit 0/1）
./build/bin/agent-monitor.exe --screenshot shots  # 11 页自动截图（视觉回归）
```

### 新增一个 agent 适配器

1. 新建 `XxxAdapter : AgentAdapter`，实现 `id / displayName / color /
   dataPresent / listSessionFiles / parseFile`（把自家 JSONL / JSON / SQLite
   归一化为 `types::SessionInfo` + `types::UsageRecord`，逐行防御式解析；
   支持对话重放则再实现 `parseMessages`）；
2. 在 `AgentRegistry::createAdapters()` 注册；
3. CMakeLists 加入源文件。

UI、过滤、聚合、缓存、归档全部自动生效。

### 架构

```
src/
├── core/     数据层（无 Widgets 依赖）
│   ├── Types.h          统一模型（UsageRecord / SessionInfo / 聚合）
│   ├── AgentAdapter     适配器插件接口
│   ├── ZCodeAdapter     SQLite 源（含 message/part 对话重放）
│   ├── CodexAdapter     rollout JSONL（新旧双格式）
│   ├── ClaudeAdapter    projects JSONL（message.id 流式分组 → 精确 TPS）
│   ├── GeminiLike / OpenCode / PiAdapter
│   ├── Store            增量扫描 + SQLite 缓存 + 二进制归档 + 全部查询
│   └── Paths            各 agent 目录解析（环境变量可覆盖）
├── ui/
│   ├── Theme / UiUtil   双主题 QSS 令牌 / 徽章·卡片·悬停表格·比例条 / JSON 着色
│   ├── MainWindow       顶栏 + 页面栈 + 健康轮询 + toast + 截图巡检
│   ├── widgets/         KpiCard · MiniChart · LiveFeed · UsageDonut · UsageHeatmap
│   └── pages/           实时监控 · 使用统计 · 会话(+四标签) · 模型 · 原始 · 数据源
├── util/     Format（中文单位/速度分层）· Async · Frost(DWM) · CrashHandler
└── selftest/ --selftest 无头验证（真实本地数据）
```

质量基线：构建零警告（Qt 6.8.3 / MinGW g++ 13 / C++17）；44 项断言含聚合
逐字段自洽、增量扫描幂等、工具聚合 / 错误簿记 / 对话重放 / 归档写入与对账。

### 打包分发（Inno Setup）

项目 `Agent-Monitor-Pro` 为发布工作区（源码副本 + `AgentMonitor.iss` +
中文语言文件）。更新版本两步：

```bash
cmake --build build        # 保证 exe 最新
"D:\Applications\innosetup\Inno Setup 6\ISCC.exe" AgentMonitor.iss
# 产物：AgentMonitor-Setup.exe（项目根目录）
```

版本号改 `AgentMonitor.iss` 顶部的 `#define MyAppVersion`。

---

## 已知限制

1. **"所有用户"安装 + 标准用户运行时，自有归档不落盘**：`usage-archive.bin`
   写在 exe 旁，标准用户对 Program Files 无写权限——程序不报错、不崩溃
   （静默跳过写入，本次会话内归档照常工作），但跨会话的历史归档会失效。
   变通：以管理员运行，或设 `AGENT_MONITOR_ARCHIVE` 指向可写目录。
2. Codex 的 TPS 为响应级近似（**≈** 标注）；token 级 TTFT/ITL 需要本地代理
   截流，暂未实现。
3. Codex `rate_limits` 配额字段（含 used_percent / resets_in_seconds）已确认
   存在于 rollout 中，但采样样本全为 null，配额燃尽曲线暂未接入。
4. ZCode 的子 agent 树（session_task_link）未接入。
5. ChatGPT 网页版 / Codex cloud 任务无本地数据，不在覆盖范围；DeepSeek
   Harness（dsh）在路线图上。
6. 仅实测 Windows；代码层的非 Windows 分支存在但未验证。

---

## 许可

MIT
