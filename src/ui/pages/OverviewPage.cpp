// OverviewPage.cpp — see OverviewPage.h.

#include "OverviewPage.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "ui/widgets/KpiCard.h"
#include "ui/widgets/LiveFeed.h"
#include "ui/widgets/MiniChart.h"
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

void tint(QTableWidgetItem* it, const QColor& c)
{
    it->setForeground(c);
}

QColor speedTierColor(double tps)
{
    return UiUtil::speedColor(Format::speedTier(tps));
}

QString windowLabel(const QString& window)
{
    if (window == QLatin1String("today")) return QStringLiteral("今日");
    if (window == QLatin1String("7d")) return QStringLiteral("7 天");
    if (window == QLatin1String("30d")) return QStringLiteral("30 天");
    if (window == QLatin1String("all")) return QStringLiteral("全部");
    return QStringLiteral("24 小时");
}

} // namespace

OverviewPage::OverviewPage(QWidget* parent)
    : QWidget(parent)
{
    connect(&Store::instance(), &Store::newRecords, this,
            &OverviewPage::onNewRecords, Qt::QueuedConnection);
    connect(&Store::instance(), &Store::dataChanged, this,
            &OverviewPage::onDataChanged, Qt::QueuedConnection);
    connect(&Store::instance(), &Store::scanFinished, this,
            &OverviewPage::onScanFinished, Qt::QueuedConnection);

    connect(&Theme::instance(), &Theme::changed, this, [this](bool) { renderData(); });

    buildUi();
}

