//
//  BlackScholesTests.mm
//  PricingTests
//
//  Closed-form pricing checked against textbook reference values (Hull, Options,
//  Futures and Other Derivatives), put-call parity, finite-difference Greeks and
//  limiting behaviour.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/BlackScholes.h"

#include <random>

using namespace pricing;

namespace {

Inputs hullExample()
{
    // Hull, Example 15.6: S = 42, K = 40, r = 10%, sigma = 20%, T = 0.5.
    Inputs in;
    in.spot = 42.0;
    in.strike = 40.0;
    in.riskFreeRate = 0.10;
    in.volatility = 0.20;
    in.maturity = 0.5;
    return in;
}

/// Central finite difference of a scalar function of Inputs.
template <typename Mutator, typename Value>
double centralDifference(const Inputs& base, Mutator mutate, Value value, double h)
{
    Inputs up = base, down = base;
    mutate(up, h);
    mutate(down, -h);
    return (value(up) - value(down)) / (2.0 * h);
}

} // namespace

@interface BlackScholesTests : XCTestCase
@end

@implementation BlackScholesTests

- (void)testHullReferencePrices
{
    const Result r = price(hullExample());
    XCTAssertEqualWithAccuracy(r.callPrice, 4.76, 0.005);
    XCTAssertEqualWithAccuracy(r.putPrice, 0.81, 0.005);
    XCTAssertEqualWithAccuracy(r.d1, 0.7693, 1e-4);
    XCTAssertEqualWithAccuracy(r.d2, 0.6278, 1e-4);
}

- (void)testAtTheMoneyOneYearReference
{
    Inputs in;
    in.spot = 100; in.strike = 100; in.riskFreeRate = 0.05; in.volatility = 0.20; in.maturity = 1.0;
    const Result r = price(in);
    // Widely quoted reference values for these inputs.
    XCTAssertEqualWithAccuracy(r.callPrice, 10.450584, 1e-6);
    XCTAssertEqualWithAccuracy(r.putPrice, 5.573526, 1e-6);
    XCTAssertEqualWithAccuracy(r.call.delta, 0.636831, 1e-6);
    XCTAssertEqualWithAccuracy(r.call.gamma, 0.018762, 1e-6);
    XCTAssertEqualWithAccuracy(r.call.vega, 0.375240, 1e-6);         // per 1%
    XCTAssertEqualWithAccuracy(r.call.theta * 365.0, -6.414028, 1e-5); // annual
    XCTAssertEqualWithAccuracy(r.call.rho, 0.532325, 1e-6);          // per 1%
}

- (void)testHullGreeks
{
    // Hull Chapter 19 examples use S = 49, K = 50, r = 5%, sigma = 20%, T = 20 weeks.
    Inputs in;
    in.spot = 49; in.strike = 50; in.riskFreeRate = 0.05; in.volatility = 0.20; in.maturity = 20.0 / 52.0;
    const Result r = price(in);
    XCTAssertEqualWithAccuracy(r.call.delta, 0.522, 0.001);
    XCTAssertEqualWithAccuracy(r.call.gamma, 0.066, 0.001);
    XCTAssertEqualWithAccuracy(r.call.vega * 100.0, 12.1, 0.05);
    XCTAssertEqualWithAccuracy(r.call.theta * 365.0, -4.31, 0.01);
    XCTAssertEqualWithAccuracy(r.call.rho * 100.0, 8.91, 0.01);
}

- (void)testPutCallParityOnRandomInputs
{
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> spot(10.0, 500.0), moneyness(0.5, 1.5), rate(-0.02, 0.12),
        yield(0.0, 0.08), vol(0.05, 1.2), maturity(0.01, 5.0);
    for (int i = 0; i < 500; ++i) {
        Inputs in;
        in.spot = spot(rng);
        in.strike = in.spot * moneyness(rng);
        in.riskFreeRate = rate(rng);
        in.dividendYield = yield(rng);
        in.volatility = vol(rng);
        in.maturity = maturity(rng);
        const Result r = price(in);
        const double lhs = r.callPrice - r.putPrice;
        const double rhs = in.spot * std::exp(-in.dividendYield * in.maturity) - in.strike * std::exp(-in.riskFreeRate * in.maturity);
        XCTAssertEqualWithAccuracy(lhs, rhs, 1e-9 * std::max(1.0, in.spot), @"parity failed for sample %d", i);
        // Deltas differ by the dividend discount; gammas and vegas coincide.
        XCTAssertEqualWithAccuracy(r.call.delta - r.put.delta, std::exp(-in.dividendYield * in.maturity), 1e-12);
        XCTAssertEqualWithAccuracy(r.call.gamma, r.put.gamma, 1e-15);
        XCTAssertEqualWithAccuracy(r.call.vega, r.put.vega, 1e-15);
    }
}

