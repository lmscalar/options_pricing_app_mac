//
//  VolSurface.h
//  OptionPricing
//
//  Turns option-chain quotes into implied volatilities, checks each expiry for static
//  arbitrage, fits a raw SVI smile per expiry and interpolates between expiries in
//  total variance to form a surface.
//

#pragma once

#include "BlackScholes.h"
#include "Csv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pricing {

/// A chain quote with its solved implied volatility.
struct ImpliedQuote {
    ChainQuote quote;
    double impliedVol = 0.0;
    bool solved = false;
    std::string note;           ///< why the solve failed, if it did
    double logMoneyness = 0.0;  ///< ln(K / F)
};

struct ChainIssue {
    double strike = 0.0;
    std::string message;
};

/// Raw SVI parameters: w(k) = a + b * (rho * (k - m) + sqrt((k - m)^2 + sigma^2)).
struct SviParams {
    double a = 0.0;
    double b = 0.0;
    double rho = 0.0;
    double m = 0.0;
    double sigma = 0.1;

    double totalVariance(double k) const
    {
        const double x = k - m;
        return a + b * (rho * x + std::sqrt(x * x + sigma * sigma));
    }
};

/// Implied vol from an SVI slice at log-moneyness k.
inline double sviVol(const SviParams& s, double k, double maturity)
{
    const double w = std::max(s.totalVariance(k), 1e-12);
    return std::sqrt(w / maturity);
}

struct ExpirySlice {
    double maturity = 0.0;
    std::string expiryDate;      ///< ISO date when the quotes carried one
    int daysToExpiry = 0;        ///< calendar days to expiry (derived from maturity when no date is known)
    double forward = 0.0;
    double rate = 0.0;
    std::vector<ImpliedQuote> calls;
    std::vector<ImpliedQuote> puts;
    std::vector<ChainIssue> issues;
    SviParams svi;
    bool fitted = false;
    double fitRmse = 0.0;        ///< root-mean-square error of the fit, in vol (decimal)
    int pointsUsed = 0;

    /// Implied vol from the fitted smile at the given strike.
    double smileVol(double strike) const
    {
        if (!fitted || maturity <= 0.0 || strike <= 0.0) return 0.0;
        return sviVol(svi, std::log(strike / forward), maturity);
    }

    double atmVol() const { return fitted ? sviVol(svi, 0.0, maturity) : 0.0; }
};

namespace detail {

/// Nelder-Mead simplex minimiser for small unconstrained problems.
template <size_t N>
inline std::array<double, N> nelderMead(const std::function<double(const std::array<double, N>&)>& f,
                                        std::array<double, N> start, const std::array<double, N>& scale,
                                        int maxIterations = 4000, double tolerance = 1e-12)
{
    std::array<std::array<double, N>, N + 1> simplex;
    std::array<double, N + 1> values;
    simplex[0] = start;
    for (size_t i = 0; i < N; ++i) {
        simplex[i + 1] = start;
        simplex[i + 1][i] += scale[i];
    }
    for (size_t i = 0; i <= N; ++i) values[i] = f(simplex[i]);

    for (int iter = 0; iter < maxIterations; ++iter) {
        std::array<size_t, N + 1> order;
        for (size_t i = 0; i <= N; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return values[x] < values[y]; });
        const size_t best = order[0], worst = order[N], secondWorst = order[N - 1];

        if (std::fabs(values[worst] - values[best]) < tolerance * (1.0 + std::fabs(values[best]))) {
            break;
        }

        std::array<double, N> centroid{};
        for (size_t i = 0; i <= N; ++i) {
            if (i == worst) continue;
            for (size_t d = 0; d < N; ++d) centroid[d] += simplex[i][d] / N;
        }
        auto along = [&](double t) {
            std::array<double, N> p;
            for (size_t d = 0; d < N; ++d) p[d] = centroid[d] + t * (simplex[worst][d] - centroid[d]);
            return p;
        };

        const auto reflected = along(-1.0);
        const double fr = f(reflected);
        if (fr < values[best]) {
            const auto expanded = along(-2.0);
            const double fe = f(expanded);
            if (fe < fr) { simplex[worst] = expanded; values[worst] = fe; }
            else { simplex[worst] = reflected; values[worst] = fr; }
        } else if (fr < values[secondWorst]) {
            simplex[worst] = reflected; values[worst] = fr;
        } else {
            const auto contracted = along(fr < values[worst] ? -0.5 : 0.5);
            const double fc = f(contracted);
            if (fc < std::min(fr, values[worst])) {
                simplex[worst] = contracted; values[worst] = fc;
            } else {
                for (size_t i = 0; i <= N; ++i) {
                    if (i == best) continue;
                    for (size_t d = 0; d < N; ++d) simplex[i][d] = simplex[best][d] + 0.5 * (simplex[i][d] - simplex[best][d]);
                    values[i] = f(simplex[i]);
                }
            }
        }
    }
    size_t bestIndex = 0;
    for (size_t i = 1; i <= N; ++i) if (values[i] < values[bestIndex]) bestIndex = i;
    return simplex[bestIndex];
}

} // namespace detail

