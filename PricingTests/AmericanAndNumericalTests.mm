//
//  AmericanAndNumericalTests.mm
//  PricingTests
//
//  American exercise (binomial tree, Bjerksund-Stensland) and the independent
//  numerical engines (finite differences, Monte Carlo), plus day-count and rate-curve
//  utilities.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/American.h"
#include "../OptionPricing/Pricing/DayCount.h"
#include "../OptionPricing/Pricing/FiniteDifference.h"
#include "../OptionPricing/Pricing/MonteCarlo.h"
#include "../OptionPricing/Pricing/RateCurve.h"

using namespace pricing;

@interface AmericanAndNumericalTests : XCTestCase
@end

@implementation AmericanAndNumericalTests

- (void)testBinomialEuropeanConvergesToClosedForm
{
    Inputs in;
    in.spot = 100; in.strike = 100; in.riskFreeRate = 0.05; in.volatility = 0.2; in.maturity = 1.0;
    const Result closed = price(in);
    const TreeResult call = binomialPrice(in, OptionType::Call, Exercise::European, 1000);
    const TreeResult put = binomialPrice(in, OptionType::Put, Exercise::European, 1000);
    XCTAssertEqualWithAccuracy(call.price, closed.callPrice, 0.01);
    XCTAssertEqualWithAccuracy(put.price, closed.putPrice, 0.01);
    XCTAssertEqualWithAccuracy(call.delta, closed.call.delta, 0.002);
    XCTAssertEqualWithAccuracy(call.gamma, closed.call.gamma, 0.001);
    XCTAssertEqualWithAccuracy(call.theta, closed.call.theta, 0.001);
}

- (void)testAmericanPutReferenceValue
{
    // S = K = 100, r = 5%, sigma = 20%, T = 1: the American put is about 6.09 (Hull /
    // QuantLib finite-difference and tree values agree to the cent).
    Inputs in;
    in.spot = 100; in.strike = 100; in.riskFreeRate = 0.05; in.volatility = 0.2; in.maturity = 1.0;
    const AmericanResult put = americanValuation(in, OptionType::Put, 1000);
    XCTAssertEqualWithAccuracy(put.price, 6.09, 0.01);
    XCTAssertGreaterThan(put.earlyExercisePremium, 0.4);
    XCTAssertEqualWithAccuracy(put.europeanPrice, 5.573526, 1e-6);
}

- (void)testAmericanNeverBelowEuropeanOrIntrinsic
{
    for (double strike : { 80.0, 100.0, 120.0 }) {
        for (double yield : { 0.0, 0.03, 0.08 }) {
            Inputs in;
            in.spot = 100; in.strike = strike; in.riskFreeRate = 0.04; in.dividendYield = yield; in.volatility = 0.25; in.maturity = 1.5;
            for (OptionType type : { OptionType::Call, OptionType::Put }) {
                // Compare trees with the same step count so discretisation error cancels;
                // the American roll-back applies max(continuation, intrinsic) at every node.
                const double europeanTree = binomialPrice(in, type, Exercise::European, 500).price;
                const double american = binomialPrice(in, type, Exercise::American, 500).price;
                XCTAssertGreaterThanOrEqual(american + 1e-9, europeanTree);
                XCTAssertGreaterThanOrEqual(american + 1e-9, payoff(type, strike, in.spot));
                // Against the closed form the tree is accurate to well under a cent.
                XCTAssertGreaterThanOrEqual(american + 0.01, legPrice(in, type));
            }
        }
    }
}

- (void)testAmericanCallWithoutDividendsEqualsEuropean
{
    Inputs in;
    in.spot = 100; in.strike = 95; in.riskFreeRate = 0.06; in.dividendYield = 0.0; in.volatility = 0.3; in.maturity = 1.0;
    const double european = price(in).callPrice;
    XCTAssertEqualWithAccuracy(binomialPrice(in, OptionType::Call, Exercise::American, 800).price, european, 0.01);
    XCTAssertEqualWithAccuracy(bjerksundStenslandPrice(in, OptionType::Call), european, 1e-9);
}

