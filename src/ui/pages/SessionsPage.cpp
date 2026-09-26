// SessionsPage.cpp — see SessionsPage.h.

#include "SessionsPage.h"

#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "ui/widgets/MiniChart.h"
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

void tint(QTableWidgetItem* it, const QColor& c)
{
    it->setForeground(c);
}

void kvRow(QTableWidget* t, int row, const QString& key, const QString& value)
{
    auto* k = new QTableWidgetItem(key);
    k->setFlags(Qt::ItemIsEnabled);
    k->setForeground(Theme::instance().pal().fg3);
    auto* v = new QTableWidgetItem(value);
    v->setFlags(Qt::ItemIsEnabled);
    v->setFont(QFont(QStringLiteral("Consolas"), 9));
    t->setItem(row, 0, k);
    t->setItem(row, 1, v);
}

// 列表内容指纹：行数 + 每行关键字段。2s 轮询里内容没变就整表跳过 ——
// 重建 500 项要重新分配 item、测字体、写 tooltip，纯属白费
QString listFingerprint(const QVector<types::SessionInfo>& rows)
{
    QString s;
    s.reserve(rows.size() * 64);
    for (const types::SessionInfo& r : rows) {
        s += r.agentId;
        s += QLatin1Char('\x1f');
        s += r.sessionId;
        s += QLatin1Char('\x1f');
        s += r.title;
        s += QLatin1Char('\x1f');
        s += r.model;
        s += QLatin1Char('\x1f');
        s += QString::number(r.updatedMs);
        s += QLatin1Char('\x1f');
        s += QString::number(r.totalTokens());
        s += QLatin1Char('\x1e');
    }
    return s;
}

// session list rows: two lines, each elided against the REAL paint width
// (option.rect) — width guessing can never be exact (late scrollbar, DPI),
// a delegate cannot be wrong
class SessionRowDelegate : public QStyledItemDelegate {
public:
    explicit SessionRowDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // background/selection/focus — call the STYLE directly: going through
        // QStyledItemDelegate::paint would re-init the option from the model
        // (re-fetching the display text) and draw it a second time → ghosting
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        QStyle* style = option.widget ? option.widget->style()
                                      : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, option.widget);

        const QString text = index.data(Qt::DisplayRole).toString();
        if (text.isEmpty())
            return;
        const QStringList lines = text.split(QLatin1Char('\n'));

        const Palette& pal = Theme::instance().pal();
        const QColor line1Color = index.data(Qt::ForegroundRole)
                                      .value<QColor>();
        p->save();
        // never bleed outside the item rect: a layout/delegate metric
        // mismatch must degrade to clipped text, never overlap neighbours
        p->setClipRect(option.rect);
        QFontMetrics fm(option.font);
        const QRect r = option.rect.adjusted(10, 6, -10, -6);
        const int lineH = fm.height() + 3;
        int y = r.top() + qMax(0, (r.height() - lines.size() * lineH + 3) / 2);
        for (int i = 0; i < lines.size(); ++i) {
            QString text = lines[i];
            QString tail;
            int avail = r.width();
            // line2 的 token 数右对齐完整显示，左侧信息从右边省略
            if (i == 1) {
                tail = index.data(Qt::UserRole + 2).toString();
                if (!tail.isEmpty())
                    avail = qMax(40, r.width() - fm.horizontalAdvance(tail) - 10);
            }
            const QString elided = fm.elidedText(text, Qt::ElideRight, avail);
            p->setFont(option.font);
            p->setPen(i == 0 && line1Color.isValid() ? line1Color : pal.fg2);
            if (i == 0) {
                QFont f = option.font;
                f.setBold(true);
                p->setFont(f);
            } else {
                p->setPen(pal.fg3);
            }
            p->drawText(r.left(), y, avail, fm.height(),
                        Qt::AlignLeft | Qt::AlignVCenter, elided);
            if (i == 1 && !tail.isEmpty()) {
                p->setFont(option.font);
                p->setPen(pal.fg2);
                const int tailW = fm.horizontalAdvance(tail);
                p->drawText(r.right() - tailW, y, tailW, fm.height(),
                            Qt::AlignLeft | Qt::AlignVCenter, tail);
            }
            y += lineH;
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex&) const override
    {
        QFontMetrics fm(option.font);
        return QSize(fm.height() * 8, fm.height() * 2 + 14);
    }
};

} // namespace