void OverviewPage::buildUi()
{
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    content->setMaximumWidth(1400);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(24, 20, 24, 64);
    outer->setSpacing(0);
    auto* center = new QHBoxLayout;
    center->setContentsMargins(0, 0, 0, 0);
    center->addStretch(1);
    center->addWidget(content, 10);
    center->addStretch(1);
    auto* centerHost = new QWidget;
    centerHost->setLayout(center);
    scroll->setWidget(centerHost);
    rootLay->addWidget(scroll);

    // ── toolbar ──
    auto* toolbar = new QWidget;
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(0, 0, 0, 12);
    tb->setSpacing(10);
    auto* h1 = new QLabel(QStringLiteral("实时监控"));
    h1->setObjectName(QStringLiteral("h1"));
    auto* sub = new QLabel(QStringLiteral("所有 agent 的模型调用 · token 消耗 · 速度"));
    sub->setProperty("cls", "muted");
    sub->setStyleSheet("font-size:9pt;");
    tb->addWidget(h1);
    tb->addWidget(sub);
    tb->addStretch(1);
    m_windowCombo = new QComboBox;
    m_windowCombo->addItem(QStringLiteral("今天"), QStringLiteral("today"));
    m_windowCombo->addItem(QStringLiteral("近 24 小时"), QStringLiteral("24h"));
    m_windowCombo->addItem(QStringLiteral("近 7 天"), QStringLiteral("7d"));
    m_windowCombo->addItem(QStringLiteral("近 30 天"), QStringLiteral("30d"));
    m_windowCombo->addItem(QStringLiteral("全部"), QStringLiteral("all"));
    m_windowCombo->setCurrentIndex(1);
    connect(m_windowCombo, &QComboBox::currentIndexChanged, this, &OverviewPage::reload);
    tb->addWidget(m_windowCombo);
    m_refreshBtn = new QPushButton(QStringLiteral("↻ 刷新"));
    m_refreshBtn->setProperty("cls", "ghost");
    connect(m_refreshBtn, &QPushButton::clicked, this, &OverviewPage::reload);
    tb->addWidget(m_refreshBtn);
    outer->addWidget(toolbar);
    outer->addWidget(UiUtil::gradientDivider());

    // ── KPI 区：6 列；总 token（左/右）各独占两行 ──
    auto* kpiHost = new QWidget;
    m_kpiGrid = new QGridLayout(kpiHost);
    m_kpiGrid->setContentsMargins(0, 0, 0, 0);
    m_kpiGrid->setSpacing(10);
    for (int c = 0; c < 6; ++c)
        m_kpiGrid->setColumnStretch(c, 1);
    m_kTodayTotal = new KpiCard(QStringLiteral("总 token (今日)"));
    m_kTodayTotal->setLargeValue(true);
    m_kRequests = new KpiCard(QStringLiteral("模型请求"));
    m_kAvgDur = new KpiCard(QStringLiteral("平均流式时长"));
    m_kIn = new KpiCard(QStringLiteral("输入 token"));
    m_kOut = new KpiCard(QStringLiteral("输出 token"));
    m_kTotal = new KpiCard(QStringLiteral("总 token"));
    m_kTotal->setLargeValue(true);
    m_kReason = new KpiCard(QStringLiteral("推理 token 占比"));
    m_kCache = new KpiCard(QStringLiteral("缓存命中"));
    m_kSessions = new KpiCard(QStringLiteral("活跃会话"));
    m_kErrors = new KpiCard(QStringLiteral("错误 / 中止"));
    m_kSpeed = new KpiCard(QStringLiteral("平均 Token 速度"));
    m_kpiGrid->addWidget(m_kTodayTotal, 0, 0, 2, 1);
    m_kpiGrid->addWidget(m_kRequests, 0, 1);
    m_kpiGrid->addWidget(m_kAvgDur, 0, 2);
    m_kpiGrid->addWidget(m_kIn, 0, 3);
    m_kOut->setMinimumWidth(150);
    m_kpiGrid->addWidget(m_kOut, 0, 4);
    m_kpiGrid->addWidget(m_kTotal, 0, 5, 2, 1);
    m_kpiGrid->addWidget(m_kReason, 1, 1);
    m_kpiGrid->addWidget(m_kCache, 1, 2);
    m_kpiGrid->addWidget(m_kSessions, 1, 3);
    m_kpiGrid->addWidget(m_kErrors, 1, 4);
    m_kpiGrid->setRowStretch(0, 1);
    m_kpiGrid->setRowStretch(1, 1);
    outer->addWidget(kpiHost);

    // 平均速度独占一行（宽卡），与老版 zcode-monitor 同布局
    m_kSpeed->setMinimumHeight(110);
    outer->addWidget(m_kSpeed);

    // ── 趋势 ──
    {
        auto* row = sectionLabel(QStringLiteral("趋势"));
        m_seriesRange = row->findChild<QLabel*>(QStringLiteral("sectionSub"));
        outer->addWidget(row);
    }
    auto* card1 = new CardFrame;
    auto* c1 = new QVBoxLayout(card1);
    c1->setContentsMargins(14, 12, 14, 12);
    auto* t1 = new QLabel(QStringLiteral("模型请求 / 时间桶"));
    t1->setObjectName(QStringLiteral("cardTitle"));
    c1->addWidget(t1);
    m_chCalls = new MiniChart;
    m_chCalls->setMinimumHeight(300);
    c1->addWidget(m_chCalls, 1);
    outer->addWidget(card1);
    outer->addSpacing(12);

    auto* card2 = new CardFrame;
    auto* c2 = new QVBoxLayout(card2);
    c2->setContentsMargins(14, 12, 14, 12);
    auto* t2 = new QLabel(QStringLiteral("Token 构成（输入 / 输出 / 推理）"));
    t2->setObjectName(QStringLiteral("cardTitle"));
    c2->addWidget(t2);
    m_chTokens = new MiniChart;
    m_chTokens->setMinimumHeight(300);
    c2->addWidget(m_chTokens, 1);
    outer->addWidget(card2);

    // ── Token 速度 ──
    outer->addWidget(sectionLabel(QStringLiteral("Token 速度"),
                                  QStringLiteral("tokens/sec · 每次完成的模型响应")));
    auto* cardS1 = new CardFrame;
    auto* s1 = new QVBoxLayout(cardS1);
    s1->setContentsMargins(14, 12, 14, 12);
    auto* st1 = new QLabel(QStringLiteral("速度随时间"));
    st1->setObjectName(QStringLiteral("cardTitle"));
    s1->addWidget(st1);
    m_chSpeed = new MiniChart;
    m_chSpeed->setMinimumHeight(300);
    s1->addWidget(m_chSpeed, 1);
    outer->addWidget(cardS1);
    outer->addSpacing(12);

    auto* cardS2 = new CardFrame;
    auto* s2 = new QVBoxLayout(cardS2);
    s2->setContentsMargins(0, 0, 0, 0);
    s2->setSpacing(0);
    auto* st2row = new QHBoxLayout;
    auto* st2 = new QLabel(QStringLiteral("最近请求速度"));
    st2->setObjectName(QStringLiteral("cardTitle"));
    st2->setContentsMargins(14, 12, 0, 0);
    st2row->addWidget(st2);
    st2row->addStretch(1);
    m_speedModelFilter = new QComboBox;
    m_speedModelFilter->setToolTip(QStringLiteral(
        "按模型大类筛选（大类包含其全部具体型号）"));
    connect(m_speedModelFilter, &QComboBox::currentIndexChanged, this,
            &OverviewPage::onSpeedFilterChanged);
    st2row->addWidget(m_speedModelFilter);
    st2row->setContentsMargins(0, 0, 14, 0);
    s2->addLayout(st2row);
    auto* speedWrap = new QWidget;
    auto* sw = new QVBoxLayout(speedWrap);
    sw->setContentsMargins(0, 0, 0, 0);
    m_speedTable = new RowHoverTable;
    m_speedTable->setColumnCount(7);
    m_speedTable->setHorizontalHeaderLabels({QStringLiteral("时间"), QStringLiteral("Agent"),
                                              QStringLiteral("模型"), QStringLiteral("输出"),
                                              QStringLiteral("推理"), QStringLiteral("时长"),
                                              QStringLiteral("速度")});
    m_speedTable->verticalHeader()->hide();
    m_speedTable->horizontalHeader()->setStretchLastSection(true);
    m_speedTable->setAlternatingRowColors(true); // zebra rows
    m_speedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_speedTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_speedTable->setMinimumHeight(300);
    sw->addWidget(m_speedTable);
    // 页脚：加权均速 / 总 token / 请求数（与老版 zcode-monitor 对齐）
    m_speedFoot = new QWidget;
    auto* sf = new QHBoxLayout(m_speedFoot);
    sf->setContentsMargins(12, 9, 12, 9);
    sf->setSpacing(16);
    for (int i = 0; i < 4; ++i) {
        auto* l = new QLabel;
        l->setContentsMargins(0, 0, 0, 0);
        sf->addWidget(l, 1);
    }
    sw->addWidget(m_speedFoot);
    speedWrap->setMinimumHeight(360);
    s2->addWidget(speedWrap, 1);
    outer->addWidget(cardS2);

    // ── 实时活动 ──
    {
        auto* row = sectionLabel(QStringLiteral("实时活动"),
                                 QStringLiteral("新完成的模型响应（2s 轮询）"));
        m_liveStatus = new BadgeLabel(QStringLiteral("扫描中…"), QStringLiteral("dim"));
        static_cast<QBoxLayout*>(row->layout())->addWidget(m_liveStatus);
        outer->addWidget(row);
    }
    auto* feedCard = new CardFrame;
    auto* fc = new QVBoxLayout(feedCard);
    fc->setContentsMargins(4, 4, 4, 4);
    m_feed = new LiveFeed;
    m_feed->setMinimumHeight(160);
    m_feed->setMaximumHeight(400);
    fc->addWidget(m_feed);
    outer->addWidget(feedCard);

    // ── 算力分布 ──
    outer->addWidget(sectionLabel(QStringLiteral("算力分布"), QStringLiteral("花在哪")));
    auto* bmCard = new CardFrame;
    auto* bmc = new QVBoxLayout(bmCard);
    bmc->setContentsMargins(14, 12, 14, 12);
    auto* bmt = new QLabel(QStringLiteral("按模型"));
    bmt->setObjectName(QStringLiteral("cardTitle"));
    bmc->addWidget(bmt);
    m_byModelTable = new RowHoverTable;
    m_byModelTable->setColumnCount(8);
    m_byModelTable->setHorizontalHeaderLabels(
        {QStringLiteral("agent / 模型"), QStringLiteral("请求数"), QStringLiteral("输入"),
         QStringLiteral("缓存"), QStringLiteral("输出"), QStringLiteral("推理"),
         QStringLiteral("均时长"), QStringLiteral("速度")});
    m_byModelTable->verticalHeader()->hide();
    m_byModelTable->horizontalHeader()->setStretchLastSection(true);
    m_byModelTable->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    m_byModelTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_byModelTable->setSelectionMode(QAbstractItemView::NoSelection);
    bmc->addWidget(m_byModelTable);
    outer->addWidget(bmCard);
    outer->addSpacing(12);

    auto* baCard = new CardFrame;
    auto* bac = new QVBoxLayout(baCard);
    bac->setContentsMargins(14, 12, 14, 12);
    auto* bat = new QLabel(QStringLiteral("按 agent"));
    bat->setObjectName(QStringLiteral("cardTitle"));
    bac->addWidget(bat);
    m_byAgentTable = new RowHoverTable;
    m_byAgentTable->setColumnCount(7);
    m_byAgentTable->setHorizontalHeaderLabels(
        {QStringLiteral("agent"), QStringLiteral("请求数"), QStringLiteral("输入"),
         QStringLiteral("输出"), QStringLiteral("推理"), QStringLiteral("总 token"),
         QStringLiteral("占比")});
    m_byAgentTable->verticalHeader()->hide();
    m_byAgentTable->horizontalHeader()->setStretchLastSection(true);
    m_byAgentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_byAgentTable->setSelectionMode(QAbstractItemView::NoSelection);
    bac->addWidget(m_byAgentTable);
    outer->addWidget(baCard);
    outer->addSpacing(12);

    auto* btCard = new CardFrame;
    auto* btc = new QVBoxLayout(btCard);
    btc->setContentsMargins(14, 12, 14, 12);
    auto* btt = new QLabel(QStringLiteral("按工具"));
    btt->setObjectName(QStringLiteral("cardTitle"));
    btc->addWidget(btt);
    m_byToolTable = new RowHoverTable;
    m_byToolTable->setColumnCount(4);
    m_byToolTable->setHorizontalHeaderLabels(
        {QStringLiteral("agent / 工具"), QStringLiteral("调用次数"),
         QStringLiteral("涉及会话"), QStringLiteral("")});
    m_byToolTable->verticalHeader()->hide();
    m_byToolTable->horizontalHeader()->setStretchLastSection(true);
    m_byToolTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_byToolTable->setSelectionMode(QAbstractItemView::NoSelection);
    btc->addWidget(m_byToolTable);
    outer->addWidget(btCard);
    outer->addStretch(1);
}

void OverviewPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    reload(); // 轮询生命周期在 MainWindow（显示即扫、最小化暂停）
}

void OverviewPage::reload()
{
    const QString w = m_windowCombo->currentData().toString();
    const int seq = ++m_reloadSeq;
    Async::run<types::OverviewData>(
        this,
        [w]() { return Store::instance().overview(w); },
        [this, seq](const types::OverviewData& d) {
            if (seq != m_reloadSeq)
                return;
            // no fingerprint skip: renders are batched (setUpdatesEnabled)
            // and value-animations only restart on real changes, so a full
            // re-render is visually free — and a skip could never again
            // strand the page in a transient empty state
            m_data = d;
            m_loaded = true;
            renderData();
        });
}

void OverviewPage::onDataChanged()
{
    reload();
}

void OverviewPage::onNewRecords(const QVector<types::UsageRecord>& records)
{
    m_feed->feedModel()->pushRecords(records);
}

void OverviewPage::onScanFinished(bool ok, const QString& err, int, int)
{
    if (!m_liveStatus)
        return;
    if (ok) {
        m_liveStatus->setText(QStringLiteral("已连接"));
        m_liveStatus->setKind(QStringLiteral("green"));
    } else {
        m_liveStatus->setText(QStringLiteral("扫描失败：") + err.left(40));
        m_liveStatus->setKind(QStringLiteral("red"));
    }
}

