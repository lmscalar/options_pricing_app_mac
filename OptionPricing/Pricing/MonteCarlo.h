//
//  MonteCarlo.h
//  OptionPricing
//
//  Monte Carlo cross-check for European options: terminal-value simulation under
//  geometric Brownian motion with antithetic variates. Reports a standard error so
//  the user can judge agreement with the closed form.
//

#pragma once

#include "BlackScholes.h"

#include <cmath>
#include <random>

namespace pricing {

struct MonteCarloResult {
    double price = 0.0;
    double standardError = 0.0;
    int paths = 0;
};

/// Simulates `paths` terminal spots (half of them antithetic) and discounts the mean payoff.
inline MonteCarloResult monteCarloPrice(const Inputs& in, OptionType type, int paths = 200000,
                                        unsigned long long seed = 20240601ULL)
{
    MonteCarloResult out;
    if (!isValid(in) || paths < 2) {
        return out;
    }

    const double S0 = effectiveSpot(in);
    const double K = in.strike;
    const double r = in.riskFreeRate;
    const double q = effectiveYield(in);
    const double v = in.volatility;
    const double T = in.maturity;

    const double drift = (r - q - 0.5 * v * v) * T;
    const double diffusion = v * std::sqrt(T);
    const double discount = std::exp(-r * T);

    std::mt19937_64 rng(seed);
    std::normal_distribution<double> normal(0.0, 1.0);

    const int pairs = paths / 2;
    double sum = 0.0;
    double sumSquares = 0.0;
    for (int i = 0; i < pairs; ++i) {
        const double z = normal(rng);
        const double up = payoff(type, K, S0 * std::exp(drift + diffusion * z));
        const double down = payoff(type, K, S0 * std::exp(drift - diffusion * z));
        // Average the antithetic pair so each sample is one independent estimate.
        const double sample = 0.5 * (up + down);
        sum += sample;
        sumSquares += sample * sample;
    }

    const double mean = sum / pairs;
    const double variance = std::max(0.0, sumSquares / pairs - mean * mean);
    out.price = discount * mean;
    out.standardError = discount * std::sqrt(variance / pairs);
    out.paths = pairs * 2;
    return out;
}

} // namespace pricing
