//
//  VolatilityTests.mm
//  PricingTests
//
//  Realized volatility estimators, the volatility cone, EWMA and GARCH fitting.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Volatility.h"

#include <random>

using namespace pricing;

namespace {

/// Bars with a constant high/low ratio exp(x), open = close = geometric mid, and a flat
/// close path, so every range estimator has a closed form.
std::vector<DailyBar> rangeBars(int count, double x, double close = 100.0)
{
    std::vector<DailyBar> bars;
    for (int i = 0; i < count; ++i) {
        DailyBar b;
        b.date = "2026-01-" + std::to_string(1 + i % 28);
        b.open = close;
        b.close = close;
        b.high = close * std::exp(0.5 * x);
        b.low = close * std::exp(-0.5 * x);
        b.volume = 1000;
        bars.push_back(b);
    }
    return bars;
}

/// Simulates a GARCH(1,1) return path with Gaussian shocks and a fixed seed.
std::vector<double> simulateGarch(size_t n, double omega, double alpha, double beta, double gamma, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<double> r;
    r.reserve(n);
    double s2 = omega / (1.0 - alpha - beta - 0.5 * gamma);
    for (size_t i = 0; i < n + 500; ++i) {   // burn-in
        const double e = std::sqrt(s2) * z(rng);
        if (i >= 500) r.push_back(e);
        s2 = omega + alpha * e * e + (e < 0.0 ? gamma * e * e : 0.0) + beta * s2;
    }
    return r;
}

} // namespace

@interface VolatilityTests : XCTestCase
@end

@implementation VolatilityTests

- (void)testCloseToCloseMatchesSampleStandardDeviation
{
    // Alternating ±1% log returns: mean zero, sample variance n/(n−1) · 1e-4.
    std::vector<DailyBar> bars;
    double close = 100.0;
    for (int i = 0; i <= 20; ++i) {
        DailyBar b;
        b.open = b.high = b.low = b.close = close;
        bars.push_back(b);
        close *= std::exp(i % 2 == 0 ? 0.01 : -0.01);
    }
    const double expectedVar = 20.0 / 19.0 * 1e-4;
    XCTAssertEqualWithAccuracy(realizedVariance(bars, RealizedEstimator::CloseToClose, 1, bars.size()), expectedVar, 1e-12);
    XCTAssertEqualWithAccuracy(realizedVol(bars, RealizedEstimator::CloseToClose, 20), std::sqrt(expectedVar * 252.0), 1e-10);
    XCTAssertTrue(std::isnan(realizedVol(bars, RealizedEstimator::CloseToClose, 25)));   // not enough history
}

- (void)testRangeEstimatorsHaveClosedFormsOnConstantRangeBars
{
    const double x = 0.02;
    const std::vector<DailyBar> bars = rangeBars(30, x);
    XCTAssertEqualWithAccuracy(realizedVariance(bars, RealizedEstimator::Parkinson, 1, bars.size()), x * x / (4.0 * std::log(2.0)), 1e-12);
    XCTAssertEqualWithAccuracy(realizedVariance(bars, RealizedEstimator::GarmanKlass, 1, bars.size()), 0.5 * x * x, 1e-12);
    // Rogers-Satchell with O = C at the geometric mid: ln(H/C)·ln(H/O) + ln(L/C)·ln(L/O) = x²/4 + x²/4.
    XCTAssertEqualWithAccuracy(realizedVariance(bars, RealizedEstimator::RogersSatchell, 1, bars.size()), 0.5 * x * x, 1e-12);
    // Yang-Zhang: zero overnight and open-to-close variance leaves (1 − k) · RS.
    const double n = bars.size() - 1.0;
    const double k = 0.34 / (1.34 + (n + 1.0) / (n - 1.0));
    XCTAssertEqualWithAccuracy(realizedVariance(bars, RealizedEstimator::YangZhang, 1, bars.size()), (1.0 - k) * 0.5 * x * x, 1e-12);
    // Close-to-close sees a flat close path.
    XCTAssertEqualWithAccuracy(realizedVariance(bars, RealizedEstimator::CloseToClose, 1, bars.size()), 0.0, 1e-15);
}

- (void)testRollingSeriesAndConeAreConsistent
{
    std::vector<double> returns = simulateGarch(600, 2e-6, 0.08, 0.90, 0.0, 7u);
    std::vector<DailyBar> bars;
    double close = 100.0;
    for (size_t i = 0; i < returns.size(); ++i) {
        DailyBar b;
        b.date = "d" + std::to_string(i);
        b.open = b.high = b.low = b.close = close;
        bars.push_back(b);
        close *= std::exp(returns[i]);
    }
    const auto series = rollingRealizedVol(bars, RealizedEstimator::CloseToClose, 20);
    XCTAssertEqual(series.size(), bars.size() - 20);
    XCTAssertEqualWithAccuracy(series.back().vol, realizedVol(bars, RealizedEstimator::CloseToClose, 20), 1e-12);
    XCTAssertEqual(series.back().date, bars.back().date);

    const auto cone = volCone(bars, RealizedEstimator::CloseToClose, { 10, 20, 60, 120 });
    XCTAssertEqual(cone.size(), 4u);
    for (const ConeRow& row : cone) {
        XCTAssertLessThanOrEqual(row.min, row.p10);
        XCTAssertLessThanOrEqual(row.p10, row.p25);
        XCTAssertLessThanOrEqual(row.p25, row.median);
        XCTAssertLessThanOrEqual(row.median, row.p75);
        XCTAssertLessThanOrEqual(row.p75, row.p90);
        XCTAssertLessThanOrEqual(row.p90, row.max);
        XCTAssertGreaterThanOrEqual(row.current, row.min);
        XCTAssertLessThanOrEqual(row.current, row.max);
        XCTAssertGreaterThanOrEqual(row.percentile, 0.0);
        XCTAssertLessThanOrEqual(row.percentile, 1.0);
    }
    XCTAssertEqualWithAccuracy(cone[1].current, series.back().vol, 1e-12);
}

