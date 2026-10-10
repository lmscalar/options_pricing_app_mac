//
//  MarketDataClient.cpp
//  OptionPricing
//

#include "MarketDataClient.h"
#include "../Pricing/DayCount.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

namespace {

constexpr const char* kBaseUrl = "https://api.massive.com";
constexpr const char* kSettingsKey = "massive/apiKey";

pricing::CivilDate civil(const QDate& d)
{
    return pricing::CivilDate{ d.year(), d.month(), d.day() };
}

QUrl endpoint(const QString& path, const QList<QPair<QString, QString>>& query = {})
{
    QUrl url(QString::fromLatin1(kBaseUrl) + path);
    QUrlQuery q;
    for (const auto& item : query) {
        q.addQueryItem(item.first, item.second);
    }
    url.setQuery(q);
    return url;
}

} // namespace

MarketDataClient::MarketDataClient()
{
    m_apiKey = discoverApiKey(&m_source);
}

QString MarketDataClient::discoverApiKey(QString* source)
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : { "MASSIVE_API_KEY", "POLYGON_API_KEY" }) {
        const QString value = env.value(QString::fromLatin1(name)).trimmed();
        if (!value.isEmpty()) {
            if (source) *source = QStringLiteral("environment variable %1").arg(QString::fromLatin1(name));
            return value;
        }
    }
    const QSettings settings;
    const QString stored = settings.value(kSettingsKey).toString().trimmed();
    if (!stored.isEmpty()) {
        if (source) *source = QStringLiteral("application preferences");
        return stored;
    }
    if (source) source->clear();
    return QString();
}

void MarketDataClient::setApiKey(const QString& key, bool remember)
{
    m_apiKey = key.trimmed();
    m_source = m_apiKey.isEmpty() ? QString() : (remember ? QStringLiteral("application preferences") : QStringLiteral("this session"));
    QSettings settings;
    if (remember && !m_apiKey.isEmpty()) {
        settings.setValue(kSettingsKey, m_apiKey);
    } else {
        settings.remove(kSettingsKey);
    }
}

void MarketDataClient::forgetStoredApiKey()
{
    QSettings settings;
    settings.remove(kSettingsKey);
}

// MARK: - Transport

QString MarketDataClient::describeFailure(int httpStatus, const QJsonObject& body, const QString& transportError)
{
    QString detail = body["error"].toString();
    if (detail.isEmpty()) detail = body["message"].toString();
    switch (httpStatus) {
    case 401:
    case 403:
        return QStringLiteral("Massive rejected the API key (HTTP %1). %2").arg(httpStatus).arg(detail);
    case 404:
        return QStringLiteral("Massive has no data for that request (HTTP 404). %1").arg(detail);
    case 429:
        return QStringLiteral("Massive rate limit reached (HTTP 429). Wait a minute and try again, or reduce the number of expiries. %1").arg(detail);
    default:
        break;
    }
    if (httpStatus >= 400) {
        return QStringLiteral("Massive returned HTTP %1. %2").arg(httpStatus).arg(detail);
    }
    return QStringLiteral("Network error: %1").arg(transportError);
}

void MarketDataClient::get(const QUrl& url, std::function<void(const QJsonObject&)> ok, ErrorHandler err)
{
    if (m_apiKey.isEmpty()) {
        err("No Massive API key. Set MASSIVE_API_KEY or POLYGON_API_KEY, or enter a key via Market > Set Massive API Key.");
        return;
    }
    getWithRetry(url, 0, std::move(ok), std::move(err));
}

