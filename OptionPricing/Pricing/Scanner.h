//
//  Scanner.h
//  OptionPricing
//
//  Trade-idea scanner over option chains. Per chain: volatility and activity metrics
//  (ATM implied vol at a target tenor, term-structure slope, 25-delta put skew, the
//  straddle-implied expected move, put/call ratios, liquidity). Then candidate strategies
//  are built on the listed strike ladder with delta-targeted strikes, marked at chain mids
//  with the chain's implied vols, analysed (probability of profit, max profit / loss,
//  return on risk, expected P&L) and ranked. Qt-free so the tests can drive it directly.
//

#pragma once

#include "Activity.h"
#include "ChainStrategy.h"
#include "Strategy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pricing {
namespace scan {

/// What a strategy needs the underlying to do.
enum class Bias { Any, Bullish, Bearish, Neutral, Volatile };

inline const char* biasName(Bias b)
{
    switch (b) {
    case Bias::Any:      return "Any";
    case Bias::Bullish:  return "Bullish";
    case Bias::Bearish:  return "Bearish";
    case Bias::Neutral:  return "Neutral";
    case Bias::Volatile: return "Volatile";
    }
    return "";
}

inline Bias biasOf(StrategyPreset p)
{
    switch (p) {
    case StrategyPreset::LongCall: case StrategyPreset::CoveredCall: case StrategyPreset::ProtectivePut: case StrategyPreset::Collar:
    case StrategyPreset::BullCallSpread: case StrategyPreset::BullPutSpread: case StrategyPreset::RiskReversal:
        return Bias::Bullish;
    case StrategyPreset::LongPut: case StrategyPreset::BearPutSpread: case StrategyPreset::BearCallSpread:
        return Bias::Bearish;
    case StrategyPreset::ShortStraddle: case StrategyPreset::ShortStrangle: case StrategyPreset::IronCondor: case StrategyPreset::IronButterfly:
    case StrategyPreset::LongCallButterfly: case StrategyPreset::CallCalendar: case StrategyPreset::PutCalendar:
        return Bias::Neutral;
    case StrategyPreset::LongStraddle: case StrategyPreset::LongStrangle:
        return Bias::Volatile;
    }
    return Bias::Any;
}

/// Strategies whose loss is capped by the structure itself (no naked short option, no stock).
inline bool definedRisk(StrategyPreset p)
{
    switch (p) {
    case StrategyPreset::ShortStraddle: case StrategyPreset::ShortStrangle: case StrategyPreset::RiskReversal:
    case StrategyPreset::CoveredCall: case StrategyPreset::ProtectivePut: case StrategyPreset::Collar:
        return false;
    default:
        return true;
    }
}

/// Net premium sign the structure is built for.
inline bool isCreditStrategy(StrategyPreset p)
{
    switch (p) {
    case StrategyPreset::BullPutSpread: case StrategyPreset::BearCallSpread: case StrategyPreset::ShortStraddle: case StrategyPreset::ShortStrangle:
    case StrategyPreset::IronCondor: case StrategyPreset::IronButterfly:
        return true;
    default:
        return false;
    }
}

inline const char* presetName(StrategyPreset p)
{
    for (const PresetInfo& info : strategyPresets()) if (info.id == p) return info.name;
    return "";
}

inline std::vector<StrategyPreset> allPresets()
{
    std::vector<StrategyPreset> out;
    for (const PresetInfo& info : strategyPresets()) out.push_back(info.id);
    return out;
}

/// One listed contract with its implied vol and delta under the scan market.
struct LadderPoint {
    const ChainQuote* quote = nullptr;
    double iv = 0.0;
    double delta = 0.0;     ///< signed (calls positive, puts negative)
    double spread = 0.0;    ///< (ask - bid) / mid, 0 when bid/ask are absent
};

/// Implied vol of a quote: solved from the mid; the vendor's figure only when the solver
/// merely failed to converge. A mid outside the no-arbitrage bounds (a stale last trade,
/// which is what chains carry outside market hours or without a quote entitlement) gives 0
/// so the contract is left out rather than priced off bad data.
inline double quoteImpliedVol(const ChainQuote& q, const ActivityMarket& market)
{
    Inputs in;
    in.model = market.model;
    in.spot = market.spot;
    in.strike = q.strike;
    in.riskFreeRate = market.rateFor(q.maturity);
    in.dividendYield = market.dividendYield;
    in.maturity = q.maturity;
    const ImpliedVolResult iv = impliedVolatility(in, q.type, q.mid);
    if (iv.status == ImpliedVolResult::Status::Converged) return iv.volatility;
    if (iv.status == ImpliedVolResult::Status::NotConverged) return q.vendorImpliedVol;
    return 0.0;
}

/// Every listed strike of one type at one expiry, with vol and delta (strikes ascending).
inline std::vector<LadderPoint> ladder(const ChainIndex& chain, const ListedExpiry& expiry, OptionType type, const ActivityMarket& market)
{
    std::vector<LadderPoint> out;
    for (double strike : expiry.strikes) {
        const ChainQuote* q = chain.quote(expiry.key.maturity, strike, type);
        if (!q || q->mid <= 0.0) continue;
        LadderPoint p;
        p.quote = q;
        p.iv = quoteImpliedVol(*q, market);
        if (p.iv <= 0.0) continue;
        Inputs in;
        in.model = market.model;
        in.spot = market.spot;
        in.strike = strike;
        in.riskFreeRate = market.rateFor(q->maturity);
        in.dividendYield = market.dividendYield;
        in.volatility = p.iv;
        in.maturity = q->maturity;
        if (!isValid(in)) continue;
        const Result r = price(in);
        p.delta = type == OptionType::Call ? r.call.delta : r.put.delta;
        p.spread = (q->ask > q->bid && q->bid >= 0.0 && q->mid > 0.0) ? (q->ask - q->bid) / q->mid : 0.0;
        out.push_back(p);
    }
    return out;
}

inline const LadderPoint* nearestStrike(const std::vector<LadderPoint>& l, double strike)
{
    const LadderPoint* best = nullptr;
    for (const LadderPoint& p : l) if (!best || std::fabs(p.quote->strike - strike) < std::fabs(best->quote->strike - strike)) best = &p;
    return best;
}

/// Contract whose |delta| is closest to `absDelta`; restricted to out-of-the-money strikes
/// when `otmOnly` so a 30-delta target never lands in the money on a sparse ladder.
inline const LadderPoint* atDelta(const std::vector<LadderPoint>& l, double absDelta, double spot, bool otmOnly = true)
{
    const LadderPoint* best = nullptr;
    for (const LadderPoint& p : l) {
        if (otmOnly) {
            const bool call = p.delta >= 0.0;
            if ((call && p.quote->strike < spot) || (!call && p.quote->strike > spot)) continue;
        }
        if (!best || std::fabs(std::fabs(p.delta) - absDelta) < std::fabs(std::fabs(best->delta) - absDelta)) best = &p;
    }
    return best ? best : (otmOnly ? atDelta(l, absDelta, spot, false) : nullptr);
}

/// Listed expiry closest to `targetDays` with between `minDays` and `maxDays` to run.
inline const ListedExpiry* expiryNear(const ChainIndex& chain, int targetDays, int minDays, int maxDays)
{
    const ListedExpiry* best = nullptr;
    for (const ListedExpiry* e : chain.expiries()) {
        if (e->key.daysToExpiry < minDays || e->key.daysToExpiry > maxDays) continue;
        if (!best || std::abs(e->key.daysToExpiry - targetDays) < std::abs(best->key.daysToExpiry - targetDays)) best = e;
    }
    return best;
}

/// Per-chain volatility and activity snapshot.
struct ChainMetrics {
    bool ok = false;
    double spot = 0.0;
    ExpiryKey expiry;              ///< tenor the vol figures refer to
    ExpiryKey farExpiry;           ///< later tenor used for the term slope (may equal expiry)
    double atmIv = 0.0;            ///< decimal
    double farAtmIv = 0.0;
    double termSlope = 0.0;        ///< farAtmIv - atmIv; negative = near-dated vol bid (event / stress)
    double putIv25 = 0.0, callIv25 = 0.0;
    double skew = 0.0;             ///< putIv25 - callIv25, decimal (positive = puts bid)
    double expectedMove = 0.0;     ///< ATM straddle mid / spot, fraction, at `expiry`
    double putCallVolume = 0.0, putCallOpenInterest = 0.0;
    double totalVolume = 0.0, totalOpenInterest = 0.0;
    double medianSpread = 0.0;     ///< median (ask-bid)/mid of near-the-money contracts at `expiry`
    int contracts = 0, expiries = 0;
};

inline ChainMetrics chainMetrics(const std::vector<ChainQuote>& quotes, const ActivityMarket& market, int targetDays = 35, int minDays = 7, int maxDays = 120)
{
    ChainMetrics m;
    m.spot = market.spot;
    m.contracts = static_cast<int>(quotes.size());
    if (quotes.empty() || market.spot <= 0.0) return m;
    ChainIndex chain(quotes);
    m.expiries = static_cast<int>(chain.expiries().size());
    const ListedExpiry* e = expiryNear(chain, targetDays, minDays, maxDays);
    if (!e) e = chain.firstExpiryAtLeast(minDays);
    if (!e) return m;
    m.expiry = e->key;
    const std::vector<LadderPoint> calls = ladder(chain, *e, OptionType::Call, market);
    const std::vector<LadderPoint> puts = ladder(chain, *e, OptionType::Put, market);
    const LadderPoint* atmC = nearestStrike(calls, market.spot);
    const LadderPoint* atmP = nearestStrike(puts, market.spot);
    if (atmC && atmP) m.atmIv = 0.5 * (atmC->iv + atmP->iv);
    else if (atmC || atmP) m.atmIv = (atmC ? atmC : atmP)->iv;
    if (atmC && atmP) m.expectedMove = (atmC->quote->mid + atmP->quote->mid) / market.spot;
    if (const LadderPoint* p = atDelta(puts, 0.25, market.spot)) m.putIv25 = p->iv;
    if (const LadderPoint* c = atDelta(calls, 0.25, market.spot)) m.callIv25 = c->iv;
    if (m.putIv25 > 0.0 && m.callIv25 > 0.0) m.skew = m.putIv25 - m.callIv25;

    // Term slope: the first expiry at least twice as far out, else the last later one.
    const ListedExpiry* far = nullptr;
    for (const ListedExpiry* x : chain.expiries()) {
        if (x->key.daysToExpiry <= e->key.daysToExpiry) continue;
        far = x;
        if (x->key.daysToExpiry >= 2 * e->key.daysToExpiry) break;
    }
    if (far) {
        m.farExpiry = far->key;
        const std::vector<LadderPoint> fc = ladder(chain, *far, OptionType::Call, market);
        const std::vector<LadderPoint> fp = ladder(chain, *far, OptionType::Put, market);
        const LadderPoint* c = nearestStrike(fc, market.spot);
        const LadderPoint* p = nearestStrike(fp, market.spot);
        if (c && p) m.farAtmIv = 0.5 * (c->iv + p->iv);
        else if (c || p) m.farAtmIv = (c ? c : p)->iv;
        if (m.farAtmIv > 0.0 && m.atmIv > 0.0) m.termSlope = m.farAtmIv - m.atmIv;
    } else {
        m.farExpiry = m.expiry;
    }

    const ActivitySummary a = summarizeActivity(quotes, ActivityMetric::Volume, SideFilter::Both);
    m.putCallVolume = a.putCallVolumeRatio;
    m.putCallOpenInterest = a.putCallOpenInterestRatio;
    m.totalVolume = a.callVolume + a.putVolume;
    m.totalOpenInterest = a.callOpenInterest + a.putOpenInterest;

    std::vector<double> spreads;
    for (const std::vector<LadderPoint>* l : { &calls, &puts }) {
        for (const LadderPoint& p : *l) if (std::fabs(p.quote->strike / market.spot - 1.0) <= 0.10 && p.spread > 0.0) spreads.push_back(p.spread);
    }
    if (!spreads.empty()) {
        std::sort(spreads.begin(), spreads.end());
        const size_t n = spreads.size();
        m.medianSpread = n % 2 ? spreads[n / 2] : 0.5 * (spreads[n / 2 - 1] + spreads[n / 2]);
    }
    m.ok = m.atmIv > 0.0;
    return m;
}

/// Scan settings.
struct Criteria {
    int targetDays = 35;
    int minDays = 20;
    int maxDays = 60;
    double shortDelta = 0.30;        ///< |delta| of the short (or out-of-the-money) strikes
    double wingDelta = 0.15;         ///< |delta| of the protective wings
    double minProbability = 0.0;     ///< probability of profit, fraction
    double minReturnOnRisk = 0.0;    ///< max profit / |max loss|, fraction
    double maxSpread = 0.25;         ///< per-leg (ask - bid) / mid; 0 = ignore
    double minOpenInterest = 0.0;    ///< per leg; 0 = ignore
    Bias bias = Bias::Any;
    std::vector<StrategyPreset> strategies;   ///< empty = every preset
    bool definedRiskOnly = false;
    size_t maxPerTicker = 3;
    double multiplier = 100.0;
};

/// A ranked candidate trade.
struct Idea {
    std::string ticker;
    StrategyPreset preset = StrategyPreset::IronCondor;
    std::string strategy;
    Bias bias = Bias::Any;
    ExpiryKey expiry;
    Position position;              ///< legs marked at chain mids with chain implied vols
    std::string legs;               ///< "-1 P 740 / +1 P 730"
    double netPremium = 0.0;        ///< > 0 debit paid, < 0 credit received (whole position)
    double maxProfit = 0.0, maxLoss = 0.0;
    bool unboundedProfit = false, unboundedLoss = false;
    double probabilityOfProfit = 0.0;
    double expectedPnl = 0.0;       ///< risk-neutral, at the first expiry
    double returnOnRisk = 0.0;      ///< profit / loss; open-ended sides measured at ±2 expected moves
    std::vector<double> breakevens;
    Greeks greeks;
    double atmIv = 0.0;
    double expectedMove = 0.0;      ///< fraction
    double worstSpread = 0.0;       ///< widest per-leg (ask-bid)/mid
    double minOpenInterest = 0.0;   ///< thinnest leg
    double score = 0.0;
    std::string rationale;
};

namespace detail {

inline std::string trimmedNumber(double v, int decimals = 2)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

inline Leg legFrom(const LadderPoint& p, LegKind kind, double qty)
{
    Leg leg;
    leg.kind = kind;
    leg.quantity = qty;
    leg.strike = p.quote->strike;
    leg.maturity = p.quote->maturity;
    leg.volatility = p.iv;
    leg.entryPrice = p.quote->mid;
    leg.marketPrice = p.quote->mid;
    leg.expiryDate = p.quote->expiryDate;
    return leg;
}

inline Leg stockLeg(double spot, double qty)
{
    Leg leg;
    leg.kind = LegKind::Underlying;
    leg.quantity = qty;
    leg.strike = 0.0;
    leg.maturity = 0.0;
    leg.entryPrice = spot;
    leg.marketPrice = spot;
    return leg;
}

/// Picks the strikes for a preset on the expiry's ladders. Returns false when a required
/// contract is missing or two legs would collapse onto the same strike.
inline bool buildLegs(StrategyPreset preset, const ChainIndex& chain, const ListedExpiry& near, const ListedExpiry* far,
                      const ActivityMarket& market, const Criteria& c, std::vector<Leg>& legs, std::vector<LadderPoint>& used,
                      const std::vector<LadderPoint>& calls, const std::vector<LadderPoint>& puts)
{
    const double spot = market.spot;
    const LadderPoint* atmC = nearestStrike(calls, spot);
    const LadderPoint* atmP = nearestStrike(puts, spot);
    const LadderPoint* shortC = atDelta(calls, c.shortDelta, spot);
    const LadderPoint* shortP = atDelta(puts, c.shortDelta, spot);
    const LadderPoint* wingC = atDelta(calls, c.wingDelta, spot);
    const LadderPoint* wingP = atDelta(puts, c.wingDelta, spot);
    // Wings must sit beyond the short strikes.
    if (shortC && wingC && wingC->quote->strike <= shortC->quote->strike) wingC = nearestStrike(calls, chain.strikeOffset(near.key.maturity, shortC->quote->strike, 1));
    if (shortP && wingP && wingP->quote->strike >= shortP->quote->strike) wingP = nearestStrike(puts, chain.strikeOffset(near.key.maturity, shortP->quote->strike, -1));
    // Short strikes must be away from the money.
    if (atmC && shortC && shortC->quote->strike <= atmC->quote->strike) shortC = nearestStrike(calls, chain.strikeOffset(near.key.maturity, atmC->quote->strike, 1));
    if (atmP && shortP && shortP->quote->strike >= atmP->quote->strike) shortP = nearestStrike(puts, chain.strikeOffset(near.key.maturity, atmP->quote->strike, -1));

    auto add = [&](const LadderPoint* p, LegKind kind, double qty) {
        if (!p) return false;
        legs.push_back(legFrom(*p, kind, qty));
        used.push_back(*p);   // copied: ladders built for the far expiry do not outlive this call
        return true;
    };
    auto distinct = [&](const LadderPoint* a, const LadderPoint* b) { return a && b && std::fabs(a->quote->strike - b->quote->strike) > 1e-9; };

    switch (preset) {
    case StrategyPreset::LongCall:       return add(atmC, LegKind::Call, 1);
    case StrategyPreset::LongPut:        return add(atmP, LegKind::Put, 1);
    case StrategyPreset::CoveredCall:    legs.push_back(stockLeg(spot, 1)); return add(shortC, LegKind::Call, -1);
    case StrategyPreset::ProtectivePut:  legs.push_back(stockLeg(spot, 1)); return add(shortP, LegKind::Put, 1);
    case StrategyPreset::Collar:         legs.push_back(stockLeg(spot, 1)); return add(shortP, LegKind::Put, 1) && add(shortC, LegKind::Call, -1);
    case StrategyPreset::BullCallSpread: return distinct(atmC, shortC) && add(atmC, LegKind::Call, 1) && add(shortC, LegKind::Call, -1);
    case StrategyPreset::BearPutSpread:  return distinct(atmP, shortP) && add(atmP, LegKind::Put, 1) && add(shortP, LegKind::Put, -1);
    case StrategyPreset::BullPutSpread:  return distinct(shortP, wingP) && add(shortP, LegKind::Put, -1) && add(wingP, LegKind::Put, 1);
    case StrategyPreset::BearCallSpread: return distinct(shortC, wingC) && add(shortC, LegKind::Call, -1) && add(wingC, LegKind::Call, 1);
    case StrategyPreset::LongStraddle:   return add(atmC, LegKind::Call, 1) && add(atmP, LegKind::Put, 1);
    case StrategyPreset::ShortStraddle:  return add(atmC, LegKind::Call, -1) && add(atmP, LegKind::Put, -1);
    case StrategyPreset::LongStrangle:   return add(shortP, LegKind::Put, 1) && add(shortC, LegKind::Call, 1);
    case StrategyPreset::ShortStrangle:  return add(shortP, LegKind::Put, -1) && add(shortC, LegKind::Call, -1);
    case StrategyPreset::IronCondor:
        return distinct(shortP, wingP) && distinct(shortC, wingC) && add(wingP, LegKind::Put, 1) && add(shortP, LegKind::Put, -1)
            && add(shortC, LegKind::Call, -1) && add(wingC, LegKind::Call, 1);
    case StrategyPreset::IronButterfly:
        return distinct(atmP, shortP) && distinct(atmC, shortC) && add(shortP, LegKind::Put, 1) && add(atmP, LegKind::Put, -1)
            && add(atmC, LegKind::Call, -1) && add(shortC, LegKind::Call, 1);
    case StrategyPreset::LongCallButterfly: {
        const LadderPoint* lowC = shortP ? nearestStrike(calls, shortP->quote->strike) : nullptr;
        return distinct(lowC, atmC) && distinct(atmC, shortC) && add(lowC, LegKind::Call, 1) && add(atmC, LegKind::Call, -2) && add(shortC, LegKind::Call, 1);
    }
    case StrategyPreset::CallCalendar:
    case StrategyPreset::PutCalendar: {
        if (!far) return false;
        const OptionType type = preset == StrategyPreset::CallCalendar ? OptionType::Call : OptionType::Put;
        const LegKind kind = type == OptionType::Call ? LegKind::Call : LegKind::Put;
        const std::vector<LadderPoint> farLadder = ladder(chain, *far, type, market);
        const LadderPoint* nearAtm = type == OptionType::Call ? atmC : atmP;
        if (!nearAtm) return false;
        const LadderPoint* farAtm = nearestStrike(farLadder, nearAtm->quote->strike);
        if (!farAtm) return false;
        legs.push_back(legFrom(*nearAtm, kind, -1));
        legs.push_back(legFrom(*farAtm, kind, 1));
        used.push_back(*nearAtm);
        used.push_back(*farAtm);
        return true;
    }
    case StrategyPreset::RiskReversal:   return add(shortP, LegKind::Put, -1) && add(shortC, LegKind::Call, 1);
    }
    return false;
}

inline std::string describeLegs(const Position& pos)
{
    std::string out;
    for (const Leg& leg : pos.legs) {
        if (!out.empty()) out += " / ";
        const std::string qty = (leg.quantity > 0 ? "+" : "") + trimmedNumber(leg.quantity, 0);
        if (leg.kind == LegKind::Underlying) out += qty + "00 shares";
        else out += qty + (leg.kind == LegKind::Call ? " C " : " P ") + trimmedNumber(leg.strike);
    }
    return out;
}

} // namespace detail

/// Builds and analyses every candidate for one chain; ideas sorted by score, best first.
/// `base` supplies the model, day basis and discrete dividends; spot, rate and vol come
/// from the chain (ATM implied vol drives the probability of profit).
inline std::vector<Idea> scanChain(const std::string& ticker, const std::vector<ChainQuote>& quotes, const ActivityMarket& market,
                                   const Market& base, const Criteria& c, const ChainMetrics* precomputed = nullptr)
{
    std::vector<Idea> ideas;
    if (quotes.empty() || market.spot <= 0.0) return ideas;
    const ChainMetrics metrics = precomputed ? *precomputed : chainMetrics(quotes, market, c.targetDays, c.minDays, c.maxDays);
    if (!metrics.ok) return ideas;
    ChainIndex chain(quotes);

    // Candidate expiries: the target tenor plus the next listed one inside the window.
    std::vector<const ListedExpiry*> candidates;
    if (const ListedExpiry* e = expiryNear(chain, c.targetDays, c.minDays, c.maxDays)) {
        candidates.push_back(e);
        for (const ListedExpiry* x : chain.expiries()) {
            if (x->key.daysToExpiry > e->key.daysToExpiry && x->key.daysToExpiry <= c.maxDays) { candidates.push_back(x); break; }
        }
    }
    if (candidates.empty()) return ideas;

    const std::vector<StrategyPreset> presets = c.strategies.empty() ? allPresets() : c.strategies;
    for (const ListedExpiry* e : candidates) {
        const std::vector<LadderPoint> calls = ladder(chain, *e, OptionType::Call, market);
        const std::vector<LadderPoint> puts = ladder(chain, *e, OptionType::Put, market);
        if (calls.empty() || puts.empty()) continue;
        const ListedExpiry* far = nullptr;
        for (const ListedExpiry* x : chain.expiries()) {
            if (x->key.daysToExpiry <= e->key.daysToExpiry) continue;
            far = x;
            if (x->key.daysToExpiry >= 2 * e->key.daysToExpiry) break;
        }
        Market m = base;
        m.spot = market.spot;
        m.riskFreeRate = market.rateFor(e->key.maturity);
        m.dividendYield = market.dividendYield;
        m.volatility = metrics.atmIv > 0.0 ? metrics.atmIv : base.volatility;

        for (StrategyPreset preset : presets) {
            if (c.bias != Bias::Any && biasOf(preset) != c.bias) continue;
            if (c.definedRiskOnly && !definedRisk(preset)) continue;
            Idea idea;
            idea.position.multiplier = c.multiplier;
            std::vector<LadderPoint> used;
            if (!detail::buildLegs(preset, chain, *e, far, market, c, idea.position.legs, used, calls, puts)) continue;
            // Liquidity of every option leg.
            bool liquid = true;
            idea.minOpenInterest = 1e18;
            for (const LadderPoint& p : used) {
                idea.worstSpread = std::max(idea.worstSpread, p.spread);
                idea.minOpenInterest = std::min(idea.minOpenInterest, p.quote->openInterest);
                if (c.maxSpread > 0.0 && p.spread > c.maxSpread) liquid = false;
                if (c.minOpenInterest > 0.0 && p.quote->openInterest < c.minOpenInterest) liquid = false;
            }
            if (!liquid) continue;
            if (idea.minOpenInterest > 1e17) idea.minOpenInterest = 0.0;

            // Stale prints: a leg priced far off the ATM vol, or a structure that cannot lose, is data, not edge.
            bool consistent = true;
            for (const LadderPoint& p : used) if (p.iv < 0.4 * metrics.atmIv || p.iv > 3.0 * metrics.atmIv) consistent = false;
            if (!consistent) continue;
            const StrategyAnalysis a = analyzePosition(idea.position, m, 401);
            if (a.maxLoss >= 0.0 || a.probabilityOfProfit > 0.98) continue;
            idea.ticker = ticker;
            idea.preset = preset;
            idea.strategy = presetName(preset);
            idea.bias = biasOf(preset);
            idea.expiry = e->key;
            idea.legs = detail::describeLegs(idea.position);
            idea.netPremium = a.netPremium;
            idea.maxProfit = a.maxProfit;
            idea.maxLoss = a.maxLoss;
            idea.unboundedProfit = a.unboundedProfit;
            idea.unboundedLoss = a.unboundedLoss;
            idea.probabilityOfProfit = a.probabilityOfProfit;
            idea.expectedPnl = a.expectedPnl;
            idea.breakevens = a.breakevens;
            idea.greeks = a.greeks;
            idea.atmIv = metrics.atmIv;
            idea.expectedMove = metrics.expectedMove;
            // Open-ended sides: measure the P&L two expected moves away so the ratio stays finite.
            const double em = std::max(metrics.expectedMove, 0.02);
            const double upFar = positionPnl(idea.position, m, m.spot * (1.0 + 2.0 * em), a.horizon);
            const double downFar = positionPnl(idea.position, m, m.spot * std::max(0.05, 1.0 - 2.0 * em), a.horizon);
            const double profit = a.unboundedProfit ? std::max({ upFar, downFar, 0.0 }) : a.maxProfit;
            const double loss = a.unboundedLoss ? std::fabs(std::min({ upFar, downFar, 0.0 })) : std::fabs(a.maxLoss);
            if (profit <= 0.0 || loss <= 0.0) continue;   // nothing to gain, or no risk measured: not an idea
            idea.returnOnRisk = profit / loss;
            if (idea.returnOnRisk > 10.0) continue;   // more than 1000% on risk only happens on bad marks
            if (idea.probabilityOfProfit < c.minProbability) continue;
            if (idea.returnOnRisk < c.minReturnOnRisk) continue;
            // Score: probability-weighted return on risk, capped so lottery tickets do not dominate,
            // discounted by how wide the markets are.
            idea.score = idea.probabilityOfProfit * std::min(idea.returnOnRisk, 3.0) * (1.0 - std::min(idea.worstSpread, 0.5));

            char buf[256];
            const double premiumAbs = std::fabs(idea.netPremium);
            std::snprintf(buf, sizeof buf, "%s %s on %s%s risk (%.0f%% return on risk), %.0f%% probability of profit, 1 sd move ±%.1f%%, ATM IV %.0f%%",
                          idea.netPremium < 0 ? "Credit" : "Debit", detail::trimmedNumber(premiumAbs, 0).c_str(),
                          a.unboundedLoss ? "open-ended (" : "", (detail::trimmedNumber(loss, 0) + (a.unboundedLoss ? " at 2 sd)" : "")).c_str(),
                          idea.returnOnRisk * 100.0, idea.probabilityOfProfit * 100.0, metrics.expectedMove * 100.0, metrics.atmIv * 100.0);
            idea.rationale = buf;
            ideas.push_back(idea);
        }
    }
    std::sort(ideas.begin(), ideas.end(), [](const Idea& x, const Idea& y) { return x.score > y.score; });
    if (ideas.size() > c.maxPerTicker) ideas.resize(c.maxPerTicker);
    return ideas;
}

} // namespace scan
} // namespace pricing