- (void)testEwmaRecursion
{
    const std::vector<double> r = { 0.01, -0.02, 0.015 };
    const EwmaResult e = ewmaVariance(r, 0.94);
    const double var0 = detail::sampleVariance(r);
    XCTAssertEqualWithAccuracy(e.variance[0], var0, 1e-15);
    XCTAssertEqualWithAccuracy(e.variance[1], 0.94 * var0 + 0.06 * 1e-4, 1e-15);
    XCTAssertEqualWithAccuracy(e.variance[2], 0.94 * e.variance[1] + 0.06 * 4e-4, 1e-15);
    XCTAssertEqualWithAccuracy(e.nextVariance, 0.94 * e.variance[2] + 0.06 * 2.25e-4, 1e-15);
    XCTAssertEqualWithAccuracy(e.currentVol(), std::sqrt(e.nextVariance * 252.0), 1e-12);
}

- (void)testGarchRecoversSimulatedParameters
{
    const double omega = 2e-6, alpha = 0.08, beta = 0.90;   // long-run daily var 1e-4 → 15.9% annual
    const std::vector<double> r = simulateGarch(5000, omega, alpha, beta, 0.0, 12345u);
    const GarchFit fit = fitGarch(r, GarchModel::Garch11);
    XCTAssertTrue(fit.converged);
    XCTAssertEqual(fit.observations, 5000);
    XCTAssertEqualWithAccuracy(fit.alpha, alpha, 0.03);
    XCTAssertEqualWithAccuracy(fit.beta, beta, 0.04);
    XCTAssertEqualWithAccuracy(fit.persistence(), alpha + beta, 0.02);
    XCTAssertEqualWithAccuracy(fit.unconditionalVol(), std::sqrt(omega / (1.0 - alpha - beta) * 252.0), 0.03);
    XCTAssertTrue(std::isfinite(fit.logLikelihood));
    XCTAssertGreaterThan(fit.aic, 0.0 - 1e9);
    XCTAssertEqual(fit.conditionalVariance.size(), r.size());

    // The fitted likelihood must beat the true parameters' likelihood only marginally or
    // exceed it: the optimiser cannot do worse than the truth by much.
    std::vector<double> eps;
    for (double x : r) eps.push_back((x - fit.mean) * 100.0);
    const double trueNegLL = detail::garchNegLogLikelihood(eps, omega * 1e4, alpha, beta, 0.0, detail::sampleVariance(eps));
    XCTAssertGreaterThanOrEqual(fit.logLikelihood, -trueNegLL + r.size() * std::log(100.0) - 1.0);
}

- (void)testGarchForecastConvergesToLongRunVariance
{
    const std::vector<double> r = simulateGarch(3000, 2e-6, 0.08, 0.90, 0.0, 99u);
    const GarchFit fit = fitGarch(r, GarchModel::Garch11);
    XCTAssertTrue(fit.converged);
    const auto var = fit.forecastVariance(2000);
    XCTAssertEqual(var.size(), 2000u);
    XCTAssertEqualWithAccuracy(var.front(), fit.nextVariance, 1e-18);
    XCTAssertEqualWithAccuracy(var.back(), fit.unconditionalVariance(), fit.unconditionalVariance() * 1e-3);
    // Term vol lies between the one-day vol and the long-run vol and converges towards it.
    const auto term = fit.forecastTermVol(2000);
    const double lo = std::min(fit.currentVol(), fit.unconditionalVol());
    const double hi = std::max(fit.currentVol(), fit.unconditionalVol());
    XCTAssertGreaterThanOrEqual(term[20], lo - 1e-9);
    XCTAssertLessThanOrEqual(term[20], hi + 1e-9);
    XCTAssertEqualWithAccuracy(term.back(), fit.unconditionalVol(), 0.01);
    XCTAssertGreaterThan(fit.halfLife(), 5.0);
}

- (void)testGjrGarchDetectsLeverage
{
    const std::vector<double> r = simulateGarch(6000, 2e-6, 0.03, 0.90, 0.10, 2024u);
    const GarchFit gjr = fitGarch(r, GarchModel::GjrGarch11);
    XCTAssertTrue(gjr.converged);
    XCTAssertGreaterThan(gjr.gamma, 0.04);                 // asymmetry is picked up
    XCTAssertEqualWithAccuracy(gjr.persistence(), 0.98, 0.02);
    const GarchFit sym = fitGarch(r, GarchModel::Garch11);
    XCTAssertGreaterThan(gjr.logLikelihood, sym.logLikelihood);   // the extra parameter must not hurt the fit
    XCTAssertEqual(gjr.parameterCount(), 4);
}

- (void)testGarchNeedsEnoughData
{
    const std::vector<double> r(30, 0.001);
    const GarchFit fit = fitGarch(r);
    XCTAssertFalse(fit.converged);
    XCTAssertTrue(fit.conditionalVariance.empty());
    XCTAssertTrue(fit.forecastVariance(0).empty());
}

@end
