//
//  PricerTab.cpp
//  OptionPricing
//

#include "PricerTab.h"
#include "Formatting.h"
#include "../Pricing/FiniteDifference.h"
#include "../Pricing/MonteCarlo.h"

#include <cmath>

using namespace pricing;
using ui::money;
using ui::number;
using ui::percent;

PricerTab::PricerTab(MarketState& state, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
{
    buildUi();
    wire();
    refreshFromState();
    m_state.subscribe([this] { refreshFromState(); });
}

// MARK: - Construction

void PricerTab::buildUi()
{
    auto* inputsColumn = new QVBoxLayout;
    inputsColumn->setSpacing(12);
    buildInputs(inputsColumn);
    buildImpliedVol(inputsColumn);
    inputsColumn->addStretch(1);

    auto* resultsColumn = new QVBoxLayout;
    resultsColumn->setSpacing(12);
    buildOutputs(resultsColumn);
    resultsColumn->addStretch(1);

    auto* body = new QHBoxLayout;
    body->setSpacing(16);
    body->addLayout(inputsColumn, 4);
    body->addLayout(resultsColumn, 6);

    // Footer: status + actions
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setWordWrap(true);

    m_addCallLeg = ui::makeButton(this, "Add Call to Strategy", "secondary", "Add one long call with these terms to the Strategy tab");
    m_addPutLeg = ui::makeButton(this, "Add Put to Strategy", "secondary", "Add one long put with these terms to the Strategy tab");
    m_resetButton = ui::makeButton(this, "Reset", "secondary", "Restore the default example inputs");

    auto* footer = new QHBoxLayout;
    footer->addWidget(m_status, 1);
    footer->addWidget(m_addCallLeg);
    footer->addWidget(m_addPutLeg);
    footer->addWidget(m_resetButton);

    auto* content = new QWidget;
    content->setObjectName("root");
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);
    root->addLayout(body, 1);
    root->addLayout(footer);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void PricerTab::buildInputs(QVBoxLayout* column)
{
    m_underlying = new QComboBox(this);
    m_underlying->addItem("Spot asset (Black-Scholes-Merton)", static_cast<int>(Model::BlackScholesMerton));
    m_underlying->addItem("Futures (Black-76)", static_cast<int>(Model::Black76));
    m_underlying->setToolTip("Choose whether the option is written on a spot asset or on a futures contract");

    m_exercise = new QComboBox(this);
    m_exercise->addItem("European", static_cast<int>(Exercise::European));
    m_exercise->addItem("American", static_cast<int>(Exercise::American));
    m_exercise->setToolTip("American options are priced with a 400-step binomial tree; the European closed form is shown alongside");

    m_spot       = ui::makeSpinBox(this, 0.01, 1'000'000.0, 1.0,  2, 100.0);
    m_strike     = ui::makeSpinBox(this, 0.01, 1'000'000.0, 1.0,  2, 100.0);
    m_rate       = ui::makeSpinBox(this, -10.0, 100.0,      0.25, 3, 5.0,  " %");
    m_dividend   = ui::makeSpinBox(this, -10.0, 100.0,      0.25, 3, 0.0,  " %");
    m_volatility = ui::makeSpinBox(this, 0.01, 500.0,       1.0,  2, 20.0, " %");
    m_maturity   = ui::makeSpinBox(this, 0.0001, 50.0,      0.25, 4, 1.0,  " yrs");

    m_spot->setToolTip("Current price of the underlying asset");
    m_strike->setToolTip("Exercise price of the option");
    m_rate->setToolTip("Continuously compounded annual risk-free rate. Negative rates are allowed.");
    m_dividend->setToolTip("Continuously compounded annual dividend yield of the underlying");
    m_volatility->setToolTip("Annualised volatility of the underlying's log returns");
    m_maturity->setToolTip("Time to expiration in years (e.g. 0.5 for six months)");

    m_useCurve = new QCheckBox("Use rate curve", this);
    m_useCurve->setToolTip("Discount at the zero rate interpolated from the term structure for this expiry");
    m_editCurve = ui::makeButton(this, "Edit curve…", "secondary", "Enter zero rates by tenor");
    auto* rateRow = new QHBoxLayout;
    rateRow->setContentsMargins(0, 0, 0, 0);
    rateRow->addWidget(m_rate, 1);
    rateRow->addWidget(m_useCurve);
    rateRow->addWidget(m_editCurve);

    m_expiryMode = new QComboBox(this);
    m_expiryMode->addItem("Years", 0);
    m_expiryMode->addItem("Date", 1);
    m_expiryMode->setToolTip("Enter the expiry as a year fraction or pick a calendar date");

    m_expiryDate = new QDateEdit(QDate::currentDate().addYears(1), this);
    m_expiryDate->setCalendarPopup(true);
    m_expiryDate->setDisplayFormat("yyyy-MM-dd");
    m_expiryDate->setMinimumDate(QDate::currentDate().addDays(1));
    m_expiryDate->setToolTip("Expiration date. The year fraction follows the day-count convention.");

    m_dayCount = new QComboBox(this);
    m_dayCount->addItem("ACT/365", static_cast<int>(DayCount::Actual365));
    m_dayCount->addItem("ACT/360", static_cast<int>(DayCount::Actual360));
    m_dayCount->addItem("BUS/252", static_cast<int>(DayCount::Business252));
    m_dayCount->setToolTip("Day-count convention used to convert the expiry date into years");

    m_thetaBasis = new QComboBox(this);
    m_thetaBasis->addItem("per calendar day (÷365)", 365);
    m_thetaBasis->addItem("per trading day (÷252)", 252);
    m_thetaBasis->setToolTip("Basis for quoting theta, charm and color");

    auto* expiryRow = new QHBoxLayout;
    expiryRow->setContentsMargins(0, 0, 0, 0);
    expiryRow->addWidget(m_expiryMode);
    expiryRow->addWidget(m_maturity, 1);
    expiryRow->addWidget(m_expiryDate, 1);
    expiryRow->addWidget(m_dayCount);

    m_expiryInfo = new QLabel(this);
    m_expiryInfo->setObjectName("muted");

    // Discrete dividends
    m_dividendsPanel = new QWidget(this);
    m_dividendsTable = new QTableWidget(0, 2, m_dividendsPanel);
    m_dividendsTable->setHorizontalHeaderLabels({ "Time (yrs)", "Amount" });
    m_dividendsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_dividendsTable->verticalHeader()->setVisible(false);
    m_dividendsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_dividendsTable->setMaximumHeight(110);
    m_dividendsTable->setToolTip("Known cash dividends before expiry. Their present value is escrowed out of the spot.");
    m_addDividend = ui::makeButton(m_dividendsPanel, "+", "secondary", "Add a cash dividend");
    m_removeDividend = ui::makeButton(m_dividendsPanel, "−", "secondary", "Remove the selected dividend");
    auto* dividendButtons = new QVBoxLayout;
    dividendButtons->setContentsMargins(0, 0, 0, 0);
    dividendButtons->addWidget(m_addDividend);
    dividendButtons->addWidget(m_removeDividend);
    dividendButtons->addStretch(1);
    auto* dividendsLayout = new QHBoxLayout(m_dividendsPanel);
    dividendsLayout->setContentsMargins(0, 0, 0, 0);
    dividendsLayout->addWidget(m_dividendsTable, 1);
    dividendsLayout->addLayout(dividendButtons);

    // "&&" renders a literal ampersand; a single "&" would be read as a mnemonic marker.
    auto* inputsBox = new QGroupBox("Market && Contract Inputs", this);
    m_form = new QFormLayout(inputsBox);
    m_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_form->setFormAlignment(Qt::AlignTop);
    m_form->setHorizontalSpacing(14);
    m_form->setVerticalSpacing(10);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_form->addRow("Underlying", m_underlying);
    m_form->addRow("Exercise", m_exercise);
    m_form->addRow("Spot price (S)", m_spot);
    m_form->addRow("Strike price (K)", m_strike);
    m_form->addRow("Risk-free rate (r)", rateRow);
    m_form->addRow("Dividend yield (q)", m_dividend);
    m_form->addRow("Cash dividends", m_dividendsPanel);
    m_form->addRow("Volatility (σ)", m_volatility);
    m_form->addRow("Expiry (T)", expiryRow);
    m_form->addRow("", m_expiryInfo);
    m_form->addRow("Theta basis", m_thetaBasis);
    m_spotLabel = qobject_cast<QLabel*>(m_form->labelForField(m_spot));

    column->addWidget(inputsBox);
}

void PricerTab::buildImpliedVol(QVBoxLayout* column)
{
    m_ivType = new QComboBox(this);
    m_ivType->addItem("Call", static_cast<int>(OptionType::Call));
    m_ivType->addItem("Put", static_cast<int>(OptionType::Put));
    m_ivType->setToolTip("Which leg the observed market price refers to");

    m_marketPrice = ui::makeSpinBox(this, 0.0, 1'000'000.0, 0.10, 4, 0.0);
    m_marketPrice->setSpecialValueText("–");
    m_marketPrice->setToolTip("Observed market price of the option. Uses the contract inputs above.");

    m_ivValue = new QLabel("–", this);
    m_ivValue->setObjectName("ivValue");
    m_ivValue->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_ivValue->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_ivStatus = new QLabel(this);
    m_ivStatus->setObjectName("muted");
    m_ivStatus->setWordWrap(true);

    m_solveButton = ui::makeButton(this, "Solve", "secondary", "Find the volatility that reproduces the market price");
    m_applyIvButton = ui::makeButton(this, "Use as σ", "secondary", "Copy the solved implied volatility into the Volatility input");
    m_applyIvButton->setEnabled(false);

    auto* ivBox = new QGroupBox("Implied Volatility", this);
    auto* ivForm = new QFormLayout;
    ivForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ivForm->setHorizontalSpacing(14);
    ivForm->setVerticalSpacing(10);
    ivForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    ivForm->addRow("Option type", m_ivType);
    ivForm->addRow("Market price", m_marketPrice);

    auto* ivResultRow = new QHBoxLayout;
    ivResultRow->addWidget(m_ivValue, 1);
    ivResultRow->addWidget(m_applyIvButton);
    ivResultRow->addWidget(m_solveButton);

    auto* ivLayout = new QVBoxLayout(ivBox);
    ivLayout->setSpacing(10);
    ivLayout->addLayout(ivForm);
    ivLayout->addLayout(ivResultRow);
    ivLayout->addWidget(m_ivStatus);
    column->addWidget(ivBox);
}

void PricerTab::buildOutputs(QVBoxLayout* column)
{
    // Price cards
    auto* pricesBox = new QGroupBox("Option Prices", this);
    auto* pricesLayout = new QHBoxLayout(pricesBox);
    pricesLayout->setSpacing(12);
    m_callCard = ui::makeCard(this, "CALL", "callPrice");
    m_putCard = ui::makeCard(this, "PUT", "putPrice");
    pricesLayout->addWidget(m_callCard.frame);
    pricesLayout->addWidget(m_putCard.frame);
    column->addWidget(pricesBox);

    // Greeks table
    auto* greeksBox = new QGroupBox("Greeks", this);
    auto* grid = new QGridLayout(greeksBox);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 2);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);

    auto* callHeader = new QLabel("Call", greeksBox);
    callHeader->setObjectName("columnHeader");
    callHeader->setAlignment(Qt::AlignRight);
    auto* putHeader = new QLabel("Put", greeksBox);
    putHeader->setObjectName("columnHeader");
    putHeader->setAlignment(Qt::AlignRight);
    grid->addWidget(callHeader, 0, 1);
    grid->addWidget(putHeader, 0, 2);

    struct RowSpec {
        const char* name;
        const char* tooltip;
        GreekRow* row;
    };
    const RowSpec rows[] = {
        { "Delta  (∂V/∂S)",                "Change in option value per 1 unit move in the underlying", &m_delta },
        { "Gamma  (∂²V/∂S²)",              "Change in delta per 1 unit move in the underlying", &m_gamma },
        { "Vega  (per 1% vol)",            "Change in option value for a 1 percentage-point rise in volatility", &m_vega },
        { "Theta  (per day)",              "Change in option value as one day passes", &m_theta },
        { "Rho  (per 1% rate)",            "Change in option value for a 1 percentage-point rise in the rate", &m_rho },
        { "Vanna  (∂Δ/∂σ per 1%)",         "Change in delta for a 1 point rise in volatility", &m_vanna },
        { "Volga  (∂Vega/∂σ per 1%)",      "Change in vega for a 1 point rise in volatility", &m_volga },
        { "Charm  (∂Δ/∂t per day)",        "Change in delta as one day passes", &m_charm },
        { "Speed  (∂Γ/∂S)",                "Change in gamma per 1 unit move in the underlying", &m_speed },
        { "Color  (∂Γ/∂t per day)",        "Change in gamma as one day passes", &m_color },
        { "Zomma  (∂Γ/∂σ per 1%)",         "Change in gamma for a 1 point rise in volatility", &m_zomma },
    };
    int rowIndex = 1;
    for (const RowSpec& spec : rows) {
        auto* name = new QLabel(QString::fromUtf8(spec.name), greeksBox);
        name->setObjectName("rowLabel");
        name->setToolTip(spec.tooltip);
        spec.row->call = ui::makeValueLabel(greeksBox);
        spec.row->put = ui::makeValueLabel(greeksBox);
        grid->addWidget(name, rowIndex, 0);
        grid->addWidget(spec.row->call, rowIndex, 1);
        grid->addWidget(spec.row->put, rowIndex, 2);
        ++rowIndex;
        if (rowIndex == 6) {
            grid->addWidget(ui::makeSeparator(greeksBox), rowIndex++, 0, 1, 3);
        }
    }
    grid->addWidget(ui::makeSeparator(greeksBox), rowIndex++, 0, 1, 3);

    auto* d1Label = new QLabel("d₁", greeksBox);
    d1Label->setObjectName("rowLabel");
    auto* d2Label = new QLabel("d₂", greeksBox);
    d2Label->setObjectName("rowLabel");
    m_d1 = ui::makeValueLabel(greeksBox);
    m_d2 = ui::makeValueLabel(greeksBox);
    grid->addWidget(d1Label, rowIndex, 0);
    grid->addWidget(m_d1, rowIndex, 1, 1, 2);
    ++rowIndex;
    grid->addWidget(d2Label, rowIndex, 0);
    grid->addWidget(m_d2, rowIndex, 1, 1, 2);
    ++rowIndex;
    m_greeksNote = new QLabel(greeksBox);
    m_greeksNote->setObjectName("muted");
    m_greeksNote->setWordWrap(true);
    grid->addWidget(m_greeksNote, rowIndex, 0, 1, 3);
    column->addWidget(greeksBox);

    // Probabilities
    auto* probBox = new QGroupBox("Probabilities && Expected Move", this);
    auto* probGrid = new QGridLayout(probBox);
    probGrid->setHorizontalSpacing(18);
    probGrid->setVerticalSpacing(6);
    probGrid->setColumnStretch(0, 2);
    probGrid->setColumnStretch(1, 1);
    probGrid->setColumnStretch(2, 2);
    probGrid->setColumnStretch(3, 1);
    struct ProbSpec { const char* name; const char* tooltip; QLabel** label; int row; int col; };
    const ProbSpec probs[] = {
        { "P(call expires ITM)", "Risk-neutral probability N(d₂) that the spot finishes above the strike", &m_probCallItm, 0, 0 },
        { "P(put expires ITM)",  "Risk-neutral probability N(−d₂) that the spot finishes below the strike", &m_probPutItm, 1, 0 },
        { "P(touch strike)",     "Risk-neutral probability the spot path reaches the strike before expiry", &m_probTouch, 2, 0 },
        { "Forward price",       "Forward implied by spot, carry and discrete dividends", &m_forward, 3, 0 },
        { "1σ expected move",    "S · σ · √T", &m_oneSigma, 0, 2 },
        { "1σ range at expiry",  "Lognormal one-standard-deviation band of the terminal spot", &m_sigmaRange, 1, 2 },
        { "Call breakeven",      "Strike plus call premium", &m_callBreakeven, 2, 2 },
        { "Put breakeven",       "Strike minus put premium", &m_putBreakeven, 3, 2 },
    };
    for (const ProbSpec& spec : probs) {
        auto* name = new QLabel(spec.name, probBox);
        name->setObjectName("rowLabel");
        name->setToolTip(spec.tooltip);
        *spec.label = ui::makeValueLabel(probBox);
        probGrid->addWidget(name, spec.row, spec.col);
        probGrid->addWidget(*spec.label, spec.row, spec.col + 1);
    }
    column->addWidget(probBox);

    // Cross-check
    auto* checkBox = new QGroupBox("Engine Cross-Check", this);
    auto* checkLayout = new QVBoxLayout(checkBox);
    checkLayout->setSpacing(8);
    m_crossCheck = new QTableWidget(0, 4, checkBox);
    m_crossCheck->setHorizontalHeaderLabels({ "Engine", "Call", "Put", "Note" });
    m_crossCheck->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_crossCheck->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_crossCheck->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_crossCheck->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_crossCheck->verticalHeader()->setVisible(false);
    m_crossCheck->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_crossCheck->setSelectionMode(QAbstractItemView::NoSelection);
    m_crossCheck->setMinimumHeight(170);
    m_crossCheck->setAlternatingRowColors(true);
    m_crossCheckButton = ui::makeButton(checkBox, "Run Cross-Check", "secondary",
                                        "Re-price with a binomial tree, finite differences and Monte Carlo to confirm the closed form");
    auto* checkButtons = new QHBoxLayout;
    checkButtons->addStretch(1);
    checkButtons->addWidget(m_crossCheckButton);
    checkLayout->addWidget(m_crossCheck);
    checkLayout->addLayout(checkButtons);
    column->addWidget(checkBox);
}

