//
//  PortfolioTab.h
//  OptionPricing
//
//  The book: stock and option positions across underlyings with live marks (bulk snapshot
//  for shares, stored option chains for contracts), P&L, aggregate Greeks, a what-if shock
//  row, and portfolio risk: parametric, historical and Monte Carlo VaR / expected shortfall
//  (Pricing/Risk.h), component VaR by underlying, a spot x vol stress grid, a time-decay
//  ladder and the simulated P&L distribution. Positions persist in the preferences.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "MarketDataClient.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Risk.h"

#include <functional>
#include <map>
#include <vector>

class PortfolioTab : public QWidget
{
public:
    explicit PortfolioTab(MarketState& state, QWidget* parent = nullptr);

    void setStore(ChainStore* store) { m_store = store; }
    void applyTheme(const Theme& theme);
    /// Re-marks every position from live data and recomputes the summary (not the simulations).
    void refreshMarks();
    /// Runs the three VaR methods, the stress grid and the decay ladder for the current settings.
    void runRisk();
    /// Sample book for screenshots and tests (synthetic marks and return history when offline).
    void loadSampleData(bool synthetic);
    /// Opens the Add position dialog (what the Add… button does).
    void openAddDialog() { promptPosition(-1); }

    // ---- Assistant hooks ----
    /// Adds a position from JSON: {symbol, type: "stock"|"call"|"put", quantity, strike, expiry (ISO), entry, iv (percent)}.
    bool addHolding(const QJsonObject& spec, QString* error = nullptr);
    /// Removes positions for a symbol (optionally only a strike/type match); returns how many.
    int removeHoldings(const QString& symbol, const QString& type = QString(), double strike = 0.0);
    /// Imports the Strategy tab's legs for the current underlying as positions.
    int importStrategy(const pricing::Position& position, const QString& symbol);
    QString summaryText() const;          ///< book exposure and the latest risk figures
    QString resultsCsv() const;           ///< one row per position with marks, Greeks and P&L
    QString riskCsv() const;              ///< VaR table, component VaR, stress grid
    bool setRiskSettings(const QString& method, double confidence, int horizonDays);
    size_t holdingCount() const { return m_book.holdings.size(); }

    // ---- Strategy books ----
    /// Named strategies; the view is either one of them or "All strategies" (every book together).
    QStringList portfolioNames() const;
    QString activePortfolio() const { return m_activeBook; }
    /// Switches the view ("all" / "global" = every strategy) and re-marks; false if no such strategy.
    bool loadPortfolio(const QString& name);
    bool createPortfolio(const QString& name, bool copyCurrent, bool activate);
    bool renamePortfolio(const QString& from, const QString& to);
    bool deletePortfolio(const QString& name);

    std::function<pricing::Position()> strategyProvider;
    std::function<void(const QString& ticker)> onTickerSelected;

private:
    struct Row {
        pricing::Holding holding;
        pricing::Exposure exposure;
        QString markSource;   ///< "chain mid", "model", "snapshot"
    };
    struct History {
        std::vector<QDate> dates;
        std::vector<double> closes;
    };

