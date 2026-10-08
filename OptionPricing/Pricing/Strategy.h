//
//  Strategy.h
//  OptionPricing
//
//  Multi-leg positions: valuation, P&L, aggregated Greeks, breakevens, maximum gain and
//  loss, probability of profit, and a library of standard strategy presets.
//

#pragma once

#include "BlackScholes.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pricing {

enum class LegKind {
    Call,
    Put,
    Underlying,
};

/// One leg of a position. Quantity is signed: positive is long, negative is short.
struct Leg {
    LegKind kind = LegKind::Call;
    double quantity = 1.0;        ///< contracts (or units of underlying)
    double strike = 100.0;        ///< ignored for Underlying
    double maturity = 1.0;        ///< years, ignored for Underlying
    double volatility = 0.0;      ///< per-leg implied vol; 0 means use the market volatility
    double entryPrice = 0.0;      ///< premium per unit paid (long) or received (short); purchase price for Underlying
    double marketPrice = 0.0;     ///< current chain mid for the contract when a chain is loaded; 0 = unknown
    std::string expiryDate;       ///< ISO expiration date when the leg came from a chain
};

/// Market state shared by every leg.
struct Market {
    Model model = Model::BlackScholesMerton;
    double spot = 100.0;
    double riskFreeRate = 0.05;
    double dividendYield = 0.0;
    double volatility = 0.20;
    double dayBasis = 365.0;
    std::vector<Dividend> dividends;
};

struct Position {
    std::vector<Leg> legs;
    double multiplier = 100.0;    ///< contract multiplier (100 for US equity options)
};

inline OptionType optionTypeOf(LegKind kind)
{
    return kind == LegKind::Put ? OptionType::Put : OptionType::Call;
}

/// Pricing inputs for a leg after `elapsed` years have passed at the given spot.
inline Inputs legInputs(const Leg& leg, const Market& market, double spot, double elapsed)
{
    Inputs in;
    in.model = market.model;
    in.spot = spot;
    in.strike = leg.strike;
    in.riskFreeRate = market.riskFreeRate;
    in.dividendYield = market.dividendYield;
    in.volatility = leg.volatility > 0.0 ? leg.volatility : market.volatility;
    in.maturity = leg.maturity - elapsed;
    in.dayBasis = market.dayBasis;
    // Dividends already paid by `elapsed` no longer affect the leg.
    for (const Dividend& d : market.dividends) {
        if (d.time > elapsed) {
            in.dividends.push_back({ d.time - elapsed, d.amount });
        }
    }
    return in;
}

/// Value of one unit of the leg. Expired options are worth their intrinsic value.
inline double legValue(const Leg& leg, const Market& market, double spot, double elapsed)
{
    if (leg.kind == LegKind::Underlying) {
        return spot;
    }
    const Inputs in = legInputs(leg, market, spot, elapsed);
    if (in.maturity <= 1e-9) {
        return payoff(optionTypeOf(leg.kind), leg.strike, spot);
    }
    if (!isValid(in)) {
        return 0.0;
    }
    return legPrice(in, optionTypeOf(leg.kind));
}

/// Greeks of one unit of the leg (zero for expired options, delta 1 for the underlying).
inline Greeks legGreeks(const Leg& leg, const Market& market, double spot, double elapsed)
{
    Greeks g;
    if (leg.kind == LegKind::Underlying) {
        g.delta = 1.0;
        return g;
    }
    const Inputs in = legInputs(leg, market, spot, elapsed);
    if (in.maturity <= 1e-9 || !isValid(in)) {
        return g;
    }
    const Result r = price(in);
    return leg.kind == LegKind::Put ? r.put : r.call;
}

inline double positionValue(const Position& pos, const Market& market, double spot, double elapsed)
{
    double total = 0.0;
    for (const Leg& leg : pos.legs) {
        total += leg.quantity * pos.multiplier * legValue(leg, market, spot, elapsed);
    }
    return total;
}

/// Net amount paid to open the position (positive = net debit, negative = net credit).
inline double positionCost(const Position& pos)
{
    double total = 0.0;
    for (const Leg& leg : pos.legs) {
        total += leg.quantity * pos.multiplier * leg.entryPrice;
    }
    return total;
}

