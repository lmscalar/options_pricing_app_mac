//
//  ScannerTests.mm
//  PricingTests
//
//  Trade-idea scanner: chain metrics recover the smile they were built from, delta
//  targeting lands out of the money, credit structures come out as credits with a sensible
//  probability of profit, and the filters are honoured.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Scanner.h"
#include "../OptionPricing/Pricing/VolSurface.h"

#include <cmath>

using namespace pricing;

namespace {

/// Synthetic chain with expiry metadata as the live client supplies it.
std::vector<ChainQuote> sampleChain(double spot, double atmVol, double skew)
{
    ChainMarket cm;
    cm.spot = spot;
    cm.riskFreeRate = 0.04;
    std::vector<ChainQuote> quotes = syntheticChain(cm, { 20.0 / 365.0, 45.0 / 365.0, 90.0 / 365.0 }, atmVol, skew, 0.5, 5.0, 14, 0.04);
    for (ChainQuote& q : quotes) {
        q.daysToExpiry = static_cast<int>(std::lround(q.maturity * 365.0));
        q.expiryDate = "2026-11-" + std::string(q.daysToExpiry < 10 ? "0" : "") + std::to_string(std::min(28, q.daysToExpiry));
        q.openInterest = 500;
        q.volume = 100;
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
    m.volatility = 0.25;
    return m;
}

} // namespace

@interface ScannerTests : XCTestCase
@end

@implementation ScannerTests

- (void)testChainMetricsRecoverAtmVolAndPutSkew
{
    const auto quotes = sampleChain(200.0, 0.30, -0.40);
    const scan::ChainMetrics m = scan::chainMetrics(quotes, marketAt(200.0), 45, 7, 120);
    XCTAssertTrue(m.ok);
    XCTAssertEqual(m.expiry.daysToExpiry, 45);
    XCTAssertEqualWithAccuracy(m.atmIv, 0.30, 0.01);
    XCTAssertGreaterThan(m.skew, 0.0, @"negative skew parameter means puts carry more vol than calls");
    XCTAssertGreaterThan(m.expectedMove, 0.05);
    XCTAssertLessThan(m.expectedMove, 0.15);
    XCTAssertEqual(m.farExpiry.daysToExpiry, 90);
    XCTAssertGreaterThan(m.medianSpread, 0.0);
    XCTAssertEqualWithAccuracy(m.putCallVolume, 1.0, 1e-9);
}

- (void)testDeltaTargetingPicksOutOfTheMoneyStrikes
{
    const auto quotes = sampleChain(200.0, 0.30, -0.40);
    ChainIndex chain(quotes);
    const ListedExpiry* e = scan::expiryNear(chain, 45, 20, 60);
    XCTAssertTrue(e != nullptr);
    const auto calls = scan::ladder(chain, *e, OptionType::Call, marketAt(200.0));
    const auto puts = scan::ladder(chain, *e, OptionType::Put, marketAt(200.0));
    const scan::LadderPoint* c30 = scan::atDelta(calls, 0.30, 200.0);
    const scan::LadderPoint* p30 = scan::atDelta(puts, 0.30, 200.0);
    XCTAssertTrue(c30 && p30);
    XCTAssertGreaterThan(c30->quote->strike, 200.0);
    XCTAssertLessThan(p30->quote->strike, 200.0);
    XCTAssertEqualWithAccuracy(std::fabs(c30->delta), 0.30, 0.08);
    XCTAssertEqualWithAccuracy(std::fabs(p30->delta), 0.30, 0.08);
}

- (void)testIronCondorIsACreditWithDefinedRiskAndHighProbability
{
    const auto quotes = sampleChain(200.0, 0.30, -0.40);
    scan::Criteria c;
    c.minDays = 20; c.maxDays = 60; c.targetDays = 45;
    c.strategies = { StrategyPreset::IronCondor };
    c.maxPerTicker = 5;
    const auto ideas = scan::scanChain("TEST", quotes, marketAt(200.0), baseMarket(), c);
    XCTAssertFalse(ideas.empty());
    const scan::Idea& idea = ideas.front();
    XCTAssertEqual(idea.position.legs.size(), 4u);
    XCTAssertLessThan(idea.netPremium, 0.0, @"an iron condor is opened for a credit");
    XCTAssertFalse(idea.unboundedLoss);
    XCTAssertFalse(idea.unboundedProfit);
    XCTAssertEqualWithAccuracy(idea.maxProfit, -idea.netPremium, 1.0, @"max profit of a condor is the credit");
    XCTAssertGreaterThan(idea.probabilityOfProfit, 0.5);
    XCTAssertLessThan(idea.probabilityOfProfit, 0.95);
    XCTAssertGreaterThan(idea.returnOnRisk, 0.05);
    XCTAssertEqual(idea.breakevens.size(), 2u);
    XCTAssertTrue(idea.legs.find(" P ") != std::string::npos && idea.legs.find(" C ") != std::string::npos);
}

- (void)testFiltersAndBiasAreHonoured
{
    const auto quotes = sampleChain(200.0, 0.30, -0.40);
    scan::Criteria c;
    c.minDays = 20; c.maxDays = 60; c.targetDays = 45;
    c.bias = scan::Bias::Bullish;
    c.maxPerTicker = 20;
    auto ideas = scan::scanChain("TEST", quotes, marketAt(200.0), baseMarket(), c);
    XCTAssertFalse(ideas.empty());
    for (const scan::Idea& i : ideas) XCTAssertEqual(i.bias, scan::Bias::Bullish);
    // Defined risk only drops covered calls, collars, protective puts and risk reversals.
    c.definedRiskOnly = true;
    ideas = scan::scanChain("TEST", quotes, marketAt(200.0), baseMarket(), c);
    for (const scan::Idea& i : ideas) XCTAssertTrue(scan::definedRisk(i.preset));
    // An impossible probability filter leaves nothing.
    c.minProbability = 0.999;
    XCTAssertTrue(scan::scanChain("TEST", quotes, marketAt(200.0), baseMarket(), c).empty());
    // Expiry window outside the listed tenors leaves nothing.
    scan::Criteria far;
    far.minDays = 200; far.maxDays = 400; far.targetDays = 300;
    XCTAssertTrue(scan::scanChain("TEST", quotes, marketAt(200.0), baseMarket(), far).empty());
}

- (void)testIdeasAreRankedByScoreAndCappedPerTicker
{
    const auto quotes = sampleChain(200.0, 0.30, -0.40);
    scan::Criteria c;
    c.minDays = 20; c.maxDays = 60; c.targetDays = 45;
    c.maxPerTicker = 3;
    const auto ideas = scan::scanChain("TEST", quotes, marketAt(200.0), baseMarket(), c);
    XCTAssertEqual(ideas.size(), 3u);
    for (size_t i = 1; i < ideas.size(); ++i) XCTAssertGreaterThanOrEqual(ideas[i - 1].score, ideas[i].score);
    for (const scan::Idea& i : ideas) {
        XCTAssertFalse(i.rationale.empty());
        XCTAssertGreaterThan(i.score, 0.0);
        XCTAssertEqual(i.ticker, "TEST");
    }
}

@end
