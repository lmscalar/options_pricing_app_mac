//
//  IvHistory.h
//  OptionPricing
//
//  Implied-volatility history: a daily sample of the 30-day constant-maturity ATM implied
//  vol (interpolated in total variance from the chain's term structure), IV rank and IV
//  percentile over a lookback, and the implied vol of a call / put pair from their daily
//  closes (used to rebuild history from historical option bars). Qt-free.
//

#pragma once

#include "BlackScholes.h"
#include "Events.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pricing {
namespace ivhist {

/// One day of implied-vol history.
struct Sample {
    std::string date;        ///< ISO yyyy-mm-dd
    double iv30 = 0.0;       ///< 30-day constant-maturity ATM implied vol, decimal (0 = none)
    double ivNear = 0.0;     ///< ATM vol of the nearest listed expiry at least a week out
    double spot = 0.0;
    std::string source;      ///< "snapshot" (a stored chain) or "backfill" (historical option bars)
};

/// IV rank and percentile of `current` against the history.
struct Stats {
    bool ok = false;
    double current = 0.0;
    double rank = 0.0;           ///< (current - low) / (high - low), 0..1
    double percentile = 0.0;     ///< share of samples at or below current, 0..1
    double low = 0.0, high = 0.0;
    std::string lowDate, highDate;
    double mean = 0.0, median = 0.0;
    int samples = 0;
    int lookback = 0;            ///< samples considered
};

/// Constant-maturity ATM vol at `days` from the term structure (linear in total variance).
inline double constantMaturityIv(const std::vector<events::TermPoint>& term, int days)
{
    if (term.empty() || days <= 0) return 0.0;
    const double T = days / 365.0;
    const double w = events::detail::interpolatedVariance(term, T);
    return w > 0.0 ? std::sqrt(w / T) : 0.0;
}

/// Today's sample from a chain: IV30 plus the near-expiry ATM vol.
inline Sample sampleFromChain(const std::string& date, const std::vector<ChainQuote>& quotes, const ActivityMarket& market, const std::string& source = "snapshot")
{
    Sample s;
    s.date = date;
    s.spot = market.spot;
    s.source = source;
    const events::Analysis a = events::analyze(quotes, market, -1);
    if (!a.ok) return s;
    s.iv30 = constantMaturityIv(a.term, 30);
    for (const events::TermPoint& tp : a.term) if (tp.expiry.daysToExpiry >= 7) { s.ivNear = tp.atmIv; break; }
    if (s.ivNear <= 0.0) s.ivNear = a.term.front().atmIv;
    if (s.iv30 <= 0.0) s.iv30 = s.ivNear;
    return s;
}

/// Rank and percentile of `current` over the last `lookback` samples with a value (history ascending by date).
inline Stats stats(const std::vector<Sample>& history, double current, int lookback = 252)
{
    Stats st;
    st.current = current;
    std::vector<const Sample*> used;
    for (auto it = history.rbegin(); it != history.rend() && static_cast<int>(used.size()) < lookback; ++it) if (it->iv30 > 0.0) used.push_back(&*it);
    st.samples = static_cast<int>(used.size());
    st.lookback = lookback;
    if (used.size() < 2 || current <= 0.0) return st;
    std::vector<double> values;
    values.reserve(used.size());
    double sum = 0.0;
    const Sample* lo = used.front();
    const Sample* hi = used.front();
    int atOrBelow = 0;
    for (const Sample* s : used) {
        values.push_back(s->iv30);
        sum += s->iv30;
        if (s->iv30 < lo->iv30) lo = s;
        if (s->iv30 > hi->iv30) hi = s;
        if (s->iv30 <= current) ++atOrBelow;
    }
    std::sort(values.begin(), values.end());
    st.low = lo->iv30;
    st.high = hi->iv30;
    st.lowDate = lo->date;
    st.highDate = hi->date;
    st.mean = sum / values.size();
    st.median = values.size() % 2 ? values[values.size() / 2] : 0.5 * (values[values.size() / 2 - 1] + values[values.size() / 2]);
    st.rank = st.high > st.low ? std::clamp((current - st.low) / (st.high - st.low), 0.0, 1.0) : 0.5;
    st.percentile = static_cast<double>(atOrBelow) / values.size();
    st.ok = true;
    return st;
}

/// Implied vol of one option from a daily close; 0 when the price has no time value or cannot be solved.
inline double impliedFromClose(double spot, double strike, double maturity, double rate, OptionType type, double close)
{
    if (spot <= 0.0 || strike <= 0.0 || maturity <= 0.0 || close <= 0.0) return 0.0;
    Inputs in;
    in.spot = spot;
    in.strike = strike;
    in.riskFreeRate = rate;
    in.maturity = maturity;
    const ImpliedVolResult r = impliedVolatility(in, type, close);
    return r.status == ImpliedVolResult::Status::Converged ? r.volatility : 0.0;
}

/// Average implied vol of a call and put at the same strike from their closes (either alone when
/// only one solves). Averaging the pair cancels most of the error from stale, non-simultaneous prints.
inline double impliedPairFromCloses(double spot, double strike, double maturity, double rate, double callClose, double putClose)
{
    const double c = impliedFromClose(spot, strike, maturity, rate, OptionType::Call, callClose);
    const double p = impliedFromClose(spot, strike, maturity, rate, OptionType::Put, putClose);
    if (c > 0.0 && p > 0.0) return 0.5 * (c + p);
    return c > 0.0 ? c : p;
}

} // namespace ivhist
} // namespace pricing
