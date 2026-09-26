// MainWindow.cpp — see MainWindow.h.

#include "MainWindow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDate>
#include <QDialog>
#include <QFrame>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Store.h"
#include "ui/Theme.h"
#include "ui/UiUtil.h"
#include "ui/pages/ModelsPage.h"
#include "ui/pages/OverviewPage.h"
#include "ui/pages/RawPage.h"
#include "ui/pages/SessionsPage.h"
#include "ui/pages/SourcesPage.h"
#include "ui/pages/UsageStatsPage.h"
#include "util/Async.h"
#include "util/Frost.h"
#include "util/Format.h"

using namespace UiUtil;

// ── ElideLabel ────────────────────────────────────────────────

ElideLabel::ElideLabel(QWidget* parent)
    : QLabel(parent)
{
    setTextFormat(Qt::RichText);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void ElideLabel::setContent(const QString& plain, const QString& rich)
{
    m_plain = plain;
    m_rich = rich;
    applyElide();
    updateGeometry();
}

QSize ElideLabel::sizeHint() const
{
    // 始终按完整文本申报宽度（与当前显示的可能已省略的文本无关），
    // 否则省略一次之后标签就再也长不回完整文本
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(m_plain) + 4, QLabel::sizeHint().height());
}

QSize ElideLabel::minimumSizeHint() const
{
    return QSize(0, QLabel::minimumSizeHint().height());
}

void ElideLabel::resizeEvent(QResizeEvent* e)
{
    QLabel::resizeEvent(e);
    applyElide();
}

void ElideLabel::applyElide()
{
    if (m_plain.isEmpty()) {
        QLabel::setText(QString());
        return;
    }
    const QFontMetrics fm(font());
    const int avail = qMax(0, width() - 4);
    if (fm.horizontalAdvance(m_plain) <= avail) {
        if (QLabel::text() != m_rich)
            QLabel::setText(m_rich);
        return;
    }
    const QString elided = fm.elidedText(m_plain, Qt::ElideRight, avail);
    QLabel::setText(QStringLiteral("<span style='color:%1'>%2</span>")
                        .arg(Theme::instance().pal().fg4.name(),
                             elided.toHtmlEscaped()));
}

namespace {

// 自绘氛围背景：深色基底 + 漂移的蓝/紫光晕。光晕在 1/6 分辨率的离屏
// 位图上渲染再平滑放大 —— 天然的高斯模糊质感，且每帧成本可忽略
class AuroraBackground : public QWidget {
public:
    explicit AuroraBackground(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("centralHost"));
        // 2fps：光晕是模糊渐变，步进不可感知；再快就是整窗重绘，
        // 表格/图表全跟着陪跑，得不偿失
        m_drift.setInterval(500);
        connect(&m_drift, &QTimer::timeout, this, [this] {
            // 最小化时不重绘（也不白白重算离屏光晕）
            if (window() && (window()->windowState() & Qt::WindowMinimized))
                return;
            m_phase += 0.35;
            rerender();
            update(); // 光晕实际可见地漂移 —— 没有这一行，重算的帧从未上屏
        });
        connect(&Theme::instance(), &Theme::changed, this, [this] {
            rerender();
            update();
            // frost flipped while visible — keep the drift in sync
            if (Theme::instance().isFrost() && isVisible() && !m_drift.isActive())
                m_drift.start();
            else if (!Theme::instance().isFrost())
                m_drift.stop();
        });
    }

    void startDrift()
    {
        rerender();
        if (Theme::instance().isFrost() && !m_drift.isActive())
            m_drift.start();
    }
    void stopDrift() { m_drift.stop(); }

protected:
    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        startDrift();
    }
    void hideEvent(QHideEvent* e) override
    {
        QWidget::hideEvent(e);
        stopDrift();
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        rerender();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (m_buffer.isNull()) {
            const Palette& pal = Theme::instance().pal();
            p.fillRect(rect(), pal.bg);
            return;
        }
        p.drawPixmap(rect(), m_buffer);
    }