// ── 明细（虚拟化）──────────────────────────────────────────────
// 千级响应的会话，QTableWidget 要为每个单元格造 item；模型/视图分离后
// 只为可见行取数与绘制，滚动零成本

class UsageTableModel : public QAbstractTableModel {
public:
    explicit UsageTableModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setRecords(const QVector<types::UsageRecord>& rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }

    int rowCount(const QModelIndex& = QModelIndex()) const override
    {
        return m_rows.size();
    }
    int columnCount(const QModelIndex& = QModelIndex()) const override
    {
        return 10;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return {};
        const int col = index.column();
        const types::UsageRecord& r = m_rows.at(index.row());
        const Palette& pal = Theme::instance().pal();
        if (role == Qt::DisplayRole) {
            switch (col) {
            case 0: return Format::fmtTime(r.timeMs);
            case 1: return r.model.isEmpty() ? QStringLiteral("?") : r.model;
            case 2: return r.effort;
            case 3: return Format::fmtInt(double(r.inputTokens));
            case 4: return Format::fmtInt(double(r.cachedTokens));
            case 5: return Format::fmtInt(double(r.outputTokens));
            case 6: return Format::fmtInt(double(r.reasoningTokens));
            case 7: return r.durOk ? Format::fmtMs(r.durationMs)
                                    : QStringLiteral("—");
            case 8: return r.tpsOk
                        ? (r.durApprox ? QStringLiteral("≈") : QString())
                              + QString::number(r.tps, 'f', 1)
                              + QStringLiteral(" t/s")
                        : QStringLiteral("—");
            case 9: return r.source;
            }
            return {};
        }
        if (role == Qt::TextAlignmentRole) {
            if (col >= 3 && col <= 7)
                return int(Qt::AlignRight | Qt::AlignVCenter);
            if (col == 8)
                return int(Qt::AlignHCenter | Qt::AlignVCenter);
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (role == Qt::FontRole && col == 1) {
            QFont f;
            f.setFamily(QStringLiteral("Consolas"));
            f.setPointSize(9);
            return f;
        }
        if (role == Qt::ForegroundRole) {
            if (col == 4 && r.cachedTokens == 0)
                return pal.fg4;
            if (col == 6 && r.reasoningTokens == 0)
                return pal.fg4;
            if (col == 8)
                return r.tpsOk ? UiUtil::speedColor(Format::speedTier(r.tps))
                               : pal.fg4;
            return {};
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            static const char* kHeaders[] = {
                "时间", "模型", "effort", "输入", "缓存", "输出",
                "推理", "时长", "速度", "来源",
            };
            return section >= 0 && section < 10
                ? QString::fromUtf8(kHeaders[section]) : QString();
        }
        return {};
    }

    Qt::ItemFlags flags(const QModelIndex&) const override
    {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

private:
    QVector<types::UsageRecord> m_rows;
};

// 行悬停视觉与 RowHoverTable 同款：viewport 事件过滤 + delegate 底色
class RowHoverView : public QTableView {
public:
    explicit RowHoverView(QWidget* parent = nullptr)
        : QTableView(parent)
    {
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
        viewport()->installEventFilter(this);
    }
    int hoverRow() const { return m_hoverRow; }

protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (watched == viewport()) {
            if (e->type() == QEvent::MouseMove) {
                const int row = rowAt(static_cast<QMouseEvent*>(e)->pos().y());
                if (row != m_hoverRow) {
                    m_hoverRow = row;
                    viewport()->update();
                }
            } else if (e->type() == QEvent::Leave) {
                if (m_hoverRow != -1) {
                    m_hoverRow = -1;
                    viewport()->update();
                }
            }
        }
        return QTableView::eventFilter(watched, e);
    }

private:
    int m_hoverRow = -1;
};

class RespDelegate : public QStyledItemDelegate {
public:
    explicit RespDelegate(RowHoverView* view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (m_view && index.row() == m_view->hoverRow())
            p->fillRect(option.rect, Theme::instance().pal().surface2);
        QStyledItemDelegate::paint(p, option, index);
    }

private:
    const RowHoverView* m_view;
};

SessionsPage::SessionsPage(QWidget* parent)
    : QWidget(parent)
{
    connect(&Store::instance(), &Store::dataChanged, this,
            &SessionsPage::onDataChanged, Qt::QueuedConnection);
    connect(&Theme::instance(), &Theme::changed, this, [this](bool) { rebuildList(); });
    buildUi();
}

void SessionsPage::buildUi()
{
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QWidget;
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(24, 20, 24, 12);
    tb->setSpacing(10);
    auto* h1 = new QLabel(QStringLiteral("会话"));
    h1->setObjectName(QStringLiteral("h1"));
    auto* sub = new QLabel(QStringLiteral("所有 agent 的本地会话"));
    sub->setProperty("cls", "muted");
    sub->setStyleSheet("font-size:9pt;");
    tb->addWidget(h1);
    tb->addWidget(sub);
    tb->addStretch(1);
    rootLay->addWidget(toolbar);
    rootLay->addWidget(UiUtil::gradientDivider());

    auto* split = new QSplitter(Qt::Horizontal, this);
    rootLay->addWidget(split, 1);

    // ── left list pane ──
    auto* listPane = new QFrame;
    listPane->setObjectName(QStringLiteral("listpane"));
    listPane->setMaximumWidth(430);
    auto* lp = new QVBoxLayout(listPane);
    lp->setContentsMargins(10, 0, 10, 10);
    lp->setSpacing(8);

    auto* filterRow = new QWidget;
    auto* fr = new QHBoxLayout(filterRow);
    fr->setContentsMargins(0, 0, 0, 0);
    fr->setSpacing(6);
    m_agentCombo = new QComboBox;
    m_agentCombo->addItem(QStringLiteral("全部 agent"), QString());
    for (AgentAdapter* a : Store::instance().adapters())
        m_agentCombo->addItem(a->displayName(), a->id());
    connect(m_agentCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { rebuildList(); });
    fr->addWidget(m_agentCombo, 1);
    m_sortCombo = new QComboBox;
    m_sortCombo->addItem(QStringLiteral("最近更新"), QStringLiteral("updated"));
    m_sortCombo->addItem(QStringLiteral("token 最多"), QStringLiteral("tokens"));
    connect(m_sortCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { rebuildList(); });
    fr->addWidget(m_sortCombo);
    lp->addWidget(filterRow);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("搜索标题 / 目录 / 模型 / id…"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this,
            [this](const QString& t) { if (t.isEmpty() || t.size() >= 2) rebuildList(); });
    lp->addWidget(m_search);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("sessionList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setWordWrap(false);
    m_list->setItemDelegate(new SessionRowDelegate(m_list));
    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &SessionsPage::onSelectionChanged);
    lp->addWidget(m_list, 1);
    split->addWidget(listPane);

    // ── right detail ──
    auto* detail = new QWidget;
    auto* dl = new QVBoxLayout(detail);
    dl->setContentsMargins(18, 12, 24, 16);
    dl->setSpacing(8);

    m_detailTitle = new QLabel(QStringLiteral("选择一个会话"));
    m_detailTitle->setObjectName(QStringLiteral("detailTitle"));
    m_detailTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(m_detailTitle);
    m_detailSub = new QLabel;
    m_detailSub->setObjectName(QStringLiteral("detailSub"));
    m_detailSub->setWordWrap(true);
    dl->addWidget(m_detailSub);
    m_detailStats = new QLabel;
    m_detailStats->setProperty("cls", "muted");
    dl->addWidget(m_detailStats);

    // empty state: no session selected yet — hide the tabs until a detail lands
    m_detailEmpty = new QLabel(QStringLiteral("← 从左侧选择一个会话查看明细"));
    m_detailEmpty->setProperty("cls", "faint");
    m_detailEmpty->setAlignment(Qt::AlignCenter);
    dl->addWidget(m_detailEmpty, 1);

    m_tabs = new QTabWidget;
    m_tabs->hide();
    // 概览
    auto* overviewTab = new QWidget;
    auto* ol = new QVBoxLayout(overviewTab);
    ol->setContentsMargins(0, 8, 0, 0);
    ol->setSpacing(10);
    m_infoTable = new RowHoverTable;
    m_infoTable->setColumnCount(2);
    m_infoTable->setHorizontalHeaderLabels({QStringLiteral("项"), QStringLiteral("值")});
    m_infoTable->verticalHeader()->hide();
    m_infoTable->horizontalHeader()->setStretchLastSection(true);
    m_infoTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_infoTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_infoTable->setFixedHeight(UiUtil::tableContentHeight(m_infoTable) + 2);
    ol->addWidget(m_infoTable);
    auto* tpsCard = new CardFrame;
    auto* tl = new QVBoxLayout(tpsCard);
    tl->setContentsMargins(14, 12, 14, 12);
    auto* tt = new QLabel(QStringLiteral("逐响应速度 (tok/s)"));
    tt->setObjectName(QStringLiteral("cardTitle"));
    tl->addWidget(tt);
    m_tpsChart = new MiniChart;
    m_tpsChart->setMinimumHeight(260);
    tl->addWidget(m_tpsChart, 1);
    ol->addWidget(tpsCard, 1);
    m_tabs->addTab(overviewTab, QStringLiteral("概览"));

    // 对话重放（懒加载：切到该标签才解析源文件）
    m_chatView = new QTextBrowser;
    m_chatView->setOpenExternalLinks(false);
    m_tabs->addTab(m_chatView, QStringLiteral("对话"));

    // 明细（虚拟化表格：只为可见行取数绘制）
    auto* usageTab = new QWidget;
    auto* ul = new QVBoxLayout(usageTab);
    ul->setContentsMargins(0, 8, 0, 0);
    m_respModel = new UsageTableModel(this);
    auto* respView = new RowHoverView;
    m_respView = respView;
    respView->setModel(m_respModel);
    respView->setItemDelegate(new RespDelegate(respView));
    m_respView->verticalHeader()->hide();
    m_respView->horizontalHeader()->setStretchLastSection(true);
    m_respView->setAlternatingRowColors(true); // zebra rows
    m_respView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_respView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ul->addWidget(m_respView);
    m_tabs->addTab(usageTab, QStringLiteral("明细"));

    // JSON（语法着色与「响应记录」对话框共用同一套主题配色）
    m_jsonView = new QTextBrowser;
    m_jsonView->setOpenExternalLinks(false);
    m_jsonView->document()->setDocumentMargin(14);
    m_tabs->addTab(m_jsonView, QStringLiteral("JSON"));

    connect(m_tabs, &QTabWidget::currentChanged, this, &SessionsPage::onTabChanged);

    dl->addWidget(m_tabs, 1);
    m_tabs->hide(); // until the first detail renders
    split->addWidget(detail);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
}

void SessionsPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_list->count() == 0)
        rebuildList();
}

