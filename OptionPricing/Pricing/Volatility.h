//
//  Volatility.h
//  OptionPricing
//
//  Historical (realized) volatility and volatility forecasting from daily bars:
//
//    * Realized estimators: close-to-close, Parkinson, Garman-Klass, Rogers-Satchell and
//      Yang-Zhang, over a trailing window or as a rolling series.
//    * Volatility cone: percentiles of rolling realized vol per window length, with the
//      current reading and its percentile rank.
//    * EWMA (RiskMetrics) variance.
//    * GARCH(1,1) and GJR-GARCH(1,1) fitted by Gaussian quasi-maximum likelihood, with
//      multi-step variance forecasts and the implied forecast volatility term structure.
//
//  Volatilities are annualised decimals (0.25 = 25%). Variances inside the GARCH and EWMA
//  structures are per period (per trading day) unless stated otherwise.
//

#pragma once

#include "VolSurface.h"   // detail::nelderMead

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace pricing {

struct DailyBar {
    std::string date;              ///< ISO yyyy-mm-dd
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

enum class RealizedEstimator { CloseToClose, Parkinson, GarmanKlass, RogersSatchell, YangZhang };

inline const char* estimatorName(RealizedEstimator e)
{
    switch (e) {
    case RealizedEstimator::CloseToClose:   return "Close-to-close";
    case RealizedEstimator::Parkinson:      return "Parkinson";
    case RealizedEstimator::GarmanKlass:    return "Garman-Klass";
    case RealizedEstimator::RogersSatchell: return "Rogers-Satchell";
    case RealizedEstimator::YangZhang:      return "Yang-Zhang";
    }
    return "";
}

inline const char* estimatorDescription(RealizedEstimator e)
{
    switch (e) {
    case RealizedEstimator::CloseToClose:   return "Sample standard deviation of daily log returns. Unbiased but noisy; ignores intraday range.";
    case RealizedEstimator::Parkinson:      return "High-low range estimator, about 5x more efficient than close-to-close. Assumes no drift and no overnight gaps.";
    case RealizedEstimator::GarmanKlass:    return "Open-high-low-close estimator, about 8x more efficient than close-to-close. Assumes no drift and no overnight gaps.";
    case RealizedEstimator::RogersSatchell: return "OHLC estimator that allows a non-zero drift. Ignores overnight gaps.";
    case RealizedEstimator::YangZhang:      return "Combines overnight, open-to-close and Rogers-Satchell variance; handles drift and gaps. Best general-purpose choice.";
    }
    return "";
}

inline double annualiseVariance(double periodVariance, int periodsPerYear)
{
    return (std::isfinite(periodVariance) && periodVariance > 0.0) ? std::sqrt(periodVariance * periodsPerYear) : (periodVariance == 0.0 ? 0.0 : std::numeric_limits<double>::quiet_NaN());
}

namespace detail {

inline double mean(const std::vector<double>& x)
{
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (double v : x) s += v;
    return s / static_cast<double>(x.size());
}

inline double sampleVariance(const std::vector<double>& x)
{
    if (x.size() < 2) return 0.0;
    const double m = mean(x);
    double s = 0.0;
    for (double v : x) s += (v - m) * (v - m);
    return s / static_cast<double>(x.size() - 1);
}

/// Linear-interpolated percentile (p in [0,1]) of an ascending-sorted vector.
inline double percentileSorted(const std::vector<double>& sorted, double p)
{
    if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (sorted.size() == 1) return sorted.front();
    const double pos = std::clamp(p, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double f = pos - static_cast<double>(lo);
    return sorted[lo] + f * (sorted[hi] - sorted[lo]);
}

inline bool validBar(const DailyBar& b)
{
    return b.open > 0.0 && b.high > 0.0 && b.low > 0.0 && b.close > 0.0 && b.high >= b.low;
}

} // namespace detail

/// Daily log returns, one per bar after the first.
inline std::vector<double> logReturns(const std::vector<DailyBar>& bars)
{
    std::vector<double> r;
    if (bars.size() < 2) return r;
    r.reserve(bars.size() - 1);
    for (size_t i = 1; i < bars.size(); ++i) {
        r.push_back((bars[i].close > 0.0 && bars[i - 1].close > 0.0) ? std::log(bars[i].close / bars[i - 1].close) : 0.0);
    }
    return r;
}

/// Per-period realized variance of the bars with indices in [begin, end). The window
/// needs the previous close for the close-to-close and Yang-Zhang estimators, so
/// `begin` must be at least 1. Returns NaN when the window is too short.
inline double realizedVariance(const std::vector<DailyBar>& bars, RealizedEstimator estimator, size_t begin, size_t end)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (end > bars.size() || begin < 1 || end <= begin + 1) return nan;
    const size_t n = end - begin;
    const double count = static_cast<double>(n);

    switch (estimator) {
    case RealizedEstimator::CloseToClose: {
        std::vector<double> r;
        r.reserve(n);
        for (size_t i = begin; i < end; ++i) {
            if (bars[i].close <= 0.0 || bars[i - 1].close <= 0.0) return nan;
            r.push_back(std::log(bars[i].close / bars[i - 1].close));
        }
        return detail::sampleVariance(r);
    }
    case RealizedEstimator::Parkinson: {
        double s = 0.0;
        for (size_t i = begin; i < end; ++i) {
            if (!detail::validBar(bars[i])) return nan;
            const double hl = std::log(bars[i].high / bars[i].low);
            s += hl * hl;
        }
        return s / (4.0 * std::log(2.0) * count);
    }
    case RealizedEstimator::GarmanKlass: {
        double s = 0.0;
        for (size_t i = begin; i < end; ++i) {
            if (!detail::validBar(bars[i])) return nan;
            const double hl = std::log(bars[i].high / bars[i].low);
            const double co = std::log(bars[i].close / bars[i].open);
            s += 0.5 * hl * hl - (2.0 * std::log(2.0) - 1.0) * co * co;
        }
        return std::max(0.0, s / count);
    }
    case RealizedEstimator::RogersSatchell: {
        double s = 0.0;
        for (size_t i = begin; i < end; ++i) {
            if (!detail::validBar(bars[i])) return nan;
            const DailyBar& b = bars[i];
            s += std::log(b.high / b.close) * std::log(b.high / b.open) + std::log(b.low / b.close) * std::log(b.low / b.open);
        }
        return std::max(0.0, s / count);
    }
    case RealizedEstimator::YangZhang: {
        std::vector<double> overnight, openToClose;
        overnight.reserve(n);
        openToClose.reserve(n);
        double rs = 0.0;
        for (size_t i = begin; i < end; ++i) {
            const DailyBar& b = bars[i];
            if (!detail::validBar(b) || bars[i - 1].close <= 0.0) return nan;
            overnight.push_back(std::log(b.open / bars[i - 1].close));
            openToClose.push_back(std::log(b.close / b.open));
            rs += std::log(b.high / b.close) * std::log(b.high / b.open) + std::log(b.low / b.close) * std::log(b.low / b.open);
        }
        rs /= count;
        const double k = 0.34 / (1.34 + (count + 1.0) / (count - 1.0));
        return detail::sampleVariance(overnight) + k * detail::sampleVariance(openToClose) + (1.0 - k) * rs;
    }
    }
    return nan;
}

/// Annualised realized volatility over the most recent `window` bars.
inline double realizedVol(const std::vector<DailyBar>& bars, RealizedEstimator estimator, int window, int periodsPerYear = 252)
{
    if (window < 2 || bars.size() < static_cast<size_t>(window) + 1) return std::numeric_limits<double>::quiet_NaN();
    return annualiseVariance(realizedVariance(bars, estimator, bars.size() - static_cast<size_t>(window), bars.size()), periodsPerYear);
}

struct VolPoint {
    std::string date;      ///< date of the last bar in the window
    double vol = 0.0;      ///< annualised
};

/// Rolling annualised realized volatility; one point per bar from index `window` on.
inline std::vector<VolPoint> rollingRealizedVol(const std::vector<DailyBar>& bars, RealizedEstimator estimator, int window, int periodsPerYear = 252)
{
    std::vector<VolPoint> out;
    if (window < 2 || bars.size() < static_cast<size_t>(window) + 1) return out;
    out.reserve(bars.size() - static_cast<size_t>(window));
    for (size_t end = static_cast<size_t>(window) + 1; end <= bars.size(); ++end) {
        const double v = annualiseVariance(realizedVariance(bars, estimator, end - static_cast<size_t>(window), end), periodsPerYear);
        if (std::isfinite(v)) out.push_back({ bars[end - 1].date, v });
    }
    return out;
}

/// One row of the volatility cone.
struct ConeRow {
    int window = 0;
    double current = 0.0;          ///< latest rolling value
    double min = 0.0, p10 = 0.0, p25 = 0.0, median = 0.0, p75 = 0.0, p90 = 0.0, max = 0.0;
    double percentile = 0.0;       ///< rank of `current` within the history, 0..1
    int samples = 0;
};

inline std::vector<ConeRow> volCone(const std::vector<DailyBar>& bars, RealizedEstimator estimator, const std::vector<int>& windows, int periodsPerYear = 252)
{
    std::vector<ConeRow> rows;
    for (int w : windows) {
        const std::vector<VolPoint> series = rollingRealizedVol(bars, estimator, w, periodsPerYear);
        if (series.size() < 2) continue;
        std::vector<double> values;
        values.reserve(series.size());
        for (const VolPoint& p : series) values.push_back(p.vol);
        std::sort(values.begin(), values.end());
        ConeRow row;
        row.window = w;
        row.current = series.back().vol;
        row.min = values.front();
        row.p10 = detail::percentileSorted(values, 0.10);
        row.p25 = detail::percentileSorted(values, 0.25);
        row.median = detail::percentileSorted(values, 0.5);
        row.p75 = detail::percentileSorted(values, 0.75);
        row.p90 = detail::percentileSorted(values, 0.90);
        row.max = values.back();
        const auto below = std::lower_bound(values.begin(), values.end(), row.current) - values.begin();
        row.percentile = static_cast<double>(below) / static_cast<double>(values.size() - 1);
        row.samples = static_cast<int>(values.size());
        rows.push_back(row);
    }
    return rows;
}

// MARK: - EWMA

struct EwmaResult {
    double lambda = 0.94;
    int periodsPerYear = 252;
    std::vector<double> variance;      ///< per-period variance forecast for each return (made the day before)
    double nextVariance = 0.0;         ///< forecast for the period after the last return
    double currentVol() const { return annualiseVariance(nextVariance, periodsPerYear); }
};

/// RiskMetrics exponentially weighted variance. The recursion starts from the sample
/// variance of the returns.
inline EwmaResult ewmaVariance(const std::vector<double>& returns, double lambda = 0.94, int periodsPerYear = 252)
{
    EwmaResult r;
    r.lambda = lambda;
    r.periodsPerYear = periodsPerYear;
    if (returns.empty()) return r;
    r.variance.reserve(returns.size());
    double s2 = detail::sampleVariance(returns);
    if (s2 <= 0.0) s2 = returns.front() * returns.front();
    for (double ret : returns) {
        r.variance.push_back(s2);
        s2 = lambda * s2 + (1.0 - lambda) * ret * ret;
    }
    r.nextVariance = s2;
    return r;
}

// MARK: - GARCH

enum class GarchModel { Garch11, GjrGarch11 };

inline const char* garchModelName(GarchModel m)
{
    return m == GarchModel::Garch11 ? "GARCH(1,1)" : "GJR-GARCH(1,1)";
}

struct GarchFit {
    GarchModel model = GarchModel::Garch11;
    double omega = 0.0;      ///< per-period variance units
    double alpha = 0.0;
    double beta = 0.0;
    double gamma = 0.0;      ///< leverage term (GJR only): extra weight on squared negative shocks
    double mean = 0.0;       ///< per-period mean return removed before fitting
    bool converged = false;
    double logLikelihood = std::numeric_limits<double>::quiet_NaN();
    double aic = std::numeric_limits<double>::quiet_NaN();
    double bic = std::numeric_limits<double>::quiet_NaN();
    int observations = 0;
    int periodsPerYear = 252;
    std::vector<double> conditionalVariance;   ///< σ²_t for each return t (forecast made at t−1)
    double nextVariance = 0.0;                 ///< σ² for the first period after the sample
    double lastResidual = 0.0;

    int parameterCount() const { return model == GarchModel::Garch11 ? 3 : 4; }
    /// Rate at which variance shocks decay: α + β (+ γ/2 for GJR, assuming symmetric shocks).
    double persistence() const { return alpha + beta + 0.5 * gamma; }
    double unconditionalVariance() const
    {
        const double p = persistence();
        return p < 1.0 ? omega / (1.0 - p) : std::numeric_limits<double>::quiet_NaN();
    }
    /// Periods for a variance shock to decay by half.
    double halfLife() const
    {
        const double p = persistence();
        return (p > 0.0 && p < 1.0) ? std::log(0.5) / std::log(p) : std::numeric_limits<double>::quiet_NaN();
    }
    double currentVol() const { return annualiseVariance(nextVariance, periodsPerYear); }
    double unconditionalVol() const { return annualiseVariance(unconditionalVariance(), periodsPerYear); }
    /// Annualised conditional vol series aligned with the returns.
    std::vector<double> conditionalVol() const
    {
        std::vector<double> v;
        v.reserve(conditionalVariance.size());
        for (double s2 : conditionalVariance) v.push_back(annualiseVariance(s2, periodsPerYear));
        return v;
    }

    /// Per-period variance forecasts for horizons 1..horizon. Beyond the first step the
    /// forecast decays geometrically from the current variance towards the long-run level.
    std::vector<double> forecastVariance(int horizon) const
    {
        std::vector<double> out;
        if (horizon <= 0) return out;
        out.reserve(static_cast<size_t>(horizon));
        const double p = persistence();
        double s2 = nextVariance;
        out.push_back(s2);
        for (int h = 2; h <= horizon; ++h) {
            s2 = omega + p * s2;
            out.push_back(s2);
        }
        return out;
    }

    /// Annualised volatility implied by the average forecast variance over 1..h periods,
    /// for h = 1..horizon; comparable to an option's implied vol for that tenor.
    std::vector<double> forecastTermVol(int horizon) const
    {
        const std::vector<double> var = forecastVariance(horizon);
        std::vector<double> out;
        out.reserve(var.size());
        double cumulative = 0.0;
        for (size_t i = 0; i < var.size(); ++i) {
            cumulative += var[i];
            out.push_back(annualiseVariance(cumulative / static_cast<double>(i + 1), periodsPerYear));
        }
        return out;
    }
};

namespace detail {

inline double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }
inline double logit(double p) { p = std::clamp(p, 1e-9, 1.0 - 1e-9); return std::log(p / (1.0 - p)); }

/// Maps unconstrained optimiser coordinates to (ω, α, β, γ) with ω > 0, α, β, γ ≥ 0 and
/// α + β + γ/2 < 1. Returns are in percent inside the optimiser for conditioning.
template <size_t N>
inline std::array<double, 4> garchParamsFrom(const std::array<double, N>& x, GarchModel model)
{
    const double omega = std::exp(x[0]);
    const double persistence = 0.9999 * sigmoid(x[1]);
    if (model == GarchModel::Garch11 || N < 4) {
        const double share = sigmoid(x[2]);
        return { omega, persistence * share, persistence * (1.0 - share), 0.0 };
    }
    // Softmax split of the persistence between α, β and γ/2.
    const double ea = std::exp(x[2]), eb = 1.0, eg = std::exp(x[3]);
    const double sum = ea + eb + eg;
    return { omega, persistence * ea / sum, persistence * eb / sum, 2.0 * persistence * eg / sum };
}

/// Negative Gaussian log-likelihood (returns in percent) and the variance path.
inline double garchNegLogLikelihood(const std::vector<double>& eps, double omega, double alpha, double beta, double gamma, double var0,
                                    std::vector<double>* path = nullptr, double* next = nullptr)
{
    double s2 = var0;
    double sum = 0.0;
    if (path) path->clear();
    for (double e : eps) {
        if (!(s2 > 0.0) || !std::isfinite(s2)) return std::numeric_limits<double>::infinity();
        if (path) path->push_back(s2);
        sum += std::log(s2) + e * e / s2;
        s2 = omega + alpha * e * e + (e < 0.0 ? gamma * e * e : 0.0) + beta * s2;
    }
    if (next) *next = s2;
    return 0.5 * (sum + static_cast<double>(eps.size()) * std::log(2.0 * M_PI));
}

} // namespace detail

/// Fits a GARCH(1,1) or GJR-GARCH(1,1) to daily log returns by quasi-maximum likelihood
/// (Nelder-Mead on a constrained parameterisation). The mean is the sample mean. Needs
/// at least 60 observations; otherwise `converged` is false.
inline GarchFit fitGarch(const std::vector<double>& returns, GarchModel model = GarchModel::Garch11, int periodsPerYear = 252)
{
    GarchFit fit;
    fit.model = model;
    fit.periodsPerYear = periodsPerYear;
    fit.observations = static_cast<int>(returns.size());
    if (returns.size() < 60) return fit;

    // Work in percent: variances near 1 keep the optimiser well conditioned.
    constexpr double scale = 100.0;
    fit.mean = detail::mean(returns);
    std::vector<double> eps;
    eps.reserve(returns.size());
    for (double r : returns) eps.push_back((r - fit.mean) * scale);
    const double var0 = std::max(detail::sampleVariance(eps), 1e-8);

    const double startPersistence = 0.97;
    const double startAlpha = 0.08;
    auto objective4 = [&](const std::array<double, 4>& x) {
        const auto p = detail::garchParamsFrom(x, model);
        return detail::garchNegLogLikelihood(eps, p[0], p[1], p[2], p[3], var0);
    };
    auto objective3 = [&](const std::array<double, 3>& x) {
        const auto p = detail::garchParamsFrom(x, model);
        return detail::garchNegLogLikelihood(eps, p[0], p[1], p[2], p[3], var0);
    };

    std::array<double, 4> params{};
    if (model == GarchModel::Garch11) {
        std::array<double, 3> x = { std::log(var0 * (1.0 - startPersistence)), detail::logit(startPersistence / 0.9999), detail::logit(startAlpha / startPersistence) };
        x = detail::nelderMead<3>(objective3, x, { 1.0, 1.0, 1.0 }, 4000, 1e-10);
        x = detail::nelderMead<3>(objective3, x, { 0.2, 0.2, 0.2 }, 4000, 1e-12);   // restart tightens the optimum
        params = detail::garchParamsFrom(x, model);
    } else {
        // Start symmetric (γ small) and let the data pull the leverage term.
        const double shareAlpha = std::log(startAlpha / (startPersistence - startAlpha - 0.02));
        std::array<double, 4> x = { std::log(var0 * (1.0 - startPersistence)), detail::logit(startPersistence / 0.9999), shareAlpha, std::log(0.01 / (startPersistence - startAlpha - 0.02)) };
        x = detail::nelderMead<4>(objective4, x, { 1.0, 1.0, 1.0, 1.0 }, 6000, 1e-10);
        x = detail::nelderMead<4>(objective4, x, { 0.2, 0.2, 0.2, 0.2 }, 6000, 1e-12);
        params = detail::garchParamsFrom(x, model);
    }

    std::vector<double> path;
    double next = 0.0;
    const double negLL = detail::garchNegLogLikelihood(eps, params[0], params[1], params[2], params[3], var0, &path, &next);
    if (!std::isfinite(negLL)) return fit;

    const double n = static_cast<double>(eps.size());
    fit.omega = params[0] / (scale * scale);
    fit.alpha = params[1];
    fit.beta = params[2];
    fit.gamma = params[3];
    fit.conditionalVariance.reserve(path.size());
    for (double s2 : path) fit.conditionalVariance.push_back(s2 / (scale * scale));
    fit.nextVariance = next / (scale * scale);
    fit.lastResidual = eps.back() / scale;
    // Likelihood in decimal-return units: the density picks up a factor `scale` per observation.
    fit.logLikelihood = -negLL + n * std::log(scale);
    const int k = fit.parameterCount();
    fit.aic = 2.0 * k - 2.0 * fit.logLikelihood;
    fit.bic = k * std::log(n) - 2.0 * fit.logLikelihood;
    fit.converged = fit.persistence() < 0.9999 && fit.omega > 0.0;
    return fit;
}

} // namespace pricing
