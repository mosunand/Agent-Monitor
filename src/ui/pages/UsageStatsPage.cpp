// UsageStatsPage.cpp — see UsageStatsPage.h.

#include "UsageStatsPage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QVBoxLayout>

#include <algorithm>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "ui/widgets/KpiCard.h"
#include "ui/widgets/MiniChart.h"
#include "ui/widgets/UsageDonut.h"
#include "ui/widgets/UsageHeatmap.h"
#include "util/Async.h"
#include "util/Format.h"

using namespace UiUtil;

namespace {

QTableWidgetItem* numItem(const QString& text, double value = -1.0)
{
    auto* it = new QTableWidgetItem(text);
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    it->setFlags(Qt::ItemIsEnabled);
    if (value >= 0)
        it->setData(Qt::UserRole + 1, value); // 比例条基准值
    return it;
}

QTableWidgetItem* txtItem(const QString& text)
{
    auto* it = new QTableWidgetItem(text);
    it->setFlags(Qt::ItemIsEnabled);
    return it;
}

// color cycle for model slices (same family as ModelsPage trend)
QColor sliceColor(int i, const Palette& pal)
{
    switch (i % 8) {
    case 0: return pal.accent;
    case 1: return pal.catUsage;
    case 2: return pal.catLlm2;
    case 3: return pal.catTool2;
    case 4: return pal.sevWarn;
    case 5: return pal.catConversation;
    case 6: return pal.catNetwork;
    default: return pal.catLifecycle;
    }
}

} // namespace

UsageStatsPage::UsageStatsPage(QWidget* parent)
    : QWidget(parent)
{
    connect(&Theme::instance(), &Theme::changed, this, [this](bool) {
        if (m_loaded)
            renderData();
    });
    connect(&Store::instance(), &Store::dataChanged, this,
            [this]() { if (isVisible()) reload(); }, Qt::QueuedConnection);

    // KPI count-up
    m_countAnim.setDuration(700);
    m_countAnim.setEasingCurve(QEasingCurve::OutCubic);
    m_countAnim.setStartValue(0.0);
    m_countAnim.setEndValue(1.0);
    connect(&m_countAnim, &QVariantAnimation::finished, this, [this] {
        for (KpiCard* c : { m_kTotal, m_kPeak, m_kLongest, m_kStreak, m_kMaxStreak })
            c->setFlashEnabled(true);
    });
    connect(&m_countAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& v) {
                m_countProg = v.toDouble();
                updateKpiValues();
            });
    buildUi();
}