inline double positionPnl(const Position& pos, const Market& market, double spot, double elapsed)
{
    return positionValue(pos, market, spot, elapsed) - positionCost(pos);
}

inline Greeks positionGreeks(const Position& pos, const Market& market, double spot, double elapsed)
{
    Greeks total;
    for (const Leg& leg : pos.legs) {
        const Greeks g = legGreeks(leg, market, spot, elapsed);
        const double scale = leg.quantity * pos.multiplier;
        total.delta += scale * g.delta;
        total.gamma += scale * g.gamma;
        total.vega  += scale * g.vega;
        total.theta += scale * g.theta;
        total.rho   += scale * g.rho;
        total.vanna += scale * g.vanna;
        total.volga += scale * g.volga;
        total.charm += scale * g.charm;
        total.speed += scale * g.speed;
        total.color += scale * g.color;
        total.zomma += scale * g.zomma;
    }
    return total;
}

/// Shortest option maturity in the position, or 0 when it holds only the underlying.
inline double earliestExpiry(const Position& pos)
{
    double earliest = 0.0;
    bool found = false;
    for (const Leg& leg : pos.legs) {
        if (leg.kind != LegKind::Underlying) {
            earliest = found ? std::min(earliest, leg.maturity) : leg.maturity;
            found = true;
        }
    }
    return found ? earliest : 0.0;
}

inline double latestExpiry(const Position& pos)
{
    double latest = 0.0;
    for (const Leg& leg : pos.legs) {
        if (leg.kind != LegKind::Underlying) {
            latest = std::max(latest, leg.maturity);
        }
    }
    return latest;
}

/// Summary statistics of a position evaluated at its first expiry.
struct StrategyAnalysis {
    double netPremium = 0.0;            ///< positive = debit paid, negative = credit received
    double horizon = 0.0;               ///< years to the first expiry
    double maxProfit = 0.0;
    double maxLoss = 0.0;               ///< reported as a negative number
    bool unboundedProfit = false;
    bool unboundedLoss = false;
    std::vector<double> breakevens;     ///< spot levels where P&L at the horizon crosses zero
    double probabilityOfProfit = 0.0;   ///< risk-neutral, at the horizon
    double expectedPnl = 0.0;           ///< risk-neutral expectation at the horizon
    std::vector<double> spots;          ///< evaluation grid
    std::vector<double> pnlAtHorizon;   ///< P&L on the grid at the first expiry
    std::vector<double> pnlToday;       ///< mark-to-model P&L on the grid today
    Greeks greeks;                      ///< net Greeks at the current spot today
};

/// Spot grid wide enough to show every strike with some margin around them.
inline std::vector<double> analysisSpotGrid(const Position& pos, double spot, int points)
{
    double lo = spot * 0.6;
    double hi = spot * 1.4;
    for (const Leg& leg : pos.legs) {
        if (leg.kind != LegKind::Underlying) {
            lo = std::min(lo, leg.strike * 0.8);
            hi = std::max(hi, leg.strike * 1.2);
        }
    }
    lo = std::max(lo, 0.01);
    std::vector<double> grid;
    grid.reserve(static_cast<size_t>(points));
    for (int i = 0; i < points; ++i) {
        grid.push_back(lo + (hi - lo) * i / (points - 1));
    }
    return grid;
}