private:
    void rerender()
    {
        const Palette& pal = Theme::instance().pal();
        const QSize small = QSize(qMax(8, width() / 6), qMax(8, height() / 6));
        if (m_buffer.size() != small)
            m_buffer = QPixmap(small);
        QPainter p(&m_buffer);
        p.fillRect(QRect(QPoint(0, 0), small), pal.bg);

        if (Theme::instance().isFrost()) {
            p.setRenderHint(QPainter::Antialiasing);
            const double w = small.width(), h = small.height();
            auto glow = [&](double cx, double cy, double radius,
                            const QColor& base, int alpha) {
                // 漂移：光晕中心随相位缓慢摆动
                const QPointF c(cx * w + std::sin(m_phase * 0.9 + cx * 7.0) * w * 0.03,
                                cy * h + std::cos(m_phase * 0.7 + cy * 5.0) * h * 0.03);
                QRadialGradient g(c, qMax(8.0, radius * w));
                QColor c0 = base;
                c0.setAlpha(alpha);
                QColor c1 = base;
                c1.setAlpha(0);
                g.setColorAt(0, c0);
                g.setColorAt(1, c1);
                p.fillRect(QRect(QPoint(0, 0), small), QBrush(g));
            };
            glow(0.12, 0.02, 0.55, pal.accent, 18);
            glow(0.95, 0.95, 0.6, pal.accent2, 13);
            glow(0.75, 0.08, 0.35, pal.catTool2, 10);
        }
    }

    QPixmap m_buffer;
    QTimer m_drift;
    double m_phase = 0;
};

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("agent-monitor"));
    setAttribute(Qt::WA_TranslucentBackground);
    // remember window size/position across runs
    QSettings winSettings;
    const QByteArray geo = winSettings.value(QStringLiteral("win/geometry")).toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
    else
        resize(1500, 920);
    setMinimumSize(1080, 660);

    auto* central = new AuroraBackground(this);
    central->setObjectName(QStringLiteral("centralHost")); // single tint layer over acrylic
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(buildTopbar());

    m_stack = new QStackedWidget;
    m_overviewPage = new OverviewPage;
    m_usagePage = new UsageStatsPage;
    m_sessionsPage = new SessionsPage;
    m_modelsPage = new ModelsPage;
    m_rawPage = new RawPage;
    m_sourcesPage = new SourcesPage;
    m_stack->addWidget(m_overviewPage);
    m_stack->addWidget(m_usagePage);
    m_stack->addWidget(m_sessionsPage);
    m_stack->addWidget(m_modelsPage);
    m_stack->addWidget(m_rawPage);
    m_stack->addWidget(m_sourcesPage);
    outer->addWidget(m_stack, 1);
    setCentralWidget(central);

    // toast overlay
    m_toast = new QLabel(central);
    m_toast->setObjectName(QStringLiteral("toast"));
    m_toast->hide();
    m_toastTimer.setSingleShot(true);
    m_toastTimer.setInterval(2400);
    connect(&m_toastTimer, &QTimer::timeout, m_toast, &QLabel::hide);

    connect(&Theme::instance(), &Theme::changed, this, [this](bool) {
        updateThemeButton();
        applyFrost();
        m_statusDot->setColor(m_lastScanOk ? Theme::instance().pal().sevOk
                                           : Theme::instance().pal().sevErr);
        healthTick(); // 顶栏信息里的品牌色/muted 色随主题立即刷新
    });

    auto* sc = new QShortcut(QKeySequence(Qt::Key_T), this);
    connect(sc, &QShortcut::activated, this, [this]() {
        Theme::instance().toggle();
    });

    // health loop (scan state + agent dirs, every 5s)
    connect(&m_healthTimer, &QTimer::timeout, this, &MainWindow::healthTick);
    m_healthTimer.start(5000);
    healthTick();

    connect(&Store::instance(), &Store::scanFinished, this,
            [this](bool ok, const QString&, int changed, int) {
                m_lastScanOk = ok;
                if (changed > 0)
                    healthTick(); // refresh counts after new data
            }, Qt::QueuedConnection);

    navigateTo(QStringLiteral("overview"));

    // Ctrl+1..6 快捷切页
    const QStringList pageOrder = { "overview", "usage", "sessions", "models",
                                    "raw", "sources" };
    for (int i = 0; i < pageOrder.size(); ++i) {
        auto* sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this);
        const QString pg = pageOrder.at(i);
        connect(sc, &QShortcut::activated, this, [this, pg] { navigateTo(pg); });
    }
}

void MainWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    // 启动即轮询：这是常驻监控，「会话 / 模型」等页面同样靠扫描驱动的
    // dataChanged 刷新 —— 轮询不应只属于实时监控页。最小化由 changeEvent 暂停。
    Store::instance().startPolling();
}

void MainWindow::changeEvent(QEvent* e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange)
        Store::instance().setPollPaused(isMinimized()); // 最小化：暂停后台扫描
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    QSettings settings;
    settings.setValue(QStringLiteral("win/geometry"), saveGeometry());
    QMainWindow::closeEvent(e);
}

QWidget* MainWindow::buildTopbar()
{
    auto* bar = new QFrame;
    bar->setObjectName(QStringLiteral("topbar"));
    bar->setFixedHeight(44);
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(14, 0, 14, 0);
    lay->setSpacing(12);

    m_statusDot = new Dot(8);
    m_statusDot->setColor(Theme::instance().pal().sevOk);

    auto* brand = new QLabel(QStringLiteral("agent-monitor"));
    brand->setObjectName(QStringLiteral("brand"));

    struct Nav {
        const char* id;
        const char* label;
    };
    const QVector<Nav> navs = {
        { "overview", "◉ 实时监控" }, { "usage", "◎ 使用统计" }, { "sessions", "▤ 会话" },
        { "models", "◇ 模型用量" }, { "raw", "≣ 原始数据" }, { "sources", "▦ 数据源" },
    };
    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);
    for (const Nav& n : navs) {
        auto* btn = new QToolButton;
        btn->setText(QString::fromUtf8(n.label));
        btn->setProperty("cls", "nav");
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        const QString page = QString::fromLatin1(n.id);
        connect(btn, &QToolButton::clicked, this, [this, page] { navigateTo(page); });
        m_navGroup->addButton(btn);
        lay->addWidget(btn);
    }

    lay->addSpacing(6);
    lay->addStretch(1);

    m_meta = new ElideLabel;
    m_meta->setObjectName(QStringLiteral("metaLabel"));
    m_meta->setContent(QStringLiteral("—"), QStringLiteral("—"));
    lay->addWidget(m_meta);

    // 顶栏实时速度胶囊：agent 有新响应时浮现，静默 6s 自动隐藏
    m_liveChip = new QLabel;
    m_liveChip->setObjectName(QStringLiteral("liveChip"));
    m_liveChip->setTextFormat(Qt::RichText);
    m_liveChip->hide();
    lay->addWidget(m_liveChip);
    m_liveHide.setSingleShot(true);
    m_liveHide.setInterval(6000);
    connect(&m_liveHide, &QTimer::timeout, m_liveChip, &QLabel::hide);
    connect(&Store::instance(), &Store::newRecords, this,
            [this](const QVector<types::UsageRecord>& rows) {
                if (rows.isEmpty())
                    return;
                const types::UsageRecord& r = rows.last();
                const QString speed = r.tpsOk
                    ? (r.durApprox ? QStringLiteral("≈") : QString())
                          + QString::number(r.tps, 'f', 1) + QStringLiteral(" t/s")
                    : QStringLiteral("in ") + Format::fmtNum(double(r.inputTokens));
                QColor c = Theme::instance().pal().accent;
                if (AgentAdapter* a = Store::instance().adapter(r.agentId))
                    c = a->color();
                const QString modelPart = r.model.isEmpty()
                    ? QString() : QStringLiteral(" · ") + r.model.toHtmlEscaped();
                m_liveChip->setText(QStringLiteral(
                    "<span style='color:%1'>●</span> %2%3")
                    .arg(c.name(), speed, modelPart));
                m_liveChip->show();
                m_liveHide.start();
            }, Qt::QueuedConnection);

    auto* settingsBtn = new QToolButton;
    settingsBtn->setText(QStringLiteral("⚙"));
    settingsBtn->setProperty("cls", "ghost");
    settingsBtn->setCursor(Qt::PointingHandCursor);
    settingsBtn->setToolTip(QStringLiteral("设置（归档保留天数）"));
    connect(settingsBtn, &QToolButton::clicked, this, &MainWindow::onSettingsClicked);
    lay->addWidget(settingsBtn);

    m_themeBtn = new QToolButton;
    m_themeBtn->setProperty("cls", "ghost");
    m_themeBtn->setCursor(Qt::PointingHandCursor);
    m_themeBtn->setToolTip(QStringLiteral("切换主题 (t)"));
    connect(m_themeBtn, &QToolButton::clicked, this, &MainWindow::onThemeClicked);
    lay->addWidget(m_themeBtn);
    updateThemeButton();

    auto* frostBtn = new QToolButton;
    frostBtn->setText(QStringLiteral("霜"));
    frostBtn->setProperty("cls", "ghost");
    frostBtn->setCursor(Qt::PointingHandCursor);
    frostBtn->setToolTip(QStringLiteral("氛围光晕 开/关"));
    frostBtn->setCheckable(true);
    frostBtn->setChecked(Theme::instance().isFrost());
    connect(frostBtn, &QToolButton::clicked, this, [this, frostBtn](bool on) {
        Theme::instance().setFrost(on);
        frostBtn->setChecked(Theme::instance().isFrost());
    });
    lay->addWidget(frostBtn);

    lay->insertWidget(0, m_statusDot);
    lay->insertWidget(1, brand);
    return bar;
}

