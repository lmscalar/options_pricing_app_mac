//
//  StrategyTab.h
//  OptionPricing
//
//  Multi-leg strategy builder: editable legs table, preset library, summary metrics,
//  net Greeks, a P&L chart at expiry / today / a chosen future date, and a chart of any
//  Greek across spot.
//

#pragma once

#include "QtHeaders.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/ChainStrategy.h"
#include "../Pricing/Strategy.h"

#include <functional>

class StrategyTab : public QWidget
{
public:
    explicit StrategyTab(MarketState& state, QWidget* parent = nullptr);

    const pricing::Position& position() const { return m_position; }
    void addLeg(const pricing::Leg& leg);
    /// Replaces the legs (used by the Trade Ideas scanner to hand over a candidate).
    void loadPosition(const pricing::Position& position) { setPosition(position); }

    /// Market used to value the position (rate resolved from the curve when enabled).
    pricing::Market valuationMarket() const;

    void applyTheme(const Theme& theme);
    void recompute();

    /// Replaces the legs with the preset currently selected in the preset combo box.
    void loadPreset();
    /// Selects the preset whose name contains `name` (case-insensitive) and loads it.
    bool selectPreset(const QString& name);
    QStringList presetNames() const;
    /// Greek chart mode: "spot" (across the underlying price) or "time" (along the calendar to the first expiry).
    bool setGreekMode(const QString& mode);
    QString greekMode() const;
    bool setGreek(const QString& name);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& json);
    QString resultsCsv() const;

    std::function<void()> onPositionChanged;

private:
    enum Column { ColKind = 0, ColQuantity, ColStrike, ColExpiry, ColVol, ColEntry, ColMarket, ColModel, ColUnitPnl, ColPnl, ColumnCount };

    void buildUi();
    void buildCharts(QVBoxLayout* column);
    void wire();
    void rebuildTable();
    void appendRow(const pricing::Leg& leg);
    void readTableIntoPosition();
    void updateSummary();
    void updateCharts();
    void repriceEntries();
    void applySurfaceToLegs();
    void setPosition(const pricing::Position& position);

    // Chain-driven mode (active whenever the Option Chain tab has a chain loaded)
    bool chainMode() const { return !m_chain.empty(); }
    pricing::ActivityMarket activityMarket() const;
    void refreshChainIndex();
    void populateExpiryCombo();
    void populateStrikeCombo(QComboBox* strikes, double maturity, double selected) const;
    double presetMaturity() const;
    void updateLegFromChain(int row);

    MarketState& m_state;
    Theme m_theme;
    pricing::Position m_position;
    pricing::StrategyAnalysis m_analysis;
    pricing::ChainIndex m_chain;
    QString m_chainTicker;
    bool m_updating = false;

    // Controls
    QComboBox* m_presets = nullptr;
    QLabel* m_presetDescription = nullptr;
    QDoubleSpinBox* m_presetMaturity = nullptr;
    QComboBox* m_presetExpiry = nullptr;
    QLabel* m_presetExpiryLabel = nullptr;
    QLabel* m_strikeStepLabel = nullptr;
    QLabel* m_chainInfo = nullptr;
    QDoubleSpinBox* m_strikeStep = nullptr;
    QDoubleSpinBox* m_multiplier = nullptr;
    QPushButton* m_loadPreset = nullptr;
    QPushButton* m_addLeg = nullptr;
    QPushButton* m_removeLeg = nullptr;
    QPushButton* m_clearLegs = nullptr;
    QPushButton* m_repriceEntries = nullptr;
    QPushButton* m_applySurface = nullptr;
    QTableWidget* m_table = nullptr;

    // Summary
    ui::Card m_netPremium, m_maxProfit, m_maxLoss, m_breakevens, m_probabilityOfProfit, m_expectedPnl;
    QLabel* m_gDelta = nullptr;
    QLabel* m_gGamma = nullptr;
    QLabel* m_gVega = nullptr;
    QLabel* m_gTheta = nullptr;
    QLabel* m_gRho = nullptr;
    QLabel* m_gVanna = nullptr;
    QLabel* m_gCharm = nullptr;
    QLabel* m_summaryNote = nullptr;

    // Charts
    QChart* m_payoffChart = nullptr;
    QChartView* m_payoffView = nullptr;
    QValueAxis* m_payoffX = nullptr;
    QValueAxis* m_payoffY = nullptr;
    QSlider* m_daysForward = nullptr;
    QLabel* m_daysLabel = nullptr;

    QChart* m_greekChart = nullptr;
    QChartView* m_greekView = nullptr;
    QValueAxis* m_greekX = nullptr;
    QValueAxis* m_greekY = nullptr;
    QComboBox* m_greekSelect = nullptr;
    QComboBox* m_greekMode = nullptr;      ///< across spot / across time
    QGroupBox* m_greekBox = nullptr;
};