void PricerTab::wire()
{
    auto marketChanged = [this] {
        if (m_updating) return;
        pushMarketToState();
    };
    connect(m_underlying, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updating) return;
        updateModelMode();
        pushMarketToState();
    });
    for (QDoubleSpinBox* box : { m_spot, m_rate, m_dividend, m_volatility }) {
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [marketChanged](double) { marketChanged(); });
    }
    connect(m_thetaBasis, qOverload<int>(&QComboBox::currentIndexChanged), this, [marketChanged](int) { marketChanged(); });
    connect(m_useCurve, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updating) return;
        m_state.useRateCurve = checked;
        updateRateMode();
        m_state.notify();
    });
    connect(m_editCurve, &QPushButton::clicked, this, [this] { if (onEditRateCurve) onEditRateCurve(); });

    connect(m_exercise, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { recalculate(); });
    connect(m_strike, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { recalculate(); });
    connect(m_maturity, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) {
        if (m_updating) return;
        updateExpiryMode();
        recalculate();
    });
    connect(m_expiryDate, &QDateEdit::dateChanged, this, [this](const QDate&) {
        if (m_updating) return;
        updateExpiryMode();
        recalculate();
    });
    connect(m_expiryMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        updateExpiryMode();
        recalculate();
    });
    connect(m_dayCount, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        updateExpiryMode();
        recalculate();
    });

    connect(m_addDividend, &QPushButton::clicked, this, [this] {
        addDividendRow(std::min(0.5, m_maturity->value() * 0.5), 1.0);
        syncDividendsToState();
    });
    connect(m_removeDividend, &QPushButton::clicked, this, [this] {
        const int row = m_dividendsTable->currentRow();
        if (row >= 0) {
            m_dividendsTable->removeRow(row);
        } else if (m_dividendsTable->rowCount() > 0) {
            m_dividendsTable->removeRow(m_dividendsTable->rowCount() - 1);
        }
        syncDividendsToState();
    });

    connect(m_solveButton, &QPushButton::clicked, this, [this] { solveImpliedVolatility(); });
    connect(m_applyIvButton, &QPushButton::clicked, this, [this] { applyImpliedVolatility(); });
    connect(m_ivType, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { solveImpliedVolatility(); });
    connect(m_marketPrice, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { solveImpliedVolatility(); });

    connect(m_crossCheckButton, &QPushButton::clicked, this, [this] { runCrossCheck(); });
    connect(m_addCallLeg, &QPushButton::clicked, this, [this] { addLegToStrategy(OptionType::Call); });
    connect(m_addPutLeg, &QPushButton::clicked, this, [this] { addLegToStrategy(OptionType::Put); });
    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        m_state.market = Market();
        m_state.useRateCurve = false;
        m_updating = true;
        m_exercise->setCurrentIndex(0);
        m_strike->setValue(100.0);
        m_expiryMode->setCurrentIndex(0);
        m_maturity->setValue(1.0);
        m_dayCount->setCurrentIndex(0);
        m_marketPrice->setValue(0.0);
        m_ivType->setCurrentIndex(0);
        m_updating = false;
        m_state.notify();
    });
}

