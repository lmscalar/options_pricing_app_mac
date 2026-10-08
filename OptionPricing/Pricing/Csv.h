//
//  Csv.h
//  OptionPricing
//
//  Minimal RFC-4180-style CSV reader and writer, plus parsers for the two CSV layouts
//  the application understands: option chains and batch pricing requests. No Qt.
//

#pragma once

#include "BlackScholes.h"
#include "DayCount.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pricing {
namespace csv {

using Row = std::vector<std::string>;

/// Parses CSV text into rows. Handles quoted fields, escaped quotes and CRLF line ends.
inline std::vector<Row> parse(const std::string& text, char delimiter = ',')
{
    std::vector<Row> rows;
    Row row;
    std::string field;
    bool inQuotes = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == delimiter) {
            row.push_back(field);
            field.clear();
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else {
            field.push_back(c);
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }

    // Drop completely blank lines.
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Row& r) {
        return std::all_of(r.begin(), r.end(), [](const std::string& s) {
            return std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isspace(ch); });
        });
    }), rows.end());
    return rows;
}

/// Quotes a field if it contains the delimiter, quotes or line breaks.
inline std::string quote(const std::string& field, char delimiter = ',')
{
    const bool needsQuotes = field.find(delimiter) != std::string::npos
                          || field.find('"') != std::string::npos
                          || field.find('\n') != std::string::npos;
    if (!needsQuotes) {
        return field;
    }
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

inline std::string join(const Row& row, char delimiter = ',')
{
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) {
        if (i) out.push_back(delimiter);
        out += quote(row[i], delimiter);
    }
    return out;
}

