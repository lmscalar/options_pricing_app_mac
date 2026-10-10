//
//  VolatilityTab.h
//  OptionPricing
//
//  Realized and forecast volatility for a ticker from daily bars: rolling realized vol
//  (five estimators), a volatility cone, EWMA and GARCH / GJR-GARCH fits with a forecast
//  term structure, all compared with the at-the-money implied vols of the loaded chain.
//  Either volatility can be pushed into the market inputs as σ.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "IvBackfill.h"
#include "MarketDataClient.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Volatility.h"
#include "../Pricing/IvHistory.h"

#include <QtCharts/QCategoryAxis>
#include <QtCore/QTimeZone>

#include <functional>
#include <memory>
#include <vector>

class VolatilityTab : public QWidget
{
public:
    explicit VolatilityTab(MarketState& state, QWidget* parent = nullptr);

    void setTicker(const QString& ticker);
    QString ticker() const { return m_ticker; }
    /// Shared store: daily implied-vol samples (from stored chains and backfills) live there.
    void setStore(ChainStore* store);

    // ---- Implied-vol history: IV rank and percentile ----
    /// Rebuilds about a year of daily implied vol from historical option bars (needs daily bars loaded).
    void backfillIvHistory();
    bool backfillBusy() const { return m_backfill && m_backfill->busy(); }
    const pricing::ivhist::Stats& ivStats() const { return m_ivStats; }
    /// One sentence: IV30 now, rank, percentile, one-year range with dates, sample count.
    QString ivSummary() const;
    /// Downloads daily bars for the ticker from Massive.com and recomputes everything.
    void fetchHistory();
    /// User-initiated fetch: loads the history and, when the ticker differs from the loaded
    /// chain, asks the Option Chain tab to download that chain so every tab switches.
    void fetchAll();
    /// Loads a synthetic GARCH path so the tab can be shown without market data.
    void loadSample();

    void applyTheme(const Theme& theme);
    QString resultsCsv() const;
    /// One-paragraph plain-text summary of the current numbers (for the live smoke test).
    QString summaryText() const;

    std::function<void(bool ok, const QString& message)> onFetchFinished;
    /// Fetches history for `symbol` (or recomputes if already loaded) and reports once.
    void fetchHistoryThen(const QString& symbol, std::function<void(bool ok, const QString& message)> done);
    /// Asks the Option Chain tab to download a chain for the ticker (wired by MainWindow).
    std::function<void(const QString& ticker)> onRequestChain;

private:
    void buildUi();
    void wire();
    void setBars(std::vector<pricing::DailyBar> bars, const QString& source);
    void recompute();
    void updateCards();
    /// Loads the stored implied-vol history for the ticker and ranks today's IV30 against it.
    void updateIvStats();
    double currentIv30() const;
    void updateModelLabel();
    void updateConeTable();
    /// Fixes the table's height to its header and rows (no inner scrolling) for the sidebar.
    void fitConeTableHeight();
    void updateCharts();
    void updateHistoryChart();
    void updateConeChart();
    void updateForecastChart();
    void applyVol(double vol, const QString& source);
    void showHover(QLabel* readout, const QString& text, bool state);
    void setStatus(const QString& text, ui::StatusKind kind);
    void saveLayoutState() const;
    void reportFetch(bool ok, const QString& message);
    std::function<void(bool, const QString&)> m_fetchOnce;

    pricing::RealizedEstimator estimator() const;
    pricing::GarchModel model() const;
    static const std::vector<int>& coneWindows();
    /// True when the loaded option chain belongs to this ticker (or is synthetic).
    bool chainMatches() const;
    /// ATM implied vol for a maturity in years from the fitted surface (0 when unavailable).
    double impliedAtmVol(double maturity) const;
    /// (trading days, ATM vol) per fitted expiry.
    std::vector<std::pair<double, double>> impliedTermStructure() const;
    QString surfaceSignature() const;

    MarketState& m_state;
    MarketDataClient m_client;
    Theme m_theme;
    std::vector<pricing::DailyBar> m_bars;
    QString m_ticker;
    QString m_barsSource;
    QString m_lastChainTicker;
    QString m_lastSurfaceSignature;
    bool m_loading = false;
    ChainStore* m_store = nullptr;
    std::unique_ptr<IvBackfill> m_backfill;
    std::vector<pricing::ivhist::Sample> m_ivHistory;   ///< ascending, from the store
    pricing::ivhist::Stats m_ivStats;

    // Results
    std::vector<pricing::VolPoint> m_series;       ///< rolling realized vol, short window
    std::vector<pricing::VolPoint> m_seriesLong;   ///< rolling realized vol, long window
    pricing::EwmaResult m_ewma;
    pricing::GarchFit m_garch;
    std::vector<pricing::ConeRow> m_cone;
    std::vector<double> m_returns;

    // Controls
    QLineEdit* m_tickerEdit = nullptr;
    QComboBox* m_history = nullptr;
    QPushButton* m_fetch = nullptr;
    QPushButton* m_sample = nullptr;
    QPushButton* m_backfillButton = nullptr;
    QComboBox* m_estimator = nullptr;
    QSpinBox* m_window = nullptr;
    QSpinBox* m_windowLong = nullptr;
    QComboBox* m_model = nullptr;
    QSpinBox* m_horizon = nullptr;
    QPushButton* m_useRealized = nullptr;
    QPushButton* m_useForecast = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_estimatorNote = nullptr;
    QLabel* m_modelLabel = nullptr;

    // Cards
    ui::Card m_cardRealized, m_cardLong, m_cardEwma, m_cardGarchNow, m_cardForecast, m_cardLongRun, m_cardImplied, m_cardPersistence, m_cardIvRank, m_cardIvPercentile;

    // Charts
    QChart* m_historyChart = nullptr;
    QCategoryAxis* m_historyX = nullptr;   ///< x is epoch milliseconds; labels are placed at month starts
    QValueAxis* m_historyY = nullptr;
    QLabel* m_historyReadout = nullptr;
    QChart* m_coneChart = nullptr;
    QValueAxis* m_coneX = nullptr;
    QValueAxis* m_coneY = nullptr;
    QLabel* m_coneReadout = nullptr;
    QChart* m_forecastChart = nullptr;
    QValueAxis* m_forecastX = nullptr;
    QValueAxis* m_forecastY = nullptr;
    QLabel* m_forecastReadout = nullptr;
    QTableWidget* m_coneTable = nullptr;
    QSplitter* m_rootSplitter = nullptr;    ///< sidebar | charts
    QSplitter* m_chartSplitter = nullptr;   ///< history / lower row
    QSplitter* m_lowerSplitter = nullptr;   ///< cone table | cone chart | forecast chart
};
