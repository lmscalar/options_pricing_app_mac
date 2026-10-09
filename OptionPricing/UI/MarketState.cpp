//
//  MarketState.cpp
//  OptionPricing
//

#include "MarketState.h"
#include "../Pricing/Activity.h"

#include <QtCore/QRegularExpression>

#include <cmath>

QString MarketState::businessSummary(int maxChars) const
{
    QString industryText = industry.trimmed();
    if (!industryText.isEmpty()) {
        // "Services-Prepackaged Software" -> "Services – Prepackaged Software", in title case.
        industryText.replace(QRegularExpression("\\s*-\\s*"), QStringLiteral(" – "));
        QStringList words = industryText.toLower().split(' ', Qt::SkipEmptyParts);
        for (QString& w : words) if (!w.isEmpty() && w[0].isLetter()) w[0] = w[0].toUpper();
        industryText = words.join(' ');
    }
    QString sentence = companyDescription.trimmed();
    if (!sentence.isEmpty()) {
        const QRegularExpressionMatch end = QRegularExpression("[.!?](\\s|$)").match(sentence);
        if (end.hasMatch()) sentence = sentence.left(end.capturedStart() + 1);
    }
    QString out = industryText;
    if (!sentence.isEmpty()) out += (out.isEmpty() ? QString() : QStringLiteral("  ·  ")) + sentence;
    if (out.size() > maxChars) out = out.left(maxChars - 1).trimmed() + QStringLiteral("…");
    return out;
}

void MarketState::applySpotPolicy()
{
    pricing::ActivityMarket am;
    am.model = market.model;
    am.spot = vendorSpot > 0.0 ? vendorSpot : market.spot;
    am.dividendYield = market.dividendYield;
    am.rateFor = [this](double t) { return rateFor(t); };
    const pricing::ImpliedSpotEstimate estimate = pricing::impliedSpotFromParity(chainQuotes, am, am.spot, market.dividends);
    impliedSpot = estimate.valid ? estimate.spot : 0.0;
    impliedSpotNote = estimate.valid
        ? QStringLiteral("parity-implied from %1 options (%2 DTE, %3 strike pairs, ±%4)")
              .arg(estimate.expiry.expiryDate.empty() ? QStringLiteral("nearest") : QString::fromStdString(estimate.expiry.expiryDate))
              .arg(estimate.expiry.daysToExpiry).arg(estimate.pairs).arg(QString::number(estimate.dispersion, 'f', 2))
        : QString();
    if (useImpliedSpot && estimate.valid) {
        market.spot = estimate.spot;
        spotSource = "option parity";
    } else if (vendorSpot > 0.0) {
        market.spot = vendorSpot;
        spotSource = vendorSource;
    }
}

bool MarketState::updateVendorQuote(const QString& ticker, double last, double previousClose, const QDateTime& asOf, const QString& source)
{
    if (ticker.trimmed().toUpper() != underlyingTicker || underlyingTicker.isEmpty() || !(last > 0.0)) return false;
    // Never move backwards: the chain's spot timer and the watchlist refresh both deliver
    // the same vendor snapshot; keep whichever is newer.
    if (asOf.isValid() && spotAsOf.isValid() && asOf < spotAsOf) return false;
    const bool changed = std::fabs(vendorSpot - last) > 1e-9 || (previousClose > 0.0 && std::fabs(this->previousClose - previousClose) > 1e-9) || asOf != spotAsOf;
    vendorSpot = last;
    if (previousClose > 0.0 && !m_previousCloseFromBars) this->previousClose = previousClose;
    vendorSource = source;
    spotAsOf = asOf;
    spotTime = QDateTime::currentDateTime();
    applySpotPolicy();
    return changed;
}

bool MarketState::setPreviousCloseFromBars(const QString& ticker, double close)
{
    if (ticker.trimmed().toUpper() != underlyingTicker || !(close > 0.0)) return false;
    m_previousCloseFromBars = true;
    if (std::fabs(previousClose - close) < 1e-9) return false;
    previousClose = close;
    return true;
}

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
    if (!companyName.isEmpty()) root["companyName"] = companyName;
    if (!exchange.isEmpty()) root["exchange"] = exchange;
    if (!industry.isEmpty()) root["industry"] = industry;
    if (!companyDescription.isEmpty()) root["companyDescription"] = companyDescription;
    if (previousClose > 0.0) root["previousClose"] = previousClose;
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
    companyName = root["companyName"].toString();
    exchange = root["exchange"].toString();
    industry = root["industry"].toString();
    companyDescription = root["companyDescription"].toString();
    logo = QImage();   // re-fetched (or served from the disk cache) when the chain tab sees the ticker
    previousClose = root["previousClose"].toDouble(0.0);
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