void MarketDataClient::getWithRetry(const QUrl& url, int attempt, std::function<void(const QJsonObject&)> ok, ErrorHandler err)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(25000);
    QNetworkReply* reply = m_manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply, url, attempt, ok = std::move(ok), err = std::move(err)] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject body = doc.object();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            // Many parallel requests (chain preloading) can draw an HTTP/2 GOAWAY, which Qt
            // reports as a cancelled operation, or a 429; both are worth a short back-off.
            const bool networkLevel = reply->error() != QNetworkReply::NoError && status == 0
                                      && reply->error() != QNetworkReply::AuthenticationRequiredError
                                      && reply->error() != QNetworkReply::SslHandshakeFailedError;
            const bool transient = networkLevel || status == 429 || status >= 500;
            if (transient && attempt < 2) {
                const int delayMs = attempt == 0 ? 800 : 3200;
                // The manager is the context: if the client is destroyed first, the retry is dropped.
                QTimer::singleShot(delayMs, &m_manager, [this, url, attempt, ok, err] { getWithRetry(url, attempt + 1, ok, err); });
                return;
            }
            err(describeFailure(status, body, reply->errorString()));
            return;
        }
        if (!body.isEmpty() && body.contains("status") && body["status"].toString() != "OK" && body["status"].toString() != "DELAYED") {
            err(QStringLiteral("Massive responded with status '%1'. %2").arg(body["status"].toString(), body["error"].toString()));
            return;
        }
        ok(body);
    });
}

void MarketDataClient::getPaged(const QUrl& url, int maxPages, std::function<void(const QJsonArray&)> onPage,
                                std::function<void()> done, ErrorHandler err)
{
    struct State {
        std::function<void(const QUrl&, int)> step;
    };
    auto state = std::make_shared<State>();
    state->step = [this, state, maxPages, onPage, done, err](const QUrl& pageUrl, int page) {
        get(pageUrl, [state, maxPages, onPage, done, page](const QJsonObject& body) {
            onPage(body["results"].toArray());
            const QString next = body["next_url"].toString();
            if (!next.isEmpty() && page + 1 < maxPages) {
                state->step(QUrl(next), page + 1);
            } else {
                done();
            }
        }, err);
    };
    state->step(url, 0);
}

// MARK: - Endpoints

void MarketDataClient::fetchUnderlying(const QString& ticker, std::function<void(const UnderlyingSnapshot&)> ok, ErrorHandler err)
{
    const QString symbol = ticker.trimmed().toUpper();
    get(endpoint(QStringLiteral("/v2/snapshot/locale/us/markets/stocks/tickers/%1").arg(symbol)),
        [symbol, ok, err, this](const QJsonObject& body) {
            const QJsonObject t = body["ticker"].toObject();
            UnderlyingSnapshot snap;
            snap.ticker = symbol;
            snap.previousClose = t["prevDay"].toObject()["c"].toDouble();
            const double minuteClose = t["min"].toObject()["c"].toDouble();
            const double dayClose = t["day"].toObject()["c"].toDouble();
            const qint64 minuteMillis = static_cast<qint64>(t["min"].toObject()["t"].toDouble());
            const qint64 updatedNanos = static_cast<qint64>(t["updated"].toDouble());
            if (minuteMillis > 0) snap.asOf = QDateTime::fromMSecsSinceEpoch(minuteMillis);
            else if (updatedNanos > 0) snap.asOf = QDateTime::fromMSecsSinceEpoch(updatedNanos / 1000000);
            if (minuteClose > 0.0) {
                snap.price = minuteClose;
                snap.priceSource = "last minute bar";
            } else if (dayClose > 0.0) {
                snap.price = dayClose;
                snap.priceSource = "day close";
            } else if (snap.previousClose > 0.0) {
                snap.price = snap.previousClose;
                snap.priceSource = "previous close";
            }
            if (snap.price > 0.0) {
                ok(snap);
                return;
            }
            // Snapshot empty (e.g. new listing or outside the plan's entitlements): fall back to the previous close aggregate.
            get(endpoint(QStringLiteral("/v2/aggs/ticker/%1/prev").arg(symbol)), [symbol, ok, err](const QJsonObject& prev) {
                const QJsonArray results = prev["results"].toArray();
                if (results.isEmpty()) {
                    err(QStringLiteral("No price data for %1.").arg(symbol));
                    return;
                }
                UnderlyingSnapshot s;
                s.ticker = symbol;
                s.price = results.first().toObject()["c"].toDouble();
                s.previousClose = s.price;
                s.priceSource = "previous close";
                const qint64 barMillis = static_cast<qint64>(results.first().toObject()["t"].toDouble());
                if (barMillis > 0) s.asOf = QDateTime::fromMSecsSinceEpoch(barMillis);
                if (s.price <= 0.0) {
                    err(QStringLiteral("No price data for %1.").arg(symbol));
                    return;
                }
                ok(s);
            }, err);
        }, err);
}

