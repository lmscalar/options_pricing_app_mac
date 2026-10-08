//
//  StrategyAndChainTests.mm
//  PricingTests
//
//  Position analytics, scenario grids, CSV parsing, implied-vol chains and SVI fitting.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Activity.h"
#include "../OptionPricing/Pricing/ChainStrategy.h"
#include "../OptionPricing/Pricing/Csv.h"
#include "../OptionPricing/Pricing/Scenario.h"
#include "../OptionPricing/Pricing/Strategy.h"
#include "../OptionPricing/Pricing/VolSurface.h"

using namespace pricing;

@interface StrategyAndChainTests : XCTestCase
@end

@implementation StrategyAndChainTests

- (void)testPresetsStartWithZeroMarkToModelPnl
{
    Market market;
    for (const PresetInfo& info : strategyPresets()) {
        const Position pos = buildPreset(info.id, market, 0.5, 5.0);
        XCTAssertFalse(pos.legs.empty(), @"%s has no legs", info.name);
        XCTAssertEqualWithAccuracy(positionPnl(pos, market, market.spot, 0.0), 0.0, 1e-8, @"%s", info.name);
    }
}

- (void)testIronCondorRiskProfile
{
    Market market;   // spot 100, r 5%, vol 20%
    const Position pos = buildPreset(StrategyPreset::IronCondor, market, 0.5, 5.0);
    const StrategyAnalysis a = analyzePosition(pos, market);
    XCTAssertLessThan(a.netPremium, 0.0);                       // credit
    XCTAssertFalse(a.unboundedProfit);
    XCTAssertFalse(a.unboundedLoss);
    XCTAssertEqualWithAccuracy(a.maxProfit, -a.netPremium, 1e-6);   // keep the whole credit
    XCTAssertEqualWithAccuracy(a.maxLoss, -(5.0 * pos.multiplier + a.netPremium), 1e-6);   // wing width minus credit
    XCTAssertEqual(a.breakevens.size(), 2u);
    XCTAssertEqualWithAccuracy(a.breakevens[0], 95.0 - (-a.netPremium) / pos.multiplier, 1e-3);
    XCTAssertEqualWithAccuracy(a.breakevens[1], 105.0 + (-a.netPremium) / pos.multiplier, 1e-3);
    XCTAssertGreaterThan(a.probabilityOfProfit, 0.2);
    XCTAssertLessThan(a.probabilityOfProfit, 0.8);
    XCTAssertEqualWithAccuracy(a.horizon, 0.5, 1e-12);
}

- (void)testLongCallAndCoveredCallBounds
{
    Market market;
    const Position longCall = buildPreset(StrategyPreset::LongCall, market, 1.0, 5.0);
    const StrategyAnalysis a = analyzePosition(longCall, market);
    XCTAssertTrue(a.unboundedProfit);
    XCTAssertFalse(a.unboundedLoss);
    XCTAssertEqualWithAccuracy(a.maxLoss, -a.netPremium, 1e-6);   // lose the premium
    XCTAssertEqual(a.breakevens.size(), 1u);
    XCTAssertEqualWithAccuracy(a.breakevens[0], 100.0 + longCall.legs[0].entryPrice, 1e-3);
    // Delta of a long ATM call is about 0.64 per share, times the multiplier.
    XCTAssertEqualWithAccuracy(a.greeks.delta, 63.68, 0.1);

    const Position covered = buildPreset(StrategyPreset::CoveredCall, market, 1.0, 5.0);
    const StrategyAnalysis c = analyzePosition(covered, market);
    XCTAssertFalse(c.unboundedProfit);   // capped by the short call
    XCTAssertFalse(c.unboundedLoss);     // stock cannot fall below zero
    XCTAssertEqualWithAccuracy(c.maxProfit, (105.0 - 100.0) * 100.0 + covered.legs[1].entryPrice * 100.0, 1e-6);
}

- (void)testShortStraddleHasUnboundedLoss
{
    Market market;
    const StrategyAnalysis a = analyzePosition(buildPreset(StrategyPreset::ShortStraddle, market, 0.5, 5.0), market);
    XCTAssertTrue(a.unboundedLoss);
    XCTAssertFalse(a.unboundedProfit);
    XCTAssertEqualWithAccuracy(a.maxProfit, -a.netPremium, 1e-6);
    XCTAssertEqual(a.breakevens.size(), 2u);
}