// MARK: - State synchronisation

void PricerTab::refreshFromState()
{
    m_updating = true;
    const Market& m = m_state.market;
    m_underlying->setCurrentIndex(m_underlying->findData(static_cast<int>(m.model)));
    m_spot->setValue(m.spot);
    m_rate->setValue(m.riskFreeRate * 100.0);
    m_dividend->setValue(m.dividendYield * 100.0);
    m_volatility->setValue(m.volatility * 100.0);
    m_thetaBasis->setCurrentIndex(m.dayBasis == 252.0 ? 1 : 0);
    m_useCurve->setChecked(m_state.useRateCurve);
    rebuildDividendRows();
    updateModelMode();
    updateRateMode();
    updateExpiryMode();
    m_updating = false;
    recalculate();
}

void PricerTab::pushMarketToState()
{
    Market& m = m_state.market;
    m.model = static_cast<Model>(m_underlying->currentData().toInt());
    if (std::fabs(m.spot - m_spot->value()) > 1e-9) {
        // A hand-entered spot no longer comes from the data feed; stop the chain tab
        // from overriding it with the vendor or parity price until the next fetch.
        m_state.spotSource.clear();
        m_state.vendorSpot = 0.0;
        m_state.impliedSpot = 0.0;
    }
    m.spot = m_spot->value();
    // When the curve drives the rate the spin box only displays it; keep the flat rate.
    if (m_rate->isEnabled()) {
        m.riskFreeRate = m_rate->value() / 100.0;
    }
    m.dividendYield = m_dividend->value() / 100.0;
    m.volatility = m_volatility->value() / 100.0;
    m.dayBasis = m_thetaBasis->currentData().toDouble();
    m_state.notify();
}

