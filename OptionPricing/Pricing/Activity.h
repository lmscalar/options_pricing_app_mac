//
//  Activity.h
//  OptionPricing
//
//  Option-chain activity analytics: a strike x expiry grid of volume or open interest,
//  a ranking of the most active contracts, and put-call parity checks per expiry.
//  Works on the ChainQuote vector produced by the chain importer or the live client.
//

#pragma once

#include "BlackScholes.h"
#include "Csv.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pricing {

enum class ActivityMetric {
    Volume,
    OpenInterest,
    VolumePlusOpenInterest,
    Turnover,          ///< volume / open interest
};

enum class SideFilter {
    Both,
    Calls,
    Puts,
};

inline const char* activityMetricName(ActivityMetric m)
{
    switch (m) {
    case ActivityMetric::Volume:                 return "Volume";
    case ActivityMetric::OpenInterest:           return "Open interest";
    case ActivityMetric::VolumePlusOpenInterest: return "Volume + open interest";
    case ActivityMetric::Turnover:               return "Volume / open interest";
    }
    return "";
}

inline bool sideMatches(SideFilter side, OptionType type)
{
    return side == SideFilter::Both || (side == SideFilter::Calls && type == OptionType::Call)
        || (side == SideFilter::Puts && type == OptionType::Put);
}

/// Activity value of one contract under the chosen metric.
inline double activityValue(const ChainQuote& q, ActivityMetric metric)
{
    switch (metric) {
    case ActivityMetric::Volume:                 return q.volume;
    case ActivityMetric::OpenInterest:           return q.openInterest;
    case ActivityMetric::VolumePlusOpenInterest: return q.volume + q.openInterest;
    case ActivityMetric::Turnover:               return q.openInterest > 0.0 ? q.volume / q.openInterest : 0.0;
    }
    return 0.0;
}

/// One expiry column of the grid.
struct ExpiryKey {
    double maturity = 0.0;
    std::string expiryDate;
    int daysToExpiry = 0;
};

struct ActivityGrid {
    std::vector<double> strikes;          ///< rows, highest strike first
    std::vector<ExpiryKey> expiries;      ///< columns, nearest first
    std::vector<std::vector<double>> cells;   ///< cells[row][col], aggregated metric
    std::vector<std::vector<int>> contracts;  ///< contracts contributing to each cell
    double maxValue = 0.0;
    double total = 0.0;
};

namespace detail {

inline long long expiryBucket(double maturity)
{
    return static_cast<long long>(std::llround(maturity * 3650.0));
}

} // namespace detail