void SessionsPage::rebuildList()
{
    const QString agent = m_agentCombo->currentData().toString();
    const QString search = m_search ? m_search->text().trimmed() : QString();
    const QString sort = m_sortCombo ? m_sortCombo->currentData().toString() : QString();
    const int seq = ++m_listSeq; // list rebuilds have their own counter
    Async::run<QVector<types::SessionInfo>>(
        this,
        [agent, search]() {
            return Store::instance().sessionList(agent, search, 500);
        },
        [this, seq, sort](const QVector<types::SessionInfo>& rows) {
            if (seq != m_listSeq)
                return;
            QVector<types::SessionInfo> sorted = rows;
            if (sort == QLatin1String("tokens")) {
                std::sort(sorted.begin(), sorted.end(),
                          [](const types::SessionInfo& a, const types::SessionInfo& b) {
                              return a.totalTokens() > b.totalTokens();
                          });
            }
            const QString fp = listFingerprint(sorted);
            if (fp == m_listFingerprint)
                return; // 内容没变：不动列表（也保住滚动位置与悬停）
            m_listFingerprint = fp;
            QSignalBlocker blocker(m_list);
            m_list->setUpdatesEnabled(false);
            const int scrollPos = m_list->verticalScrollBar()->value();
            m_list->clear();
            for (const types::SessionInfo& s : sorted) {
                auto* it = new QListWidgetItem(m_list);
                const QString title = s.title.isEmpty() ? s.sessionId.left(16) : s.title;
                // line1 = title；line2 = "agent · model · time"；token 数放在
                // UserRole+2，delegate 右对齐完整显示（永不截断）
                it->setText(QStringLiteral("%1\n%2 · %3 · %4")
                                .arg(title, s.agentId,
                                     s.model.isEmpty() ? QStringLiteral("?") : s.model,
                                     Format::fmtTime(s.updatedMs)));
                it->setData(Qt::UserRole, s.agentId);
                it->setData(Qt::UserRole + 1, s.sessionId);
                it->setData(Qt::UserRole + 2,
                            Format::fmtNumCn(double(s.totalTokens()))
                                + QStringLiteral(" tokens"));
                it->setToolTip(s.cwd + QStringLiteral("\n") + s.filePath);
                if (AgentAdapter* a = Store::instance().adapter(s.agentId))
                    it->setForeground(a->color());
                QFontMetrics fm(m_list->font());
                it->setSizeHint(QSize(0, fm.height() * 2 + 14));
            }
            // restore scroll position so live rebuilds don't yank the view
            m_list->verticalScrollBar()->setValue(scrollPos);
            m_list->setUpdatesEnabled(true);
            // keep selection if the current session is still in the list
            if (!m_currentSession.isEmpty()) {
                for (int i = 0; i < m_list->count(); ++i) {
                    auto* it = m_list->item(i);
                    if (it->data(Qt::UserRole).toString() == m_currentAgent
                        && it->data(Qt::UserRole + 1).toString() == m_currentSession) {
                        m_list->setCurrentRow(i);
                        break;
                    }
                }
            }
        });
}