void MainWindow::updateThemeButton()
{
    if (m_themeBtn)
        m_themeBtn->setText(Theme::instance().isDark() ? QStringLiteral("🌙")
                                                        : QStringLiteral("☀️"));
}

void MainWindow::navigateTo(const QString& page)
{
    static const QStringList order = { "overview", "usage", "sessions", "models",
                                       "raw", "sources" };
    int idx = order.indexOf(page);
    if (idx < 0)
        idx = 0;
    m_stack->setCurrentIndex(idx);
    const auto buttons = m_navGroup->buttons();
    if (idx < buttons.size()) {
        m_navGroup->blockSignals(true);
        buttons[idx]->setChecked(true);
        m_navGroup->blockSignals(false);
    }
    // 页面切换淡入（灵动）。Rapid-switch guard：如果已有过渡效果在跑
    // （setGraphicsEffect 会 delete 旧 effect，而旧 animation 的 target
    //  指向它 → 悬空指针崩溃），跳过本次
    if (QWidget* w = m_stack->currentWidget()) {
        if (w->graphicsEffect())
            return;
        auto* fx = new QGraphicsOpacityEffect(w);
        w->setGraphicsEffect(fx);
        auto* anim = new QPropertyAnimation(fx, "opacity", w);
        anim->setDuration(180);
        anim->setStartValue(0.55);
        anim->setEndValue(1.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(anim, &QPropertyAnimation::finished, w,
                [w] { w->setGraphicsEffect(nullptr); });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void MainWindow::healthTick()
{
    // directory walks run off-thread — the GUI thread only renders the result
    Async::run<QVector<types::AgentStatus>>(
        this,
        []() { return Store::instance().agentStatuses(); },
        [this](const QVector<types::AgentStatus>& statuses) {
            renderHealthMeta(statuses);
        });
}

void MainWindow::renderHealthMeta(const QVector<types::AgentStatus>& statuses)
{
    const Palette& pal = Theme::instance().pal();
    m_statusDot->setColor(m_lastScanOk ? pal.sevOk : pal.sevErr);

    // 只列有数据的 agent（各用自己的品牌色），未检测的收成一个计数，
    // 完整清单放 tooltip —— 一长串"无"会把顶栏挤爆且零信息量
    QStringList rich, plain, tip;
    int missing = 0;
    for (const types::AgentStatus& s : statuses) {
        tip << QStringLiteral("%1：%2").arg(
            s.name,
            s.found ? QStringLiteral("%1 个会话").arg(s.sessions)
                    : QStringLiteral("未检测"));
        if (!s.found) {
            ++missing;
            continue;
        }
        const QColor c = s.color.isValid() ? s.color : pal.fg3;
        const QString seg = QStringLiteral("%1 %2").arg(s.id).arg(s.sessions);
        rich << QStringLiteral("<span style='color:%1'>%2</span>").arg(c.name(), seg);
        plain << seg;
    }
    if (missing > 0) {
        const QString seg = QStringLiteral("未检测 %1").arg(missing);
        rich << QStringLiteral("<span style='color:%1'>%2</span>").arg(pal.fg4.name(), seg);
        plain << seg;
    }
    if (Store::instance().lastScanMs() > 0) {
        const QString seg = QStringLiteral("扫描 %1ms").arg(Store::instance().lastScanMs());
        rich << QStringLiteral("<span style='color:%1'>%2</span>").arg(pal.fg4.name(), seg);
        plain << seg;
    }

    m_meta->setToolTip(tip.join(QStringLiteral("\n")));
    const QString sep = QStringLiteral("<span style='color:%1'> · </span>").arg(pal.fg5.name());
    m_meta->setContent(plain.join(QStringLiteral(" · ")), rich.join(sep));
}

void MainWindow::onThemeClicked()
{
    Theme::instance().toggle();
}

void MainWindow::onSettingsClicked()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("设置"));
    dlg.setMinimumWidth(480);
    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(12);

    // ── 归档保留：卡片 + 实时预览生效日期 ──
    auto* card = new CardFrame(&dlg);
    auto* cl = new QVBoxLayout(card);
    cl->setContentsMargins(16, 14, 16, 14);
    cl->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("历史归档保留"), card);
    title->setObjectName(QStringLiteral("h2"));
    cl->addWidget(title);
    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    auto* lab = new QLabel(QStringLiteral("保留天数"), card);
    lab->setProperty("cls", "muted");
    row->addWidget(lab);
    auto* spin = new QSpinBox(card);
    spin->setRange(30, 3650);
    spin->setValue(int(Store::instance().retentionDays()));
    spin->setSuffix(QStringLiteral(" 天"));
    spin->setAlignment(Qt::AlignRight);
    row->addWidget(spin, 1);
    cl->addLayout(row);
    // 拖动天数时立刻看到"数据会从哪天起被清"，不用猜
    auto* cutoff = new QLabel(card);
    cutoff->setProperty("cls", "muted");
    cutoff->setWordWrap(true);
    const auto updateCutoff = [cutoff](int days) {
        const QDate cut = QDate::currentDate().addDays(-qMax(30, days));
        cutoff->setText(QStringLiteral(
            "生效后保留 %1 之后的数据；默认 730 天（约 2 年）。超出保留期的"
            "历史在下次合并时清除，调整立即生效。")
            .arg(cut.toString(QStringLiteral("yyyy/M/d"))));
    };
    updateCutoff(spin->value());
    connect(spin, &QSpinBox::valueChanged, &dlg, updateCutoff);
    cl->addWidget(cutoff);
    lay->addWidget(card);

    const QFileInfo archiveInfo(Store::archivePath());
    auto* archiveInfoLabel = new QLabel(
        QStringLiteral("当前归档：%1 条记录 · %2 KB · %3")
            .arg(Store::instance().archiveEntryCount())
            .arg(qMax<qint64>(0, archiveInfo.size()) / 1024)
            .arg(Store::archivePath()),
        &dlg);
    archiveInfoLabel->setProperty("cls", "mono-faint");
    archiveInfoLabel->setWordWrap(true);
    lay->addWidget(archiveInfoLabel);

    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消"), &dlg);
    auto* save = new QPushButton(QStringLiteral("保存"), &dlg);
    save->setDefault(true);
    btns->addWidget(cancel);
    btns->addWidget(save);
    lay->addLayout(btns);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dlg, &QDialog::accept);
    if (dlg.exec() == QDialog::Accepted) {
        Store::instance().setRetentionDays(spin->value());
        showToast(QStringLiteral("归档保留 %1 天").arg(spin->value()));
    }
}