void OverviewPage::renderData()
{
    if (!m_loaded)
        return;
    const types::Kpis& k = m_data.kpis;
    const Palette& pal = Theme::instance().pal();

    // 速度表模型大类筛选：只在实际出现的大类中重建（避免每 2s 拆掉
    // 用户正打开的下拉），默认「全部」
    if (m_speedModelFilter) {
        QSet<QString> fams;
        for (const types::ModelBreakdown& m : m_data.byModel) {
            const QString f = Store::modelFamily(m.model);
            if (!f.isEmpty())
                fams.insert(f);
        }
        QStringList cur;
        for (int i = 0; i < m_speedModelFilter->count(); ++i)
            cur << m_speedModelFilter->itemData(i).toString();
        QStringList want { QString() };
        for (const QString& f : fams)
            want << f;
        if (cur != want) {
            QSignalBlocker b(m_speedModelFilter);
            const QString keep = m_speedModelFilter->currentData().toString();
            m_speedModelFilter->clear();
            m_speedModelFilter->addItem(QStringLiteral("全部模型"), QString());
            for (const QString& f : want)
                if (!f.isEmpty())
                    m_speedModelFilter->addItem(Store::familyDisplay(f), f);
            const int idx = m_speedModelFilter->findData(keep);
            m_speedModelFilter->setCurrentIndex(idx >= 0 ? idx : 0);
        }
    }

    // tables rebuild from scratch — suppress repaints so live polling does
    // not flicker the page (and does not kill the user's row hover)
    setUpdatesEnabled(false);

    // ── KPI cards ──
    m_kRequests->setCaption(QStringLiteral("模型请求 (%1)").arg(windowLabel(m_data.window)));
    m_kSpeed->setCaption(QStringLiteral("平均 Token 速度 (%1)").arg(windowLabel(m_data.window)));
    m_kRequests->setValue(Format::fmtInt(double(k.requests)), pal.accent);
    m_kRequests->setDelta(QStringLiteral("覆盖 %1 个会话").arg(Format::fmtInt(double(k.sessions))));

    m_kAvgDur->setValue(k.avgDurOk ? Format::fmtMs(k.avgDurMs) : QStringLiteral("—"));
    m_kAvgDur->setDelta(QStringLiteral("%1 次可测流式").arg(Format::fmtInt(double(k.tpsSamples))));

    const int cacheRate = k.inTok
        ? qMin(100, qRound(double(k.cacheRead) * 100.0 / double(k.inTok)))
        : 0;
    m_kIn->setValue(Format::fmtNum(double(k.inTok)),
                    k.inTok >= 100000000 ? pal.sevErr : pal.sevOk);
    m_kIn->setBar(cacheRate, pal.catTool2);
    m_kIn->setDelta(QStringLiteral("缓存命中 %1% · 写入 %2")
                        .arg(cacheRate)
                        .arg(Format::fmtNum(double(k.cacheWrite))));

    m_kOut->setValue(Format::fmtNum(double(k.outTok)),
                     k.outTok >= 100000000 ? pal.sevErr : QColor());
    m_kOut->setDelta(QStringLiteral("模型实际生成"));

    // 今日大卡（窗口 ≠ today 时单独取固定 today 数据）
    if (m_data.window == QLatin1String("today")) {
        renderTotalCard(m_kTodayTotal, m_data);
    } else {
        types::OverviewData today;
        today.window = QStringLiteral("today");
        today.kpis = m_data.todayKpis;
        today.byModel = m_data.todayByModel;
        renderTotalCard(m_kTodayTotal, today);
    }
    renderTotalCard(m_kTotal, m_data);

    QString reasonVal = QStringLiteral("—");
    QColor reasonColor;
    const qint64 outAll = k.outTok + k.reasonTok;
    if (outAll > 0) {
        const double ratio = double(k.reasonTok) / double(outAll);
        reasonVal = QString::number(ratio * 100, 'f', 1) + '%';
        reasonColor = ratio > 0.3 ? pal.accent2 : (ratio > 0.05 ? pal.catTool2 : pal.accent);
    }
    m_kReason->setValue(reasonVal, reasonColor);
    m_kReason->setDelta(QStringLiteral("推理 %1 / 生成 %2")
                            .arg(Format::fmtNum(double(k.reasonTok)),
                                 Format::fmtNum(double(outAll))));

    m_kCache->setValue(k.inTok ? QString::number(cacheRate) + '%' : QStringLiteral("—"),
                       cacheRate >= 50 ? pal.sevOk : QColor());
    m_kCache->setDelta(QStringLiteral("读 %1 / 输入 %2")
                           .arg(Format::fmtNum(double(k.cacheRead)),
                                Format::fmtNum(double(k.inTok))));

    m_kSessions->setValue(Format::fmtInt(double(k.sessions)));
    m_kSessions->setDelta(QStringLiteral("窗口内有模型请求"));

    const double errRate = k.requests
        ? double(k.errors) / double(k.requests) * 100.0 : 0.0;
    m_kErrors->setValue(QString::number(errRate, 'f', 1) + '%',
                        errRate > 5 ? pal.sevErr : QColor());
    m_kErrors->setDelta(QStringLiteral("%1 次 / %2 请求")
                            .arg(Format::fmtInt(double(k.errors)),
                                 Format::fmtInt(double(k.requests))));

    m_kSpeed->setValue(k.tpsOk
                            ? QString::number(k.weightedTps, 'f', 1) + QStringLiteral(" t/s")
                            : QStringLiteral("—"),
                       UiUtil::speedColor(Format::speedTier(k.tpsOk ? k.weightedTps : qQNaN())));
    m_kSpeed->setDelta(QStringLiteral("加权 Σtoken÷Σ秒 · 样本 %1")
                           .arg(Format::fmtInt(double(k.tpsSamples))));
    m_kSpeed->setBar(-1, QColor());

    if (m_seriesRange)
        m_seriesRange->setText(QStringLiteral("窗口：%1").arg(windowLabel(m_data.window)));

    renderCharts();

    // ── speed table ──
    const auto& recent = m_data.recentSpeed;
    m_speedTable->setRowCount(int(recent.size()));
    for (int i = 0; i < recent.size(); ++i) {
        const types::SpeedRow& r = recent[i];
        m_speedTable->setItem(i, 0, txtItem(Format::fmtTime(r.timeMs)));
        auto* ag = txtItem(r.agentId);
        if (AgentAdapter* a = Store::instance().adapter(r.agentId))
            tint(ag, a->color());
        m_speedTable->setItem(i, 1, ag);
        auto* mod = txtItem(r.model.isEmpty() ? QStringLiteral("?") : r.model);
        mod->setFont(QFont(QStringLiteral("Consolas"), 9));
        m_speedTable->setItem(i, 2, mod);
        m_speedTable->setItem(i, 3, numItem(Format::fmtInt(double(r.output))));
        auto* rs = numItem(Format::fmtInt(double(r.reasoning)));
        if (r.reasoning == 0)
            tint(rs, pal.fg4);
        m_speedTable->setItem(i, 4, rs);
        m_speedTable->setItem(i, 5, numItem(Format::fmtMs(double(r.durationMs))));
        // 速度列：居中 + 加粗，阈值着色（<20 红 / >100 绿）
        auto* spd = new QTableWidgetItem(r.tpsOk
                    ? QString::number(r.tps, 'f', 1) + QStringLiteral(" t/s")
                    : QStringLiteral("—"));
        spd->setFlags(Qt::ItemIsEnabled);
        spd->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        QFont bold = spd->font();
        bold.setBold(true);
        spd->setFont(bold);
        tint(spd, r.tpsOk ? speedTierColor(r.tps) : pal.fg4);
        m_speedTable->setItem(i, 6, spd);
    }
    m_speedTable->resizeColumnsToContents();
    // 前几列留出呼吸空间，信息列不再挤在一起
    static const int kColMinW[] = { 96, 76, 130, 76, 76, 76 };
    for (int c = 0; c < 6 && c < m_speedTable->columnCount(); ++c)
        if (m_speedTable->columnWidth(c) < kColMinW[c])
            m_speedTable->setColumnWidth(c, kColMinW[c]);
    // 速度列占满剩余宽度，内容居中
    m_speedTable->horizontalHeader()->setSectionResizeMode(
        m_speedTable->columnCount() - 1, QHeaderView::Stretch);

    // footer: recompute from the same rows (加权均速，与老版对齐)
    const auto footLabels = m_speedFoot->findChildren<QLabel*>();
    if (recent.isEmpty()) {
        m_speedFoot->hide();
    } else {
        qint64 totTok = 0;
        double totSec = 0;
        QSet<QString> agents;
        for (const types::SpeedRow& r : recent) {
            totTok += r.output + r.reasoning;
            totSec += double(r.durationMs) / 1000.0;
            agents.insert(r.agentId);
        }
        const QString wTps = totSec > 0 ? QString::number(totTok / totSec, 'f', 1)
                                        : QStringLiteral("—");
        const QString texts[4] = {
            QStringLiteral("均速  %1 t/s").arg(wTps),
            QStringLiteral("总生成  %1").arg(Format::fmtInt(double(totTok))),
            QStringLiteral("请求  %1").arg(Format::fmtInt(double(recent.size()))),
            QStringLiteral("agent  %1").arg(agents.size()),
        };
        m_speedFoot->show();
        for (int i = 0; i < qMin(4, footLabels.size()); ++i) {
            footLabels[i]->setText(texts[i]);
            if (i == 0 && totSec > 0)
                // 均速是页脚主角：大号加粗 + 阈值色
                footLabels[i]->setStyleSheet(
                    QStringLiteral("color:%1;font-family:'Consolas';"
                                    "font-size:13pt;font-weight:700;")
                        .arg(speedTierColor(totTok / totSec).name()));
            else
                footLabels[i]->setStyleSheet(
                    QStringLiteral("color:%1;font-size:9.5pt;")
                        .arg(pal.fg3.name()));
        }
    }

    // ── by model table ──
    const auto& byModel = m_data.byModel;
    // 数值列比例条基准：以本列最大值为满格
    double maxIn = 0, maxOut = 0, maxReason = 0;
    for (const types::ModelBreakdown& m : byModel) {
        maxIn = qMax(maxIn, double(m.inTok));
        maxOut = qMax(maxOut, double(m.outTok));
        maxReason = qMax(maxReason, double(m.reasonTok));
    }
    m_byModelTable->setColumnMax(2, maxIn);
    m_byModelTable->setColumnMax(4, maxOut);
    m_byModelTable->setColumnMax(5, maxReason);
    m_byModelTable->setRowCount(int(byModel.size()));
    for (int i = 0; i < byModel.size(); ++i) {
        const types::ModelBreakdown& m = byModel[i];
        QString name = m.model.isEmpty() ? QStringLiteral("?") : m.model;
        if (!m.effort.isEmpty())
            name += QStringLiteral(" · ") + m.effort;
        auto* nameIt = txtItem(QStringLiteral("%1\n%2").arg(m.agentId, name));
        nameIt->setFont(QFont(QStringLiteral("Consolas"), 9));
        nameIt->setToolTip(name);
        if (AgentAdapter* a = Store::instance().adapter(m.agentId))
            tint(nameIt, a->color());
        m_byModelTable->setItem(i, 0, nameIt);
        m_byModelTable->setItem(i, 1, numItem(Format::fmtInt(double(m.requests))));
        m_byModelTable->setItem(i, 2, numItem(Format::fmtNum(double(m.inTok)),
                                               double(m.inTok)));
        m_byModelTable->setItem(i, 3, numItem(Format::fmtNum(double(m.cachedTok))));
        m_byModelTable->setItem(i, 4, numItem(Format::fmtNum(double(m.outTok)),
                                               double(m.outTok)));
        m_byModelTable->setItem(i, 5, numItem(Format::fmtNum(double(m.reasonTok)),
                                               double(m.reasonTok)));
        m_byModelTable->setItem(i, 6,
                                numItem(m.avgDurOk ? Format::fmtMs(m.avgDurMs) : QStringLiteral("—")));
        auto* spd = numItem(m.tpsOk ? QString::number(m.weightedTps, 'f', 1) + QStringLiteral(" t/s")
                                     : QStringLiteral("—"));
        if (m.tpsOk)
            tint(spd, speedTierColor(m.weightedTps));
        else
            tint(spd, pal.fg4);
        m_byModelTable->setItem(i, 7, spd);
    }
    m_byModelTable->resizeColumnsToContents();
    m_byModelTable->resizeRowsToContents();
    m_byModelTable->horizontalHeader()->setSectionResizeMode(
        m_byModelTable->columnCount() - 1, QHeaderView::Stretch);
    m_byModelTable->setFixedHeight(UiUtil::tableContentHeight(m_byModelTable, 900));

    // ── by agent table ──
    const auto& byAgent = m_data.byAgent;
    double maxAIn = 0, maxAOut = 0, maxAReason = 0, maxATot = 0;
    for (const types::AgentBreakdown& a : byAgent) {
        maxAIn = qMax(maxAIn, double(a.inTok));
        maxAOut = qMax(maxAOut, double(a.outTok));
        maxAReason = qMax(maxAReason, double(a.reasonTok));
        maxATot = qMax(maxATot, double(a.totalTokens()));
    }
    m_byAgentTable->setColumnMax(2, maxAIn);
    m_byAgentTable->setColumnMax(3, maxAOut);
    m_byAgentTable->setColumnMax(4, maxAReason);
    m_byAgentTable->setColumnMax(5, maxATot);
    m_byAgentTable->setColumnMax(6, 100.0); // 占比：条长即占比
    qint64 agentTokTotal = 0;
    for (const types::AgentBreakdown& a : byAgent)
        agentTokTotal += a.totalTokens();
    m_byAgentTable->setRowCount(int(byAgent.size()));
    for (int i = 0; i < byAgent.size(); ++i) {
        const types::AgentBreakdown& a = byAgent[i];
        auto* name = txtItem(a.agentId);
        name->setFont(QFont(QStringLiteral("Consolas"), 10));
        if (AgentAdapter* ad = Store::instance().adapter(a.agentId))
            tint(name, ad->color());
        m_byAgentTable->setItem(i, 0, name);
        m_byAgentTable->setItem(i, 1, numItem(Format::fmtInt(double(a.requests))));
        m_byAgentTable->setItem(i, 2, numItem(Format::fmtNum(double(a.inTok)),
                                              double(a.inTok)));
        m_byAgentTable->setItem(i, 3, numItem(Format::fmtNum(double(a.outTok)),
                                              double(a.outTok)));
        m_byAgentTable->setItem(i, 4, numItem(Format::fmtNum(double(a.reasonTok)),
                                              double(a.reasonTok)));
        m_byAgentTable->setItem(i, 5, numItem(Format::fmtNumCn(double(a.totalTokens())),
                                              double(a.totalTokens())));
        const double share = agentTokTotal
            ? double(a.totalTokens()) * 100.0 / double(agentTokTotal) : 0.0;
        m_byAgentTable->setItem(i, 6,
            numItem(QString::number(share, 'f', 1) + QStringLiteral("%"), share));
    }
    m_byAgentTable->resizeColumnsToContents();
    m_byAgentTable->horizontalHeader()->setSectionResizeMode(
        m_byAgentTable->columnCount() - 1, QHeaderView::Stretch);
    m_byAgentTable->setFixedHeight(UiUtil::tableContentHeight(m_byAgentTable, 620));

    // ── by tool table ──
    const auto& byTool = m_data.byTool;
    m_byToolTable->setRowCount(int(byTool.size()));
    for (int i = 0; i < byTool.size(); ++i) {
        const types::ToolBreakdown& t = byTool[i];
        auto* name = txtItem(QStringLiteral("%1 · %2").arg(t.agentId, t.toolName));
        name->setFont(QFont(QStringLiteral("Consolas"), 9));
        if (AgentAdapter* ad = Store::instance().adapter(t.agentId))
            tint(name, ad->color());
        m_byToolTable->setItem(i, 0, name);
        m_byToolTable->setItem(i, 1, numItem(Format::fmtInt(double(t.calls))));
        m_byToolTable->setItem(i, 2, numItem(Format::fmtInt(double(t.sessions))));
        m_byToolTable->setItem(i, 3, txtItem(QString()));
    }
    m_byToolTable->resizeColumnsToContents();
    m_byToolTable->horizontalHeader()->setSectionResizeMode(
        m_byToolTable->columnCount() - 1, QHeaderView::Stretch);
    m_byToolTable->setFixedHeight(UiUtil::tableContentHeight(m_byToolTable, 620));
    setUpdatesEnabled(true);
}

