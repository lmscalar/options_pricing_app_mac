//
//  IvHistoryTests.mm
//  PricingTests
//
//  IV rank / percentile statistics, the 30-day constant-maturity vol, a chain sample, and
//  the implied vol recovered from a call / put pair of closes.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/IvHistory.h"
#include "../OptionPricing/Pricing/VolSurface.h"

#include <cmath>

using namespace pricing;

namespace {

std::vector<ivhist::Sample> rampHistory(int n, double from, double to)
{
    std::vector<ivhist::Sample> h;
    for (int i = 0; i < n; ++i) {
        ivhist::Sample s;
        s.date = "2026-01-" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(std::min(28, i + 1));
        s.iv30 = from + (to - from) * i / (n - 1);
        h.push_back(s);
    }
    return h;
}

} // namespace

@interface IvHistoryTests : XCTestCase
@end

@implementation IvHistoryTests

- (void)testRankAndPercentileOnARamp
{
    const auto h = rampHistory(21, 0.20, 0.40);   // 0.20, 0.21, ... 0.40
    const ivhist::Stats mid = ivhist::stats(h, 0.305);   // just above the eleventh sample, clear of rounding
    XCTAssertTrue(mid.ok);
    XCTAssertEqual(mid.samples, 21);
    XCTAssertEqualWithAccuracy(mid.rank, 0.525, 1e-9);
    XCTAssertEqualWithAccuracy(mid.percentile, 11.0 / 21.0, 1e-9, @"eleven samples at or below 0.305");
    XCTAssertEqualWithAccuracy(mid.low, 0.20, 1e-12);
    XCTAssertEqualWithAccuracy(mid.high, 0.40, 1e-12);
    XCTAssertEqualWithAccuracy(mid.median, 0.30, 1e-12);
    XCTAssertEqualWithAccuracy(mid.mean, 0.30, 1e-12);
    const ivhist::Stats top = ivhist::stats(h, 0.45);
    XCTAssertEqualWithAccuracy(top.rank, 1.0, 1e-12, @"rank is clamped above the range");
    XCTAssertEqualWithAccuracy(top.percentile, 1.0, 1e-12);
    const ivhist::Stats bottom = ivhist::stats(h, 0.10);
    XCTAssertEqualWithAccuracy(bottom.rank, 0.0, 1e-12);
    XCTAssertEqualWithAccuracy(bottom.percentile, 0.0, 1e-12);
}

- (void)testLookbackLimitsTheWindowAndSkipsEmptySamples
{
    auto h = rampHistory(21, 0.20, 0.40);
    h[5].iv30 = 0.0;   // a day without a chain
    const ivhist::Stats st = ivhist::stats(h, 0.35, 10);
    XCTAssertEqual(st.samples, 10, @"only the last ten valid samples count");
    XCTAssertEqualWithAccuracy(st.low, 0.31, 1e-9, @"window starts at the eleventh-from-last sample");
    XCTAssertEqualWithAccuracy(st.high, 0.40, 1e-9);
    const ivhist::Stats tooFew = ivhist::stats(rampHistory(21, 0.2, 0.4), 0.3, 1);
    XCTAssertFalse(tooFew.ok);
}

- (void)testConstantMaturityVolOnAFlatTermStructureIsFlat
{
    ChainMarket cm;
    cm.spot = 100.0;
    cm.riskFreeRate = 0.03;
    std::vector<ChainQuote> quotes = syntheticChain(cm, { 10.0 / 365.0, 45.0 / 365.0, 100.0 / 365.0 }, 0.28, 0.0, 0.0, 2.5, 12, 0.02);
    for (ChainQuote& q : quotes) q.daysToExpiry = static_cast<int>(std::lround(q.maturity * 365.0));
    ActivityMarket am;
    am.spot = 100.0;
    am.rateFor = [](double) { return 0.03; };
    const ivhist::Sample s = ivhist::sampleFromChain("2026-10-09", quotes, am);
    XCTAssertEqualWithAccuracy(s.iv30, 0.28, 0.01);
    XCTAssertEqualWithAccuracy(s.ivNear, 0.28, 0.01);
    XCTAssertEqualWithAccuracy(s.spot, 100.0, 1e-12);
    XCTAssertEqual(s.source, "snapshot");
}

- (void)testPairOfClosesRecoversTheVol
{
    Inputs in;
    in.spot = 250.0;
    in.strike = 250.0;
    in.riskFreeRate = 0.04;
    in.volatility = 0.33;
    in.maturity = 35.0 / 365.0;
    const Result r = price(in);
    const double iv = ivhist::impliedPairFromCloses(250.0, 250.0, in.maturity, 0.04, r.callPrice, r.putPrice);
    XCTAssertEqualWithAccuracy(iv, 0.33, 1e-4);
    // One side stale below intrinsic: the other side carries the estimate.
    const double oneSide = ivhist::impliedPairFromCloses(250.0, 250.0, in.maturity, 0.04, 0.01, r.putPrice);
    XCTAssertEqualWithAccuracy(oneSide, 0.33, 1e-4);
    XCTAssertEqual(ivhist::impliedPairFromCloses(250.0, 250.0, in.maturity, 0.04, 0.0, 0.0), 0.0);
}

@end
