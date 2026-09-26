#pragma once
// MainWindow.h — topbar + page stack + health polling + toast + screenshot
// tour (ported structure from zcode-monitor).

#include <QFontMetrics>
#include <QLabel>
#include <QMainWindow>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVariantAnimation>
#include <QVector>

#include "core/Types.h"

class QButtonGroup;
class QStackedWidget;
class QToolButton;
class Dot;
class QGraphicsOpacityEffect;
class OverviewPage;
class UsageStatsPage;
class SessionsPage;
class ModelsPage;
class RawPage;
class SourcesPage;

// 顶栏信息标签：宽度不够时按真实宽度省略（绝不留半截字），并且可以被压到
// 0 —— 普通 QLabel 的最小宽度等于整段文字宽，会把右侧按钮顶出窗口
class ElideLabel : public QLabel {
public:
    explicit ElideLabel(QWidget* parent = nullptr);

    void setContent(const QString& plain, const QString& rich);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void applyElide();

    QString m_plain, m_rich;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    void applyFrost();
    void screenshotTour(const QString& outDir);

protected:
    void showEvent(QShowEvent* e) override;
    void changeEvent(QEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    QWidget* buildTopbar();
    void navigateTo(const QString& page);
    void healthTick();
    void renderHealthMeta(const QVector<types::AgentStatus>& statuses);
    void onThemeClicked();
    void onSettingsClicked();
    void showToast(const QString& msg);
    void updateThemeButton();
    void tourAdvance();

    struct TourStep {
        QString name;
        int action = 0; // 0 nav, 1 open session, 3 light, 4 dark
        QString page;
        QString agentId, sessionId;
        int tabIndex = -1;
    };

    Dot* m_statusDot = nullptr;
    ElideLabel* m_meta = nullptr;
    QButtonGroup* m_navGroup = nullptr;
    QToolButton* m_themeBtn = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_toast = nullptr;
    QTimer m_toastTimer;
    QTimer m_healthTimer;
    QTimer m_liveHide; // 顶栏实时胶囊静默后自动隐藏
    QLabel* m_liveChip = nullptr;
    QGraphicsOpacityEffect* m_toastFx = nullptr;
    QPropertyAnimation m_toastAnim;
    QVariantAnimation m_toastSlide;

    OverviewPage* m_overviewPage = nullptr;
    UsageStatsPage* m_usagePage = nullptr;
    SessionsPage* m_sessionsPage = nullptr;
    ModelsPage* m_modelsPage = nullptr;
    RawPage* m_rawPage = nullptr;
    SourcesPage* m_sourcesPage = nullptr;

    // tour state
    QString m_tourDir;
    QVector<TourStep> m_tourSteps;
    int m_tourIndex = 0;
    bool m_lastScanOk = true;
    bool m_tourDark = true;   // user's theme/frost, restored when the tour ends
    bool m_tourFrost = true;
};
