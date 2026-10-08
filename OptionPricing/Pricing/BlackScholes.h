//
//  BlackScholes.h
//  OptionPricing
//
//  Closed-form European option pricing: Black-Scholes-Merton for spot assets with a
//  continuous dividend yield and/or discrete cash dividends, and Black-76 for options
//  on futures. Produces prices, first- and second-order Greeks, an implied-volatility
//  solver and risk-neutral probability metrics.
//
//  Conventions
//   - Rates, yields and volatilities are decimals (0.05 == 5%).
//   - Time to maturity is in years.
//   - Vega, rho, vanna, zomma are quoted per 1 percentage-point move.
//   - Volga is quoted as the change in vega (per 1%) for a 1% move in volatility.
//   - Theta, charm and color are quoted per day, where the day basis is configurable
//     (365 calendar days by default, 252 for trading-day theta).
//

#pragma once

#include "Normal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pricing {

/// Which closed-form model to apply.
///  - BlackScholesMerton: option on a spot asset paying a continuous dividend yield.
///  - Black76: option on a futures contract. The futures price replaces spot, there is
///    no carry, and the entire expected payoff is discounted at the risk-free rate.
enum class Model {
    BlackScholesMerton,
    Black76,
};

enum class OptionType {
    Call,
    Put,
};

/// A single known cash dividend paid at `time` years from today.
struct Dividend {
    double time = 0.0;
    double amount = 0.0;
};

/// Market and contract parameters.
struct Inputs {
    Model model = Model::BlackScholesMerton;
    double spot = 0.0;             ///< Spot price, or futures price under Black76.
    double strike = 0.0;
    double riskFreeRate = 0.0;     ///< Continuously compounded. Negative rates are allowed.
    double dividendYield = 0.0;    ///< Continuous yield. Ignored under Black76.
    double volatility = 0.0;
    double maturity = 0.0;         ///< Years.
    double dayBasis = 365.0;       ///< Days per year used to quote theta, charm and color.
    std::vector<Dividend> dividends; ///< Discrete cash dividends (escrowed-dividend model).
};

/// First- and second-order sensitivities for one option leg.
struct Greeks {
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;    ///< per 1% volatility
    double theta = 0.0;   ///< per day
    double rho = 0.0;     ///< per 1% rate
    double vanna = 0.0;   ///< dDelta/dVol per 1% volatility
    double volga = 0.0;   ///< dVega/dVol per 1% volatility
    double charm = 0.0;   ///< dDelta/dt per day (time passing)
    double speed = 0.0;   ///< dGamma/dSpot
    double color = 0.0;   ///< dGamma/dt per day (time passing)
    double zomma = 0.0;   ///< dGamma/dVol per 1% volatility
};

struct Result {
    double d1 = 0.0;
    double d2 = 0.0;
    double effectiveSpot = 0.0;    ///< Spot net of the present value of discrete dividends.
    double forward = 0.0;          ///< Forward price implied by the inputs.
    double callPrice = 0.0;
    double putPrice = 0.0;
    Greeks call;
    Greeks put;
};

/// Effective carry yield: Black-76 has zero cost of carry, so the yield equals the rate.
inline double effectiveYield(const Inputs& in)
{
    return in.model == Model::Black76 ? in.riskFreeRate : in.dividendYield;
}

/// Present value of all discrete dividends paid strictly before maturity.
inline double presentValueOfDividends(const Inputs& in)
{
    if (in.model == Model::Black76) {
        return 0.0;
    }
    double pv = 0.0;
    for (const Dividend& d : in.dividends) {
        if (d.time > 0.0 && d.time <= in.maturity && std::isfinite(d.amount)) {
            pv += d.amount * std::exp(-in.riskFreeRate * d.time);
        }
    }
    return pv;
}

/// Spot price after escrowing the discrete dividends. This is the quantity that
/// follows geometric Brownian motion under the escrowed-dividend model.
inline double effectiveSpot(const Inputs& in)
{
    return in.spot - presentValueOfDividends(in);
}

