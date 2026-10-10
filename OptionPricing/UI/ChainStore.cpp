//
//  ChainStore.cpp
//  OptionPricing
//

#include "ChainStore.h"
#include "../Pricing/IvHistory.h"

#include <QtCore/QStandardPaths>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

using namespace pricing;

namespace {
int gConnectionCounter = 0;
} // namespace

ChainStore::ChainStore()
    : m_connectionName(QStringLiteral("chainstore-%1").arg(++gConnectionCounter))
{
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(":memory:");
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return;
    }
    m_open = createSchema();
}

ChainStore::~ChainStore()
{
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool ChainStore::createSchema()
{
    QSqlQuery q(m_db);
    const char* statements[] = {
        "PRAGMA journal_mode = MEMORY",
        "PRAGMA synchronous = OFF",
        "CREATE TABLE IF NOT EXISTS chains ("
        "  ticker TEXT PRIMARY KEY, fetched_at TEXT NOT NULL, spot REAL, previous_close REAL, price_source TEXT, spot_as_of TEXT,"
        "  contracts INTEGER, skipped INTEGER, with_quotes INTEGER)",
        "CREATE TABLE IF NOT EXISTS contracts ("
        "  ticker TEXT NOT NULL, type INTEGER NOT NULL, strike REAL NOT NULL, maturity REAL NOT NULL, expiry TEXT, dte INTEGER,"
        "  bid REAL, ask REAL, mid REAL, volume REAL, open_interest REAL, vendor_iv REAL)",
        "CREATE INDEX IF NOT EXISTS idx_contracts_ticker ON contracts(ticker, maturity, strike)",
        "CREATE TABLE IF NOT EXISTS quotes ("
        "  ticker TEXT PRIMARY KEY, last REAL, previous_close REAL, change REAL, change_pct REAL,"
        "  day_open REAL, day_high REAL, day_low REAL, day_volume REAL, as_of TEXT, fetched_at TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)",
        "CREATE TABLE IF NOT EXISTS iv_history ("
        "  ticker TEXT NOT NULL, date TEXT NOT NULL, iv30 REAL, iv_near REAL, spot REAL, source TEXT, PRIMARY KEY (ticker, date))",
    };
    for (const char* sql : statements) {
        if (!q.exec(QString::fromLatin1(sql))) {
            m_lastError = q.lastError().text();
            return false;
        }
    }
    return true;
}

// MARK: - Storage

bool ChainStore::put(const StoredChain& chain)
{
    if (!m_open || chain.ticker.isEmpty()) return false;
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    QSqlQuery del(m_db);
    del.prepare("DELETE FROM contracts WHERE ticker = ?");
    del.addBindValue(chain.ticker);
    bool ok = del.exec();

    QSqlQuery head(m_db);
    head.prepare("INSERT OR REPLACE INTO chains (ticker, fetched_at, spot, previous_close, price_source, spot_as_of, contracts, skipped, with_quotes)"
                 " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    head.addBindValue(chain.ticker);
    head.addBindValue(chain.fetchedAt.toString(Qt::ISODateWithMs));
    head.addBindValue(chain.snapshot.price);
    head.addBindValue(chain.snapshot.previousClose);
    head.addBindValue(chain.snapshot.priceSource);
    head.addBindValue(chain.snapshot.asOf.isValid() ? chain.snapshot.asOf.toString(Qt::ISODateWithMs) : QString());
    head.addBindValue(chain.download.contracts);
    head.addBindValue(chain.download.skipped);
    head.addBindValue(chain.download.withQuotes);
    ok = ok && head.exec();

    if (ok && !chain.download.quotes.empty()) {
        // One batched insert: QtSql binds each column as a list and executes once per row inside the transaction.
        QVariantList tickers, types, strikes, maturities, expiries, dtes, bids, asks, mids, volumes, ois, ivs;
        const int n = static_cast<int>(chain.download.quotes.size());
        for (QVariantList* list : { &tickers, &types, &strikes, &maturities, &expiries, &dtes, &bids, &asks, &mids, &volumes, &ois, &ivs }) list->reserve(n);
        for (const ChainQuote& c : chain.download.quotes) {
            tickers << chain.ticker;
            types << (c.type == OptionType::Call ? 0 : 1);
            strikes << c.strike;
            maturities << c.maturity;
            expiries << QString::fromStdString(c.expiryDate);
            dtes << c.daysToExpiry;
            bids << c.bid;
            asks << c.ask;
            mids << c.mid;
            volumes << c.volume;
            ois << c.openInterest;
            ivs << c.vendorImpliedVol;
        }
        QSqlQuery ins(m_db);
        ins.prepare("INSERT INTO contracts (ticker, type, strike, maturity, expiry, dte, bid, ask, mid, volume, open_interest, vendor_iv)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        for (QVariantList* list : { &tickers, &types, &strikes, &maturities, &expiries, &dtes, &bids, &asks, &mids, &volumes, &ois, &ivs }) ins.addBindValue(*list);
        ok = ins.execBatch();
        if (!ok) m_lastError = ins.lastError().text();
    }
    if (!ok) {
        if (m_lastError.isEmpty()) m_lastError = head.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) return false;
    // Today's implied-vol sample: the chain's 30-day constant-maturity ATM vol.
    if (chain.snapshot.price > 0.0 && !chain.download.quotes.empty()) {
        pricing::ActivityMarket am;
        am.spot = chain.snapshot.price;
        am.rateFor = rateFor;
        const pricing::ivhist::Sample s = pricing::ivhist::sampleFromChain(QDate::currentDate().toString(Qt::ISODate).toStdString(), chain.download.quotes, am);
        if (s.iv30 > 0.0) {
            IvPoint p;
            p.date = QDate::currentDate();
            p.iv30 = s.iv30;
            p.ivNear = s.ivNear;
            p.spot = s.spot;
            p.source = "snapshot";
            putIvSamples(chain.ticker, { p }, true);
        }
    }
    return true;
}

// MARK: - Implied-vol history

bool ChainStore::putIvSamples(const QString& ticker, const std::vector<IvPoint>& points, bool replace)
{
    if (!m_open || ticker.isEmpty() || points.empty()) return false;
    if (!m_db.transaction()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR %1 INTO iv_history (ticker, date, iv30, iv_near, spot, source) VALUES (?, ?, ?, ?, ?, ?)").arg(replace ? "REPLACE" : "IGNORE"));
    bool ok = true;
    for (const IvPoint& p : points) {
        if (!p.date.isValid() || p.iv30 <= 0.0) continue;
        q.addBindValue(ticker);
        q.addBindValue(p.date.toString(Qt::ISODate));
        q.addBindValue(p.iv30);
        q.addBindValue(p.ivNear);
        q.addBindValue(p.spot);
        q.addBindValue(p.source);
        ok = q.exec() && ok;
    }
    if (!ok) { m_lastError = q.lastError().text(); m_db.rollback(); return false; }
    return m_db.commit();
}

std::vector<ChainStore::IvPoint> ChainStore::ivHistory(const QString& ticker, int maxPoints) const
{
    std::vector<IvPoint> out;
    if (!m_open) return out;
    QSqlQuery q(m_db);
    q.prepare("SELECT date, iv30, iv_near, spot, source FROM iv_history WHERE ticker = ? ORDER BY date DESC LIMIT ?");
    q.addBindValue(ticker);
    q.addBindValue(maxPoints);
    if (!q.exec()) return out;
    while (q.next()) {
        IvPoint p;
        p.date = QDate::fromString(q.value(0).toString(), Qt::ISODate);
        p.iv30 = q.value(1).toDouble();
        p.ivNear = q.value(2).toDouble();
        p.spot = q.value(3).toDouble();
        p.source = q.value(4).toString();
        out.push_back(p);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

int ChainStore::ivHistoryCount(const QString& ticker) const
{
    if (!m_open) return 0;
    QSqlQuery q(m_db);
    q.prepare("SELECT COUNT(*) FROM iv_history WHERE ticker = ?");
    q.addBindValue(ticker);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

std::optional<ChainStore::StoredChain> ChainStore::get(const QString& ticker) const
{
    if (!m_open) return std::nullopt;
    QSqlQuery head(m_db);
    head.prepare("SELECT fetched_at, spot, previous_close, price_source, spot_as_of, contracts, skipped, with_quotes FROM chains WHERE ticker = ?");
    head.addBindValue(ticker);
    if (!head.exec() || !head.next()) return std::nullopt;

    StoredChain chain;
    chain.ticker = ticker;
    chain.fetchedAt = QDateTime::fromString(head.value(0).toString(), Qt::ISODateWithMs);
    chain.snapshot.ticker = ticker;
    chain.snapshot.price = head.value(1).toDouble();
    chain.snapshot.previousClose = head.value(2).toDouble();
    chain.snapshot.priceSource = head.value(3).toString();
    const QString asOf = head.value(4).toString();
    if (!asOf.isEmpty()) chain.snapshot.asOf = QDateTime::fromString(asOf, Qt::ISODateWithMs);
    chain.download.contracts = head.value(5).toInt();
    chain.download.skipped = head.value(6).toInt();
    chain.download.withQuotes = head.value(7).toInt();

    QSqlQuery rows(m_db);
    rows.prepare("SELECT type, strike, maturity, expiry, dte, bid, ask, mid, volume, open_interest, vendor_iv FROM contracts WHERE ticker = ? ORDER BY maturity, strike, type");
    rows.addBindValue(ticker);
    if (!rows.exec()) return std::nullopt;
    chain.download.quotes.reserve(static_cast<size_t>(std::max(0, chain.download.contracts)));
    const QDate today = QDate::currentDate();
    while (rows.next()) {
        ChainQuote c;
        c.type = rows.value(0).toInt() == 0 ? OptionType::Call : OptionType::Put;
        c.strike = rows.value(1).toDouble();
        c.maturity = rows.value(2).toDouble();
        c.expiryDate = rows.value(3).toString().toStdString();
        c.daysToExpiry = rows.value(4).toInt();
        // A chain restored from an earlier session: re-measure time to expiry from today
        // (ACT/365, as the chain parser does) and drop contracts that have since expired.
        const QDate expiryDate = QDate::fromString(rows.value(3).toString(), Qt::ISODate);
        if (expiryDate.isValid()) {
            const qint64 dte = today.daysTo(expiryDate);
            if (dte <= 0) continue;
            c.daysToExpiry = static_cast<int>(dte);
            c.maturity = static_cast<double>(dte) / 365.0;
        }
        c.bid = rows.value(5).toDouble();
        c.ask = rows.value(6).toDouble();
        c.mid = rows.value(7).toDouble();
        c.volume = rows.value(8).toDouble();
        c.openInterest = rows.value(9).toDouble();
        c.vendorImpliedVol = rows.value(10).toDouble();
        chain.download.quotes.push_back(c);
        const QDate expiry = QDate::fromString(rows.value(3).toString(), Qt::ISODate);
        if (expiry.isValid()) chain.download.expiries.insert(expiry);
    }
    return chain;
}

bool ChainStore::contains(const QString& ticker) const
{
    return fetchedAt(ticker).isValid();
}

QDateTime ChainStore::fetchedAt(const QString& ticker) const
{
    if (!m_open) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT fetched_at FROM chains WHERE ticker = ?");
    q.addBindValue(ticker);
    if (!q.exec() || !q.next()) return {};
    return QDateTime::fromString(q.value(0).toString(), Qt::ISODateWithMs);
}

qint64 ChainStore::ageSeconds(const QString& ticker) const
{
    const QDateTime at = fetchedAt(ticker);
    return at.isValid() ? at.secsTo(QDateTime::currentDateTime()) : -1;
}

std::vector<ChainStore::Summary> ChainStore::summaries() const
{
    std::vector<Summary> out;
    if (!m_open) return out;
    QSqlQuery q(m_db);
    if (!q.exec("SELECT c.ticker, c.fetched_at, c.spot, COUNT(k.strike), COUNT(DISTINCT k.expiry)"
                " FROM chains c LEFT JOIN contracts k ON k.ticker = c.ticker GROUP BY c.ticker ORDER BY c.ticker")) {
        return out;
    }
    while (q.next()) {
        Summary s;
        s.ticker = q.value(0).toString();
        s.fetchedAt = QDateTime::fromString(q.value(1).toString(), Qt::ISODateWithMs);
        s.spot = q.value(2).toDouble();
        s.contracts = q.value(3).toInt();
        s.expiries = q.value(4).toInt();
        out.push_back(s);
    }
    return out;
}

void ChainStore::remove(const QString& ticker)
{
    if (!m_open) return;
    for (const char* sql : { "DELETE FROM contracts WHERE ticker = ?", "DELETE FROM chains WHERE ticker = ?" }) {
        QSqlQuery q(m_db);
        q.prepare(QString::fromLatin1(sql));
        q.addBindValue(ticker);
        q.exec();
    }
}

int ChainStore::tickerCount() const
{
    if (!m_open) return 0;
    QSqlQuery q(m_db);
    return (q.exec("SELECT COUNT(*) FROM chains") && q.next()) ? q.value(0).toInt() : 0;
}

int ChainStore::contractCount() const
{
    if (!m_open) return 0;
    QSqlQuery q(m_db);
    return (q.exec("SELECT COUNT(*) FROM contracts") && q.next()) ? q.value(0).toInt() : 0;
}

qint64 ChainStore::approximateBytes() const
{
    if (!m_open) return 0;
    qint64 pages = 0, pageSize = 0;
    QSqlQuery q(m_db);
    if (q.exec("PRAGMA page_count") && q.next()) pages = q.value(0).toLongLong();
    if (q.exec("PRAGMA page_size") && q.next()) pageSize = q.value(0).toLongLong();
    return pages * pageSize;
}

// MARK: - Persistence

QString ChainStore::defaultCachePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    return dir + "/chains.sqlite";
}

bool ChainStore::saveTo(const QString& path)
{
    if (!m_open || path.isEmpty()) return false;
    {
        QSqlQuery meta(m_db);
        meta.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES ('saved_at', ?)");
        meta.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        meta.exec();
    }
    // VACUUM INTO refuses to overwrite: write a temporary file, then swap it in.
    const QString temp = path + ".tmp";
    QFile::remove(temp);
    QSqlQuery q(m_db);
    QString escaped = temp;
    escaped.replace('\'', "''");
    if (!q.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped))) {
        m_lastError = q.lastError().text();
        QFile::remove(temp);
        return false;
    }
    QFile::remove(path);
    if (!QFile::rename(temp, path)) {
        m_lastError = QStringLiteral("could not replace %1").arg(path);
        QFile::remove(temp);
        return false;
    }
    m_savedAt = QDateTime::currentDateTime();
    return true;
}

int ChainStore::loadFrom(const QString& path)
{
    if (!m_open || path.isEmpty() || !QFile::exists(path)) return 0;
    QString escaped = path;
    escaped.replace('\'', "''");
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("ATTACH DATABASE '%1' AS disk").arg(escaped))) {
        m_lastError = q.lastError().text();
        return 0;
    }
    int loaded = 0;
    bool ok = m_db.transaction();
    QStringList copies = {
        "INSERT OR REPLACE INTO chains SELECT * FROM disk.chains",
        "DELETE FROM contracts WHERE ticker IN (SELECT ticker FROM disk.chains)",
        "INSERT INTO contracts SELECT * FROM disk.contracts",
        "INSERT OR REPLACE INTO quotes SELECT * FROM disk.quotes",
        "INSERT OR REPLACE INTO meta SELECT * FROM disk.meta",
    };
    // Files written before the implied-vol history existed have no iv_history table.
    if (q.exec("SELECT COUNT(*) FROM disk.sqlite_master WHERE type = 'table' AND name = 'iv_history'") && q.next() && q.value(0).toInt() > 0) {
        copies << "INSERT OR REPLACE INTO iv_history SELECT * FROM disk.iv_history";
    }
    for (const QString& sql : copies) {
        if (ok && !q.exec(sql)) {
            // Older or damaged files: give up on this file rather than half-load it.
            m_lastError = q.lastError().text();
            ok = false;
        }
    }
    if (ok) {
        ok = m_db.commit();
        if (q.exec("SELECT COUNT(*) FROM disk.chains") && q.next()) loaded = q.value(0).toInt();
        if (q.exec("SELECT value FROM meta WHERE key = 'saved_at'") && q.next()) m_savedAt = QDateTime::fromString(q.value(0).toString(), Qt::ISODateWithMs);
    } else {
        m_db.rollback();
        loaded = 0;
    }
    q.finish();
    QSqlQuery detach(m_db);
    detach.exec("DETACH DATABASE disk");
    if (!ok) {
        // Start clean next time.
        QFile::remove(path);
        for (const char* sql : { "DELETE FROM contracts", "DELETE FROM chains", "DELETE FROM quotes", "DELETE FROM iv_history" }) { QSqlQuery d(m_db); d.exec(QString::fromLatin1(sql)); }
    }
    return ok ? loaded : 0;
}

// MARK: - Underlying quotes

bool ChainStore::putQuotes(const std::vector<MarketDataClient::Quote>& quotes)
{
    if (!m_open || quotes.empty()) return false;
    if (!m_db.transaction()) return false;
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    bool ok = true;
    for (const MarketDataClient::Quote& quote : quotes) {
        QSqlQuery q(m_db);
        q.prepare("INSERT OR REPLACE INTO quotes (ticker, last, previous_close, change, change_pct, day_open, day_high, day_low, day_volume, as_of, fetched_at)"
                  " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        q.addBindValue(quote.ticker);
        q.addBindValue(quote.last);
        q.addBindValue(quote.previousClose);
        q.addBindValue(quote.change);
        q.addBindValue(quote.changePercent);
        q.addBindValue(quote.dayOpen);
        q.addBindValue(quote.dayHigh);
        q.addBindValue(quote.dayLow);
        q.addBindValue(quote.dayVolume);
        q.addBindValue(quote.asOf.isValid() ? quote.asOf.toString(Qt::ISODateWithMs) : QString());
        q.addBindValue(now);
        ok = q.exec() && ok;
    }
    if (!ok) {
        m_db.rollback();
        return false;
    }
    return m_db.commit();
}

std::vector<MarketDataClient::Quote> ChainStore::quotes() const
{
    std::vector<MarketDataClient::Quote> out;
    if (!m_open) return out;
    QSqlQuery q(m_db);
    if (!q.exec("SELECT ticker, last, previous_close, change, change_pct, day_open, day_high, day_low, day_volume, as_of FROM quotes ORDER BY ticker")) return out;
    while (q.next()) {
        MarketDataClient::Quote quote;
        quote.ticker = q.value(0).toString();
        quote.last = q.value(1).toDouble();
        quote.previousClose = q.value(2).toDouble();
        quote.change = q.value(3).toDouble();
        quote.changePercent = q.value(4).toDouble();
        quote.dayOpen = q.value(5).toDouble();
        quote.dayHigh = q.value(6).toDouble();
        quote.dayLow = q.value(7).toDouble();
        quote.dayVolume = q.value(8).toDouble();
        const QString asOf = q.value(9).toString();
        if (!asOf.isEmpty()) quote.asOf = QDateTime::fromString(asOf, Qt::ISODateWithMs);
        out.push_back(quote);
    }
    return out;
}

QDateTime ChainStore::quotesFetchedAt() const
{
    if (!m_open) return {};
    QSqlQuery q(m_db);
    if (!q.exec("SELECT MAX(fetched_at) FROM quotes") || !q.next()) return {};
    return QDateTime::fromString(q.value(0).toString(), Qt::ISODateWithMs);
}

// MARK: - Preloading

void ChainStore::refreshApiKey()
{
    if (!m_client.hasApiKey()) {
        const QString key = MarketDataClient::discoverApiKey();
        if (!key.isEmpty()) m_client.setApiKey(key, false);
    }
}

void ChainStore::preload(const QStringList& tickers, qint64 maxAgeSeconds)
{
    refreshApiKey();
    if (!m_client.hasApiKey()) return;
    for (const QString& raw : tickers) {
        const QString ticker = raw.trimmed().toUpper();
        if (ticker.isEmpty() || isQueued(ticker) || isFetching(ticker)) continue;
        const qint64 age = ageSeconds(ticker);
        if (age >= 0 && (maxAgeSeconds <= 0 || age < maxAgeSeconds)) continue;   // fresh enough
        m_queue << ticker;
        ++m_total;
    }
    pump();
}

void ChainStore::refresh(const QString& raw, std::function<void(bool, const QString&)> done)
{
    refreshApiKey();
    const QString ticker = raw.trimmed().toUpper();
    if (ticker.isEmpty() || !m_client.hasApiKey()) {
        if (done) done(false, ticker.isEmpty() ? QStringLiteral("No ticker") : QStringLiteral("No Massive API key"));
        return;
    }
    if (done) m_waiters[ticker].push_back(std::move(done));
    if (isFetching(ticker)) return;
    if (m_queue.contains(ticker)) {
        m_queue.removeAll(ticker);     // already counted; just move it to the front
    } else {
        ++m_total;
    }
    m_queue.prepend(ticker);
    pump();
}

bool ChainStore::isFetching(const QString& ticker) const
{
    return m_active.contains(ticker.trimmed().toUpper());
}

bool ChainStore::isQueued(const QString& ticker) const
{
    return m_queue.contains(ticker.trimmed().toUpper());
}

void ChainStore::pump()
{
    while (static_cast<int>(m_active.size()) < m_concurrency && !m_queue.isEmpty()) {
        const QString ticker = m_queue.takeFirst();
        m_active.insert(ticker);
        fetchOne(ticker);
    }
    if (m_active.isEmpty() && m_queue.isEmpty()) {
        m_total = 0;
        m_done = 0;
        if (onIdle) onIdle();
    }
}

void ChainStore::fetchOne(const QString& ticker)
{
    const QDate today = QDate::currentDate();
    m_client.fetchUnderlying(ticker, [this, ticker, today](const MarketDataClient::UnderlyingSnapshot& snap) {
        m_client.fetchChains(ticker, today, 0, [](int, int) {},
            [this, ticker, snap](const MarketDataClient::ChainDownload& download) {
                StoredChain chain;
                chain.ticker = ticker;
                chain.snapshot = snap;
                chain.download = download;
                chain.fetchedAt = QDateTime::currentDateTime();
                const bool stored = put(chain);
                finishOne(ticker, stored, stored ? QStringLiteral("%1: %2 contracts across %3 expiries stored").arg(ticker).arg(download.quotes.size()).arg(download.expiries.size())
                                                 : QStringLiteral("%1: could not store the chain (%2)").arg(ticker, m_lastError));
            },
            [this, ticker](const QString& message) { finishOne(ticker, false, QStringLiteral("%1: %2").arg(ticker, message)); });
    }, [this, ticker](const QString& message) { finishOne(ticker, false, QStringLiteral("%1: %2").arg(ticker, message)); });
}

void ChainStore::finishOne(const QString& ticker, bool ok, const QString& message)
{
    m_active.remove(ticker);
    ++m_done;
    if (onProgress) onProgress(ticker, m_done, std::max(m_total, m_done));
    if (onChainStored) onChainStored(ticker, ok, message);
    const auto waiters = m_waiters.find(ticker);
    if (waiters != m_waiters.end()) {
        const auto callbacks = std::move(waiters->second);
        m_waiters.erase(waiters);
        for (const auto& cb : callbacks) cb(ok, message);
    }
    pump();
}
