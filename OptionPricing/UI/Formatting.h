//
//  Formatting.h
//  OptionPricing
//
//  Number formatting shared by every tab.
//

#pragma once

#include "QtHeaders.h"

#include <cmath>

namespace ui {

/// Fixed-point number; prints exact zero instead of "-0.0000".
inline QString number(double value, int decimals = 4)
{
    if (!std::isfinite(value)) {
        return std::isnan(value) ? QStringLiteral("–") : (value > 0 ? QStringLiteral("∞") : QStringLiteral("−∞"));
    }
    if (std::fabs(value) < 0.5 * std::pow(10.0, -decimals)) {
        value = 0.0;
    }
    return QString::number(value, 'f', decimals);
}

inline QString money(double value, int decimals = 4)
{
    if (!std::isfinite(value)) {
        return number(value, decimals);
    }
    return QStringLiteral("$ %1").arg(number(value, decimals));
}

/// Money with thousands separators and an explicit sign, for P&L figures.
inline QString signedMoney(double value, int decimals = 2)
{
    if (!std::isfinite(value)) {
        return number(value, decimals);
    }
    const QString magnitude = QLocale(QLocale::English).toString(std::fabs(value), 'f', decimals);
    if (std::fabs(value) < 0.5 * std::pow(10.0, -decimals)) {
        return QStringLiteral("$ %1").arg(magnitude);
    }
    return QStringLiteral("%1$ %2").arg(value < 0 ? QStringLiteral("−") : QStringLiteral("+"), magnitude);
}

/// Decimal fraction shown as a percentage.
inline QString percent(double fraction, int decimals = 2)
{
    if (!std::isfinite(fraction)) {
        return number(fraction, decimals);
    }
    return QStringLiteral("%1%").arg(number(fraction * 100.0, decimals));
}

/// Compact representation for table cells: fewer decimals for large magnitudes.
inline QString compact(double value)
{
    if (!std::isfinite(value)) {
        return number(value);
    }
    const double magnitude = std::fabs(value);
    if (magnitude >= 10000.0) return QLocale(QLocale::English).toString(value, 'f', 0);
    if (magnitude >= 100.0)   return QString::number(value, 'f', 2);
    if (magnitude >= 1.0)     return QString::number(value, 'f', 3);
    return number(value, 4);
}

} // namespace ui
