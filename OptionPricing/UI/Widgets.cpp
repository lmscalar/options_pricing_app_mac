//
//  Widgets.cpp
//  OptionPricing
//

#include "Widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtWidgets/QTextBrowser>
#include <algorithm>

#include <cmath>

namespace ui {

QDoubleSpinBox* makeSpinBox(QWidget* parent, double minimum, double maximum, double step, int decimals,
                            double initial, const QString& suffix)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setValue(initial);
    box->setSuffix(suffix);
    box->setAlignment(Qt::AlignRight);
    box->setKeyboardTracking(false);
    box->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    return box;
}

QSpinBox* makeIntSpinBox(QWidget* parent, int minimum, int maximum, int initial)
{
    auto* box = new QSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setValue(initial);
    box->setAlignment(Qt::AlignRight);
    box->setKeyboardTracking(false);
    return box;
}

QLabel* makeValueLabel(QWidget* parent, const QString& objectName)
{
    auto* label = new QLabel(QStringLiteral("–"), parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setObjectName(objectName.isEmpty() ? QStringLiteral("value") : objectName);
    return label;
}

Card makeCard(QWidget* parent, const QString& title, const QString& valueObjectName)
{
    Card card;
    card.frame = new QFrame(parent);
    card.frame->setObjectName("card");
    card.frame->setAttribute(Qt::WA_StyledBackground, true);

    // Headings are upper-case Latin so cards read the same on every tab; Greek letters
    // (σ, λ) keep their case.
    QString heading;
    for (const QChar ch : title) heading += ch.unicode() < 128 ? ch.toUpper() : ch;
    card.title = new QLabel(heading, card.frame);
    card.title->setObjectName("cardTitle");
    card.title->setAlignment(Qt::AlignCenter);

    card.value = new QLabel(QStringLiteral("–"), card.frame);
    card.value->setObjectName(valueObjectName);
    card.value->setAlignment(Qt::AlignCenter);
    card.value->setTextInteractionFlags(Qt::TextSelectableByMouse);

    card.subtitle = new QLabel(QString(), card.frame);
    card.subtitle->setObjectName("cardSubtitle");
    card.subtitle->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    card.subtitle->setWordWrap(true);
    // Exactly two lines, whatever the text: every card in a row (and every row) then has
    // the same height, so bottom edges line up.
    card.subtitle->setFixedHeight(card.subtitle->fontMetrics().lineSpacing() * 2 + 2);
    card.subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    // The heading band spans the card's full width; the value and subtitle sit centred
    // in the body below it (vertically centred when a layout hands the card extra height).
    auto* layout = new QVBoxLayout(card.frame);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(0);
    layout->addWidget(card.title);
    auto* body = new QVBoxLayout;
    body->setContentsMargins(14, 8, 14, 0);
    body->setSpacing(3);
    body->addStretch(1);
    body->addWidget(card.value);
    body->addWidget(card.subtitle);
    body->addStretch(1);
    layout->addLayout(body, 1);
    return card;
}

QPushButton* makeButton(QWidget* parent, const QString& text, const QString& objectName, const QString& tooltip)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    return button;
}

QFrame* makeSeparator(QWidget* parent)
{
    auto* separator = new QFrame(parent);
    separator->setFrameShape(QFrame::HLine);
    separator->setObjectName("separator");
    return separator;
}

void setStatus(QLabel* label, const QString& text, StatusKind kind)
{
    switch (kind) {
    case StatusKind::Info:    label->setObjectName("muted"); break;
    case StatusKind::Warning: label->setObjectName("warning"); break;
    case StatusKind::Error:   label->setObjectName("error"); break;
    }
    label->setText(text);
    restyle(label);
}