- (void)testCalendarSpreadValuesFarLegAtHorizon
{
    Market market;
    const Position cal = buildPreset(StrategyPreset::CallCalendar, market, 0.25, 5.0);
    const StrategyAnalysis a = analyzePosition(cal, market);
    XCTAssertEqualWithAccuracy(a.horizon, 0.25, 1e-12);
    // At the near expiry the far call still has time value, so the P&L at the
    // short strike is the max and positive.
    double best = -1e9;
    for (double v : a.pnlAtHorizon) best = std::max(best, v);
    XCTAssertGreaterThan(best, 0.0);
    XCTAssertEqualWithAccuracy(a.maxProfit, best, 1e-6);
}

- (void)testPositionGreeksAggregate
{
    Market market;
    Position pos;
    Leg stock;
    stock.kind = LegKind::Underlying;
    stock.quantity = 1;
    stock.entryPrice = 100;
    Leg call;
    call.kind = LegKind::Call;
    call.quantity = -1;
    call.strike = 100;
    call.maturity = 1.0;
    pos.legs = { stock, call };
    const Greeks g = positionGreeks(pos, market, 100.0, 0.0);
    const Result r = price(legInputs(call, market, 100.0, 0.0));
    XCTAssertEqualWithAccuracy(g.delta, 100.0 * (1.0 - r.call.delta), 1e-9);
    XCTAssertEqualWithAccuracy(g.gamma, -100.0 * r.call.gamma, 1e-9);
    XCTAssertEqualWithAccuracy(g.vega, -100.0 * r.call.vega, 1e-9);
    // After expiry the option contributes nothing.
    const Greeks later = positionGreeks(pos, market, 100.0, 1.5);
    XCTAssertEqualWithAccuracy(later.delta, 100.0, 1e-12);
    XCTAssertEqualWithAccuracy(later.gamma, 0.0, 1e-12);
}

- (void)testScenarioGridShapeAndMonotonicity
{
    Market market;
    const Position pos = buildPreset(StrategyPreset::LongCall, market, 1.0, 5.0);
    ScenarioRequest req;
    req.metric = ScenarioMetric::Pnl;
    req.axis = ScenarioAxis::Volatility;
    req.rows = 5;
    req.columns = 3;
    req.spotRangePercent = 10.0;
    req.volRangePoints = 5.0;
    const ScenarioGrid grid = buildScenarioGrid(pos, market, req);
    XCTAssertEqual(grid.spots.size(), 5u);
    XCTAssertEqual(grid.columnValues.size(), 3u);
    XCTAssertEqualWithAccuracy(grid.spots.front(), 110.0, 1e-9);   // highest first
    XCTAssertEqualWithAccuracy(grid.spots.back(), 90.0, 1e-9);
    XCTAssertEqualWithAccuracy(grid.cells[2][1], 0.0, 1e-6);        // centre cell: no change
    // Long call: higher spot and higher vol both help.
    XCTAssertGreaterThan(grid.cells[0][1], grid.cells[4][1]);
    XCTAssertGreaterThan(grid.cells[2][2], grid.cells[2][0]);
    XCTAssertLessThanOrEqual(grid.minValue, grid.maxValue);

    req.axis = ScenarioAxis::Time;
    req.maxDays = 0;   // to expiry
    const ScenarioGrid timeGrid = buildScenarioGrid(pos, market, req);
    XCTAssertEqualWithAccuracy(timeGrid.columnValues.back(), 365.0, 1e-9);
    // Theta decay: at the money the long call loses value as days pass.
    XCTAssertLessThan(timeGrid.cells[2][2], timeGrid.cells[2][0]);
}

