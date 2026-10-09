//
//  QuotesTab.h
//  OptionPricing
//
//  Quotes and charts: an editable watchlist (ticker, last, change, percent change) on
//  the left and a TradingView Lightweight Charts(TM) view on the right with timeframe
//  buttons, candlestick / bar / Heikin-Ashi / line styles, an Indicators menu (up to three
//  SMAs and three EMAs with their own periods and colours, a MACD with editable
//  fast/slow/signal, volume) remembered between sessions. Data comes from Massive.com
//  aggregates and bulk snapshots.
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
    /// Fired after a ticker is added to or removed from the watchlist, or another list is loaded.
    std::function<void(const QStringList& watchlist)> onWatchlistChanged;
    QStringList watchlist() const { return m_watchlist; }

    // ---- Named watchlists (saved in the preferences) ----
    QStringList watchlistNames() const;
    QString activeWatchlistName() const { return m_activeWatchlist; }
    /// Loads a saved list: replaces the table, refreshes quotes and fires onWatchlistChanged
    /// (which preloads the option chains). False if no list has that name.
    bool loadWatchlistNamed(const QString& name);
    bool createWatchlist(const QString& name, const QStringList& tickers, bool activate);
    /// Creates and loads a list from free text ("AAPL,NVDA,IBM", lines, spaces); false if no symbols.
    bool createWatchlistFromText(const QString& name, const QString& text);
    /// Adds every symbol found in the list that is not already present; returns how many were added.
    int addTickers(const QStringList& tickers);
    QStringList clipboardTickers() const;
    bool renameWatchlist(const QString& from, const QString& to);
    bool deleteWatchlist(const QString& name);

    // ---- Assistant hooks ----
    const MarketDataClient::BarSeries& bars() const { return m_bars; }
    QString chartTicker() const { return m_chartTicker; }
    QString timeframeLabel() const;
    QStringList timeframeLabels() const;
    /// Switches the timeframe button (and reloads the chart); false if the label is unknown.
    bool setTimeframe(const QString& label);
    bool setChartType(const QString& type);

    // ---- Technical indicators (TA-Lib) ----
    // Each indicator is a JSON object {"func":"RSI","params":{"optInTimePeriod":14},"colors":["#rrggbb",...]}
    // naming a TA-Lib function (see Pricing/TechnicalAnalysis.h); parameters may also use the
    // aliases period/fast/slow/signal/nbdevup/nbdevdn/matype, and the older
    // {"type":"sma"|"ema"|"macd",...} specs are converted. Limits: kMaxIndicators in total,
    // kMaxPerFunction copies of the same function, kMaxPanes indicators in their own panes.
    // The list is saved in the preferences and restored on the next launch.
    static constexpr int kMaxIndicators = 12;
    static constexpr int kMaxPerFunction = 3;
    static constexpr int kMaxPanes = 4;
    QJsonArray indicators() const { return m_indicators; }
    /// Replaces the whole set after validation; on failure nothing changes and `error` says why.
    bool setIndicators(const QJsonArray& indicators, QString* error = nullptr);
    /// Adds one indicator (colours are chosen if the spec has none); false with `error` if invalid or at a limit.
    bool addIndicator(QJsonObject spec, QString* error = nullptr);
    /// Removes indicators of TA-Lib function `func` ("all" = every one) and, when > 0, with that time period. Returns how many were removed.
    int removeIndicators(const QString& func, int period = 0);
    bool volumeShown() const;
    void setVolumeShown(bool on);
    /// Human-readable list, e.g. "SMA 20, EMA 50, MACD 12/26/9, RSI 14; volume on".
    QString indicatorsSummary() const;
    static QString indicatorLabel(const QJsonObject& spec);
    /// Resolves a spoken or typed name ("rsi", "bollinger bands", "average true range", "engulfing") to a TA-Lib function name; empty if unknown.
    static QString resolveIndicatorName(const QString& text);
    /// The catalogue by category for the assistant: "Momentum Indicators: RSI – Relative Strength Index (period 14); ...".
    static QString indicatorCatalogText();

    /// Loads `symbol` on `timeframe` (empty = current) and reports when the bars are in.
    void loadChartThen(const QString& symbol, const QString& timeframe, std::function<void(bool ok)> done);
    /// Adds a drawing from a JSON spec (see chartApi.addDrawing) and persists it.
    void addDrawing(const QJsonObject& spec);
    void clearDrawings();
    /// Reloads the bars into the chart and restores autoscale, default zoom and the cursor tool.
    void resetChart();
    /// JSON array of the charted symbol's drawings (from the persisted copy).
    QString drawingsJson() const;
    /// Renders the chart (with drawings) to an image asynchronously; null image on failure.
    void chartImage(std::function<void(const QImage&)> done);
    /// Plain-text description of the chart for the assistant: symbol, timeframe, indicator
    /// settings, drawings, and the most recent bars as CSV.
    QString contextSummary(int maxBars = 80) const;

    /// Test hooks: the watchlist row's displayed Last / change / percent, and the chart legend text.
    QString debugRowText(const QString& symbol) const;
    void debugLegendText(std::function<void(const QString&)> done);

    /// Test hooks for the live smoke: draws sample annotations through the page's mouse
    /// handlers and reports how many drawings exist; checks the per-symbol persistence.
    void debugSimulateDrawings(std::function<void(int count)> done);
    /// Right-clicks the first drawing through the page's handlers and chooses Delete;
    /// reports the remaining count, or -1 if the menu did not appear or nothing was removed.
    void debugSimulateContextDelete(std::function<void(int remaining)> done);
    /// Puts back the drawings stashed by debugSimulateDrawings(); reports how many were restored.
    void debugRestoreDrawings(std::function<void(int count)> done);
    /// Toggles the indicator pane between its default and expanded height; reports the pane height in px (0 = no pane).
    void debugTogglePane(std::function<void(int px)> done);
    /// Sets the user's drawings aside (without persisting the empty set) so tests start clean.
    void debugStashDrawings(std::function<void(int stashed)> done);
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
    void refreshWatchlistCombo();
    void promptNewWatchlist(bool copyCurrent);
    void promptRenameWatchlist();
    void promptDeleteWatchlist();
    void importWatchlistFile();
    void exportWatchlistFile();
    void createWatchlistFromClipboard();
    void addTickersFromClipboard();
    void addTicker(const QString& ticker);
    void removeSelectedTicker();
    void rebuildTable();
    void fillQuoteRow(int row, const MarketDataClient::Quote& quote);
    void loadChart(const QString& ticker);
    void loadIndicators();
    void saveIndicators() const;
    void rebuildIndicatorsMenu();
    /// Computes every indicator with TA-Lib over the loaded bars and sends the results to the chart.
    void pushIndicators();
    /// Dialog to add or edit an indicator (parameters from TA-Lib's description, a colour per output); false if cancelled.
    bool editIndicatorDialog(QJsonObject& spec, bool adding);
    void promptAddIndicator(const QString& func);
    /// Searchable, categorised browser of the whole TA-Lib catalogue.
    void promptBrowseIndicators();
    QStringList unusedIndicatorColors(int count) const;
    static bool normalizeIndicator(QJsonObject& spec, QString* error);
    void pushBars();
    void pushOptions();
    void pushTheme();
    /// Shows the app-wide ticker's row with the headline's figures and sends the live price to the chart.
    void syncActiveQuote();
    void pushLive();
    void pinPreviousCloseFromBars();
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
    QStringList m_watchlist;                      ///< tickers of the active list
    QMap<QString, QStringList> m_watchlists;      ///< every saved list by name
    QString m_activeWatchlist;
    bool m_updatingWatchlists = false;
    QComboBox* m_watchlistCombo = nullptr;
    QToolButton* m_watchlistMenu = nullptr;
    QAction* m_deleteWatchlistAction = nullptr;
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
    std::function<void(bool)> m_chartLoaded;   ///< one-shot callback for loadChartThen()

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
    QToolButton* m_indicatorsButton = nullptr;   ///< "Indicators ▾" drop-down menu
    QMenu* m_indicatorsMenu = nullptr;
    QAction* m_volumeAction = nullptr;           ///< checkable "Volume" entry in the menu
    QJsonArray m_indicators;                     ///< see indicators(); mirrors QSettings quotes/indicators
    QCheckBox* m_priceLineCheck = nullptr;
    QPushButton* m_openChain = nullptr;
    QPushButton* m_saveImage = nullptr;
    QPushButton* m_resetChart = nullptr;
    int m_loadSequence = 0;            ///< increments per loadChart(); stale responses are dropped
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
