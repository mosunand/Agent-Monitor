// SourcesPage.cpp — see SourcesPage.h.

#include "SourcesPage.h"

#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "util/Format.h"

using namespace UiUtil;

SourcesPage::SourcesPage(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

void SourcesPage::buildUi()
{
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    content->setMaximumWidth(1100);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(24, 20, 24, 64);
    outer->setSpacing(0);
    auto* center = new QHBoxLayout;
    center->addStretch(1);
    center->addWidget(content, 10);
    center->addStretch(1);
    auto* host = new QWidget;
    host->setLayout(center);
    scroll->setWidget(host);
    rootLay->addWidget(scroll);

    auto* toolbar = new QWidget;
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(0, 0, 0, 12);
    tb->setSpacing(10);
    auto* h1 = new QLabel(QStringLiteral("数据源"));
    h1->setObjectName(QStringLiteral("h1"));
    auto* sub = new QLabel(QStringLiteral("只读观测，绝不修改任何 agent 的数据"));
    sub->setProperty("cls", "muted");
    sub->setStyleSheet("font-size:9pt;");
    tb->addWidget(h1);
    tb->addWidget(sub);
    tb->addStretch(1);
    outer->addWidget(toolbar);
    outer->addWidget(UiUtil::gradientDivider());

    auto* statusCard = new CardFrame;
    auto* sl = new QVBoxLayout(statusCard);
    sl->setContentsMargins(14, 12, 14, 12);
    auto* st = new QLabel(QStringLiteral("已支持的 agent（自动探测）"));
    st->setObjectName(QStringLiteral("cardTitle"));
    sl->addWidget(st);
    m_statusTable = new RowHoverTable;
    m_statusTable->setColumnCount(6);
    m_statusTable->setHorizontalHeaderLabels(
        {QStringLiteral("Agent"), QStringLiteral("状态"), QStringLiteral("数据目录"),
         QStringLiteral("会话文件"), QStringLiteral("最近活动"), QStringLiteral("备注")});
    m_statusTable->verticalHeader()->hide();
    m_statusTable->horizontalHeader()->setStretchLastSection(true);
    m_statusTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_statusTable->setSelectionMode(QAbstractItemView::NoSelection);
    sl->addWidget(m_statusTable);
    outer->addWidget(statusCard);
    outer->addSpacing(12);

    // ── docs ──
    outer->addWidget(sectionLabel(QStringLiteral("数据来源与口径")));
    const Palette& pal = Theme::instance().pal();
    auto addDoc = [&](const QString& title, const QStringList& lines) {
        auto* card = new CardFrame;
        auto* l = new QVBoxLayout(card);
        l->setContentsMargins(14, 12, 14, 12);
        l->setSpacing(4);
        auto* t = new QLabel(title);
        t->setProperty("cls", "how-title");
        l->addWidget(t);
        for (const QString& line : lines) {
            auto* b = new QLabel(line);
            b->setProperty("cls", "how-body");
            b->setWordWrap(true);
            b->setTextFormat(Qt::RichText);
            b->setOpenExternalLinks(false);
            l->addWidget(b);
        }
        outer->addWidget(card);
        outer->addSpacing(8);
    };

    addDoc(QStringLiteral("Codex（OpenAI，覆盖 ChatGPT 订阅 / API）"), {
        QStringLiteral("目录：<span style=\"font-family:Consolas\">~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl</span>（含 archived_sessions）"),
        QStringLiteral("口径：<span style=\"font-family:Consolas\">token_usage_record</span>（新格式）或 <span style=\"font-family:Consolas\">event_msg:token_count · last_token_usage</span>（旧格式）→ 每响应 input / cached / cache_write / output / reasoning 细分。"),
        QStringLiteral("TPS：Codex 不落每 token 时间戳，速度 = 相邻响应时间差（≈，含工具间隔），界面用 <b>≈</b> 标注。"),
        QStringLiteral("模型 / effort：<span style=\"font-family:Consolas\">turn_context</span> 逐回合记录。"),
    });
    addDoc(QStringLiteral("Claude Code（Anthropic）"), {
        QStringLiteral("目录：<span style=\"font-family:Consolas\">~/.claude/projects/&lt;munged-cwd&gt;/&lt;session-uuid&gt;.jsonl</span>"),
        QStringLiteral("口径：assistant 行 <span style=\"font-family:Consolas\">message.usage</span>（input / cache_creation / cache_read / output）；同一 message.id 的多条流式行取最后一条 usage。"),
        QStringLiteral("TPS：同 id 首末行时间差 = 真实流式时长（精确，无 ≈ 标注）；isSidechain 行标记为 sidechain。"),
    });
    addDoc(QStringLiteral("token 速度分层"), {
        QStringLiteral("<span style=\"color:%1\">■</span> &lt;30 t/s &nbsp; <span style=\"color:%2\">■</span> 30–80 t/s &nbsp; <span style=\"color:%3\">■</span> &gt;80 t/s")
            .arg(pal.sevErr.name(), pal.sevWarn.name(), pal.sevOk.name()),
        QStringLiteral("加权平均 = Σ(输出+推理 token) ÷ Σ(流式秒数)，只统计可测时长的响应。"),
    });
    addDoc(QStringLiteral("缓存库（本应用自己的数据）"), {
        QStringLiteral("路径：<span style=\"font-family:Consolas\">&lt;AppData/Local&gt;/agent-monitor/cache.sqlite</span>（WAL，可随时删除重建）"),
        QStringLiteral("扫描：每 2s 对比各会话文件的 mtime/size，只重解析变化的文件；响应记录按文件路径增量入库。"),
        QStringLiteral("新增 agent：实现 <span style=\"font-family:Consolas\">AgentAdapter</span>（id / 目录探测 / 文件枚举 / parseFile→统一模型）并加入 <span style=\"font-family:Consolas\">AgentRegistry::createAdapters()</span>，UI 与聚合自动生效。"),
    });
    outer->addStretch(1);
}

void SourcesPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    reloadStatus();
}

