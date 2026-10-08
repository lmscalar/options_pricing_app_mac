//
//  RateCurve.h
//  OptionPricing
//
//  Simple zero-rate term structure: continuously compounded zero rates at a set of
//  tenors, linearly interpolated in rate with flat extrapolation beyond the ends.
//

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace pricing {

struct RatePoint {
    double tenor = 0.0;   ///< years
    double rate = 0.0;    ///< continuously compounded, decimal
};

class RateCurve {
public:
    RateCurve() = default;
    explicit RateCurve(std::vector<RatePoint> points) { setPoints(std::move(points)); }

    void setPoints(std::vector<RatePoint> points)
    {
        std::sort(points.begin(), points.end(), [](const RatePoint& a, const RatePoint& b) { return a.tenor < b.tenor; });
        m_points.clear();
        for (const RatePoint& p : points) {
            if (p.tenor >= 0.0 && std::isfinite(p.tenor) && std::isfinite(p.rate)) {
                if (!m_points.empty() && std::fabs(m_points.back().tenor - p.tenor) < 1e-12) {
                    m_points.back() = p;   // later duplicate wins
                } else {
                    m_points.push_back(p);
                }
            }
        }
    }

    const std::vector<RatePoint>& points() const { return m_points; }
    bool empty() const { return m_points.empty(); }

    /// Zero rate for the given maturity in years.
    double rate(double maturity) const
    {
        if (m_points.empty()) {
            return 0.0;
        }
        if (maturity <= m_points.front().tenor) {
            return m_points.front().rate;
        }
        if (maturity >= m_points.back().tenor) {
            return m_points.back().rate;
        }
        const auto upper = std::upper_bound(m_points.begin(), m_points.end(), maturity,
                                            [](double t, const RatePoint& p) { return t < p.tenor; });
        const RatePoint& b = *upper;
        const RatePoint& a = *(upper - 1);
        const double w = (maturity - a.tenor) / (b.tenor - a.tenor);
        return a.rate + w * (b.rate - a.rate);
    }

    /// Discount factor exp(-r(T) * T).
    double discount(double maturity) const
    {
        return std::exp(-rate(maturity) * maturity);
    }

    /// Continuously compounded forward rate between two maturities.
    double forwardRate(double from, double to) const
    {
        if (to <= from) {
            return rate(to);
        }
        return (rate(to) * to - rate(from) * from) / (to - from);
    }

private:
    std::vector<RatePoint> m_points;
};

} // namespace pricing
