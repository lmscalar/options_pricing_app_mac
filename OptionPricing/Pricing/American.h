//
//  American.h
//  OptionPricing
//
//  American-exercise pricing: a Cox-Ross-Rubinstein binomial tree (reference engine)
//  and the Bjerksund-Stensland (1993) closed-form approximation (fast engine).
//  Both operate on the same Inputs as the European model; discrete dividends are
//  handled with the escrowed-dividend adjustment to the spot.
//

#pragma once

#include "BlackScholes.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pricing {

enum class Exercise {
    European,
    American,
};

/// Price plus the Greeks that fall out of the tree directly.
struct TreeResult {
    double price = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double theta = 0.0;   ///< per day, using Inputs::dayBasis
};

/// Cox-Ross-Rubinstein binomial tree. `steps` of 200 to 500 give prices accurate to
/// well under a cent for typical equity inputs.
inline TreeResult binomialPrice(const Inputs& in, OptionType type, Exercise exercise, int steps = 400)
{
    TreeResult out;
    if (!isValid(in) || steps < 3) {
        return out;
    }

    const double S0 = effectiveSpot(in);
    const double K = in.strike;
    const double r = in.riskFreeRate;
    const double q = effectiveYield(in);
    const double v = in.volatility;
    const double T = in.maturity;

    const double dt = T / steps;
    const double u = std::exp(v * std::sqrt(dt));
    const double d = 1.0 / u;
    const double growth = std::exp((r - q) * dt);
    const double p = std::clamp((growth - d) / (u - d), 0.0, 1.0);
    const double disc = std::exp(-r * dt);

    // Terminal payoffs.
    std::vector<double> values(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double ST = S0 * std::pow(u, i) * std::pow(d, steps - i);
        values[static_cast<size_t>(i)] = payoff(type, K, ST);
    }

    // Roll back, remembering the node values at steps 1 and 2 for the Greeks.
    double v1Up = 0.0, v1Down = 0.0;
    double v2Up = 0.0, v2Mid = 0.0, v2Down = 0.0;
    for (int step = steps - 1; step >= 0; --step) {
        for (int i = 0; i <= step; ++i) {
            double continuation = disc * (p * values[static_cast<size_t>(i) + 1] + (1.0 - p) * values[static_cast<size_t>(i)]);
            if (exercise == Exercise::American) {
                const double St = S0 * std::pow(u, i) * std::pow(d, step - i);
                continuation = std::max(continuation, payoff(type, K, St));
            }
            values[static_cast<size_t>(i)] = continuation;
        }
        if (step == 2) {
            v2Down = values[0];
            v2Mid = values[1];
            v2Up = values[2];
        } else if (step == 1) {
            v1Down = values[0];
            v1Up = values[1];
        }
    }

    out.price = values[0];

    const double sUp = S0 * u;
    const double sDown = S0 * d;
    out.delta = (v1Up - v1Down) / (sUp - sDown);

    const double sUU = S0 * u * u;
    const double sDD = S0 * d * d;
    const double deltaUp = (v2Up - v2Mid) / (sUU - S0);
    const double deltaDown = (v2Mid - v2Down) / (S0 - sDD);
    out.gamma = (deltaUp - deltaDown) / (0.5 * (sUU - sDD));

    // The middle node two steps ahead has the same spot as the root, so the value
    // difference is a pure time derivative over 2*dt.
    out.theta = (v2Mid - out.price) / (2.0 * dt) / in.dayBasis;
    return out;
}