- (void)testBlack76ParityAndRho
{
    Inputs in;
    in.model = Model::Black76;
    in.spot = 100; in.strike = 95; in.riskFreeRate = 0.04; in.volatility = 0.3; in.maturity = 0.75;
    const Result r = price(in);
    const double discount = std::exp(-in.riskFreeRate * in.maturity);
    XCTAssertEqualWithAccuracy(r.callPrice - r.putPrice, discount * (in.spot - in.strike), 1e-10);
    // Rho under Black-76: dV/dr = -T V.
    XCTAssertEqualWithAccuracy(r.call.rho, -in.maturity * r.callPrice / 100.0, 1e-12);
    // Delta is discounted N(d1), never above the discount factor.
    XCTAssertLessThanOrEqual(r.call.delta, discount + 1e-12);
    // Dividend yield is ignored for futures.
    Inputs withYield = in;
    withYield.dividendYield = 0.07;
    XCTAssertEqualWithAccuracy(price(withYield).callPrice, r.callPrice, 1e-12);
}

- (void)testFirstOrderGreeksMatchFiniteDifferences
{
    Inputs in;
    in.spot = 105; in.strike = 100; in.riskFreeRate = 0.03; in.dividendYield = 0.015; in.volatility = 0.25; in.maturity = 0.8;
    const Result r = price(in);
    auto callValue = [](const Inputs& i) { return price(i).callPrice; };
    auto putValue = [](const Inputs& i) { return price(i).putPrice; };

    const double dSpot = centralDifference(in, [](Inputs& i, double h) { i.spot += h; }, callValue, 1e-3);
    XCTAssertEqualWithAccuracy(r.call.delta, dSpot, 1e-6);
    const double dSpotPut = centralDifference(in, [](Inputs& i, double h) { i.spot += h; }, putValue, 1e-3);
    XCTAssertEqualWithAccuracy(r.put.delta, dSpotPut, 1e-6);

    const double dVol = centralDifference(in, [](Inputs& i, double h) { i.volatility += h; }, callValue, 1e-4);
    XCTAssertEqualWithAccuracy(r.call.vega, dVol / 100.0, 1e-6);

    const double dRate = centralDifference(in, [](Inputs& i, double h) { i.riskFreeRate += h; }, callValue, 1e-5);
    XCTAssertEqualWithAccuracy(r.call.rho, dRate / 100.0, 1e-6);
    const double dRatePut = centralDifference(in, [](Inputs& i, double h) { i.riskFreeRate += h; }, putValue, 1e-5);
    XCTAssertEqualWithAccuracy(r.put.rho, dRatePut / 100.0, 1e-6);

    // Theta is the derivative with respect to time passing: -dV/dT per day.
    const double dMaturity = centralDifference(in, [](Inputs& i, double h) { i.maturity += h; }, callValue, 1e-5);
    XCTAssertEqualWithAccuracy(r.call.theta, -dMaturity / 365.0, 1e-7);
    const double dMaturityPut = centralDifference(in, [](Inputs& i, double h) { i.maturity += h; }, putValue, 1e-5);
    XCTAssertEqualWithAccuracy(r.put.theta, -dMaturityPut / 365.0, 1e-7);

    // Gamma is the second derivative in spot.
    const double h = 0.05;
    Inputs up = in, down = in;
    up.spot += h;
    down.spot -= h;
    const double gammaFd = (price(up).callPrice - 2.0 * r.callPrice + price(down).callPrice) / (h * h);
    XCTAssertEqualWithAccuracy(r.call.gamma, gammaFd, 1e-6);
}