/// Builds the strike x expiry activity grid. Strikes outside spot * (1 +/- band) are
/// dropped when band > 0. For Turnover the cell holds total volume / total open interest.
inline ActivityGrid buildActivityGrid(const std::vector<ChainQuote>& quotes, ActivityMetric metric, SideFilter side,
                                      double spot, double bandFraction)
{
    ActivityGrid grid;
    std::map<long long, ExpiryKey> expiryMap;
    std::map<double, int, std::greater<double>> strikeMap;
    for (const ChainQuote& q : quotes) {
        if (!sideMatches(side, q.type) || q.maturity <= 0.0 || q.strike <= 0.0) continue;
        if (bandFraction > 0.0 && spot > 0.0 && (q.strike < spot * (1.0 - bandFraction) || q.strike > spot * (1.0 + bandFraction))) continue;
        ExpiryKey& key = expiryMap[detail::expiryBucket(q.maturity)];
        if (key.maturity == 0.0) {
            key.maturity = q.maturity;
            key.expiryDate = q.expiryDate;
            key.daysToExpiry = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::llround(q.maturity * 365.0));
        }
        strikeMap[q.strike] = 0;
    }
    if (expiryMap.empty() || strikeMap.empty()) {
        return grid;
    }

    std::map<long long, int> expiryIndex;
    for (auto& [bucket, key] : expiryMap) {
        expiryIndex[bucket] = static_cast<int>(grid.expiries.size());
        grid.expiries.push_back(key);
    }
    int row = 0;
    for (auto& [strike, index] : strikeMap) {
        index = row++;
        grid.strikes.push_back(strike);
    }

    const size_t rows = grid.strikes.size(), cols = grid.expiries.size();
    std::vector<std::vector<double>> volume(rows, std::vector<double>(cols, 0.0));
    std::vector<std::vector<double>> openInterest(rows, std::vector<double>(cols, 0.0));
    grid.contracts.assign(rows, std::vector<int>(cols, 0));
    for (const ChainQuote& q : quotes) {
        if (!sideMatches(side, q.type) || q.maturity <= 0.0 || q.strike <= 0.0) continue;
        const auto s = strikeMap.find(q.strike);
        const auto e = expiryIndex.find(detail::expiryBucket(q.maturity));
        if (s == strikeMap.end() || e == expiryIndex.end()) continue;
        const size_t r = static_cast<size_t>(s->second), c = static_cast<size_t>(e->second);
        volume[r][c] += q.volume;
        openInterest[r][c] += q.openInterest;
        ++grid.contracts[r][c];
    }

    grid.cells.assign(rows, std::vector<double>(cols, 0.0));
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            double value = 0.0;
            switch (metric) {
            case ActivityMetric::Volume:                 value = volume[r][c]; break;
            case ActivityMetric::OpenInterest:           value = openInterest[r][c]; break;
            case ActivityMetric::VolumePlusOpenInterest: value = volume[r][c] + openInterest[r][c]; break;
            case ActivityMetric::Turnover:               value = openInterest[r][c] > 0.0 ? volume[r][c] / openInterest[r][c] : 0.0; break;
            }
            grid.cells[r][c] = value;
            grid.maxValue = std::max(grid.maxValue, value);
            grid.total += value;
        }
    }
    return grid;
}

/// A contract ranked by activity, with its implied vol and parity gap for context.
struct ActiveContract {
    ChainQuote quote;
    double activity = 0.0;
    double impliedVol = 0.0;        ///< 0 when it could not be solved
    double counterpartMid = 0.0;    ///< mid of the same-strike opposite type, 0 if absent
    double parityGap = 0.0;         ///< C - P - (F - K) e^{-rT}; NaN when no counterpart
    double shareOfTotal = 0.0;      ///< activity / total activity of the filtered set
};

/// Market context for implied vols and parity: rate may vary by maturity.
struct ActivityMarket {
    Model model = Model::BlackScholesMerton;
    double spot = 100.0;
    double dividendYield = 0.0;
    std::function<double(double maturity)> rateFor = [](double) { return 0.0; };
};