void MainWindow::showToast(const QString& msg)
{
    m_toast->setText(msg);
    m_toast->adjustSize();
    const int x = centralWidget()->width() - m_toast->width() - 18;
    const int y = centralWidget()->height() - m_toast->height() - 18;
    m_toast->move(qMax(0, x), qMax(0, y));
    m_toast->raise();
    m_toast->show();
    // slide-up + fade-in（灵动感）
    if (!m_toastFx) {
        m_toastFx = new QGraphicsOpacityEffect(m_toast);
        m_toast->setGraphicsEffect(m_toastFx);
        m_toastAnim.setDuration(260);
        m_toastAnim.setEasingCurve(QEasingCurve::OutCubic);
        m_toastAnim.setStartValue(0.0);
        m_toastAnim.setEndValue(1.0);
        m_toastSlide.setDuration(260);
        m_toastSlide.setEasingCurve(QEasingCurve::OutCubic);
        m_toastSlide.setStartValue(0.0);
        m_toastSlide.setEndValue(1.0);
        connect(&m_toastAnim, &QPropertyAnimation::valueChanged, this,
                [this](const QVariant& v) {
                    if (m_toastFx)
                        m_toastFx->setOpacity(v.toDouble());
                });
        connect(&m_toastSlide, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& v) {
                    const double k = v.toDouble();
                    const int x = centralWidget()->width() - m_toast->width() - 18;
                    const int y = centralWidget()->height() - m_toast->height() - 18
                                  + int((1.0 - k) * 16);
                    m_toast->move(qMax(0, x), qMax(0, y));
                });
    }
    m_toastAnim.stop();
    m_toastSlide.stop();
    m_toastAnim.start();
    m_toastSlide.start();
    m_toastTimer.start();
}