- (void)testBjerksundStenslandIsCloseToTree
{
    // Haug, "The Complete Guide to Option Pricing Formulas": American call with S = 42,
    // K = 40, T = 0.75, r = 4%, b = -4% (q = 8%), sigma = 35% is 5.2704 under BS (1993).
    Inputs in;
    in.spot = 42; in.strike = 40; in.riskFreeRate = 0.04; in.dividendYield = 0.08; in.volatility = 0.35; in.maturity = 0.75;
    const double approx = bjerksundStenslandPrice(in, OptionType::Call);
    XCTAssertEqualWithAccuracy(approx, 5.2704, 5e-4);
    const double tree = binomialPrice(in, OptionType::Call, Exercise::American, 1000).price;
    XCTAssertEqualWithAccuracy(approx, tree, 0.05);
    XCTAssertGreaterThan(approx, price(in).callPrice);

    // Puts via the transformation.
    Inputs putIn;
    putIn.spot = 100; putIn.strike = 100; putIn.riskFreeRate = 0.05; putIn.volatility = 0.2; putIn.maturity = 1.0;
    const double putApprox = bjerksundStenslandPrice(putIn, OptionType::Put);
    const double putTree = binomialPrice(putIn, OptionType::Put, Exercise::American, 1000).price;
    XCTAssertEqualWithAccuracy(putApprox, putTree, 0.15);   // BS1993 is known to run a few cents low
    XCTAssertGreaterThanOrEqual(putApprox, price(putIn).putPrice);
}

- (void)testDeepInTheMoneyAmericanPutIsIntrinsic
{
    Inputs in;
    in.spot = 50; in.strike = 100; in.riskFreeRate = 0.05; in.volatility = 0.2; in.maturity = 1.0;
    XCTAssertEqualWithAccuracy(binomialPrice(in, OptionType::Put, Exercise::American, 400).price, 50.0, 1e-6);
    XCTAssertEqualWithAccuracy(finiteDifferencePrice(in, OptionType::Put, Exercise::American).price, 50.0, 0.01);
}

- (void)testFiniteDifferenceMatchesClosedFormAndTree
{
    Inputs in;
    in.spot = 100; in.strike = 100; in.riskFreeRate = 0.05; in.dividendYield = 0.02; in.volatility = 0.25; in.maturity = 1.0;
    const Result closed = price(in);
    const FiniteDifferenceResult call = finiteDifferencePrice(in, OptionType::Call, Exercise::European, 600, 600);
    const FiniteDifferenceResult put = finiteDifferencePrice(in, OptionType::Put, Exercise::European, 600, 600);
    XCTAssertEqualWithAccuracy(call.price, closed.callPrice, 0.01);
    XCTAssertEqualWithAccuracy(put.price, closed.putPrice, 0.01);
    XCTAssertEqualWithAccuracy(call.delta, closed.call.delta, 0.003);
    XCTAssertEqualWithAccuracy(call.gamma, closed.call.gamma, 0.001);

    const FiniteDifferenceResult americanPut = finiteDifferencePrice(in, OptionType::Put, Exercise::American, 600, 600);
    const double treePut = binomialPrice(in, OptionType::Put, Exercise::American, 1000).price;
    XCTAssertEqualWithAccuracy(americanPut.price, treePut, 0.02);
}

- (void)testMonteCarloWithinStandardError
{
    Inputs in;
    in.spot = 100; in.strike = 105; in.riskFreeRate = 0.03; in.dividendYield = 0.01; in.volatility = 0.3; in.maturity = 0.75;
    const Result closed = price(in);
    const MonteCarloResult call = monteCarloPrice(in, OptionType::Call, 400000, 42);
    const MonteCarloResult put = monteCarloPrice(in, OptionType::Put, 400000, 43);
    XCTAssertEqual(call.paths, 400000);
    XCTAssertGreaterThan(call.standardError, 0.0);
    XCTAssertLessThan(std::fabs(call.price - closed.callPrice), 4.0 * call.standardError);
    XCTAssertLessThan(std::fabs(put.price - closed.putPrice), 4.0 * put.standardError);
    // Same seed reproduces the same estimate.
    XCTAssertEqual(monteCarloPrice(in, OptionType::Call, 400000, 42).price, call.price);
}