/// Returns the top `count` contracts by the chosen metric.
inline std::vector<ActiveContract> mostActiveContracts(const std::vector<ChainQuote>& quotes, ActivityMetric metric, SideFilter side,
                                                       const ActivityMarket& market, size_t count)
{
    // Index counterparts by (expiry bucket, strike, type) for parity.
    std::map<std::pair<long long, double>, const ChainQuote*> calls, puts;
    for (const ChainQuote& q : quotes) {
        (q.type == OptionType::Call ? calls : puts)[{ detail::expiryBucket(q.maturity), q.strike }] = &q;
    }

    std::vector<ActiveContract> ranked;
    double total = 0.0;
    for (const ChainQuote& q : quotes) {
        if (!sideMatches(side, q.type) || q.maturity <= 0.0) continue;
        const double value = activityValue(q, metric);
        total += value;
        if (value <= 0.0) continue;
        ActiveContract c;
        c.quote = q;
        c.activity = value;
        ranked.push_back(c);
    }
    std::sort(ranked.begin(), ranked.end(), [](const ActiveContract& a, const ActiveContract& b) {
        if (a.activity != b.activity) return a.activity > b.activity;
        if (a.quote.maturity != b.quote.maturity) return a.quote.maturity < b.quote.maturity;
        return a.quote.strike < b.quote.strike;
    });
    if (ranked.size() > count) ranked.resize(count);

    for (ActiveContract& c : ranked) {
        c.shareOfTotal = total > 0.0 ? c.activity / total : 0.0;
        Inputs in;
        in.model = market.model;
        in.spot = market.spot;
        in.strike = c.quote.strike;
        in.riskFreeRate = market.rateFor(c.quote.maturity);
        in.dividendYield = market.dividendYield;
        in.maturity = c.quote.maturity;
        const ImpliedVolResult iv = impliedVolatility(in, c.quote.type, c.quote.mid);
        c.impliedVol = iv.status == ImpliedVolResult::Status::Converged ? iv.volatility : 0.0;

        const auto key = std::make_pair(detail::expiryBucket(c.quote.maturity), c.quote.strike);
        const auto& others = c.quote.type == OptionType::Call ? puts : calls;
        const auto it = others.find(key);
        if (it == others.end()) {
            c.parityGap = std::nan("");
            continue;
        }
        c.counterpartMid = it->second->mid;
        const double r = in.riskFreeRate;
        const double q = market.model == Model::Black76 ? r : market.dividendYield;
        const double forward = market.spot * std::exp((r - q) * c.quote.maturity);
        const double callMid = c.quote.type == OptionType::Call ? c.quote.mid : it->second->mid;
        const double putMid = c.quote.type == OptionType::Put ? c.quote.mid : it->second->mid;
        c.parityGap = callMid - putMid - (forward - c.quote.strike) * std::exp(-r * c.quote.maturity);
    }
    return ranked;
}

/// Put-call parity summary for one expiry.
struct ParityRow {
    ExpiryKey expiry;
    int pairs = 0;                   ///< strikes with both a call and a put
    double impliedForward = 0.0;     ///< median of K + (C - P) e^{rT} across pairs
    double modelForward = 0.0;       ///< S e^{(r - q) T}
    double forwardGap = 0.0;         ///< implied - model
    double impliedYield = 0.0;       ///< dividend yield that would reconcile the implied forward
    double meanAbsGap = 0.0;         ///< mean |C - P - (F_model - K) e^{-rT}| across pairs
    double worstGap = 0.0;           ///< largest signed gap by magnitude
    double worstGapStrike = 0.0;
    double callVolume = 0.0, putVolume = 0.0;
    double callOpenInterest = 0.0, putOpenInterest = 0.0;
};

