// ModelsPage.cpp — see ModelsPage.h.

#include "ModelsPage.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
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

} // namespace

ModelsPage::ModelsPage(QWidget* parent)
    : QWidget(parent)
{
    connect(&Theme::instance(), &Theme::changed, this, [this](bool) { renderData(); });
    buildUi();
}

void ModelsPage::buildUi()
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
    center->addStretch(1);
    center->addWidget(content, 10);
    center->addStretch(1);
    auto* centerHost = new QWidget;
    centerHost->setLayout(center);
    scroll->setWidget(centerHost);
    rootLay->addWidget(scroll);

    auto* toolbar = new QWidget;
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(0, 0, 0, 12);
    tb->setSpacing(10);
    auto* h1 = new QLabel(QStringLiteral("模型用量"));
    h1->setObjectName(QStringLiteral("h1"));
    m_sub = new QLabel(QStringLiteral("每个模型的 token 消耗与速度"));
    m_sub->setProperty("cls", "muted");
    m_sub->setStyleSheet("font-size:9pt;");
    tb->addWidget(h1);
    tb->addWidget(m_sub);
    tb->addStretch(1);
    m_windowCombo = new QComboBox;
    m_windowCombo->addItem(QStringLiteral("今天"), QStringLiteral("today"));
    m_windowCombo->addItem(QStringLiteral("近 24 小时"), QStringLiteral("24h"));
    m_windowCombo->addItem(QStringLiteral("近 7 天"), QStringLiteral("7d"));
    m_windowCombo->addItem(QStringLiteral("近 30 天"), QStringLiteral("30d"));
    m_windowCombo->addItem(QStringLiteral("全部"), QStringLiteral("all"));
    m_windowCombo->setCurrentIndex(4);
    connect(m_windowCombo, &QComboBox::currentIndexChanged, this, &ModelsPage::reload);
    tb->addWidget(m_windowCombo);
    auto* refreshBtn = new QPushButton(QStringLiteral("↻ 刷新"));
    refreshBtn->setProperty("cls", "ghost");
    connect(refreshBtn, &QPushButton::clicked, this, &ModelsPage::reload);
    tb->addWidget(refreshBtn);
    outer->addWidget(toolbar);
    outer->addWidget(UiUtil::gradientDivider());

    auto* chartCard = new CardFrame;
    auto* cl = new QVBoxLayout(chartCard);
    cl->setContentsMargins(14, 12, 14, 12);
    auto* ct = new QLabel(QStringLiteral("每日生成 token（output + reasoning，Top 5 模型）"));
    ct->setObjectName(QStringLiteral("cardTitle"));
    cl->addWidget(ct);
    m_chart = new MiniChart;
    m_chart->setMinimumHeight(300);
    cl->addWidget(m_chart, 1);
    outer->addWidget(chartCard);
    outer->addSpacing(12);

    auto* tableCard = new CardFrame;
    auto* tl = new QVBoxLayout(tableCard);
    tl->setContentsMargins(14, 12, 14, 12);
    auto* tt = new QLabel(QStringLiteral("按模型汇总"));
    tt->setObjectName(QStringLiteral("cardTitle"));
    tl->addWidget(tt);
    m_table = new RowHoverTable;
    m_table->setColumnCount(11);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("agent / 模型"), QStringLiteral("请求数"), QStringLiteral("输入"),
         QStringLiteral("缓存"), QStringLiteral("输出"), QStringLiteral("推理"),
         QStringLiteral("总 token"), QStringLiteral("占比"), QStringLiteral("均时长"),
         QStringLiteral("速度"), QStringLiteral("最近使用")});
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    tl->addWidget(m_table);
    outer->addWidget(tableCard);
    outer->addStretch(1);
}

void ModelsPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!m_loaded)
        reload();
}

void ModelsPage::reload()
{
    const QString w = m_windowCombo->currentData().toString();
    const int seq = ++m_reloadSeq;
    struct Data {
        QVector<types::ModelBreakdown> byModel;
        QVector<Store::ModelDayPoint> daily;
    };
    Async::run<Data>(
        this,
        [w]() {
            Data d;
            const qint64 since = Store::windowSince(w);
            d.byModel = Store::instance().modelBreakdown(since);
            d.daily = Store::instance().modelDailySeries(since, 5);
            return d;
        },
        [this, seq](const Data& d) {
            if (seq != m_reloadSeq)
                return;
            m_byModel = d.byModel;
            m_daily = d.daily;
            m_loaded = true;
            renderData();
            renderChart();
        });
}