/// Fits raw SVI to (log-moneyness, implied vol) points for one expiry. Returns false if
/// there are too few points.
inline bool fitSvi(const std::vector<std::pair<double, double>>& kAndVol, double maturity, SviParams& out, double& rmse)
{
    if (kAndVol.size() < 3 || maturity <= 0.0) {
        return false;
    }
    double minW = INFINITY, meanK = 0.0;
    for (const auto& [k, vol] : kAndVol) {
        minW = std::min(minW, vol * vol * maturity);
        meanK += k / static_cast<double>(kAndVol.size());
    }

    // Objective: mean squared vol error with penalties that keep the parameters in the
    // region where the slice is free of butterfly arbitrage.
    auto objective = [&](const std::array<double, 5>& p) {
        SviParams s{ p[0], p[1], p[2], p[3], p[4] };
        double penalty = 0.0;
        if (s.b < 0.0) penalty += 1e3 * s.b * s.b;
        if (std::fabs(s.rho) >= 0.999) penalty += 1e3 * (std::fabs(s.rho) - 0.999 + 1e-3);
        if (s.sigma <= 1e-4) penalty += 1e3 * (1e-4 - s.sigma + 1e-4);
        const double minVariance = s.a + s.b * s.sigma * std::sqrt(std::max(0.0, 1.0 - s.rho * s.rho));
        if (minVariance < 0.0) penalty += 1e3 * minVariance * minVariance;
        if (s.b * (1.0 + std::fabs(s.rho)) > 4.0 / maturity) penalty += 10.0 * (s.b * (1.0 + std::fabs(s.rho)) - 4.0 / maturity);

        double error = 0.0;
        for (const auto& [k, vol] : kAndVol) {
            const double w = s.totalVariance(k);
            const double modelVol = w > 0.0 ? std::sqrt(w / maturity) : 0.0;
            const double diff = modelVol - vol;
            error += diff * diff;
        }
        return error / static_cast<double>(kAndVol.size()) + penalty;
    };

    const std::array<double, 5> start{ std::max(minW * 0.5, 1e-6), 0.1, -0.3, meanK, 0.1 };
    const std::array<double, 5> scale{ std::max(minW * 0.25, 1e-4), 0.1, 0.2, 0.1, 0.1 };
    std::array<double, 5> best = detail::nelderMead<5>(objective, start, scale);
    best = detail::nelderMead<5>(objective, best, { scale[0] * 0.1, 0.02, 0.05, 0.02, 0.02 });

    out = SviParams{ best[0], best[1], std::clamp(best[2], -0.999, 0.999), best[3], std::max(best[4], 1e-4) };
    double error = 0.0;
    for (const auto& [k, vol] : kAndVol) {
        const double diff = sviVol(out, k, maturity) - vol;
        error += diff * diff;
    }
    rmse = std::sqrt(error / static_cast<double>(kAndVol.size()));
    return std::isfinite(rmse);
}

/// Market inputs needed to imply volatilities from a chain.
struct ChainMarket {
    Model model = Model::BlackScholesMerton;
    double spot = 100.0;
    double riskFreeRate = 0.05;
    double dividendYield = 0.0;
};

namespace detail {

/// Flags monotonicity and convexity violations in a strip of mid prices for one type.
inline void checkStrip(std::vector<ImpliedQuote>& strip, OptionType type, double discount, std::vector<ChainIssue>& issues)
{
    std::sort(strip.begin(), strip.end(), [](const ImpliedQuote& a, const ImpliedQuote& b) { return a.quote.strike < b.quote.strike; });
    const char* name = type == OptionType::Call ? "Call" : "Put";
    for (size_t i = 1; i < strip.size(); ++i) {
        const double k0 = strip[i - 1].quote.strike, k1 = strip[i].quote.strike;
        const double p0 = strip[i - 1].quote.mid, p1 = strip[i].quote.mid;
        if (k1 <= k0) continue;
        const double slope = (p1 - p0) / (k1 - k0);
        if (type == OptionType::Call) {
            if (slope > 1e-9) issues.push_back({ k1, std::string(name) + " price rises with strike (" + std::to_string(k0) + " to " + std::to_string(k1) + ")." });
            if (slope < -discount - 1e-9) issues.push_back({ k1, std::string(name) + " spread exceeds the discounted strike difference." });
        } else {
            if (slope < -1e-9) issues.push_back({ k1, std::string(name) + " price falls with strike (" + std::to_string(k0) + " to " + std::to_string(k1) + ")." });
            if (slope > discount + 1e-9) issues.push_back({ k1, std::string(name) + " spread exceeds the discounted strike difference." });
        }
        if (i >= 2) {
            const double kPrev = strip[i - 2].quote.strike, pPrev = strip[i - 2].quote.mid;
            const double slopePrev = (p0 - pPrev) / (k0 - kPrev);
            // Convexity: slopes must be non-decreasing in strike (butterfly >= 0).
            if (slopePrev > slope + 1e-9) {
                issues.push_back({ k0, std::string(name) + " butterfly centred at " + std::to_string(k0) + " has negative value." });
            }
        }
    }
}

} // namespace detail