// 总 token 大卡（今日 / 所选窗口共用）：中文单位总量 + 生成占比 + 每模型彩色用量
void OverviewPage::renderTotalCard(KpiCard* card, const types::OverviewData& d)
{
    const types::Kpis& k = d.kpis;
    const Palette& pal = Theme::instance().pal();
    card->setCaption(QStringLiteral("总 token (%1)").arg(windowLabel(d.window)));

    const qint64 totalTok = k.inTok + k.outTok + k.reasonTok;
    QVector<QPair<QString, qint64>> perModel;
    for (const types::ModelBreakdown& m : d.byModel)
        perModel.push_back({ m.model.isEmpty() ? QStringLiteral("?") : m.model,
                             m.totalTokens() });
    std::sort(perModel.begin(), perModel.end(),
              [](const QPair<QString, qint64>& a, const QPair<QString, qint64>& b) {
                  return a.second > b.second;
              });

    if (totalTok > 0) {
        const double genPct = double(k.outTok + k.reasonTok) * 100.0 / double(totalTok);
        const bool hot = totalTok >= 100000000;
        card->setValue(Format::fmtNumCn(double(totalTok)),
                       hot ? pal.sevErr : QColor());
        card->setBar(qMin(100, qRound(genPct)), pal.accent);
        card->setDelta(QStringLiteral("生成占 %1% · 输入 %2")
                           .arg(QString::number(genPct, 'f', 1))
                           .arg(Format::fmtNumCn(double(k.inTok))));
        const QColor dotColors[] = {pal.catConversation, pal.catUsage, pal.catLlm2,
                                    pal.catTool2, pal.catLlm};
        QStringList lines;
        for (int i = 0; i < qMin(5, perModel.size()); ++i) {
            const QColor& dc = dotColors[i % 5];
            const bool mHot = perModel[i].second >= 100000000;
            const QString vc = mHot ? pal.sevErr.name() : pal.fg1.name();
            lines << QStringLiteral(
                "<span style='color:%1'>●</span> %2 <b style='color:%3'>%4</b>")
                .arg(dc.name(), perModel[i].first.toHtmlEscaped(),
                     vc, Format::fmtNumCn(double(perModel[i].second)));
        }
        if (perModel.size() > 5)
            lines << QStringLiteral("等 %1 个模型").arg(perModel.size());
        card->setDetails(lines.join(QStringLiteral("<br/>")));
    } else {
        card->setValue(QStringLiteral("—"));
        card->setBar(-1, QColor());
        card->setDelta(QStringLiteral("输入 + 输出 + 推理"));
        card->setDetails(QString());
    }
}

