//
//  SectorHeatmapTab.h
//  OptionPricing
//
//  Market treemap in the style of professional terminals: large-cap stocks grouped by
//  sector, each tile sized by market capitalisation and coloured by performance over the
//  chosen period (Daily, 1W, 30D, 90D, YTD). A Sectors view collapses every sector to one
//  cap-weighted tile. Prices come from the Massive.com bulk snapshot; reference closes for
//  the longer periods from grouped daily aggregates; market caps from ticker details (cached
//  on disk). Clicking a tile cascades the ticker to every tab; double-click opens its chain.
//

#pragma once

#include "QtHeaders.h"
#include "MarketDataClient.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Treemap.h"

#include <functional>
#include <map>
#include <vector>

class TreemapView;

class SectorHeatmapTab : public QWidget
{
public:
    struct Stock {
        QString ticker;
        QString name;
        QString sector;
        double marketCap = 0.0;        ///< USD
        double last = 0.0;
        double previousClose = 0.0;
        double referenceClose = 0.0;   ///< close at the start of the selected period (non-daily)
        double performance = std::numeric_limits<double>::quiet_NaN();   ///< percent over the period
    };

    explicit SectorHeatmapTab(QWidget* parent = nullptr);

    /// Downloads prices (and reference closes / market caps as needed) and redraws.
    void refresh();
    void applyTheme(const Theme& theme);
    /// Offline data for screenshots and tests: the built-in universe with synthetic moves.
    void loadSampleData();

    QStringList periodLabels() const;
    QString periodLabel() const;
    bool setPeriod(const QString& label);
    bool setView(const QString& view);   ///< "stocks" or "sectors"
    /// One paragraph for the assistant: period, breadth, best and worst sectors and stocks.
    QString summaryText() const;
    /// CSV: sector, ticker, name, market cap (USD bn), last, performance %.
    QString resultsCsv() const;
    const std::vector<Stock>& stocks() const { return m_stocks; }

    std::function<void(const QString& ticker)> onTickerSelected;
    std::function<void(const QString& ticker)> onOpenChain;
    /// Supplies the Quotes watchlist when the universe combo is set to "Watchlist".
    std::function<QStringList()> watchlistProvider;

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct Period {
        const char* label;
        int lookbackDays;      ///< 0 = daily (snapshot change), -1 = year to date
        double colorScale;     ///< percent that saturates the colour ramp
    };
    static const std::vector<Period>& periods();

    void buildUi();
    void wire();
    void buildUniverse();
    void fetchQuotes();
    void ensureReferenceCloses();
    void fetchReferenceFallback(const QDate& target);
    void applyReferenceCloses();
    void loadCapCache();
    void saveCapCache() const;
    void fetchMissingCaps();
    void recomputePerformance();
    void relayout();
    void updateStatus(const QString& text, ui::StatusKind kind);
    QDate referenceDate() const;
    QString capCachePath() const;
    QString sectorForWatchlistTicker(const QString& ticker, QString* name) const;

    MarketDataClient m_client;
    Theme m_theme;
    std::vector<Stock> m_stocks;
    int m_periodIndex = 0;
    bool m_sectorsView = false;
    bool m_watchlistUniverse = false;
    bool m_loaded = false;
    bool m_loadingQuotes = false;
    QDateTime m_pricesAsOf;
    QDate m_referenceSessionDate;
    std::map<QString, std::map<QString, double>> m_referenceCloses;   ///< ISO date -> ticker -> close
    std::map<QString, QDate> m_referenceSession;                      ///< period label -> session actually used
    int m_pendingReference = 0;
    struct CapEntry { double marketCap = 0.0; QString name; QDateTime asOf; };
    std::map<QString, CapEntry> m_capCache;
    QStringList m_capQueue;
    int m_capInFlight = 0;

    QButtonGroup* m_viewGroup = nullptr;
    QButtonGroup* m_periodGroup = nullptr;
    QComboBox* m_universe = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_refresh = nullptr;
    TreemapView* m_view = nullptr;
};
