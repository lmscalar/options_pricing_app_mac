//
//  SurfaceGridAndGreekPathTests.mm
//  PricingTests
//
//  The 3D surface grid sampled from a fitted surface, and Greeks along the calendar.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Strategy.h"
#include "../OptionPricing/Pricing/VolSurface.h"

#include <cmath>

using namespace pricing;

@interface SurfaceGridAndGreekPathTests : XCTestCase
@end

@implementation SurfaceGridAndGreekPathTests

- (void)testSurfaceGridFollowsTheFittedSlices
{
    ChainMarket cm;
    cm.spot = 100.0;
    cm.riskFreeRate = 0.03;
    const std::vector<ChainQuote> quotes = syntheticChain(cm, { 30.0 / 365.0, 90.0 / 365.0, 180.0 / 365.0 }, 0.25, -0.3, 0.5, 2.5, 14, 0.02);
    VolSurface surface(buildExpirySlices(quotes, cm));
    XCTAssertEqual(surface.slices().size(), 3u);
    const SurfaceGrid g = sampleSurface(surface, cm, 0.8, 1.2, 21);
    XCTAssertFalse(g.empty());
    XCTAssertEqual(g.strikes.size(), 21u);
    XCTAssertEqual(g.days.size(), 3u);
    XCTAssertEqual(g.vols.size(), 3u);
    XCTAssertEqualWithAccuracy(g.strikes.front(), 80.0, 1e-9);
    XCTAssertEqualWithAccuracy(g.strikes.back(), 120.0, 1e-9);
    XCTAssertLessThan(g.days[0], g.days[1]);
    XCTAssertLessThan(g.days[1], g.days[2]);
    // The grid at the money reproduces the fitted ATM vol; wings carry the skew (puts richer).
    const double atm = g.vols[0][10];
    XCTAssertEqualWithAccuracy(atm, surface.slices()[0].atmVol(), 0.01);
    XCTAssertGreaterThan(g.vols[0].front(), g.vols[0].back(), @"negative skew: low strikes carry more vol");
    XCTAssertGreaterThan(g.maxVol, g.minVol);
    XCTAssertTrue(sampleSurface(VolSurface(), cm).empty());
}

- (void)testGreeksOverTimeConvergeToExpiryBehaviour
{
    Market m;
    m.spot = 100.0;
    m.riskFreeRate = 0.03;
    m.volatility = 0.25;
    Position itm;
    itm.legs = { Leg{ LegKind::Call, 1.0, 90.0, 60.0 / 365.0, 0.0, 0.0, 0.0 } };
    const auto itmPath = greeksOverTime(itm, m, 100.0, 40);
    XCTAssertEqual(itmPath.size(), 40u);
    XCTAssertEqualWithAccuracy(itmPath.front().days, 0.0, 1e-12);
    XCTAssertGreaterThan(itmPath.back().days, 55.0);
    XCTAssertGreaterThan(itmPath.back().greeks.delta, itmPath.front().greeks.delta, @"an in-the-money call's delta rises toward 100 as expiry nears");
    XCTAssertGreaterThan(itmPath.back().greeks.delta, 99.0);
    Position otm;
    otm.legs = { Leg{ LegKind::Call, 1.0, 115.0, 60.0 / 365.0, 0.0, 0.0, 0.0 } };
    const auto otmPath = greeksOverTime(otm, m, 100.0, 40);
    XCTAssertLessThan(otmPath.back().greeks.delta, otmPath.front().greeks.delta, @"an out-of-the-money call's delta decays toward zero");
    XCTAssertLessThan(otmPath.back().greeks.delta, 1.0);
    Position atm;
    atm.legs = { Leg{ LegKind::Call, 1.0, 100.0, 60.0 / 365.0, 0.0, 0.0, 0.0 } };
    const auto atmPath = greeksOverTime(atm, m, 100.0, 40);
    XCTAssertGreaterThan(atmPath.back().greeks.gamma, atmPath.front().greeks.gamma, @"at-the-money gamma grows into expiry");
    XCTAssertLessThan(atmPath.back().greeks.theta, atmPath.front().greeks.theta, @"at-the-money theta becomes more negative into expiry");
    // No option legs: a single point.
    Position stock;
    stock.legs = { Leg{ LegKind::Underlying, 100.0, 0.0, 0.0, 0.0, 100.0, 100.0 } };
    XCTAssertEqual(greeksOverTime(stock, m, 100.0, 40).size(), 1u);
}

@end
