//
//  ChainStrategy.h
//  OptionPricing
//
//  Bridges a loaded option chain and the strategy builder: an index of listed expiries
//  and strikes, market-marking of legs (mid price and implied vol from the chain), and
//  presets built on the listed strike grid with chain entry prices.
//

#pragma once

#include "Activity.h"
#include "Strategy.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace pricing {

/// One listed expiry with its strike ladder.
struct ListedExpiry {
    ExpiryKey key;
    std::vector<double> strikes;   ///< ascending
};

/// Lookup structure over a chain: expiries, strikes per expiry and quotes per contract.
class ChainIndex {
public:
    ChainIndex() = default;
    explicit ChainIndex(const std::vector<ChainQuote>& quotes) { build(quotes); }

    void build(const std::vector<ChainQuote>& quotes)
    {
        m_expiries.clear();
        m_quotes.clear();
        std::map<long long, std::map<double, int>> strikeSets;
        for (const ChainQuote& q : quotes) {
            if (q.maturity <= 0.0 || q.strike <= 0.0) continue;
            const long long bucket = detail::expiryBucket(q.maturity);
            ListedExpiry& e = m_expiries[bucket];
            if (e.key.maturity == 0.0) {
                e.key.maturity = q.maturity;
                e.key.expiryDate = q.expiryDate;
                e.key.daysToExpiry = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::llround(q.maturity * 365.0));
            }
            strikeSets[bucket][q.strike] = 0;
            m_quotes[std::make_tuple(bucket, q.strike, q.type == OptionType::Call ? 0 : 1)] = q;
        }
        for (auto& [bucket, e] : m_expiries) {
            for (const auto& [strike, unused] : strikeSets[bucket]) e.strikes.push_back(strike);
        }
    }

    bool empty() const { return m_expiries.empty(); }

    std::vector<const ListedExpiry*> expiries() const
    {
        std::vector<const ListedExpiry*> out;
        for (const auto& [bucket, e] : m_expiries) out.push_back(&e);
        return out;
    }

    const ListedExpiry* expiry(double maturity) const
    {
        const auto it = m_expiries.find(detail::expiryBucket(maturity));
        return it == m_expiries.end() ? nullptr : &it->second;
    }

    /// Listed expiry whose maturity is closest to `target` (nullptr when the chain is empty).
    const ListedExpiry* nearestExpiry(double target) const
    {
        const ListedExpiry* best = nullptr;
        for (const auto& [bucket, e] : m_expiries) {
            if (!best || std::fabs(e.key.maturity - target) < std::fabs(best->key.maturity - target)) best = &e;
        }
        return best;
    }

    /// First listed expiry with at least `minDays` to run, else the last one.
    const ListedExpiry* firstExpiryAtLeast(int minDays) const
    {
        const ListedExpiry* last = nullptr;
        for (const auto& [bucket, e] : m_expiries) {
            if (e.key.daysToExpiry >= minDays) return &e;
            last = &e;
        }
        return last;
    }

    const ChainQuote* quote(double maturity, double strike, OptionType type) const
    {
        const auto it = m_quotes.find(std::make_tuple(detail::expiryBucket(maturity), strike, type == OptionType::Call ? 0 : 1));
        return it == m_quotes.end() ? nullptr : &it->second;
    }

    /// Listed strike nearest to `target` for the expiry, or 0 when none is listed.
    double nearestStrike(double maturity, double target) const
    {
        const ListedExpiry* e = expiry(maturity);
        if (!e || e->strikes.empty()) return 0.0;
        double best = e->strikes.front();
        for (double k : e->strikes) {
            if (std::fabs(k - target) < std::fabs(best - target)) best = k;
        }
        return best;
    }

    /// The strike `steps` positions above (positive) or below (negative) `from` on the
    /// expiry's ladder, clamped to the ends. Returns `from` when it is not listed.
    double strikeOffset(double maturity, double from, int steps) const
    {
        const ListedExpiry* e = expiry(maturity);
        if (!e || e->strikes.empty()) return from;
        const auto it = std::lower_bound(e->strikes.begin(), e->strikes.end(), from - 1e-9);
        if (it == e->strikes.end() || std::fabs(*it - from) > 1e-9) return from;
        const long index = std::clamp<long>(static_cast<long>(it - e->strikes.begin()) + steps, 0, static_cast<long>(e->strikes.size()) - 1);
        return e->strikes[static_cast<size_t>(index)];
    }

