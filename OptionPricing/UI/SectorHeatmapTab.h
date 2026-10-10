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

#include <QtWidgets/QStackedWidget>

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
        double dayVolume = 0.0;        ///< session volume (shares or contracts), for $-volume sizing
        double multiplier = 1.0;       ///< contract unit for futures (1 for stocks and ETFs)
        double shortReferenceClose = 0.0;   ///< close one week back, for the rotation view's momentum axis
        double shortPerformance = std::numeric_limits<double>::quiet_NaN();   ///< percent over the short window
    };
    /// What the map shows: the built-in large caps by sector, a curated ETF list by asset
    /// class, the front-month futures by product group (real-time), or the Quotes watchlist.
    enum class Universe { LargeCaps, Etfs, Futures, Watchlist };

    explicit SectorHeatmapTab(QWidget* parent = nullptr);

    /// Downloads prices (and reference closes / market caps as needed) and redraws.
    void refresh();
    void applyTheme(const Theme& theme);
    /// Offline data for screenshots and tests: the built-in universe with synthetic moves.
    void loadSampleData();

    QStringList periodLabels() const;
    QString periodLabel() const;
    bool setPeriod(const QString& label);
    bool setView(const QString& view);   ///< "stocks", "sectors" or "rotation"
    Universe universeKind() const { return m_universeKind; }
    /// "large caps", "etfs", "futures" or "watchlist"; false if unknown.
    bool setUniverse(const QString& name);
    /// Tile sizing: "market cap", "volume" ($ traded today) or "equal"; false if unknown.
    bool setSizing(const QString& name);
    QString sizingLabel() const;
    /// Rotation view figures: each group's relative strength and momentum versus the universe, with its quadrant.
    QString rotationSummary() const;
    /// Expands one sector to fill the map (as clicking its tile does); false if no such sector.
    bool focusSector(const QString& sector);
    /// Back to the full sector map.
    void clearFocus();
    QString focusedSector() const { return m_focusSector; }
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
    /// Rotation scatter: one point per group, x = relative strength over the period, y = momentum over the short window.
    void buildRotation();
    /// Loads the one-week reference closes the rotation view needs (in addition to the period's).
    void ensureShortReference();
    /// Tile weight under the chosen sizing (market cap, $ volume or equal), with sensible fallbacks.
    double weightFor(const Stock& s) const;
    QString universeTitle() const;
    void updateStatus(const QString& text, ui::StatusKind kind);
    QDate referenceDate() const;
    QString capCachePath() const;
    QString sectorForWatchlistTicker(const QString& ticker, QString* name) const;

    MarketDataClient m_client;
    Theme m_theme;
    std::vector<Stock> m_stocks;
    int m_periodIndex = 0;
    bool m_sectorsView = false;
    QString m_focusSector;           ///< non-empty while one sector is expanded
    bool m_watchlistUniverse = false;
    Universe m_universeKind = Universe::LargeCaps;
    int m_sizing = 0;                ///< 0 market cap, 1 $ volume, 2 equal
    bool m_rotationView = false;
    bool m_shortReferencePending = false;   ///< one-week reference closes are being fetched
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
    QComboBox* m_sizingBox = nullptr;
    QStackedWidget* m_stack = nullptr;      ///< treemap or rotation chart
    QChartView* m_rotationChart = nullptr;
    QChart* m_rotation = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_reset = nullptr;
    TreemapView* m_view = nullptr;
};