void SessionsPage::onSelectionChanged()
{
    QListWidgetItem* it = m_list->currentItem();
    if (!it)
        return;
    loadDetail(it->data(Qt::UserRole).toString(),
               it->data(Qt::UserRole + 1).toString());
}

void SessionsPage::openSession(const QString& agent, const QString& sessionId)
{
    m_currentAgent = agent;
    m_currentSession = sessionId;
    bool found = false;
    for (int i = 0; i < m_list->count(); ++i) {
        auto* it = m_list->item(i);
        if (it->data(Qt::UserRole).toString() == agent
            && it->data(Qt::UserRole + 1).toString() == sessionId) {
            m_list->setCurrentRow(i);
            found = true;
            break;
        }
    }
    if (!found)
        loadDetail(agent, sessionId); // not in the (filtered) list — load anyway
}

void SessionsPage::onDataChanged()
{
    if (isVisible())
        rebuildList();
}

void SessionsPage::onTabChanged(int index)
{
    if (!m_infoLoaded)
        return;
    if (index == 1 && !m_chatLoaded)
        loadChat();
    else if (index == 2 && !m_respLoaded)
        buildRespTable(); // 明细懒加载
    else if (index == 3 && !m_jsonLoaded)
        buildJsonView();  // JSON 懒加载
}