void PricerTab::updateModelMode()
{
    const bool onFutures = (static_cast<Model>(m_underlying->currentData().toInt()) == Model::Black76);
    if (m_spotLabel) {
        m_spotLabel->setText(onFutures ? "Futures price (F)" : "Spot price (S)");
    }
    m_spot->setToolTip(onFutures ? "Current price of the underlying futures contract" : "Current price of the underlying asset");
    // A futures contract has no carry, so dividends are not inputs under Black-76.
    m_form->setRowVisible(m_dividend, !onFutures);
    m_form->setRowVisible(m_dividendsPanel, !onFutures);
}

void PricerTab::updateRateMode()
{
    const bool useCurve = m_useCurve->isChecked() && !m_state.rateCurve.empty();
    m_rate->setEnabled(!useCurve);
    if (useCurve) {
        const bool was = m_updating;
        m_updating = true;
        m_rate->setValue(m_state.rateCurve.rate(m_maturity->value()) * 100.0);
        m_updating = was;
        m_rate->setToolTip("Zero rate interpolated from the rate curve for this expiry");
    } else {
        m_rate->setToolTip("Continuously compounded annual risk-free rate. Negative rates are allowed.");
        if (m_useCurve->isChecked() && m_state.rateCurve.empty()) {
            // Nothing to interpolate yet: fall back to the flat rate silently.
            m_rate->setEnabled(true);
        }
    }
}