inline StrategyAnalysis analyzePosition(const Position& pos, const Market& market, int gridPoints = 601)
{
    StrategyAnalysis a;
    a.netPremium = positionCost(pos);
    a.horizon = earliestExpiry(pos);
    a.greeks = positionGreeks(pos, market, market.spot, 0.0);
    if (pos.legs.empty()) {
        return a;
    }

    a.spots = analysisSpotGrid(pos, market.spot, std::max(gridPoints, 11));
    a.pnlAtHorizon.reserve(a.spots.size());
    a.pnlToday.reserve(a.spots.size());
    for (double s : a.spots) {
        a.pnlAtHorizon.push_back(positionPnl(pos, market, s, a.horizon));
        a.pnlToday.push_back(positionPnl(pos, market, s, 0.0));
    }

    // Extremes: sample the grid, the strikes, and a point near zero spot.
    double best = -INFINITY, worst = INFINITY;
    auto consider = [&](double s) {
        const double v = positionPnl(pos, market, s, a.horizon);
        best = std::max(best, v);
        worst = std::min(worst, v);
    };
    for (double s : a.spots) consider(s);
    for (const Leg& leg : pos.legs) if (leg.kind != LegKind::Underlying) consider(leg.strike);
    consider(0.01);

    // Slope of the horizon P&L as spot goes to infinity: calls and stock contribute +1 per unit.
    double farRightSlope = 0.0;
    for (const Leg& leg : pos.legs) {
        if (leg.kind == LegKind::Call || leg.kind == LegKind::Underlying) {
            farRightSlope += leg.quantity * pos.multiplier;
        }
        // Puts that expire after the horizon still have some value far from the money,
        // but it tends to zero on the upside, so they do not change the slope.
    }
    const double slopeTolerance = 1e-9 * std::max(1.0, pos.multiplier);
    a.unboundedProfit = farRightSlope > slopeTolerance;
    a.unboundedLoss = farRightSlope < -slopeTolerance;
    a.maxProfit = a.unboundedProfit ? INFINITY : best;
    a.maxLoss = a.unboundedLoss ? -INFINITY : worst;

    // Breakevens: sign changes on the grid, refined by bisection.
    for (size_t i = 1; i < a.spots.size(); ++i) {
        const double f0 = a.pnlAtHorizon[i - 1];
        const double f1 = a.pnlAtHorizon[i];
        if ((f0 < 0.0 && f1 >= 0.0) || (f0 >= 0.0 && f1 < 0.0)) {
            double lo = a.spots[i - 1], hi = a.spots[i];
            double flo = f0;
            for (int iter = 0; iter < 60; ++iter) {
                const double mid = 0.5 * (lo + hi);
                const double fm = positionPnl(pos, market, mid, a.horizon);
                if ((flo < 0.0) == (fm < 0.0)) { lo = mid; flo = fm; } else { hi = mid; }
                if (hi - lo < 1e-7 * market.spot) break;
            }
            a.breakevens.push_back(0.5 * (lo + hi));
        }
    }

    // Probability of profit and expected P&L under the risk-neutral lognormal at the horizon.
    if (a.horizon > 1e-9 && market.volatility > 0.0) {
        const double v = market.volatility;
        const double T = a.horizon;
        const double q = market.model == Model::Black76 ? market.riskFreeRate : market.dividendYield;
        const double S0 = effectiveSpot(legInputs(Leg{ LegKind::Call, 1.0, market.spot, T, 0.0, 0.0 }, market, market.spot, 0.0));
        const double mu = (market.riskFreeRate - q - 0.5 * v * v) * T;
        const double sd = v * std::sqrt(T);
        auto cdf = [&](double s) { return s <= 0.0 ? 0.0 : normalCdf((std::log(s / S0) - mu) / sd); };

        double probability = 0.0;
        double expectation = 0.0;
        // Left tail below the grid.
        {
            const double p = cdf(a.spots.front());
            if (a.pnlAtHorizon.front() > 0.0) probability += p;
            expectation += p * a.pnlAtHorizon.front();
        }
        for (size_t i = 1; i < a.spots.size(); ++i) {
            const double p = cdf(a.spots[i]) - cdf(a.spots[i - 1]);
            const double midPnl = 0.5 * (a.pnlAtHorizon[i] + a.pnlAtHorizon[i - 1]);
            if (midPnl > 0.0) probability += p;
            expectation += p * midPnl;
        }
        // Right tail above the grid: extrapolate linearly with the far-right slope.
        {
            const double p = 1.0 - cdf(a.spots.back());
            if (a.pnlAtHorizon.back() > 0.0 || (a.unboundedProfit && p > 0.0 && a.pnlAtHorizon.back() >= 0.0)) probability += p;
            expectation += p * a.pnlAtHorizon.back();
        }
        a.probabilityOfProfit = std::clamp(probability, 0.0, 1.0);
        a.expectedPnl = expectation;
    } else {
        a.probabilityOfProfit = positionPnl(pos, market, market.spot, 0.0) > 0.0 ? 1.0 : 0.0;
        a.expectedPnl = positionPnl(pos, market, market.spot, 0.0);
    }
    return a;
}