private:
    std::map<long long, ListedExpiry> m_expiries;
    std::map<std::tuple<long long, double, int>, ChainQuote> m_quotes;
};

/// Implied vol of a chain quote under the given market, or 0 when it cannot be solved.
inline double chainImpliedVol(const ChainQuote& q, const ActivityMarket& market)
{
    Inputs in;
    in.model = market.model;
    in.spot = market.spot;
    in.strike = q.strike;
    in.riskFreeRate = market.rateFor(q.maturity);
    in.dividendYield = market.dividendYield;
    in.maturity = q.maturity;
    const ImpliedVolResult iv = impliedVolatility(in, q.type, q.mid);
    return iv.status == ImpliedVolResult::Status::Converged ? iv.volatility : 0.0;
}

/// Copies the chain's mid, implied vol, maturity and expiry date onto an option leg.
/// Returns false (and clears the market price) when the contract is not listed.
inline bool markLegToChain(Leg& leg, const ChainIndex& chain, const ActivityMarket& market)
{
    if (leg.kind == LegKind::Underlying) {
        leg.marketPrice = market.spot;
        leg.expiryDate.clear();
        return true;
    }
    const ChainQuote* q = chain.quote(leg.maturity, leg.strike, optionTypeOf(leg.kind));
    if (!q) {
        leg.marketPrice = 0.0;
        return false;
    }
    leg.marketPrice = q->mid;
    leg.maturity = q->maturity;
    leg.expiryDate = q->expiryDate;
    const double vol = chainImpliedVol(*q, market);
    if (vol > 0.0) leg.volatility = vol;
    return true;
}

inline void markPositionToChain(Position& pos, const ChainIndex& chain, const ActivityMarket& market)
{
    for (Leg& leg : pos.legs) markLegToChain(leg, chain, market);
}