/// Returns true when the contract inputs (everything except volatility) are usable.
inline bool hasValidContract(const Inputs& in)
{
    return in.spot > 0.0 && in.strike > 0.0 && in.maturity > 0.0 && in.dayBasis > 0.0
        && std::isfinite(in.spot) && std::isfinite(in.strike) && std::isfinite(in.maturity)
        && std::isfinite(in.riskFreeRate) && std::isfinite(in.dividendYield)
        && effectiveSpot(in) > 0.0;
}

/// Returns true when the inputs are in the domain where the closed-form solution is defined.
inline bool isValid(const Inputs& in)
{
    return hasValidContract(in) && in.volatility > 0.0 && std::isfinite(in.volatility);
}

/// Generalised Black-Scholes pricing. Black-76 is the special case where the cost of
/// carry is zero, obtained by setting the effective yield equal to the rate.
inline Result price(const Inputs& in)
{
    const bool onFutures = (in.model == Model::Black76);

    const double S = effectiveSpot(in);
    const double K = in.strike;
    const double r = in.riskFreeRate;
    const double q = effectiveYield(in);
    const double v = in.volatility;
    const double T = in.maturity;
    const double basis = in.dayBasis;

    const double sqrtT = std::sqrt(T);
    const double vSqrtT = v * sqrtT;
    const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / vSqrtT;
    const double d2 = d1 - vSqrtT;

    const double discountR = std::exp(-r * T);
    const double discountQ = std::exp(-q * T);
    const double Nd1 = normalCdf(d1);
    const double Nd2 = normalCdf(d2);
    const double NminusD1 = normalCdf(-d1);
    const double NminusD2 = normalCdf(-d2);
    const double nd1 = normalPdf(d1);

    Result out;
    out.d1 = d1;
    out.d2 = d2;
    out.effectiveSpot = S;
    out.forward = S * discountQ / discountR;

    out.callPrice = S * discountQ * Nd1 - K * discountR * Nd2;
    out.putPrice = K * discountR * NminusD2 - S * discountQ * NminusD1;

    // First order
    out.call.delta = discountQ * Nd1;
    out.put.delta = discountQ * (Nd1 - 1.0);

    const double gamma = discountQ * nd1 / (S * vSqrtT);
    out.call.gamma = gamma;
    out.put.gamma = gamma;

    const double vegaUnit = S * discountQ * nd1 * sqrtT;   // per unit of volatility
    out.call.vega = vegaUnit / 100.0;
    out.put.vega = vegaUnit / 100.0;

    const double thetaCommon = -S * discountQ * nd1 * v / (2.0 * sqrtT);
    out.call.theta = (thetaCommon - r * K * discountR * Nd2 + q * S * discountQ * Nd1) / basis;
    out.put.theta = (thetaCommon + r * K * discountR * NminusD2 - q * S * discountQ * NminusD1) / basis;

    if (onFutures) {
        // Under Black-76 the rate only enters through the discount factor, so dV/dr = -T * V.
        out.call.rho = -T * out.callPrice / 100.0;
        out.put.rho = -T * out.putPrice / 100.0;
    } else {
        out.call.rho = K * T * discountR * Nd2 / 100.0;
        out.put.rho = -K * T * discountR * NminusD2 / 100.0;
    }

    // Second order. These are identical for calls and puts except charm.
    const double vanna = -discountQ * nd1 * d2 / v / 100.0;
    const double volga = vegaUnit * d1 * d2 / v / 10000.0;
    const double speed = -gamma / S * (1.0 + d1 / vSqrtT);
    const double zomma = gamma * (d1 * d2 - 1.0) / v / 100.0;

    // dDelta/dT and dGamma/dT (derivatives with respect to maturity). Time passing
    // shortens maturity, so the per-day decay figures are the negatives of these.
    const double d1PrimeNumerator = 2.0 * (r - q) * T - d2 * vSqrtT;
    const double dDeltaDT_call = discountQ * (nd1 * d1PrimeNumerator / (2.0 * T * vSqrtT) - q * Nd1);
    const double dDeltaDT_put  = discountQ * (nd1 * d1PrimeNumerator / (2.0 * T * vSqrtT) + q * NminusD1);
    const double dGammaDT = -gamma / (2.0 * T) * (2.0 * q * T + 1.0 + d1 * d1PrimeNumerator / vSqrtT);

    for (Greeks* g : { &out.call, &out.put }) {
        g->vanna = vanna;
        g->volga = volga;
        g->speed = speed;
        g->zomma = zomma;
        g->color = -dGammaDT / basis;
    }
    out.call.charm = -dDeltaDT_call / basis;
    out.put.charm = -dDeltaDT_put / basis;

    return out;
}