void UsageStatsPage::buildUi()
{
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    content->setMaximumWidth(1200);
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
    auto* h1 = new QLabel(QStringLiteral("使用统计"));
    h1->setObjectName(QStringLiteral("h1"));
    auto* sub = new QLabel(QStringLiteral("累计消耗 · 活动日历 · 连续使用 · 模型占比"));
    sub->setProperty("cls", "muted");
    sub->setStyleSheet("font-size:9pt;");
    tb->addWidget(h1);
    tb->addWidget(sub);
    tb->addStretch(1);
    // ── 全局筛选（右上角）：agent / 主模型，切换后整页统计联动 ──
    tb->addWidget(UiUtil::label(QStringLiteral("Agent"), QStringLiteral("muted")));
    m_filterAgent = new QComboBox;
    m_filterAgent->setMinimumContentsLength(12);
    connect(m_filterAgent, &QComboBox::currentIndexChanged, this,
            [this](int) { reload(); });
    tb->addWidget(m_filterAgent);
    tb->addWidget(UiUtil::label(QStringLiteral("模型"), QStringLiteral("muted")));
    m_filterModel = new QComboBox;
    connect(m_filterModel, &QComboBox::currentIndexChanged, this,
            [this](int) { reload(); });
    tb->addWidget(m_filterModel);
    outer->addWidget(toolbar);
    outer->addWidget(UiUtil::gradientDivider());

    // ── KPI row: 5 cards ──
    auto* kpiRow = new QWidget;
    auto* kl = new QHBoxLayout(kpiRow);
    kl->setContentsMargins(0, 0, 0, 0);
    kl->setSpacing(10);
    m_kTotal = new KpiCard(QStringLiteral("累计 Token 数"));
    m_kPeak = new KpiCard(QStringLiteral("峰值 Token 数"));
    m_kLongest = new KpiCard(QStringLiteral("最长聊天时长"));
    m_kStreak = new KpiCard(QStringLiteral("当前连续天数"));
    m_kMaxStreak = new KpiCard(QStringLiteral("最长连续天数"));
    for (KpiCard* c : { m_kTotal, m_kPeak, m_kLongest, m_kStreak, m_kMaxStreak }) {
        c->setValueFirst(true); // modern stats card: value on top, label below
        kl->addWidget(c, 1);
    }
    outer->addWidget(kpiRow);
    outer->addSpacing(12);

    // ── Token 活动 (heatmap) ──
    auto* heatCard = new CardFrame;
    auto* hl = new QVBoxLayout(heatCard);
    hl->setContentsMargins(14, 12, 14, 12);
    hl->setSpacing(8);
    auto* heatHead = new QHBoxLayout;
    auto* heatTitle = new QLabel(QStringLiteral("Token 活动"));
    heatTitle->setObjectName(QStringLiteral("cardTitle"));
    heatHead->addWidget(heatTitle);
    heatHead->addStretch(1);
    auto* modeRow = new QWidget;
    auto* mr = new QHBoxLayout(modeRow);
    mr->setContentsMargins(0, 0, 0, 0);
    mr->setSpacing(0);
    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->setExclusive(true);
    const struct { const char* label; int mode; } modes[] = {
        { "每日", int(UsageHeatmap::Daily) },
        { "每周", int(UsageHeatmap::Weekly) },
        { "累计", int(UsageHeatmap::Cumulative) },
    };
    for (const auto& m : modes) {
        auto* b = new QPushButton(QString::fromUtf8(m.label));
        b->setCheckable(true);
        b->setProperty("cls", "on");
        b->setCursor(Qt::PointingHandCursor);
        b->setChecked(m.mode == int(UsageHeatmap::Daily));
        m_modeGroup->addButton(b, m.mode);
        mr->addWidget(b);
    }
    connect(m_modeGroup, &QButtonGroup::idClicked, this,
            &UsageStatsPage::onModeChanged);
    heatHead->addWidget(modeRow);
    hl->addLayout(heatHead);
    m_heatmap = new UsageHeatmap;
    m_heatmap->setMinimumHeight(160);
    hl->addWidget(m_heatmap);
    outer->addWidget(heatCard);
    outer->addSpacing(12);

    // ── 每日 Token 趋势图 ──
    auto* trendCard = new CardFrame;
    auto* tl = new QVBoxLayout(trendCard);
    tl->setContentsMargins(14, 12, 14, 12);
    tl->setSpacing(8);
    auto* trendHead = new QHBoxLayout;
    auto* trendTitle = new QLabel(QStringLiteral("每日 Token 趋势图"));
    trendTitle->setObjectName(QStringLiteral("cardTitle"));
    trendHead->addWidget(trendTitle);
    trendHead->addStretch(1);
    auto* winRow = new QWidget;
    auto* wr = new QHBoxLayout(winRow);
    wr->setContentsMargins(0, 0, 0, 0);
    wr->setSpacing(0);
    m_windowGroup = new QButtonGroup(this);
    m_windowGroup->setExclusive(true);
    const struct { const char* label; int days; } windows[] = {
        { "近 7 日", 7 }, { "近 30 日", 30 },
    };
    for (const auto& w : windows) {
        auto* b = new QPushButton(QString::fromUtf8(w.label));
        b->setCheckable(true);
        b->setProperty("cls", "on");
        b->setCursor(Qt::PointingHandCursor);
        b->setChecked(w.days == 7);
        m_windowGroup->addButton(b, w.days);
        wr->addWidget(b);
    }
    connect(m_windowGroup, &QButtonGroup::idClicked, this,
            &UsageStatsPage::onWindowChanged);
    trendHead->addWidget(winRow);
    tl->addLayout(trendHead);
    m_trend = new MiniChart;
    m_trend->setMinimumHeight(280);
    tl->addWidget(m_trend, 1);
    outer->addWidget(trendCard);
    outer->addSpacing(12);

    // ── 模型用量 (donut + legend) ──
    auto* shareCard = new CardFrame;
    shareCard->setMinimumHeight(360);
    auto* sl = new QHBoxLayout(shareCard);
    sl->setContentsMargins(18, 16, 18, 16);
    sl->setSpacing(24);
    m_donut = new UsageDonut;
    m_donut->setMinimumSize(340, 340);
    sl->addWidget(m_donut, 5);
    auto* legendHost = new QWidget;
    auto* ll = new QVBoxLayout(legendHost);
    ll->setContentsMargins(0, 0, 0, 0);
    // 图例用行悬停表格：条目多时鼠标所在行整行高亮，一眼能对上
    m_legendTable = new RowHoverTable;
    m_legendTable->setColumnCount(4);
    m_legendTable->setHorizontalHeaderLabels(
        {QStringLiteral(""), QStringLiteral("模型"), QStringLiteral("用量"),
         QStringLiteral("占比")});
    m_legendTable->verticalHeader()->hide();
    m_legendTable->horizontalHeader()->hide();
    m_legendTable->setFrameShape(QFrame::NoFrame);
    m_legendTable->setShowGrid(false);
    m_legendTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_legendTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_legendTable->setFocusPolicy(Qt::NoFocus);
    m_legendTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_legendTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ll->addWidget(m_legendTable);
    sl->addWidget(legendHost, 5);
    outer->addWidget(shareCard);
    outer->addSpacing(12);

    // ── 按 agent 分布（累计口径，填满页尾；与页首全局筛选联动）──
    auto* agentCard = new CardFrame;
    auto* alc = new QVBoxLayout(agentCard);
    alc->setContentsMargins(14, 12, 14, 12);
    auto* alt = new QLabel(QStringLiteral("按 agent 分布"));
    alt->setObjectName(QStringLiteral("cardTitle"));
    alc->addWidget(alt);
    m_agentTable = new RowHoverTable;
    m_agentTable->setColumnCount(7);
    m_agentTable->setHorizontalHeaderLabels(
        {QStringLiteral("agent"), QStringLiteral("请求数"), QStringLiteral("输入"),
         QStringLiteral("输出"), QStringLiteral("推理"), QStringLiteral("总 token"),
         QStringLiteral("占比")});
    m_agentTable->verticalHeader()->hide();
    m_agentTable->horizontalHeader()->setStretchLastSection(true);
    m_agentTable->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    m_agentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_agentTable->setSelectionMode(QAbstractItemView::NoSelection);
    alc->addWidget(m_agentTable);
    outer->addWidget(agentCard);
    outer->addStretch(1);
}

void UsageStatsPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!m_loaded)
        reload();
    m_donut->animate(); // sweep-in every time the page opens
}

void UsageStatsPage::refreshFilters()
{
    // 只列出实际用过的 agent / 模型系列（来自归档数据），没用过的不显示。
    // 重建选项时保留当前选择。
    const QSet<QString> usedAgents = [] {
        QSet<QString> s;
        for (const QString& id : Store::instance().archiveAgentIds())
            s.insert(id);
        return s;
    }();
    // rebuild ONLY when the option list changed — an open dropdown must
    // not be torn down under the user every 2s poll
    QStringList agentOpts { QString() };
    for (AgentAdapter* a : Store::instance().adapters())
        if (usedAgents.contains(a->id()))
            agentOpts << a->id();
    QStringList currentOpts;
    for (int i = 0; i < m_filterAgent->count(); ++i)
        currentOpts << m_filterAgent->itemData(i).toString();
    if (currentOpts != agentOpts) {
        QSignalBlocker ba(m_filterAgent);
        const QString curAgent = m_filterAgent->currentData().toString();
        m_filterAgent->clear();
        m_filterAgent->addItem(QStringLiteral("全部 agent"), QString());
        for (const QString& id : agentOpts) {
            if (id.isEmpty())
                continue;
            if (AgentAdapter* a = Store::instance().adapter(id))
                m_filterAgent->addItem(a->displayName(), id);
        }
        const int idx = m_filterAgent->findData(curAgent);
        m_filterAgent->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    const QStringList famKeys = Store::instance().archiveModelFamilies();
    QStringList curKeys;
    for (int i = 0; i < m_filterModel->count(); ++i)
        curKeys << m_filterModel->itemData(i).toString();
    if (curKeys != famKeys) {
        QSignalBlocker bm(m_filterModel);
        const QString curFam = m_filterModel->currentData().toString();
        m_filterModel->clear();
        m_filterModel->addItem(QStringLiteral("全部模型"), QString());
        for (const QString& fam : famKeys)
            m_filterModel->addItem(Store::familyDisplay(fam), fam);
        const int idx = m_filterModel->findData(curFam);
        m_filterModel->setCurrentIndex(idx >= 0 ? idx : 0);
    }
}

void UsageStatsPage::reload()
{
    refreshFilters();
    const QString agent = m_filterAgent->currentData().toString();
    const QString family = m_filterModel->currentData().toString();
    const int seq = ++m_reloadSeq;
    struct Data {
        QVector<Store::DailyTotal> daily;
        QVector<Store::ModelDayPoint> byModel30;
        QVector<Store::AgentTotal> byAgent;
        qint64 longestSessionMs;
    };
    Async::run<Data>(
        this,
        [agent, family]() {
            Data d;
            d.daily = Store::instance().dailyTotals(agent, family);
            d.byModel30 = Store::instance().dailyTokensByModel(
                Store::windowSince(QStringLiteral("30d")), 8, agent, family);
            d.byAgent = Store::instance().agentTotals(agent, family);
            d.longestSessionMs =
                Store::instance().longestSessionActiveMs(agent, family);
            return d;
        },
        [this, seq](const Data& d) {
            if (seq != m_reloadSeq)
                return;
            m_daily = d.daily;
            m_byModel30 = d.byModel30;
            m_byAgent = d.byAgent;
            m_longestSessionMs = d.longestSessionMs;
            m_loaded = true;
            renderData();
        });
}

void UsageStatsPage::onModeChanged()
{
    if (!m_heatmap)
        return;
    m_heatmap->setMode(UsageHeatmap::Mode(m_modeGroup->checkedId()));
}

void UsageStatsPage::onWindowChanged()
{
    renderTrend();
    renderDonut();
}

void UsageStatsPage::renderData()
{
    const Palette& pal = Theme::instance().pal();

    // ── KPI cards (client-side aggregates over daily totals) ──
    qint64 total = 0, peak = 0, curStreak = 0, maxStreak = 0;
    QSet<qint64> activeDays;
    for (const Store::DailyTotal& d : m_daily) {
        total += d.tokens;
        peak = qMax(peak, d.tokens);
        if (d.tokens > 0)
            activeDays.insert(d.dayMs);
    }
    const qint64 dayMs = 86400000LL;
    const qint64 today = QDateTime(QDate::currentDate(), QTime(0, 0))
                             .toMSecsSinceEpoch();
    qint64 walk = activeDays.contains(today) ? today : today - dayMs;
    while (activeDays.contains(walk)) {
        ++curStreak;
        walk -= dayMs;
    }
    qint64 run = 0, prev = 0;
    for (const Store::DailyTotal& d : m_daily) {
        if (d.tokens <= 0)
            continue;
        run = (prev != 0 && d.dayMs - prev == dayMs) ? run + 1 : 1;
        maxStreak = qMax(maxStreak, run);
        prev = d.dayMs;
    }

    // KPI count-up：首次渲染数字从 0 滚到目标值，之后直接落定
    m_tTotal = total;
    m_tPeak = peak;
    m_tLongest = m_longestSessionMs;
    m_tStreak = curStreak;
    m_tMaxStreak = maxStreak;
    if (!m_counted) {
        m_counted = true;
        m_countProg = 0.0;
        for (KpiCard* c : { m_kTotal, m_kPeak, m_kLongest, m_kStreak, m_kMaxStreak })
            c->setFlashEnabled(false);
        m_countAnim.start();
    } else {
        m_countProg = 1.0;
    }
    updateKpiValues();

    renderHeatmap();
    renderTrend();
    renderDonut();
    renderAgentTable();
}

void UsageStatsPage::updateKpiValues()
{
    const Palette& pal = Theme::instance().pal();
    const double k = m_countProg;
    m_kTotal->setValue(Format::fmtNumCn(double(m_tTotal) * k), QColor());
    m_kTotal->setBar(-1, QColor());
    m_kTotal->setDetails(QString());
    m_kPeak->setValue(Format::fmtNumCn(double(m_tPeak) * k));
    m_kPeak->setDelta(QStringLiteral("单日最高"));
    m_kLongest->setValue(m_tLongest > 0
                             ? Format::fmtDur(m_tLongest / 1000.0 * k)
                             : QStringLiteral("—"));
    m_kLongest->setDelta(QStringLiteral("单会话使用时长"));
    const int streak = int(std::round(m_tStreak * k));
    m_kStreak->setValue(QString::number(streak) + QStringLiteral(" 天"),
                        streak > 0 ? pal.sevOk : QColor());
    m_kStreak->setDelta(QStringLiteral("到今天连续有用量"));
    const int maxStreak = int(std::round(m_tMaxStreak * k));
    m_kMaxStreak->setValue(QString::number(maxStreak) + QStringLiteral(" 天"));
    m_kMaxStreak->setDelta(QStringLiteral("历史最长连续"));
}

void UsageStatsPage::renderHeatmap()
{
    QVector<QPair<qint64, qint64>> daily;
    for (const Store::DailyTotal& d : m_daily)
        daily.push_back({ d.dayMs, d.tokens });
    m_heatmap->setData(daily);
    m_heatmap->setMode(UsageHeatmap::Mode(
        m_modeGroup->checkedId() >= 0 ? m_modeGroup->checkedId() : 0));
}

void UsageStatsPage::renderTrend()
{
    const int days = m_windowGroup->checkedId() > 0 ? m_windowGroup->checkedId() : 7;

    // calendar-day window ending today (zero-filled: a day with no requests
    // must still occupy its x slot, or "近7日" silently stretches)
    const qint64 dayMs = 86400000LL;
    const QDate today = QDate::currentDate();
    QVector<qint64> cols; // local-midnight day keys, oldest → newest
    for (int i = days - 1; i >= 0; --i)
        cols.push_back(QDateTime(today.addDays(-i), QTime(0, 0)).toMSecsSinceEpoch());
    QHash<qint64, int> colOfDay;
    QStringList labels;
    for (int i = 0; i < cols.size(); ++i) {
        colOfDay.insert(cols[i], i);
        labels << Format::fmtDayTick(cols[i]);
    }

    // rank models by window total, top 8 — same ranking/colors as the donut
    const qint64 cutoff = cols.first();
    QHash<QString, qint64> totals;
    for (const auto& p : m_byModel30)
        if (p.dayMs >= cutoff)
            totals.insert(p.model, totals.value(p.model) + p.genTok);
    QVector<QPair<QString, qint64>> ranked;
    for (auto it = totals.constBegin(); it != totals.constEnd(); ++it)
        ranked.push_back({ it.key(), it.value() });
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    const Palette& pal = Theme::instance().pal();
    QVector<MiniChart::Series> list;
    for (int i = 0; i < ranked.size() && i < 8; ++i) {
        MiniChart::Series s;
        s.name = ranked[i].first;
        s.color = sliceColor(i, pal);
        s.fillAlpha = 0.05;
        s.lineWidth = 1.8;
        s.values.resize(cols.size());
        list << s;
    }
    for (const auto& p : m_byModel30) {
        const int col = colOfDay.value(p.dayMs, -1);
        if (col < 0)
            continue;
        for (MiniChart::Series& s : list)
            if (s.name == p.model)
                s.values[col] = double(p.genTok);
    }
    if (list.isEmpty() || labels.isEmpty())
        m_trend->setEmpty(QStringLiteral("时间范围内无数据"));
    else
        m_trend->setLines(labels, list, QStringLiteral("token/天"),
                          MiniChart::YNum);
}

void UsageStatsPage::renderDonut()
{
    const int days = m_windowGroup->checkedId() > 0 ? m_windowGroup->checkedId() : 7;
    const qint64 cutoff = QDateTime(QDate::currentDate().addDays(-(days - 1)),
                                    QTime(0, 0)).toMSecsSinceEpoch();
    // aggregate the 30-day per-model series over the selected calendar window
    QHash<QString, qint64> byModel;
    for (const auto& p : m_byModel30)
        if (p.dayMs >= cutoff)
            byModel.insert(p.model, byModel.value(p.model) + p.genTok);
    QVector<QPair<QString, qint64>> ranked;
    for (auto it = byModel.constBegin(); it != byModel.constEnd(); ++it)
        ranked.push_back({ it.key(), it.value() });
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    const Palette& pal = Theme::instance().pal();
    QVector<UsageDonut::Slice> slices;
    for (int i = 0; i < ranked.size() && i < 8; ++i)
        slices.push_back({ ranked[i].first, ranked[i].second,
                           sliceColor(i, pal) });
    m_donut->setData(slices);

    // legend：行悬停表格，用量与占比右对齐，多行时整行高亮不迷路
    m_legendTable->setRowCount(int(ranked.size()));
    qint64 total = 0;
    for (const auto& r : ranked)
        total += r.second;
    for (int i = 0; i < ranked.size(); ++i) {
        const auto& r = ranked[i];
        const QColor color = sliceColor(i, pal);

        auto* dot = new QWidget;
        dot->setFixedSize(10, 10);
        dot->setAttribute(Qt::WA_NoSystemBackground);
        dot->setStyleSheet(
            QStringLiteral("background:%1;border-radius:5px;").arg(color.name()));
        m_legendTable->setCellWidget(i, 0, dot);

        auto* name = new QTableWidgetItem(r.first);
        name->setFlags(Qt::ItemIsEnabled);
        name->setFont(QFont(QStringLiteral("Consolas"), 10));
        name->setForeground(pal.fg1);
        m_legendTable->setItem(i, 1, name);

        auto* tokens = new QTableWidgetItem(Format::fmtNumCn(double(r.second))
                                            + QStringLiteral(" tokens"));
        tokens->setFlags(Qt::ItemIsEnabled);
        tokens->setForeground(pal.fg3);
        tokens->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_legendTable->setItem(i, 2, tokens);

        const double pct = total > 0 ? double(r.second) * 100.0 / double(total) : 0.0;
        auto* pctItem = new QTableWidgetItem(QString::number(pct, 'f', 1) + '%');
        pctItem->setFlags(Qt::ItemIsEnabled);
        pctItem->setFont(QFont(QStringLiteral("Consolas"), 10));
        pctItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_legendTable->setItem(i, 3, pctItem);
    }
    m_legendTable->resizeColumnsToContents();
    m_legendTable->setColumnWidth(0, 22); // color dot column
    m_legendTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_legendTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_legendTable->setFixedHeight(UiUtil::tableContentHeight(m_legendTable, 420));
}

// 按 agent 分布（累计口径，与页首全局筛选联动；比例条 + 占比与
// 实时监控页的「按 agent」表同一套语言）
void UsageStatsPage::renderAgentTable()
{
    const Palette& pal = Theme::instance().pal();
    double maxIn = 0, maxOut = 0, maxReason = 0, maxTot = 0;
    qint64 grand = 0;
    for (const Store::AgentTotal& t : m_byAgent) {
        maxIn = qMax(maxIn, double(t.inTok));
        maxOut = qMax(maxOut, double(t.outTok));
        maxReason = qMax(maxReason, double(t.reasonTok));
        maxTot = qMax(maxTot, double(t.inTok + t.outTok + t.reasonTok));
        grand += t.inTok + t.outTok + t.reasonTok;
    }
    m_agentTable->setColumnMax(2, maxIn);
    m_agentTable->setColumnMax(3, maxOut);
    m_agentTable->setColumnMax(4, maxReason);
    m_agentTable->setColumnMax(5, maxTot);
    m_agentTable->setColumnMax(6, 100.0); // 占比：条长即占比

    m_agentTable->setRowCount(int(m_byAgent.size()));
    for (int i = 0; i < m_byAgent.size(); ++i) {
        const Store::AgentTotal& t = m_byAgent[i];
        const qint64 tot = t.inTok + t.outTok + t.reasonTok;
        auto* name = txtItem(t.agent);
        name->setFont(QFont(QStringLiteral("Consolas"), 10));
        if (AgentAdapter* a = Store::instance().adapter(t.agent))
            name->setForeground(a->color());
        m_agentTable->setItem(i, 0, name);
        m_agentTable->setItem(i, 1, numItem(Format::fmtInt(double(t.requests))));
        m_agentTable->setItem(i, 2, numItem(Format::fmtNum(double(t.inTok)),
                                            double(t.inTok)));
        m_agentTable->setItem(i, 3, numItem(Format::fmtNum(double(t.outTok)),
                                            double(t.outTok)));
        m_agentTable->setItem(i, 4, numItem(Format::fmtNum(double(t.reasonTok)),
                                            double(t.reasonTok)));
        m_agentTable->setItem(i, 5, numItem(Format::fmtNumCn(double(tot)),
                                            double(tot)));
        const double share = grand ? double(tot) * 100.0 / double(grand) : 0.0;
        m_agentTable->setItem(i, 6,
            numItem(QString::number(share, 'f', 1) + QStringLiteral("%"), share));
    }
    m_agentTable->resizeColumnsToContents();
    m_agentTable->horizontalHeader()->setSectionResizeMode(
        m_agentTable->columnCount() - 1, QHeaderView::Stretch);
    m_agentTable->setFixedHeight(UiUtil::tableContentHeight(m_agentTable, 420));
}