void SessionsPage::loadDetail(const QString& agent, const QString& sessionId)
{
    m_currentAgent = agent;
    m_currentSession = sessionId;
    const int seq = ++m_loadSeq;
    struct Detail {
        types::SessionInfo info;
        QVector<types::UsageRecord> records;
    };
    Async::run<Detail>(
        this,
        [agent, sessionId]() {
            Detail d;
            d.info = Store::instance().sessionInfo(agent, sessionId);
            d.records = Store::instance().sessionRecords(agent, sessionId);
            return d;
        },
        [this, seq](const Detail& d) {
            if (seq != m_loadSeq)
                return;
            m_info = d.info;
            m_records = d.records;
            m_infoLoaded = true;
            renderDetail();
        });
}

void SessionsPage::renderDetail()
{
    if (!m_infoLoaded)
        return;
    const Palette& pal = Theme::instance().pal();
    const types::SessionInfo& s = m_info;
    setUpdatesEnabled(false);

    m_detailEmpty->hide();
    m_tabs->show();
    // conversation replay must be re-loaded for the newly selected session
    m_chatLoaded = false;
    m_chat.clear();
    m_chatView->clear();
    m_chatView->setPlaceholderText(QStringLiteral("切换到该标签时按需解析源文件"));
    // if the user is already ON the chat tab, load for the new session now
    if (m_tabs->currentIndex() == 1)
        loadChat();

    m_detailTitle->setText(s.title);
    QString sub = QStringLiteral("%1 · %2").arg(
        s.agentId, s.model.isEmpty() ? QStringLiteral("?") : s.model);
    sub += QStringLiteral(" · %1 → %2")
               .arg(Format::fmtTimeFull(s.startedMs), Format::fmtTimeFull(s.updatedMs));
    m_detailSub->setText(sub);
    m_detailStats->setText(QStringLiteral(
        "%1 次模型响应 · %2 次工具调用 · 总 token %3")
        .arg(Format::fmtInt(double(s.responses)),
             Format::fmtInt(double(s.toolCalls)),
             Format::fmtNumCn(double(s.totalTokens()))));

    // ── info grid ──
    const QList<QPair<QString, QString>> rows = {
        { QStringLiteral("agent"), s.agentId },
        { QStringLiteral("session id"), s.sessionId },
        { QStringLiteral("模型"), s.model.isEmpty() ? QStringLiteral("—") : s.model },
        { QStringLiteral("目录"), s.cwd.isEmpty() ? QStringLiteral("—") : s.cwd },
        { QStringLiteral("来源"), s.originator.isEmpty() ? QStringLiteral("—") : s.originator },
        { QStringLiteral("会话文件"), s.filePath },
        { QStringLiteral("开始 / 更新"),
          QStringLiteral("%1 / %2").arg(Format::fmtTimeFull(s.startedMs),
                                        Format::fmtTimeFull(s.updatedMs)) },
        { QStringLiteral("上下文窗口"),
          s.contextWindow > 0 ? Format::fmtInt64(s.contextWindow) : QStringLiteral("—") },
        { QStringLiteral("输入 token"), Format::fmtInt64(s.inputTokens) },
        { QStringLiteral("缓存读 / 写"),
          QStringLiteral("%1 / %2").arg(Format::fmtInt64(s.cachedTokens),
                                        Format::fmtInt64(s.cacheWriteTokens)) },
        { QStringLiteral("输出 token"), Format::fmtInt64(s.outputTokens) },
        { QStringLiteral("推理 token"), Format::fmtInt64(s.reasoningTokens) },
        { QStringLiteral("缓存命中率"),
          s.inputTokens
              ? QString::number(qMin(100.0, double(s.cachedTokens) * 100.0
                                     / double(s.inputTokens)), 'f', 1) + '%'
              : QStringLiteral("—") },
    };
    m_infoTable->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i)
        kvRow(m_infoTable, i, rows[i].first, rows[i].second);
    m_infoTable->resizeColumnsToContents();
    m_infoTable->setFixedHeight(UiUtil::tableContentHeight(m_infoTable, 420));

    // ── per-response tps chart ──
    {
        QStringList labels;
        MiniChart::Series sp;
        sp.name = QStringLiteral("tok/s");
        sp.color = pal.accent;
        sp.showPoints = true;
        for (const types::UsageRecord& r : m_records) {
            if (!r.tpsOk)
                continue;
            labels << Format::fmtTimeTick(r.timeMs);
            sp.values << r.tps;
            sp.pointColors << UiUtil::speedColor(Format::speedTier(r.tps));
        }
        if (labels.isEmpty())
            m_tpsChart->setEmpty(QStringLiteral("无可测流式响应"));
        else
            m_tpsChart->setLines(labels, {sp}, QStringLiteral("tok/s"),
                                 MiniChart::YTps);
    }

    // ── 明细 / JSON 标签页懒加载 ──
    m_respModel->setRecords({});
    m_respLoaded = false;
    m_jsonView->clear();
    m_jsonLoaded = false;
    // 用户正停在这两个标签页上时，为新会话立即构建
    if (m_tabs->currentIndex() == 2)
        buildRespTable();
    else if (m_tabs->currentIndex() == 3)
        buildJsonView();
    setUpdatesEnabled(true);
}