inline std::vector<ParityRow> parityByExpiry(const std::vector<ChainQuote>& quotes, const ActivityMarket& market)
{
    struct Bucket {
        ExpiryKey key;
        std::map<double, const ChainQuote*> calls, puts;
        double callVolume = 0, putVolume = 0, callOI = 0, putOI = 0;
    };
    std::map<long long, Bucket> buckets;
    for (const ChainQuote& q : quotes) {
        if (q.maturity <= 0.0) continue;
        Bucket& b = buckets[detail::expiryBucket(q.maturity)];
        if (b.key.maturity == 0.0) {
            b.key.maturity = q.maturity;
            b.key.expiryDate = q.expiryDate;
            b.key.daysToExpiry = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::llround(q.maturity * 365.0));
        }
        if (q.type == OptionType::Call) {
            b.calls[q.strike] = &q;
            b.callVolume += q.volume;
            b.callOI += q.openInterest;
        } else {
            b.puts[q.strike] = &q;
            b.putVolume += q.volume;
            b.putOI += q.openInterest;
        }
    }

    std::vector<ParityRow> rows;
    for (auto& [bucket, b] : buckets) {
        ParityRow row;
        row.expiry = b.key;
        row.callVolume = b.callVolume;
        row.putVolume = b.putVolume;
        row.callOpenInterest = b.callOI;
        row.putOpenInterest = b.putOI;
        const double T = b.key.maturity;
        const double r = market.rateFor(T);
        const double q = market.model == Model::Black76 ? r : market.dividendYield;
        row.modelForward = market.spot * std::exp((r - q) * T);
        const double discount = std::exp(-r * T);

        std::vector<double> forwards;
        double absSum = 0.0;
        for (const auto& [strike, call] : b.calls) {
            const auto put = b.puts.find(strike);
            if (put == b.puts.end()) continue;
            if (call->mid <= 0.0 || put->second->mid <= 0.0) continue;
            ++row.pairs;
            const double gap = call->mid - put->second->mid - (row.modelForward - strike) * discount;
            absSum += std::fabs(gap);
            if (std::fabs(gap) > std::fabs(row.worstGap)) {
                row.worstGap = gap;
                row.worstGapStrike = strike;
            }
            forwards.push_back(strike + (call->mid - put->second->mid) / discount);
        }
        if (row.pairs > 0) {
            std::sort(forwards.begin(), forwards.end());
            const size_t n = forwards.size();
            row.impliedForward = n % 2 ? forwards[n / 2] : 0.5 * (forwards[n / 2 - 1] + forwards[n / 2]);
            row.forwardGap = row.impliedForward - row.modelForward;
            row.meanAbsGap = absSum / row.pairs;
            if (market.spot > 0.0 && row.impliedForward > 0.0 && T > 0.0) {
                // F = S e^{(r - q_impl) T}  =>  q_impl = r - ln(F / S) / T
                row.impliedYield = r - std::log(row.impliedForward / market.spot) / T;
            }
        }
        rows.push_back(row);
    }
    return rows;
}

/// Underlying price implied by put-call parity on a near expiry. Because option trades
/// are often disseminated with less delay than the underlying quote, this gives a fresher
/// estimate of spot than a delayed stock feed.
struct ImpliedSpotEstimate {
    bool valid = false;
    double spot = 0.0;             ///< forward discounted back to today, plus PV of dividends before expiry
    double forward = 0.0;          ///< median of K + (C - P) e^{rT} across the pairs used
    ExpiryKey expiry;
    int pairs = 0;
    double dispersion = 0.0;       ///< median absolute deviation of the per-strike forwards
};

/// Uses the nearest expiry with at least `minPairs` call/put pairs whose strikes lie within
/// `band` of `referenceSpot`. Expiries closer than `minDays` are skipped when a later one
/// qualifies, because same-day options carry little information about the forward.
inline ImpliedSpotEstimate impliedSpotFromParity(const std::vector<ChainQuote>& quotes, const ActivityMarket& market,
                                                 double referenceSpot, const std::vector<Dividend>& dividends,
                                                 double band = 0.06, int minPairs = 3, int minDays = 2, int maxDays = 60)
{
    struct Bucket {
        ExpiryKey key;
        std::map<double, double> callMid, putMid;
    };
    std::map<long long, Bucket> buckets;
    for (const ChainQuote& q : quotes) {
        if (q.maturity <= 0.0 || q.mid <= 0.0) continue;
        if (referenceSpot > 0.0 && band > 0.0 && std::fabs(q.strike / referenceSpot - 1.0) > band) continue;
        Bucket& b = buckets[detail::expiryBucket(q.maturity)];
        if (b.key.maturity == 0.0) {
            b.key.maturity = q.maturity;
            b.key.expiryDate = q.expiryDate;
            b.key.daysToExpiry = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::llround(q.maturity * 365.0));
        }
        (q.type == OptionType::Call ? b.callMid : b.putMid)[q.strike] = q.mid;
    }

    auto estimateFor = [&](const Bucket& b) {
        ImpliedSpotEstimate e;
        const double T = b.key.maturity;
        const double r = market.rateFor(T);
        const double q = market.model == Model::Black76 ? r : market.dividendYield;
        std::vector<double> forwards;
        for (const auto& [strike, call] : b.callMid) {
            const auto put = b.putMid.find(strike);
            if (put == b.putMid.end()) continue;
            forwards.push_back(strike + (call - put->second) * std::exp(r * T));
        }
        if (static_cast<int>(forwards.size()) < minPairs) return e;
        std::sort(forwards.begin(), forwards.end());
        const size_t n = forwards.size();
        const double median = n % 2 ? forwards[n / 2] : 0.5 * (forwards[n / 2 - 1] + forwards[n / 2]);
        std::vector<double> deviations;
        for (double f : forwards) deviations.push_back(std::fabs(f - median));
        std::sort(deviations.begin(), deviations.end());
        e.dispersion = n % 2 ? deviations[n / 2] : 0.5 * (deviations[n / 2 - 1] + deviations[n / 2]);
        e.forward = median;
        double pvDividends = 0.0;
        for (const Dividend& d : dividends) {
            if (d.time > 0.0 && d.time <= T) pvDividends += d.amount * std::exp(-r * d.time);
        }
        e.spot = median * std::exp(-(r - q) * T) + pvDividends;
        e.expiry = b.key;
        e.pairs = static_cast<int>(n);
        e.valid = e.spot > 0.0 && std::isfinite(e.spot);
        return e;
    };

    ImpliedSpotEstimate fallback;
    for (const auto& [bucket, b] : buckets) {
        if (b.key.daysToExpiry > maxDays) break;
        const ImpliedSpotEstimate e = estimateFor(b);
        if (!e.valid) continue;
        if (b.key.daysToExpiry >= minDays) return e;
        if (!fallback.valid) fallback = e;   // same-day expiry: use only if nothing better exists
    }
    return fallback;
}