- (void)testCsvParserHandlesQuotesAndLineEndings
{
    const auto rows = csv::parse("a,b,c\r\n1,\"x, y\",\"he said \"\"hi\"\"\"\n\n2,,3\n");
    XCTAssertEqual(rows.size(), 3u);
    XCTAssertEqual(rows[1][1], "x, y");
    XCTAssertEqual(rows[1][2], "he said \"hi\"");
    XCTAssertEqual(rows[2].size(), 3u);
    XCTAssertEqual(rows[2][1], "");
    XCTAssertEqual(csv::quote("plain"), "plain");
    XCTAssertEqual(csv::quote("a,b"), "\"a,b\"");
    XCTAssertEqual(csv::toDouble(" 12.5% ").value(), 0.125);
    XCTAssertEqual(csv::toDouble("$1,250.50").value(), 1250.5);
    XCTAssertFalse(csv::toDouble("abc").has_value());
}

- (void)testChainCsvParsing
{
    const std::string text =
        "Strike,Type,Bid,Ask,Days\n"
        "95,C,6.10,6.30,90\n"
        "100,call,3.00,3.20,90\n"
        "100,P,2.90,3.10,90\n"
        "oops,P,1,2,90\n"
        "105,X,1,2,90\n";
    const ChainParseResult parsed = parseChainCsv(text);
    XCTAssertTrue(parsed.error.empty());
    XCTAssertEqual(parsed.quotes.size(), 3u);
    XCTAssertEqual(parsed.warnings.size(), 2u);
    XCTAssertEqual(parsed.quotes[0].type, OptionType::Call);
    XCTAssertEqualWithAccuracy(parsed.quotes[0].mid, 6.20, 1e-12);
    XCTAssertEqualWithAccuracy(parsed.quotes[0].maturity, 90.0 / 365.0, 1e-12);
    XCTAssertEqual(parsed.quotes[2].type, OptionType::Put);

    // Missing expiry column falls back to the default maturity.
    const ChainParseResult fallback = parseChainCsv("strike,type,mid\n100,c,5\n", 0.5);
    XCTAssertEqual(fallback.quotes.size(), 1u);
    XCTAssertEqualWithAccuracy(fallback.quotes[0].maturity, 0.5, 1e-12);

    XCTAssertFalse(parseChainCsv("foo,bar\n1,2\n").error.empty());
}

- (void)testChainCsvWithExpirationDatesCarriesDateAndDte
{
    const CivilDate valuation{ 2026, 10, 7 };
    const std::string text =
        "strike,type,bid,ask,expiration\n"
        "100,call,5.0,5.2,2026-11-06\n"
        "100,put,4.0,4.2,2026-11-06\n"
        "100,call,8.0,8.3,2027-01-15\n";
    const ChainParseResult parsed = parseChainCsv(text, 0.0, &valuation);
    XCTAssertTrue(parsed.error.empty());
    XCTAssertEqual(parsed.quotes.size(), 3u);
    XCTAssertEqual(parsed.quotes[0].expiryDate, "2026-11-06");
    XCTAssertEqual(parsed.quotes[0].daysToExpiry, 30);
    XCTAssertEqualWithAccuracy(parsed.quotes[0].maturity, 30.0 / 365.0, 1e-12);
    XCTAssertEqual(parsed.quotes[2].daysToExpiry, 100);

    ChainMarket market;
    const std::vector<ExpirySlice> slices = buildExpirySlices(parsed.quotes, market);
    XCTAssertEqual(slices.size(), 2u);
    XCTAssertEqual(slices[0].expiryDate, "2026-11-06");
    XCTAssertEqual(slices[0].daysToExpiry, 30);
    XCTAssertEqual(slices[1].expiryDate, "2027-01-15");
    XCTAssertEqual(slices[1].daysToExpiry, 100);

    // Without a valuation date the date column is ignored and the row needs another expiry source.
    const ChainParseResult noValuation = parseChainCsv(text, 0.5);
    XCTAssertEqual(noValuation.quotes.size(), 3u);
    XCTAssertTrue(noValuation.quotes[0].expiryDate.empty());
    XCTAssertEqualWithAccuracy(noValuation.quotes[0].maturity, 0.5, 1e-12);

    CivilDate parsedDate;
    XCTAssertTrue(parseIsoDate("2028-02-29", parsedDate));
    XCTAssertFalse(parseIsoDate("2027-02-29", parsedDate));
    XCTAssertFalse(parseIsoDate("11/06/2026", parsedDate));
    XCTAssertEqual(formatIsoDate(CivilDate{ 2026, 1, 5 }), "2026-01-05");
}