// 明细标签页：虚拟化模型/视图（切到该标签页时才装载 —— 装载本身也只是
// 一次模型重置 + 列宽测量，不再逐格造 item）
void SessionsPage::buildRespTable()
{
    if (!m_infoLoaded)
        return;
    m_respLoaded = true;
    m_respModel->setRecords(m_records);
    m_respView->resizeColumnsToContents();
    // deterministic stretch: last column absorbs the remaining width
    m_respView->horizontalHeader()->setSectionResizeMode(
        m_respModel->columnCount() - 1, QHeaderView::Stretch);
}

// JSON 标签页：统一模型的原始记录（切到该标签页时才构建着色 HTML ——
// 大会话的 JSON 着色要过 5 遍正则 + QTextDocument 解理，几十毫秒级）
void SessionsPage::buildJsonView()
{
    if (!m_infoLoaded)
        return;
    m_jsonLoaded = true;
    const Palette& pal = Theme::instance().pal();
    QJsonArray arr;
    for (const types::UsageRecord& r : m_records) {
        QJsonObject o;
        o.insert(QStringLiteral("agent"), r.agentId);
        o.insert(QStringLiteral("session_id"), r.sessionId);
        o.insert(QStringLiteral("turn_id"), r.turnId);
        o.insert(QStringLiteral("response_id"), r.responseId);
        o.insert(QStringLiteral("time"), Format::fmtTimeFull(r.timeMs));
        o.insert(QStringLiteral("time_ms"), double(r.timeMs));
        if (r.durOk) {
            o.insert(QStringLiteral("duration_ms"), r.durationMs);
            o.insert(QStringLiteral("duration_approx"), r.durApprox);
        }
        o.insert(QStringLiteral("model"), r.model);
        if (!r.effort.isEmpty())
            o.insert(QStringLiteral("effort"), r.effort);
        o.insert(QStringLiteral("source"), r.source);
        o.insert(QStringLiteral("input_tokens"), double(r.inputTokens));
        o.insert(QStringLiteral("cached_input_tokens"), double(r.cachedTokens));
        o.insert(QStringLiteral("cache_write_input_tokens"), double(r.cacheWriteTokens));
        o.insert(QStringLiteral("output_tokens"), double(r.outputTokens));
        o.insert(QStringLiteral("reasoning_output_tokens"), double(r.reasoningTokens));
        if (r.tpsOk)
            o.insert(QStringLiteral("tps"), r.tps);
        arr.append(o);
    }
    const QString jsonText = QString::fromUtf8(
        QJsonDocument(arr).toJson(QJsonDocument::Indented));
    m_jsonView->setHtml(QStringLiteral(
        "<style>body{color:%1;line-height:1.5;}</style><pre style='margin:0'>%2</pre>")
        .arg(pal.fg1.name(), UiUtil::jsonHighlightHtml(jsonText, pal)));
}