/// Totals and standouts for the filtered chain.
struct ActivitySummary {
    double callVolume = 0, putVolume = 0, callOpenInterest = 0, putOpenInterest = 0;
    double putCallVolumeRatio = 0.0;
    double putCallOpenInterestRatio = 0.0;
    ExpiryKey busiestExpiry;
    double busiestExpiryValue = 0.0;
    double busiestStrike = 0.0;
    double busiestStrikeValue = 0.0;
    int contracts = 0;
};

inline ActivitySummary summarizeActivity(const std::vector<ChainQuote>& quotes, ActivityMetric metric, SideFilter side)
{
    ActivitySummary s;
    std::map<long long, std::pair<ExpiryKey, double>> byExpiry;
    std::map<double, double> byStrike;
    for (const ChainQuote& q : quotes) {
        if (q.maturity <= 0.0) continue;
        if (q.type == OptionType::Call) { s.callVolume += q.volume; s.callOpenInterest += q.openInterest; }
        else { s.putVolume += q.volume; s.putOpenInterest += q.openInterest; }
        if (!sideMatches(side, q.type)) continue;
        ++s.contracts;
        const double value = activityValue(q, metric);
        auto& e = byExpiry[detail::expiryBucket(q.maturity)];
        if (e.first.maturity == 0.0) {
            e.first.maturity = q.maturity;
            e.first.expiryDate = q.expiryDate;
            e.first.daysToExpiry = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::llround(q.maturity * 365.0));
        }
        e.second += value;
        byStrike[q.strike] += value;
    }
    s.putCallVolumeRatio = s.callVolume > 0.0 ? s.putVolume / s.callVolume : 0.0;
    s.putCallOpenInterestRatio = s.callOpenInterest > 0.0 ? s.putOpenInterest / s.callOpenInterest : 0.0;
    for (const auto& [bucket, e] : byExpiry) {
        if (e.second > s.busiestExpiryValue) { s.busiestExpiryValue = e.second; s.busiestExpiry = e.first; }
    }
    for (const auto& [strike, value] : byStrike) {
        if (value > s.busiestStrikeValue) { s.busiestStrikeValue = value; s.busiestStrike = strike; }
    }
    return s;
}

} // namespace pricing