/// Groups quotes by expiry, solves implied vols, checks for arbitrage and fits SVI.
inline std::vector<ExpirySlice> buildExpirySlices(const std::vector<ChainQuote>& quotes, const ChainMarket& market)
{
    std::map<long long, ExpirySlice> byExpiry;   // key: maturity in whole days to merge near-equal values
    for (const ChainQuote& q : quotes) {
        if (q.maturity <= 0.0 || q.strike <= 0.0) continue;
        const long long key = static_cast<long long>(std::llround(q.maturity * 365.0 * 10.0));
        ExpirySlice& slice = byExpiry[key];
        if (slice.maturity == 0.0) {
            slice.maturity = q.maturity;
            slice.expiryDate = q.expiryDate;
            slice.daysToExpiry = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::llround(q.maturity * 365.0));
            slice.rate = market.riskFreeRate;
            const double carry = market.model == Model::Black76 ? 0.0 : market.riskFreeRate - market.dividendYield;
            slice.forward = market.spot * std::exp(carry * q.maturity);
        }

        Inputs in;
        in.model = market.model;
        in.spot = market.spot;
        in.strike = q.strike;
        in.riskFreeRate = market.riskFreeRate;
        in.dividendYield = market.dividendYield;
        in.maturity = slice.maturity;

        ImpliedQuote iq;
        iq.quote = q;
        iq.logMoneyness = std::log(q.strike / slice.forward);
        const ImpliedVolResult iv = impliedVolatility(in, q.type, q.mid);
        if (iv.status == ImpliedVolResult::Status::Converged) {
            iq.solved = true;
            iq.impliedVol = iv.volatility;
        } else {
            switch (iv.status) {
            case ImpliedVolResult::Status::BelowLowerBound: iq.note = "below intrinsic"; break;
            case ImpliedVolResult::Status::AboveUpperBound: iq.note = "above no-arbitrage bound"; break;
            case ImpliedVolResult::Status::NotConverged:    iq.note = "did not converge"; break;
            default:                                        iq.note = "invalid"; break;
            }
        }
        (q.type == OptionType::Call ? slice.calls : slice.puts).push_back(iq);
    }

    std::vector<ExpirySlice> slices;
    for (auto& [key, slice] : byExpiry) {
        const double discount = std::exp(-slice.rate * slice.maturity);
        detail::checkStrip(slice.calls, OptionType::Call, discount, slice.issues);
        detail::checkStrip(slice.puts, OptionType::Put, discount, slice.issues);

        // Put-call parity at common strikes: C - P should equal (F - K) * discount.
        for (const ImpliedQuote& c : slice.calls) {
            for (const ImpliedQuote& p : slice.puts) {
                if (std::fabs(c.quote.strike - p.quote.strike) > 1e-9) continue;
                const double parity = (slice.forward - c.quote.strike) * discount;
                const double observed = c.quote.mid - p.quote.mid;
                const double spreadAllowance = 0.5 * (std::max(0.0, c.quote.ask - c.quote.bid) + std::max(0.0, p.quote.ask - p.quote.bid));
                if (std::fabs(observed - parity) > std::max(spreadAllowance, 0.01 * market.spot * 0.01 + 0.02)) {
                    slice.issues.push_back({ c.quote.strike, "Put-call parity gap of " + std::to_string(observed - parity) + " at strike " + std::to_string(c.quote.strike) + "." });
                }
            }
        }

        // Fit SVI to out-of-the-money quotes (puts below the forward, calls above),
        // which carry the cleanest volatility information.
        std::vector<std::pair<double, double>> points;
        for (const ImpliedQuote& iq : slice.calls) {
            if (iq.solved && iq.quote.strike >= slice.forward) points.emplace_back(iq.logMoneyness, iq.impliedVol);
        }
        for (const ImpliedQuote& iq : slice.puts) {
            if (iq.solved && iq.quote.strike < slice.forward) points.emplace_back(iq.logMoneyness, iq.impliedVol);
        }
        if (points.size() < 3) {
            // Fall back to every solved quote.
            points.clear();
            for (const ImpliedQuote& iq : slice.calls) if (iq.solved) points.emplace_back(iq.logMoneyness, iq.impliedVol);
            for (const ImpliedQuote& iq : slice.puts) if (iq.solved) points.emplace_back(iq.logMoneyness, iq.impliedVol);
        }
        slice.pointsUsed = static_cast<int>(points.size());
        slice.fitted = fitSvi(points, slice.maturity, slice.svi, slice.fitRmse);
        slices.push_back(std::move(slice));
    }
    return slices;
}