/// Convenience: price of a single leg.
inline double legPrice(const Inputs& in, OptionType type)
{
    const Result r = price(in);
    return type == OptionType::Call ? r.callPrice : r.putPrice;
}

/// Undiscounted intrinsic value at expiry for a given terminal spot.
inline double payoff(OptionType type, double strike, double terminalSpot)
{
    return type == OptionType::Call ? std::max(terminalSpot - strike, 0.0)
                                    : std::max(strike - terminalSpot, 0.0);
}

// MARK: - Implied volatility

/// Outcome of an implied-volatility search.
struct ImpliedVolResult {
    enum class Status {
        Converged,
        InvalidInputs,     // contract inputs or market price unusable
        BelowLowerBound,   // price at or below intrinsic (discounted) value: no time value
        AboveUpperBound,   // price exceeds the no-arbitrage ceiling
        NotConverged,      // iteration limit hit
    };

    Status status = Status::InvalidInputs;
    double volatility = 0.0;   // decimal, valid only when Converged
    int iterations = 0;
    double lowerBound = 0.0;   // no-arbitrage price bounds for the chosen leg
    double upperBound = 0.0;
};

/// Solves price(sigma) == marketPrice for sigma. The option price is strictly increasing
/// in volatility, so a Newton step guarded by a shrinking bisection bracket converges
/// reliably from any starting point inside the bracket.
inline ImpliedVolResult impliedVolatility(Inputs in, OptionType type, double marketPrice)
{
    ImpliedVolResult out;
    if (!hasValidContract(in) || !std::isfinite(marketPrice) || marketPrice <= 0.0) {
        out.status = ImpliedVolResult::Status::InvalidInputs;
        return out;
    }

    const double discountR = std::exp(-in.riskFreeRate * in.maturity);
    const double discountQ = std::exp(-effectiveYield(in) * in.maturity);
    const double forwardLeg = effectiveSpot(in) * discountQ;
    const double strikeLeg = in.strike * discountR;

    if (type == OptionType::Call) {
        out.lowerBound = std::max(forwardLeg - strikeLeg, 0.0);
        out.upperBound = forwardLeg;
    } else {
        out.lowerBound = std::max(strikeLeg - forwardLeg, 0.0);
        out.upperBound = strikeLeg;
    }

    const double tolerance = 1e-10 * std::max(1.0, marketPrice);
    if (marketPrice <= out.lowerBound + tolerance) {
        out.status = ImpliedVolResult::Status::BelowLowerBound;
        return out;
    }
    if (marketPrice >= out.upperBound - tolerance) {
        out.status = ImpliedVolResult::Status::AboveUpperBound;
        return out;
    }

    auto modelPrice = [&](double sigma) {
        in.volatility = sigma;
        return legPrice(in, type);
    };

    // Bracket: price is monotonic in sigma, so widen the top until it exceeds the market.
    double lo = 1e-6;
    double hi = 2.0;
    while (modelPrice(hi) < marketPrice && hi < 50.0) {
        hi *= 2.0;
    }

    // Brenner-Subrahmanyam starting guess, clamped into the bracket.
    double sigma = std::sqrt(2.0 * M_PI / in.maturity) * marketPrice / forwardLeg;
    sigma = std::clamp(sigma, lo * 10.0, hi * 0.9);

    constexpr int maxIterations = 100;
    for (int i = 1; i <= maxIterations; ++i) {
        out.iterations = i;
        in.volatility = sigma;
        const Result r = price(in);
        const double f = (type == OptionType::Call ? r.callPrice : r.putPrice) - marketPrice;

        if (std::fabs(f) < tolerance) {
            out.status = ImpliedVolResult::Status::Converged;
            out.volatility = sigma;
            return out;
        }

        // Tighten the bracket around the root.
        if (f > 0.0) {
            hi = sigma;
        } else {
            lo = sigma;
        }

        // Newton step using vega per unit of volatility; fall back to bisection when
        // vega is tiny or the step would leave the bracket.
        const double vegaPerUnit = r.call.vega * 100.0;
        double next = (vegaPerUnit > 1e-12) ? sigma - f / vegaPerUnit : std::nan("");
        if (!std::isfinite(next) || next <= lo || next >= hi) {
            next = 0.5 * (lo + hi);
        }

        if (std::fabs(next - sigma) < 1e-12 || (hi - lo) < 1e-12) {
            out.status = ImpliedVolResult::Status::Converged;
            out.volatility = next;
            return out;
        }
        sigma = next;
    }

    out.status = ImpliedVolResult::Status::NotConverged;
    out.volatility = sigma;
    return out;
}