void MainWindow::applyFrost()
{
    Frost::apply(this, Theme::instance().isFrost(), Theme::instance().isDark());
}

// ── GUI assembly ─────────────────────────────────────────────

namespace {

QPixmap appIconPixmap()
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QRect r(2, 2, 60, 60);
    QLinearGradient g(0, 0, 0, 64);
    g.setColorAt(0, QColor(0x38, 0xbd, 0xf8));
    g.setColorAt(1, QColor(0x8b, 0x5c, 0xf6));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(r, 14, 14);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(26);
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, QStringLiteral("AM"));
    p.end();
    return pm;
}

} // namespace

int runGui(QApplication& app, const QString& screenshotDir)
{
    QApplication::setApplicationName(QStringLiteral("agent-monitor"));
    QApplication::setOrganizationName(QStringLiteral("zhipucode"));
    QFont base(QStringLiteral("Segoe UI"), 10);
    QApplication::setFont(base);
    Theme::instance().init();
    app.setWindowIcon(QIcon(appIconPixmap()));

    QString cacheErr;
    if (!Store::instance().openCache(&cacheErr))
        qWarning("cache db unavailable: %s", qPrintable(cacheErr));
    if (!screenshotDir.isNull())
        Store::instance().scan(); // deterministic data before pages load

    MainWindow w;
    w.show();
    w.applyFrost();
    if (!screenshotDir.isNull())
        w.screenshotTour(screenshotDir);
    return app.exec();
}

// ── screenshot tour (automated visual verification) ──────────

