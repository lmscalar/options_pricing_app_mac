//
//  OptimizerTab.h
//  OptionPricing
//
//  Strategy comparison and optimiser for the current ticker's option chain. State a view
//  (target price at expiry and the uncertainty around it), choose an objective and a
//  strategy family, and the optimiser enumerates listed strikes and expiries and ranks the
//  candidates; "Compare presets" lines up one delta-targeted candidate per preset instead.
//  Selected rows are overlaid on a payoff chart; a candidate opens in the Strategy tab or
//  books into the Portfolio.
//

#pragma once

#include "QtHeaders.h"
#include "ChainStore.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Optimizer.h"

#include <functional>
#include <vector>

class OptimizerTab : public QWidget
{
public:
    explicit OptimizerTab(MarketState& state, QWidget* parent = nullptr);

    void setStore(ChainStore* store) { m_store = store; }
    void applyTheme(const Theme& theme);

    /// Enumerates strikes and expiries for the chosen families and ranks by the objective.
    void runOptimizer();
    /// One candidate per preset on the expiry nearest the middle of the window.
    void comparePresets();
    /// Synthetic chain for screenshots and tests.
    void loadSampleData();

    // ---- Assistant hooks ----
    QStringList objectiveNames() const;
    bool setObjective(const QString& name);
    QStringList familyNames() const;
    bool setFamily(const QString& name);
    void setTargetPrice(double price);
    void setTargetMovePercent(double percent);
    void setViewVolPercent(double percent);
    void setDays(int minDays, int maxDays);
    const std::vector<pricing::opt::Candidate>& candidates() const { return m_candidates; }
    QString summaryText() const;
    QString resultsCsv() const;
    bool openCandidate(int index);

    std::function<void(const QString& ticker, const pricing::Position& position)> onOpenInStrategy;
    std::function<void(const QString& ticker, const pricing::Position& position)> onAddToPortfolio;

private:
    struct Family {
        const char* label;
        std::vector<pricing::StrategyPreset> presets;   ///< empty = every preset
    };
    static const std::vector<Family>& families();

    void buildUi();
    void wire();
    void loadSettings();
    void saveSettings() const;
    bool chain(std::vector<pricing::ChainQuote>& quotes, double& spot, QString& ticker) const;
    pricing::ActivityMarket activityMarket(double spot) const;
    pricing::opt::Options options() const;
    pricing::opt::View view() const;
    void refreshUnderlying();
    void fillTable();
    void fillChart();
    void setStatus(const QString& text, ui::StatusKind kind);
    std::vector<int> selectedCandidates() const;
    QString fmtMoney(double v) const;

    MarketState& m_state;
    ChainStore* m_store = nullptr;
    Theme m_theme;
    std::vector<pricing::opt::Candidate> m_candidates;
    std::vector<pricing::ChainQuote> m_sampleQuotes;   ///< offline chain when loadSampleData() was used
    double m_sampleSpot = 0.0;
    QString m_ticker;
    double m_spot = 0.0;
    double m_atmIv = 0.0;
    QString m_mode;                 ///< "optimizer" or "presets"
    QDateTime m_ranAt;
    bool m_updating = false;

    // Controls
    QLabel* m_underlying = nullptr;
    QDoubleSpinBox* m_target = nullptr;
    QDoubleSpinBox* m_move = nullptr;
    QDoubleSpinBox* m_viewVol = nullptr;
    QSpinBox* m_minDays = nullptr;
    QSpinBox* m_maxDays = nullptr;
    QComboBox* m_objective = nullptr;
    QComboBox* m_family = nullptr;
    QDoubleSpinBox* m_maxLoss = nullptr;
    QCheckBox* m_definedRisk = nullptr;
    QPushButton* m_compare = nullptr;
    QPushButton* m_run = nullptr;
    QLabel* m_status = nullptr;

    // Results
    QTableWidget* m_table = nullptr;
    QLabel* m_tableTitle = nullptr;
    QPushButton* m_openStrategy = nullptr;
    QPushButton* m_addPortfolio = nullptr;
    QPushButton* m_exportCsv = nullptr;
    QChart* m_chart = nullptr;
    ui::HoverChartView* m_chartView = nullptr;
    QSplitter* m_splitter = nullptr;
};
