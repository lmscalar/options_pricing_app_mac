//
//  OptimizerTests.mm
//  PricingTests
//
//  Strategy optimiser: view metrics respond to the target, the optimiser favours bullish
//  structures for a bullish view and range structures for a neutral one, filters and caps
//  are honoured, and the preset comparison yields one candidate per preset.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Optimizer.h"
#include "../OptionPricing/Pricing/VolSurface.h"

#include <cmath>

using namespace pricing;

namespace {

std::vector<ChainQuote> sampleChain(double spot)
{
    ChainMarket cm;
    cm.spot = spot;
    cm.riskFreeRate = 0.04;
    std::vector<ChainQuote> quotes = syntheticChain(cm, { 30.0 / 365.0, 58.0 / 365.0, 120.0 / 365.0 }, 0.30, -0.3, 0.4, 5.0, 12, 0.04);
    for (ChainQuote& q : quotes) {
        q.daysToExpiry = static_cast<int>(std::lround(q.maturity * 365.0));
        q.expiryDate = "2026-12-" + std::string(q.daysToExpiry < 10 ? "0" : "") + std::to_string(std::min(28, q.daysToExpiry));
        q.openInterest = 500;
    }
    return quotes;
}

ActivityMarket marketAt(double spot)
{
    ActivityMarket am;
    am.spot = spot;
    am.rateFor = [](double) { return 0.04; };
    return am;
}

Market baseMarket()
{
    Market m;
    m.riskFreeRate = 0.04;
    m.volatility = 0.30;
    return m;
}

} // namespace

@interface OptimizerTests : XCTestCase
@end

@implementation OptimizerTests

- (void)testViewMetricsFollowTheTarget
{
    Market m = baseMarket();
    m.spot = 200.0;
    Position call;
    call.legs = { Leg{ LegKind::Call, 1.0, 200.0, 45.0 / 365.0, 0.30, 7.5, 7.5 } };
    const StrategyAnalysis a = analyzePosition(call, m, 401);
    const opt::ViewMetrics bull = opt::evaluateView(call, m, a, 230.0, 0.30);
    const opt::ViewMetrics bear = opt::evaluateView(call, m, a, 175.0, 0.30);
    XCTAssertGreaterThan(bull.expectedPnl, 0.0);
    XCTAssertLessThan(bear.expectedPnl, 0.0);
    XCTAssertGreaterThan(bull.probabilityOfProfit, bear.probabilityOfProfit);
    XCTAssertEqualWithAccuracy(bull.pnlAtTarget, (30.0 - 7.5) * 100.0, 1e-6, @"intrinsic minus premium at the target");
    XCTAssertLessThan(bear.pnlAtTarget, 0.0);
}

- (void)testBullishViewPrefersBullishStructures
{
    const auto quotes = sampleChain(200.0);
    opt::View view;
    view.targetSpot = 225.0;
    opt::Options o;
    o.minDays = 20; o.maxDays = 70;
    o.objective = opt::Objective::ExpectedPnl;
    o.maxResults = 10;
    const auto best = opt::optimize(quotes, marketAt(200.0), baseMarket(), view, o);
    XCTAssertFalse(best.empty());
    XCTAssertEqual(scan::biasOf(best.front().preset), scan::Bias::Bullish, @"top candidate for a +12% target should be a bullish structure");
    for (size_t i = 1; i < best.size(); ++i) XCTAssertGreaterThanOrEqual(best[i - 1].objective, best[i].objective, @"sorted by objective");
    for (const opt::Candidate& c : best) {
        XCTAssertTrue(scan::definedRisk(c.preset), @"defined risk only by default");
        XCTAssertLessThan(c.analysis.maxLoss, 0.0);
        XCTAssertFalse(c.legs.empty());
    }
}

- (void)testNeutralViewWithProbabilityObjectiveFindsRangeTrades
{
    const auto quotes = sampleChain(200.0);
    opt::View view;
    view.targetSpot = 200.0;
    view.volAnnual = 0.18;   // calmer than implied
    opt::Options o;
    o.minDays = 20; o.maxDays = 70;
    o.objective = opt::Objective::ProbabilityOfProfit;
    o.families = { StrategyPreset::IronCondor, StrategyPreset::LongStraddle, StrategyPreset::LongStrangle };
    o.maxResults = 5;
    const auto best = opt::optimize(quotes, marketAt(200.0), baseMarket(), view, o);
    XCTAssertFalse(best.empty());
    // A calm, neutral view: the range trade beats the long-volatility structures on probability.
    XCTAssertEqual(best.front().preset, StrategyPreset::IronCondor);
    XCTAssertGreaterThan(best.front().view.probabilityOfProfit, 0.6);
    XCTAssertGreaterThanOrEqual(best.front().returnOnRisk, 0.05, @"trades below the minimum return on risk are dropped");
    for (const opt::Candidate& c : best) if (c.preset == StrategyPreset::IronCondor) XCTAssertLessThan(c.analysis.netPremium, 0.0, @"a condor is a credit");
}

- (void)testMaxLossCapAndFamilyFilterAreHonoured
{
    const auto quotes = sampleChain(200.0);
    opt::View view;
    view.targetSpot = 210.0;
    opt::Options o;
    o.minDays = 20; o.maxDays = 70;
    o.families = { StrategyPreset::BullCallSpread };
    o.maxLoss = 400.0;
    o.maxResults = 50;
    const auto best = opt::optimize(quotes, marketAt(200.0), baseMarket(), view, o);
    XCTAssertFalse(best.empty());
    for (const opt::Candidate& c : best) {
        XCTAssertEqual(c.preset, StrategyPreset::BullCallSpread);
        XCTAssertLessThanOrEqual(std::fabs(c.analysis.maxLoss), 400.0 + 1e-6);
        XCTAssertEqual(c.position.legs.size(), 2u);
    }
}

- (void)testPresetComparisonBuildsOnePerPreset
{
    const auto quotes = sampleChain(200.0);
    opt::View view;
    opt::Options o;
    o.definedRiskOnly = false;
    o.maxResults = 50;
    const auto all = opt::presetsOnChain(quotes, marketAt(200.0), baseMarket(), view, 58, o);
    XCTAssertGreaterThanOrEqual(all.size(), 15u, @"most of the 19 presets build on a full ladder");
    for (size_t i = 0; i < all.size(); ++i) {
        XCTAssertEqual(all[i].expiry.daysToExpiry, 58);
        for (size_t j = i + 1; j < all.size(); ++j) XCTAssertNotEqual(all[i].preset, all[j].preset, @"each preset once");
    }
}

@end
