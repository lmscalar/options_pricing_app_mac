//
//  StrategyTab.cpp
//  OptionPricing
//

#include "StrategyTab.h"
#include "Formatting.h"

#include <cmath>

using namespace pricing;
using ui::number;
using ui::percent;
using ui::signedMoney;

namespace {

QString legKindName(LegKind kind)
{
    switch (kind) {
    case LegKind::Call:       return "Call";
    case LegKind::Put:        return "Put";
    case LegKind::Underlying: return "Underlying";
    }
    return QString();
}

double greekValue(const Greeks& g, int index)
{
    switch (index) {
    case 0: return g.delta;
    case 1: return g.gamma;
    case 2: return g.vega;
    case 3: return g.theta;
    case 4: return g.rho;
    case 5: return g.vanna;
    case 6: return g.charm;
    default: return 0.0;
    }
}

} // namespace

StrategyTab::StrategyTab(MarketState& state, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
{
    buildUi();
    wire();
    m_state.subscribe([this] { recompute(); });
    recompute();
}

// MARK: - Construction

void StrategyTab::buildUi()
{
    // Preset bar
    m_presets = new QComboBox(this);
    for (const PresetInfo& p : strategyPresets()) {
        m_presets->addItem(p.name, static_cast<int>(p.id));
    }
    m_presets->setCurrentIndex(static_cast<int>(StrategyPreset::IronCondor));
    m_presets->setToolTip("Standard strategies built around the current spot");
    m_presetDescription = new QLabel(this);
    m_presetDescription->setObjectName("muted");
    m_presetDescription->setWordWrap(true);

    m_presetMaturity = ui::makeSpinBox(this, 0.01, 10.0, 0.25, 2, 0.5, " yrs");
    m_presetMaturity->setToolTip("Expiry used for preset legs (calendars use twice this for the far leg)");
    m_strikeStep = ui::makeSpinBox(this, 0.01, 10000.0, 1.0, 2, 5.0);
    m_strikeStep->setToolTip("Strike interval used to place preset wings");
    m_multiplier = ui::makeSpinBox(this, 1.0, 100000.0, 1.0, 0, 100.0);
    m_multiplier->setToolTip("Contract multiplier (100 for US equity options)");
    m_loadPreset = ui::makeButton(this, "Load Preset", "primary", "Replace the legs with the selected preset");

    auto* presetBox = new QGroupBox("Presets", this);
    auto* presetForm = new QGridLayout(presetBox);
    presetForm->setHorizontalSpacing(12);
    presetForm->setVerticalSpacing(8);
    presetForm->addWidget(new QLabel("Strategy", presetBox), 0, 0);
    presetForm->addWidget(m_presets, 0, 1);
    presetForm->addWidget(new QLabel("Expiry", presetBox), 0, 2);
    presetForm->addWidget(m_presetMaturity, 0, 3);
    presetForm->addWidget(new QLabel("Strike step", presetBox), 0, 4);
    presetForm->addWidget(m_strikeStep, 0, 5);
    presetForm->addWidget(new QLabel("Multiplier", presetBox), 0, 6);
    presetForm->addWidget(m_multiplier, 0, 7);
    presetForm->addWidget(m_loadPreset, 0, 8);
    presetForm->addWidget(m_presetDescription, 1, 0, 1, 9);
    presetForm->setColumnStretch(1, 2);

    // Legs table
    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Type", "Qty", "Strike", "Expiry (yrs)", "IV % (0 = mkt)", "Entry price", "Model price", "Unit P&L", "Position P&L" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setMinimumHeight(160);
    m_table->setToolTip("Positive quantity is long, negative is short. Entry price is the premium per unit paid or received.");

    m_addLeg = ui::makeButton(this, "Add Leg", "secondary", "Append an at-the-money call");
    m_removeLeg = ui::makeButton(this, "Remove Leg", "secondary", "Remove the selected leg");
    m_clearLegs = ui::makeButton(this, "Clear", "secondary", "Remove every leg");
    m_repriceEntries = ui::makeButton(this, "Reprice Entries at Model", "secondary", "Set each entry price to the current model price");
    m_applySurface = ui::makeButton(this, "Apply Vol Surface to Legs", "secondary", "Set each leg's implied vol from the fitted option-chain surface");
    m_applySurface->setEnabled(false);

    auto* legButtons = new QHBoxLayout;
    legButtons->addWidget(m_addLeg);
    legButtons->addWidget(m_removeLeg);
    legButtons->addWidget(m_clearLegs);
    legButtons->addStretch(1);
    legButtons->addWidget(m_applySurface);
    legButtons->addWidget(m_repriceEntries);

    auto* legsBox = new QGroupBox("Legs", this);
    auto* legsLayout = new QVBoxLayout(legsBox);
    legsLayout->setSpacing(8);
    legsLayout->addWidget(m_table);
    legsLayout->addLayout(legButtons);

    // Summary cards
    m_netPremium = ui::makeCard(this, "NET PREMIUM");
    m_maxProfit = ui::makeCard(this, "MAX PROFIT");
    m_maxLoss = ui::makeCard(this, "MAX LOSS");
    m_breakevens = ui::makeCard(this, "BREAKEVENS");
    m_probabilityOfProfit = ui::makeCard(this, "PROB. OF PROFIT");
    m_expectedPnl = ui::makeCard(this, "EXPECTED P&L");
    m_netPremium.frame->setToolTip("Net debit paid (positive) or credit received (negative) to open the position");
    m_maxProfit.frame->setToolTip("Best P&L at the first expiry across all spot levels");
    m_maxLoss.frame->setToolTip("Worst P&L at the first expiry across all spot levels");
    m_breakevens.frame->setToolTip("Spot levels where P&L at the first expiry is zero");
    m_probabilityOfProfit.frame->setToolTip("Risk-neutral probability that P&L at the first expiry is positive");
    m_expectedPnl.frame->setToolTip("Risk-neutral expected P&L at the first expiry");
    auto* cards = new QGridLayout;
    cards->setSpacing(10);
    cards->addWidget(m_netPremium.frame, 0, 0);
    cards->addWidget(m_maxProfit.frame, 0, 1);
    cards->addWidget(m_maxLoss.frame, 0, 2);
    cards->addWidget(m_breakevens.frame, 1, 0);
    cards->addWidget(m_probabilityOfProfit.frame, 1, 1);
    cards->addWidget(m_expectedPnl.frame, 1, 2);

    // Net Greeks
    auto* greeksBox = new QGroupBox("Net Greeks (today)", this);
    auto* gGrid = new QGridLayout(greeksBox);
    gGrid->setHorizontalSpacing(16);
    gGrid->setVerticalSpacing(6);
    struct Spec { const char* name; QLabel** label; };
    const Spec specs[] = {
        { "Delta", &m_gDelta }, { "Gamma", &m_gGamma }, { "Vega (per 1%)", &m_gVega }, { "Theta (per day)", &m_gTheta },
        { "Rho (per 1%)", &m_gRho }, { "Vanna (per 1%)", &m_gVanna }, { "Charm (per day)", &m_gCharm },
    };
    int row = 0;
    for (const Spec& spec : specs) {
        auto* name = new QLabel(spec.name, greeksBox);
        name->setObjectName("rowLabel");
        *spec.label = ui::makeValueLabel(greeksBox);
        gGrid->addWidget(name, row, 0);
        gGrid->addWidget(*spec.label, row, 1);
        ++row;
    }
    m_summaryNote = new QLabel(greeksBox);
    m_summaryNote->setObjectName("muted");
    m_summaryNote->setWordWrap(true);
    gGrid->addWidget(m_summaryNote, row, 0, 1, 2);
    gGrid->setColumnStretch(0, 1);
    gGrid->setColumnStretch(1, 1);

    auto* summaryColumn = new QVBoxLayout;
    summaryColumn->setSpacing(12);
    summaryColumn->addLayout(cards);
    summaryColumn->addWidget(greeksBox);
    summaryColumn->addStretch(1);

    auto* chartsColumn = new QVBoxLayout;
    chartsColumn->setSpacing(12);
    buildCharts(chartsColumn);

    auto* lower = new QHBoxLayout;
    lower->setSpacing(16);
    lower->addLayout(summaryColumn, 2);
    lower->addLayout(chartsColumn, 3);

    auto* content = new QWidget;
    content->setObjectName("root");
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);
    root->addWidget(presetBox);
    root->addWidget(legsBox);
    root->addLayout(lower, 1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void StrategyTab::buildCharts(QVBoxLayout* column)
{
    // P&L chart
    m_payoffChart = new QChart;
    m_payoffChart->setTitle("P&L versus underlying price");
    m_payoffX = new QValueAxis;
    m_payoffX->setTitleText("Underlying price");
    m_payoffX->setLabelFormat("%.2f");
    m_payoffY = new QValueAxis;
    m_payoffY->setTitleText("P&L");
    m_payoffY->setLabelFormat("%.0f");
    m_payoffChart->addAxis(m_payoffX, Qt::AlignBottom);
    m_payoffChart->addAxis(m_payoffY, Qt::AlignLeft);
    m_payoffView = ui::makeChartView(this, m_payoffChart, 300);

    m_daysForward = new QSlider(Qt::Horizontal, this);
    m_daysForward->setRange(0, 1);
    m_daysForward->setValue(0);
    m_daysForward->setToolTip("Days forward for the intermediate P&L curve");
    m_daysLabel = new QLabel("Today", this);
    m_daysLabel->setObjectName("muted");
    m_daysLabel->setMinimumWidth(170);
    auto* sliderRow = new QHBoxLayout;
    sliderRow->addWidget(new QLabel("Time forward", this));
    sliderRow->addWidget(m_daysForward, 1);
    sliderRow->addWidget(m_daysLabel);

    auto* payoffBox = new QGroupBox("Payoff", this);
    auto* payoffLayout = new QVBoxLayout(payoffBox);
    payoffLayout->addWidget(m_payoffView, 1);
    payoffLayout->addLayout(sliderRow);
    column->addWidget(payoffBox, 3);

    // Greek chart
    m_greekChart = new QChart;
    m_greekX = new QValueAxis;
    m_greekX->setTitleText("Underlying price");
    m_greekX->setLabelFormat("%.2f");
    m_greekY = new QValueAxis;
    m_greekY->setLabelFormat("%.3g");
    m_greekChart->addAxis(m_greekX, Qt::AlignBottom);
    m_greekChart->addAxis(m_greekY, Qt::AlignLeft);
    m_greekView = ui::makeChartView(this, m_greekChart, 240);

    m_greekSelect = new QComboBox(this);
    m_greekSelect->addItems({ "Delta", "Gamma", "Vega", "Theta", "Rho", "Vanna", "Charm" });
    m_greekSelect->setToolTip("Greek to plot across the underlying price");
    auto* greekRow = new QHBoxLayout;
    greekRow->addWidget(new QLabel("Greek", this));
    greekRow->addWidget(m_greekSelect);
    greekRow->addStretch(1);

    auto* greekBox = new QGroupBox("Greeks across spot", this);
    auto* greekLayout = new QVBoxLayout(greekBox);
    greekLayout->addLayout(greekRow);
    greekLayout->addWidget(m_greekView, 1);
    column->addWidget(greekBox, 2);
}

void StrategyTab::wire()
{
    connect(m_presets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        const auto& presets = strategyPresets();
        if (index >= 0 && static_cast<size_t>(index) < presets.size()) {
            m_presetDescription->setText(presets[static_cast<size_t>(index)].description);
        }
    });
    m_presetDescription->setText(strategyPresets()[static_cast<size_t>(m_presets->currentIndex())].description);

    connect(m_loadPreset, &QPushButton::clicked, this, [this] { loadPreset(); });
    connect(m_multiplier, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_position.multiplier = value;
        recompute();
    });
    connect(m_addLeg, &QPushButton::clicked, this, [this] {
        Leg leg;
        leg.kind = LegKind::Call;
        leg.quantity = 1.0;
        leg.strike = roundToStrike(m_state.market.spot, m_strikeStep->value());
        leg.maturity = m_presetMaturity->value();
        leg.entryPrice = legValue(leg, valuationMarket(), m_state.market.spot, 0.0);
        addLeg(leg);
    });
    connect(m_removeLeg, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row >= 0 && static_cast<size_t>(row) < m_position.legs.size()) {
            m_position.legs.erase(m_position.legs.begin() + row);
            rebuildTable();
            recompute();
        }
    });
    connect(m_clearLegs, &QPushButton::clicked, this, [this] {
        m_position.legs.clear();
        rebuildTable();
        recompute();
    });
    connect(m_repriceEntries, &QPushButton::clicked, this, [this] { repriceEntries(); });
    connect(m_applySurface, &QPushButton::clicked, this, [this] { applySurfaceToLegs(); });
    connect(m_daysForward, &QSlider::valueChanged, this, [this](int) { updateCharts(); });
    connect(m_greekSelect, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { updateCharts(); });
}

