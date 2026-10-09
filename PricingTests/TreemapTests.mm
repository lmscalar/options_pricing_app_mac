//
//  TreemapTests.mm
//  PricingTests
//
//  Squarified treemap: areas proportional to weights, no overlaps, tiles inside the bounds.
//

#import <XCTest/XCTest.h>

#include "../OptionPricing/Pricing/Treemap.h"

#include <cmath>

@interface TreemapTests : XCTestCase
@end

@implementation TreemapTests

- (void)testAreasAreProportionalAndTilesFillTheBounds
{
    const std::vector<double> weights{ 6, 6, 4, 3, 2, 2, 1 };
    const treemap::Rect bounds{ 0, 0, 600, 400 };
    const auto rects = treemap::squarify(weights, bounds);
    XCTAssertEqual(rects.size(), weights.size());
    const double total = 24.0;
    double covered = 0.0;
    for (size_t i = 0; i < rects.size(); ++i) {
        const treemap::Rect& r = rects[i];
        XCTAssertFalse(r.empty());
        XCTAssertEqualWithAccuracy(r.area(), bounds.area() * weights[i] / total, 1e-6, @"tile %zu", i);
        XCTAssertGreaterThanOrEqual(r.x, bounds.x - 1e-9);
        XCTAssertGreaterThanOrEqual(r.y, bounds.y - 1e-9);
        XCTAssertLessThanOrEqual(r.x + r.w, bounds.x + bounds.w + 1e-6);
        XCTAssertLessThanOrEqual(r.y + r.h, bounds.y + bounds.h + 1e-6);
        covered += r.area();
    }
    XCTAssertEqualWithAccuracy(covered, bounds.area(), 1e-6);
}

- (void)testTilesDoNotOverlap
{
    std::vector<double> weights;
    for (int i = 1; i <= 25; ++i) weights.push_back(std::fmod(i * 37.0, 11.0) + 1.0);
    const auto rects = treemap::squarify(weights, { 10, 20, 500, 300 });
    for (size_t a = 0; a < rects.size(); ++a) {
        for (size_t b = a + 1; b < rects.size(); ++b) {
            const auto& r1 = rects[a];
            const auto& r2 = rects[b];
            const double ox = std::min(r1.x + r1.w, r2.x + r2.w) - std::max(r1.x, r2.x);
            const double oy = std::min(r1.y + r1.h, r2.y + r2.h) - std::max(r1.y, r2.y);
            XCTAssertTrue(ox <= 1e-6 || oy <= 1e-6, @"tiles %zu and %zu overlap by %f x %f", a, b, ox, oy);
        }
    }
}

- (void)testSquarifiedTilesAreNotExtremelyElongated
{
    const std::vector<double> weights{ 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    const auto rects = treemap::squarify(weights, { 0, 0, 400, 300 });
    double worst = 1.0;
    for (const auto& r : rects) worst = std::max(worst, std::max(r.w / r.h, r.h / r.w));
    // A plain slice-and-dice layout of these weights would exceed 8:1; squarified stays compact.
    XCTAssertLessThan(worst, 4.0, @"worst aspect ratio %f", worst);
}

- (void)testDegenerateInputs
{
    XCTAssertTrue(treemap::squarify({}, { 0, 0, 100, 100 }).empty());
    const auto zeros = treemap::squarify({ 0, -1, std::nan("") }, { 0, 0, 100, 100 });
    XCTAssertEqual(zeros.size(), 3u);
    for (const auto& r : zeros) XCTAssertTrue(r.empty());
    const auto mixed = treemap::squarify({ 0, 5, 0 }, { 0, 0, 100, 50 });
    XCTAssertTrue(mixed[0].empty());
    XCTAssertEqualWithAccuracy(mixed[1].area(), 5000.0, 1e-9);
    XCTAssertTrue(mixed[2].empty());
    XCTAssertTrue(treemap::squarify({ 1, 2 }, { 0, 0, 0, 100 })[0].empty(), @"empty bounds give empty tiles");
    const treemap::Rect shrunk = treemap::inset({ 0, 0, 10, 10 }, 6);
    XCTAssertEqual(shrunk.w, 0.0);
}

@end
