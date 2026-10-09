//
//  Theme.cpp
//  OptionPricing
//

#include "Theme.h"

#include <algorithm>

Theme lightTheme()
{
    Theme t;
    t.window        = "#eef1f5";
    t.surface       = "#ffffff";
    t.surfaceAlt    = "#f8fafc";
    t.border        = "#d7dce3";
    t.borderHover   = "#94a3b8";
    t.text          = "#374151";
    t.textStrong    = "#111827";
    t.textMuted     = "#6b7280";
    t.accent        = "#2563eb";
    t.accentHover   = "#1d4ed8";
    t.accentPressed = "#1e40af";
    t.accentText    = "#ffffff";
    t.call          = "#15803d";
    t.put           = "#b91c1c";
    t.error         = "#b91c1c";
    t.warning       = "#b45309";
    t.separator     = "#e5e7eb";
    t.buttonBg      = "#ffffff";
    t.buttonHover   = "#f3f4f6";
    t.buttonPressed = "#e5e7eb";
    t.disabledBg    = "#f3f4f6";
    t.disabledText  = "#9ca3af";
    t.tooltipBg     = "#111827";
    t.tooltipText   = "#f9fafb";
    t.profit        = "#16a34a";
    t.loss          = "#dc2626";
    t.gridLine      = "#e5e7eb";
    t.up            = "#15803d";
    t.down          = "#dc2626";
    t.flat          = "#6b7280";
    t.accent2       = "#b45309";
    t.accent3       = "#0e7490";
    t.itmCall       = "#e8f5ec";
    t.itmPut        = "#fbeaea";
    t.dark = false;
    return t;
}

Theme darkTheme()
{
    // Trading-desk palette: near-black navy, amber section titles, cyan highlights,
    // saturated green/red for direction.
    Theme t;
    t.window        = "#0a0f1c";
    t.surface       = "#121a2b";
    t.surfaceAlt    = "#192338";
    t.border        = "#273449";
    t.borderHover   = "#4b5d7a";
    t.text          = "#c7d2e3";
    t.textStrong    = "#f3f6fb";
    t.textMuted     = "#8294ad";
    t.accent        = "#3b82f6";
    t.accentHover   = "#2563eb";
    t.accentPressed = "#1d4ed8";
    t.accentText    = "#ffffff";
    t.call          = "#4ade80";
    t.put           = "#f87171";
    t.error         = "#f87171";
    t.warning       = "#fbbf24";
    t.separator     = "#273449";
    t.buttonBg      = "#162038";
    t.buttonHover   = "#1e2b45";
    t.buttonPressed = "#273449";
    t.disabledBg    = "#121a2b";
    t.disabledText  = "#5b6b85";
    t.tooltipBg     = "#f3f6fb";
    t.tooltipText   = "#0a0f1c";
    t.profit        = "#22c55e";
    t.loss          = "#ef4444";
    t.gridLine      = "#1f2a3f";
    t.up            = "#22c55e";
    t.down          = "#ef4444";
    t.flat          = "#8294ad";
    t.accent2       = "#f59e0b";
    t.accent3       = "#22d3ee";
    t.itmCall       = "#15291f";
    t.itmPut        = "#2c1a22";
    t.dark = true;
    return t;
}

QString changeColor(const Theme& t, double change)
{
    if (change > 1e-9) return t.up;
    if (change < -1e-9) return t.down;
    return t.flat;
}

QPalette paletteFor(const Theme& t)
{
    QPalette p;
    p.setColor(QPalette::Window,          QColor(t.window));
    p.setColor(QPalette::WindowText,      QColor(t.text));
    p.setColor(QPalette::Base,            QColor(t.surface));
    p.setColor(QPalette::AlternateBase,   QColor(t.surfaceAlt));
    p.setColor(QPalette::Text,            QColor(t.textStrong));
    p.setColor(QPalette::PlaceholderText, QColor(t.textMuted));
    p.setColor(QPalette::Button,          QColor(t.buttonBg));
    p.setColor(QPalette::ButtonText,      QColor(t.text));
    p.setColor(QPalette::Highlight,       QColor(t.accent));
    p.setColor(QPalette::HighlightedText, QColor(t.accentText));
    p.setColor(QPalette::ToolTipBase,     QColor(t.tooltipBg));
    p.setColor(QPalette::ToolTipText,     QColor(t.tooltipText));
    p.setColor(QPalette::Light,           QColor(t.surfaceAlt));
    p.setColor(QPalette::Midlight,        QColor(t.border));
    p.setColor(QPalette::Mid,             QColor(t.border));
    p.setColor(QPalette::Dark,            QColor(t.borderHover));
    p.setColor(QPalette::Shadow,          QColor(t.window));
    p.setColor(QPalette::Disabled, QPalette::Text,       QColor(t.disabledText));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(t.disabledText));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(t.disabledText));
    return p;
}

