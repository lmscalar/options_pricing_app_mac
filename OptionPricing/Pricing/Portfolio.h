//
//  Portfolio.h
//  OptionPricing
//
//  A book of stock and option holdings across several underlyings, valued with the
//  Black-Scholes-Merton legs from Strategy.h. Holdings carry their own implied volatility
//  and an optional observed mark; each underlying has its own Market (spot, rate, yield,
//  default vol). Qt-free so the risk library and the tests can use it directly.
//

#pragma once

#include "Strategy.h"
#include "Csv.h"
#include "DayCount.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace pricing {

/// One line of the book. Quantity is signed: positive long, negative short; shares for the
/// underlying, contracts for options.
struct Holding {
    std::string symbol;
    LegKind kind = LegKind::Underlying;
    double quantity = 0.0;
    double strike = 0.0;          ///< options only
    double maturity = 0.0;        ///< years to expiry (options only)
    std::string expiryDate;       ///< ISO date, informational
    double entryPrice = 0.0;      ///< per share / per contract unit
    double volatility = 0.0;      ///< implied vol used to value the option (decimal); 0 = the underlying's market vol
    double multiplier = 1.0;      ///< 100 for US equity options, 1 for shares
    double markPrice = 0.0;       ///< observed market price per unit (chain mid), 0 = none
    std::string currency = "USD"; ///< ISO code of the prices (entry, mark, spot); the host converts to its reporting currency
    std::string group;            ///< strategy / book the holding belongs to (host-defined label)
    std::string tradeDate;        ///< ISO date the trade was entered, informational

    bool isOption() const { return kind != LegKind::Underlying; }
};

struct Portfolio {
    std::vector<Holding> holdings;
};

/// Per-underlying market data keyed by symbol.
using Markets = std::map<std::string, Market>;

inline Market marketFor(const Markets& markets, const std::string& symbol)
{
    const auto it = markets.find(symbol);
    return it == markets.end() ? Market{} : it->second;
}

/// Years to an ISO expiry from an ISO valuation date (Actual/365); 0 when either is invalid or past.
inline double maturityFromDates(const std::string& valuationIso, const std::string& expiryIso)
{
    CivilDate from, to;
    if (!parseIsoDate(valuationIso, from) || !parseIsoDate(expiryIso, to)) return 0.0;
    return std::max(0.0, yearFraction(from, to, DayCount::Actual365));
}

/// Model value of one unit of the holding at `spot`, with the vol scaled by `volFactor`
/// (1 = unchanged) and `elapsed` years passed.
inline double holdingUnitValue(const Holding& h, const Market& market, double spot, double volFactor, double elapsed)
{
    if (!h.isOption()) return spot;
    Leg leg;
    leg.kind = h.kind;
    leg.quantity = 1.0;
    leg.strike = h.strike;
    leg.maturity = h.maturity;
    leg.volatility = (h.volatility > 0.0 ? h.volatility : market.volatility) * std::max(0.01, volFactor);
    return legValue(leg, market, spot, elapsed);
}

/// Greeks of one unit of the holding.
inline Greeks holdingUnitGreeks(const Holding& h, const Market& market, double spot, double volFactor, double elapsed)
{
    Leg leg;
    leg.kind = h.kind;
    leg.quantity = 1.0;
    leg.strike = h.strike;
    leg.maturity = h.maturity;
    leg.volatility = (h.volatility > 0.0 ? h.volatility : market.volatility) * std::max(0.01, volFactor);
    return legGreeks(leg, market, spot, elapsed);
}

/// Market value of the holding (quantity x multiplier x unit value). When `useMarks` is
/// set and the holding carries an observed mark, that mark is used at the current spot.
inline double holdingValue(const Holding& h, const Market& market, double spot, double volFactor, double elapsed, bool useMarks)
{
    const bool atCurrent = volFactor == 1.0 && elapsed == 0.0 && spot == market.spot;
    const double unit = (useMarks && atCurrent && h.markPrice > 0.0) ? h.markPrice : holdingUnitValue(h, market, spot, volFactor, elapsed);
    return h.quantity * h.multiplier * unit;
}