void MarketDataClient::fetchExpirations(const QString& ticker, std::function<void(const QList<QDate>&)> ok, ErrorHandler err)
{
    auto dates = std::make_shared<std::set<QDate>>();
    const QUrl url = endpoint("/v3/reference/options/contracts",
                              { { "underlying_ticker", ticker.trimmed().toUpper() }, { "expired", "false" },
                                { "limit", "1000" }, { "sort", "expiration_date" }, { "order", "asc" } });
    getPaged(url, 3,
             [dates](const QJsonArray& results) {
                 for (const QJsonValue v : results) {
                     const QDate d = QDate::fromString(v.toObject()["expiration_date"].toString(), Qt::ISODate);
                     if (d.isValid()) dates->insert(d);
                 }
             },
             [dates, ok, err, ticker] {
                 if (dates->empty()) {
                     err(QStringLiteral("No listed option contracts found for %1.").arg(ticker.trimmed().toUpper()));
                     return;
                 }
                 QList<QDate> list;
                 for (const QDate& d : *dates) list << d;
                 ok(list);
             },
             err);
}

void MarketDataClient::fetchChains(const QString& ticker, const QDate& valuationDate, int maxExpiries,
                                   std::function<void(int pages, int contracts)> progress,
                                   std::function<void(const ChainDownload&)> ok, ErrorHandler err)
{
    const QString symbol = ticker.trimmed().toUpper();
    auto download = std::make_shared<ChainDownload>();

    // Every contract in the snapshot is mapped to a ChainQuote with its own maturity
    // measured from the valuation date on ACT/365.
    auto consume = [download, valuationDate](const QJsonArray& results) {
        for (const QJsonValue v : results) {
            const QJsonObject contract = v.toObject();
            const QJsonObject details = contract["details"].toObject();
            const QJsonObject quote = contract["last_quote"].toObject();
            const QJsonObject day = contract["day"].toObject();
            ++download->contracts;

            pricing::ChainQuote q;
            const QString type = details["contract_type"].toString();
            if (type == "call") q.type = pricing::OptionType::Call;
            else if (type == "put") q.type = pricing::OptionType::Put;
            else { ++download->skipped; continue; }

            const QDate expiry = QDate::fromString(details["expiration_date"].toString(), Qt::ISODate);
            if (!expiry.isValid() || expiry <= valuationDate) { ++download->skipped; continue; }
            q.daysToExpiry = static_cast<int>(valuationDate.daysTo(expiry));
            q.maturity = q.daysToExpiry / 365.0;
            q.expiryDate = expiry.toString(Qt::ISODate).toStdString();
            download->expiries.insert(expiry);

            q.strike = details["strike_price"].toDouble();
            q.bid = quote["bid"].toDouble();
            q.ask = quote["ask"].toDouble();
            const double midpoint = quote["midpoint"].toDouble();
            if (q.bid > 0.0 && q.ask > 0.0) {
                q.mid = 0.5 * (q.bid + q.ask);
                ++download->withQuotes;
            } else if (midpoint > 0.0) {
                q.mid = midpoint;
                ++download->withQuotes;
            } else if (day["close"].toDouble() > 0.0) {
                q.mid = day["close"].toDouble();
            } else {
                ++download->skipped;
                continue;
            }
            q.volume = day["volume"].toDouble();
            q.openInterest = contract["open_interest"].toDouble();
            q.vendorImpliedVol = contract["implied_volatility"].toDouble();
            if (q.strike > 0.0) {
                download->quotes.push_back(q);
            } else {
                ++download->skipped;
            }
        }
    };

    auto runSnapshot = [this, symbol, valuationDate, download, consume, progress, ok, err](const QDate& lastExpiry) {
        QList<QPair<QString, QString>> query = {
            { "expiration_date.gt", valuationDate.toString(Qt::ISODate) },
            { "limit", "250" },
            { "sort", "ticker" },
        };
        if (lastExpiry.isValid()) {
            query.append({ "expiration_date.lte", lastExpiry.toString(Qt::ISODate) });
        }
        auto pages = std::make_shared<int>(0);
        getPaged(endpoint(QStringLiteral("/v3/snapshot/options/%1").arg(symbol), query), 400,
                 [consume, progress, pages, download](const QJsonArray& results) {
                     consume(results);
                     ++*pages;
                     if (progress) progress(*pages, download->contracts);
                 },
                 [download, ok, err, symbol] {
                     if (download->quotes.empty()) {
                         err(QStringLiteral("Massive returned no priced option contracts for %1.").arg(symbol));
                         return;
                     }
                     ok(*download);
                 },
                 err);
    };

    if (maxExpiries <= 0) {
        runSnapshot(QDate());
        return;
    }
    // Limited to the N nearest expiries: look them up first so the snapshot can be filtered server-side.
    fetchExpirations(symbol, [runSnapshot, maxExpiries, valuationDate, err, symbol](const QList<QDate>& dates) {
        QList<QDate> future;
        for (const QDate& d : dates) {
            if (d > valuationDate) future << d;
        }
        if (future.isEmpty()) {
            err(QStringLiteral("%1 has no unexpired option expirations.").arg(symbol));
            return;
        }
        const int index = std::min<int>(maxExpiries, static_cast<int>(future.size())) - 1;
        runSnapshot(future.at(index));
    }, err);
}

