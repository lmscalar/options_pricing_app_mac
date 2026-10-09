//
//  RiskTests.mm
//  PricingTests
//
//  Portfolio valuation and risk: parametric, historical and Monte Carlo VaR / expected
//  shortfall against closed forms, stress grid behaviour, component VaR allocation.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Risk.h"

#include <cmath>
#include <random>

using namespace pricing;

namespace {

Markets flatMarkets(double spot = 100.0, double vol = 0.25)
{
    Market m;
    m.spot = spot;
    m.riskFreeRate = 0.04;
    m.dividendYield = 0.0;
    m.volatility = vol;
    return { { "ABC", m } };
}

Holding shares(const std::string& symbol, double qty, double entry)
{
    Holding h;
    h.symbol = symbol;
    h.kind = LegKind::Underlying;
    h.quantity = qty;
    h.entryPrice = entry;
    h.multiplier = 1.0;
    return h;
}

Holding option(const std::string& symbol, LegKind kind, double qty, double strike, double maturity, double vol, double entry)
{
    Holding h;
    h.symbol = symbol;
    h.kind = kind;
    h.quantity = qty;
    h.strike = strike;
    h.maturity = maturity;
    h.volatility = vol;
    h.entryPrice = entry;
    h.multiplier = 100.0;
    return h;
}

/// Gaussian daily returns with a known standard deviation (seeded).
risk::ReturnHistory gaussianHistory(const std::vector<std::string>& symbols, double dailySigma, size_t days, unsigned seed)
{
    risk::ReturnHistory h;
    h.symbols = symbols;
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, dailySigma);
    for (size_t t = 0; t < days; ++t) {
        std::vector<double> row;
        for (size_t i = 0; i < symbols.size(); ++i) row.push_back(z(rng));
        h.returns.push_back(row);
    }
    return h;
}

} // namespace

@interface RiskTests : XCTestCase
@end

@implementation RiskTests

- (void)testExposureOfSharesAndOptions
{
    const Markets markets = flatMarkets(100.0, 0.25);
    Portfolio p;
    p.holdings.push_back(shares("ABC", 100, 90.0));
    p.holdings.push_back(option("ABC", LegKind::Call, 2, 100.0, 0.5, 0.25, 7.0));
    const BookExposure book = bookExposure(p, markets, false);
    XCTAssertEqual(book.total.holdings, 2);
    XCTAssertEqualWithAccuracy(book.byUnderlying.at("ABC").marketValue - book.total.marketValue, 0.0, 1e-9);
    // Shares: value 10,000, P&L 1,000, delta 100 shares.
    const Exposure sharesOnly = exposure(p.holdings[0], markets.at("ABC"), false);
    XCTAssertEqualWithAccuracy(sharesOnly.marketValue, 10000.0, 1e-9);
    XCTAssertEqualWithAccuracy(sharesOnly.pnl, 1000.0, 1e-9);
    XCTAssertEqualWithAccuracy(sharesOnly.delta, 100.0, 1e-9);
    XCTAssertEqualWithAccuracy(sharesOnly.deltaDollars, 10000.0, 1e-9);
    XCTAssertEqual(sharesOnly.gammaDollars, 0.0);
    // ATM call: delta about 0.55-0.6 per contract, positive gamma, vega and negative theta.
    const Exposure call = exposure(p.holdings[1], markets.at("ABC"), false);
    XCTAssertGreaterThan(call.delta, 100.0);
    XCTAssertLessThan(call.delta, 130.0);
    XCTAssertGreaterThan(call.gammaDollars, 0.0);
    XCTAssertGreaterThan(call.vega, 0.0);
    XCTAssertLessThan(call.theta, 0.0);
    // An observed mark overrides the model value at the current spot only.
    Holding marked = p.holdings[1];
    marked.markPrice = 9.0;
    XCTAssertEqualWithAccuracy(holdingValue(marked, markets.at("ABC"), 100.0, 1.0, 0.0, true), 1800.0, 1e-9);
    XCTAssertNotEqualWithAccuracy(holdingValue(marked, markets.at("ABC"), 101.0, 1.0, 0.0, true), 1800.0, 1e-6);
}