- (void)testSecondOrderGreeksMatchFiniteDifferences
{
    Inputs in;
    in.spot = 95; in.strike = 100; in.riskFreeRate = 0.02; in.dividendYield = 0.01; in.volatility = 0.3; in.maturity = 0.6;
    const Result r = price(in);
    auto callDelta = [](const Inputs& i) { return price(i).call.delta; };
    auto putDelta = [](const Inputs& i) { return price(i).put.delta; };
    auto gamma = [](const Inputs& i) { return price(i).call.gamma; };
    auto vegaPerUnit = [](const Inputs& i) { return price(i).call.vega * 100.0; };

    // Vanna: dDelta/dVol per 1%.
    const double vanna = centralDifference(in, [](Inputs& i, double h) { i.volatility += h; }, callDelta, 1e-4);
    XCTAssertEqualWithAccuracy(r.call.vanna, vanna / 100.0, 1e-7);

    // Volga: d(vega per unit)/dVol, quoted per 1% squared.
    const double volga = centralDifference(in, [](Inputs& i, double h) { i.volatility += h; }, vegaPerUnit, 1e-4);
    XCTAssertEqualWithAccuracy(r.call.volga, volga / 10000.0, 1e-7);

    // Charm: dDelta/dt = -dDelta/dT per day, for both legs.
    const double charmCall = centralDifference(in, [](Inputs& i, double h) { i.maturity += h; }, callDelta, 1e-5);
    XCTAssertEqualWithAccuracy(r.call.charm, -charmCall / 365.0, 1e-8);
    const double charmPut = centralDifference(in, [](Inputs& i, double h) { i.maturity += h; }, putDelta, 1e-5);
    XCTAssertEqualWithAccuracy(r.put.charm, -charmPut / 365.0, 1e-8);

    // Speed: dGamma/dS.
    const double speed = centralDifference(in, [](Inputs& i, double h) { i.spot += h; }, gamma, 1e-3);
    XCTAssertEqualWithAccuracy(r.call.speed, speed, 1e-8);

    // Color: dGamma/dt = -dGamma/dT per day.
    const double color = centralDifference(in, [](Inputs& i, double h) { i.maturity += h; }, gamma, 1e-5);
    XCTAssertEqualWithAccuracy(r.call.color, -color / 365.0, 1e-8);

    // Zomma: dGamma/dVol per 1%.
    const double zomma = centralDifference(in, [](Inputs& i, double h) { i.volatility += h; }, gamma, 1e-4);
    XCTAssertEqualWithAccuracy(r.call.zomma, zomma / 100.0, 1e-7);
}

- (void)testThetaBasisScalesPerDayQuotes
{
    Inputs in = hullExample();
    const Result calendar = price(in);
    in.dayBasis = 252.0;
    const Result trading = price(in);
    XCTAssertEqualWithAccuracy(trading.call.theta * 252.0, calendar.call.theta * 365.0, 1e-12);
    XCTAssertEqualWithAccuracy(trading.call.charm * 252.0, calendar.call.charm * 365.0, 1e-12);
    XCTAssertEqualWithAccuracy(trading.call.color * 252.0, calendar.call.color * 365.0, 1e-12);
    XCTAssertEqualWithAccuracy(trading.callPrice, calendar.callPrice, 0.0);
}

- (void)testNearExpiryConvergesToIntrinsic
{
    Inputs in;
    in.spot = 110; in.strike = 100; in.riskFreeRate = 0.05; in.volatility = 0.2; in.maturity = 1e-6;
    const Result r = price(in);
    XCTAssertEqualWithAccuracy(r.callPrice, 10.0, 1e-3);
    XCTAssertEqualWithAccuracy(r.putPrice, 0.0, 1e-6);
    XCTAssertEqualWithAccuracy(r.call.delta, 1.0, 1e-6);
    XCTAssertEqualWithAccuracy(r.put.delta, 0.0, 1e-6);
    XCTAssertTrue(std::isfinite(r.call.gamma));
    XCTAssertTrue(std::isfinite(r.call.theta));
}

- (void)testDeepInAndOutOfTheMoneyBounds
{
    Inputs in;
    in.spot = 100; in.strike = 20; in.riskFreeRate = 0.03; in.volatility = 0.25; in.maturity = 1.0;
    Result r = price(in);
    // Deep ITM call is worth the forward minus discounted strike.
    XCTAssertEqualWithAccuracy(r.callPrice, in.spot - in.strike * std::exp(-in.riskFreeRate * in.maturity), 1e-6);
    XCTAssertEqualWithAccuracy(r.putPrice, 0.0, 1e-6);

    in.strike = 500;
    r = price(in);
    XCTAssertEqualWithAccuracy(r.callPrice, 0.0, 1e-6);
    XCTAssertEqualWithAccuracy(r.putPrice, in.strike * std::exp(-in.riskFreeRate * in.maturity) - in.spot, 1e-6);
}

- (void)testNegativeRatesArePriced
{
    Inputs in;
    in.spot = 100; in.strike = 100; in.riskFreeRate = -0.01; in.volatility = 0.15; in.maturity = 2.0;
    XCTAssertTrue(isValid(in));
    const Result r = price(in);
    XCTAssertTrue(std::isfinite(r.callPrice));
    // With a negative rate the forward is below spot, so the put is worth more than the call.
    XCTAssertGreaterThan(r.putPrice, r.callPrice);
    XCTAssertEqualWithAccuracy(r.callPrice - r.putPrice, in.spot - in.strike * std::exp(-in.riskFreeRate * in.maturity), 1e-10);
}