// ── conversation replay (lazy, off-thread parse of the source file) ──

void SessionsPage::loadChat()
{
    const QString agent = m_currentAgent;
    const QString sessionId = m_currentSession;
    const int seq = ++m_chatSeq;
    m_chatView->setPlaceholderText(QString());
    m_chatView->setHtml(QStringLiteral(
        "<p style='color:%1'>解析会话文件…</p>")
        .arg(Theme::instance().pal().fg4.name()));
    Async::run<QVector<types::ChatMessage>>(
        this,
        [agent, sessionId]() {
            QVector<types::ChatMessage> msgs;
            Store::instance().sessionMessages(agent, sessionId, msgs);
            return msgs;
        },
        [this, seq, agent, sessionId](const QVector<types::ChatMessage>& msgs) {
            if (seq != m_chatSeq || agent != m_currentAgent
                || sessionId != m_currentSession)
                return; // user moved on to another session meanwhile
            m_chat = msgs;
            m_chatLoaded = true;
            m_chatSupported = !msgs.isEmpty();
            m_chatView->setHtml(renderChatHtml());
        });
}

namespace {

// 面板背景 = tint 色在 surface1 上的实色合成 —— Qt 富文本的 table
// bgcolor 不吃带 alpha 的颜色，先合成不透明
QColor blendOver(const QColor& top, int alpha, const QColor& base)
{
    return QColor((top.red() * alpha + base.red() * (255 - alpha)) / 255,
                  (top.green() * alpha + base.green() * (255 - alpha)) / 255,
                  (top.blue() * alpha + base.blue() * (255 - alpha)) / 255);
}

// 等宽面板（工具参数/输出/代码块）：表格 bgcolor 是 Qt 富文本里
// 最可靠的块级背景方案；<pre> 保留原始排版
QString panelHtml(const QString& rawText, const QColor& bg)
{
    return QStringLiteral(
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"9\" bgcolor=\"%1\">"
        "<tr><td><pre><font face=\"Consolas\">%2</font></pre></td></tr></table>")
        .arg(bg.name(), rawText.toHtmlEscaped());
}

// 推理面板：微染底 + 斜体
QString thinkHtml(const QString& rawText, const QColor& bg)
{
    return QStringLiteral(
        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" bgcolor=\"%1\">"
        "<tr><td><i>%2</i></td></tr></table>")
        .arg(bg.name(),
             rawText.toHtmlEscaped().replace(QLatin1Char('\n'), QLatin1String("<br/>")));
}

// 正文：``` 围栏代码块 → 独立代码面板，其余保留换行
QString proseHtml(const QString& raw, const QColor& codeBg)
{
    const QStringList segs = raw.split(QStringLiteral("```"));
    QString out;
    for (int i = 0; i < segs.size(); ++i) {
        if (i % 2 == 1) { // 代码段
            QString code = segs[i];
            // 去掉 ```lang 的语言标注行
            const int nl = code.indexOf(QLatin1Char('\n'));
            if (nl >= 0 && nl < 24 && !code.left(nl).contains(QLatin1Char(' ')))
                code = code.mid(nl + 1);
            out += panelHtml(code.trimmed(), codeBg);
        } else {
            out += segs[i].toHtmlEscaped()
                       .replace(QLatin1Char('\n'), QLatin1String("<br/>"));
        }
    }
    return out;
}

} // namespace