void PricerTab::updateExpiryMode()
{
    const bool was = m_updating;
    m_updating = true;
    const bool byDate = m_expiryMode->currentData().toInt() == 1;
    // Show only the active editor so the row has room for its text.
    m_maturity->setVisible(!byDate);
    m_expiryDate->setVisible(byDate);
    m_dayCount->setVisible(byDate);

    const QDate today = QDate::currentDate();
    if (byDate) {
        const double T = yearFraction(civil(today), civil(m_expiryDate->date()), dayCount());
        m_maturity->setValue(std::max(T, m_maturity->minimum()));
        const int calendarDays = calendarDaysBetween(civil(today), civil(m_expiryDate->date()));
        const int businessDays = businessDaysBetween(civil(today), civil(m_expiryDate->date()));
        m_expiryInfo->setText(QStringLiteral("%1 calendar days, %2 trading days → T = %3 years")
                                  .arg(calendarDays).arg(businessDays).arg(number(m_maturity->value(), 4)));
    } else {
        const int days = static_cast<int>(std::lround(m_maturity->value() * 365.0));
        m_expiryDate->setDate(today.addDays(std::max(days, 1)));
        m_expiryInfo->setText(QStringLiteral("≈ %1 calendar days (expires about %2)")
                                  .arg(days).arg(m_expiryDate->date().toString("yyyy-MM-dd")));
    }
    m_updating = was;
    if (m_useCurve->isChecked()) {
        updateRateMode();
    }
}

DayCount PricerTab::dayCount() const
{
    return static_cast<DayCount>(m_dayCount->currentData().toInt());
}

CivilDate PricerTab::civil(const QDate& date) const
{
    return CivilDate{ date.year(), date.month(), date.day() };
}

void PricerTab::addDividendRow(double time, double amount)
{
    const int row = m_dividendsTable->rowCount();
    m_dividendsTable->insertRow(row);
    auto* timeBox = ui::makeSpinBox(m_dividendsTable, 0.0, 50.0, 0.25, 3, time, " yrs");
    auto* amountBox = ui::makeSpinBox(m_dividendsTable, 0.0, 100000.0, 0.25, 2, amount);
    timeBox->setFrame(false);
    amountBox->setFrame(false);
    m_dividendsTable->setCellWidget(row, 0, timeBox);
    m_dividendsTable->setCellWidget(row, 1, amountBox);
    connect(timeBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { if (!m_updating) syncDividendsToState(); });
    connect(amountBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { if (!m_updating) syncDividendsToState(); });
}

void PricerTab::rebuildDividendRows()
{
    m_dividendsTable->setRowCount(0);
    for (const Dividend& d : m_state.market.dividends) {
        addDividendRow(d.time, d.amount);
    }
}

void PricerTab::syncDividendsToState()
{
    std::vector<Dividend> dividends;
    for (int row = 0; row < m_dividendsTable->rowCount(); ++row) {
        auto* timeBox = qobject_cast<QDoubleSpinBox*>(m_dividendsTable->cellWidget(row, 0));
        auto* amountBox = qobject_cast<QDoubleSpinBox*>(m_dividendsTable->cellWidget(row, 1));
        if (timeBox && amountBox) {
            dividends.push_back({ timeBox->value(), amountBox->value() });
        }
    }
    m_state.market.dividends = dividends;
    m_state.notify();
}

// MARK: - Valuation

Inputs PricerTab::currentInputs() const
{
    const Market& m = m_state.market;
    Inputs in;
    in.model = m.model;
    in.spot = m.spot;
    in.strike = m_strike->value();
    in.riskFreeRate = m_state.rateFor(m_maturity->value());
    in.dividendYield = m.dividendYield;
    in.volatility = m.volatility;
    in.maturity = m_maturity->value();
    in.dayBasis = m.dayBasis;
    in.dividends = m.dividends;
    return in;
}

Exercise PricerTab::exercise() const
{
    return static_cast<Exercise>(m_exercise->currentData().toInt());
}

void PricerTab::setContract(double strike, double maturity, double volatility)
{
    m_updating = true;
    m_expiryMode->setCurrentIndex(0);
    m_strike->setValue(strike);
    m_maturity->setValue(std::clamp(maturity, m_maturity->minimum(), m_maturity->maximum()));
    m_updating = false;
    updateExpiryMode();
    if (volatility > 0.0) {
        m_state.market.volatility = volatility;
        m_state.notify();
    } else {
        recalculate();
    }
}

void PricerTab::setContractWithDate(double strike, const QDate& expiry, double volatility)
{
    if (!expiry.isValid() || expiry <= QDate::currentDate()) {
        setContract(strike, std::max(1.0 / 365.0, QDate::currentDate().daysTo(expiry) / 365.0), volatility);
        return;
    }
    m_updating = true;
    m_strike->setValue(strike);
    m_expiryDate->setDate(expiry);
    m_expiryMode->setCurrentIndex(1);
    m_updating = false;
    updateExpiryMode();
    if (volatility > 0.0) {
        m_state.market.volatility = volatility;
        m_state.notify();
    } else {
        recalculate();
    }
}