void MarketDataClient::fetchTickerDetails(const QString& ticker, std::function<void(const TickerDetails&)> ok, ErrorHandler err)
{
    const QString symbol = ticker.trimmed().toUpper();
    get(endpoint(QStringLiteral("/v3/reference/tickers/%1").arg(symbol)), [symbol, ok](const QJsonObject& body) {
        const QJsonObject r = body["results"].toObject();
        const QJsonObject branding = r["branding"].toObject();
        TickerDetails d;
        d.ticker = symbol;
        d.name = r["name"].toString();
        d.exchange = r["primary_exchange"].toString();
        d.type = r["type"].toString();
        d.iconUrl = branding["icon_url"].toString();
        d.logoUrl = branding["logo_url"].toString();
        d.description = r["description"].toString();
        d.sicDescription = r["sic_description"].toString();
        d.marketCap = r["market_cap"].toDouble();
        ok(d);
    }, err);
}

void MarketDataClient::fetchImage(const QString& url, std::function<void(const QImage&)> ok, ErrorHandler err)
{
    if (m_apiKey.isEmpty() || url.isEmpty()) {
        err("No image URL or API key.");
        return;
    }
    QNetworkRequest request{ QUrl(url) };
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    request.setTransferTimeout(20000);
    QNetworkReply* reply = m_manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, ok = std::move(ok), err = std::move(err)] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            err(describeFailure(status, QJsonObject(), reply->errorString()));
            return;
        }
        QImage image;
        if (!image.loadFromData(reply->readAll())) {
            err("The branding image could not be decoded.");
            return;
        }
        ok(image);
    });
}

void MarketDataClient::fetchAggregates(const QString& ticker, int multiplier, const QString& timespan, const QDate& from, const QDate& to,
                                       std::function<void(const BarSeries&)> ok, ErrorHandler err)
{
    auto series = std::make_shared<BarSeries>();
    series->ticker = ticker.trimmed().toUpper();
    series->multiplier = multiplier;
    series->timespan = timespan;
    const QUrl url = endpoint(QStringLiteral("/v2/aggs/ticker/%1/range/%2/%3/%4/%5")
                                  .arg(series->ticker).arg(multiplier).arg(timespan, from.toString(Qt::ISODate), to.toString(Qt::ISODate)),
                              { { "adjusted", "true" }, { "sort", "asc" }, { "limit", "50000" } });
    getPaged(url, 20,
             [series](const QJsonArray& results) {
                 for (const QJsonValue v : results) {
                     const QJsonObject o = v.toObject();
                     Bar bar;
                     bar.timeMs = static_cast<qint64>(o["t"].toDouble());
                     bar.open = o["o"].toDouble();
                     bar.high = o["h"].toDouble();
                     bar.low = o["l"].toDouble();
                     bar.close = o["c"].toDouble();
                     bar.volume = o["v"].toDouble();
                     if (bar.timeMs > 0 && bar.close > 0.0) series->bars.push_back(bar);
                 }
             },
             [series, ok, err] {
                 if (series->bars.empty()) {
                     err(QStringLiteral("No price history returned for %1 in that range.").arg(series->ticker));
                     return;
                 }
                 std::sort(series->bars.begin(), series->bars.end(), [](const Bar& a, const Bar& b) { return a.timeMs < b.timeMs; });
                 ok(*series);
             },
             err);
}

