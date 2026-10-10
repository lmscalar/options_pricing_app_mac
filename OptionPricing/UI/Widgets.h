//
//  Widgets.h
//  OptionPricing
//
//  Small factory helpers that keep the tabs' construction code short and consistent.
//

#pragma once

#include "QtHeaders.h"
#include "Theme.h"

#include <functional>

namespace ui {

QDoubleSpinBox* makeSpinBox(QWidget* parent, double minimum, double maximum, double step, int decimals,
                            double initial, const QString& suffix = QString());

QSpinBox* makeIntSpinBox(QWidget* parent, int minimum, int maximum, int initial);

/// Right-aligned, selectable monospace value label.
QLabel* makeValueLabel(QWidget* parent, const QString& objectName = QString());

/// A rounded card with a small caption, a large value and an optional subtitle line.
struct Card {
    QFrame* frame = nullptr;
    QLabel* title = nullptr;
    QLabel* value = nullptr;
    QLabel* subtitle = nullptr;
};
Card makeCard(QWidget* parent, const QString& title, const QString& valueObjectName = QStringLiteral("bigValue"));

QPushButton* makeButton(QWidget* parent, const QString& text, const QString& objectName, const QString& tooltip = QString());

QFrame* makeSeparator(QWidget* parent);

/// Swaps a label between the muted, warning and error styles and forces a restyle.
enum class StatusKind { Info, Warning, Error };
void setStatus(QLabel* label, const QString& text, StatusKind kind);

/// Re-applies the stylesheet after changing a widget's objectName.
void restyle(QWidget* widget);

/// Configures a chart view with antialiasing and a sensible minimum size.
QChartView* makeChartView(QWidget* parent, QChart* chart, int minimumHeight = 260);

/// A chart view with a hover readout: a dashed crosshair at the cursor's x and a box with the
/// lines `readout(x)` returns. `probe` is any series attached to the axes used for mapping
/// pixels to values; with no probe or no readout the view behaves like a plain QChartView.
class HoverChartView : public QChartView
{
public:
    explicit HoverChartView(QChart* chart, QWidget* parent = nullptr);
    std::function<QStringList(double x)> readout;
    QAbstractSeries* probe = nullptr;
    QColor lineColour = QColor("#94a3b8");
    QColor boxBackground = QColor(15, 23, 42, 235);
    QColor boxBorder = QColor("#334155");
    QColor textColour = QColor("#e2e8f0");

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QPointF m_cursor;
    bool m_hovering = false;
};
HoverChartView* makeHoverChartView(QWidget* parent, QChart* chart, int minimumHeight = 260);

/// Modal, scrollable rich-text help page styled for the current theme (headings in the accent colour).
void showHelpDialog(QWidget* parent, const Theme& theme, const QString& title, const QString& html);

/// Company icon scaled into a rounded square of `size` device-independent pixels.
QPixmap roundedLogo(const QImage& image, int size, qreal devicePixelRatio);

/// Fallback badge: the first letters of `text` on a rounded square.
QPixmap monogramBadge(const QString& text, const QColor& background, const QColor& foreground, int size, qreal devicePixelRatio);

/// Non-editable table cell with right alignment.
QTableWidgetItem* makeCell(const QString& text, Qt::Alignment alignment = Qt::AlignRight | Qt::AlignVCenter);

/// A four-point diamond that spins while something is in progress (the assistant
/// thinking). Hidden when idle so layouts do not reserve space for it.
class SpinningDiamond : public QWidget
{
public:
    explicit SpinningDiamond(QWidget* parent = nullptr, int size = 18);
    void start();
    void stop();
    bool active() const { return m_timer->isActive(); }
    void setColor(const QColor& color) { m_color = color; update(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QTimer* m_timer = nullptr;
    qreal m_angle = 0.0;
    QColor m_color;
    int m_size;
};

} // namespace ui