// MARK: - Probability metrics

/// Risk-neutral probability and expected-move statistics for the contract.
struct Probabilities {
    double callInTheMoney = 0.0;    ///< N(d2)
    double putInTheMoney = 0.0;     ///< N(-d2)
    double touchStrike = 0.0;       ///< Probability the spot path touches the strike before expiry.
    double oneSigmaMove = 0.0;      ///< S * sigma * sqrt(T)
    double lowerOneSigma = 0.0;     ///< Lognormal 1-sigma band of the terminal spot.
    double upperOneSigma = 0.0;
    double callBreakeven = 0.0;     ///< K + call premium
    double putBreakeven = 0.0;      ///< K - put premium
};

inline Probabilities probabilities(const Inputs& in, const Result& r)
{
    Probabilities p;
    const double S = r.effectiveSpot;
    const double K = in.strike;
    const double v = in.volatility;
    const double T = in.maturity;
    const double sqrtT = std::sqrt(T);

    p.callInTheMoney = normalCdf(r.d2);
    p.putInTheMoney = normalCdf(-r.d2);

    // First-passage probability for geometric Brownian motion with drift mu under the
    // risk-neutral measure: P(max S_t >= K) for K > S, mirrored for K < S.
    const double mu = in.riskFreeRate - effectiveYield(in) - 0.5 * v * v;
    if (std::fabs(K - S) < 1e-12 * S) {
        p.touchStrike = 1.0;
    } else {
        const double sign = (K > S) ? 1.0 : -1.0;       // +1: barrier above, -1: barrier below
        const double b = sign * std::log(K / S);        // positive distance in log space
        const double m = sign * mu;                     // drift toward the barrier
        const double a1 = (-b + m * T) / (v * sqrtT);
        const double a2 = (-b - m * T) / (v * sqrtT);
        p.touchStrike = normalCdf(a1) + std::exp(2.0 * m * b / (v * v)) * normalCdf(a2);
        p.touchStrike = std::clamp(p.touchStrike, 0.0, 1.0);
    }

    p.oneSigmaMove = in.spot * v * sqrtT;
    const double forwardSpot = r.forward;
    p.lowerOneSigma = forwardSpot * std::exp(-0.5 * v * v * T - v * sqrtT);
    p.upperOneSigma = forwardSpot * std::exp(-0.5 * v * v * T + v * sqrtT);
    p.callBreakeven = K + r.callPrice;
    p.putBreakeven = K - r.putPrice;
    return p;
}

} // namespace pricing