// MARK: - Presets

enum class StrategyPreset {
    LongCall,
    LongPut,
    CoveredCall,
    ProtectivePut,
    Collar,
    BullCallSpread,
    BearPutSpread,
    BullPutSpread,
    BearCallSpread,
    LongStraddle,
    ShortStraddle,
    LongStrangle,
    ShortStrangle,
    IronCondor,
    IronButterfly,
    LongCallButterfly,
    CallCalendar,
    PutCalendar,
    RiskReversal,
};

struct PresetInfo {
    StrategyPreset id;
    const char* name;
    const char* description;
};

inline const std::vector<PresetInfo>& strategyPresets()
{
    static const std::vector<PresetInfo> presets = {
        { StrategyPreset::LongCall,          "Long Call",           "Buy one call at the money." },
        { StrategyPreset::LongPut,           "Long Put",            "Buy one put at the money." },
        { StrategyPreset::CoveredCall,       "Covered Call",        "Long 100 shares, short one out-of-the-money call." },
        { StrategyPreset::ProtectivePut,     "Protective Put",      "Long 100 shares, long one out-of-the-money put." },
        { StrategyPreset::Collar,            "Collar",              "Long shares, long put below, short call above." },
        { StrategyPreset::BullCallSpread,    "Bull Call Spread",    "Long lower call, short higher call." },
        { StrategyPreset::BearPutSpread,     "Bear Put Spread",     "Long higher put, short lower put." },
        { StrategyPreset::BullPutSpread,     "Bull Put Spread",     "Short higher put, long lower put (credit)." },
        { StrategyPreset::BearCallSpread,    "Bear Call Spread",    "Short lower call, long higher call (credit)." },
        { StrategyPreset::LongStraddle,      "Long Straddle",       "Long call and put at the same strike." },
        { StrategyPreset::ShortStraddle,     "Short Straddle",      "Short call and put at the same strike." },
        { StrategyPreset::LongStrangle,      "Long Strangle",       "Long out-of-the-money call and put." },
        { StrategyPreset::ShortStrangle,     "Short Strangle",      "Short out-of-the-money call and put." },
        { StrategyPreset::IronCondor,        "Iron Condor",         "Short strangle hedged with further wings." },
        { StrategyPreset::IronButterfly,     "Iron Butterfly",      "Short straddle hedged with wings." },
        { StrategyPreset::LongCallButterfly, "Long Call Butterfly", "Long wings, short two body calls." },
        { StrategyPreset::CallCalendar,      "Call Calendar",       "Short near-dated call, long far-dated call." },
        { StrategyPreset::PutCalendar,       "Put Calendar",        "Short near-dated put, long far-dated put." },
        { StrategyPreset::RiskReversal,      "Risk Reversal",       "Short out-of-the-money put, long out-of-the-money call." },
    };
    return presets;
}

/// Rounds a price to the nearest strike on a grid of `step`.
inline double roundToStrike(double value, double step)
{
    if (step <= 0.0) return value;
    return std::round(value / step) * step;
}

