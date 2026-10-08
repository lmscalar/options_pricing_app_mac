//
//  Widgets.cpp
//  OptionPricing
//

#include "Widgets.h"

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

    card.title = new QLabel(title, card.frame);
    card.title->setObjectName("cardTitle");
    card.title->setAlignment(Qt::AlignCenter);

    card.value = new QLabel(QStringLiteral("–"), card.frame);
    card.value->setObjectName(valueObjectName);
    card.value->setAlignment(Qt::AlignCenter);
    card.value->setTextInteractionFlags(Qt::TextSelectableByMouse);

    card.subtitle = new QLabel(QString(), card.frame);
    card.subtitle->setObjectName("cardSubtitle");
    card.subtitle->setAlignment(Qt::AlignCenter);
    card.subtitle->setWordWrap(true);

    auto* layout = new QVBoxLayout(card.frame);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(3);
    layout->addWidget(card.title);
    layout->addWidget(card.value);
    layout->addWidget(card.subtitle);
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

QTableWidgetItem* makeCell(const QString& text, Qt::Alignment alignment)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setTextAlignment(alignment);
    return item;
}

} // namespace ui
