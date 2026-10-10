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
#include <map>
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
        QString sicDescription;      ///< industry classification text, e.g. "Services-Prepackaged Software"
        double marketCap = 0.0;
    };

    /// One OHLCV aggregate bar.
    struct Bar {
        qint64 timeMs = 0;           ///< bar start, UTC epoch milliseconds
        double open = 0, high = 0, low = 0, close = 0, volume = 0;
    };

    struct BarSeries {
        QString ticker;
        int multiplier = 1;
        QString timespan;            ///< "minute", "hour", "day", "week"
        std::vector<Bar> bars;       ///< ascending by time
    };

    /// Bulk stock snapshot row for the watchlist.
    struct Quote {
        QString ticker;
        double last = 0.0;
        double previousClose = 0.0;
        double change = 0.0;
        double changePercent = 0.0;
        double dayOpen = 0.0, dayHigh = 0.0, dayLow = 0.0, dayVolume = 0.0;
        QDateTime asOf;
    };

    /// One listed (or expired) option contract from the reference data.
    struct OptionContract {
        QString ticker;              ///< e.g. "O:AAPL260918C00255000"
        double strike = 0.0;
        pricing::OptionType type = pricing::OptionType::Call;
        QDate expiry;
    };

    /// Next scheduled earnings report.
    struct EarningsInfo {
        QDate date;
        QString timing;              ///< "before the open", "after the close" or empty
        double epsEstimate = 0.0;    ///< consensus EPS, 0 when unknown
        double revenueEstimate = 0.0;///< consensus revenue, 0 when unknown
        QString fiscalPeriod;        ///< e.g. "Q4 FY2026"
        bool confirmed = false;      ///< the company confirmed the date (otherwise the calendar's projection)
        bool estimated = false;      ///< projected from the filing cadence rather than a published calendar
        QString source;
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
    /// Aggregate bars between two dates (inclusive), following pagination.
    void fetchAggregates(const QString& ticker, int multiplier, const QString& timespan, const QDate& from, const QDate& to,
                         std::function<void(const BarSeries&)> ok, ErrorHandler err);
    /// Snapshot quotes for many tickers in one request.
    void fetchQuotes(const QStringList& tickers, std::function<void(const std::vector<Quote>&)> ok, ErrorHandler err);
    /// Closing prices of every US stock for one session (grouped daily aggregates): ticker -> close.
    /// Empty map when the date was not a trading day.
    void fetchGroupedDaily(const QDate& date, std::function<void(const std::map<QString, double>&)> ok, ErrorHandler err);
    /// Downloads an image behind the API (branding icons need the bearer token).
    void fetchImage(const QString& url, std::function<void(const QImage&)> ok, ErrorHandler err);
    void fetchDividends(const QString& ticker, const QDate& valuationDate, double horizonYears,
                        std::function<void(const DividendInfo&)> ok, ErrorHandler err);
    /// Next earnings date: the Benzinga calendar when the plan carries it, otherwise projected
    /// from the cadence of the company's quarterly filings (flagged `estimated`).
    void fetchEarnings(const QString& ticker, std::function<void(const EarningsInfo&)> ok, ErrorHandler err);
    /// Contracts of `underlying` expiring on `expiry` with strikes in [strikeLo, strikeHi], including expired ones.
    void fetchOptionContracts(const QString& underlying, const QDate& expiry, double strikeLo, double strikeHi,
                              std::function<void(const std::vector<OptionContract>&)> ok, ErrorHandler err);

    // ---- Futures (Massive /futures/v1, real-time) ----
    // App-wide convention, as on trading platforms: a leading "/" marks a futures symbol.
    // "/ES" is the front month of the E-mini S&P 500, "/ESZ6" a specific contract. The
    // quote, aggregate, underlying and ticker-details fetches accept these symbols and route
    // to the futures endpoints, so charts, watchlists, portfolios and risk work unchanged;
    // option-chain fetches report that options on futures are not available yet.
    struct FuturesProduct {
        QString code;              ///< product code, e.g. "ES"
        QString name;              ///< "E-mini S&P 500"
        QString category;          ///< "Equity index", "Energy", "Metals", "Rates", "Grains", "Meats", "FX", "Crypto"
        double multiplier = 1.0;   ///< contract unit (unit_of_measure_qty): 50 for ES
        QString unit;              ///< "index points", "barrels", ...
        QString venue;             ///< exchange MIC, e.g. "XCME"
    };
    struct FuturesContract {
        QString symbol;            ///< the app symbol asked for, e.g. "/ES" or "/ESZ6"
        QString ticker;            ///< vendor contract ticker, e.g. "ESZ6"
        QString productCode;       ///< "ES"
        QString name;              ///< "ESZ6 Future"
        QDate lastTradeDate;
        QDate settlementDate;
        int daysToMaturity = 0;
        double tickSize = 0.0;
    };
    /// True for a futures symbol in either form: with the platform-style slash ("/ES", "/CLX6")
    /// or Massive's bare contract ticker ("CLX6", "ESZ6", "6EZ6": a known product root, a month
    /// code F G H J K M N Q U V X Z and one or two year digits). A bare product root alone
    /// ("ES", "CL") stays a stock symbol, since those collide with listed companies.
    static bool isFutures(const QString& symbol);
    /// "/ESZ6" -> "ES", "/6EZ6" -> "6E", "/ES" -> "ES".
    static QString futuresProductCode(const QString& symbol);
    /// "ESZ6" -> "Dec 2026" (empty for a product without a month code).
    static QString contractLabel(const QString& ticker);
    static const std::vector<FuturesProduct>& knownFuturesProducts();
    static const FuturesProduct* knownFuturesProduct(const QString& code);
    /// Resolves "/ES" to its front-month contract (the nearest expiry with at least a week
    /// left, otherwise the next one). An explicit contract ("ESZ6", "NGX6", "NGX26") is matched
    /// by month and year against the product's listed contracts, so a one-digit year maps to
    /// the vendor's ticker whatever its convention (NGX6 -> NGX26, ESZ26 -> ESZ6); a ticker that
    /// already matches passes through. Cached per session day.
    void resolveFutures(const QString& symbol, std::function<void(const FuturesContract&)> ok, ErrorHandler err);
    /// Product specification: the built-in table, else the products endpoint (multiplier, unit, venue).
    void fetchFuturesProduct(const QString& code, std::function<void(const FuturesProduct&)> ok, ErrorHandler err);

private:
    void fetchFuturesQuotes(const QStringList& symbols, std::function<void(const std::vector<Quote>&)> ok, ErrorHandler err);
    void fetchFuturesAggregates(const QString& symbol, int multiplier, const QString& timespan, const QDate& from, const QDate& to,
                                std::function<void(const BarSeries&)> ok, ErrorHandler err);
    void fetchFuturesUnderlying(const QString& symbol, std::function<void(const UnderlyingSnapshot&)> ok, ErrorHandler err);
    void fetchFuturesDetails(const QString& symbol, std::function<void(const TickerDetails&)> ok, ErrorHandler err);
    /// Listed outright contracts of a product (sorted by days to maturity), cached per session day.
    void loadFuturesContracts(const QString& productCode, std::function<void(const std::vector<FuturesContract>&)> ok, ErrorHandler err);
    std::map<QString, std::vector<FuturesContract>> m_contracts;   ///< product code -> contracts listed today
    QDate m_frontMonthDate;                                        ///< session day the cache belongs to
    std::map<QString, FuturesProduct> m_products;       ///< product code -> specification from the API

    void get(const QUrl& url, std::function<void(const QJsonObject&)> ok, ErrorHandler err);
    /// GET with up to two retries (back-off 0.8 s, 3.2 s) on transient failures: network
    /// errors including HTTP/2 GOAWAY cancellations and timeouts, 429 and 5xx responses.
    void getWithRetry(const QUrl& url, int attempt, std::function<void(const QJsonObject&)> ok, ErrorHandler err);
    /// Follows `next_url` pagination, delivering each page's `results` array.
    void getPaged(const QUrl& url, int maxPages, std::function<void(const QJsonArray&)> onPage,
                  std::function<void()> done, ErrorHandler err);
    static QString describeFailure(int httpStatus, const QJsonObject& body, const QString& transportError);

    QNetworkAccessManager m_manager;
    QString m_apiKey;
    QString m_source;
};