QString styleSheetFor(const Theme& t)
{
    QString css = QStringLiteral(R"(
        QMainWindow, QWidget#root, QDialog {
            background: @window;
        }
        QWidget {
            color: @text;
            font-size: 13px;
        }
        QMenuBar {
            background: @window;
            color: @text;
        }
        QMenuBar::item:selected {
            background: @surfaceAlt;
        }
        QMenu {
            background: @surface;
            color: @textStrong;
            border: 1px solid @border;
        }
        QMenu::item:selected {
            background: @accent;
            color: @accentText;
        }
        QStatusBar {
            background: @window;
            color: @textMuted;
        }
        QLabel#title {
            font-size: 22px;
            font-weight: 700;
            color: @textStrong;
        }
        QLabel#muted {
            color: @textMuted;
        }
        QLabel#error {
            color: @error;
            font-weight: 600;
        }
        QLabel#warning {
            color: @warning;
            font-weight: 600;
        }
        QGroupBox {
            font-weight: 600;
            color: @text;
            background: @surface;
            border: 1px solid @border;
            border-radius: 8px;
            margin-top: 12px;
            padding: 12px 10px 8px 10px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 10px;
            padding: 0 6px;
            background: @surface;
            border-radius: 4px;
            color: @accent2;
            font-size: 11px;
            font-weight: 700;
            letter-spacing: 1px;
            text-transform: uppercase;
        }
        QDoubleSpinBox, QSpinBox, QComboBox, QDateEdit, QLineEdit {
            background: @surface;
            color: @textStrong;
            border: 1px solid @border;
            border-radius: 6px;
            padding: 5px 8px;
            min-width: 72px;
            selection-background-color: @accent;
            selection-color: @accentText;
        }
        QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover, QDateEdit:hover, QLineEdit:hover {
            border-color: @borderHover;
        }
        QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus, QDateEdit:focus, QLineEdit:focus {
            border: 1px solid @accent;
        }
        QComboBox QAbstractItemView {
            background: @surface;
            color: @textStrong;
            selection-background-color: @accent;
            selection-color: @accentText;
            border: 1px solid @border;
            outline: none;
        }
        QCheckBox {
            color: @text;
            spacing: 6px;
        }
        QCheckBox::indicator {
            width: 15px;
            height: 15px;
            border: 1px solid @borderHover;
            border-radius: 4px;
            background: @surface;
        }
        QCheckBox::indicator:checked {
            background: @accent;
            border-color: @accentHover;
        }
        QCheckBox::indicator:disabled {
            background: @disabledBg;
            border-color: @border;
        }
        QTabWidget::pane {
            border: 1px solid @border;
            border-radius: 8px;
            background: @window;
            top: -1px;
        }
        QTabBar::tab {
            background: @surfaceAlt;
            color: @textMuted;
            border: 1px solid @border;
            border-bottom: none;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
            padding: 7px 16px;
            margin-right: 2px;
            font-weight: 600;
        }
        QTabBar::tab:selected {
            background: @window;
            color: @textStrong;
            border-top: 2px solid @accent;
        }
        QTabBar::tab:hover:!selected {
            background: @buttonHover;
        }
        QTableWidget, QTableView {
            background: @surface;
            alternate-background-color: @surfaceAlt;
            color: @textStrong;
            gridline-color: @separator;
            border: 1px solid @border;
            border-radius: 6px;
            selection-background-color: @accent;
            selection-color: @accentText;
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 12px;
        }
        QHeaderView::section {
            background: @surfaceAlt;
            color: @textMuted;
            border: none;
            border-right: 1px solid @separator;
            border-bottom: 1px solid @border;
            padding: 5px 8px;
            font-weight: 700;
            font-size: 11px;
        }
        QComboBox#chartTypeCombo {
            min-width: 128px;
        }
        QHeaderView#heatmapHeader::section {
            font-size: 10px;
            font-weight: 500;
            padding: 3px 4px;
        }
        QTableCornerButton::section {
            background: @surfaceAlt;
            border: none;
        }
        QPlainTextEdit {
            background: @surface;
            color: @textStrong;
            border: 1px solid @border;
            border-radius: 6px;
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 12px;
        }
        QFrame#card {
            background: @surfaceAlt;
            border: 1px solid @border;
            border-radius: 10px;
        }
        QFrame#pane {
            background: @surface;
            border: 1px solid @border;
            border-radius: 10px;
        }
        QLabel#cardTitle {
            font-size: 11px;
            font-weight: 700;
            color: @accent3;
            letter-spacing: 1px;
        }
        QFrame#tickerBanner {
            background: @surface;
            border: 1px solid @border;
            border-left: 3px solid @accent2;
            border-radius: 8px;
        }
        QLabel#tickerSymbol {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 15px;
            font-weight: 700;
            color: @accent2;
            letter-spacing: 1px;
        }
        QLabel#tickerPrice {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 26px;
            font-weight: 700;
            color: @textStrong;
        }
        QLabel#tickerUp {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 14px;
            font-weight: 700;
            color: @up;
        }
        QLabel#tickerDown {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 14px;
            font-weight: 700;
            color: @down;
        }
        QLabel#tickerFlat {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 14px;
            font-weight: 700;
            color: @flat;
        }
        QLabel#tickerMeta {
            font-size: 11px;
            color: @textMuted;
        }
        QLabel#priceUp {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 24px;
            font-weight: 700;
            color: @up;
        }
        QLabel#priceDown {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 24px;
            font-weight: 700;
            color: @down;
        }
        QLabel#bidValue {
            font-family: "Menlo", "SF Mono", monospace;
            color: @up;
        }
        QLabel#askValue {
            font-family: "Menlo", "SF Mono", monospace;
            color: @down;
        }
        QLabel#cardSubtitle {
            font-size: 11px;
            color: @textMuted;
        }
        QLabel#callPrice {
            font-size: 28px;
            font-weight: 700;
            color: @call;
        }
        QLabel#putPrice {
            font-size: 28px;
            font-weight: 700;
            color: @put;
        }
        QLabel#bigValue {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 22px;
            font-weight: 700;
            color: @textStrong;
        }
        QLabel#ivValue {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 24px;
            font-weight: 700;
            color: @accent;
        }
        QLabel#spotUp, QLabel#spotDown, QLabel#spotFlat {
            font-family: "Menlo", "SF Mono", monospace;
            font-size: 14px;
            font-weight: 700;
        }
        QLabel#spotUp { color: @up; }
        QLabel#spotDown { color: @down; }
        QLabel#spotFlat { color: @accent; }
        QLabel#columnHeader {
            font-weight: 700;
            color: @textMuted;
        }
        QLabel#rowLabel {
            color: @text;
            font-weight: 400;
        }
        QLabel#value {
            font-family: "Menlo", "SF Mono", monospace;
            color: @textStrong;
        }
        QLabel#profitValue {
            font-family: "Menlo", "SF Mono", monospace;
            color: @profit;
            font-weight: 600;
        }
        QLabel#lossValue {
            font-family: "Menlo", "SF Mono", monospace;
            color: @loss;
            font-weight: 600;
        }
        QFrame#separator {
            color: @separator;
        }
        QPushButton, QToolButton {
            border-radius: 7px;
            padding: 7px 20px;
            font-weight: 600;
        }
        QPushButton#secondary, QPushButton#themeToggle, QToolButton {
            background: @buttonBg;
            color: @text;
            border: 1px solid @border;
        }
        QPushButton#secondary:hover, QPushButton#themeToggle:hover, QToolButton:hover {
            background: @buttonHover;
            border-color: @borderHover;
        }
        QPushButton#secondary:pressed, QPushButton#themeToggle:pressed, QToolButton:pressed {
            background: @buttonPressed;
        }
        /* The selected timeframe / drawing tool / listening state stands out in the accent colour. */
        QToolButton:checked {
            background: @accent;
            color: @accentText;
            border-color: @accentHover;
            font-weight: 700;
        }
        QToolButton:checked:hover {
            background: @accentHover;
        }
        QToolButton#assistantToggle {
            font-weight: 600;
            padding: 6px 12px;
            border-radius: 8px;
            color: @accent3;
        }
        QToolButton#assistantToggle:checked {
            background: @accent;
            color: @accentText;
        }
        QPushButton#themeToggle {
            padding: 5px 14px;
            font-weight: 500;
        }
        QToolButton {
            padding: 4px 10px;
        }
        QPushButton#primary {
            background: @accent;
            color: @accentText;
            border: 1px solid @accentHover;
        }
        QPushButton#primary:hover {
            background: @accentHover;
        }
        QPushButton#primary:pressed {
            background: @accentPressed;
        }
        QPushButton:disabled {
            background: @disabledBg;
            color: @disabledText;
            border: 1px solid @border;
        }
        QSlider::groove:horizontal {
            height: 4px;
            background: @border;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: @accent;
            width: 14px;
            margin: -6px 0;
            border-radius: 7px;
        }
        QScrollArea {
            border: none;
            background: transparent;
        }
        QSplitter::handle:vertical {
            background: @border;
            height: 10px;
            border-radius: 5px;
            margin: 2px 40%;
        }
        QSplitter::handle:vertical:hover {
            background: @accent;
        }
        QSplitter#quotesSplitter::handle {
            background: @border;
            width: 4px;
            border-radius: 2px;
            margin: 24px 1px;
        }
        QSplitter#quotesSplitter::handle:hover {
            background: @accent;
        }
        QToolTip {
            background: @tooltipBg;
            color: @tooltipText;
            border: none;
            padding: 6px 8px;
        }
    )");

    // Longer token names are listed before their prefixes so "@textStrong" is not
    // partially replaced by "@text".
    const struct { const char* token; const QString& value; } tokens[] = {
        { "@window",        t.window },
        { "@surfaceAlt",    t.surfaceAlt },
        { "@surface",       t.surface },
        { "@borderHover",   t.borderHover },
        { "@border",        t.border },
        { "@textStrong",    t.textStrong },
        { "@textMuted",     t.textMuted },
        { "@text",          t.text },
        { "@accentHover",   t.accentHover },
        { "@accentPressed", t.accentPressed },
        { "@accentText",    t.accentText },
        { "@accent2",       t.accent2 },
        { "@accent3",       t.accent3 },
        { "@accent",        t.accent },
        { "@call",          t.call },
        { "@put",           t.put },
        { "@error",         t.error },
        { "@warning",       t.warning },
        { "@separator",     t.separator },
        { "@buttonBg",      t.buttonBg },
        { "@buttonHover",   t.buttonHover },
        { "@buttonPressed", t.buttonPressed },
        { "@disabledBg",    t.disabledBg },
        { "@disabledText",  t.disabledText },
        { "@tooltipBg",     t.tooltipBg },
        { "@tooltipText",   t.tooltipText },
        { "@profit",        t.profit },
        { "@loss",          t.loss },
        { "@up",            t.up },
        { "@down",          t.down },
        { "@flat",          t.flat },
    };
    for (const auto& tok : tokens) {
        css.replace(QLatin1String(tok.token), tok.value);
    }
    return css;
}

void styleChart(QChart* chart, const Theme& t)
{
    if (!chart) {
        return;
    }
    chart->setBackgroundBrush(QBrush(QColor(t.surface)));
    chart->setBackgroundRoundness(8.0);
    chart->setPlotAreaBackgroundBrush(QBrush(QColor(t.surface)));
    chart->setPlotAreaBackgroundVisible(true);
    chart->setTitleBrush(QBrush(QColor(t.textStrong)));
    QFont titleFont = chart->titleFont();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    chart->setTitleFont(titleFont);
    chart->legend()->setLabelColor(QColor(t.text));
    chart->legend()->setAlignment(Qt::AlignBottom);
    chart->setMargins(QMargins(8, 8, 8, 8));

    for (QAbstractAxis* axis : chart->axes()) {
        axis->setLabelsColor(QColor(t.textMuted));
        axis->setTitleBrush(QBrush(QColor(t.text)));
        axis->setLinePenColor(QColor(t.border));
        axis->setGridLineColor(QColor(t.gridLine));
        axis->setMinorGridLineVisible(false);
    }
}

QColor blend(const QColor& a, const QColor& b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            1.0);
}
