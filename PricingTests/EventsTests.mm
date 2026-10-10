//
//  EventsTests.mm
//  PricingTests
//
//  Earnings analytics: a chain built with extra variance on the first expiry yields the
//  injected event move, the event is found with and without a known date, skew makes the
//  miss side deeper than the beat side, the cone jumps at the event date, and a flat term
//  structure reports no event.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Events.h"
#include "../OptionPricing/Pricing/VolSurface.h"

#include <cmath>

using namespace pricing;

namespace {

/// Expiries at 20, 48 and 90 days. Every expiry at or after `eventExpiryIndex` carries
/// `eventMove`^2 of extra total variance on top of a flat `baseVol`.
std::vector<ChainQuote> chainWithEvent(double spot, double baseVol, double eventMove, int eventExpiryIndex, double skew)
{
    const double days[] = { 20.0, 48.0, 90.0 };
    std::vector<ChainQuote> all;
    for (int i = 0; i < 3; ++i) {
        const double T = days[i] / 365.0;
        const double variance = baseVol * baseVol * T + (i >= eventExpiryIndex ? eventMove * eventMove : 0.0);
        ChainMarket cm;
        cm.spot = spot;
        cm.riskFreeRate = 0.04;
        std::vector<ChainQuote> slice = syntheticChain(cm, { T }, std::sqrt(variance / T), skew, 0.3, 5.0, 16, 0.03);
        for (ChainQuote& q : slice) {
            q.daysToExpiry = static_cast<int>(days[i]);
            q.expiryDate = "2026-11-" + std::string(q.daysToExpiry < 10 ? "0" : "") + std::to_string(std::min(28, q.daysToExpiry));
        }
        all.insert(all.end(), slice.begin(), slice.end());
    }
    return all;
}

ActivityMarket marketAt(double spot)
{
    ActivityMarket am;
    am.spot = spot;
    am.rateFor = [](double) { return 0.04; };
    return am;
}

} // namespace

@interface EventsTests : XCTestCase
@end

@implementation EventsTests

- (void)testTermStructureRecoversTheVolsItWasBuiltFrom
{
    const auto quotes = chainWithEvent(200.0, 0.30, 0.0, 99, 0.0);
    const events::Analysis a = events::analyze(quotes, marketAt(200.0));
    XCTAssertTrue(a.ok);
    XCTAssertEqual(a.term.size(), 3u);
    for (const events::TermPoint& tp : a.term) XCTAssertEqualWithAccuracy(tp.atmIv, 0.30, 0.01);
    XCTAssertFalse(a.eventDetected, @"a flat term structure prices no event");
    XCTAssertEqualWithAccuracy(a.term[1].forwardVol, 0.30, 0.02);
}

- (void)testEventMoveIsInferredFromTheVarianceJump
{
    const auto quotes = chainWithEvent(200.0, 0.30, 0.08, 0, 0.0);
    const events::Analysis a = events::analyze(quotes, marketAt(200.0));
    XCTAssertTrue(a.ok);
    XCTAssertTrue(a.eventDetected);
    XCTAssertFalse(a.eventDateKnown);
    XCTAssertEqual(a.eventExpiry.daysToExpiry, 20);
    XCTAssertEqualWithAccuracy(a.eventMove, 0.08, 0.01);
    XCTAssertEqualWithAccuracy(a.baselineVol, 0.30, 0.03);
    XCTAssertGreaterThan(a.jumpRatio, 1.25);
    XCTAssertGreaterThan(a.eventDays, 0);
    XCTAssertLessThanOrEqual(a.eventDays, 20);
}

- (void)testKnownDateSelectsTheExpiryAfterIt
{
    // Event between the first and second expiry: the 48-day expiry carries the variance.
    const auto quotes = chainWithEvent(200.0, 0.30, 0.06, 1, 0.0);
    const events::Analysis a = events::analyze(quotes, marketAt(200.0), 30);
    XCTAssertTrue(a.eventDetected);
    XCTAssertTrue(a.eventDateKnown);
    XCTAssertEqual(a.eventDays, 30);
    XCTAssertEqual(a.eventExpiry.daysToExpiry, 48);
    XCTAssertEqualWithAccuracy(a.eventMove, 0.06, 0.01);
    // Without a date the inference lands on the same expiry.
    const events::Analysis inferred = events::analyze(quotes, marketAt(200.0));
    XCTAssertTrue(inferred.eventDetected);
    XCTAssertEqual(inferred.eventExpiry.daysToExpiry, 48);
    XCTAssertEqual(inferred.eventDays, 21, @"inferred as the day after the previous expiry");
}

- (void)testPutSkewMakesTheMissSideDeeper
{
    const auto quotes = chainWithEvent(200.0, 0.30, 0.08, 0, -0.5);
    const events::Analysis a = events::analyze(quotes, marketAt(200.0), 10);
    XCTAssertTrue(a.eventDetected);
    XCTAssertGreaterThan(a.skewDown, a.skewUp);
    XCTAssertGreaterThan(a.downMove, a.upMove);
    XCTAssertGreaterThan(a.downMove, 0.0);
    const events::ConePoint c = events::coneAt(a, 15.0 / 365.0);
    XCTAssertGreaterThan(200.0 - c.down1, c.up1 - 200.0, @"the lower band sits further from spot than the upper band");
}

- (void)testConeJumpsAtTheEventDateAndWidensWithTime
{
    const auto quotes = chainWithEvent(200.0, 0.30, 0.08, 0, 0.0);
    const events::Analysis a = events::analyze(quotes, marketAt(200.0), 10);
    const events::ConePoint before = events::coneAt(a, 9.0 / 365.0);
    const events::ConePoint after = events::coneAt(a, 10.0 / 365.0);
    const events::ConePoint later = events::coneAt(a, 19.0 / 365.0);
    XCTAssertGreaterThan(after.up1 - before.up1, 5.0, @"the event variance switches on at the event date");
    XCTAssertGreaterThan(later.up1, after.up1);
    XCTAssertLessThan(later.down1, after.down1);
    XCTAssertGreaterThan(after.up2, after.up1);
    XCTAssertLessThan(after.down2, after.down1);
    // Before the event only the baseline diffusion applies: ~0.30 * sqrt(9/365) one sd.
    XCTAssertEqualWithAccuracy(std::log(before.up1 / 200.0), 0.30 * std::sqrt(9.0 / 365.0), 0.01);
    // At the event: baseline plus the 8% event move in quadrature.
    XCTAssertEqualWithAccuracy(std::log(after.up1 / 200.0), std::sqrt(0.09 * 10.0 / 365.0 + 0.0064), 0.01);
    const events::ConePoint start = events::coneAt(a, 0.0);
    XCTAssertEqualWithAccuracy(start.up1, 200.0, 1e-9);
}

@end
