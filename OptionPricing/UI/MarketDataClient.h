//
//  MarketDataClient.h
//  OptionPricing
//
//  Asynchronous client for the Massive.com (formerly Polygon.io) REST API: underlying
//  snapshots, option expirations and chain snapshots, US Treasury yields and dividend
//  history. All calls run on the Qt event loop and report back through callbacks.
//
//  Authentication uses a bearer token. The key is read from the MASSIVE_API_KEY or
//  POLYGON_API_KEY environment variable, from the application preferences if the user
//  chose to remember it, or set explicitly for the session.
//

#pragma once

#include "QtHeaders.h"
#include "../Pricing/Csv.h"
#include "../Pricing/RateCurve.h"

#include <functional>
#include <set>
#include <vector>

class MarketDataClient
{
public:
    using ErrorHandler = std::function<void(const QString& message)>;

    struct UnderlyingSnapshot {
        QString ticker;
        double price = 0.0;          ///< most recent trade or close available
        double previousClose = 0.0;
        QString priceSource;         ///< "last minute bar", "day close", "previous close"
        QDateTime asOf;              ///< vendor timestamp of the price (reveals feed delay)
    };

    struct ChainDownload {
        std::vector<pricing::ChainQuote> quotes;
        std::set<QDate> expiries;    ///< distinct expiration dates seen
        int contracts = 0;           ///< contracts returned by the API
        int skipped = 0;             ///< contracts without a usable price or already expired
        int withQuotes = 0;          ///< contracts that carried a bid/ask quote
    };

    struct TreasuryCurve {
        QDate date;
        std::vector<pricing::RatePoint> points;   ///< continuously compounded zero rates
    };

    struct TickerDetails {
        QString ticker;
        QString name;                ///< company name, e.g. "Nvidia Corp"
        QString exchange;            ///< primary exchange MIC, e.g. "XNAS"
        QString type;                ///< "CS" common stock, "ETF", ...
        QString iconUrl;             ///< square icon (PNG/JPEG), empty if none
        QString logoUrl;             ///< wordmark (often SVG), empty if none
        QString description;
        double marketCap = 0.0;
    };

    struct DividendInfo {
        double cashAmount = 0.0;
        int frequency = 0;           ///< payments per year (4 = quarterly)
        QDate lastExDate;
        std::vector<pricing::Dividend> projected;   ///< future dividends in years from the valuation date
    };

    MarketDataClient();

    /// Reads the key from the environment, then from preferences. Returns an empty string if none.
    static QString discoverApiKey(QString* source = nullptr);

    void setApiKey(const QString& key, bool remember);
    static void forgetStoredApiKey();
    bool hasApiKey() const { return !m_apiKey.isEmpty(); }
    QString apiKeySource() const { return m_source; }

    void fetchUnderlying(const QString& ticker, std::function<void(const UnderlyingSnapshot&)> ok, ErrorHandler err);
    void fetchExpirations(const QString& ticker, std::function<void(const QList<QDate>&)> ok, ErrorHandler err);
    /// Downloads the option chain for every unexpired expiration (maxExpiries <= 0) or
    /// for the N nearest ones. The snapshot is paged 250 contracts at a time; `progress`
    /// is called after each page.
    void fetchChains(const QString& ticker, const QDate& valuationDate, int maxExpiries,
                     std::function<void(int pages, int contracts)> progress,
                     std::function<void(const ChainDownload&)> ok, ErrorHandler err);
    void fetchTreasuryCurve(std::function<void(const TreasuryCurve&)> ok, ErrorHandler err);
    void fetchTickerDetails(const QString& ticker, std::function<void(const TickerDetails&)> ok, ErrorHandler err);
    /// Downloads an image behind the API (branding icons need the bearer token).
    void fetchImage(const QString& url, std::function<void(const QImage&)> ok, ErrorHandler err);
    void fetchDividends(const QString& ticker, const QDate& valuationDate, double horizonYears,
                        std::function<void(const DividendInfo&)> ok, ErrorHandler err);

private:
    void get(const QUrl& url, std::function<void(const QJsonObject&)> ok, ErrorHandler err);
    /// Follows `next_url` pagination, delivering each page's `results` array.
    void getPaged(const QUrl& url, int maxPages, std::function<void(const QJsonArray&)> onPage,
                  std::function<void()> done, ErrorHandler err);
    static QString describeFailure(int httpStatus, const QJsonObject& body, const QString& transportError);

    QNetworkAccessManager m_manager;
    QString m_apiKey;
    QString m_source;
};
