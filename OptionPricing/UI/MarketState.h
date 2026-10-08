//
//  MarketState.h
//  OptionPricing
//
//  The market context shared by every tab (spot, rates, yield, volatility, dividends,
//  rate curve and fitted volatility surface) with a simple change-notification list.
//  The Pricer tab edits it; the other tabs observe it.
//

#pragma once

#include "QtHeaders.h"
#include "../Pricing/RateCurve.h"
#include "../Pricing/Strategy.h"
#include "../Pricing/VolSurface.h"

#include <functional>
#include <vector>

class MarketState {
public:
    using Listener = std::function<void()>;

    pricing::Market market;            ///< model, spot, flat rate, yield, vol, day basis, dividends
    pricing::RateCurve rateCurve;
    bool useRateCurve = false;
    pricing::VolSurface surface;       ///< fitted from the option chain, may be empty
    std::vector<pricing::ChainQuote> chainQuotes;   ///< raw chain, kept so it can be saved
    QString underlyingTicker;                        ///< symbol the chain and spot came from, if downloaded
    QString companyName;                             ///< issuer name from the reference data, if known
    QString exchange;                                ///< primary exchange code, if known
    QImage logo;                                     ///< company icon, null when unavailable
    QString spotSource;                              ///< "last minute bar", "day close", "option parity", ... (empty for manual spot)
    QDateTime spotTime;                              ///< when the live spot was fetched
    QDateTime spotAsOf;                              ///< vendor timestamp of the delayed stock price
    double vendorSpot = 0.0;                         ///< last delayed stock price from the feed (0 = none)
    double previousClose = 0.0;                      ///< prior session close from the feed, for daily change (0 = none)
    bool useImpliedSpot = true;                      ///< prefer the parity-implied spot from fresh option prices
    double impliedSpot = 0.0;                        ///< latest parity-implied spot (0 = unavailable)
    QString impliedSpotNote;                         ///< how the implied spot was derived
    QDateTime chainTime;                             ///< when the chain was last downloaded

    /// Rate applicable to a maturity: from the curve if enabled, otherwise the flat rate.
    double rateFor(double maturity) const
    {
        if (useRateCurve && !rateCurve.empty()) {
            return rateCurve.rate(maturity);
        }
        return market.riskFreeRate;
    }

    /// Market with the rate resolved for a given maturity.
    pricing::Market marketFor(double maturity) const
    {
        pricing::Market m = market;
        m.riskFreeRate = rateFor(maturity);
        return m;
    }

    /// Daily change of the current spot versus the previous close (0 when unknown).
    double dayChange() const { return previousClose > 0.0 ? market.spot - previousClose : 0.0; }
    double dayChangePercent() const { return previousClose > 0.0 ? (market.spot / previousClose - 1.0) * 100.0 : 0.0; }
    bool hasDayChange() const { return previousClose > 0.0; }

    pricing::ChainMarket chainMarket(double maturity) const
    {
        pricing::ChainMarket cm;
        cm.model = market.model;
        cm.spot = market.spot;
        cm.riskFreeRate = rateFor(maturity);
        cm.dividendYield = market.dividendYield;
        return cm;
    }

    void subscribe(Listener listener) { m_listeners.push_back(std::move(listener)); }

    /// Notifies every observer. Re-entrancy is guarded so observers that write back
    /// into the state do not cause infinite loops.
    void notify()
    {
        if (m_notifying) {
            return;
        }
        m_notifying = true;
        for (const Listener& l : m_listeners) {
            l();
        }
        m_notifying = false;
    }

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& json);

private:
    std::vector<Listener> m_listeners;
    bool m_notifying = false;
};