// MARK: - Legs table

void StrategyTab::appendRow(const Leg& leg)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto* kind = new QComboBox(m_table);
    kind->addItem("Call", static_cast<int>(LegKind::Call));
    kind->addItem("Put", static_cast<int>(LegKind::Put));
    kind->addItem("Underlying", static_cast<int>(LegKind::Underlying));
    kind->setCurrentIndex(kind->findData(static_cast<int>(leg.kind)));
    kind->setFrame(false);

    auto* quantity = ui::makeSpinBox(m_table, -100000.0, 100000.0, 1.0, 0, leg.quantity);
    auto* strike = ui::makeSpinBox(m_table, 0.01, 1'000'000.0, 1.0, 2, leg.kind == LegKind::Underlying ? m_state.market.spot : leg.strike);
    auto* expiry = ui::makeSpinBox(m_table, 0.0001, 50.0, 0.25, 4, leg.kind == LegKind::Underlying ? 1.0 : leg.maturity, " yrs");
    auto* vol = ui::makeSpinBox(m_table, 0.0, 500.0, 1.0, 2, leg.volatility * 100.0, " %");
    auto* entry = ui::makeSpinBox(m_table, -1'000'000.0, 1'000'000.0, 0.05, 4, leg.entryPrice);
    for (QDoubleSpinBox* box : { quantity, strike, expiry, vol, entry }) {
        box->setFrame(false);
    }
    vol->setSpecialValueText("market");
    const bool isOption = leg.kind != LegKind::Underlying;
    strike->setEnabled(isOption);
    expiry->setEnabled(isOption);
    vol->setEnabled(isOption);

    m_table->setCellWidget(row, ColKind, kind);
    m_table->setCellWidget(row, ColQuantity, quantity);
    m_table->setCellWidget(row, ColStrike, strike);
    m_table->setCellWidget(row, ColExpiry, expiry);
    m_table->setCellWidget(row, ColVol, vol);
    m_table->setCellWidget(row, ColEntry, entry);
    m_table->setItem(row, ColModel, ui::makeCell("–"));
    m_table->setItem(row, ColUnitPnl, ui::makeCell("–"));
    m_table->setItem(row, ColPnl, ui::makeCell("–"));

    auto changed = [this] {
        if (m_updating) return;
        readTableIntoPosition();
        recompute();
    };
    connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, changed, strike, expiry, vol, kind](int) {
        const bool option = static_cast<LegKind>(kind->currentData().toInt()) != LegKind::Underlying;
        strike->setEnabled(option);
        expiry->setEnabled(option);
        vol->setEnabled(option);
        changed();
    });
    for (QDoubleSpinBox* box : { quantity, strike, expiry, vol, entry }) {
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    }
}

