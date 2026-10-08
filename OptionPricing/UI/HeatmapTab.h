//
//  HeatmapTab.h
//  OptionPricing
//
//  Activity heatmap for the loaded option chain: a strike x expiry grid coloured by
//  volume or open interest, a ranking of the most active contracts, and put-call parity
//  by expiry. Works on whatever chain the Option Chain tab has loaded (live or CSV).
//

#pragma once

#include "QtHeaders.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Activity.h"

#include <functional>

class HeatmapTab : public QWidget
{
public:
    explicit HeatmapTab(MarketState& state, QWidget* parent = nullptr);

    void refresh();
    void applyTheme(const Theme& theme);
    QString resultsCsv() const;

    /// Asks the Option Chain tab to download a chain for the ticker typed here.
    std::function<void(const QString& ticker)> onRequestFetch;
    /// Sends a contract to the Pricer (strike, maturity, implied vol, ISO expiry date).
    std::function<void(double strike, double maturity, double volatility, const QString& expiryDate)> onSendToPricer;

private:
    void buildUi();
    void wire();
    pricing::ActivityMetric metric() const;
    pricing::SideFilter side() const;
    pricing::ActivityMarket activityMarket() const;
    void fillGrid();
    void fillMostActive();
    void fillParity();
    void fillSummary();

    MarketState& m_state;
    Theme m_theme;
    bool m_updating = false;

    pricing::ActivityGrid m_grid;
    std::vector<pricing::ActiveContract> m_topContracts;
    std::vector<pricing::ParityRow> m_parity;
    pricing::ActivitySummary m_summary;

    QLineEdit* m_ticker = nullptr;
    QPushButton* m_fetch = nullptr;
    QComboBox* m_metric = nullptr;
    QComboBox* m_side = nullptr;
    QDoubleSpinBox* m_band = nullptr;
    QSpinBox* m_topCount = nullptr;
    QLabel* m_status = nullptr;

    ui::Card m_cardVolume, m_cardOpenInterest, m_cardPutCall, m_cardBusiestExpiry, m_cardBusiestStrike, m_cardParity;

    QTableWidget* m_heatmap = nullptr;
    QTableWidget* m_active = nullptr;
    QTableWidget* m_parityTable = nullptr;
    QPushButton* m_toggleTables = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_lowerPane = nullptr;
};