QString SessionsPage::renderChatHtml() const
{
    const Palette& pal = Theme::instance().pal();
    if (!m_chatSupported)
        return QStringLiteral("<p style='color:%1'>该会话没有可重放的对话内容。</p>")
            .arg(pal.fg4.name());

    // 面板底色：代码/工具 = surface1 上浮一层 catTool；推理 = catLlm2 微染；
    // 出错输出 = sevErr 染红（错误要靠底色一眼认出来，不能只靠正文）
    const QColor codeBg = blendOver(pal.catTool, 26, pal.surface1);
    const QColor thinkBg = blendOver(pal.catLlm2, 22, pal.surface1);
    const QColor errBg = blendOver(pal.sevErr, 30, pal.surface1);

    QString html = QStringLiteral(
        "<style>body{font-family:'Segoe UI','Microsoft YaHei UI';font-size:9.5pt;}"
        ".head{font-family:Consolas;font-size:8pt;}"
        ".msg{margin:4px 0 14px 0;}</style>");
    const int max = qMin(400, m_chat.size());
    for (int i = 0; i < max; ++i) {
        const types::ChatMessage& m = m_chat.at(i);
        QString head, body;
        if (m.role == QLatin1String("user")) {
            head = QStringLiteral("<span class='head' style='color:%1'>● 用户")
                       .arg(pal.catConversation.name());
            body = proseHtml(m.text, codeBg);
        } else if (m.role == QLatin1String("assistant")) {
            head = QStringLiteral("<span class='head' style='color:%1'>● 助手%2</span>")
                       .arg(pal.catLlm2.name(),
                            m.model.isEmpty() ? QString()
                                              : QStringLiteral(" · %1").arg(m.model.toHtmlEscaped()));
            body = proseHtml(m.text, codeBg);
        } else if (m.role == QLatin1String("reasoning")) {
            head = QStringLiteral("<span class='head' style='color:%1'>● 推理</span>")
                       .arg(pal.catLlm2.name());
            // reasoning summaries carry markdown emphasis markers — strip the
            // asterisks, plain italic text reads cleaner in the replay
            body = thinkHtml(QString(m.text).remove(QLatin1String("**")), thinkBg);
        } else if (m.role == QLatin1String("tool")) {
            head = QStringLiteral("<span class='head' style='color:%1'>● ⚙ 工具 %2</span>")
                       .arg(pal.catTool.name(), m.toolName.toHtmlEscaped());
            body = panelHtml(m.text, codeBg);
        } else { // tool_output
            head = QStringLiteral(
                        "<span class='head' style='color:%1'>● 工具输出%2</span>")
                       .arg(m.isError ? pal.sevErr.name() : pal.fg4.name(),
                            m.isError ? QStringLiteral(" ✗") : QString());
            body = panelHtml(m.text, m.isError ? errBg : codeBg);
        }
        if (m.timeMs > 0)
            head += QStringLiteral(" <span style='color:%1'>%2</span>")
                        .arg(pal.fg5.name(), Format::fmtTime(m.timeMs));
        head += QLatin1String("</span>");
        html += QStringLiteral("<div class='msg'>%1<br/>%2</div>").arg(head, body);
    }
    if (m_chat.size() > max)
        html += QStringLiteral("<p style='color:%1'>仅显示前 %2 条（共 %3 条）。</p>")
                    .arg(pal.fg4.name()).arg(max).arg(m_chat.size());
    return html;
}
