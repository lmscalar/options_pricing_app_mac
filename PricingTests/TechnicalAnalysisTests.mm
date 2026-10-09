//
//  TechnicalAnalysisTests.mm
//  PricingTests
//
//  TA-Lib wrapper: the catalogue read from the library, generic evaluation, output
//  alignment, labels and parameter normalisation.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/TechnicalAnalysis.h"

#include <cmath>

namespace {

ta::Bars syntheticBars(int n)
{
    ta::Bars bars;
    double price = 100.0;
    for (int i = 0; i < n; ++i) {
        const double move = std::sin(i * 0.37) * 1.5 + std::cos(i * 0.11) * 0.8;
        const double open = price;
        price = std::max(5.0, price + move);
        bars.open.push_back(open);
        bars.close.push_back(price);
        bars.high.push_back(std::max(open, price) + 0.6);
        bars.low.push_back(std::min(open, price) - 0.6);
        bars.volume.push_back(1000.0 + 50.0 * (i % 7));
    }
    return bars;
}

} // namespace

@interface TechnicalAnalysisTests : XCTestCase
@end

@implementation TechnicalAnalysisTests

- (void)testCatalogueIsCategorised
{
    const ta::Catalog& c = ta::catalog();
    XCTAssertGreaterThan(c.functions.size(), 100u, @"TA-Lib should expose well over a hundred functions");
    for (const char* group : { "Overlap Studies", "Momentum Indicators", "Volume Indicators", "Volatility Indicators", "Price Transform", "Cycle Indicators", "Pattern Recognition", "Statistic Functions" }) {
        XCTAssertTrue(std::find(c.groups.begin(), c.groups.end(), group) != c.groups.end(), @"missing group %s", group);
        XCTAssertFalse(c.inGroup(group).empty(), @"group %s is empty", group);
    }
    XCTAssertTrue(std::find(c.groups.begin(), c.groups.end(), "Math Operators") == c.groups.end(), @"arithmetic groups are not indicators");
    XCTAssertEqual(c.groups.front(), std::string("Overlap Studies"));
    XCTAssertNotEqual(c.find("rsi"), nullptr, @"lookup is case-insensitive");
    XCTAssertEqual(c.find("CORREL"), nullptr, @"two-series functions are left out");

    const ta::FunctionInfo* sma = c.find("SMA");
    XCTAssertNotEqual(sma, nullptr);
    XCTAssertTrue(sma->overlay);
    XCTAssertEqual(sma->params.size(), 1u);
    XCTAssertEqual(sma->params[0].name, std::string("optInTimePeriod"));
    XCTAssertEqual(sma->params[0].defaultValue, 30.0);

    const ta::FunctionInfo* macd = c.find("MACD");
    XCTAssertNotEqual(macd, nullptr);
    XCTAssertFalse(macd->overlay);
    XCTAssertEqual(macd->outputs.size(), 3u);
    XCTAssertEqual(macd->outputs[2].style, ta::OutputInfo::Style::Histogram);

    const ta::FunctionInfo* doji = c.find("CDLDOJI");
    XCTAssertNotEqual(doji, nullptr);
    XCTAssertTrue(doji->candlestick);
    XCTAssertEqual(doji->group, std::string("Pattern Recognition"));
    XCTAssertTrue(doji->outputs[0].integer);
    XCTAssertEqual(doji->outputs[0].style, ta::OutputInfo::Style::Pattern);

    XCTAssertFalse(c.search("bollinger").empty());
    XCTAssertEqual(c.search("bollinger").front()->name, std::string("BBANDS"));
}

- (void)testSmaMatchesArithmeticMean
{
    const ta::Bars bars = syntheticBars(60);
    const ta::Result r = ta::compute("SMA", bars, { { "optInTimePeriod", 10.0 } });
    XCTAssertTrue(r.ok, @"%s", r.error.c_str());
    XCTAssertEqual(r.lookback, 9);
    XCTAssertEqual(r.outputs.size(), 1u);
    XCTAssertEqual(r.outputs[0].values.size(), bars.size());
    for (int i = 0; i < 9; ++i) XCTAssertTrue(std::isnan(r.outputs[0].values[i]), @"no value before the lookback");
    for (size_t i = 9; i < bars.size(); ++i) {
        double sum = 0.0;
        for (size_t k = i + 1 - 10; k <= i; ++k) sum += bars.close[k];
        XCTAssertEqualWithAccuracy(r.outputs[0].values[i], sum / 10.0, 1e-9);
    }
}