- (void)testParametricVarOfSharesMatchesNormalClosedForm
{
    const Markets markets = flatMarkets(100.0, 0.25);
    Portfolio p;
    p.holdings.push_back(shares("ABC", 100, 100.0));
    const double dailySigma = 0.01;
    const auto history = gaussianHistory({ "ABC" }, dailySigma, 4000, 7);
    const risk::VarResult r = risk::parametricVar(p, markets, history, 0.95, 1);
    XCTAssertTrue(r.ok);
    // Sample sigma differs slightly from 1%; compare against the realised covariance.
    const double sigma = std::sqrt(risk::covariance(history)[0][0]);
    const double z = normalInverseCdf(0.95);
    XCTAssertEqualWithAccuracy(r.var, z * sigma * 10000.0, 1e-6);
    XCTAssertEqualWithAccuracy(r.cvar, normalPdf(z) / 0.05 * sigma * 10000.0, 1e-6);
    XCTAssertGreaterThan(r.cvar, r.var);
    // Ten-day VaR scales with the square root of time.
    const risk::VarResult r10 = risk::parametricVar(p, markets, history, 0.95, 10);
    XCTAssertEqualWithAccuracy(r10.var / r.var, std::sqrt(10.0), 1e-6);
    // Component VaR of a single underlying is the whole VaR.
    XCTAssertEqualWithAccuracy(r.componentVar.at("ABC"), r.var, 1e-6);
}

- (void)testComponentVarSumsToTotalAcrossUnderlyings
{
    Markets markets = flatMarkets(100.0, 0.25);
    markets["XYZ"] = markets["ABC"];
    markets["XYZ"].spot = 50.0;
    Portfolio p;
    p.holdings.push_back(shares("ABC", 100, 100.0));
    p.holdings.push_back(shares("XYZ", -50, 50.0));
    const auto history = gaussianHistory({ "ABC", "XYZ" }, 0.012, 2000, 11);
    const risk::VarResult r = risk::parametricVar(p, markets, history, 0.99, 1);
    double sum = 0.0;
    for (const auto& [sym, c] : r.componentVar) sum += c;
    XCTAssertEqualWithAccuracy(sum, r.var, 1e-6);
}

- (void)testHistoricalVarQuantileAndShortfall
{
    const Markets markets = flatMarkets(100.0, 0.25);
    Portfolio p;
    p.holdings.push_back(shares("ABC", 100, 100.0));
    // Deterministic returns: -5%, -4%, ..., +4%, +5% repeated.
    risk::ReturnHistory h;
    h.symbols = { "ABC" };
    for (int rep = 0; rep < 10; ++rep) for (int k = -5; k <= 5; ++k) h.returns.push_back({ k / 100.0 });
    const risk::VarResult r = risk::historicalVar(p, markets, h, 0.95, 1);
    XCTAssertTrue(r.ok, @"%s", r.note.c_str());
    XCTAssertEqual(r.pnl.size(), h.days());
    // Worst 5% of 110 days: the -5% days (10 of them, 9.1%) span the 5% quantile, so VaR = $500.
    XCTAssertEqualWithAccuracy(r.var, 500.0, 1e-6);
    XCTAssertEqualWithAccuracy(r.cvar, 500.0, 1e-6);
    XCTAssertGreaterThanOrEqual(r.cvar, r.var);
    XCTAssertEqualWithAccuracy(r.meanPnl, 0.0, 1e-9);
    const risk::VarResult tooShort = risk::historicalVar(p, markets, gaussianHistory({ "ABC" }, 0.01, 15, 3), 0.95, 1);
    XCTAssertFalse(tooShort.ok);
}

- (void)testMonteCarloAgreesWithParametricForLinearBook
{
    const Markets markets = flatMarkets(100.0, 0.25);
    Portfolio p;
    p.holdings.push_back(shares("ABC", 100, 100.0));
    const auto history = gaussianHistory({ "ABC" }, 0.01, 1000, 5);
    const risk::VarResult param = risk::parametricVar(p, markets, history, 0.95, 1);
    const risk::VarResult mc = risk::monteCarloVar(p, markets, history, 0.95, 1, 40000, 42);
    XCTAssertTrue(mc.ok);
    XCTAssertEqualWithAccuracy(mc.var, param.var, param.var * 0.05);
    XCTAssertEqualWithAccuracy(mc.cvar, param.cvar, param.cvar * 0.05);
    XCTAssertGreaterThan(mc.cvar, mc.var);
    XCTAssertEqual(mc.pnl.size(), 40000u);
}