void StrategyTab::rebuildTable()
{
    m_updating = true;
    m_table->setRowCount(0);
    for (const Leg& leg : m_position.legs) {
        appendRow(leg);
    }
    m_updating = false;
}

void StrategyTab::readTableIntoPosition()
{
    std::vector<Leg> legs;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* kind = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColKind));
        auto* quantity = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColQuantity));
        auto* strike = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColStrike));
        auto* expiry = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColExpiry));
        auto* vol = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColVol));
        auto* entry = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColEntry));
        if (!kind || !quantity || !strike || !expiry || !vol || !entry) continue;
        Leg leg;
        leg.kind = static_cast<LegKind>(kind->currentData().toInt());
        leg.quantity = quantity->value();
        leg.strike = strike->value();
        leg.maturity = expiry->value();
        leg.volatility = vol->value() / 100.0;
        leg.entryPrice = entry->value();
        legs.push_back(leg);
    }
    m_position.legs = legs;
}

void StrategyTab::addLeg(const Leg& leg)
{
    m_position.legs.push_back(leg);
    m_updating = true;
    appendRow(leg);
    m_updating = false;
    recompute();
}

void StrategyTab::setPosition(const Position& position)
{
    m_position = position;
    m_updating = true;
    m_multiplier->setValue(position.multiplier);
    m_updating = false;
    rebuildTable();
    recompute();
}