- (void)testRsiStaysInRangeAndMacdHistogramIsDifference
{
    const ta::Bars bars = syntheticBars(200);
    const ta::Result rsi = ta::compute("RSI", bars, { { "optInTimePeriod", 14.0 } });
    XCTAssertTrue(rsi.ok, @"%s", rsi.error.c_str());
    for (double v : rsi.outputs[0].values) {
        if (!std::isnan(v)) { XCTAssertGreaterThanOrEqual(v, 0.0); XCTAssertLessThanOrEqual(v, 100.0); }
    }
    const ta::Result macd = ta::compute("MACD", bars, { { "optInFastPeriod", 12.0 }, { "optInSlowPeriod", 26.0 }, { "optInSignalPeriod", 9.0 } });
    XCTAssertTrue(macd.ok, @"%s", macd.error.c_str());
    XCTAssertEqual(macd.outputs.size(), 3u);
    for (size_t i = 0; i < bars.size(); ++i) {
        const double line = macd.outputs[0].values[i], signal = macd.outputs[1].values[i], hist = macd.outputs[2].values[i];
        if (std::isnan(line) || std::isnan(signal)) { XCTAssertTrue(std::isnan(hist)); continue; }
        XCTAssertEqualWithAccuracy(hist, line - signal, 1e-9);
    }
}

- (void)testBollingerBandsBracketTheMiddle
{
    const ta::Bars bars = syntheticBars(120);
    const ta::Result bb = ta::compute("BBANDS", bars, { { "optInTimePeriod", 20.0 }, { "optInNbDevUp", 2.0 }, { "optInNbDevDn", 2.0 } });
    XCTAssertTrue(bb.ok, @"%s", bb.error.c_str());
    XCTAssertEqual(bb.outputs.size(), 3u);
    XCTAssertTrue(bb.outputs[0].info.upperLimit);
    XCTAssertTrue(bb.outputs[2].info.lowerLimit);
    for (size_t i = 0; i < bars.size(); ++i) {
        const double up = bb.outputs[0].values[i], mid = bb.outputs[1].values[i], lo = bb.outputs[2].values[i];
        if (std::isnan(mid)) continue;
        XCTAssertGreaterThanOrEqual(up, mid);
        XCTAssertLessThanOrEqual(lo, mid);
    }
}

- (void)testCandlestickPatternOutputsAreSignedHundreds
{
    const ta::Bars bars = syntheticBars(150);
    const ta::Result r = ta::compute("CDLDOJI", bars);
    XCTAssertTrue(r.ok, @"%s", r.error.c_str());
    for (double v : r.outputs[0].values) {
        if (std::isnan(v)) continue;
        XCTAssertTrue(v == 0.0 || v == 100.0 || v == -100.0, @"pattern value %f", v);
    }
}

- (void)testParameterNormalisationAndLabels
{
    const ta::FunctionInfo* macd = ta::catalog().find("MACD");
    XCTAssertNotEqual(macd, nullptr);
    const ta::Params defaults = ta::normalizedParams(*macd, {});
    XCTAssertEqual(defaults.at("optInFastPeriod"), 12.0);
    XCTAssertEqual(defaults.at("optInSlowPeriod"), 26.0);
    XCTAssertEqual(defaults.at("optInSignalPeriod"), 9.0);
    XCTAssertEqual(ta::label(*macd, {}), std::string("MACD 12/26/9"));

    const ta::FunctionInfo* sma = ta::catalog().find("SMA");
    XCTAssertEqual(ta::label(*sma, { { "optInTimePeriod", 200.0 } }), std::string("SMA 200"));
    // Out-of-range values are clamped to TA-Lib's documented range (the minimum differs between library versions).
    XCTAssertEqual(ta::normalizedParams(*sma, { { "optInTimePeriod", -5.0 } }).at("optInTimePeriod"), sma->params[0].min);
    XCTAssertEqual(ta::normalizedParams(*sma, { { "optInTimePeriod", 1e9 } }).at("optInTimePeriod"), sma->params[0].max);

    const ta::FunctionInfo* bb = ta::catalog().find("BBANDS");
    const std::string bbLabel = ta::label(*bb, {});
    const std::string expectedPrefix = "BBANDS " + std::to_string(static_cast<int>(bb->params[0].defaultValue)) + "/2/2/";   // default period and MA-type spelling come from the library version
    XCTAssertTrue(bbLabel.rfind(expectedPrefix, 0) == 0, @"label was %s", bbLabel.c_str());
    XCTAssertEqual(ta::label(*ta::catalog().find("CDLDOJI"), {}), std::string("CDLDOJI"));

    const ta::Result bad = ta::compute("NOPE", syntheticBars(10));
    XCTAssertFalse(bad.ok);
    XCTAssertFalse(bad.error.empty());
}

@end