void MarketDataClient::fetchQuotes(const QStringList& tickers, std::function<void(const std::vector<Quote>&)> ok, ErrorHandler err)
{
    if (tickers.isEmpty()) {
        ok({});
        return;
    }
    QStringList symbols;
    for (const QString& t : tickers) symbols << t.trimmed().toUpper();
    get(endpoint("/v2/snapshot/locale/us/markets/stocks/tickers", { { "tickers", symbols.join(',') } }),
        [ok](const QJsonObject& body) {
            std::vector<Quote> quotes;
            for (const QJsonValue v : body["tickers"].toArray()) {
                const QJsonObject t = v.toObject();
                Quote q;
                q.ticker = t["ticker"].toString();
                const QJsonObject day = t["day"].toObject();
                const QJsonObject minute = t["min"].toObject();
                const QJsonObject prev = t["prevDay"].toObject();
                q.previousClose = prev["c"].toDouble();
                const double minuteClose = minute["c"].toDouble();
                const double dayClose = day["c"].toDouble();
                q.last = minuteClose > 0.0 ? minuteClose : (dayClose > 0.0 ? dayClose : q.previousClose);
                q.dayOpen = day["o"].toDouble();
                q.dayHigh = day["h"].toDouble();
                q.dayLow = day["l"].toDouble();
                q.dayVolume = day["v"].toDouble();
                // Always our own arithmetic against prevDay.c, so every display agrees on the basis.
                q.change = q.previousClose > 0.0 ? q.last - q.previousClose : 0.0;
                q.changePercent = q.previousClose > 0.0 ? (q.last / q.previousClose - 1.0) * 100.0 : 0.0;
                const qint64 minuteMillis = static_cast<qint64>(minute["t"].toDouble());
                const qint64 updatedNanos = static_cast<qint64>(t["updated"].toDouble());
                if (minuteMillis > 0) q.asOf = QDateTime::fromMSecsSinceEpoch(minuteMillis);
                else if (updatedNanos > 0) q.asOf = QDateTime::fromMSecsSinceEpoch(updatedNanos / 1000000);
                if (!q.ticker.isEmpty()) quotes.push_back(q);
            }
            ok(quotes);
        }, err);
}

void MarketDataClient::fetchGroupedDaily(const QDate& date, std::function<void(const std::map<QString, double>&)> ok, ErrorHandler err)
{
    get(endpoint(QStringLiteral("/v2/aggs/grouped/locale/us/market/stocks/%1").arg(date.toString(Qt::ISODate)), { { "adjusted", "true" } }),
        [ok](const QJsonObject& body) {
            std::map<QString, double> closes;
            for (const QJsonValue v : body["results"].toArray()) {
                const QJsonObject o = v.toObject();
                const QString ticker = o["T"].toString();
                const double close = o["c"].toDouble();
                if (!ticker.isEmpty() && close > 0.0) closes[ticker] = close;
            }
            ok(closes);
        }, err);
}