void StrategyTab::loadPreset()
{
    const auto preset = static_cast<StrategyPreset>(m_presets->currentData().toInt());
    Position pos = buildPreset(preset, valuationMarket(), m_presetMaturity->value(), m_strikeStep->value());
    pos.multiplier = m_multiplier->value();
    setPosition(pos);
}

void StrategyTab::repriceEntries()
{
    const Market market = valuationMarket();
    for (Leg& leg : m_position.legs) {
        leg.entryPrice = legValue(leg, market, market.spot, 0.0);
    }
    rebuildTable();
    recompute();
}

void StrategyTab::applySurfaceToLegs()
{
    if (m_state.surface.empty()) return;
    for (Leg& leg : m_position.legs) {
        if (leg.kind == LegKind::Underlying) continue;
        const double vol = m_state.surface.impliedVol(leg.strike, leg.maturity, m_state.chainMarket(leg.maturity));
        if (vol > 0.0) leg.volatility = vol;
    }
    rebuildTable();
    recompute();
}

Market StrategyTab::valuationMarket() const
{
    const double horizon = latestExpiry(m_position);
    return m_state.marketFor(horizon > 0.0 ? horizon : 1.0);
}

// MARK: - Analysis

void StrategyTab::recompute()
{
    const Market market = valuationMarket();
    m_analysis = analyzePosition(m_position, market);
    m_applySurface->setEnabled(!m_state.surface.empty());

    // Per-leg model prices and P&L.
    for (int row = 0; row < m_table->rowCount() && static_cast<size_t>(row) < m_position.legs.size(); ++row) {
        const Leg& leg = m_position.legs[static_cast<size_t>(row)];
        const double model = legValue(leg, market, market.spot, 0.0);
        const double unitPnl = model - leg.entryPrice;
        const double pnl = unitPnl * leg.quantity * m_position.multiplier;
        m_table->item(row, ColModel)->setText(number(model));
        m_table->item(row, ColUnitPnl)->setText(number(unitPnl));
        m_table->item(row, ColPnl)->setText(signedMoney(pnl));
        m_table->item(row, ColPnl)->setForeground(QBrush(QColor(pnl >= 0 ? m_theme.profit : m_theme.loss)));
    }

    const int days = std::max(1, static_cast<int>(std::floor(m_analysis.horizon * market.dayBasis)));
    {
        const QSignalBlocker blocker(m_daysForward);
        m_daysForward->setRange(0, days);
        if (m_daysForward->value() > days) m_daysForward->setValue(days);
    }

    updateSummary();
    updateCharts();
    if (onPositionChanged) onPositionChanged();
}

