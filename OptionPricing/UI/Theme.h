//
//  Theme.h
//  OptionPricing
//
//  Colour tokens for the light and dark appearances, the palette derived from them,
//  and the application stylesheet.
//

#pragma once

#include "QtHeaders.h"

/// Colour tokens for one appearance. The stylesheet template references these by name.
struct Theme {
    QString window;
    QString surface;
    QString surfaceAlt;
    QString border;
    QString borderHover;
    QString text;
    QString textStrong;
    QString textMuted;
    QString accent;
    QString accentHover;
    QString accentPressed;
    QString accentText;
    QString call;
    QString put;
    QString error;
    QString warning;
    QString separator;
    QString buttonBg;
    QString buttonHover;
    QString buttonPressed;
    QString disabledBg;
    QString disabledText;
    QString tooltipBg;
    QString tooltipText;
    QString profit;      ///< heatmap and chart colour for gains
    QString loss;        ///< heatmap and chart colour for losses
    QString gridLine;    ///< chart grid lines
    QString up;          ///< price up / bid side
    QString down;        ///< price down / ask side
    QString flat;        ///< unchanged price
    QString accent2;     ///< secondary accent (amber) for section titles and the ticker symbol
    QString accent3;     ///< tertiary accent (cyan) for informational highlights
    QString itmCall;     ///< background tint for in-the-money call cells
    QString itmPut;      ///< background tint for in-the-money put cells
    bool dark = false;
};

/// Colour for a signed change: up, down or flat.
QString changeColor(const Theme& t, double change);

Theme lightTheme();
Theme darkTheme();

/// Palette used for anything the stylesheet does not cover (spin arrows, popup lists, etc.).
QPalette paletteFor(const Theme& t);

/// Application-wide stylesheet with the theme tokens substituted.
QString styleSheetFor(const Theme& t);

/// Applies the theme's look to a chart (background, axis colours, grid).
void styleChart(QChart* chart, const Theme& t);

/// Linear blend between two colours.
QColor blend(const QColor& a, const QColor& b, double t);
