//
//  Risk.h
//  OptionPricing
//
//  Portfolio risk measures on a Portfolio.h book:
//   - parametric (delta-gamma-normal) VaR and expected shortfall with component VaR,
//   - historical simulation with full revaluation over aligned daily returns,
//   - Monte Carlo with correlated normal returns (Cholesky) and full revaluation,
//   - a spot x vol stress grid and a time-decay ladder.
//  Losses are reported as positive numbers. Qt-free; unit tested in RiskTests.mm.
//

#pragma once

#include "Portfolio.h"
#include "Normal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace pricing {
namespace risk {

/// Aligned daily log (or simple) returns: `returns[t][i]` for day t and symbol i.
struct ReturnHistory {
    std::vector<std::string> symbols;
    std::vector<std::vector<double>> returns;
    size_t days() const { return returns.size(); }
    int indexOf(const std::string& symbol) const
    {
        for (size_t i = 0; i < symbols.size(); ++i) if (symbols[i] == symbol) return static_cast<int>(i);
        return -1;
    }
};

/// Sample covariance matrix of the history (per day).
inline std::vector<std::vector<double>> covariance(const ReturnHistory& h)
{
    const size_t k = h.symbols.size(), n = h.days();
    std::vector<std::vector<double>> cov(k, std::vector<double>(k, 0.0));
    if (n < 2) return cov;
    std::vector<double> mean(k, 0.0);
    for (const auto& row : h.returns) for (size_t i = 0; i < k; ++i) mean[i] += row[i];
    for (double& m : mean) m /= static_cast<double>(n);
    for (const auto& row : h.returns) {
        for (size_t i = 0; i < k; ++i) {
            for (size_t j = i; j < k; ++j) {
                const double c = (row[i] - mean[i]) * (row[j] - mean[j]);
                cov[i][j] += c;
                if (i != j) cov[j][i] += c;
            }
        }
    }
    for (auto& row : cov) for (double& c : row) c /= static_cast<double>(n - 1);
    return cov;
}

/// Builds the covariance used by the parametric and Monte Carlo methods for the book's
/// underlyings: historical where available, otherwise the holding's implied vol as a
/// stand-alone daily variance (zero correlation with the rest).
inline std::vector<std::vector<double>> bookCovariance(const std::vector<std::string>& symbols, const ReturnHistory& history, const Markets& markets)
{
    const size_t k = symbols.size();
    std::vector<std::vector<double>> cov(k, std::vector<double>(k, 0.0));
    const auto histCov = covariance(history);
    for (size_t i = 0; i < k; ++i) {
        const int hi = history.indexOf(symbols[i]);
        for (size_t j = 0; j < k; ++j) {
            const int hj = history.indexOf(symbols[j]);
            if (hi >= 0 && hj >= 0) cov[i][j] = histCov[static_cast<size_t>(hi)][static_cast<size_t>(hj)];
        }
        if (hi < 0) {
            const double sigma = marketFor(markets, symbols[i]).volatility / std::sqrt(252.0);
            cov[i][i] = sigma * sigma;
        }
    }
    return cov;
}

/// Cholesky factor L (lower) with L Lᵀ = A; small diagonal jitter keeps near-singular inputs usable.
inline std::vector<std::vector<double>> cholesky(std::vector<std::vector<double>> a)
{
    const size_t n = a.size();
    std::vector<std::vector<double>> L(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i) a[i][i] += 1e-12;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j <= i; ++j) {
            double sum = a[i][j];
            for (size_t m = 0; m < j; ++m) sum -= L[i][m] * L[j][m];
            if (i == j) L[i][i] = std::sqrt(std::max(sum, 0.0));
            else L[i][j] = L[j][j] > 0.0 ? sum / L[j][j] : 0.0;
        }
    }
    return L;
}

struct VarResult {
    std::string method;
    double confidence = 0.95;
    int horizonDays = 1;
    double var = 0.0;                 ///< loss not exceeded with probability `confidence`
    double cvar = 0.0;                ///< expected loss beyond VaR (expected shortfall)
    double meanPnl = 0.0;
    double stdevPnl = 0.0;
    std::vector<double> pnl;          ///< simulated P&L sample (empty for parametric), ascending
    std::map<std::string, double> componentVar;   ///< parametric: Euler contributions summing to VaR
    bool ok = false;
    std::string note;
};

