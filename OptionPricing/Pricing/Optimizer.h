//
//  Optimizer.h
//  OptionPricing
//
//  Strategy comparison and optimisation on a listed option chain. A candidate is a position
//  built from listed contracts, marked at chain mids with the chain's implied vols, analysed
//  risk-neutrally (Strategy.h) and under the user's own view: a lognormal distribution of
//  the spot at the first expiry centred on a target price with a chosen volatility. The
//  optimiser enumerates strikes and expiries for each strategy family within a band and
//  ranks the candidates by an objective; presetsOnChain() builds one candidate per preset
//  for a side-by-side comparison. Qt-free.
//

#pragma once

#include "Activity.h"
#include "ChainStrategy.h"
#include "Normal.h"
#include "Scanner.h"
#include "Strategy.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pricing {
namespace opt {

enum class Objective { ExpectedReturnOnRisk, ExpectedPnl, PnlAtTarget, ProbabilityOfProfit, ReturnOnRisk, Score };

inline const char* objectiveName(Objective o)
{
    switch (o) {
    case Objective::ExpectedReturnOnRisk: return "Expected return on risk under my view";
    case Objective::ExpectedPnl:         return "Expected P&L under my view";
    case Objective::PnlAtTarget:         return "P&L at the target price";
    case Objective::ProbabilityOfProfit: return "Probability of profit under my view";
    case Objective::ReturnOnRisk:        return "Return on risk";
    case Objective::Score:               return "Probability-weighted return on risk";
    }
    return "";
}

/// The user's forecast: where the spot is expected to be at the first expiry and how uncertain that is.
struct View {
    double targetSpot = 0.0;     ///< expected spot at the horizon (0 = the current spot)
    double volAnnual = 0.0;      ///< annualised uncertainty around the target (0 = the chain's ATM vol)
};

struct ViewMetrics {
    double expectedPnl = 0.0;          ///< E[P&L] at the first expiry under the view
    double probabilityOfProfit = 0.0;  ///< P(P&L > 0) under the view
    double pnlAtTarget = 0.0;          ///< P&L if the spot lands exactly on the target
};

/// Integrates the horizon P&L grid of `a` against a lognormal whose mean is the target.
inline ViewMetrics evaluateView(const Position& pos, const Market& m, const StrategyAnalysis& a, double targetSpot, double viewVol)
{
    ViewMetrics v;
    const double T = a.horizon;
    v.pnlAtTarget = positionPnl(pos, m, targetSpot, std::max(T, 0.0));
    if (a.spots.size() < 2 || T <= 1e-9 || viewVol <= 0.0 || targetSpot <= 0.0) {
        v.expectedPnl = v.pnlAtTarget;
        v.probabilityOfProfit = v.pnlAtTarget > 0.0 ? 1.0 : 0.0;
        return v;
    }
    const double sd = viewVol * std::sqrt(T);
    const double mu = std::log(targetSpot) - 0.5 * sd * sd;   // E[S_T] = target
    auto cdf = [&](double s) { return s <= 0.0 ? 0.0 : normalCdf((std::log(s) - mu) / sd); };
    double prob = 0.0, expectation = 0.0;
    {
        const double p = cdf(a.spots.front());
        if (a.pnlAtHorizon.front() > 0.0) prob += p;
        expectation += p * a.pnlAtHorizon.front();
    }
    for (size_t i = 1; i < a.spots.size(); ++i) {
        const double p = cdf(a.spots[i]) - cdf(a.spots[i - 1]);
        const double midPnl = 0.5 * (a.pnlAtHorizon[i] + a.pnlAtHorizon[i - 1]);
        if (midPnl > 0.0) prob += p;
        expectation += p * midPnl;
    }
    {
        const double p = 1.0 - cdf(a.spots.back());
        if (a.pnlAtHorizon.back() > 0.0) prob += p;
        expectation += p * a.pnlAtHorizon.back();
    }
    v.expectedPnl = expectation;
    v.probabilityOfProfit = std::clamp(prob, 0.0, 1.0);
    return v;
}

struct Candidate {
    StrategyPreset preset = StrategyPreset::LongCall;
    std::string strategy;
    ExpiryKey expiry;
    Position position;
    std::string legs;
    StrategyAnalysis analysis;        ///< risk-neutral analysis at the first expiry
    ViewMetrics view;
    Market market;                    ///< the market the candidate was analysed under
    double loss = 0.0;                ///< risk measured for the ratios (|max loss|, or the two-sd loss when open-ended)
    double expectedReturnOnRisk = 0.0;///< view expected P&L / loss
    double returnOnRisk = 0.0;        ///< max profit / max loss; open-ended sides measured two view-sd away
    double score = 0.0;               ///< risk-neutral PoP × min(return on risk, 3)
    double objective = 0.0;           ///< the ranked value
    double worstSpread = 0.0;
    double atmIv = 0.0;
};

struct Options {
    std::vector<StrategyPreset> families;   ///< empty = every preset
    int minDays = 20;
    int maxDays = 60;
    double band = 0.20;                     ///< strikes within spot × (1 ± band)
    int maxSpreadSteps = 6;                 ///< widest vertical spread, in listed strikes
    int maxWingSteps = 4;                   ///< widest condor / butterfly wing, in listed strikes
    size_t maxResults = 25;
    size_t maxEvaluations = 25000;          ///< safety cap on candidates analysed
    int maxExpiries = 4;                    ///< expiries used inside the window (evenly spread) to bound the search
    int gridPoints = 201;
    double multiplier = 100.0;
    Objective objective = Objective::ExpectedReturnOnRisk;
    bool definedRiskOnly = true;
    double maxLoss = 0.0;                   ///< cap on |max loss| per position, 0 = none
    double minReturnOnRisk = 0.05;          ///< drops trades whose best case is under 5% of the risk (commission fodder)
    double maxAbsDelta = 0.90;              ///< legs deeper in the money than this are excluded (no time value, stale prints)
    double minAbsDelta = 0.03;              ///< legs further out of the money than this are excluded (lottery tickets)
    double maxSpread = 0.25;                ///< per-leg (ask − bid) / mid when quotes exist
    double shortDelta = 0.30;               ///< presetsOnChain(): short strikes
    double wingDelta = 0.15;                ///< presetsOnChain(): wings
};

namespace detail {

inline double objectiveValue(const Candidate& c, Objective o)
{
    switch (o) {
    case Objective::ExpectedReturnOnRisk: return c.expectedReturnOnRisk;
    case Objective::ExpectedPnl:         return c.view.expectedPnl;
    case Objective::PnlAtTarget:         return c.view.pnlAtTarget;
    case Objective::ProbabilityOfProfit: return c.view.probabilityOfProfit;
    case Objective::ReturnOnRisk:        return c.returnOnRisk;
    case Objective::Score:               return c.score;
    }
    return 0.0;
}

/// True when every option leg expires at the first expiry (the horizon payoff is then pure intrinsic value).
inline bool singleExpiry(const Position& pos)
{
    double T = -1.0;
    for (const Leg& leg : pos.legs) {
        if (leg.kind == LegKind::Underlying) continue;
        if (T < 0.0) T = leg.maturity;
        else if (std::fabs(leg.maturity - T) > 1e-9) return false;
    }
    return true;
}

/// The expiry part of analyzePosition() without model pricing: intrinsic payoffs on the grid,
/// extremes, linearly interpolated breakevens and the risk-neutral probability of profit.
/// Used while enumerating; the survivors get the full analysis (Greeks, today's curve).
inline StrategyAnalysis quickAnalysis(const Position& pos, const Market& market, int gridPoints)
{
    StrategyAnalysis a;
    a.netPremium = positionCost(pos);
    a.horizon = earliestExpiry(pos);
    if (pos.legs.empty()) return a;
    a.spots = analysisSpotGrid(pos, market.spot, std::max(gridPoints, 11));
    auto pnlAt = [&](double s) {
        double v = 0.0;
        for (const Leg& leg : pos.legs) v += leg.quantity * pos.multiplier * ((leg.kind == LegKind::Underlying ? s : payoff(optionTypeOf(leg.kind), leg.strike, s)) - leg.entryPrice);
        return v;
    };
    a.pnlAtHorizon.reserve(a.spots.size());
    double best = -INFINITY, worst = INFINITY;
    for (double s : a.spots) { const double v = pnlAt(s); a.pnlAtHorizon.push_back(v); best = std::max(best, v); worst = std::min(worst, v); }
    for (const Leg& leg : pos.legs) if (leg.kind != LegKind::Underlying) { const double v = pnlAt(leg.strike); best = std::max(best, v); worst = std::min(worst, v); }
    { const double v = pnlAt(0.01); best = std::max(best, v); worst = std::min(worst, v); }
    double farRightSlope = 0.0;
    for (const Leg& leg : pos.legs) if (leg.kind == LegKind::Call || leg.kind == LegKind::Underlying) farRightSlope += leg.quantity * pos.multiplier;
    const double tol = 1e-9 * std::max(1.0, pos.multiplier);
    a.unboundedProfit = farRightSlope > tol;
    a.unboundedLoss = farRightSlope < -tol;
    a.maxProfit = a.unboundedProfit ? INFINITY : best;
    a.maxLoss = a.unboundedLoss ? -INFINITY : worst;
    for (size_t i = 1; i < a.spots.size(); ++i) {
        const double f0 = a.pnlAtHorizon[i - 1], f1 = a.pnlAtHorizon[i];
        if ((f0 < 0.0 && f1 >= 0.0) || (f0 >= 0.0 && f1 < 0.0)) a.breakevens.push_back(a.spots[i - 1] + (a.spots[i] - a.spots[i - 1]) * (0.0 - f0) / (f1 - f0));
    }
    if (a.horizon > 1e-9 && market.volatility > 0.0) {
        const double v = market.volatility, T = a.horizon;
        const double q = market.model == Model::Black76 ? market.riskFreeRate : market.dividendYield;
        const double mu = (market.riskFreeRate - q - 0.5 * v * v) * T, sd = v * std::sqrt(T), S0 = market.spot;
        auto cdf = [&](double s) { return s <= 0.0 ? 0.0 : normalCdf((std::log(s / S0) - mu) / sd); };
        double probability = 0.0, expectation = 0.0;
        { const double p = cdf(a.spots.front()); if (a.pnlAtHorizon.front() > 0.0) probability += p; expectation += p * a.pnlAtHorizon.front(); }
        for (size_t i = 1; i < a.spots.size(); ++i) {
            const double p = cdf(a.spots[i]) - cdf(a.spots[i - 1]);
            const double midPnl = 0.5 * (a.pnlAtHorizon[i] + a.pnlAtHorizon[i - 1]);
            if (midPnl > 0.0) probability += p;
            expectation += p * midPnl;
        }
        { const double p = 1.0 - cdf(a.spots.back()); if (a.pnlAtHorizon.back() > 0.0) probability += p; expectation += p * a.pnlAtHorizon.back(); }
        a.probabilityOfProfit = std::clamp(probability, 0.0, 1.0);
        a.expectedPnl = expectation;
    }
    return a;
}

/// Analyses a built position; false when it is not a sensible trade (no risk, no reward, outside the caps).
/// `full` runs the model analysis (Greeks, today's curve); otherwise the quick intrinsic analysis when possible.
inline bool evaluate(Candidate& c, const Market& m, const View& view, const Options& o, double atmIv, bool full = false)
{
    c.market = m;
    c.analysis = (!full && singleExpiry(c.position)) ? quickAnalysis(c.position, m, std::min(o.gridPoints, 161)) : analyzePosition(c.position, m, o.gridPoints);
    const StrategyAnalysis& a = c.analysis;
    if (a.maxLoss >= 0.0) return false;   // cannot lose: stale marks, not an edge
    const double viewVol = view.volAnnual > 0.0 ? view.volAnnual : atmIv;
    const double target = view.targetSpot > 0.0 ? view.targetSpot : m.spot;
    c.view = evaluateView(c.position, m, a, target, viewVol);
    const double em = std::max(viewVol * std::sqrt(std::max(a.horizon, 1.0 / 365.0)), 0.02);
    const double upFar = positionPnl(c.position, m, m.spot * (1.0 + 2.0 * em), a.horizon);
    const double downFar = positionPnl(c.position, m, m.spot * std::max(0.05, 1.0 - 2.0 * em), a.horizon);
    const double profit = a.unboundedProfit ? std::max({ upFar, downFar, 0.0 }) : a.maxProfit;
    const double loss = a.unboundedLoss ? std::fabs(std::min({ upFar, downFar, 0.0 })) : std::fabs(a.maxLoss);
    if (profit <= 0.0 || loss <= 0.0) return false;
    if (o.maxLoss > 0.0 && loss > o.maxLoss) return false;
    c.loss = loss;
    c.returnOnRisk = profit / loss;
    if (c.returnOnRisk > 10.0 || c.returnOnRisk < o.minReturnOnRisk) return false;
    c.expectedReturnOnRisk = c.view.expectedPnl / loss;
    c.score = a.probabilityOfProfit * std::min(c.returnOnRisk, 3.0);
    c.atmIv = atmIv;
    c.legs = scan::detail::describeLegs(c.position);
    c.strategy = scan::presetName(c.preset);
    c.objective = objectiveValue(c, o.objective);
    return std::isfinite(c.objective);
}

inline Leg legOf(const scan::LadderPoint& p, LegKind kind, double qty) { return scan::detail::legFrom(p, kind, qty); }

/// Strikes where the call and put mids break put-call parity by more than `tolerance` of spot:
/// one of the two prints is stale, so neither leg can be trusted.
inline std::vector<double> parityBreaks(const std::vector<scan::LadderPoint>& calls, const std::vector<scan::LadderPoint>& puts, const ActivityMarket& market,
                                        double maturity, double tolerance = 0.0075)
{
    std::vector<double> bad;
    const double r = market.rateFor(maturity);
    const double q = market.model == Model::Black76 ? r : market.dividendYield;
    const double forward = market.spot * std::exp((r - q) * maturity);
    const double discount = std::exp(-r * maturity);
    for (const scan::LadderPoint& c : calls) {
        for (const scan::LadderPoint& p : puts) {
            if (std::fabs(p.quote->strike - c.quote->strike) > 1e-9) continue;
            const double gap = c.quote->mid - p.quote->mid - (forward - c.quote->strike) * discount;
            if (std::fabs(gap) > tolerance * market.spot) bad.push_back(c.quote->strike);
            break;
        }
    }
    return bad;
}

inline bool parityBroken(const std::vector<double>& bad, double strike)
{
    for (double k : bad) if (std::fabs(k - strike) < 1e-9) return true;
    return false;
}

/// Sorted, de-duplicated by legs, trimmed to maxResults, then fully analysed (Greeks, today's curve).
inline void finish(std::vector<Candidate>& out, const View& view, const Options& o)
{
    std::sort(out.begin(), out.end(), [](const Candidate& x, const Candidate& y) { return x.objective > y.objective; });
    std::vector<Candidate> unique;
    for (Candidate& c : out) {
        bool dup = false;
        for (const Candidate& u : unique) if (u.legs == c.legs && u.expiry.daysToExpiry == c.expiry.daysToExpiry) { dup = true; break; }
        if (!dup) unique.push_back(std::move(c));
        if (unique.size() >= o.maxResults) break;
    }
    std::vector<Candidate> finished;
    for (Candidate& c : unique) if (evaluate(c, c.market, view, o, c.atmIv, true)) finished.push_back(std::move(c));
    std::sort(finished.begin(), finished.end(), [](const Candidate& x, const Candidate& y) { return x.objective > y.objective; });
    out.swap(finished);
}

} // namespace detail

/// One candidate per preset (delta-targeted strikes as the scanner builds them) on the expiry nearest `targetDays`.
inline std::vector<Candidate> presetsOnChain(const std::vector<ChainQuote>& quotes, const ActivityMarket& market, const Market& base, const View& view,
                                             int targetDays, const Options& o)
{
    std::vector<Candidate> out;
    if (quotes.empty() || market.spot <= 0.0) return out;
    ChainIndex chain(quotes);
    const ListedExpiry* e = scan::expiryNear(chain, targetDays, 1, 10000);
    if (!e) return out;
    const scan::ChainMetrics metrics = scan::chainMetrics(quotes, market, e->key.daysToExpiry, 1, 10000);
    // Presets pick their strikes by delta: offer them only contracts whose call/put pair respects parity.
    const std::vector<scan::LadderPoint> allCalls = scan::ladder(chain, *e, OptionType::Call, market);
    const std::vector<scan::LadderPoint> allPuts = scan::ladder(chain, *e, OptionType::Put, market);
    const std::vector<double> bad = detail::parityBreaks(allCalls, allPuts, market, e->key.maturity);
    std::vector<scan::LadderPoint> calls, puts;
    for (const scan::LadderPoint& p : allCalls) if (!detail::parityBroken(bad, p.quote->strike)) calls.push_back(p);
    for (const scan::LadderPoint& p : allPuts) if (!detail::parityBroken(bad, p.quote->strike)) puts.push_back(p);
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
    scan::Criteria crit;
    crit.shortDelta = o.shortDelta;
    crit.wingDelta = o.wingDelta;
    const std::vector<StrategyPreset> presets = o.families.empty() ? scan::allPresets() : o.families;
    for (StrategyPreset preset : presets) {
        if (o.definedRiskOnly && !scan::definedRisk(preset)) continue;
        Candidate c;
        c.preset = preset;
        c.expiry = e->key;
        c.position.multiplier = o.multiplier;
        std::vector<scan::LadderPoint> used;
        if (!scan::detail::buildLegs(preset, chain, *e, far, market, crit, c.position.legs, used, calls, puts)) continue;
        bool consistent = true;
        for (const scan::LadderPoint& p : used) {
            c.worstSpread = std::max(c.worstSpread, p.spread);
            if (metrics.atmIv > 0.0 && (p.iv < 0.4 * metrics.atmIv || p.iv > 3.0 * metrics.atmIv)) consistent = false;
        }
        if (!consistent || (o.maxSpread > 0.0 && c.worstSpread > o.maxSpread)) continue;
        if (detail::evaluate(c, m, view, o, metrics.atmIv)) out.push_back(std::move(c));
    }
    detail::finish(out, view, o);
    return out;
}

/// Enumerates listed strikes (and expiries in the window) for every family and ranks by the objective.
inline std::vector<Candidate> optimize(const std::vector<ChainQuote>& quotes, const ActivityMarket& market, const Market& base, const View& view, const Options& o)
{
    std::vector<Candidate> out;
    if (quotes.empty() || market.spot <= 0.0) return out;
    ChainIndex chain(quotes);
    const double spot = market.spot;
    const scan::ChainMetrics metrics = scan::chainMetrics(quotes, market, (o.minDays + o.maxDays) / 2, std::min(7, o.minDays), std::max(o.maxDays, 120));
    const std::vector<StrategyPreset> presets = o.families.empty() ? scan::allPresets() : o.families;
    auto wants = [&](StrategyPreset p) { return std::find(presets.begin(), presets.end(), p) != presets.end() && !(o.definedRiskOnly && !scan::definedRisk(p)); };
    size_t evaluated = 0;
    // Expiries inside the window, thinned to maxExpiries evenly spread (weeklies would otherwise multiply the search).
    std::vector<const ListedExpiry*> expiries;
    for (const ListedExpiry* e : chain.expiries()) if (e->key.daysToExpiry >= o.minDays && e->key.daysToExpiry <= o.maxDays) expiries.push_back(e);
    if (o.maxExpiries > 0 && static_cast<int>(expiries.size()) > o.maxExpiries) {
        std::vector<const ListedExpiry*> thinned;
        for (int i = 0; i < o.maxExpiries; ++i) thinned.push_back(expiries[static_cast<size_t>(std::lround(i * (expiries.size() - 1.0) / (o.maxExpiries - 1.0)))]);
        expiries.swap(thinned);
    }

    for (const ListedExpiry* e : expiries) {
        if (evaluated >= o.maxEvaluations) break;
        // Strikes in the band with a sane mark: within the spread cap and with an implied vol in a
        // plausible band around the ATM vol (deep in-the-money prints that last traded days ago
        // otherwise show up as free spreads).
        auto usable = [&](const scan::LadderPoint& p) {
            if (p.quote->strike < spot * (1.0 - o.band) || p.quote->strike > spot * (1.0 + o.band)) return false;
            if (o.maxSpread > 0.0 && p.spread > o.maxSpread) return false;
            // Deep in-the-money contracts carry no time value and stale prints (a 95-delta put
            // spread "for 11% of its width"); far out-of-the-money ones are lottery tickets.
            if (std::fabs(p.delta) > o.maxAbsDelta || std::fabs(p.delta) < o.minAbsDelta) return false;
            return metrics.atmIv <= 0.0 || (p.iv >= 0.4 * metrics.atmIv && p.iv <= 3.0 * metrics.atmIv);
        };
        const std::vector<scan::LadderPoint> allCalls = scan::ladder(chain, *e, OptionType::Call, market);
        const std::vector<scan::LadderPoint> allPuts = scan::ladder(chain, *e, OptionType::Put, market);
        const std::vector<double> bad = detail::parityBreaks(allCalls, allPuts, market, e->key.maturity);
        std::vector<scan::LadderPoint> calls, puts;
        for (const scan::LadderPoint& p : allCalls) if (usable(p) && !detail::parityBroken(bad, p.quote->strike)) calls.push_back(p);
        for (const scan::LadderPoint& p : allPuts) if (usable(p) && !detail::parityBroken(bad, p.quote->strike)) puts.push_back(p);
        if (calls.empty() && puts.empty()) continue;
        // Far expiry for calendars: the first listed at least twice as far out, else the last later one.
        const ListedExpiry* far = nullptr;
        for (const ListedExpiry* x : chain.expiries()) {
            if (x->key.daysToExpiry <= e->key.daysToExpiry) continue;
            far = x;
            if (x->key.daysToExpiry >= 2 * e->key.daysToExpiry) break;
        }
        Market m = base;
        m.spot = spot;
        m.riskFreeRate = market.rateFor(e->key.maturity);
        m.dividendYield = market.dividendYield;
        m.volatility = metrics.atmIv > 0.0 ? metrics.atmIv : base.volatility;

        auto consider = [&](StrategyPreset preset, std::vector<Leg> legs, double worstSpread) {
            if (evaluated >= o.maxEvaluations) return;
            ++evaluated;
            Candidate c;
            c.preset = preset;
            c.expiry = e->key;
            c.position.multiplier = o.multiplier;
            c.position.legs = std::move(legs);
            c.worstSpread = worstSpread;
            if (detail::evaluate(c, m, view, o, metrics.atmIv)) out.push_back(std::move(c));
        };
        auto sp = [](const scan::LadderPoint& a, const scan::LadderPoint& b) { return std::max(a.spread, b.spread); };
        // Index of the put / call nearest the money.
        auto nearestIndex = [&](const std::vector<scan::LadderPoint>& l) {
            size_t best = 0;
            for (size_t i = 0; i < l.size(); ++i) if (std::fabs(l[i].quote->strike - spot) < std::fabs(l[best].quote->strike - spot)) best = i;
            return best;
        };
        auto callAt = [&](double strike) -> const scan::LadderPoint* { for (const scan::LadderPoint& p : calls) if (std::fabs(p.quote->strike - strike) < 1e-9) return &p; return nullptr; };

        // Single options.
        if (wants(StrategyPreset::LongCall)) for (const scan::LadderPoint& p : calls) consider(StrategyPreset::LongCall, { detail::legOf(p, LegKind::Call, 1) }, p.spread);
        if (wants(StrategyPreset::LongPut)) for (const scan::LadderPoint& p : puts) consider(StrategyPreset::LongPut, { detail::legOf(p, LegKind::Put, 1) }, p.spread);
        // Vertical spreads: every pair up to maxSpreadSteps strikes apart.
        for (size_t i = 0; i < calls.size(); ++i) {
            for (size_t j = i + 1; j < calls.size() && j <= i + static_cast<size_t>(o.maxSpreadSteps); ++j) {
                if (wants(StrategyPreset::BullCallSpread)) consider(StrategyPreset::BullCallSpread, { detail::legOf(calls[i], LegKind::Call, 1), detail::legOf(calls[j], LegKind::Call, -1) }, sp(calls[i], calls[j]));
                if (wants(StrategyPreset::BearCallSpread)) consider(StrategyPreset::BearCallSpread, { detail::legOf(calls[i], LegKind::Call, -1), detail::legOf(calls[j], LegKind::Call, 1) }, sp(calls[i], calls[j]));
            }
        }
        for (size_t i = 0; i < puts.size(); ++i) {
            for (size_t j = i + 1; j < puts.size() && j <= i + static_cast<size_t>(o.maxSpreadSteps); ++j) {
                if (wants(StrategyPreset::BearPutSpread)) consider(StrategyPreset::BearPutSpread, { detail::legOf(puts[j], LegKind::Put, 1), detail::legOf(puts[i], LegKind::Put, -1) }, sp(puts[i], puts[j]));
                if (wants(StrategyPreset::BullPutSpread)) consider(StrategyPreset::BullPutSpread, { detail::legOf(puts[j], LegKind::Put, -1), detail::legOf(puts[i], LegKind::Put, 1) }, sp(puts[i], puts[j]));
            }
        }
        // Straddles at every strike, strangles with the put below and the call above (limited width).
        for (const scan::LadderPoint& p : puts) {
            if (const scan::LadderPoint* c = callAt(p.quote->strike)) {
                if (wants(StrategyPreset::LongStraddle)) consider(StrategyPreset::LongStraddle, { detail::legOf(*c, LegKind::Call, 1), detail::legOf(p, LegKind::Put, 1) }, sp(*c, p));
                if (wants(StrategyPreset::ShortStraddle)) consider(StrategyPreset::ShortStraddle, { detail::legOf(*c, LegKind::Call, -1), detail::legOf(p, LegKind::Put, -1) }, sp(*c, p));
            }
            if (p.quote->strike > spot) continue;
            int width = 0;
            for (const scan::LadderPoint& c : calls) {
                if (c.quote->strike < spot || c.quote->strike <= p.quote->strike) continue;
                if (++width > o.maxSpreadSteps) break;
                if (wants(StrategyPreset::LongStrangle)) consider(StrategyPreset::LongStrangle, { detail::legOf(p, LegKind::Put, 1), detail::legOf(c, LegKind::Call, 1) }, sp(c, p));
                if (wants(StrategyPreset::ShortStrangle)) consider(StrategyPreset::ShortStrangle, { detail::legOf(p, LegKind::Put, -1), detail::legOf(c, LegKind::Call, -1) }, sp(c, p));
                if (wants(StrategyPreset::RiskReversal)) consider(StrategyPreset::RiskReversal, { detail::legOf(p, LegKind::Put, -1), detail::legOf(c, LegKind::Call, 1) }, sp(c, p));
            }
        }
        // Iron condors: short put at or below spot, short call at or above, wings 1..maxWingSteps strikes out.
        if (wants(StrategyPreset::IronCondor) && !puts.empty() && !calls.empty()) {
            for (size_t ip = 0; ip < puts.size(); ++ip) {
                if (puts[ip].quote->strike > spot) continue;
                for (size_t ic = 0; ic < calls.size(); ++ic) {
                    if (calls[ic].quote->strike < spot || calls[ic].quote->strike <= puts[ip].quote->strike) continue;
                    for (int w = 1; w <= o.maxWingSteps; ++w) {
                        if (ip < static_cast<size_t>(w) || ic + static_cast<size_t>(w) >= calls.size()) continue;
                        const scan::LadderPoint &wp = puts[ip - static_cast<size_t>(w)], &wc = calls[ic + static_cast<size_t>(w)];
                        consider(StrategyPreset::IronCondor, { detail::legOf(wp, LegKind::Put, 1), detail::legOf(puts[ip], LegKind::Put, -1), detail::legOf(calls[ic], LegKind::Call, -1), detail::legOf(wc, LegKind::Call, 1) },
                                 std::max(sp(wp, puts[ip]), sp(calls[ic], wc)));
                    }
                }
            }
        }
        // Iron butterflies and long call butterflies centred near the money.
        if ((wants(StrategyPreset::IronButterfly) || wants(StrategyPreset::LongCallButterfly)) && !puts.empty() && !calls.empty()) {
            const size_t pc = nearestIndex(puts), cc = nearestIndex(calls);
            for (int shift = -2; shift <= 2; ++shift) {
                const long ipl = static_cast<long>(pc) + shift, icl = static_cast<long>(cc) + shift;
                if (ipl < 0 || icl < 0 || ipl >= static_cast<long>(puts.size()) || icl >= static_cast<long>(calls.size())) continue;
                const size_t ip = static_cast<size_t>(ipl), ic = static_cast<size_t>(icl);
                if (std::fabs(puts[ip].quote->strike - calls[ic].quote->strike) > 1e-9) continue;
                for (int w = 1; w <= o.maxWingSteps; ++w) {
                    if (ip < static_cast<size_t>(w) || ic + static_cast<size_t>(w) >= calls.size() || ic < static_cast<size_t>(w)) continue;
                    if (wants(StrategyPreset::IronButterfly)) {
                        consider(StrategyPreset::IronButterfly, { detail::legOf(puts[ip - static_cast<size_t>(w)], LegKind::Put, 1), detail::legOf(puts[ip], LegKind::Put, -1),
                                                                   detail::legOf(calls[ic], LegKind::Call, -1), detail::legOf(calls[ic + static_cast<size_t>(w)], LegKind::Call, 1) },
                                 std::max(sp(puts[ip - static_cast<size_t>(w)], puts[ip]), sp(calls[ic], calls[ic + static_cast<size_t>(w)])));
                    }
                    if (wants(StrategyPreset::LongCallButterfly)) {
                        consider(StrategyPreset::LongCallButterfly, { detail::legOf(calls[ic - static_cast<size_t>(w)], LegKind::Call, 1), detail::legOf(calls[ic], LegKind::Call, -2), detail::legOf(calls[ic + static_cast<size_t>(w)], LegKind::Call, 1) },
                                 std::max(sp(calls[ic - static_cast<size_t>(w)], calls[ic]), sp(calls[ic], calls[ic + static_cast<size_t>(w)])));
                    }
                }
            }
        }
        // Calendars around the money.
        if ((wants(StrategyPreset::CallCalendar) || wants(StrategyPreset::PutCalendar)) && far) {
            const std::vector<scan::LadderPoint> farCalls = scan::ladder(chain, *far, OptionType::Call, market);
            const std::vector<scan::LadderPoint> farPuts = scan::ladder(chain, *far, OptionType::Put, market);
            for (const scan::LadderPoint& c : calls) {
                if (std::fabs(c.quote->strike / spot - 1.0) > 0.08) continue;
                if (wants(StrategyPreset::CallCalendar)) if (const scan::LadderPoint* f = scan::nearestStrike(farCalls, c.quote->strike); f && std::fabs(f->quote->strike - c.quote->strike) < 1e-9)
                    consider(StrategyPreset::CallCalendar, { detail::legOf(c, LegKind::Call, -1), detail::legOf(*f, LegKind::Call, 1) }, sp(c, *f));
            }
            for (const scan::LadderPoint& p : puts) {
                if (std::fabs(p.quote->strike / spot - 1.0) > 0.08) continue;
                if (wants(StrategyPreset::PutCalendar)) if (const scan::LadderPoint* f = scan::nearestStrike(farPuts, p.quote->strike); f && std::fabs(f->quote->strike - p.quote->strike) < 1e-9)
                    consider(StrategyPreset::PutCalendar, { detail::legOf(p, LegKind::Put, -1), detail::legOf(*f, LegKind::Put, 1) }, sp(p, *f));
            }
        }
        // Share-based structures (not defined risk): covered calls, protective puts, collars.
        if (wants(StrategyPreset::CoveredCall)) for (const scan::LadderPoint& c : calls) if (c.quote->strike >= spot) consider(StrategyPreset::CoveredCall, { scan::detail::stockLeg(spot, 1), detail::legOf(c, LegKind::Call, -1) }, c.spread);
        if (wants(StrategyPreset::ProtectivePut)) for (const scan::LadderPoint& p : puts) if (p.quote->strike <= spot) consider(StrategyPreset::ProtectivePut, { scan::detail::stockLeg(spot, 1), detail::legOf(p, LegKind::Put, 1) }, p.spread);
        if (wants(StrategyPreset::Collar)) {
            for (const scan::LadderPoint& p : puts) {
                if (p.quote->strike > spot) continue;
                for (const scan::LadderPoint& c : calls) if (c.quote->strike >= spot) consider(StrategyPreset::Collar, { scan::detail::stockLeg(spot, 1), detail::legOf(p, LegKind::Put, 1), detail::legOf(c, LegKind::Call, -1) }, sp(p, c));
            }
        }
    }
    detail::finish(out, view, o);
    return out;
}

} // namespace opt
} // namespace pricing
