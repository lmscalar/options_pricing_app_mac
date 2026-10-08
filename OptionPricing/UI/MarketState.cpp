//
//  MarketState.cpp
//  OptionPricing
//

#include "MarketState.h"

QJsonObject MarketState::toJson() const
{
    QJsonObject m;
    m["model"] = market.model == pricing::Model::Black76 ? "black76" : "bsm";
    m["spot"] = market.spot;
    m["rate"] = market.riskFreeRate;
    m["dividendYield"] = market.dividendYield;
    m["volatility"] = market.volatility;
    m["dayBasis"] = market.dayBasis;
    QJsonArray dividends;
    for (const pricing::Dividend& d : market.dividends) {
        QJsonObject o;
        o["time"] = d.time;
        o["amount"] = d.amount;
        dividends.append(o);
    }
    m["dividends"] = dividends;

    QJsonArray curve;
    for (const pricing::RatePoint& p : rateCurve.points()) {
        QJsonObject o;
        o["tenor"] = p.tenor;
        o["rate"] = p.rate;
        curve.append(o);
    }

    QJsonArray chain;
    for (const pricing::ChainQuote& q : chainQuotes) {
        QJsonObject o;
        o["type"] = q.type == pricing::OptionType::Call ? "call" : "put";
        o["strike"] = q.strike;
        o["maturity"] = q.maturity;
        o["bid"] = q.bid;
        o["ask"] = q.ask;
        o["mid"] = q.mid;
        if (q.volume > 0) o["volume"] = q.volume;
        if (q.openInterest > 0) o["openInterest"] = q.openInterest;
        if (q.vendorImpliedVol > 0) o["vendorImpliedVol"] = q.vendorImpliedVol;
        if (!q.expiryDate.empty()) o["expiryDate"] = QString::fromStdString(q.expiryDate);
        if (q.daysToExpiry > 0) o["daysToExpiry"] = q.daysToExpiry;
        chain.append(o);
    }

    QJsonObject root;
    root["market"] = m;
    root["rateCurve"] = curve;
    root["useRateCurve"] = useRateCurve;
    root["chain"] = chain;
    if (!underlyingTicker.isEmpty()) root["underlyingTicker"] = underlyingTicker;
    if (!spotSource.isEmpty()) {
        root["spotSource"] = spotSource;
        root["spotTime"] = spotTime.toString(Qt::ISODate);
    }
    return root;
}

void MarketState::fromJson(const QJsonObject& root)
{
    const QJsonObject m = root["market"].toObject();
    market.model = m["model"].toString() == "black76" ? pricing::Model::Black76 : pricing::Model::BlackScholesMerton;
    market.spot = m["spot"].toDouble(100.0);
    market.riskFreeRate = m["rate"].toDouble(0.05);
    market.dividendYield = m["dividendYield"].toDouble(0.0);
    market.volatility = m["volatility"].toDouble(0.20);
    market.dayBasis = m["dayBasis"].toDouble(365.0);
    market.dividends.clear();
    const QJsonArray dividends = m["dividends"].toArray();
    for (const QJsonValue v : dividends) {
        const QJsonObject o = v.toObject();
        market.dividends.push_back({ o["time"].toDouble(), o["amount"].toDouble() });
    }

    std::vector<pricing::RatePoint> points;
    const QJsonArray curve = root["rateCurve"].toArray();
    for (const QJsonValue v : curve) {
        const QJsonObject o = v.toObject();
        points.push_back({ o["tenor"].toDouble(), o["rate"].toDouble() });
    }
    rateCurve.setPoints(points);
    useRateCurve = root["useRateCurve"].toBool(false);

    underlyingTicker = root["underlyingTicker"].toString();
    spotSource = root["spotSource"].toString();
    spotTime = QDateTime::fromString(root["spotTime"].toString(), Qt::ISODate);

    chainQuotes.clear();
    const QJsonArray chain = root["chain"].toArray();
    for (const QJsonValue v : chain) {
        const QJsonObject o = v.toObject();
        pricing::ChainQuote q;
        q.type = o["type"].toString() == "put" ? pricing::OptionType::Put : pricing::OptionType::Call;
        q.strike = o["strike"].toDouble();
        q.maturity = o["maturity"].toDouble();
        q.bid = o["bid"].toDouble();
        q.ask = o["ask"].toDouble();
        q.mid = o["mid"].toDouble();
        q.volume = o["volume"].toDouble();
        q.openInterest = o["openInterest"].toDouble();
        q.vendorImpliedVol = o["vendorImpliedVol"].toDouble();
        q.expiryDate = o["expiryDate"].toString().toStdString();
        q.daysToExpiry = o["daysToExpiry"].toInt();
        chainQuotes.push_back(q);
    }
    if (!chainQuotes.empty()) {
        surface.setSlices(pricing::buildExpirySlices(chainQuotes, chainMarket(chainQuotes.front().maturity)));
    } else {
        surface = pricing::VolSurface();
    }
}