/// Builds a preset at the market. Entry prices are the model prices so the position
/// starts with zero mark-to-model P&L. `maturity` is the primary expiry in years and
/// `strikeStep` is the exchange strike interval used to place the wings.
inline Position buildPreset(StrategyPreset preset, const Market& market, double maturity, double strikeStep)
{
    Position pos;
    const double atm = roundToStrike(market.spot, strikeStep);
    const double step = strikeStep > 0.0 ? strikeStep : std::max(1.0, std::round(market.spot * 0.05));

    auto priceLeg = [&](Leg leg) {
        leg.entryPrice = legValue(leg, market, market.spot, 0.0);
        return leg;
    };
    auto option = [&](LegKind kind, double qty, double strike, double T) {
        Leg leg;
        leg.kind = kind;
        leg.quantity = qty;
        leg.strike = strike;
        leg.maturity = T;
        return priceLeg(leg);
    };
    auto stock = [&](double qty) {
        Leg leg;
        leg.kind = LegKind::Underlying;
        leg.quantity = qty;
        leg.strike = 0.0;
        leg.maturity = 0.0;
        return priceLeg(leg);
    };

    const double T = maturity;
    const double farT = maturity * 2.0;
    switch (preset) {
    case StrategyPreset::LongCall:
        pos.legs = { option(LegKind::Call, 1, atm, T) };
        break;
    case StrategyPreset::LongPut:
        pos.legs = { option(LegKind::Put, 1, atm, T) };
        break;
    case StrategyPreset::CoveredCall:
        pos.legs = { stock(1), option(LegKind::Call, -1, atm + step, T) };
        break;
    case StrategyPreset::ProtectivePut:
        pos.legs = { stock(1), option(LegKind::Put, 1, atm - step, T) };
        break;
    case StrategyPreset::Collar:
        pos.legs = { stock(1), option(LegKind::Put, 1, atm - step, T), option(LegKind::Call, -1, atm + step, T) };
        break;
    case StrategyPreset::BullCallSpread:
        pos.legs = { option(LegKind::Call, 1, atm, T), option(LegKind::Call, -1, atm + step, T) };
        break;
    case StrategyPreset::BearPutSpread:
        pos.legs = { option(LegKind::Put, 1, atm, T), option(LegKind::Put, -1, atm - step, T) };
        break;
    case StrategyPreset::BullPutSpread:
        pos.legs = { option(LegKind::Put, -1, atm, T), option(LegKind::Put, 1, atm - step, T) };
        break;
    case StrategyPreset::BearCallSpread:
        pos.legs = { option(LegKind::Call, -1, atm, T), option(LegKind::Call, 1, atm + step, T) };
        break;
    case StrategyPreset::LongStraddle:
        pos.legs = { option(LegKind::Call, 1, atm, T), option(LegKind::Put, 1, atm, T) };
        break;
    case StrategyPreset::ShortStraddle:
        pos.legs = { option(LegKind::Call, -1, atm, T), option(LegKind::Put, -1, atm, T) };
        break;
    case StrategyPreset::LongStrangle:
        pos.legs = { option(LegKind::Put, 1, atm - step, T), option(LegKind::Call, 1, atm + step, T) };
        break;
    case StrategyPreset::ShortStrangle:
        pos.legs = { option(LegKind::Put, -1, atm - step, T), option(LegKind::Call, -1, atm + step, T) };
        break;
    case StrategyPreset::IronCondor:
        pos.legs = { option(LegKind::Put, 1, atm - 2 * step, T), option(LegKind::Put, -1, atm - step, T),
                     option(LegKind::Call, -1, atm + step, T), option(LegKind::Call, 1, atm + 2 * step, T) };
        break;
    case StrategyPreset::IronButterfly:
        pos.legs = { option(LegKind::Put, 1, atm - step, T), option(LegKind::Put, -1, atm, T),
                     option(LegKind::Call, -1, atm, T), option(LegKind::Call, 1, atm + step, T) };
        break;
    case StrategyPreset::LongCallButterfly:
        pos.legs = { option(LegKind::Call, 1, atm - step, T), option(LegKind::Call, -2, atm, T),
                     option(LegKind::Call, 1, atm + step, T) };
        break;
    case StrategyPreset::CallCalendar:
        pos.legs = { option(LegKind::Call, -1, atm, T), option(LegKind::Call, 1, atm, farT) };
        break;
    case StrategyPreset::PutCalendar:
        pos.legs = { option(LegKind::Put, -1, atm, T), option(LegKind::Put, 1, atm, farT) };
        break;
    case StrategyPreset::RiskReversal:
        pos.legs = { option(LegKind::Put, -1, atm - step, T), option(LegKind::Call, 1, atm + step, T) };
        break;
    }
    return pos;
}

} // namespace pricing
