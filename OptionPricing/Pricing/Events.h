//
//  Events.h
//  OptionPricing
//
//  Earnings / event analytics from the option market. The at-the-money implied-vol term
//  structure is turned into total variance per expiry; a jump in variance between two
//  expiries that the diffusive (baseline) vol cannot explain is the event variance, whose
//  square root is the implied one-standard-deviation earnings move. The 25-delta call and
//  put vols make the move asymmetric: the "beat" side scales with call-wing vol, the
//  "miss" side with put-wing vol. coneAt() gives the implied price cone at any horizon,
//  with the event variance switched on at the event date. Qt-free.
//

#pragma once

#include "Activity.h"
#include "ChainStrategy.h"
#include "Scanner.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pricing {
namespace events {

/// One listed expiry on the at-the-money term structure.
struct TermPoint {
    ExpiryKey expiry;
    double atmIv = 0.0;          ///< decimal, average of the ATM call and put
    double callIv25 = 0.0;       ///< ~25-delta call vol (0 when not solvable)
    double putIv25 = 0.0;        ///< ~25-delta put vol
    double straddle = 0.0;       ///< ATM straddle mid
    double totalVariance = 0.0;  ///< atmIv^2 * T
    double forwardVol = 0.0;     ///< vol between the previous expiry and this one (0 for the first or when not positive)
};

struct Analysis {
    bool ok = false;
    double spot = 0.0;
    std::vector<TermPoint> term;        ///< ascending by maturity
    double baselineVol = 0.0;           ///< diffusive vol with the event removed (decimal)
    bool eventDetected = false;         ///< an expiry carries variance the baseline cannot explain
    bool eventDateKnown = false;        ///< the caller supplied the date (otherwise inferred)
    int eventDays = -1;                 ///< calendar days to the event (inferred: the day after the expiry before the jump)
    ExpiryKey eventExpiry;              ///< first expiry after the event
    double eventVariance = 0.0;         ///< total variance attributable to the event
    double eventMove = 0.0;             ///< 1 sd event-only move, fraction of spot
    double upMove = 0.0;                ///< 1 sd event move on a beat (call-wing scaled), fraction
    double downMove = 0.0;              ///< 1 sd event move on a miss (put-wing scaled), fraction
    double totalMove = 0.0;             ///< 1 sd move to the event expiry including diffusion (ATM IV sqrt T)
    double straddleMove = 0.0;          ///< ATM straddle / spot at the event expiry
    double jumpRatio = 0.0;             ///< forward vol of the window holding the event / baseline vol
    double skewUp = 1.0, skewDown = 1.0;///< wing / ATM vol ratios used for the cone's two sides
    std::string note;
};

/// Implied price band at one horizon.
struct ConePoint {
    double up1 = 0.0, down1 = 0.0;      ///< one standard deviation
    double up2 = 0.0, down2 = 0.0;      ///< two standard deviations
};

/// An inferred event needs at least a 2% one-sd move and a 10% vol premium over the baseline.
constexpr double kMinInferredMove = 0.02;
constexpr double kMinInferredJump = 1.10;

namespace detail {

inline double interpolatedVariance(const std::vector<TermPoint>& term, double t)
{
    if (term.empty() || t <= 0.0) return 0.0;
    const double T0 = term.front().expiry.maturity;
    if (t <= T0) return term.front().totalVariance * t / T0;
    for (size_t i = 1; i < term.size(); ++i) {
        const double Ta = term[i - 1].expiry.maturity, Tb = term[i].expiry.maturity;
        if (t <= Tb) {
            const double w = (t - Ta) / std::max(Tb - Ta, 1e-9);
            return term[i - 1].totalVariance + w * (term[i].totalVariance - term[i - 1].totalVariance);
        }
    }
    // Beyond the last expiry: extend at the last expiry's implied vol.
    const TermPoint& last = term.back();
    return last.atmIv * last.atmIv * t;
}

} // namespace detail

/// Builds the ATM term structure and isolates the event. `eventDays` < 0 means the date is
/// unknown and is inferred from the largest variance jump within `maxInferDays`.
inline Analysis analyze(const std::vector<ChainQuote>& quotes, const ActivityMarket& market, int eventDays = -1, int maxInferDays = 120)
{
    Analysis a;
    a.spot = market.spot;
    if (quotes.empty() || market.spot <= 0.0) { a.note = "no chain"; return a; }
    ChainIndex chain(quotes);
    for (const ListedExpiry* e : chain.expiries()) {
        if (e->key.daysToExpiry < 1) continue;
        const std::vector<scan::LadderPoint> calls = scan::ladder(chain, *e, OptionType::Call, market);
        const std::vector<scan::LadderPoint> puts = scan::ladder(chain, *e, OptionType::Put, market);
        const scan::LadderPoint* c = scan::nearestStrike(calls, market.spot);
        const scan::LadderPoint* p = scan::nearestStrike(puts, market.spot);
        if (!c && !p) continue;
        TermPoint tp;
        tp.expiry = e->key;
        tp.atmIv = c && p ? 0.5 * (c->iv + p->iv) : (c ? c : p)->iv;
        if (c && p) tp.straddle = c->quote->mid + p->quote->mid;
        if (const scan::LadderPoint* w = scan::atDelta(calls, 0.25, market.spot)) tp.callIv25 = w->iv;
        if (const scan::LadderPoint* w = scan::atDelta(puts, 0.25, market.spot)) tp.putIv25 = w->iv;
        tp.totalVariance = tp.atmIv * tp.atmIv * e->key.maturity;
        if (!a.term.empty()) {
            const TermPoint& prev = a.term.back();
            const double dv = tp.totalVariance - prev.totalVariance, dt = e->key.maturity - prev.expiry.maturity;
            tp.forwardVol = dv > 0.0 && dt > 0.0 ? std::sqrt(dv / dt) : 0.0;
        }
        a.term.push_back(tp);
    }
    if (a.term.empty()) { a.note = "no usable at-the-money quotes"; return a; }
    a.ok = true;

    // Forward variance per year on each segment between consecutive expiries (the first
    // segment runs from today). The event sits in the segment whose forward variance stands
    // out; the baseline (diffusive) variance is the median of the other segments.
    const size_t n = a.term.size();
    std::vector<double> fv(n, 0.0), dt(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double Ti = a.term[i].expiry.maturity, Tp = i ? a.term[i - 1].expiry.maturity : 0.0;
        const double wi = a.term[i].totalVariance, wp = i ? a.term[i - 1].totalVariance : 0.0;
        dt[i] = Ti - Tp;
        fv[i] = dt[i] > 0.0 ? std::max(0.0, (wi - wp) / dt[i]) : 0.0;
    }
    auto baselineExcluding = [&](size_t skip) {
        std::vector<double> others;
        for (size_t i = 0; i < n; ++i) if (i != skip && fv[i] > 0.0) others.push_back(fv[i]);
        if (others.empty()) return 0.0;
        std::sort(others.begin(), others.end());
        const size_t m = others.size();
        return std::sqrt(m % 2 ? others[m / 2] : 0.5 * (others[m / 2 - 1] + others[m / 2]));
    };

    size_t j = n;
    if (eventDays >= 0) {
        a.eventDateKnown = true;
        a.eventDays = eventDays;
        // The first expiry strictly after the date: a report after the close on an expiry day is
        // not in that day's options (a pre-open report is in both, so this is safe either way).
        for (size_t i = 0; i < n; ++i) if (a.term[i].expiry.daysToExpiry > eventDays) { j = i; break; }
        if (j == n) for (size_t i = 0; i < n; ++i) if (a.term[i].expiry.daysToExpiry >= eventDays) { j = i; break; }
        if (j == n) a.note = "the event falls after the last listed expiry";
        // A date a day or two early (an after-close report, a projected calendar entry) lands on
        // a window the options say is clear while the next window carries the variance: use that one.
        if (j + 1 < n && n >= 2) {
            auto excessMove = [&](size_t k) {
                const double base = baselineExcluding(k);
                return base > 0.0 ? std::sqrt(std::max(0.0, (fv[k] - base * base) * dt[k])) : 0.0;
            };
            const double baseJ = baselineExcluding(j), baseNext = baselineExcluding(j + 1);
            const bool clearHere = excessMove(j) < 0.005;
            const bool eventNext = baseNext > 0.0 && excessMove(j + 1) >= kMinInferredMove && std::sqrt(fv[j + 1]) / baseNext >= kMinInferredJump;
            if (clearHere && eventNext && baseJ >= 0.0) {
                ++j;
                a.note = "the options place the event in the window after the given date";
            }
        }
    } else if (n >= 2) {
        // Infer: the segment with the most forward variance, provided it stands clear of the rest.
        size_t best = n;
        for (size_t i = 0; i < n; ++i) {
            if (a.term[i].expiry.daysToExpiry > maxInferDays) break;
            if (fv[i] > 0.0 && (best == n || fv[i] > fv[best])) best = i;
        }
        if (best < n) {
            const double base = baselineExcluding(best);
            const double move = base > 0.0 ? std::sqrt(std::max(0.0, (fv[best] - base * base) * dt[best])) : 0.0;
            if (base > 0.0 && move >= kMinInferredMove && std::sqrt(fv[best]) / base >= kMinInferredJump) {
                j = best;
                a.eventDays = j > 0 ? a.term[j - 1].expiry.daysToExpiry + 1 : std::max(1, a.term[j].expiry.daysToExpiry / 2);
                a.note = "event inferred from the implied-vol term structure";
            }
        }
        if (j == n) a.note = "no event priced in the term structure";
    } else {
        a.note = "a single expiry cannot separate an event from the baseline";
    }

    if (j < n) {
        const TermPoint& tp = a.term[j];
        a.eventExpiry = tp.expiry;
        a.baselineVol = baselineExcluding(j);
        a.totalMove = tp.atmIv * std::sqrt(tp.expiry.maturity);
        a.straddleMove = tp.straddle > 0.0 ? tp.straddle / market.spot : 0.0;
        if (a.baselineVol > 0.0) {
            a.jumpRatio = std::sqrt(fv[j]) / a.baselineVol;   // event-window forward vol over the baseline
            a.eventVariance = std::max(0.0, (fv[j] - a.baselineVol * a.baselineVol) * dt[j]);
        }
        a.eventMove = std::sqrt(a.eventVariance);
        a.eventDetected = a.eventDateKnown ? a.eventMove >= 0.005 : (a.eventMove >= kMinInferredMove && a.jumpRatio >= kMinInferredJump);
        if (tp.callIv25 > 0.0 && tp.atmIv > 0.0) a.skewUp = std::clamp(tp.callIv25 / tp.atmIv, 0.5, 2.0);
        if (tp.putIv25 > 0.0 && tp.atmIv > 0.0) a.skewDown = std::clamp(tp.putIv25 / tp.atmIv, 0.5, 2.0);
        a.upMove = a.eventMove * a.skewUp;
        a.downMove = a.eventMove * a.skewDown;
        if (a.eventDateKnown && !a.eventDetected) a.note = "no excess variance over the baseline in the window holding the event";
    }
    if (a.baselineVol <= 0.0) {
        // Nothing to separate: the nearest expiry at least three weeks out stands in as the diffusive vol.
        for (const TermPoint& tp : a.term) if (tp.expiry.daysToExpiry >= 21) { a.baselineVol = tp.atmIv; break; }
        if (a.baselineVol <= 0.0) a.baselineVol = a.term.back().atmIv;
    }
    return a;
}

/// Implied cone at `tYears` ahead: baseline diffusion plus the event variance once the event
/// date has passed (interpolated term structure when no event is isolated).
inline ConePoint coneAt(const Analysis& a, double tYears)
{
    ConePoint c;
    if (!a.ok || tYears <= 0.0) { c.up1 = c.down1 = c.up2 = c.down2 = a.spot; return c; }
    double variance = 0.0;
    if (a.eventDetected && a.eventDays >= 0) {
        variance = a.baselineVol * a.baselineVol * tYears + (tYears >= a.eventDays / 365.0 ? a.eventVariance : 0.0);
    } else {
        variance = detail::interpolatedVariance(a.term, tYears);
    }
    const double sd = std::sqrt(std::max(variance, 0.0));
    c.up1 = a.spot * std::exp(a.skewUp * sd);
    c.down1 = a.spot * std::exp(-a.skewDown * sd);
    c.up2 = a.spot * std::exp(2.0 * a.skewUp * sd);
    c.down2 = a.spot * std::exp(-2.0 * a.skewDown * sd);
    return c;
}

} // namespace events
} // namespace pricing