inline double holdingCost(const Holding& h) { return h.quantity * h.multiplier * h.entryPrice; }

/// Dollar sensitivities of a set of holdings (one underlying or the whole book).
struct Exposure {
    double marketValue = 0.0;
    double cost = 0.0;
    double pnl = 0.0;
    double delta = 0.0;          ///< share-equivalent delta (sum of quantity x multiplier x delta)
    double deltaDollars = 0.0;   ///< delta x spot: P&L for a 100% move, i.e. per 1% move / 100
    double gammaDollars = 0.0;   ///< gamma x spot^2 / 100: change in delta dollars per 1% move
    double vega = 0.0;           ///< P&L per 1 vol point
    double theta = 0.0;          ///< P&L per calendar day
    double rho = 0.0;            ///< P&L per 1% rate
    int holdings = 0;
};

inline Exposure exposure(const Holding& h, const Market& market, bool useMarks = true)
{
    Exposure e;
    const double scale = h.quantity * h.multiplier;
    e.marketValue = holdingValue(h, market, market.spot, 1.0, 0.0, useMarks);
    e.cost = holdingCost(h);
    e.pnl = e.marketValue - e.cost;
    const Greeks g = holdingUnitGreeks(h, market, market.spot, 1.0, 0.0);
    e.delta = scale * g.delta;
    e.deltaDollars = e.delta * market.spot;
    e.gammaDollars = scale * g.gamma * market.spot * market.spot / 100.0;
    e.vega = scale * g.vega;
    e.theta = scale * g.theta;
    e.rho = scale * g.rho;
    e.holdings = 1;
    return e;
}

inline Exposure& operator+=(Exposure& a, const Exposure& b)
{
    a.marketValue += b.marketValue; a.cost += b.cost; a.pnl += b.pnl; a.delta += b.delta; a.deltaDollars += b.deltaDollars;
    a.gammaDollars += b.gammaDollars; a.vega += b.vega; a.theta += b.theta; a.rho += b.rho; a.holdings += b.holdings;
    return a;
}

/// Exposure per underlying plus the total.
struct BookExposure {
    std::map<std::string, Exposure> byUnderlying;
    Exposure total;
};

inline BookExposure bookExposure(const Portfolio& p, const Markets& markets, bool useMarks = true)
{
    BookExposure out;
    for (const Holding& h : p.holdings) {
        const Exposure e = exposure(h, marketFor(markets, h.symbol), useMarks);
        out.byUnderlying[h.symbol] += e;
        out.total += e;
    }
    return out;
}

/// Full revaluation of the book under per-underlying spot returns (decimal), a relative
/// vol change (0.2 = vols up 20%) and `elapsed` years, versus the current model value.
inline double revaluePnl(const Portfolio& p, const Markets& markets, const std::map<std::string, double>& spotReturns,
                         double volRelativeChange, double elapsed)
{
    double pnl = 0.0;
    for (const Holding& h : p.holdings) {
        const Market m = marketFor(markets, h.symbol);
        const auto r = spotReturns.find(h.symbol);
        const double shockedSpot = m.spot * (1.0 + (r == spotReturns.end() ? 0.0 : r->second));
        const double now = holdingValue(h, m, m.spot, 1.0, 0.0, false);
        const double then = holdingValue(h, m, shockedSpot, 1.0 + volRelativeChange, elapsed, false);
        pnl += then - now;
    }
    return pnl;
}

inline std::vector<std::string> underlyings(const Portfolio& p)
{
    std::vector<std::string> out;
    for (const Holding& h : p.holdings) {
        if (std::find(out.begin(), out.end(), h.symbol) == out.end()) out.push_back(h.symbol);
    }
    return out;
}

} // namespace pricing
