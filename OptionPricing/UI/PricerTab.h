//
//  PricerTab.h
//  OptionPricing
//
//  Single-contract pricer: market and contract inputs, European and American prices,
//  first- and second-order Greeks, probability metrics, implied volatility and an
//  independent cross-check of the engines. This tab is the editor for the shared
//  MarketState.
//

#pragma once

#include "QtHeaders.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/American.h"
#include "../Pricing/DayCount.h"

#include <functional>

class PricerTab : public QWidget
{
public:
    explicit PricerTab(MarketState& state, QWidget* parent = nullptr);

    /// Pricing inputs with the rate resolved from the curve when enabled.
    pricing::Inputs currentInputs() const;
    pricing::Exercise exercise() const;

    /// Sets the contract from another tab (option chain double-click). Volatility is a
    /// decimal; pass 0 to leave the market volatility untouched.
    void setContract(double strike, double maturity, double volatility);

    /// Same, but switches the expiry editor to date mode with the actual expiration date.
    void setContractWithDate(double strike, const QDate& expiry, double volatility);

    void applyTheme(const Theme& theme);
    void refreshFromState();
    void recalculate();

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& json);

    /// Results of the last valuation as CSV rows (label, call, put).
    QString resultsCsv() const;

    std::function<void(const pricing::Leg&)> onAddLeg;
    std::function<void()> onEditRateCurve;

private:
    void buildUi();
    void buildInputs(QVBoxLayout* column);
    void buildImpliedVol(QVBoxLayout* column);
    void buildOutputs(QVBoxLayout* column);
    void wire();

    void updateModelMode();
    void updateExpiryMode();
    void updateRateMode();
    void syncDividendsToState();
    void rebuildDividendRows();
    void addDividendRow(double time, double amount);
    void pushMarketToState();

    void showResult(const pricing::Result& result, const pricing::AmericanResult* call, const pricing::AmericanResult* put);
    void clearResults(const QString& reason);
    void solveImpliedVolatility();
    void applyImpliedVolatility();
    void runCrossCheck();
    void addLegToStrategy(pricing::OptionType type);

    pricing::DayCount dayCount() const;
    pricing::CivilDate civil(const QDate& date) const;

    MarketState& m_state;
    Theme m_theme;
    bool m_updating = false;

    // Inputs
    QComboBox* m_underlying = nullptr;
    QComboBox* m_exercise = nullptr;
    QFormLayout* m_form = nullptr;
    QLabel* m_spotLabel = nullptr;
    QDoubleSpinBox* m_spot = nullptr;
    QDoubleSpinBox* m_strike = nullptr;
    QDoubleSpinBox* m_rate = nullptr;
    QCheckBox* m_useCurve = nullptr;
    QPushButton* m_editCurve = nullptr;
    QDoubleSpinBox* m_dividend = nullptr;
    QDoubleSpinBox* m_volatility = nullptr;
    QComboBox* m_expiryMode = nullptr;
    QDoubleSpinBox* m_maturity = nullptr;
    QDateEdit* m_expiryDate = nullptr;
    QComboBox* m_dayCount = nullptr;
    QComboBox* m_thetaBasis = nullptr;
    QLabel* m_expiryInfo = nullptr;
    QWidget* m_dividendsPanel = nullptr;
    QTableWidget* m_dividendsTable = nullptr;
    QPushButton* m_addDividend = nullptr;
    QPushButton* m_removeDividend = nullptr;

    // Implied volatility
    QComboBox* m_ivType = nullptr;
    QDoubleSpinBox* m_marketPrice = nullptr;
    QLabel* m_ivValue = nullptr;
    QLabel* m_ivStatus = nullptr;
    QPushButton* m_solveButton = nullptr;
    QPushButton* m_applyIvButton = nullptr;
    double m_solvedVolatilityPercent = 0.0;

    // Outputs
    ui::Card m_callCard;
    ui::Card m_putCard;
    struct GreekRow {
        QLabel* call = nullptr;
        QLabel* put = nullptr;
    };
    GreekRow m_delta, m_gamma, m_vega, m_theta, m_rho, m_vanna, m_volga, m_charm, m_speed, m_color, m_zomma;
    QLabel* m_d1 = nullptr;
    QLabel* m_d2 = nullptr;
    QLabel* m_greeksNote = nullptr;

    QLabel* m_probCallItm = nullptr;
    QLabel* m_probPutItm = nullptr;
    QLabel* m_probTouch = nullptr;
    QLabel* m_oneSigma = nullptr;
    QLabel* m_sigmaRange = nullptr;
    QLabel* m_forward = nullptr;
    QLabel* m_callBreakeven = nullptr;
    QLabel* m_putBreakeven = nullptr;

    QTableWidget* m_crossCheck = nullptr;
    QPushButton* m_crossCheckButton = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_addCallLeg = nullptr;
    QPushButton* m_addPutLeg = nullptr;
    QPushButton* m_resetButton = nullptr;

    // Last results, for export.
    pricing::Result m_lastResult;
    pricing::AmericanResult m_lastAmericanCall;
    pricing::AmericanResult m_lastAmericanPut;
    pricing::Probabilities m_lastProbabilities;
    bool m_hasResult = false;
};