void MainWindow::screenshotTour(const QString& outDir)
{
    // 巡检需要确定性画面（固定暗色 + 关霜），但退出时必须还原用户自己的
    // 设置 —— 否则每跑一次截图就把用户的主题/毛玻璃开关改掉
    m_tourDark = Theme::instance().isDark();
    m_tourFrost = Theme::instance().isFrost();
    Theme::instance().setFrost(false);
    Theme::instance().setTheme(true);

    // a session with actual content for the detail shots
    QString agentId, sessionId;
    const auto sessions = Store::instance().sessionList(QString(), QString(), 50);
    if (!sessions.isEmpty()) {
        agentId = sessions.first().agentId;
        sessionId = sessions.first().sessionId;
    }

    m_tourDir = outDir;
    m_tourSteps.clear();
    m_tourSteps.push_back({ QStringLiteral("01-overview"), 0, QStringLiteral("overview"),
                            QString(), QString(), -1 });
    m_tourSteps.push_back({ QStringLiteral("02-usage"), 0, QStringLiteral("usage"),
                            QString(), QString(), -1 });
    m_tourSteps.push_back({ QStringLiteral("03-sessions"), 0, QStringLiteral("sessions"),
                            QString(), QString(), -1 });
    if (!sessionId.isEmpty()) {
        m_tourSteps.push_back({ QStringLiteral("04-session-overview"), 1, QString(),
                                agentId, sessionId, 0 });
        m_tourSteps.push_back({ QStringLiteral("05-session-context"), 1, QString(),
                                agentId, sessionId, 1 });
        m_tourSteps.push_back({ QStringLiteral("06-session-usage"), 1, QString(),
                                agentId, sessionId, 2 });
    }
    m_tourSteps.push_back({ QStringLiteral("07-models"), 0, QStringLiteral("models"),
                            QString(), QString(), -1 });
    m_tourSteps.push_back({ QStringLiteral("08-raw"), 0, QStringLiteral("raw"),
                            QString(), QString(), -1 });
    m_tourSteps.push_back({ QStringLiteral("09-sources"), 0, QStringLiteral("sources"),
                            QString(), QString(), -1 });
    m_tourSteps.push_back({ QStringLiteral("10-overview-light"), 3,
                            QStringLiteral("overview"), QString(), QString(), -1 });
    m_tourSteps.push_back({ QStringLiteral("11-overview-dark-back"), 4,
                            QStringLiteral("overview"), QString(), QString(), -1 });
    m_tourIndex = 0;

    // tall from the start: a resize issued right after show() can be clamped
    // by the window manager while the frame settles — set it early
    resize(1500, 2900);
    QTimer::singleShot(1500, this, &MainWindow::tourAdvance);
}

void MainWindow::tourAdvance()
{
    if (m_tourIndex >= m_tourSteps.size()) {
        Theme::instance().setTheme(m_tourDark);
        Theme::instance().setFrost(m_tourFrost); // 还原用户设置
        QApplication::quit();
        return;
    }
    const TourStep s = m_tourSteps.at(m_tourIndex);
    if (s.action == 3)
        Theme::instance().setTheme(false);
    else if (s.action == 4)
        Theme::instance().setTheme(true);

    // overview shots use a tall window so the whole scrolling page (speed
    // footer, tool table…) is visible to the reviewer, not just the fold
    resize(s.action == 1 ? QSize(1500, 920) : QSize(1500, 2900));

    if (s.action == 0 || s.action == 3 || s.action == 4) {
        navigateTo(s.page);
    } else if (s.action == 1) {
        navigateTo(QStringLiteral("sessions"));
        m_sessionsPage->openSession(s.agentId, s.sessionId);
        if (s.tabIndex >= 0) {
            if (QTabWidget* tabs = m_sessionsPage->findChild<QTabWidget*>())
                tabs->setCurrentIndex(s.tabIndex);
        }
    }

    QTimer::singleShot(1100, this, [this, s] {
        const QPixmap pm = grab();
        pm.save(m_tourDir + QLatin1Char('/') + s.name + QStringLiteral(".png"));
        printf("[shot] %s\n", qPrintable(s.name));
        fflush(stdout);
        ++m_tourIndex;
        tourAdvance();
    });
}