    void buildUi();
    void wire();
    void loadPositions();
    void savePositions() const;
    static pricing::Holding holdingFromJson(const QJsonObject& o);
    /// The strategy new positions go to: the active one, or the first book in the global view.
    QString targetBook() const;
    void rebuildWorkingBook();
    void commitWorkingBook();
    void refreshBookCombo();
    void promptNewBook(bool copyCurrent);
    void promptRenameBook();
    void promptDeleteBook();
    void fillStrategyTable(const pricing::Markets& mk);
    void fillTable();
    void fillCards();
    void fillWhatIf();
    void fillRiskTables();
    void fillDistributionChart();
    void promptPosition(int editRow);
    void removeSelected();
    void importCsv();
    void exportCsv();
    void fetchHistories();
    pricing::Markets markets() const;
    pricing::risk::ReturnHistory returnHistory() const;
    void markFromChain(pricing::Holding& h, QString* source) const;
    void setStatus(const QString& text, ui::StatusKind kind);
    QString riskSettingsLabel() const;
    // ---- Currency ----
    /// "$1,234" / "−€1,234" in `currency` (empty = the reporting currency).
    QString fmtMoney(double v, int decimals = 0, const QString& currency = QString()) const;
    /// Always signed: "+$1,234" / "−$1,234".
    QString fmtSigned(double v, int decimals = 0, const QString& currency = QString()) const;
    double toUsd(const QString& currency) const;
    /// Units of the reporting currency per unit of `fromCurrency`.
    double fx(const QString& fromCurrency) const;
    /// The book with every multiplier scaled into the reporting currency (what exposures and risk use).
    pricing::Portfolio reportingBook() const;
    void rebuildRows();
    void updateCurrencyHeaders();
    void fetchFx();
    void promptFxRates();

    MarketState& m_state;
    MarketDataClient m_client;
    ChainStore* m_store = nullptr;
    Theme m_theme;
    pricing::Portfolio m_book;                               ///< the working view (one strategy or all)
    QMap<QString, std::vector<pricing::Holding>> m_books;    ///< strategy name -> positions (source of truth)
    QString m_activeBook;                                    ///< a strategy name or "All strategies"
    bool m_updatingBooks = false;
    QComboBox* m_bookBox = nullptr;
    QToolButton* m_bookMenu = nullptr;
    QAction* m_deleteBookAction = nullptr;
    QLabel* m_strategyTitle = nullptr;
    QTableWidget* m_strategyTable = nullptr;
    QLabel* m_strategyNote = nullptr;
    std::vector<Row> m_rows;
    std::map<QString, double> m_spots;
    std::map<QString, double> m_previousCloses;
    std::map<QString, History> m_histories;
    QStringList m_historyQueue;
    int m_historyInFlight = 0;
    QDateTime m_marksAsOf;
    bool m_updating = false;
    QString m_currency = "USD";                 ///< reporting currency (ISO code)
    std::map<QString, double> m_fxToUsd;        ///< vendor rates: USD per unit of currency
    std::map<QString, double> m_fxOverrides;    ///< user overrides, same convention
    QDateTime m_fxAsOf;
    QComboBox* m_currencyBox = nullptr;
    QPushButton* m_fxButton = nullptr;

    pricing::risk::VarResult m_parametric, m_historical, m_monteCarlo;
    pricing::risk::StressGrid m_stress;
    std::vector<std::pair<int, double>> m_decay;
    QDateTime m_riskRunAt;

    // Positions
    QTableWidget* m_table = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_importStrategy = nullptr;
    QPushButton* m_importCsv = nullptr;
    QPushButton* m_exportCsv = nullptr;
    QPushButton* m_refresh = nullptr;
    QLabel* m_status = nullptr;

    // Cards
    ui::Card m_cardValue, m_cardPnl, m_cardDelta, m_cardGamma, m_cardVega, m_cardTheta, m_cardVar, m_cardCvar;

    // What-if
    QDoubleSpinBox* m_whatIfSpot = nullptr;
    QDoubleSpinBox* m_whatIfVol = nullptr;
    QSpinBox* m_whatIfDays = nullptr;
    QLabel* m_whatIfResult = nullptr;

    // Risk
    QComboBox* m_confidence = nullptr;
    QSpinBox* m_horizon = nullptr;
    QSpinBox* m_paths = nullptr;
    QComboBox* m_chartMethod = nullptr;
    QPushButton* m_run = nullptr;
    QTableWidget* m_varTable = nullptr;
    QTableWidget* m_componentTable = nullptr;
    QTableWidget* m_stressTable = nullptr;
    QLabel* m_decayLabel = nullptr;
    QLabel* m_riskStatus = nullptr;
    QChart* m_distributionChart = nullptr;
    ui::HoverChartView* m_distributionView = nullptr;
    QSplitter* m_splitter = nullptr;
};