/// Builds a preset on the listed strike ladder of `nearMaturity` (and the next suitable
/// listed expiry for calendars). Entry prices are chain mids and each leg carries the
/// chain's implied vol, so the position starts at zero mark-to-market P&L.
inline Position buildPresetFromChain(StrategyPreset preset, const ChainIndex& chain, const ActivityMarket& market,
                                     double nearMaturity, double multiplier)
{
    Position pos;
    pos.multiplier = multiplier;
    const ListedExpiry* near = chain.expiry(nearMaturity);
    if (!near) near = chain.nearestExpiry(nearMaturity);
    if (!near) return pos;
    const double T = near->key.maturity;

    // Far expiry for calendars: the first listed expiry at least twice as far out, else the last.
    const ListedExpiry* far = nullptr;
    for (const ListedExpiry* e : chain.expiries()) {
        if (e->key.maturity >= 2.0 * T - 1e-9 && e->key.maturity > T + 1e-9) { far = e; break; }
        if (e->key.maturity > T + 1e-9) far = e;
    }
    const double farT = far ? far->key.maturity : T;

    const double atm = chain.nearestStrike(T, market.spot);
    auto option = [&](LegKind kind, double qty, double strike, double maturity) {
        Leg leg;
        leg.kind = kind;
        leg.quantity = qty;
        // Re-snap: the far expiry may not list the near expiry's strike.
        leg.strike = chain.nearestStrike(maturity, strike);
        leg.maturity = maturity;
        markLegToChain(leg, chain, market);
        leg.entryPrice = leg.marketPrice;
        return leg;
    };
    auto stock = [&](double qty) {
        Leg leg;
        leg.kind = LegKind::Underlying;
        leg.quantity = qty;
        leg.strike = 0.0;
        leg.maturity = 0.0;
        leg.marketPrice = market.spot;
        leg.entryPrice = market.spot;
        return leg;
    };
    const double up1 = chain.strikeOffset(T, atm, 1), up2 = chain.strikeOffset(T, atm, 2);
    const double dn1 = chain.strikeOffset(T, atm, -1), dn2 = chain.strikeOffset(T, atm, -2);

    switch (preset) {
    case StrategyPreset::LongCall:          pos.legs = { option(LegKind::Call, 1, atm, T) }; break;
    case StrategyPreset::LongPut:           pos.legs = { option(LegKind::Put, 1, atm, T) }; break;
    case StrategyPreset::CoveredCall:       pos.legs = { stock(1), option(LegKind::Call, -1, up1, T) }; break;
    case StrategyPreset::ProtectivePut:     pos.legs = { stock(1), option(LegKind::Put, 1, dn1, T) }; break;
    case StrategyPreset::Collar:            pos.legs = { stock(1), option(LegKind::Put, 1, dn1, T), option(LegKind::Call, -1, up1, T) }; break;
    case StrategyPreset::BullCallSpread:    pos.legs = { option(LegKind::Call, 1, atm, T), option(LegKind::Call, -1, up1, T) }; break;
    case StrategyPreset::BearPutSpread:     pos.legs = { option(LegKind::Put, 1, atm, T), option(LegKind::Put, -1, dn1, T) }; break;
    case StrategyPreset::BullPutSpread:     pos.legs = { option(LegKind::Put, -1, atm, T), option(LegKind::Put, 1, dn1, T) }; break;
    case StrategyPreset::BearCallSpread:    pos.legs = { option(LegKind::Call, -1, atm, T), option(LegKind::Call, 1, up1, T) }; break;
    case StrategyPreset::LongStraddle:      pos.legs = { option(LegKind::Call, 1, atm, T), option(LegKind::Put, 1, atm, T) }; break;
    case StrategyPreset::ShortStraddle:     pos.legs = { option(LegKind::Call, -1, atm, T), option(LegKind::Put, -1, atm, T) }; break;
    case StrategyPreset::LongStrangle:      pos.legs = { option(LegKind::Put, 1, dn1, T), option(LegKind::Call, 1, up1, T) }; break;
    case StrategyPreset::ShortStrangle:     pos.legs = { option(LegKind::Put, -1, dn1, T), option(LegKind::Call, -1, up1, T) }; break;
    case StrategyPreset::IronCondor:        pos.legs = { option(LegKind::Put, 1, dn2, T), option(LegKind::Put, -1, dn1, T),
                                                         option(LegKind::Call, -1, up1, T), option(LegKind::Call, 1, up2, T) }; break;
    case StrategyPreset::IronButterfly:     pos.legs = { option(LegKind::Put, 1, dn1, T), option(LegKind::Put, -1, atm, T),
                                                         option(LegKind::Call, -1, atm, T), option(LegKind::Call, 1, up1, T) }; break;
    case StrategyPreset::LongCallButterfly: pos.legs = { option(LegKind::Call, 1, dn1, T), option(LegKind::Call, -2, atm, T), option(LegKind::Call, 1, up1, T) }; break;
    case StrategyPreset::CallCalendar:      pos.legs = { option(LegKind::Call, -1, atm, T), option(LegKind::Call, 1, atm, farT) }; break;
    case StrategyPreset::PutCalendar:       pos.legs = { option(LegKind::Put, -1, atm, T), option(LegKind::Put, 1, atm, farT) }; break;
    case StrategyPreset::RiskReversal:      pos.legs = { option(LegKind::Put, -1, dn1, T), option(LegKind::Call, 1, up1, T) }; break;
    }
    return pos;
}

} // namespace pricing