inline double quantileSorted(const std::vector<double>& sorted, double q)
{
    if (sorted.empty()) return 0.0;
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(sorted.size() - 1, lo + 1);
    const double w = pos - static_cast<double>(lo);
    return sorted[lo] * (1.0 - w) + sorted[hi] * w;
}

/// VaR and expected shortfall from a P&L sample (sorted in place).
inline void tailFromSample(std::vector<double>& pnl, double confidence, VarResult& out)
{
    std::sort(pnl.begin(), pnl.end());
    const double q = quantileSorted(pnl, 1.0 - confidence);
    out.var = -q;
    double sum = 0.0;
    int n = 0;
    for (double v : pnl) { if (v <= q) { sum += v; ++n; } }
    out.cvar = n > 0 ? -sum / n : out.var;
    const double mean = std::accumulate(pnl.begin(), pnl.end(), 0.0) / static_cast<double>(pnl.size());
    double var = 0.0;
    for (double v : pnl) var += (v - mean) * (v - mean);
    out.meanPnl = mean;
    out.stdevPnl = pnl.size() > 1 ? std::sqrt(var / static_cast<double>(pnl.size() - 1)) : 0.0;
    out.pnl = pnl;
    out.ok = true;
}

/// Delta-gamma-normal VaR: P&L ≈ Σ δ$_i r_i + ½ Σ γ$_i r_i² with r ~ N(0, Σ h). The gamma
/// term shifts the mean by ½ tr(ΓΣ)h and adds ½ tr((ΓΣ)²)h² to the variance. Component VaR
/// uses the delta-normal Euler allocation.
inline VarResult parametricVar(const Portfolio& p, const Markets& markets, const ReturnHistory& history, double confidence, int horizonDays)
{
    VarResult out;
    out.method = "Parametric (delta-gamma)";
    out.confidence = confidence;
    out.horizonDays = horizonDays;
    const std::vector<std::string> symbols = underlyings(p);
    const size_t k = symbols.size();
    if (k == 0) { out.note = "No holdings."; return out; }
    const BookExposure book = bookExposure(p, markets, false);
    std::vector<double> deltaD(k, 0.0), gammaD(k, 0.0);
    for (size_t i = 0; i < k; ++i) {
        const Exposure& e = book.byUnderlying.at(symbols[i]);
        deltaD[i] = e.deltaDollars;              // per unit return
        gammaD[i] = e.gammaDollars * 100.0;      // gamma x spot^2: per unit return squared
    }
    const double h = static_cast<double>(horizonDays);
    auto cov = bookCovariance(symbols, history, markets);
    for (auto& row : cov) for (double& c : row) c *= h;
    // Delta-normal variance and gamma adjustments.
    double variance = 0.0, gammaMean = 0.0, gammaVar = 0.0;
    std::vector<double> covDelta(k, 0.0);
    for (size_t i = 0; i < k; ++i) {
        for (size_t j = 0; j < k; ++j) {
            variance += deltaD[i] * cov[i][j] * deltaD[j];
            covDelta[i] += cov[i][j] * deltaD[j];
            gammaVar += 0.5 * gammaD[i] * cov[i][j] * gammaD[j] * cov[i][j];   // ½ tr((ΓΣ)²) for diagonal Γ
        }
        gammaMean += 0.5 * gammaD[i] * cov[i][i];
    }
    const double sigma = std::sqrt(std::max(0.0, variance + gammaVar));
    const double z = pricing::normalInverseCdf(confidence);
    const double pdfZ = std::exp(-0.5 * z * z) / std::sqrt(2.0 * M_PI);
    out.meanPnl = gammaMean;
    out.stdevPnl = sigma;
    out.var = std::max(0.0, z * sigma - gammaMean);
    out.cvar = std::max(out.var, sigma * pdfZ / (1.0 - confidence) - gammaMean);
    const double deltaSigma = std::sqrt(std::max(variance, 0.0));
    for (size_t i = 0; i < k; ++i) {
        out.componentVar[symbols[i]] = deltaSigma > 0.0 ? deltaD[i] * covDelta[i] / deltaSigma * z : 0.0;
    }
    out.ok = sigma > 0.0 || p.holdings.empty();
    if (history.days() < 20) out.note = "Covariance from implied volatilities (little or no return history).";
    return out;
}