void PricerTab::recalculate()
{
    if (m_updating) {
        return;
    }
    const Inputs in = currentInputs();
    m_hasResult = false;
    if (!isValid(in)) {
        clearResults(effectiveSpot(in) <= 0.0
                         ? "The present value of the cash dividends exceeds the spot price."
                         : "Spot, strike, volatility and time to expiry must all be greater than zero.");
    } else {
        const Result result = price(in);
        if (!std::isfinite(result.callPrice) || !std::isfinite(result.putPrice)) {
            clearResults("The inputs produced a non-finite result. Check the values and try again.");
        } else {
            m_lastResult = result;
            m_lastProbabilities = probabilities(in, result);
            if (exercise() == Exercise::American) {
                m_lastAmericanCall = americanValuation(in, OptionType::Call);
                m_lastAmericanPut = americanValuation(in, OptionType::Put);
                showResult(result, &m_lastAmericanCall, &m_lastAmericanPut);
            } else {
                showResult(result, nullptr, nullptr);
            }
            m_hasResult = true;
        }
    }
    m_crossCheck->setRowCount(0);
    m_addCallLeg->setEnabled(m_hasResult);
    m_addPutLeg->setEnabled(m_hasResult);

    // The implied volatility depends on the same contract inputs, so keep it in sync.
    solveImpliedVolatility();
}

void PricerTab::showResult(const Result& result, const AmericanResult* call, const AmericanResult* put)
{
    const Inputs in = currentInputs();
    const bool american = call && put;

    if (american) {
        m_callCard.value->setText(money(call->price));
        m_putCard.value->setText(money(put->price));
        m_callCard.subtitle->setText(QStringLiteral("European %1 · early exercise premium %2")
                                         .arg(number(result.callPrice), number(call->earlyExercisePremium)));
        m_putCard.subtitle->setText(QStringLiteral("European %1 · early exercise premium %2")
                                        .arg(number(result.putPrice), number(put->earlyExercisePremium)));
    } else {
        m_callCard.value->setText(money(result.callPrice));
        m_putCard.value->setText(money(result.putPrice));
        const double intrinsicCall = std::max(in.spot - in.strike, 0.0);
        const double intrinsicPut = std::max(in.strike - in.spot, 0.0);
        m_callCard.subtitle->setText(QStringLiteral("intrinsic %1 · time value %2")
                                         .arg(number(intrinsicCall), number(result.callPrice - intrinsicCall)));
        m_putCard.subtitle->setText(QStringLiteral("intrinsic %1 · time value %2")
                                        .arg(number(intrinsicPut), number(result.putPrice - intrinsicPut)));
    }

    const Greeks& gc = american ? call->greeks : result.call;
    const Greeks& gp = american ? put->greeks : result.put;
    m_delta.call->setText(number(gc.delta));   m_delta.put->setText(number(gp.delta));
    m_gamma.call->setText(number(gc.gamma));   m_gamma.put->setText(number(gp.gamma));
    m_vega.call->setText(number(gc.vega));     m_vega.put->setText(number(gp.vega));
    m_theta.call->setText(number(gc.theta));   m_theta.put->setText(number(gp.theta));
    m_rho.call->setText(number(gc.rho));       m_rho.put->setText(number(gp.rho));
    // Second-order Greeks always come from the closed form.
    m_vanna.call->setText(number(result.call.vanna)); m_vanna.put->setText(number(result.put.vanna));
    m_volga.call->setText(number(result.call.volga)); m_volga.put->setText(number(result.put.volga));
    m_charm.call->setText(number(result.call.charm, 6)); m_charm.put->setText(number(result.put.charm, 6));
    m_speed.call->setText(number(result.call.speed, 6)); m_speed.put->setText(number(result.put.speed, 6));
    m_color.call->setText(number(result.call.color, 6)); m_color.put->setText(number(result.put.color, 6));
    m_zomma.call->setText(number(result.call.zomma)); m_zomma.put->setText(number(result.put.zomma));
    m_d1->setText(number(result.d1));
    m_d2->setText(number(result.d2));
    m_greeksNote->setText(american
        ? "American first-order Greeks come from the binomial tree (vega and rho by bumping). Second-order Greeks use the European closed form."
        : QString());

    const Probabilities& p = m_lastProbabilities;
    m_probCallItm->setText(percent(p.callInTheMoney));
    m_probPutItm->setText(percent(p.putInTheMoney));
    m_probTouch->setText(percent(p.touchStrike));
    m_forward->setText(number(result.forward, 4));
    m_oneSigma->setText(QStringLiteral("± %1").arg(number(p.oneSigmaMove, 2)));
    m_sigmaRange->setText(QStringLiteral("%1 – %2").arg(number(p.lowerOneSigma, 2), number(p.upperOneSigma, 2)));
    m_callBreakeven->setText(number(p.callBreakeven, 2));
    m_putBreakeven->setText(number(p.putBreakeven, 2));

    // Put-call parity check gives the user confidence that the numbers are consistent.
    const double parityLhs = result.callPrice - result.putPrice;
    const double discountR = std::exp(-in.riskFreeRate * in.maturity);
    QString parityText;
    if (in.model == Model::Black76) {
        const double parityRhs = discountR * (in.spot - in.strike);
        parityText = QStringLiteral("Black-76 put-call parity: C − P = %1, e^(−rT)·(F − K) = %2")
                         .arg(number(parityLhs), number(parityRhs));
    } else {
        const double parityRhs = result.effectiveSpot * std::exp(-in.dividendYield * in.maturity) - in.strike * discountR;
        parityText = QStringLiteral("Put-call parity (European): C − P = %1, S*·e^(−qT) − K·e^(−rT) = %2")
                         .arg(number(parityLhs), number(parityRhs));
        if (!in.dividends.empty()) {
            parityText += QStringLiteral("  ·  S* = spot − PV(dividends) = %1").arg(number(result.effectiveSpot, 4));
        }
    }
    if (m_state.useRateCurve && !m_state.rateCurve.empty()) {
        parityText += QStringLiteral("  ·  r(T) = %1 from curve").arg(percent(in.riskFreeRate, 3));
    }
    ui::setStatus(m_status, parityText, ui::StatusKind::Info);
}