/// A collection of fitted slices with interpolation across maturity in total variance.
class VolSurface {
public:
    VolSurface() = default;
    explicit VolSurface(std::vector<ExpirySlice> slices) { setSlices(std::move(slices)); }

    void setSlices(std::vector<ExpirySlice> slices)
    {
        m_slices.clear();
        for (ExpirySlice& s : slices) {
            if (s.fitted) m_slices.push_back(std::move(s));
        }
        std::sort(m_slices.begin(), m_slices.end(), [](const ExpirySlice& a, const ExpirySlice& b) { return a.maturity < b.maturity; });
    }

    const std::vector<ExpirySlice>& slices() const { return m_slices; }
    bool empty() const { return m_slices.empty(); }

    /// Implied vol for a strike and maturity. Between fitted expiries the total variance
    /// at constant log-moneyness is interpolated linearly; outside them it is held flat
    /// in vol.
    double impliedVol(double strike, double maturity, const ChainMarket& market) const
    {
        if (m_slices.empty() || strike <= 0.0 || maturity <= 0.0) return 0.0;
        const double carry = market.model == Model::Black76 ? 0.0 : market.riskFreeRate - market.dividendYield;
        const double forward = market.spot * std::exp(carry * maturity);
        const double k = std::log(strike / forward);

        if (maturity <= m_slices.front().maturity) return sviVol(m_slices.front().svi, k, m_slices.front().maturity);
        if (maturity >= m_slices.back().maturity) return sviVol(m_slices.back().svi, k, m_slices.back().maturity);

        const auto upper = std::upper_bound(m_slices.begin(), m_slices.end(), maturity,
                                            [](double t, const ExpirySlice& s) { return t < s.maturity; });
        const ExpirySlice& hi = *upper;
        const ExpirySlice& lo = *(upper - 1);
        const double wLo = std::max(lo.svi.totalVariance(k), 1e-12);
        const double wHi = std::max(hi.svi.totalVariance(k), 1e-12);
        const double t = (maturity - lo.maturity) / (hi.maturity - lo.maturity);
        const double w = wLo + t * (std::max(wHi, wLo) - wLo);   // total variance must not decrease
        return std::sqrt(w / maturity);
    }

private:
    std::vector<ExpirySlice> m_slices;
};

/// Builds a synthetic chain from a smile model, useful for demonstrations and tests.
/// Skew is expressed as vol change per unit of log-moneyness; smile as curvature.
inline std::vector<ChainQuote> syntheticChain(const ChainMarket& market, const std::vector<double>& maturities,
                                              double atmVol, double skew, double smile, double strikeStep, int strikesEachSide,
                                              double relativeSpread = 0.04)
{
    std::vector<ChainQuote> quotes;
    for (double T : maturities) {
        const double carry = market.model == Model::Black76 ? 0.0 : market.riskFreeRate - market.dividendYield;
        const double forward = market.spot * std::exp(carry * T);
        const double atmStrike = std::round(forward / strikeStep) * strikeStep;
        for (int i = -strikesEachSide; i <= strikesEachSide; ++i) {
            const double K = atmStrike + i * strikeStep;
            if (K <= 0.0) continue;
            const double k = std::log(K / forward);
            // Term-structure: skew flattens with sqrt(T).
            const double vol = std::max(0.03, atmVol + skew * k / std::sqrt(std::max(T, 1e-3)) + smile * k * k);
            Inputs in;
            in.model = market.model;
            in.spot = market.spot;
            in.strike = K;
            in.riskFreeRate = market.riskFreeRate;
            in.dividendYield = market.dividendYield;
            in.volatility = vol;
            in.maturity = T;
            const Result r = price(in);
            for (OptionType type : { OptionType::Call, OptionType::Put }) {
                ChainQuote q;
                q.type = type;
                q.strike = K;
                q.maturity = T;
                const double mid = type == OptionType::Call ? r.callPrice : r.putPrice;
                const double halfSpread = std::max(0.005, 0.5 * relativeSpread * mid);
                q.bid = std::max(0.0, mid - halfSpread);
                q.ask = mid + halfSpread;
                q.mid = mid;
                quotes.push_back(q);
            }
        }
    }
    return quotes;
}

} // namespace pricing