/// Historical simulation: every historical day's returns (compounded over `horizonDays`
/// with overlapping windows) are applied to today's spots and the book is fully revalued.
inline VarResult historicalVar(const Portfolio& p, const Markets& markets, const ReturnHistory& history, double confidence, int horizonDays)
{
    VarResult out;
    out.method = "Historical simulation";
    out.confidence = confidence;
    out.horizonDays = horizonDays;
    const size_t n = history.days();
    const size_t h = static_cast<size_t>(std::max(1, horizonDays));
    if (n < h + 20) { out.note = "Not enough return history (need at least 20 days beyond the horizon)."; return out; }
    std::vector<double> pnl;
    pnl.reserve(n - h + 1);
    const double elapsed = static_cast<double>(horizonDays) / 365.0;
    for (size_t t = 0; t + h <= n; ++t) {
        std::map<std::string, double> shocks;
        for (size_t i = 0; i < history.symbols.size(); ++i) {
            double growth = 1.0;
            for (size_t d = 0; d < h; ++d) growth *= 1.0 + history.returns[t + d][i];
            shocks[history.symbols[i]] = growth - 1.0;
        }
        pnl.push_back(revaluePnl(p, markets, shocks, 0.0, elapsed));
    }
    tailFromSample(pnl, confidence, out);
    return out;
}

/// Monte Carlo: correlated normal returns over the horizon, full revaluation per path.
inline VarResult monteCarloVar(const Portfolio& p, const Markets& markets, const ReturnHistory& history, double confidence, int horizonDays,
                               int paths = 10000, unsigned seed = 20261009u)
{
    VarResult out;
    out.method = "Monte Carlo";
    out.confidence = confidence;
    out.horizonDays = horizonDays;
    const std::vector<std::string> symbols = underlyings(p);
    const size_t k = symbols.size();
    if (k == 0) { out.note = "No holdings."; return out; }
    auto cov = bookCovariance(symbols, history, markets);
    for (auto& row : cov) for (double& c : row) c *= static_cast<double>(horizonDays);
    const auto L = cholesky(cov);
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const double elapsed = static_cast<double>(horizonDays) / 365.0;
    std::vector<double> pnl;
    pnl.reserve(static_cast<size_t>(std::max(100, paths)));
    std::vector<double> zvec(k), rvec(k);
    for (int path = 0; path < std::max(100, paths); ++path) {
        for (size_t i = 0; i < k; ++i) zvec[i] = gauss(rng);
        std::map<std::string, double> shocks;
        for (size_t i = 0; i < k; ++i) {
            double r = 0.0;
            for (size_t j = 0; j <= i; ++j) r += L[i][j] * zvec[j];
            // Log-normal step so a one-day draw can never take the spot below zero.
            shocks[symbols[i]] = std::exp(r - 0.5 * cov[i][i]) - 1.0;
        }
        pnl.push_back(revaluePnl(p, markets, shocks, 0.0, elapsed));
    }
    tailFromSample(pnl, confidence, out);
    if (history.days() < 20) out.note = "Covariance from implied volatilities (little or no return history).";
    return out;
}

/// Spot x vol stress grid: P&L for each combination (same shock applied to every underlying).
struct StressGrid {
    std::vector<double> spotShocks;    ///< decimal, e.g. -0.10
    std::vector<double> volShocks;     ///< relative, e.g. 0.25 = vols +25%
    std::vector<std::vector<double>> pnl;   ///< [spot][vol]
};

inline StressGrid stressGrid(const Portfolio& p, const Markets& markets, std::vector<double> spotShocks = { -0.20, -0.10, -0.05, 0.0, 0.05, 0.10, 0.20 },
                             std::vector<double> volShocks = { -0.50, -0.25, 0.0, 0.25, 0.50 }, double elapsed = 0.0)
{
    StressGrid g;
    g.spotShocks = std::move(spotShocks);
    g.volShocks = std::move(volShocks);
    const std::vector<std::string> symbols = underlyings(p);
    for (double s : g.spotShocks) {
        std::vector<double> row;
        std::map<std::string, double> shocks;
        for (const std::string& sym : symbols) shocks[sym] = s;
        for (double v : g.volShocks) row.push_back(revaluePnl(p, markets, shocks, v, elapsed));
        g.pnl.push_back(row);
    }
    return g;
}

/// Theta ladder: P&L from time passing alone (spots and vols unchanged).
inline std::vector<std::pair<int, double>> decayLadder(const Portfolio& p, const Markets& markets, std::vector<int> days = { 1, 5, 10, 21, 63 })
{
    std::vector<std::pair<int, double>> out;
    for (int d : days) out.emplace_back(d, revaluePnl(p, markets, {}, 0.0, d / 365.0));
    return out;
}

} // namespace risk
} // namespace pricing