inline std::string trim(const std::string& s)
{
    const auto begin = std::find_if_not(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    const auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

inline std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline std::optional<double> toDouble(const std::string& raw)
{
    std::string s = trim(raw);
    if (s.empty()) return std::nullopt;
    // Tolerate percent signs and thousands separators.
    bool percent = false;
    if (s.back() == '%') { percent = true; s.pop_back(); }
    s.erase(std::remove(s.begin(), s.end(), ','), s.end());
    s.erase(std::remove(s.begin(), s.end(), '$'), s.end());
    char* end = nullptr;
    const double value = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return std::nullopt;
    return percent ? value / 100.0 : value;
}

/// Finds the column index whose header matches any of the given aliases.
inline int findColumn(const Row& header, std::initializer_list<const char*> aliases)
{
    for (size_t i = 0; i < header.size(); ++i) {
        const std::string h = lower(trim(header[i]));
        for (const char* alias : aliases) {
            if (h == alias) return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace csv

// MARK: - Option chain CSV

/// One quoted contract from an option chain.
struct ChainQuote {
    OptionType type = OptionType::Call;
    double strike = 0.0;
    double maturity = 0.0;     ///< years
    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;          ///< (bid+ask)/2, or the supplied mid/last when bid/ask are absent
    double volume = 0.0;
    double openInterest = 0.0;
    double vendorImpliedVol = 0.0;   ///< implied vol supplied by the data vendor, if any (decimal)
    std::string expiryDate;          ///< ISO calendar date (YYYY-MM-DD) when known, otherwise empty
    int daysToExpiry = 0;            ///< calendar days from the valuation date; 0 when unknown
};

/// Parses an ISO "YYYY-MM-DD" date. Returns false if the text is not a valid date.
inline bool parseIsoDate(const std::string& text, CivilDate& out)
{
    const std::string s = csv::trim(text);
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    const int year = std::atoi(s.substr(0, 4).c_str());
    const int month = std::atoi(s.substr(5, 2).c_str());
    const int day = std::atoi(s.substr(8, 2).c_str());
    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    out = CivilDate{ year, month, day };
    // Round-trip through the day number to reject dates like February 30.
    const CivilDate back = civilFromDays(daysFromCivil(out));
    return back.year == year && back.month == month && back.day == day;
}

inline std::string formatIsoDate(const CivilDate& d)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", d.year, d.month, d.day);
    return buffer;
}

struct ChainParseResult {
    std::vector<ChainQuote> quotes;
    std::vector<std::string> warnings;
    std::string error;   ///< non-empty when the file could not be understood at all
};

/// Parses an option chain. Required columns: strike, type (call/put or C/P), and a price
/// (bid+ask, or mid/last/price). Expiry may be given as an ISO date ("expiration",
/// "expiry date"), years ("expiry", "t", "maturity"), days ("days", "dte"), or omitted
/// when `defaultMaturity` is supplied. Dates are measured from `valuationDate` on ACT/365.
inline ChainParseResult parseChainCsv(const std::string& text, double defaultMaturity = 0.0,
                                      const CivilDate* valuationDate = nullptr)
{
    ChainParseResult out;
    const std::vector<csv::Row> rows = csv::parse(text);
    if (rows.size() < 2) {
        out.error = "The file needs a header row and at least one data row.";
        return out;
    }
    const csv::Row& header = rows.front();
    const int colStrike = csv::findColumn(header, { "strike", "k", "strike price" });
    const int colType = csv::findColumn(header, { "type", "option type", "right", "cp", "putcall", "put/call" });
    const int colBid = csv::findColumn(header, { "bid" });
    const int colAsk = csv::findColumn(header, { "ask", "offer" });
    const int colMid = csv::findColumn(header, { "mid", "price", "last", "mark", "premium" });
    const int colDate = csv::findColumn(header, { "expiration", "expiration date", "expiration_date", "expiry date", "expiry_date", "exp date", "expdate" });
    const int colYears = csv::findColumn(header, { "expiry", "t", "maturity", "years", "tenor" });
    const int colDays = csv::findColumn(header, { "days", "dte", "days to expiry" });
    const int colVolume = csv::findColumn(header, { "volume", "vol" });
    const int colOpenInterest = csv::findColumn(header, { "open interest", "oi", "openinterest" });

    if (colStrike < 0) { out.error = "No 'strike' column found."; return out; }
    if (colType < 0) { out.error = "No 'type' column found (call/put)."; return out; }
    if (colBid < 0 && colAsk < 0 && colMid < 0) { out.error = "No price column found (bid/ask, mid, last or price)."; return out; }

    auto field = [&](const csv::Row& row, int col) -> std::string {
        return (col >= 0 && static_cast<size_t>(col) < row.size()) ? row[static_cast<size_t>(col)] : std::string();
    };

    for (size_t r = 1; r < rows.size(); ++r) {
        const csv::Row& row = rows[r];
        ChainQuote q;
        const auto strike = csv::toDouble(field(row, colStrike));
        if (!strike || *strike <= 0.0) {
            out.warnings.push_back("Row " + std::to_string(r + 1) + ": invalid strike, skipped.");
            continue;
        }
        q.strike = *strike;

        const std::string typeText = csv::lower(csv::trim(field(row, colType)));
        if (typeText == "c" || typeText == "call" || typeText == "calls") {
            q.type = OptionType::Call;
        } else if (typeText == "p" || typeText == "put" || typeText == "puts") {
            q.type = OptionType::Put;
        } else {
            out.warnings.push_back("Row " + std::to_string(r + 1) + ": unknown option type '" + typeText + "', skipped.");
            continue;
        }

        const auto bid = csv::toDouble(field(row, colBid));
        const auto ask = csv::toDouble(field(row, colAsk));
        const auto mid = csv::toDouble(field(row, colMid));
        q.bid = bid.value_or(0.0);
        q.ask = ask.value_or(0.0);
        if (bid && ask && *ask > 0.0) {
            q.mid = 0.5 * (*bid + *ask);
        } else if (mid) {
            q.mid = *mid;
        } else {
            out.warnings.push_back("Row " + std::to_string(r + 1) + ": no usable price, skipped.");
            continue;
        }

        CivilDate expiry;
        if (colDate >= 0 && valuationDate && parseIsoDate(field(row, colDate), expiry)) {
            q.daysToExpiry = calendarDaysBetween(*valuationDate, expiry);
            q.maturity = q.daysToExpiry / 365.0;
            q.expiryDate = formatIsoDate(expiry);
        } else if (colYears >= 0 && csv::toDouble(field(row, colYears))) {
            q.maturity = *csv::toDouble(field(row, colYears));
        } else if (colDays >= 0 && csv::toDouble(field(row, colDays))) {
            q.maturity = *csv::toDouble(field(row, colDays)) / 365.0;
            q.daysToExpiry = static_cast<int>(std::lround(*csv::toDouble(field(row, colDays))));
        } else {
            q.maturity = defaultMaturity;
        }
        if (q.daysToExpiry == 0 && q.maturity > 0.0) {
            q.daysToExpiry = static_cast<int>(std::lround(q.maturity * 365.0));
        }
        if (q.maturity <= 0.0) {
            out.warnings.push_back("Row " + std::to_string(r + 1) + ": missing expiry, skipped.");
            continue;
        }
        q.volume = csv::toDouble(field(row, colVolume)).value_or(0.0);
        q.openInterest = csv::toDouble(field(row, colOpenInterest)).value_or(0.0);
        out.quotes.push_back(q);
    }

    if (out.quotes.empty()) {
        out.error = "No usable rows were found.";
    }
    return out;
}

// MARK: - Batch pricing CSV

/// One row of a batch pricing request.
struct BatchContract {
    std::string label;
    Inputs inputs;
    OptionType type = OptionType::Call;
    bool american = false;
};

struct BatchParseResult {
    std::vector<BatchContract> contracts;
    std::vector<std::string> warnings;
    std::string error;
};

/// Columns: label (optional), type, spot, strike, rate, dividend (optional), vol, expiry (years)
/// or days, model (optional: "futures"/"black76" for Black-76), exercise (optional: "american").
/// Missing rate/vol/spot/dividend fall back to the supplied defaults.
inline BatchParseResult parseBatchCsv(const std::string& text, const Inputs& defaults)
{
    BatchParseResult out;
    const std::vector<csv::Row> rows = csv::parse(text);
    if (rows.size() < 2) {
        out.error = "The file needs a header row and at least one data row.";
        return out;
    }
    const csv::Row& header = rows.front();
    const int colLabel = csv::findColumn(header, { "label", "name", "symbol", "id" });
    const int colType = csv::findColumn(header, { "type", "option type", "right", "cp", "put/call" });
    const int colSpot = csv::findColumn(header, { "spot", "s", "underlying", "futures", "f" });
    const int colStrike = csv::findColumn(header, { "strike", "k" });
    const int colRate = csv::findColumn(header, { "rate", "r", "risk-free rate", "rf" });
    const int colDiv = csv::findColumn(header, { "dividend", "q", "yield", "dividend yield" });
    const int colVol = csv::findColumn(header, { "vol", "volatility", "sigma", "iv" });
    const int colYears = csv::findColumn(header, { "expiry", "t", "maturity", "years" });
    const int colDays = csv::findColumn(header, { "days", "dte" });
    const int colModel = csv::findColumn(header, { "model", "underlying type" });
    const int colExercise = csv::findColumn(header, { "exercise", "style" });

    if (colStrike < 0) { out.error = "No 'strike' column found."; return out; }
    if (colType < 0) { out.error = "No 'type' column found."; return out; }
    if (colYears < 0 && colDays < 0) { out.error = "No expiry column found ('expiry' in years or 'days')."; return out; }

    auto field = [&](const csv::Row& row, int col) -> std::string {
        return (col >= 0 && static_cast<size_t>(col) < row.size()) ? row[static_cast<size_t>(col)] : std::string();
    };

    for (size_t r = 1; r < rows.size(); ++r) {
        const csv::Row& row = rows[r];
        BatchContract c;
        c.inputs = defaults;
        c.inputs.dividends.clear();
        c.label = csv::trim(field(row, colLabel));
        if (c.label.empty()) c.label = "Row " + std::to_string(r + 1);

        const std::string typeText = csv::lower(csv::trim(field(row, colType)));
        if (typeText == "c" || typeText == "call") c.type = OptionType::Call;
        else if (typeText == "p" || typeText == "put") c.type = OptionType::Put;
        else { out.warnings.push_back(c.label + ": unknown option type, skipped."); continue; }

        const auto strike = csv::toDouble(field(row, colStrike));
        if (!strike || *strike <= 0.0) { out.warnings.push_back(c.label + ": invalid strike, skipped."); continue; }
        c.inputs.strike = *strike;

        if (const auto v = csv::toDouble(field(row, colSpot))) c.inputs.spot = *v;
        if (const auto v = csv::toDouble(field(row, colRate))) c.inputs.riskFreeRate = *v;
        if (const auto v = csv::toDouble(field(row, colDiv))) c.inputs.dividendYield = *v;
        if (const auto v = csv::toDouble(field(row, colVol))) c.inputs.volatility = *v;

        // Rates and vols above 1.0 are almost certainly percentages.
        if (std::fabs(c.inputs.riskFreeRate) > 1.0) c.inputs.riskFreeRate /= 100.0;
        if (std::fabs(c.inputs.dividendYield) > 1.0) c.inputs.dividendYield /= 100.0;
        if (c.inputs.volatility > 3.0) c.inputs.volatility /= 100.0;

        if (const auto v = csv::toDouble(field(row, colYears))) c.inputs.maturity = *v;
        else if (const auto d = csv::toDouble(field(row, colDays))) c.inputs.maturity = *d / 365.0;
        else { out.warnings.push_back(c.label + ": missing expiry, skipped."); continue; }

        const std::string modelText = csv::lower(csv::trim(field(row, colModel)));
        if (modelText == "futures" || modelText == "black76" || modelText == "black-76" || modelText == "future") {
            c.inputs.model = Model::Black76;
        } else if (!modelText.empty()) {
            c.inputs.model = Model::BlackScholesMerton;
        }
        const std::string exerciseText = csv::lower(csv::trim(field(row, colExercise)));
        c.american = (exerciseText == "american" || exerciseText == "a" || exerciseText == "amer");

        if (!isValid(c.inputs)) { out.warnings.push_back(c.label + ": inputs out of range, skipped."); continue; }
        out.contracts.push_back(c);
    }
    if (out.contracts.empty()) {
        out.error = "No usable rows were found.";
    }
    return out;
}

} // namespace pricing
