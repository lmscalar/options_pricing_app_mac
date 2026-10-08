//
//  DayCount.h
//  OptionPricing
//
//  Civil-date arithmetic and day-count conventions used to turn an expiry date into a
//  year fraction. No Qt dependency: the UI converts QDate to CivilDate at the boundary.
//

#pragma once

#include <algorithm>
#include <cstdint>

namespace pricing {

/// Proleptic Gregorian calendar date.
struct CivilDate {
    int year = 1970;
    int month = 1;   ///< 1..12
    int day = 1;     ///< 1..31
};

enum class DayCount {
    Actual365,      ///< calendar days / 365
    Actual360,      ///< calendar days / 360
    Business252,    ///< weekdays (Mon-Fri) / 252
};

/// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
inline std::int64_t daysFromCivil(CivilDate d)
{
    std::int64_t y = d.year;
    const unsigned m = static_cast<unsigned>(d.month);
    const unsigned day = static_cast<unsigned>(d.day);
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// Inverse of daysFromCivil.
inline CivilDate civilFromDays(std::int64_t z)
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    CivilDate out;
    out.year = static_cast<int>(y + (m <= 2));
    out.month = static_cast<int>(m);
    out.day = static_cast<int>(d);
    return out;
}

/// 0 = Monday ... 6 = Sunday.
inline int weekdayIndex(CivilDate d)
{
    const std::int64_t z = daysFromCivil(d);
    // 1970-01-01 was a Thursday (index 3).
    return static_cast<int>(((z % 7) + 7 + 3) % 7);
}

/// Calendar days from `from` to `to` (negative if `to` precedes `from`).
inline int calendarDaysBetween(CivilDate from, CivilDate to)
{
    return static_cast<int>(daysFromCivil(to) - daysFromCivil(from));
}

/// Weekdays (Monday to Friday) strictly after `from` and up to and including `to`.
/// Public holidays are not modelled.
inline int businessDaysBetween(CivilDate from, CivilDate to)
{
    const std::int64_t a = daysFromCivil(from);
    const std::int64_t b = daysFromCivil(to);
    if (b <= a) {
        return 0;
    }
    const std::int64_t total = b - a;
    const std::int64_t fullWeeks = total / 7;
    std::int64_t count = fullWeeks * 5;
    // Remaining days after the full weeks, walked one by one.
    std::int64_t day = a + fullWeeks * 7 + 1;
    for (; day <= b; ++day) {
        const int wd = static_cast<int>(((day % 7) + 7 + 3) % 7);
        if (wd < 5) {
            ++count;
        }
    }
    return static_cast<int>(count);
}

/// Year fraction between two dates under the given convention. Never negative.
inline double yearFraction(CivilDate from, CivilDate to, DayCount convention)
{
    switch (convention) {
    case DayCount::Actual360:
        return std::max(0, calendarDaysBetween(from, to)) / 360.0;
    case DayCount::Business252:
        return businessDaysBetween(from, to) / 252.0;
    case DayCount::Actual365:
    default:
        return std::max(0, calendarDaysBetween(from, to)) / 365.0;
    }
}

/// Days-per-year basis that matches the convention, used to quote theta per day.
inline double dayBasis(DayCount convention)
{
    switch (convention) {
    case DayCount::Actual360:   return 360.0;
    case DayCount::Business252: return 252.0;
    case DayCount::Actual365:
    default:                    return 365.0;
    }
}

} // namespace pricing