- (void)testMonteCarloUsesImpliedVolWithoutHistory
{
    const Markets markets = flatMarkets(100.0, 0.32);
    Portfolio p;
    p.holdings.push_back(shares("ABC", 100, 100.0));
    const risk::VarResult r = risk::monteCarloVar(p, markets, {}, 0.95, 1, 30000, 9);
    XCTAssertTrue(r.ok);
    const double sigma = 0.32 / std::sqrt(252.0);
    XCTAssertEqualWithAccuracy(r.var, normalInverseCdf(0.95) * sigma * 10000.0, 0.06 * normalInverseCdf(0.95) * sigma * 10000.0);
    XCTAssertFalse(r.note.empty());
}

- (void)testOptionsMakeTheTailAsymmetric
{
    const Markets markets = flatMarkets(100.0, 0.25);
    Portfolio longPut, shortPut;
    longPut.holdings.push_back(option("ABC", LegKind::Put, 10, 95.0, 0.25, 0.25, 2.0));
    shortPut.holdings.push_back(option("ABC", LegKind::Put, -10, 95.0, 0.25, 0.25, 2.0));
    const auto history = gaussianHistory({ "ABC" }, 0.015, 1500, 21);
    const risk::VarResult lp = risk::monteCarloVar(longPut, markets, history, 0.99, 1, 20000, 3);
    const risk::VarResult sp = risk::monteCarloVar(shortPut, markets, history, 0.99, 1, 20000, 3);
    // Selling puts carries the fatter loss tail: VaR and CVaR are larger than for owning them.
    XCTAssertGreaterThan(sp.var, lp.var);
    XCTAssertGreaterThan(sp.cvar, lp.cvar);
    // A long option can never lose more than its premium.
    const double premium = 10 * 100 * holdingUnitValue(longPut.holdings[0], markets.at("ABC"), 100.0, 1.0, 0.0);
    XCTAssertLessThanOrEqual(lp.cvar, premium + 1e-6);
}

- (void)testStressGridAndDecayLadder
{
    const Markets markets = flatMarkets(100.0, 0.25);
    Portfolio p;
    p.holdings.push_back(option("ABC", LegKind::Call, 5, 100.0, 0.5, 0.25, 7.0));
    const risk::StressGrid g = risk::stressGrid(p, markets);
    XCTAssertEqual(g.pnl.size(), g.spotShocks.size());
    XCTAssertEqual(g.pnl.front().size(), g.volShocks.size());
    // Long call: P&L rises with spot and with vol; zero at the unshocked cell.
    const size_t midVol = 2;   // volShocks = {-50%, -25%, 0, +25%, +50%}
    for (size_t i = 1; i < g.spotShocks.size(); ++i) XCTAssertGreaterThan(g.pnl[i][midVol], g.pnl[i - 1][midVol]);
    const size_t midSpot = 3;  // spotShocks middle = 0
    for (size_t j = 1; j < g.volShocks.size(); ++j) XCTAssertGreaterThan(g.pnl[midSpot][j], g.pnl[midSpot][j - 1]);
    XCTAssertEqualWithAccuracy(g.pnl[midSpot][midVol], 0.0, 1e-9);
    const auto ladder = risk::decayLadder(p, markets);
    double previous = 0.0;
    for (const auto& [days, pnl] : ladder) { XCTAssertLessThan(pnl, previous + 1e-9, @"theta bleeds over %d days", days); previous = pnl; }
}

- (void)testMaturityFromDates
{
    XCTAssertEqualWithAccuracy(maturityFromDates("2026-01-01", "2027-01-01"), 1.0, 1e-9);
    XCTAssertEqual(maturityFromDates("2026-06-01", "2026-05-01"), 0.0);
    XCTAssertEqual(maturityFromDates("bad", "2026-05-01"), 0.0);
}

@end
