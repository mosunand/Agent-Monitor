#pragma once
// UiUtil.h — shared UI primitives: badges, cards, dots, spinner, status maps.
// (Ported from zcode-monitor.)

#include <QFrame>
#include <QHash>
#include <QLabel>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QWidget>

#include "Theme.h"

class QPainter;
class QRect;

class BadgeLabel : public QWidget {
    Q_OBJECT
public:
    explicit BadgeLabel(const QString& text, const QString& kind, QWidget* parent = nullptr);
    void setKind(const QString& kind);
    void setText(const QString& text);
    QString text() const { return m_text; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_text;
    QString m_kind;
};

class CardFrame : public QFrame {
    Q_OBJECT
public:
    explicit CardFrame(QWidget* parent = nullptr);
protected:
    // hover 浮起：进入时属性 hot=on，QSS 提亮边框 + 顶部内高光
    void enterEvent(QEnterEvent* e) override
    { setProperty("hot", true); style()->unpolish(this); style()->polish(this); QFrame::enterEvent(e); }
    void leaveEvent(QEvent* e) override
    { setProperty("hot", false); style()->unpolish(this); style()->polish(this); QFrame::leaveEvent(e); }
};

class Dot : public QWidget {
    Q_OBJECT
public:
    explicit Dot(int diameter = 8, QWidget* parent = nullptr);
    void setColor(const QColor& c);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QColor m_color;
    int m_d;
};

class SpinnerWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpinnerWidget(QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int m_angle = 0;
};

namespace UiUtil {

// status → badge kind
QString statusBadgeKind(const QString& status);
// token-speed tier (0 none,1 red,2 yellow,3 green) → color
QColor speedColor(int tier);
// category name → dot color
QColor catColor(const QString& category);
// a muted h2-style section label with optional sub text
QWidget* sectionLabel(const QString& title, const QString& sub = QString());
// 2px 横向渐变线（accent → accent2），页首专用
QWidget* gradientDivider();
// simple label helpers
QLabel* label(const QString& text, const QString& cls = QString());
// paint a rounded pill (badge / speed chip) for delegates
void paintPill(QPainter* p, const QRect& rect, const QString& text, const QColor& color,
               bool bold = false);

class ValueBarDelegate;

// 行悬停表格：悬停时整行高亮（一条横线）+ 可选数值列比例条
class RowHoverTable : public QTableWidget {
public:
    explicit RowHoverTable(QWidget* parent = nullptr);
    int hoverRow() const { return m_hoverRow; }
    // 比例条基准（0 = 该列不画条）
    void setColumnMax(int column, double max);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    int m_hoverRow = -1;
    ValueBarDelegate* m_bars = nullptr;
};

// 数值列微型比例条：以本列最大值为基准，在数字背后画一条淡色横条，
// 表格一眼看出量级。数值放在 Qt::UserRole + 1；同时保留行悬停高亮。
class ValueBarDelegate : public QStyledItemDelegate {
public:
    explicit ValueBarDelegate(RowHoverTable* table);
    void setColumnMax(int column, double max) { m_max.insert(column, max); }
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    const RowHoverTable* m_table;
    QHash<int, double> m_max;
};

// JSON 语法着色（响应记录对话框 / 会话 JSON 标签页共用）：颜色全部取自
// 当前主题 —— 写死配色会在浅色模式下不可读
QString jsonHighlightHtml(const QString& prettyJson, const Palette& pal);

// exact pixel height a table needs to show ALL its rows
int tableContentHeight(QTableWidget* t);
int tableContentHeight(QTableWidget* t, int maxHeight);

} // namespace UiUtil