- (void)testBatchCsvParsing
{
    Inputs defaults;
    defaults.spot = 100; defaults.riskFreeRate = 0.05; defaults.volatility = 0.2; defaults.maturity = 1.0;
    const std::string text =
        "label,type,strike,vol,expiry,exercise,model\n"
        "a,call,100,25,0.5,,\n"
        "b,put,90,0.3,1.0,american,\n"
        "c,call,110,,0.25,,futures\n"
        "bad,call,0,20,1,,\n";
    const BatchParseResult parsed = parseBatchCsv(text, defaults);
    XCTAssertTrue(parsed.error.empty());
    XCTAssertEqual(parsed.contracts.size(), 3u);
    XCTAssertEqual(parsed.warnings.size(), 1u);
    XCTAssertEqualWithAccuracy(parsed.contracts[0].inputs.volatility, 0.25, 1e-12);   // percent normalised
    XCTAssertEqualWithAccuracy(parsed.contracts[1].inputs.volatility, 0.30, 1e-12);
    XCTAssertTrue(parsed.contracts[1].american);
    XCTAssertEqualWithAccuracy(parsed.contracts[2].inputs.volatility, 0.20, 1e-12);   // default
    XCTAssertEqual(parsed.contracts[2].inputs.model, Model::Black76);
    XCTAssertEqualWithAccuracy(parsed.contracts[2].inputs.spot, 100.0, 1e-12);
}

- (void)testSyntheticChainRecoversVolatilitiesAndFitsSvi
{
    ChainMarket market;
    const std::vector<ChainQuote> chain = syntheticChain(market, { 0.25, 1.0 }, 0.2, -0.1, 0.3, 5.0, 8);
    XCTAssertEqual(chain.size(), 2u * 17u * 2u);
    const std::vector<ExpirySlice> slices = buildExpirySlices(chain, market);
    XCTAssertEqual(slices.size(), 2u);
    for (const ExpirySlice& s : slices) {
        XCTAssertTrue(s.fitted);
        XCTAssertLessThan(s.fitRmse, 0.003);               // under 0.3 vol points
        XCTAssertEqualWithAccuracy(s.atmVol(), 0.2, 0.01);
        XCTAssertTrue(s.issues.empty(), @"synthetic chain should be arbitrage-free, got %zu issues", s.issues.size());
        for (const ImpliedQuote& q : s.calls) {
            if (q.solved) {
                // Call and put at the same strike imply the same vol.
                for (const ImpliedQuote& p : s.puts) {
                    if (p.solved && std::fabs(p.quote.strike - q.quote.strike) < 1e-9) {
                        XCTAssertEqualWithAccuracy(p.impliedVol, q.impliedVol, 1e-6);
                    }
                }
            }
        }
        // Negative skew: lower strikes carry higher vol.
        XCTAssertGreaterThan(s.smileVol(s.forward * 0.9), s.smileVol(s.forward * 1.1));
    }

    VolSurface surface(slices);
    XCTAssertFalse(surface.empty());
    const double mid = surface.impliedVol(100.0, 0.5, market);
    XCTAssertGreaterThan(mid, 0.15);
    XCTAssertLessThan(mid, 0.25);
    // Flat extrapolation outside the fitted maturities.
    // Beyond the last fitted expiry the last slice's smile is held flat in vol, read at
    // the log-moneyness implied by the longer forward.
    const double farForward = market.spot * std::exp(market.riskFreeRate * 3.0);
    const double expected = sviVol(slices.back().svi, std::log(100.0 / farForward), slices.back().maturity);
    XCTAssertEqualWithAccuracy(surface.impliedVol(100.0, 3.0, market), expected, 1e-12);
}

