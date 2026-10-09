//
//  ChainStore.h
//  OptionPricing
//
//  In-memory option-chain database (SQLite via QtSql) plus a background preloader.
//  Every watchlist ticker's full chain and underlying snapshot is downloaded once at
//  start-up and kept in SQLite tables, so switching tickers anywhere in the app applies a
//  stored chain instantly; stale chains are refreshed in the background.
//

#pragma once

#include "QtHeaders.h"
#include "MarketDataClient.h"

#include <QtSql/QSqlDatabase>

#include <functional>
#include <map>
#include <optional>
#include <vector>

class ChainStore
{
public:
    struct StoredChain {
        QString ticker;
        MarketDataClient::UnderlyingSnapshot snapshot;
        MarketDataClient::ChainDownload download;
        QDateTime fetchedAt;
    };

    struct Summary {
        QString ticker;
        QDateTime fetchedAt;
        int contracts = 0;
        int expiries = 0;
        double spot = 0.0;
    };

    ChainStore();
    ~ChainStore();
    ChainStore(const ChainStore&) = delete;
    ChainStore& operator=(const ChainStore&) = delete;

    bool isOpen() const { return m_open; }
    QString lastError() const { return m_lastError; }

    // ---- Persistence between sessions ----
    /// Default on-disk location (Application Support); created on demand.
    static QString defaultCachePath();
    /// Writes the whole in-memory database to `path` (SQLite VACUUM INTO). Called on exit
    /// and after each preload pass.
    bool saveTo(const QString& path);
    /// Copies chains, contracts and quotes from a database saved by saveTo() into memory.
    /// Returns the number of chains loaded (0 when the file is absent or unreadable).
    int loadFrom(const QString& path);
    /// When the database was last saved to disk (from the loaded file), if known.
    QDateTime savedAt() const { return m_savedAt; }

    // ---- Underlying quotes (watchlist snapshot) ----
    bool putQuotes(const std::vector<MarketDataClient::Quote>& quotes);
    std::vector<MarketDataClient::Quote> quotes() const;
    QDateTime quotesFetchedAt() const;

    // ---- Storage ----
    bool put(const StoredChain& chain);
    std::optional<StoredChain> get(const QString& ticker) const;
    bool contains(const QString& ticker) const;
    QDateTime fetchedAt(const QString& ticker) const;
    /// Age of the stored chain in seconds; -1 when absent.
    qint64 ageSeconds(const QString& ticker) const;
    std::vector<Summary> summaries() const;
    void remove(const QString& ticker);
    int tickerCount() const;
    int contractCount() const;
    /// Bytes used by the SQLite pages (PRAGMA page_count * page_size).
    qint64 approximateBytes() const;

    // ---- Preloading (Massive.com, two downloads in flight at a time) ----
    /// Queues every ticker that has no stored chain, or whose chain is older than `maxAgeSeconds`.
    void preload(const QStringList& tickers, qint64 maxAgeSeconds = 0);
    /// Queues a ticker at the front; `done` fires when its chain is stored (or the fetch fails).
    void refresh(const QString& ticker, std::function<void(bool ok, const QString& message)> done = {});
    bool isFetching(const QString& ticker) const;
    bool isQueued(const QString& ticker) const;
    int pending() const { return static_cast<int>(m_queue.size() + m_active.size()); }
    bool busy() const { return pending() > 0; }
    void refreshApiKey();
    bool hasApiKey() const { return m_client.hasApiKey(); }

    std::function<void(const QString& ticker, int done, int total)> onProgress;
    std::function<void(const QString& ticker, bool ok, const QString& message)> onChainStored;
    std::function<void()> onIdle;

private:
    bool createSchema();
    void pump();
    void fetchOne(const QString& ticker);
    void finishOne(const QString& ticker, bool ok, const QString& message);

    QString m_connectionName;
    QSqlDatabase m_db;
    bool m_open = false;
    QString m_lastError;
    QDateTime m_savedAt;

    MarketDataClient m_client;
    QStringList m_queue;
    QSet<QString> m_active;
    std::map<QString, std::vector<std::function<void(bool, const QString&)>>> m_waiters;
    int m_concurrency = 2;
    int m_total = 0;
    int m_done = 0;
};
