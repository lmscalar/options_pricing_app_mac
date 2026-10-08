//
//  FiniteDifference.h
//  OptionPricing
//
//  Crank-Nicolson finite-difference solver on a log-spot grid. Handles European
//  options with a direct tridiagonal solve and American options with projected
//  successive over-relaxation (PSOR). Used as an independent cross-check engine.
//

#pragma once

#include "American.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pricing {

struct FiniteDifferenceResult {
    double price = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    int spaceSteps = 0;
    int timeSteps = 0;
};

inline FiniteDifferenceResult finiteDifferencePrice(const Inputs& in, OptionType type, Exercise exercise,
                                                    int spaceSteps = 400, int timeSteps = 400)
{
    FiniteDifferenceResult out;
    if (!isValid(in) || spaceSteps < 10 || timeSteps < 2) {
        return out;
    }

    const double S0 = effectiveSpot(in);
    const double K = in.strike;
    const double r = in.riskFreeRate;
    const double q = effectiveYield(in);
    const double v = in.volatility;
    const double T = in.maturity;

    // Grid in x = ln(S), centred on the strike and wide enough for ~5 standard deviations.
    const double width = std::max(5.0 * v * std::sqrt(T), 1.0);
    const double xCenter = std::log(K);
    const double xMin = std::min(xCenter, std::log(S0)) - width;
    const double xMax = std::max(xCenter, std::log(S0)) + width;
    const int N = spaceSteps;
    const double dx = (xMax - xMin) / N;
    const double dt = T / timeSteps;

    std::vector<double> x(static_cast<size_t>(N) + 1), S(static_cast<size_t>(N) + 1), V(static_cast<size_t>(N) + 1), intrinsic(static_cast<size_t>(N) + 1);
    for (int i = 0; i <= N; ++i) {
        x[static_cast<size_t>(i)] = xMin + i * dx;
        S[static_cast<size_t>(i)] = std::exp(x[static_cast<size_t>(i)]);
        intrinsic[static_cast<size_t>(i)] = payoff(type, K, S[static_cast<size_t>(i)]);
        V[static_cast<size_t>(i)] = intrinsic[static_cast<size_t>(i)];
    }

    // Coefficients of the spatial operator L V = 0.5 v^2 V_xx + (r - q - 0.5 v^2) V_x - r V.
    const double sigma2 = v * v;
    const double mu = r - q - 0.5 * sigma2;
    const double alpha = 0.5 * sigma2 / (dx * dx);
    const double betaCoef = mu / (2.0 * dx);
    const double lower = alpha - betaCoef;        // coefficient of V[i-1]
    const double diag = -2.0 * alpha - r;         // coefficient of V[i]
    const double upper = alpha + betaCoef;        // coefficient of V[i+1]

    // Crank-Nicolson: (I - dt/2 L) V_new = (I + dt/2 L) V_old
    const double aL = -0.5 * dt * lower;
    const double aD = 1.0 - 0.5 * dt * diag;
    const double aU = -0.5 * dt * upper;
    const double bL = 0.5 * dt * lower;
    const double bD = 1.0 + 0.5 * dt * diag;
    const double bU = 0.5 * dt * upper;

    std::vector<double> rhs(static_cast<size_t>(N) + 1);
    std::vector<double> cPrime(static_cast<size_t>(N) + 1), dPrime(static_cast<size_t>(N) + 1);

    for (int step = 1; step <= timeSteps; ++step) {
        const double tau = step * dt;   // time to maturity at the new level

        // Boundary values at the new time level.
        double lowBoundary, highBoundary;
        if (type == OptionType::Call) {
            lowBoundary = 0.0;
            highBoundary = S[static_cast<size_t>(N)] * std::exp(-q * tau) - K * std::exp(-r * tau);
            if (exercise == Exercise::American) highBoundary = std::max(highBoundary, intrinsic[static_cast<size_t>(N)]);
        } else {
            lowBoundary = K * std::exp(-r * tau) - S[0] * std::exp(-q * tau);
            if (exercise == Exercise::American) lowBoundary = std::max(lowBoundary, intrinsic[0]);
            highBoundary = 0.0;
        }

        for (int i = 1; i < N; ++i) {
            rhs[static_cast<size_t>(i)] = bL * V[static_cast<size_t>(i) - 1] + bD * V[static_cast<size_t>(i)] + bU * V[static_cast<size_t>(i) + 1];
        }
        rhs[1] -= aL * lowBoundary;
        rhs[static_cast<size_t>(N) - 1] -= aU * highBoundary;

        if (exercise == Exercise::European) {
            // Thomas algorithm for the interior unknowns 1..N-1.
            cPrime[1] = aU / aD;
            dPrime[1] = rhs[1] / aD;
            for (int i = 2; i < N; ++i) {
                const double m = aD - aL * cPrime[static_cast<size_t>(i) - 1];
                cPrime[static_cast<size_t>(i)] = aU / m;
                dPrime[static_cast<size_t>(i)] = (rhs[static_cast<size_t>(i)] - aL * dPrime[static_cast<size_t>(i) - 1]) / m;
            }
            V[static_cast<size_t>(N) - 1] = dPrime[static_cast<size_t>(N) - 1];
            for (int i = N - 2; i >= 1; --i) {
                V[static_cast<size_t>(i)] = dPrime[static_cast<size_t>(i)] - cPrime[static_cast<size_t>(i)] * V[static_cast<size_t>(i) + 1];
            }
        } else {
            // Projected SOR: iterate to the solution while enforcing V >= intrinsic.
            const double omega = 1.2;
            for (int iter = 0; iter < 2000; ++iter) {
                double maxChange = 0.0;
                for (int i = 1; i < N; ++i) {
                    const double gaussSeidel = (rhs[static_cast<size_t>(i)] - aL * V[static_cast<size_t>(i) - 1] - aU * V[static_cast<size_t>(i) + 1]) / aD;
                    double candidate = V[static_cast<size_t>(i)] + omega * (gaussSeidel - V[static_cast<size_t>(i)]);
                    candidate = std::max(candidate, intrinsic[static_cast<size_t>(i)]);
                    maxChange = std::max(maxChange, std::fabs(candidate - V[static_cast<size_t>(i)]));
                    V[static_cast<size_t>(i)] = candidate;
                }
                if (maxChange < 1e-9) {
                    break;
                }
            }
        }
        V[0] = lowBoundary;
        V[static_cast<size_t>(N)] = highBoundary;
    }

    // Interpolate the result at the actual spot and read off delta and gamma.
    const double xS = std::log(S0);
    int i = static_cast<int>(std::floor((xS - xMin) / dx));
    i = std::clamp(i, 1, N - 2);
    const double w = (xS - x[static_cast<size_t>(i)]) / dx;
    out.price = (1.0 - w) * V[static_cast<size_t>(i)] + w * V[static_cast<size_t>(i) + 1];

    // Derivatives in x converted to derivatives in S.
    const double Vx = (V[static_cast<size_t>(i) + 1] - V[static_cast<size_t>(i) - 1]) / (2.0 * dx);
    const double Vxx = (V[static_cast<size_t>(i) + 1] - 2.0 * V[static_cast<size_t>(i)] + V[static_cast<size_t>(i) - 1]) / (dx * dx);
    const double Si = S[static_cast<size_t>(i)];
    out.delta = Vx / Si;
    out.gamma = (Vxx - Vx) / (Si * Si);
    out.spaceSteps = N;
    out.timeSteps = timeSteps;
    return out;
}

} // namespace pricing