void restyle(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QChartView* makeChartView(QWidget* parent, QChart* chart, int minimumHeight)
{
    auto* view = new QChartView(chart, parent);
    view->setRenderHint(QPainter::Antialiasing, true);
    view->setMinimumHeight(minimumHeight);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    view->setBackgroundBrush(Qt::NoBrush);
    view->setFrameShape(QFrame::NoFrame);
    return view;
}

HoverChartView::HoverChartView(QChart* chart, QWidget* parent)
    : QChartView(chart, parent)
{
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
}

void HoverChartView::mouseMoveEvent(QMouseEvent* event)
{
    m_cursor = event->position();
    m_hovering = true;
    viewport()->update();
    QChartView::mouseMoveEvent(event);
}

void HoverChartView::leaveEvent(QEvent* event)
{
    m_hovering = false;
    viewport()->update();
    QChartView::leaveEvent(event);
}

void HoverChartView::paintEvent(QPaintEvent* event)
{
    QChartView::paintEvent(event);
    if (!m_hovering || !probe || !readout || !chart()) return;
    const QRectF plot = chart()->plotArea();
    const QPointF chartPos = chart()->mapFromScene(mapToScene(m_cursor.toPoint()));
    if (!plot.contains(chartPos)) return;
    const QStringList lines = readout(chart()->mapToValue(chartPos, probe).x());
    if (lines.isEmpty()) return;

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Crosshair across the plot area at the cursor's x (viewport coordinates).
    const QPointF top = mapFromScene(chart()->mapToScene(QPointF(chartPos.x(), plot.top())));
    const QPointF bottom = mapFromScene(chart()->mapToScene(QPointF(chartPos.x(), plot.bottom())));
    painter.setPen(QPen(lineColour, 1, Qt::DashLine));
    painter.drawLine(top, bottom);

    QFont f = font();
    f.setPointSizeF(std::max(9.0, f.pointSizeF() - 1.0));
    painter.setFont(f);
    const QFontMetricsF fm(f);
    qreal width = 0;
    for (const QString& line : lines) width = std::max(width, fm.horizontalAdvance(line));
    const qreal lineHeight = fm.height();
    QRectF box(0, 0, width + 20, lines.size() * lineHeight + 12);
    // To the right of the cursor, flipped to the left near the edge, clamped vertically.
    qreal x = m_cursor.x() + 14;
    if (x + box.width() > viewport()->width() - 4) x = m_cursor.x() - 14 - box.width();
    const qreal y = std::clamp(m_cursor.y() - box.height() / 2, 4.0, std::max(4.0, viewport()->height() - box.height() - 4));
    box.moveTo(x, y);
    painter.setPen(QPen(boxBorder, 1));
    painter.setBrush(boxBackground);
    painter.drawRoundedRect(box, 6, 6);
    painter.setPen(textColour);
    for (int i = 0; i < lines.size(); ++i) {
        if (i == 0) { QFont bold = f; bold.setBold(true); painter.setFont(bold); } else painter.setFont(f);
        painter.drawText(QPointF(box.left() + 10, box.top() + 6 + fm.ascent() + i * lineHeight), lines[i]);
    }
}

HoverChartView* makeHoverChartView(QWidget* parent, QChart* chart, int minimumHeight)
{
    auto* view = new HoverChartView(chart, parent);
    view->setRenderHint(QPainter::Antialiasing, true);
    view->setMinimumHeight(minimumHeight);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    view->setBackgroundBrush(Qt::NoBrush);
    view->setFrameShape(QFrame::NoFrame);
    return view;
}

void showHelpDialog(QWidget* parent, const Theme& theme, const QString& title, const QString& html)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(780, 680);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 12, 14, 12);
    auto* text = new QTextBrowser(&dialog);
    text->setOpenExternalLinks(false);
    text->setFrameShape(QFrame::NoFrame);
    // The global stylesheet does not cover QTextBrowser: match the theme explicitly.
    const QString surface = theme.surface.isEmpty() ? QStringLiteral("#0f172a") : theme.surface;
    const QString strong = theme.textStrong.isEmpty() ? QStringLiteral("#e2e8f0") : theme.textStrong;
    const QString body = theme.text.isEmpty() ? strong : theme.text;
    const QString accent = theme.accent2.isEmpty() ? QStringLiteral("#f59e0b") : theme.accent2;
    text->setStyleSheet(QStringLiteral("QTextBrowser { background: %1; color: %2; border: none; font-size: 13px; padding: 6px; }").arg(surface, body));
    text->document()->setDefaultStyleSheet(QStringLiteral("h2, h3 { color: %1; } b { color: %2; } li { margin-bottom: 3px; }").arg(accent, strong));
    text->setHtml(html);
    layout->addWidget(text, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.exec();
}

QPixmap roundedLogo(const QImage& image, int size, qreal devicePixelRatio)
{
    const int px = static_cast<int>(size * devicePixelRatio);
    QPixmap out(px, px);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, px, px), px * 0.22, px * 0.22);
    painter.setClipPath(clip);
    const QImage scaled = image.scaled(px, px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    painter.drawImage(QRect((px - scaled.width()) / 2, (px - scaled.height()) / 2, scaled.width(), scaled.height()), scaled);
    painter.end();
    out.setDevicePixelRatio(devicePixelRatio);
    return out;
}

QPixmap monogramBadge(const QString& text, const QColor& background, const QColor& foreground, int size, qreal devicePixelRatio)
{
    const int px = static_cast<int>(size * devicePixelRatio);
    QPixmap out(px, px);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(background);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRectF(0, 0, px, px), px * 0.22, px * 0.22);
    QFont font("Menlo");
    font.setBold(true);
    const QString letters = text.left(text.size() >= 4 ? 2 : (text.size() >= 2 ? 2 : 1)).toUpper();
    font.setPixelSize(static_cast<int>(px * (letters.size() > 1 ? 0.42 : 0.55)));
    painter.setFont(font);
    painter.setPen(foreground);
    painter.drawText(QRect(0, 0, px, px), Qt::AlignCenter, letters);
    painter.end();
    out.setDevicePixelRatio(devicePixelRatio);
    return out;
}

SpinningDiamond::SpinningDiamond(QWidget* parent, int size)
    : QWidget(parent)
    , m_color("#22d3ee")
    , m_size(size)
{
    setFixedSize(size, size);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_timer = new QTimer(this);
    m_timer->setInterval(16);   // ~60 fps; one turn every 1.4 s
    QObject::connect(m_timer, &QTimer::timeout, this, [this] {
        m_angle += 360.0 * 16.0 / 1400.0;
        if (m_angle >= 360.0) m_angle -= 360.0;
        update();
    });
    hide();
}

void SpinningDiamond::start()
{
    if (!m_timer->isActive()) m_timer->start();
    show();
}

void SpinningDiamond::stop()
{
    m_timer->stop();
    m_angle = 0.0;
    hide();
}

void SpinningDiamond::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(width() / 2.0, height() / 2.0);
    p.rotate(m_angle);
    const qreal r = m_size / 2.0 - 1.0;
    const qreal inner = r * 0.42;
    // Four-point star: long points on the axes, short points on the diagonals.
    QPolygonF star;
    for (int i = 0; i < 8; ++i) {
        const qreal angle = i * M_PI / 4.0;
        const qreal radius = (i % 2 == 0) ? r : inner;
        star << QPointF(radius * std::cos(angle), radius * std::sin(angle));
    }
    QColor fill = m_color;
    QColor rim = m_color;
    rim.setAlphaF(0.55);
    p.setPen(QPen(rim, 1.0));
    p.setBrush(fill);
    p.drawPolygon(star);
}

QTableWidgetItem* makeCell(const QString& text, Qt::Alignment alignment)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setTextAlignment(alignment);
    return item;
}

} // namespace ui