- (void)testArbitrageChecksFlagBadQuotes
{
    ChainMarket market;
    std::vector<ChainQuote> chain = syntheticChain(market, { 0.5 }, 0.2, 0.0, 0.0, 5.0, 4);
    // Make one call price rise with strike.
    for (ChainQuote& q : chain) {
        if (q.type == OptionType::Call && std::fabs(q.strike - 105.0) < 1e-9) {
            q.mid += 5.0;
            q.bid = q.mid - 0.1;
            q.ask = q.mid + 0.1;
        }
    }
    const std::vector<ExpirySlice> slices = buildExpirySlices(chain, market);
    XCTAssertEqual(slices.size(), 1u);
    XCTAssertFalse(slices[0].issues.empty());
    bool mentionsRise = false, mentionsParity = false;
    for (const ChainIssue& issue : slices[0].issues) {
        if (issue.message.find("rises with strike") != std::string::npos) mentionsRise = true;
        if (issue.message.find("parity") != std::string::npos) mentionsParity = true;
    }
    XCTAssertTrue(mentionsRise);
    XCTAssertTrue(mentionsParity);
}

- (void)testActivityGridRankingAndParity
{
    ChainMarket cm;
    cm.spot = 100; cm.riskFreeRate = 0.05; cm.dividendYield = 0.0;
    std::vector<ChainQuote> chain = syntheticChain(cm, { 0.25, 0.5 }, 0.2, 0.0, 0.0, 5.0, 3);   // strikes 85..115, both expiries
    // Attach activity: the 0.5y 105 call is the busiest; 100-strike has the most open interest.
    for (ChainQuote& q : chain) {
        q.expiryDate = q.maturity < 0.3 ? "2027-01-07" : "2027-04-08";
        q.daysToExpiry = q.maturity < 0.3 ? 91 : 182;
        q.volume = 10.0;
        q.openInterest = 100.0;
        if (q.strike == 105.0 && q.type == OptionType::Call && q.maturity > 0.3) q.volume = 500.0;
        if (q.strike == 100.0) q.openInterest = 5000.0;
    }

    // The synthetic generator centres strikes on each expiry's forward: 85..115 for the
    // near expiry and 90..120 for the far one, so the union holds eight strikes.
    const ActivityGrid grid = buildActivityGrid(chain, ActivityMetric::Volume, SideFilter::Both, 100.0, 0.0);
    XCTAssertEqual(grid.strikes.size(), 8u);
    XCTAssertEqual(grid.expiries.size(), 2u);
    XCTAssertEqualWithAccuracy(grid.strikes.front(), 120.0, 1e-12);    // highest first
    XCTAssertEqual(grid.expiries[1].expiryDate, "2027-04-08");
    XCTAssertEqual(grid.expiries[1].daysToExpiry, 182);
    // Row for strike 105 (index 3), column for the far expiry: 500 (call) + 10 (put).
    XCTAssertEqualWithAccuracy(grid.strikes[3], 105.0, 1e-12);
    XCTAssertEqualWithAccuracy(grid.cells[3][1], 510.0, 1e-9);
    XCTAssertEqualWithAccuracy(grid.maxValue, 510.0, 1e-9);
    XCTAssertEqual(grid.contracts[3][1], 2);
    XCTAssertEqual(grid.contracts[0][0], 0);                            // 120 is not listed at the near expiry
    // A 10% band around spot drops the 85 and 115 strikes.
    const ActivityGrid banded = buildActivityGrid(chain, ActivityMetric::OpenInterest, SideFilter::Calls, 100.0, 0.10);
    XCTAssertEqual(banded.strikes.size(), 5u);
    XCTAssertEqualWithAccuracy(banded.cells[2][0], 5000.0, 1e-9);     // strike 100, calls only

    ActivityMarket market;
    market.spot = 100.0;
    market.rateFor = [](double) { return 0.05; };
    const auto top = mostActiveContracts(chain, ActivityMetric::Volume, SideFilter::Both, market, 3);
    XCTAssertEqual(top.size(), 3u);
    XCTAssertEqualWithAccuracy(top[0].quote.strike, 105.0, 1e-12);
    XCTAssertEqual(top[0].quote.type, OptionType::Call);
    XCTAssertEqualWithAccuracy(top[0].activity, 500.0, 1e-12);
    XCTAssertEqualWithAccuracy(top[0].impliedVol, 0.20, 1e-4);         // synthetic mids are exact model prices
    XCTAssertEqualWithAccuracy(top[0].parityGap, 0.0, 1e-9);           // model prices satisfy parity exactly
    XCTAssertGreaterThan(top[0].shareOfTotal, 0.5);

    const auto parity = parityByExpiry(chain, market);
    XCTAssertEqual(parity.size(), 2u);
    XCTAssertEqual(parity[0].pairs, 7);
    XCTAssertEqualWithAccuracy(parity[0].impliedForward, 100.0 * std::exp(0.05 * 0.25), 1e-6);
    XCTAssertEqualWithAccuracy(parity[0].forwardGap, 0.0, 1e-6);
    XCTAssertEqualWithAccuracy(parity[0].impliedYield, 0.0, 1e-6);
    XCTAssertEqualWithAccuracy(parity[0].meanAbsGap, 0.0, 1e-9);
    XCTAssertEqualWithAccuracy(parity[1].callVolume, 560.0, 1e-9);     // 6 x 10 + 500

    // Shift every put up by 0.50: parity gap becomes -0.50 and the implied forward drops by 0.50 e^{rT}.
    for (ChainQuote& q : chain) if (q.type == OptionType::Put) q.mid += 0.5;
    const auto skewed = parityByExpiry(chain, market);
    XCTAssertEqualWithAccuracy(skewed[0].meanAbsGap, 0.5, 1e-9);
    XCTAssertEqualWithAccuracy(skewed[0].forwardGap, -0.5 * std::exp(0.05 * 0.25), 1e-6);
    XCTAssertGreaterThan(skewed[0].impliedYield, 0.0);

    const ActivitySummary summary = summarizeActivity(chain, ActivityMetric::Volume, SideFilter::Both);
    XCTAssertEqual(summary.busiestExpiry.expiryDate, "2027-04-08");
    XCTAssertEqualWithAccuracy(summary.busiestStrike, 105.0, 1e-12);
    XCTAssertEqualWithAccuracy(summary.putCallVolumeRatio, 140.0 / 630.0, 1e-9);
    XCTAssertEqual(summary.contracts, 28);
}