void PricerTab::clearResults(const QString& reason)
{
    for (GreekRow* row : { &m_delta, &m_gamma, &m_vega, &m_theta, &m_rho, &m_vanna, &m_volga, &m_charm, &m_speed, &m_color, &m_zomma }) {
        row->call->setText("–");
        row->put->setText("–");
    }
    for (QLabel* label : { m_d1, m_d2, m_probCallItm, m_probPutItm, m_probTouch, m_oneSigma, m_sigmaRange, m_forward,
                           m_callBreakeven, m_putBreakeven, m_callCard.value, m_putCard.value }) {
        label->setText("–");
    }
    m_callCard.subtitle->clear();
    m_putCard.subtitle->clear();
    m_greeksNote->clear();
    ui::setStatus(m_status, reason, ui::StatusKind::Error);
}

void PricerTab::solveImpliedVolatility()
{
    using Status = ImpliedVolResult::Status;

    m_applyIvButton->setEnabled(false);
    m_ivValue->setText("–");

    const double marketPrice = m_marketPrice->value();
    if (marketPrice <= 0.0) {
        ui::setStatus(m_ivStatus, "Enter the observed option price to solve for the implied volatility.", ui::StatusKind::Info);
        return;
    }

    const Inputs in = currentInputs();
    const OptionType type = static_cast<OptionType>(m_ivType->currentData().toInt());
    const ImpliedVolResult iv = impliedVolatility(in, type, marketPrice);
    const QString leg = (type == OptionType::Call) ? "call" : "put";

    switch (iv.status) {
    case Status::Converged:
        m_solvedVolatilityPercent = iv.volatility * 100.0;
        m_ivValue->setText(number(m_solvedVolatilityPercent, 2) + "%");
        m_applyIvButton->setEnabled(true);
        ui::setStatus(m_ivStatus,
                      QStringLiteral("Converged in %1 iteration%2. European %3 price at this volatility reproduces %4.")
                          .arg(iv.iterations).arg(iv.iterations == 1 ? "" : "s").arg(leg, money(marketPrice)),
                      ui::StatusKind::Info);
        break;
    case Status::InvalidInputs:
        ui::setStatus(m_ivStatus, "Spot, strike and time to expiry must be greater than zero.", ui::StatusKind::Error);
        break;
    case Status::BelowLowerBound:
        ui::setStatus(m_ivStatus,
                      QStringLiteral("No solution: the %1 price has no time value. It must exceed the discounted intrinsic value of %2.")
                          .arg(leg, money(iv.lowerBound)),
                      ui::StatusKind::Error);
        break;
    case Status::AboveUpperBound:
        ui::setStatus(m_ivStatus,
                      QStringLiteral("No solution: the %1 price violates the no-arbitrage ceiling of %2.").arg(leg, money(iv.upperBound)),
                      ui::StatusKind::Error);
        break;
    case Status::NotConverged:
        ui::setStatus(m_ivStatus, "The solver did not converge. Check the inputs and try again.", ui::StatusKind::Error);
        break;
    }
}

void PricerTab::applyImpliedVolatility()
{
    if (!m_applyIvButton->isEnabled()) {
        return;
    }
    m_state.market.volatility = std::clamp(m_solvedVolatilityPercent, m_volatility->minimum(), m_volatility->maximum()) / 100.0;
    m_state.notify();
}

void PricerTab::runCrossCheck()
{
    m_crossCheck->setRowCount(0);
    if (!m_hasResult) {
        return;
    }
    const Inputs in = currentInputs();
    const Exercise ex = exercise();
    const bool american = ex == Exercise::American;

    struct Row { QString engine; QString call; QString put; QString note; };
    std::vector<Row> rows;

    rows.push_back({ in.model == Model::Black76 ? "Black-76 closed form" : "Black-Scholes-Merton closed form",
                     number(m_lastResult.callPrice), number(m_lastResult.putPrice), "European reference" });

    const TreeResult treeCall = binomialPrice(in, OptionType::Call, ex, 400);
    const TreeResult treePut = binomialPrice(in, OptionType::Put, ex, 400);
    rows.push_back({ "Binomial tree (CRR, 400 steps)", number(treeCall.price), number(treePut.price),
                     american ? "American exercise" : QStringLiteral("European; Δ %1 / %2").arg(number(treeCall.delta), number(treePut.delta)) });

    const TreeResult treeCallFine = binomialPrice(in, OptionType::Call, ex, 1000);
    const TreeResult treePutFine = binomialPrice(in, OptionType::Put, ex, 1000);
    rows.push_back({ "Binomial tree (CRR, 1000 steps)", number(treeCallFine.price), number(treePutFine.price), "convergence check" });

    if (american) {
        rows.push_back({ "Bjerksund-Stensland (1993)", number(bjerksundStenslandPrice(in, OptionType::Call)),
                         number(bjerksundStenslandPrice(in, OptionType::Put)), "closed-form approximation" });
    }

    const FiniteDifferenceResult fdCall = finiteDifferencePrice(in, OptionType::Call, ex);
    const FiniteDifferenceResult fdPut = finiteDifferencePrice(in, OptionType::Put, ex);
    rows.push_back({ "Crank-Nicolson finite difference (400×400)", number(fdCall.price), number(fdPut.price),
                     QStringLiteral("Δ %1 / %2 · Γ %3").arg(number(fdCall.delta), number(fdPut.delta), number(fdCall.gamma)) });

    const MonteCarloResult mcCall = monteCarloPrice(in, OptionType::Call);
    const MonteCarloResult mcPut = monteCarloPrice(in, OptionType::Put);
    rows.push_back({ "Monte Carlo (200k paths, antithetic)", number(mcCall.price), number(mcPut.price),
                     QStringLiteral("European · s.e. ± %1 / ± %2").arg(number(mcCall.standardError), number(mcPut.standardError)) });

    m_crossCheck->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        m_crossCheck->setItem(i, 0, ui::makeCell(rows[static_cast<size_t>(i)].engine, Qt::AlignLeft | Qt::AlignVCenter));
        m_crossCheck->setItem(i, 1, ui::makeCell(rows[static_cast<size_t>(i)].call));
        m_crossCheck->setItem(i, 2, ui::makeCell(rows[static_cast<size_t>(i)].put));
        m_crossCheck->setItem(i, 3, ui::makeCell(rows[static_cast<size_t>(i)].note, Qt::AlignLeft | Qt::AlignVCenter));
    }
}

