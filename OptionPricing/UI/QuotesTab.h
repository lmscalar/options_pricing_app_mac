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
    void applyTheme(const Theme& theme);
    QString resultsCsv() const;

    /// Renders the current chart to a PNG via the charting library and reports the path
    /// (empty on failure). Used by the File menu and the live smoke test.
    void saveChartImage(const QString& path, std::function<void(const QString& written)> done);

    std::function<void(const QString& ticker)> onOpenInChain;

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
    QLabel* m_chartStatus = nullptr;
};