- (void)testBlack76EnginesAgree
{
    Inputs in;
    in.model = Model::Black76;
    in.spot = 75; in.strike = 70; in.riskFreeRate = 0.05; in.volatility = 0.35; in.maturity = 0.5;
    const Result closed = price(in);
    XCTAssertEqualWithAccuracy(binomialPrice(in, OptionType::Call, Exercise::European, 1000).price, closed.callPrice, 0.01);
    XCTAssertEqualWithAccuracy(finiteDifferencePrice(in, OptionType::Put, Exercise::European).price, closed.putPrice, 0.01);
    // American options on futures can be worth more than European for both legs.
    XCTAssertGreaterThanOrEqual(binomialPrice(in, OptionType::Call, Exercise::American).price + 1e-9, closed.callPrice);
    XCTAssertGreaterThanOrEqual(binomialPrice(in, OptionType::Put, Exercise::American).price + 1e-9, closed.putPrice);
}

- (void)testDayCountArithmetic
{
    const CivilDate start{ 2026, 10, 7 };   // a Wednesday
    XCTAssertEqual(weekdayIndex(start), 2);
    XCTAssertEqual(calendarDaysBetween(start, CivilDate{ 2027, 10, 7 }), 365);
    XCTAssertEqual(calendarDaysBetween(start, CivilDate{ 2028, 10, 7 }), 731);   // 2028 is a leap year
    XCTAssertEqual(businessDaysBetween(start, CivilDate{ 2026, 10, 14 }), 5);     // one full week
    XCTAssertEqual(businessDaysBetween(start, CivilDate{ 2026, 10, 9 }), 2);      // Thu, Fri
    XCTAssertEqual(businessDaysBetween(start, CivilDate{ 2026, 10, 12 }), 3);     // Thu, Fri, Mon
    XCTAssertEqual(businessDaysBetween(start, start), 0);
    XCTAssertEqual(businessDaysBetween(CivilDate{ 2026, 10, 12 }, start), 0);
    XCTAssertEqualWithAccuracy(yearFraction(start, CivilDate{ 2027, 10, 7 }, DayCount::Actual365), 1.0, 1e-12);
    XCTAssertEqualWithAccuracy(yearFraction(start, CivilDate{ 2027, 10, 7 }, DayCount::Actual360), 365.0 / 360.0, 1e-12);
    XCTAssertEqualWithAccuracy(yearFraction(start, CivilDate{ 2026, 10, 14 }, DayCount::Business252), 5.0 / 252.0, 1e-12);
    // Round trip through the day number.
    const CivilDate back = civilFromDays(daysFromCivil(CivilDate{ 2000, 2, 29 }));
    XCTAssertEqual(back.year, 2000);
    XCTAssertEqual(back.month, 2);
    XCTAssertEqual(back.day, 29);
    XCTAssertEqual(daysFromCivil(CivilDate{ 1970, 1, 1 }), 0);
}

- (void)testRateCurveInterpolation
{
    RateCurve curve({ { 1.0, 0.04 }, { 0.25, 0.05 }, { 2.0, 0.03 } });   // unsorted on purpose
    XCTAssertEqual(curve.points().size(), 3u);
    XCTAssertEqualWithAccuracy(curve.rate(0.25), 0.05, 1e-15);
    XCTAssertEqualWithAccuracy(curve.rate(0.625), 0.045, 1e-12);
    XCTAssertEqualWithAccuracy(curve.rate(1.5), 0.035, 1e-12);
    XCTAssertEqualWithAccuracy(curve.rate(0.0), 0.05, 1e-15);    // flat extrapolation
    XCTAssertEqualWithAccuracy(curve.rate(10.0), 0.03, 1e-15);
    XCTAssertEqualWithAccuracy(curve.discount(2.0), std::exp(-0.06), 1e-15);
    XCTAssertEqualWithAccuracy(curve.forwardRate(1.0, 2.0), (0.03 * 2.0 - 0.04 * 1.0) / 1.0, 1e-12);
    XCTAssertTrue(RateCurve().empty());
    XCTAssertEqualWithAccuracy(RateCurve().rate(1.0), 0.0, 0.0);
}

@end