void PricerTab::addLegToStrategy(OptionType type)
{
    if (!m_hasResult || !onAddLeg) {
        return;
    }
    Leg leg;
    leg.kind = type == OptionType::Call ? LegKind::Call : LegKind::Put;
    leg.quantity = 1.0;
    leg.strike = m_strike->value();
    leg.maturity = m_maturity->value();
    leg.volatility = 0.0;
    leg.entryPrice = type == OptionType::Call ? m_lastResult.callPrice : m_lastResult.putPrice;
    onAddLeg(leg);
}

// MARK: - Theme, persistence, export

void PricerTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
}

QJsonObject PricerTab::toJson() const
{
    QJsonObject o;
    o["exercise"] = exercise() == Exercise::American ? "american" : "european";
    o["strike"] = m_strike->value();
    o["maturity"] = m_maturity->value();
    o["expiryByDate"] = m_expiryMode->currentData().toInt() == 1;
    o["expiryDate"] = m_expiryDate->date().toString(Qt::ISODate);
    o["dayCount"] = m_dayCount->currentData().toInt();
    o["marketPrice"] = m_marketPrice->value();
    o["ivType"] = m_ivType->currentData().toInt();
    return o;
}

void PricerTab::fromJson(const QJsonObject& o)
{
    m_updating = true;
    m_exercise->setCurrentIndex(o["exercise"].toString() == "american" ? 1 : 0);
    m_strike->setValue(o["strike"].toDouble(100.0));
    m_dayCount->setCurrentIndex(std::max(0, m_dayCount->findData(o["dayCount"].toInt(0))));
    const QDate date = QDate::fromString(o["expiryDate"].toString(), Qt::ISODate);
    if (date.isValid() && date > QDate::currentDate()) {
        m_expiryDate->setDate(date);
    }
    m_maturity->setValue(o["maturity"].toDouble(1.0));
    m_expiryMode->setCurrentIndex(o["expiryByDate"].toBool(false) ? 1 : 0);
    m_marketPrice->setValue(o["marketPrice"].toDouble(0.0));
    m_ivType->setCurrentIndex(std::max(0, m_ivType->findData(o["ivType"].toInt(0))));
    m_updating = false;
    updateExpiryMode();
    recalculate();
}

QString PricerTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    const Inputs in = currentInputs();
    s << "field,value\n";
    s << "model," << (in.model == Model::Black76 ? "Black-76" : "Black-Scholes-Merton") << "\n";
    s << "exercise," << (exercise() == Exercise::American ? "American" : "European") << "\n";
    s << "spot," << in.spot << "\nstrike," << in.strike << "\nrate," << in.riskFreeRate << "\ndividend_yield," << in.dividendYield
      << "\nvolatility," << in.volatility << "\nmaturity_years," << in.maturity << "\n\n";
    if (!m_hasResult) {
        s << "status,invalid inputs\n";
        return out;
    }
    const bool american = exercise() == Exercise::American;
    const Greeks& gc = american ? m_lastAmericanCall.greeks : m_lastResult.call;
    const Greeks& gp = american ? m_lastAmericanPut.greeks : m_lastResult.put;
    s << "measure,call,put\n";
    s << "price," << (american ? m_lastAmericanCall.price : m_lastResult.callPrice) << "," << (american ? m_lastAmericanPut.price : m_lastResult.putPrice) << "\n";
    s << "european_price," << m_lastResult.callPrice << "," << m_lastResult.putPrice << "\n";
    s << "delta," << gc.delta << "," << gp.delta << "\n";
    s << "gamma," << gc.gamma << "," << gp.gamma << "\n";
    s << "vega," << gc.vega << "," << gp.vega << "\n";
    s << "theta," << gc.theta << "," << gp.theta << "\n";
    s << "rho," << gc.rho << "," << gp.rho << "\n";
    s << "vanna," << m_lastResult.call.vanna << "," << m_lastResult.put.vanna << "\n";
    s << "volga," << m_lastResult.call.volga << "," << m_lastResult.put.volga << "\n";
    s << "charm," << m_lastResult.call.charm << "," << m_lastResult.put.charm << "\n";
    s << "speed," << m_lastResult.call.speed << "," << m_lastResult.put.speed << "\n";
    s << "color," << m_lastResult.call.color << "," << m_lastResult.put.color << "\n";
    s << "zomma," << m_lastResult.call.zomma << "," << m_lastResult.put.zomma << "\n";
    s << "d1," << m_lastResult.d1 << ",\n";
    s << "d2," << m_lastResult.d2 << ",\n";
    s << "prob_itm," << m_lastProbabilities.callInTheMoney << "," << m_lastProbabilities.putInTheMoney << "\n";
    s << "prob_touch," << m_lastProbabilities.touchStrike << ",\n";
    s << "breakeven," << m_lastProbabilities.callBreakeven << "," << m_lastProbabilities.putBreakeven << "\n";
    return out;
}