void SourcesPage::reloadStatus()
{
    const Palette& pal = Theme::instance().pal();
    const auto statuses = Store::instance().agentStatuses();
    m_statusTable->setRowCount(int(statuses.size()));
    for (int i = 0; i < statuses.size(); ++i) {
        const types::AgentStatus& s = statuses[i];
        auto* name = new QTableWidgetItem(s.name);
        name->setFlags(Qt::ItemIsEnabled);
        name->setFont(QFont(QStringLiteral("Consolas"), 10));
        name->setForeground(s.color);
        m_statusTable->setItem(i, 0, name);
        // 状态：胶囊徽章（与实时监控页「已连接」同一套语言）
        auto* cell = new QWidget;
        auto* cl = new QHBoxLayout(cell);
        cl->setContentsMargins(2, 0, 2, 0);
        auto* badge = new BadgeLabel(
            s.found ? QStringLiteral("已检测") : QStringLiteral("未检测"),
            s.found ? QStringLiteral("green") : QStringLiteral("dim"));
        cl->addWidget(badge, 0, Qt::AlignCenter);
        m_statusTable->setCellWidget(i, 1, cell);
        auto* dir = new QTableWidgetItem(s.dataDir);
        dir->setFlags(Qt::ItemIsEnabled);
        dir->setFont(QFont(QStringLiteral("Consolas"), 8));
        m_statusTable->setItem(i, 2, dir);
        auto* n = new QTableWidgetItem(s.found ? QString::number(s.sessions) : QStringLiteral("—"));
        n->setFlags(Qt::ItemIsEnabled);
        n->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_statusTable->setItem(i, 3, n);
        auto* last = new QTableWidgetItem(s.found ? Format::fmtTime(s.lastActivityMs) : QStringLiteral("—"));
        last->setFlags(Qt::ItemIsEnabled);
        m_statusTable->setItem(i, 4, last);
        auto* note = new QTableWidgetItem(s.note);
        note->setFlags(Qt::ItemIsEnabled);
        note->setForeground(pal.fg3);
        m_statusTable->setItem(i, 5, note);
    }
    m_statusTable->resizeColumnsToContents();
    m_statusTable->resizeRowsToContents();
    // deterministic stretch: note column absorbs the remaining width
    m_statusTable->horizontalHeader()->setSectionResizeMode(
        m_statusTable->columnCount() - 1, QHeaderView::Stretch);
    m_statusTable->setFixedHeight(UiUtil::tableContentHeight(m_statusTable, 400));
}
