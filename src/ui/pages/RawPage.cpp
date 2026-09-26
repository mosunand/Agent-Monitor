// RawPage.cpp — see RawPage.h.

#include "RawPage.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QTextBrowser>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "util/Async.h"
#include "util/Format.h"

using namespace UiUtil;

namespace {

QTableWidgetItem* numItem(const QString& text)
{
    auto* it = new QTableWidgetItem(text);
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    it->setFlags(Qt::ItemIsEnabled);
    return it;
}

QTableWidgetItem* txtItem(const QString& text)
{
    auto* it = new QTableWidgetItem(text);
    it->setFlags(Qt::ItemIsEnabled);
    return it;
}

QJsonObject recordJson(const types::UsageRecord& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("agent"), r.agentId);
    o.insert(QStringLiteral("session_id"), r.sessionId);
    o.insert(QStringLiteral("turn_id"), r.turnId);
    o.insert(QStringLiteral("response_id"), r.responseId);
    o.insert(QStringLiteral("time"), Format::fmtTimeFull(r.timeMs));
    o.insert(QStringLiteral("time_ms"), double(r.timeMs));
    o.insert(QStringLiteral("model"), r.model);
    if (!r.effort.isEmpty())
        o.insert(QStringLiteral("effort"), r.effort);
    o.insert(QStringLiteral("source"), r.source);
    if (r.durOk) {
        o.insert(QStringLiteral("duration_ms"), r.durationMs);
        o.insert(QStringLiteral("duration_approx"), r.durApprox);
        if (r.tpsOk)
            o.insert(QStringLiteral("tps"), r.tps);
    }
    o.insert(QStringLiteral("input_tokens"), double(r.inputTokens));
    o.insert(QStringLiteral("cached_input_tokens"), double(r.cachedTokens));
    o.insert(QStringLiteral("cache_write_input_tokens"), double(r.cacheWriteTokens));
    o.insert(QStringLiteral("output_tokens"), double(r.outputTokens));
    o.insert(QStringLiteral("reasoning_output_tokens"), double(r.reasoningTokens));
    return o;
}

} // namespace

RawPage::RawPage(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

void RawPage::buildUi()
{
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(24, 20, 24, 24);
    rootLay->setSpacing(12);

    auto* toolbar = new QWidget;
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(0, 0, 0, 0);
    tb->setSpacing(10);
    auto* h1 = new QLabel(QStringLiteral("原始数据"));
    h1->setObjectName(QStringLiteral("h1"));
    auto* sub = new QLabel(QStringLiteral("统一响应记录（缓存库）"));
    sub->setProperty("cls", "muted");
    sub->setStyleSheet("font-size:9pt;");
    tb->addWidget(h1);
    tb->addWidget(sub);
    tb->addStretch(1);

    m_agentCombo = new QComboBox;
    m_agentCombo->addItem(QStringLiteral("全部 agent"), QString());
    for (AgentAdapter* a : Store::instance().adapters())
        m_agentCombo->addItem(a->displayName(), a->id());
    tb->addWidget(m_agentCombo);
    m_modelEdit = new QLineEdit;
    m_modelEdit->setPlaceholderText(QStringLiteral("模型过滤（LIKE）"));
    m_modelEdit->setClearButtonEnabled(true);
    m_modelEdit->setMaximumWidth(220);
    tb->addWidget(m_modelEdit);
    auto* go = new QPushButton(QStringLiteral("查询"));
    connect(go, &QPushButton::clicked, this, &RawPage::runQuery);
    tb->addWidget(go);
    m_count = new QLabel;
    m_count->setProperty("cls", "mono-faint");
    tb->addWidget(m_count);
    rootLay->addWidget(toolbar);
    rootLay->addWidget(UiUtil::gradientDivider());

    m_table = new RowHoverTable;
    m_table->setColumnCount(9);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("时间"), QStringLiteral("Agent"), QStringLiteral("模型"),
         QStringLiteral("输入"), QStringLiteral("缓存"), QStringLiteral("输出"),
         QStringLiteral("推理"), QStringLiteral("速度"), QStringLiteral("session")});
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int col) {
        Q_UNUSED(col);
        showRowJson(row);
    });
    rootLay->addWidget(m_table, 1);
}

void RawPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_table->rowCount() == 0)
        runQuery();
}