- (void)testDiscreteDividendsLowerCallsAndRaisePuts
{
    Inputs plain;
    plain.spot = 100; plain.strike = 100; plain.riskFreeRate = 0.05; plain.volatility = 0.2; plain.maturity = 1.0;
    Inputs withDividend = plain;
    withDividend.dividends = { { 0.5, 3.0 } };
    const Result a = price(plain);
    const Result b = price(withDividend);
    XCTAssertLessThan(b.callPrice, a.callPrice);
    XCTAssertGreaterThan(b.putPrice, a.putPrice);
    XCTAssertEqualWithAccuracy(b.effectiveSpot, 100.0 - 3.0 * std::exp(-0.05 * 0.5), 1e-12);
    // A dividend after expiry has no effect.
    Inputs late = plain;
    late.dividends = { { 2.0, 3.0 } };
    XCTAssertEqualWithAccuracy(price(late).callPrice, a.callPrice, 1e-12);
    // Dividends that exceed the spot make the contract invalid rather than producing NaN.
    Inputs huge = plain;
    huge.dividends = { { 0.1, 200.0 } };
    XCTAssertFalse(hasValidContract(huge));
}

- (void)testImpliedVolatilityRoundTrip
{
    std::mt19937 rng(777);
    std::uniform_real_distribution<double> moneyness(0.7, 1.3), vol(0.08, 0.9), maturity(0.05, 3.0);
    for (int i = 0; i < 200; ++i) {
        Inputs in;
        in.spot = 100;
        in.strike = 100 * moneyness(rng);
        in.riskFreeRate = 0.03;
        in.dividendYield = 0.01;
        in.volatility = vol(rng);
        in.maturity = maturity(rng);
        const Result r = price(in);
        for (OptionType type : { OptionType::Call, OptionType::Put }) {
            const double market = type == OptionType::Call ? r.callPrice : r.putPrice;
            const ImpliedVolResult iv = impliedVolatility(in, type, market);
            XCTAssertEqual(iv.status, ImpliedVolResult::Status::Converged, @"sample %d", i);
            XCTAssertEqualWithAccuracy(iv.volatility, in.volatility, 1e-6, @"sample %d", i);
            XCTAssertLessThan(iv.iterations, 40);
        }
    }
}

- (void)testImpliedVolatilityRejectsPricesOutsideBounds
{
    Inputs in;
    in.spot = 100; in.strike = 90; in.riskFreeRate = 0.05; in.maturity = 1.0;
    const double intrinsic = 100.0 - 90.0 * std::exp(-0.05);
    XCTAssertEqual(impliedVolatility(in, OptionType::Call, intrinsic * 0.5).status, ImpliedVolResult::Status::BelowLowerBound);
    XCTAssertEqual(impliedVolatility(in, OptionType::Call, 150.0).status, ImpliedVolResult::Status::AboveUpperBound);
    XCTAssertEqual(impliedVolatility(in, OptionType::Call, -1.0).status, ImpliedVolResult::Status::InvalidInputs);
    const ImpliedVolResult ok = impliedVolatility(in, OptionType::Call, intrinsic + 2.0);
    XCTAssertEqual(ok.status, ImpliedVolResult::Status::Converged);
}

- (void)testProbabilitiesAreConsistent
{
    Inputs in;
    in.spot = 100; in.strike = 110; in.riskFreeRate = 0.02; in.volatility = 0.3; in.maturity = 1.0;
    const Result r = price(in);
    const Probabilities p = probabilities(in, r);
    XCTAssertEqualWithAccuracy(p.callInTheMoney + p.putInTheMoney, 1.0, 1e-12);
    XCTAssertGreaterThanOrEqual(p.touchStrike, p.callInTheMoney);   // touching is easier than finishing beyond
    XCTAssertLessThanOrEqual(p.touchStrike, 1.0);
    XCTAssertLessThan(p.lowerOneSigma, r.forward);
    XCTAssertGreaterThan(p.upperOneSigma, r.forward);
    XCTAssertEqualWithAccuracy(p.callBreakeven, in.strike + r.callPrice, 1e-12);
    XCTAssertEqualWithAccuracy(p.putBreakeven, in.strike - r.putPrice, 1e-12);
    XCTAssertEqualWithAccuracy(p.oneSigmaMove, 30.0, 1e-12);

    // Strike at spot: already touched.
    in.strike = 100;
    const Result atm = price(in);
    XCTAssertEqualWithAccuracy(probabilities(in, atm).touchStrike, 1.0, 1e-12);
    // Mirror symmetry: a barrier below behaves like one above under reflected drift.
    in.strike = 90;
    const Probabilities below = probabilities(in, price(in));
    XCTAssertGreaterThan(below.touchStrike, below.putInTheMoney);
}

- (void)testNormalInverseIsAccurate
{
    XCTAssertEqualWithAccuracy(normalInverseCdf(0.5), 0.0, 1e-12);
    XCTAssertEqualWithAccuracy(normalInverseCdf(0.975), 1.959963984540054, 1e-10);
    XCTAssertEqualWithAccuracy(normalInverseCdf(0.001), -3.090232306167813, 1e-9);
    for (double x = -5.0; x <= 5.0; x += 0.25) {
        XCTAssertEqualWithAccuracy(normalInverseCdf(normalCdf(x)), x, 1e-8);
    }
}

@end