void OverviewPage::renderCharts()
{
    if (!m_loaded)
        return;
    const Palette& pal = Theme::instance().pal();

    // requests per bucket (bar); day-sized buckets get a date-only label —
    // an "08:00" time-of-day repeated on every tick is noise
    const bool dayBuckets = m_data.window == QLatin1String("30d")
                            || m_data.window == QLatin1String("all");
    {
        QStringList labels;
        QVector<double> values;
        for (const types::SeriesPoint& p : m_data.series) {
            labels << (dayBuckets ? Format::fmtDayTick(p.bucketMs)
                                  : Format::fmtTimeTick(p.bucketMs));
            values << double(p.requests);
        }
        if (labels.isEmpty())
            m_chCalls->setEmpty(QStringLiteral("无数据"));
        else
            m_chCalls->setBar(labels, values, pal.accent, QStringLiteral("请求数"),
                              MiniChart::YInt);
    }

    // token mix (multi-line with fills)
    {
        QStringList labels;
        for (const types::SeriesPoint& p : m_data.series)
            labels << (dayBuckets ? Format::fmtDayTick(p.bucketMs)
                                  : Format::fmtTimeTick(p.bucketMs));
        QVector<MiniChart::Series> series;
        MiniChart::Series in, out, reason;
        in.name = QStringLiteral("输入");
        in.color = pal.catUsage;
        in.fillAlpha = 0.10;
        out.name = QStringLiteral("输出");
        out.color = pal.accent;
        out.fillAlpha = 0.08;
        reason.name = QStringLiteral("推理");
        reason.color = pal.catLlm2;
        reason.fillAlpha = 0.10;
        for (const types::SeriesPoint& p : m_data.series) {
            in.values << double(p.input);
            out.values << double(p.output);
            reason.values << double(p.reasoning);
        }
        series << in << out << reason;
        if (labels.isEmpty())
            m_chTokens->setEmpty(QStringLiteral("无数据"));
        else
            m_chTokens->setLines(labels, series, QStringLiteral("token"), MiniChart::YNum);
    }

    // speed over time (per-point colors)
    {
        QVector<types::SpeedRow> rows;
        for (int i = m_data.recentSpeed.size() - 1; i >= 0; --i) { // oldest → newest
            if (m_data.recentSpeed[i].tpsOk)
                rows.push_back(m_data.recentSpeed[i]);
        }
        if (rows.isEmpty()) {
            m_chSpeed->setEmpty(QStringLiteral("窗口内无可测流式响应"));
        } else {
            QStringList labels;
            MiniChart::Series s;
            s.name = QStringLiteral("tok/s");
            s.color = pal.accent;
            s.showPoints = true;
            for (const types::SpeedRow& r : rows) {
                labels << Format::fmtTimeTick(r.timeMs);
                s.values << r.tps;
                s.pointColors << UiUtil::speedColor(Format::speedTier(r.tps));
            }
            m_chSpeed->setLines(labels, {s}, QStringLiteral("tok/s"), MiniChart::YTps);
        }
    }
}

void OverviewPage::onSpeedFilterChanged()
{
    if (!m_loaded)
        return;
    const QString family = m_speedModelFilter
        ? m_speedModelFilter->currentData().toString() : QString();
    const int seq = ++m_reloadSeq; // invalidate any in-flight full reloads
    Async::run<QVector<types::SpeedRow>>(
        this,
        [this, family]() {
            return Store::instance().recentSpeed(
                Store::windowSince(m_data.window), family, 60);
        },
        [this, seq](const QVector<types::SpeedRow>& rows) {
            if (seq != m_reloadSeq)
                return;
            m_data.recentSpeed = rows;
            renderData();
        });
}