void StrategyTab::updateSummary()
{
    const StrategyAnalysis& a = m_analysis;
    const bool empty = m_position.legs.empty();

    auto colourise = [&](QLabel* label, double value) {
        label->setObjectName(value > 0 ? "profitValue" : (value < 0 ? "lossValue" : "bigValue"));
        ui::restyle(label);
    };

    if (empty) {
        for (ui::Card* card : { &m_netPremium, &m_maxProfit, &m_maxLoss, &m_breakevens, &m_probabilityOfProfit, &m_expectedPnl }) {
            card->value->setText("–");
            card->subtitle->clear();
        }
        for (QLabel* l : { m_gDelta, m_gGamma, m_gVega, m_gTheta, m_gRho, m_gVanna, m_gCharm }) l->setText("–");
        m_summaryNote->setText("Add legs or load a preset to analyse a position.");
        return;
    }

    m_netPremium.value->setText(signedMoney(-a.netPremium));
    m_netPremium.subtitle->setText(a.netPremium > 0 ? "debit paid" : (a.netPremium < 0 ? "credit received" : "zero cost"));
    colourise(m_netPremium.value, -a.netPremium);

    m_maxProfit.value->setText(a.unboundedProfit ? "Unlimited" : signedMoney(a.maxProfit));
    m_maxProfit.subtitle->setText(QStringLiteral("at first expiry (%1 days)").arg(static_cast<int>(std::lround(a.horizon * m_state.market.dayBasis))));
    colourise(m_maxProfit.value, a.unboundedProfit ? 1.0 : a.maxProfit);

    m_maxLoss.value->setText(a.unboundedLoss ? "Unlimited" : signedMoney(a.maxLoss));
    if (!a.unboundedLoss && !a.unboundedProfit && a.maxLoss < 0 && a.maxProfit > 0) {
        m_maxLoss.subtitle->setText(QStringLiteral("reward / risk %1").arg(number(a.maxProfit / -a.maxLoss, 2)));
    } else {
        m_maxLoss.subtitle->clear();
    }
    colourise(m_maxLoss.value, a.unboundedLoss ? -1.0 : a.maxLoss);

    if (a.breakevens.empty()) {
        m_breakevens.value->setText("none");
        m_breakevens.subtitle->setText(a.pnlAtHorizon.empty() || a.pnlAtHorizon.front() >= 0 ? "always profitable at expiry" : "never profitable at expiry");
    } else {
        QStringList parts;
        for (double b : a.breakevens) parts << number(b, 2);
        m_breakevens.value->setText(parts.join("  /  "));
        m_breakevens.subtitle->setText(QStringLiteral("%1 crossing%2").arg(a.breakevens.size()).arg(a.breakevens.size() == 1 ? "" : "s"));
    }
    m_breakevens.value->setObjectName("bigValue");
    ui::restyle(m_breakevens.value);

    m_probabilityOfProfit.value->setText(percent(a.probabilityOfProfit, 1));
    m_probabilityOfProfit.subtitle->setText("risk-neutral, at first expiry");
    m_probabilityOfProfit.value->setObjectName("bigValue");
    ui::restyle(m_probabilityOfProfit.value);

    m_expectedPnl.value->setText(signedMoney(a.expectedPnl));
    m_expectedPnl.subtitle->setText("risk-neutral expectation");
    colourise(m_expectedPnl.value, a.expectedPnl);

    m_gDelta->setText(number(a.greeks.delta, 2));
    m_gGamma->setText(number(a.greeks.gamma, 4));
    m_gVega->setText(number(a.greeks.vega, 2));
    m_gTheta->setText(number(a.greeks.theta, 2));
    m_gRho->setText(number(a.greeks.rho, 2));
    m_gVanna->setText(number(a.greeks.vanna, 2));
    m_gCharm->setText(number(a.greeks.charm, 4));

    const double multiplier = m_position.multiplier;
    m_summaryNote->setText(QStringLiteral("Greeks are for the whole position (contract multiplier %1). Delta of %2 means the position behaves like %3 units of the underlying.")
                               .arg(number(multiplier, 0), number(a.greeks.delta, 1), number(a.greeks.delta, 0)));
}