- (void)testChainIndexAndChainDrivenPresets
{
    ChainMarket cm;
    cm.spot = 100; cm.riskFreeRate = 0.05; cm.dividendYield = 0.0;
    std::vector<ChainQuote> chain = syntheticChain(cm, { 30.0 / 365.0, 91.0 / 365.0 }, 0.25, 0.0, 0.0, 5.0, 4);
    for (ChainQuote& q : chain) {
        q.expiryDate = q.maturity < 0.2 ? "2026-11-07" : "2027-01-07";
        q.daysToExpiry = q.maturity < 0.2 ? 30 : 91;
    }
    const ChainIndex index(chain);
    XCTAssertFalse(index.empty());
    XCTAssertEqual(index.expiries().size(), 2u);
    XCTAssertEqual(index.expiries()[0]->key.expiryDate, "2026-11-07");
    XCTAssertEqual(index.expiries()[0]->strikes.size(), 9u);          // 80..120 by 5
    XCTAssertEqualWithAccuracy(index.nearestStrike(30.0 / 365.0, 101.9), 100.0, 1e-12);
    XCTAssertEqualWithAccuracy(index.strikeOffset(30.0 / 365.0, 100.0, 2), 110.0, 1e-12);
    XCTAssertEqualWithAccuracy(index.strikeOffset(30.0 / 365.0, 120.0, 3), 120.0, 1e-12);   // clamped at the top
    XCTAssertEqualWithAccuracy(index.strikeOffset(30.0 / 365.0, 97.0, 1), 97.0, 1e-12);     // unlisted stays put
    XCTAssertTrue(index.quote(30.0 / 365.0, 100.0, OptionType::Call) != nullptr);
    XCTAssertTrue(index.quote(30.0 / 365.0, 97.0, OptionType::Call) == nullptr);
    XCTAssertEqual(index.firstExpiryAtLeast(25)->key.daysToExpiry, 30);
    XCTAssertEqual(index.firstExpiryAtLeast(60)->key.daysToExpiry, 91);
    XCTAssertEqual(index.firstExpiryAtLeast(500)->key.daysToExpiry, 91);          // falls back to the last

    ActivityMarket market;
    market.spot = 100.0;
    market.rateFor = [](double) { return 0.05; };

    Leg leg;
    leg.kind = LegKind::Put;
    leg.strike = 95.0;
    leg.maturity = 30.0 / 365.0;
    XCTAssertTrue(markLegToChain(leg, index, market));
    XCTAssertEqual(leg.expiryDate, "2026-11-07");
    XCTAssertGreaterThan(leg.marketPrice, 0.0);
    XCTAssertEqualWithAccuracy(leg.volatility, 0.25, 1e-4);          // synthetic mids are exact model prices
    leg.strike = 97.0;
    XCTAssertFalse(markLegToChain(leg, index, market));
    XCTAssertEqualWithAccuracy(leg.marketPrice, 0.0, 0.0);

    const Position condor = buildPresetFromChain(StrategyPreset::IronCondor, index, market, 30.0 / 365.0, 100.0);
    XCTAssertEqual(condor.legs.size(), 4u);
    XCTAssertEqualWithAccuracy(condor.legs[0].strike, 90.0, 1e-12);   // two listed strikes below ATM
    XCTAssertEqualWithAccuracy(condor.legs[1].strike, 95.0, 1e-12);
    XCTAssertEqualWithAccuracy(condor.legs[2].strike, 105.0, 1e-12);
    XCTAssertEqualWithAccuracy(condor.legs[3].strike, 110.0, 1e-12);
    for (const Leg& l : condor.legs) {
        XCTAssertEqual(l.expiryDate, "2026-11-07");
        XCTAssertEqualWithAccuracy(l.entryPrice, l.marketPrice, 1e-12);   // entered at the chain mid
        XCTAssertGreaterThan(l.marketPrice, 0.0);
    }
    // Entered at chain mids that equal model prices, so the position starts flat.
    Market m;
    m.spot = 100; m.riskFreeRate = 0.05; m.volatility = 0.25;
    XCTAssertEqualWithAccuracy(positionPnl(condor, m, 100.0, 0.0), 0.0, 1e-6);

    const Position calendar = buildPresetFromChain(StrategyPreset::CallCalendar, index, market, 30.0 / 365.0, 100.0);
    XCTAssertEqual(calendar.legs.size(), 2u);
    XCTAssertEqual(calendar.legs[0].expiryDate, "2026-11-07");
    XCTAssertEqual(calendar.legs[1].expiryDate, "2027-01-07");          // next listed expiry at least twice as far
    XCTAssertEqualWithAccuracy(calendar.legs[1].maturity, 91.0 / 365.0, 1e-12);

    const Position covered = buildPresetFromChain(StrategyPreset::CoveredCall, index, market, 30.0 / 365.0, 100.0);
    XCTAssertEqual(covered.legs[0].kind, LegKind::Underlying);
    XCTAssertEqualWithAccuracy(covered.legs[0].entryPrice, 100.0, 1e-12);
    XCTAssertEqualWithAccuracy(covered.legs[1].strike, 105.0, 1e-12);
}

- (void)testSviFitReproducesKnownParameters
{
    const SviParams truth{ 0.02, 0.4, -0.4, 0.05, 0.2 };
    const double T = 1.0;
    std::vector<std::pair<double, double>> points;
    for (double k = -0.5; k <= 0.5; k += 0.05) {
        points.emplace_back(k, sviVol(truth, k, T));
    }
    SviParams fitted;
    double rmse = 1.0;
    XCTAssertTrue(fitSvi(points, T, fitted, rmse));
    XCTAssertLessThan(rmse, 5e-4);
    for (double k = -0.4; k <= 0.4; k += 0.1) {
        XCTAssertEqualWithAccuracy(sviVol(fitted, k, T), sviVol(truth, k, T), 1e-3);
    }
    XCTAssertFalse(fitSvi({ { 0.0, 0.2 }, { 0.1, 0.2 } }, T, fitted, rmse));   // too few points
}

@end