void ModelsPage::renderData()
{
    const Palette& pal = Theme::instance().pal();
    setUpdatesEnabled(false);
    // 数值列比例条基准：以本列最大值为满格
    double maxIn = 0, maxOut = 0, maxReason = 0, maxTot = 0;
    for (const types::ModelBreakdown& m : m_byModel) {
        maxIn = qMax(maxIn, double(m.inTok));
        maxOut = qMax(maxOut, double(m.outTok));
        maxReason = qMax(maxReason, double(m.reasonTok));
        maxTot = qMax(maxTot, double(m.totalTokens()));
    }
    m_table->setColumnMax(2, maxIn);
    m_table->setColumnMax(4, maxOut);
    m_table->setColumnMax(5, maxReason);
    m_table->setColumnMax(6, maxTot);
    m_table->setColumnMax(7, 100.0); // 占比：条长即占比
    qint64 modelTokTotal = 0;
    for (const types::ModelBreakdown& m : m_byModel)
        modelTokTotal += m.totalTokens();
    m_table->setRowCount(int(m_byModel.size()));
    for (int i = 0; i < m_byModel.size(); ++i) {
        const types::ModelBreakdown& m = m_byModel[i];
        QString name = m.model.isEmpty() ? QStringLiteral("?") : m.model;
        if (!m.effort.isEmpty())
            name += QStringLiteral(" · ") + m.effort;
        auto* nameIt = txtItem(QStringLiteral("%1\n%2").arg(m.agentId, name));
        nameIt->setFont(QFont(QStringLiteral("Consolas"), 9));
        if (AgentAdapter* a = Store::instance().adapter(m.agentId))
            tint(nameIt, a->color());
        m_table->setItem(i, 0, nameIt);
        m_table->setItem(i, 1, numItem(Format::fmtInt(double(m.requests))));
        m_table->setItem(i, 2, numItem(Format::fmtNum(double(m.inTok)),
                                       double(m.inTok)));
        m_table->setItem(i, 3, numItem(Format::fmtNum(double(m.cachedTok))));
        m_table->setItem(i, 4, numItem(Format::fmtNum(double(m.outTok)),
                                       double(m.outTok)));
        m_table->setItem(i, 5, numItem(Format::fmtNum(double(m.reasonTok)),
                                       double(m.reasonTok)));
        m_table->setItem(i, 6, numItem(Format::fmtNumCn(double(m.totalTokens())),
                                       double(m.totalTokens())));
        const double share = modelTokTotal
            ? double(m.totalTokens()) * 100.0 / double(modelTokTotal) : 0.0;
        m_table->setItem(i, 7,
                         numItem(QString::number(share, 'f', 1) + QStringLiteral("%"),
                                 share));
        m_table->setItem(i, 8,
                         numItem(m.avgDurOk ? Format::fmtMs(m.avgDurMs) : QStringLiteral("—")));
        auto* spd = numItem(m.tpsOk
                                ? QString::number(m.weightedTps, 'f', 1) + QStringLiteral(" t/s")
                                : QStringLiteral("—"));
        if (m.tpsOk)
            tint(spd, UiUtil::speedColor(Format::speedTier(m.weightedTps)));
        else
            tint(spd, pal.fg4);
        m_table->setItem(i, 9, spd);
        m_table->setItem(i, 10, numItem(Format::fmtTime(m.lastMs)));
    }
    m_table->resizeColumnsToContents();
    m_table->resizeRowsToContents();
    // deterministic stretch: last column absorbs the remaining card width
    m_table->horizontalHeader()->setSectionResizeMode(
        m_table->columnCount() - 1, QHeaderView::Stretch);
    m_table->setFixedHeight(UiUtil::tableContentHeight(m_table, 760));
    setUpdatesEnabled(true);
}

void ModelsPage::renderChart()
{
    const Palette& pal = Theme::instance().pal();
    if (m_daily.isEmpty()) {
        m_chart->setEmpty(QStringLiteral("无数据"));
        return;
    }
    // pivot day × model (day buckets → date-only labels)
    QMap<qint64, int> dayIdx;   // day → column
    QStringList labels;
    QHash<QString, MiniChart::Series> series;
    const QColor palette[] = {pal.accent, pal.catUsage, pal.catLlm2, pal.catTool2,
                              pal.sevWarn};
    for (const Store::ModelDayPoint& p : m_daily) {
        if (!dayIdx.contains(p.dayMs)) {
            dayIdx.insert(p.dayMs, labels.size());
            labels << Format::fmtDayTick(p.dayMs);
        }
        if (!series.contains(p.model)) {
            MiniChart::Series s;
            s.name = p.model;
            s.color = palette[series.size() % 5];
            s.fillAlpha = 0.06;
            series.insert(p.model, s);
        }
    }
    for (auto it = series.begin(); it != series.end(); ++it)
        it->values.resize(labels.size());
    for (const Store::ModelDayPoint& p : m_daily) {
        const int col = dayIdx.value(p.dayMs);
        series[p.model].values[col] = double(p.genTok);
    }
    QVector<MiniChart::Series> list;
    for (auto it = series.begin(); it != series.end(); ++it)
        list << it.value();
    m_chart->setLines(labels, list, QStringLiteral("token/天"), MiniChart::YNum);
}