void StrategyTab::updateCharts()
{
    const Market market = valuationMarket();
    const StrategyAnalysis& a = m_analysis;

    m_payoffChart->removeAllSeries();
    m_greekChart->removeAllSeries();
    if (a.spots.empty()) {
        m_payoffX->setRange(0, 1);
        m_payoffY->setRange(-1, 1);
        m_greekX->setRange(0, 1);
        m_greekY->setRange(-1, 1);
        m_daysLabel->setText("Today");
        return;
    }

    const int daysForward = m_daysForward->value();
    const double elapsed = daysForward / market.dayBasis;
    m_daysLabel->setText(daysForward == 0 ? "Today" : QStringLiteral("In %1 day%2 (%3 to expiry)")
                                                           .arg(daysForward).arg(daysForward == 1 ? "" : "s")
                                                           .arg(std::max(0, m_daysForward->maximum() - daysForward)));

    auto* expirySeries = new QLineSeries;
    expirySeries->setName("At first expiry");
    auto* todaySeries = new QLineSeries;
    todaySeries->setName("Today");
    auto* forwardSeries = new QLineSeries;
    forwardSeries->setName(m_daysLabel->text());

    double yMin = 0.0, yMax = 0.0;
    for (size_t i = 0; i < a.spots.size(); ++i) {
        const double s = a.spots[i];
        const double atExpiry = a.pnlAtHorizon[i];
        const double today = a.pnlToday[i];
        const double forward = positionPnl(m_position, market, s, std::min(elapsed, a.horizon));
        expirySeries->append(s, atExpiry);
        todaySeries->append(s, today);
        forwardSeries->append(s, forward);
        for (double v : { atExpiry, today, forward }) {
            if (std::isfinite(v)) { yMin = std::min(yMin, v); yMax = std::max(yMax, v); }
        }
    }
    const double pad = std::max((yMax - yMin) * 0.08, 1.0);
    yMin -= pad;
    yMax += pad;

    auto* zero = new QLineSeries;
    zero->append(a.spots.front(), 0.0);
    zero->append(a.spots.back(), 0.0);
    zero->setPen(QPen(QColor(m_theme.textMuted), 1.0, Qt::DashLine));
    auto* spotMarker = new QLineSeries;
    spotMarker->append(market.spot, yMin);
    spotMarker->append(market.spot, yMax);
    spotMarker->setPen(QPen(QColor(m_theme.accent), 1.0, Qt::DotLine));

    expirySeries->setPen(QPen(QColor(m_theme.profit), 2.5));
    todaySeries->setPen(QPen(QColor(m_theme.accent), 1.8, Qt::DashLine));
    forwardSeries->setPen(QPen(QColor(m_theme.warning), 1.8));

    for (QLineSeries* s : { zero, spotMarker, todaySeries, forwardSeries, expirySeries }) {
        m_payoffChart->addSeries(s);
        s->attachAxis(m_payoffX);
        s->attachAxis(m_payoffY);
    }
    // Hide the helper lines from the legend.
    for (QLegendMarker* marker : m_payoffChart->legend()->markers()) {
        if (marker->series() == zero || marker->series() == spotMarker) marker->setVisible(false);
    }
    if (daysForward == 0) {
        for (QLegendMarker* marker : m_payoffChart->legend()->markers(forwardSeries)) marker->setVisible(false);
        forwardSeries->setVisible(false);
    }
    m_payoffX->setRange(a.spots.front(), a.spots.back());
    m_payoffY->setRange(yMin, yMax);
    m_payoffY->applyNiceNumbers();

    // Greek across spot
    const int greekIndex = m_greekSelect->currentIndex();
    auto* greekToday = new QLineSeries;
    greekToday->setName(QStringLiteral("%1 today").arg(m_greekSelect->currentText()));
    greekToday->setPen(QPen(QColor(m_theme.accent), 2.0));
    auto* greekForward = new QLineSeries;
    greekForward->setName(QStringLiteral("%1 %2").arg(m_greekSelect->currentText(), m_daysLabel->text().toLower()));
    greekForward->setPen(QPen(QColor(m_theme.warning), 1.8, Qt::DashLine));
    double gMin = INFINITY, gMax = -INFINITY;
    const size_t stride = std::max<size_t>(1, a.spots.size() / 200);
    for (size_t i = 0; i < a.spots.size(); i += stride) {
        const double s = a.spots[i];
        const double today = greekValue(positionGreeks(m_position, market, s, 0.0), greekIndex);
        greekToday->append(s, today);
        gMin = std::min(gMin, today); gMax = std::max(gMax, today);
        if (daysForward > 0) {
            const double forward = greekValue(positionGreeks(m_position, market, s, std::min(elapsed, a.horizon * 0.999)), greekIndex);
            greekForward->append(s, forward);
            gMin = std::min(gMin, forward); gMax = std::max(gMax, forward);
        }
    }
    if (!std::isfinite(gMin)) { gMin = -1; gMax = 1; }
    const double gPad = std::max((gMax - gMin) * 0.1, 1e-6);
    auto* greekZero = new QLineSeries;
    greekZero->append(a.spots.front(), 0.0);
    greekZero->append(a.spots.back(), 0.0);
    greekZero->setPen(QPen(QColor(m_theme.textMuted), 1.0, Qt::DashLine));
    auto* greekSpot = new QLineSeries;
    greekSpot->append(market.spot, gMin - gPad);
    greekSpot->append(market.spot, gMax + gPad);
    greekSpot->setPen(QPen(QColor(m_theme.accent), 1.0, Qt::DotLine));
    for (QLineSeries* s : { greekZero, greekSpot, greekToday, greekForward }) {
        m_greekChart->addSeries(s);
        s->attachAxis(m_greekX);
        s->attachAxis(m_greekY);
    }
    for (QLegendMarker* marker : m_greekChart->legend()->markers()) {
        if (marker->series() == greekZero || marker->series() == greekSpot) marker->setVisible(false);
        if (daysForward == 0 && marker->series() == greekForward) marker->setVisible(false);
    }
    if (daysForward == 0) greekForward->setVisible(false);
    m_greekX->setRange(a.spots.front(), a.spots.back());
    m_greekY->setRange(gMin - gPad, gMax + gPad);
    m_greekY->setTitleText(m_greekSelect->currentText());
    m_greekChart->setTitle(QStringLiteral("Position %1 versus underlying price").arg(m_greekSelect->currentText().toLower()));

    styleChart(m_payoffChart, m_theme);
    styleChart(m_greekChart, m_theme);
}

