//
//  Scenario.h
//  OptionPricing
//
//  Two-dimensional scenario grids for a position: a chosen metric (P&L, value or a
//  Greek) evaluated across spot on one axis and either volatility or elapsed time on
//  the other. The UI renders the result as a heatmap table.
//

#pragma once

#include "Strategy.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pricing {

enum class ScenarioMetric {
    Pnl,
    Value,
    Delta,
    Gamma,
    Vega,
    Theta,
    Rho,
    Vanna,
    Charm,
};

enum class ScenarioAxis {
    Volatility,   ///< columns are absolute volatility shifts in percentage points
    Time,         ///< columns are days elapsed
};

inline const char* scenarioMetricName(ScenarioMetric m)
{
    switch (m) {
    case ScenarioMetric::Pnl:   return "P&L";
    case ScenarioMetric::Value: return "Position value";
    case ScenarioMetric::Delta: return "Delta";
    case ScenarioMetric::Gamma: return "Gamma";
    case ScenarioMetric::Vega:  return "Vega";
    case ScenarioMetric::Theta: return "Theta";
    case ScenarioMetric::Rho:   return "Rho";
    case ScenarioMetric::Vanna: return "Vanna";
    case ScenarioMetric::Charm: return "Charm";
    }
    return "";
}

struct ScenarioRequest {
    ScenarioMetric metric = ScenarioMetric::Pnl;
    ScenarioAxis axis = ScenarioAxis::Volatility;
    double spotRangePercent = 20.0;   ///< rows span spot * (1 -/+ range)
    int rows = 13;
    int columns = 7;
    double volRangePoints = 10.0;     ///< columns span vol -/+ this many percentage points
    double maxDays = 0.0;             ///< columns span 0..maxDays (0 = up to the first expiry)
};

struct ScenarioGrid {
    std::vector<double> spots;          ///< row headers
    std::vector<double> columnValues;   ///< vol shift in points, or days elapsed
    std::vector<std::vector<double>> cells;   ///< cells[row][column]
    double minValue = 0.0;
    double maxValue = 0.0;
};

inline double scenarioMetricValue(const Position& pos, const Market& market, ScenarioMetric metric,
                                  double spot, double elapsed)
{
    switch (metric) {
    case ScenarioMetric::Pnl:   return positionPnl(pos, market, spot, elapsed);
    case ScenarioMetric::Value: return positionValue(pos, market, spot, elapsed);
    default: break;
    }
    const Greeks g = positionGreeks(pos, market, spot, elapsed);
    switch (metric) {
    case ScenarioMetric::Delta: return g.delta;
    case ScenarioMetric::Gamma: return g.gamma;
    case ScenarioMetric::Vega:  return g.vega;
    case ScenarioMetric::Theta: return g.theta;
    case ScenarioMetric::Rho:   return g.rho;
    case ScenarioMetric::Vanna: return g.vanna;
    case ScenarioMetric::Charm: return g.charm;
    default: return 0.0;
    }
}

inline ScenarioGrid buildScenarioGrid(const Position& pos, const Market& market, const ScenarioRequest& req)
{
    ScenarioGrid grid;
    const int rows = std::max(req.rows, 2);
    const int cols = std::max(req.columns, 1);

    const double range = std::max(req.spotRangePercent, 0.1) / 100.0;
    for (int r = 0; r < rows; ++r) {
        // Highest spot first so the table reads like a price ladder.
        const double factor = 1.0 + range - 2.0 * range * r / (rows - 1);
        grid.spots.push_back(std::max(0.01, market.spot * factor));
    }

    double maxDays = req.maxDays;
    if (req.axis == ScenarioAxis::Time && maxDays <= 0.0) {
        maxDays = std::max(1.0, std::floor(earliestExpiry(pos) * market.dayBasis));
    }
    for (int c = 0; c < cols; ++c) {
        if (req.axis == ScenarioAxis::Volatility) {
            const double shift = cols == 1 ? 0.0 : -req.volRangePoints + 2.0 * req.volRangePoints * c / (cols - 1);
            grid.columnValues.push_back(shift);
        } else {
            const double days = cols == 1 ? 0.0 : maxDays * c / (cols - 1);
            grid.columnValues.push_back(days);
        }
    }

    grid.minValue = INFINITY;
    grid.maxValue = -INFINITY;
    grid.cells.assign(static_cast<size_t>(rows), std::vector<double>(static_cast<size_t>(cols), 0.0));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            Market m = market;
            double elapsed = 0.0;
            if (req.axis == ScenarioAxis::Volatility) {
                m.volatility = std::max(0.0001, market.volatility + grid.columnValues[static_cast<size_t>(c)] / 100.0);
            } else {
                elapsed = grid.columnValues[static_cast<size_t>(c)] / market.dayBasis;
            }
            const double value = scenarioMetricValue(pos, m, req.metric, grid.spots[static_cast<size_t>(r)], elapsed);
            grid.cells[static_cast<size_t>(r)][static_cast<size_t>(c)] = value;
            if (std::isfinite(value)) {
                grid.minValue = std::min(grid.minValue, value);
                grid.maxValue = std::max(grid.maxValue, value);
            }
        }
    }
    if (!std::isfinite(grid.minValue)) { grid.minValue = 0.0; grid.maxValue = 0.0; }
    return grid;
}

} // namespace pricing
