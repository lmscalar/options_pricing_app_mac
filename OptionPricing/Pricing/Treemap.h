//
//  Treemap.h
//  OptionPricing
//
//  Squarified treemap layout (Bruls, Huizing, van Wijk 2000): places weighted items in a
//  rectangle so that areas are proportional to weights and tiles stay close to square.
//  Qt-free so it can be unit tested; the Sector Heatmap paints the result.
//

#pragma once

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace treemap {

struct Rect {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    double area() const { return std::max(0.0, w) * std::max(0.0, h); }
    bool empty() const { return w <= 0.0 || h <= 0.0; }
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

namespace detail {

/// Worst aspect ratio of a row of areas laid along a side of length `side`.
inline double worst(const std::vector<double>& row, double side)
{
    if (row.empty() || side <= 0.0) return 1e300;
    const double s = std::accumulate(row.begin(), row.end(), 0.0);
    const double mx = *std::max_element(row.begin(), row.end());
    const double mn = *std::min_element(row.begin(), row.end());
    if (s <= 0.0 || mn <= 0.0) return 1e300;
    return std::max(side * side * mx / (s * s), s * s / (side * side * mn));
}

/// Lays `row` (areas) along the shorter side of `remaining`, writes the tiles into `out`
/// at `indices`, and shrinks `remaining`.
inline void layoutRow(const std::vector<double>& row, const std::vector<size_t>& indices, Rect& remaining, std::vector<Rect>& out)
{
    const double s = std::accumulate(row.begin(), row.end(), 0.0);
    if (s <= 0.0) return;
    if (remaining.w >= remaining.h) {
        // Column at the left; items stacked top to bottom.
        const double colW = remaining.h > 0.0 ? s / remaining.h : 0.0;
        double y = remaining.y;
        for (size_t k = 0; k < row.size(); ++k) {
            const double hh = colW > 0.0 ? row[k] / colW : 0.0;
            out[indices[k]] = Rect{ remaining.x, y, colW, hh };
            y += hh;
        }
        remaining.x += colW;
        remaining.w -= colW;
    } else {
        // Strip at the top; items left to right.
        const double rowH = remaining.w > 0.0 ? s / remaining.w : 0.0;
        double x = remaining.x;
        for (size_t k = 0; k < row.size(); ++k) {
            const double ww = rowH > 0.0 ? row[k] / rowH : 0.0;
            out[indices[k]] = Rect{ x, remaining.y, ww, rowH };
            x += ww;
        }
        remaining.y += rowH;
        remaining.h -= rowH;
    }
}

} // namespace detail

/// Lays out `weights` inside `bounds`; the returned rectangles are in the same order as
/// the weights. Non-positive weights get empty rectangles.
inline std::vector<Rect> squarify(const std::vector<double>& weights, const Rect& bounds)
{
    std::vector<Rect> out(weights.size());
    if (bounds.empty()) return out;
    std::vector<size_t> order;
    double total = 0.0;
    for (size_t i = 0; i < weights.size(); ++i) {
        if (std::isfinite(weights[i]) && weights[i] > 0.0) { order.push_back(i); total += weights[i]; }
    }
    if (order.empty() || total <= 0.0) return out;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return weights[a] > weights[b]; });
    const double scale = bounds.area() / total;

    Rect remaining = bounds;
    std::vector<double> row;
    std::vector<size_t> rowIndices;
    for (size_t idx : order) {
        const double area = weights[idx] * scale;
        const double side = std::min(remaining.w, remaining.h);
        std::vector<double> candidate = row;
        candidate.push_back(area);
        if (row.empty() || detail::worst(candidate, side) <= detail::worst(row, side)) {
            row.push_back(area);
            rowIndices.push_back(idx);
        } else {
            detail::layoutRow(row, rowIndices, remaining, out);
            row.assign(1, area);
            rowIndices.assign(1, idx);
        }
    }
    detail::layoutRow(row, rowIndices, remaining, out);
    return out;
}

/// Shrinks a rectangle by `inset` on every side (never below zero size).
inline Rect inset(const Rect& r, double inset)
{
    return Rect{ r.x + inset, r.y + inset, std::max(0.0, r.w - 2.0 * inset), std::max(0.0, r.h - 2.0 * inset) };
}

} // namespace treemap