// MARK: - Theme, persistence, export

void StrategyTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    styleChart(m_payoffChart, theme);
    styleChart(m_greekChart, theme);
    recompute();
}

QJsonObject StrategyTab::toJson() const
{
    QJsonObject o;
    o["multiplier"] = m_position.multiplier;
    o["presetMaturity"] = m_presetMaturity->value();
    o["strikeStep"] = m_strikeStep->value();
    QJsonArray legs;
    for (const Leg& leg : m_position.legs) {
        QJsonObject l;
        l["kind"] = legKindName(leg.kind).toLower();
        l["quantity"] = leg.quantity;
        l["strike"] = leg.strike;
        l["maturity"] = leg.maturity;
        l["volatility"] = leg.volatility;
        l["entryPrice"] = leg.entryPrice;
        legs.append(l);
    }
    o["legs"] = legs;
    return o;
}

void StrategyTab::fromJson(const QJsonObject& o)
{
    Position pos;
    pos.multiplier = o["multiplier"].toDouble(100.0);
    const QJsonArray legs = o["legs"].toArray();
    for (const QJsonValue v : legs) {
        const QJsonObject l = v.toObject();
        Leg leg;
        const QString kind = l["kind"].toString();
        leg.kind = kind == "put" ? LegKind::Put : (kind == "underlying" ? LegKind::Underlying : LegKind::Call);
        leg.quantity = l["quantity"].toDouble(1.0);
        leg.strike = l["strike"].toDouble(100.0);
        leg.maturity = l["maturity"].toDouble(1.0);
        leg.volatility = l["volatility"].toDouble(0.0);
        leg.entryPrice = l["entryPrice"].toDouble(0.0);
        pos.legs.push_back(leg);
    }
    m_updating = true;
    m_presetMaturity->setValue(o["presetMaturity"].toDouble(0.5));
    m_strikeStep->setValue(o["strikeStep"].toDouble(5.0));
    m_updating = false;
    setPosition(pos);
}