void MarketDataClient::fetchTreasuryCurve(std::function<void(const TreasuryCurve&)> ok, ErrorHandler err)
{
    get(endpoint("/fed/v1/treasury-yields", { { "limit", "1" }, { "sort", "date.desc" } }),
        [ok, err](const QJsonObject& body) {
            const QJsonArray results = body["results"].toArray();
            if (results.isEmpty()) {
                err("Massive returned no Treasury yield data.");
                return;
            }
            const QJsonObject row = results.first().toObject();
            TreasuryCurve curve;
            curve.date = QDate::fromString(row["date"].toString(), Qt::ISODate);
            const struct { const char* key; double tenor; } tenors[] = {
                { "yield_1_month", 1.0 / 12.0 }, { "yield_2_month", 2.0 / 12.0 }, { "yield_3_month", 0.25 },
                { "yield_4_month", 4.0 / 12.0 }, { "yield_6_month", 0.5 },        { "yield_1_year", 1.0 },
                { "yield_2_year", 2.0 },         { "yield_3_year", 3.0 },         { "yield_5_year", 5.0 },
                { "yield_7_year", 7.0 },         { "yield_10_year", 10.0 },       { "yield_20_year", 20.0 },
                { "yield_30_year", 30.0 },
            };
            for (const auto& t : tenors) {
                if (!row.contains(t.key)) continue;
                const double percent = row[t.key].toDouble();
                if (!std::isfinite(percent)) continue;
                // Treasury yields are bond-equivalent (semi-annual) percentages; convert to
                // the continuously compounded decimal the pricing library expects.
                const double continuous = 2.0 * std::log(1.0 + percent / 200.0);
                curve.points.push_back({ t.tenor, continuous });
            }
            if (curve.points.empty()) {
                err("The Treasury yield row contained no usable tenors.");
                return;
            }
            ok(curve);
        }, err);
}

void MarketDataClient::fetchDividends(const QString& ticker, const QDate& valuationDate, double horizonYears,
                                      std::function<void(const DividendInfo&)> ok, ErrorHandler err)
{
    const QString symbol = ticker.trimmed().toUpper();
    get(endpoint("/v3/reference/dividends", { { "ticker", symbol }, { "limit", "12" }, { "sort", "ex_dividend_date" }, { "order", "desc" } }),
        [ok, valuationDate, horizonYears, symbol](const QJsonObject& body) {
            DividendInfo info;
            const QJsonArray results = body["results"].toArray();
            // Most recent regular cash dividend defines the amount and cadence.
            for (const QJsonValue v : results) {
                const QJsonObject d = v.toObject();
                const QString type = d["dividend_type"].toString();
                if (type != "CD" && !type.isEmpty()) continue;   // skip special dividends
                const double amount = d["cash_amount"].toDouble();
                const int frequency = d["frequency"].toInt();
                const QDate exDate = QDate::fromString(d["ex_dividend_date"].toString(), Qt::ISODate);
                if (amount > 0.0 && frequency > 0 && exDate.isValid()) {
                    info.cashAmount = amount;
                    info.frequency = frequency;
                    info.lastExDate = exDate;
                    break;
                }
            }
            if (info.frequency == 0) {
                ok(info);   // no regular dividend: caller reports it
                return;
            }
            // Project the schedule forward from the last ex-date at the stated cadence.
            const int monthsBetween = std::max(1, 12 / info.frequency);
            QDate next = info.lastExDate;
            // Also include already-announced future ex-dates from the response.
            std::set<QDate> announced;
            for (const QJsonValue v : results) {
                const QDate exDate = QDate::fromString(v.toObject()["ex_dividend_date"].toString(), Qt::ISODate);
                if (exDate.isValid() && exDate > valuationDate) announced.insert(exDate);
            }
            for (int i = 0; i < 400; ++i) {
                next = next.addMonths(monthsBetween);
                if (next <= valuationDate) continue;
                const double years = pricing::yearFraction(civil(valuationDate), civil(next), pricing::DayCount::Actual365);
                if (years > horizonYears) break;
                info.projected.push_back({ years, info.cashAmount });
            }
            for (const QDate& d : announced) {
                const double years = pricing::yearFraction(civil(valuationDate), civil(d), pricing::DayCount::Actual365);
                // Replace the nearest projected date with the announced one when they are within a month.
                bool replaced = false;
                for (pricing::Dividend& p : info.projected) {
                    if (std::fabs(p.time - years) < 31.0 / 365.0) { p.time = years; replaced = true; break; }
                }
                if (!replaced && years <= horizonYears) info.projected.push_back({ years, info.cashAmount });
            }
            std::sort(info.projected.begin(), info.projected.end(), [](const pricing::Dividend& a, const pricing::Dividend& b) { return a.time < b.time; });
            ok(info);
        }, err);
}