namespace detail {

/// The phi helper from Bjerksund-Stensland (1993).
inline double bsPhi(double S, double T, double gamma, double H, double I, double r, double b, double v)
{
    const double vSqrtT = v * std::sqrt(T);
    const double lambda = (-r + gamma * b + 0.5 * gamma * (gamma - 1.0) * v * v) * T;
    const double d = -(std::log(S / H) + (b + (gamma - 0.5) * v * v) * T) / vSqrtT;
    const double kappa = 2.0 * b / (v * v) + (2.0 * gamma - 1.0);
    return std::exp(lambda) * std::pow(S, gamma)
         * (normalCdf(d) - std::pow(I / S, kappa) * normalCdf(d - 2.0 * std::log(I / S) / vSqrtT));
}

/// American call under Bjerksund-Stensland (1993) with cost of carry b.
inline double bjerksundStenslandCall(double S, double K, double T, double r, double b, double v)
{
    if (b >= r) {
        // Never optimal to exercise early: the American call equals the European call.
        Inputs in;
        in.model = Model::BlackScholesMerton;
        in.spot = S;
        in.strike = K;
        in.riskFreeRate = r;
        in.dividendYield = r - b;
        in.volatility = v;
        in.maturity = T;
        return price(in).callPrice;
    }

    const double v2 = v * v;
    const double beta = (0.5 - b / v2) + std::sqrt((b / v2 - 0.5) * (b / v2 - 0.5) + 2.0 * r / v2);
    if (!(beta > 1.0) || !std::isfinite(beta)) {
        return std::nan("");
    }
    const double bInfinity = beta / (beta - 1.0) * K;
    const double b0 = std::max(K, r / (r - b) * K);
    const double h = -(b * T + 2.0 * v * std::sqrt(T)) * b0 / (bInfinity - b0);
    const double I = b0 + (bInfinity - b0) * (1.0 - std::exp(h));
    const double alpha = (I - K) * std::pow(I, -beta);

    if (S >= I) {
        return S - K;
    }
    return alpha * std::pow(S, beta)
         - alpha * bsPhi(S, T, beta, I, I, r, b, v)
         + bsPhi(S, T, 1.0, I, I, r, b, v)
         - bsPhi(S, T, 1.0, K, I, r, b, v)
         - K * bsPhi(S, T, 0.0, I, I, r, b, v)
         + K * bsPhi(S, T, 0.0, K, I, r, b, v);
}

} // namespace detail

/// Bjerksund-Stensland (1993) approximation of an American option. Falls back to the
/// binomial tree if the closed form is undefined for the inputs.
inline double bjerksundStenslandPrice(const Inputs& in, OptionType type)
{
    if (!isValid(in)) {
        return 0.0;
    }
    const double S = effectiveSpot(in);
    const double K = in.strike;
    const double T = in.maturity;
    const double r = in.riskFreeRate;
    const double b = in.riskFreeRate - effectiveYield(in);   // zero under Black-76
    const double v = in.volatility;

    double value;
    if (type == OptionType::Call) {
        value = detail::bjerksundStenslandCall(S, K, T, r, b, v);
    } else {
        // Put-call transformation: P(S, K, T, r, b) = C(K, S, T, r - b, -b).
        value = detail::bjerksundStenslandCall(K, S, T, r - b, -b, v);
    }
    if (!std::isfinite(value)) {
        value = binomialPrice(in, type, Exercise::American).price;
    }
    // The approximation can dip a hair below the European price; never report less.
    return std::max(value, legPrice(in, type));
}

/// Full American valuation: price, early-exercise premium and bumped Greeks.
struct AmericanResult {
    double price = 0.0;
    double europeanPrice = 0.0;
    double earlyExercisePremium = 0.0;
    Greeks greeks;   ///< delta, gamma, vega, theta, rho via central differences on the tree
};

inline AmericanResult americanValuation(const Inputs& in, OptionType type, int steps = 400)
{
    AmericanResult out;
    if (!isValid(in)) {
        return out;
    }
    const TreeResult tree = binomialPrice(in, type, Exercise::American, steps);
    out.price = tree.price;
    out.europeanPrice = legPrice(in, type);
    out.earlyExercisePremium = std::max(0.0, out.price - out.europeanPrice);
    out.greeks.delta = tree.delta;
    out.greeks.gamma = tree.gamma;
    out.greeks.theta = tree.theta;

    auto bumped = [&](auto&& mutate) {
        Inputs copy = in;
        mutate(copy);
        return binomialPrice(copy, type, Exercise::American, steps).price;
    };

    const double dv = 0.01;
    out.greeks.vega = (bumped([&](Inputs& i) { i.volatility += dv; })
                     - bumped([&](Inputs& i) { i.volatility = std::max(1e-6, i.volatility - dv); })) / 2.0;
    const double dr = 0.01;
    out.greeks.rho = (bumped([&](Inputs& i) { i.riskFreeRate += dr; })
                    - bumped([&](Inputs& i) { i.riskFreeRate -= dr; })) / 2.0;
    return out;
}

} // namespace pricing
