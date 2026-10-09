//
//  QuotesTab.h
//  OptionPricing
//
//  Quotes and charts: an editable watchlist (ticker, last, change, percent change) on
//  the left and a TradingView Lightweight Charts(TM) view on the right with timeframe
//  buttons, candlestick / bar / Heikin-Ashi / line styles, SMA and EMA overlays and a
//  volume histogram. Data comes from Massive.com aggregates and bulk snapshots.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "ChartPage.h"
#include "MarketDataClient.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"

#include <functional>
#include <map>
#include <vector>

class QuotesTab : public QWidget
{
public:
    explicit QuotesTab(MarketState& state, QWidget* parent = nullptr);

    /// Adds the ticker to the watchlist if needed, selects it and loads its chart.
    void showTicker(const QString& ticker);
    void refreshQuotes();
    bool hasTicker(const QString& ticker) const { return m_watchlist.contains(ticker.trimmed().toUpper()); }
    /// Shared store: refreshed quotes are written to it, and loadStoredQuotes() shows the
    /// previous session's prices before the first refresh arrives.
    void setStore(ChainStore* store) { m_store = store; }
    void loadStoredQuotes();
    void applyTheme(const Theme& theme);
    QString resultsCsv() const;

    /// Renders the current chart to a PNG via the charting library and reports the path
    /// (empty on failure). Used by the File menu and the live smoke test.
    void saveChartImage(const QString& path, std::function<void(const QString& written)> done);

    std::function<void(const QString& ticker)> onOpenInChain;
    /// Fired when the user highlights a watchlist row; the app cascades the ticker to every tab.
    std::function<void(const QString& ticker)> onTickerSelected;
    /// Fired after a ticker is added to or removed from the watchlist.
    std::function<void(const QStringList& watchlist)> onWatchlistChanged;
    QStringList watchlist() const { return m_watchlist; }

    /// Test hooks for the live smoke: draws sample annotations through the page's mouse
    /// handlers and reports how many drawings exist; checks the per-symbol persistence.
    void debugSimulateDrawings(std::function<void(int count)> done);
    bool hasStoredDrawings(const QString& symbol) const;

private:
    struct Timeframe {
        const char* label;
        int multiplier;
        const char* timespan;
        int lookbackDays;
        bool intraday;
    };
    static const std::vector<Timeframe>& timeframes();

    void buildUi();
    void wire();
    void loadWatchlist();
    void saveWatchlist() const;
    void addTicker(const QString& ticker);
    void removeSelectedTicker();
    void rebuildTable();
    void fillQuoteRow(int row, const MarketDataClient::Quote& quote);
    void loadChart(const QString& ticker);
    void pushBars();
    void pushOptions();
    void pushTheme();
    /// Sends the stored drawings (trend lines, support/resistance zones) for the charted symbol.
    void pushDrawings();
    void onPageMessage(const QString& kind, const QString& payload);
    static const QStringList& drawToolNames();
    void runJs(const QString& script);
    QString themeJson() const;
    QString selectedTicker() const;
    QString tickerAtRow(int row) const;
    int rowForTicker(const QString& symbol) const;
    void saveHeaderState() const;
    void updateTimers();
    void setStatus(const QString& text, ui::StatusKind kind);

    MarketState& m_state;
    MarketDataClient m_client;
    ChainStore* m_store = nullptr;
    Theme m_theme;
    QStringList m_watchlist;
    std::map<QString, MarketDataClient::Quote> m_quotes;
    std::map<QString, QString> m_names;            ///< company names resolved from branding lookups

    QString m_chartTicker;
    int m_timeframeIndex = 6;                      ///< 1D by default
    MarketDataClient::BarSeries m_bars;
    bool m_pageReady = false;
    QStringList m_pendingJs;
    bool m_loadingChart = false;
    bool m_updating = false;
    bool m_autoSelecting = false;   ///< true while the first row is selected programmatically at start-up
    bool m_cascadeSelection = false; ///< true while showTicker() selects a row that should cascade

    // Watchlist
    QLineEdit* m_tickerEdit = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QTableWidget* m_table = nullptr;
    QCheckBox* m_autoRefresh = nullptr;
    QSpinBox* m_refreshInterval = nullptr;
    QLabel* m_quoteStatus = nullptr;
    QTimer* m_timer = nullptr;

    // Chart
    QLabel* m_chartSymbol = nullptr;
    QLabel* m_chartName = nullptr;
    QButtonGroup* m_timeframeGroup = nullptr;
    QComboBox* m_chartType = nullptr;
    QCheckBox* m_smaCheck = nullptr;
    QSpinBox* m_smaPeriod = nullptr;
    QCheckBox* m_emaCheck = nullptr;
    QSpinBox* m_emaPeriod = nullptr;
    QCheckBox* m_volumeCheck = nullptr;
    QPushButton* m_openChain = nullptr;
    QPushButton* m_saveImage = nullptr;
    QWebEngineView* m_view = nullptr;
    ChartWebPage* m_page = nullptr;
    QLabel* m_chartStatus = nullptr;

    // Drawing tools
    QButtonGroup* m_drawTools = nullptr;       ///< ids follow drawToolNames(): cursor, trend, support, resistance, edit
    QToolButton* m_undoDraw = nullptr;
    QToolButton* m_deleteDraw = nullptr;
    QToolButton* m_clearDraw = nullptr;
    QLabel* m_drawHint = nullptr;
    std::map<QString, QString> m_drawings;     ///< symbol -> JSON array of drawings (mirrors QSettings)
};