QString StrategyTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    const Market market = valuationMarket();
    s << "kind,quantity,strike,maturity_years,volatility,entry_price,model_price,unit_pnl,position_pnl\n";
    for (const Leg& leg : m_position.legs) {
        const double model = legValue(leg, market, market.spot, 0.0);
        s << legKindName(leg.kind) << "," << leg.quantity << "," << leg.strike << "," << leg.maturity << ","
          << (leg.volatility > 0 ? leg.volatility : market.volatility) << "," << leg.entryPrice << "," << model << ","
          << (model - leg.entryPrice) << "," << (model - leg.entryPrice) * leg.quantity * m_position.multiplier << "\n";
    }
    s << "\nmetric,value\n";
    s << "net_premium," << m_analysis.netPremium << "\n";
    s << "max_profit," << (m_analysis.unboundedProfit ? QStringLiteral("unlimited") : QString::number(m_analysis.maxProfit)) << "\n";
    s << "max_loss," << (m_analysis.unboundedLoss ? QStringLiteral("unlimited") : QString::number(m_analysis.maxLoss)) << "\n";
    s << "probability_of_profit," << m_analysis.probabilityOfProfit << "\n";
    s << "expected_pnl," << m_analysis.expectedPnl << "\n";
    QStringList be;
    for (double b : m_analysis.breakevens) be << QString::number(b);
    s << "breakevens," << be.join(";") << "\n";
    s << "delta," << m_analysis.greeks.delta << "\ngamma," << m_analysis.greeks.gamma << "\nvega," << m_analysis.greeks.vega
      << "\ntheta," << m_analysis.greeks.theta << "\nrho," << m_analysis.greeks.rho << "\n";
    s << "\nspot,pnl_at_expiry,pnl_today\n";
    for (size_t i = 0; i < m_analysis.spots.size(); i += 10) {
        s << m_analysis.spots[i] << "," << m_analysis.pnlAtHorizon[i] << "," << m_analysis.pnlToday[i] << "\n";
    }
    return out;
}
