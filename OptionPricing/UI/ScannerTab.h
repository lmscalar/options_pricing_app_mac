//
//  ScannerTab.h
//  OptionPricing
//
//  Trade Ideas: scans the option chains held in memory (every watchlist ticker, the
//  watchlist only, or the current ticker) for candidate trades. A market-scan table shows
//  each chain's ATM implied vol, term slope, 25-delta skew, straddle-implied expected move,
//  put/call ratios and liquidity; the ideas table ranks delta-targeted strategies built on
//  the listed strikes by probability-weighted return on risk. Screens (premium selling,
//  directional debit, volatility, income on shares) preset the strategy set and filters.
//  An idea opens in the Strategy tab or goes straight into the Portfolio.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Scanner.h"

#include <functional>
#include <map>
#include <vector>

class ScannerTab : public QWidget
{
public:
    explicit ScannerTab(MarketState& state, QWidget* parent = nullptr);

    void setStore(ChainStore* store) { m_store = store; }
    void applyTheme(const Theme& theme);

    /// Scans the selected universe with the current screen and filters.
    void runScan();
    /// Offline chains for screenshots and tests (synthetic smiles for three tickers).
    void loadSampleData();

    // ---- Assistant hooks ----
    QStringList screenNames() const;
    QString screenName() const;
    bool setScreen(const QString& name);           ///< case-insensitive, substring match
    bool setBias(const QString& name);             ///< Any / Bullish / Bearish / Neutral / Volatile
    void setDays(int minDays, int maxDays);
    bool setUniverse(const QString& name);         ///< "all", "watchlist" or "current"
    void setMinProbability(double percent);
    void setMinReturnOnRisk(double percent);
    const std::vector<pricing::scan::Idea>& ideas() const { return m_ideas; }
    QString summaryText() const;                   ///< one paragraph: settings, universe, top ideas
    QString resultsCsv() const;                    ///< ideas table
    QString metricsCsv() const;                    ///< market-scan table
    /// Sends idea `index` (into ideas()) to the Strategy tab through onOpenInStrategy.
    bool openIdea(int index);
    /// Opens the "How to use" instructions dialog (what the How to use button does).
    void showHelp();

    std::function<void(const QString& ticker, const pricing::Position& position)> onOpenInStrategy;
    std::function<void(const QString& ticker, const pricing::Position& position)> onAddToPortfolio;
    std::function<void(const QString& ticker)> onTickerSelected;
    std::function<QStringList()> watchlistProvider;

private:
    struct Screen {
        const char* label;
        const char* hint;
        std::vector<pricing::StrategyPreset> presets;   ///< empty = every preset
        double minProbability;                          ///< default filter, percent
        double minReturnOnRisk;                         ///< default filter, percent
        bool definedRiskOnly;
    };
    struct Row {
        QString ticker;
        pricing::scan::ChainMetrics metrics;
        int ideas = 0;
    };
    struct SampleChain {
        std::vector<pricing::ChainQuote> quotes;
        double spot = 0.0;
    };

    static const std::vector<Screen>& screens();
    void buildUi();
    void wire();
    void loadSettings();
    void saveSettings() const;
    pricing::scan::Criteria criteria() const;
    QStringList universe() const;
    bool chainFor(const QString& ticker, std::vector<pricing::ChainQuote>& quotes, double& spot) const;
    pricing::ActivityMarket activityMarket(const QString& ticker, double spot, const std::vector<pricing::ChainQuote>& quotes) const;
    void fillMetrics();
    void fillIdeas();
    void setStatus(const QString& text, ui::StatusKind kind);
    int selectedIdea() const;
    QString fmtMoney(double v) const;

    MarketState& m_state;
    ChainStore* m_store = nullptr;
    Theme m_theme;
    std::vector<Row> m_rows;
    std::vector<pricing::scan::Idea> m_ideas;
    std::map<QString, SampleChain> m_samples;   ///< offline chains when loadSampleData() was used
    QString m_filterTicker;                     ///< ideas table shows one ticker when set
    QDateTime m_scannedAt;
    bool m_updating = false;

    // Controls
    QComboBox* m_screen = nullptr;
    QLabel* m_screenHint = nullptr;
    QComboBox* m_bias = nullptr;
    QComboBox* m_universe = nullptr;
    QSpinBox* m_minDays = nullptr;
    QSpinBox* m_maxDays = nullptr;
    QDoubleSpinBox* m_minPop = nullptr;
    QDoubleSpinBox* m_minRor = nullptr;
    QDoubleSpinBox* m_maxSpread = nullptr;
    QSpinBox* m_minOi = nullptr;
    QCheckBox* m_definedRisk = nullptr;
    QPushButton* m_run = nullptr;
    QPushButton* m_help = nullptr;
    QLabel* m_status = nullptr;

    // Tables
    QTableWidget* m_metricsTable = nullptr;
    QTableWidget* m_ideasTable = nullptr;
    QLabel* m_ideasTitle = nullptr;
    QPushButton* m_clearFilter = nullptr;
    QPushButton* m_openStrategy = nullptr;
    QPushButton* m_addPortfolio = nullptr;
    QPushButton* m_exportCsv = nullptr;
    QSplitter* m_splitter = nullptr;
};
