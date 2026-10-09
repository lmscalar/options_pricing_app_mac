//
//  Widgets.h
//  OptionPricing
//
//  Small factory helpers that keep the tabs' construction code short and consistent.
//

#pragma once

#include "QtHeaders.h"

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