void RawPage::runQuery()
{
    const QString agent = m_agentCombo->currentData().toString();
    const QString model = m_modelEdit->text().trimmed();
    Async::run<QVector<types::UsageRecord>>(
        this,
        [agent, model]() { return Store::instance().rawRows(agent, model, 400); },
        [this](const QVector<types::UsageRecord>& rows) {
            m_rows = rows;
            m_count->setText(QStringLiteral("%1 条（上限 400）").arg(rows.size()));
            const Palette& pal = Theme::instance().pal();
            m_table->setRowCount(int(rows.size()));
            for (int i = 0; i < rows.size(); ++i) {
                const types::UsageRecord& r = rows[i];
                m_table->setItem(i, 0, txtItem(Format::fmtTime(r.timeMs)));
                auto* ag = txtItem(r.agentId);
                if (AgentAdapter* a = Store::instance().adapter(r.agentId))
                    ag->setForeground(a->color());
                m_table->setItem(i, 1, ag);
                auto* mod = txtItem(r.model.isEmpty() ? QStringLiteral("?") : r.model);
                mod->setFont(QFont(QStringLiteral("Consolas"), 9));
                m_table->setItem(i, 2, mod);
                m_table->setItem(i, 3, numItem(Format::fmtInt(double(r.inputTokens))));
                auto* cc = numItem(Format::fmtInt(double(r.cachedTokens)));
                if (r.cachedTokens == 0)
                    cc->setForeground(pal.fg4);
                m_table->setItem(i, 4, cc);
                m_table->setItem(i, 5, numItem(Format::fmtInt(double(r.outputTokens))));
                m_table->setItem(i, 6, numItem(Format::fmtInt(double(r.reasoningTokens))));
                auto* spd = numItem(r.tpsOk
                                        ? (r.durApprox ? QStringLiteral("≈") : QString())
                                              + QString::number(r.tps, 'f', 1)
                                              + QStringLiteral(" t/s")
                                        : QStringLiteral("—"));
                if (r.tpsOk)
                    spd->setForeground(UiUtil::speedColor(Format::speedTier(r.tps)));
                else
                    spd->setForeground(pal.fg4);
                m_table->setItem(i, 7, spd);
                m_table->setItem(i, 8, txtItem(r.sessionId));
            }
            m_table->resizeColumnsToContents();
            // deterministic stretch: session column absorbs remaining width
            m_table->horizontalHeader()->setSectionResizeMode(
                m_table->columnCount() - 1, QHeaderView::Stretch);
        });
}

void RawPage::showRowJson(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    const types::UsageRecord& r = m_rows.at(row);
    const Palette& pal = Theme::instance().pal();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("响应记录"));
    dlg.resize(760, 680);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(16, 14, 16, 14);
    lay->setSpacing(10);

    // ── 摘要：关键字段直接读，不必在 JSON 里翻找 ──
    {
        auto* head = new QWidget;
        auto* hl = new QVBoxLayout(head);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(3);
        auto* top = new QHBoxLayout;
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(8);
        QColor ac = pal.accent;
        if (AgentAdapter* a = Store::instance().adapter(r.agentId))
            ac = a->color();
        auto* dot = new Dot(8);
        dot->setColor(ac);
        top->addWidget(dot);
        auto* agent = new QLabel(r.agentId);
        agent->setStyleSheet(QStringLiteral("color:%1;font-weight:600;").arg(ac.name()));
        top->addWidget(agent);
        QString model = r.model.isEmpty() ? QStringLiteral("?") : r.model;
        if (!r.effort.isEmpty())
            model += QStringLiteral(" · ") + r.effort;
        auto* modelL = new QLabel(model);
        modelL->setProperty("cls", "mono");
        top->addWidget(modelL);
        top->addStretch(1);
        if (!r.error.isEmpty()) {
            auto* err = new QLabel(r.error);
            err->setStyleSheet(QStringLiteral("color:%1;font-weight:600;")
                                   .arg(pal.sevErr.name()));
            top->addWidget(err);
        }
        hl->addLayout(top);

        QStringList facts;
        facts << Format::fmtTimeFull(r.timeMs);
        if (r.durOk)
            facts << (r.durApprox ? QStringLiteral("≈") : QString())
                         + Format::fmtMs(r.durationMs);
        if (r.tpsOk)
            facts << (r.durApprox ? QStringLiteral("≈") : QString())
                         + QString::number(r.tps, 'f', 1) + QStringLiteral(" t/s");
        facts << QStringLiteral("in %1 / out %2 / reason %3")
                     .arg(Format::fmtInt64(r.inputTokens),
                          Format::fmtInt64(r.outputTokens),
                          Format::fmtInt64(r.reasoningTokens));
        if (r.cachedTokens > 0)
            facts << QStringLiteral("cache %1").arg(Format::fmtInt64(r.cachedTokens));
        auto* sub = new QLabel(facts.join(QStringLiteral(" · ")));
        sub->setProperty("cls", "mono-sub");
        hl->addWidget(sub);
        lay->addWidget(head);
    }

    // ── JSON：语法着色随主题取色（UiUtil::jsonHighlightHtml，与
    //    会话页 JSON 标签页共用同一套配色）──
    const QJsonDocument doc(recordJson(r));
    const QString plain = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));

    auto* view = new QTextBrowser(&dlg);
    view->setOpenExternalLinks(false);
    view->document()->setDocumentMargin(16); // 内边距（QSS padding 对文档无效）
    view->setHtml(QStringLiteral(
        "<style>body{color:%1;line-height:1.55;}</style>"
        "<pre style='margin:0'>%2</pre>")
        .arg(pal.fg1.name(), UiUtil::jsonHighlightHtml(plain, pal)));
    lay->addWidget(view, 1);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* copy = new QPushButton(QStringLiteral("复制 JSON"), &dlg);
    auto* close = new QPushButton(QStringLiteral("关闭"), &dlg);
    close->setDefault(true);
    btns->addWidget(copy);
    btns->addWidget(close);
    lay->addLayout(btns);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(copy, &QPushButton::clicked, &dlg, [copy, plain] {
        QGuiApplication::clipboard()->setText(plain);
        copy->setText(QStringLiteral("已复制 ✓"));
        QTimer::singleShot(1200, copy, [copy] {
            copy->setText(QStringLiteral("复制 JSON"));
        });
    });
    dlg.exec();
}