void MarketDataClient::fetchEarnings(const QString& ticker, std::function<void(const EarningsInfo&)> ok, ErrorHandler err)
{
    const QString symbol = ticker.trimmed().toUpper();
    const QDate today = QDate::currentDate();
    // Fallback: project the next report from the spacing of the last quarterly filings.
    auto fromFilings = [this, symbol, today, ok, err] {
        get(endpoint("/vX/reference/financials", { { "ticker", symbol }, { "timeframe", "quarterly" }, { "limit", "8" }, { "sort", "filing_date" }, { "order", "desc" } }),
            [ok, err, today](const QJsonObject& body) {
                std::vector<QDate> filings;
                for (const QJsonValue v : body["results"].toArray()) {
                    const QDate d = QDate::fromString(v.toObject()["filing_date"].toString(), Qt::ISODate);
                    if (d.isValid()) filings.push_back(d);
                }
                std::sort(filings.begin(), filings.end());
                filings.erase(std::unique(filings.begin(), filings.end()), filings.end());
                if (filings.empty()) { err("No earnings calendar or filing history is available for this ticker."); return; }
                // Median gap between filings (quarterly companies: ~91 days; annual filers stretch one gap).
                std::vector<qint64> gaps;
                for (size_t i = 1; i < filings.size(); ++i) gaps.push_back(filings[i - 1].daysTo(filings[i]));
                qint64 gap = 91;
                if (!gaps.empty()) { std::sort(gaps.begin(), gaps.end()); gap = std::clamp<qint64>(gaps[gaps.size() / 2], 60, 120); }
                QDate next = filings.back().addDays(gap);
                for (int i = 0; i < 8 && next <= today; ++i) next = next.addDays(gap);
                EarningsInfo info;
                info.date = next;
                info.estimated = true;
                info.source = QStringLiteral("projected from the last filing (%1) at a %2-day cadence").arg(filings.back().toString(Qt::ISODate)).arg(gap);
                ok(info);
            }, err);
    };
    get(endpoint("/benzinga/v1/earnings", { { "ticker", symbol }, { "date.gte", today.toString(Qt::ISODate) }, { "limit", "5" }, { "sort", "date.asc" } }),
        [ok, fromFilings](const QJsonObject& body) {
            EarningsInfo info;
            for (const QJsonValue v : body["results"].toArray()) {
                const QJsonObject e = v.toObject();
                const QDate d = QDate::fromString(e.value("date").toString(e.value("report_date").toString()), Qt::ISODate);
                if (!d.isValid()) continue;
                if (info.date.isValid() && d >= info.date) continue;
                info.date = d;
                // "time" is a wall-clock Eastern time such as "16:00:00"; the status is "confirmed" or "projected".
                const QString time = e.value("time").toString().toLower();
                const int hour = time.contains(':') ? time.section(':', 0, 0).toInt() : -1;
                if (time.contains("before") || time.contains("bmo") || (hour >= 0 && hour < 10)) info.timing = "before the open";
                else if (time.contains("after") || time.contains("amc") || hour >= 16) info.timing = "after the close";
                else if (hour >= 0) info.timing = "during the session";
                info.confirmed = e.value("date_status").toString().compare("confirmed", Qt::CaseInsensitive) == 0;
                info.epsEstimate = e.value("estimated_eps").toDouble(e.value("eps_estimate").toDouble());
                info.revenueEstimate = e.value("estimated_revenue").toDouble();
                if (e.contains("fiscal_period")) info.fiscalPeriod = QStringLiteral("%1 FY%2").arg(e.value("fiscal_period").toString()).arg(e.value("fiscal_year").toInt());
                info.source = QStringLiteral("Benzinga calendar, %1").arg(info.confirmed ? "confirmed" : "projected");
            }
            if (info.date.isValid()) ok(info);
            else fromFilings();
        },
        [fromFilings](const QString&) { fromFilings(); });
}
