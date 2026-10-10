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
    m_state.subscribe([this] {
        refreshChainIndex();
        recompute();
    });
    refreshChainIndex();
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
    // The preset's description is the combo's tooltip (and in the guide); it is not shown as text.
    m_presetDescription = new QLabel(this);
    m_presetDescription->setVisible(false);

    m_presetMaturity = ui::makeSpinBox(this, 0.01, 10.0, 0.25, 2, 0.5, " yrs");
    m_presetMaturity->setToolTip("Expiry used for preset legs (calendars use twice this for the far leg)");
    m_presetExpiry = new QComboBox(this);
    m_presetExpiry->setToolTip("Listed expiry from the loaded option chain used for preset legs (calendars take the next suitable listed expiry for the far leg)");
    m_presetExpiry->setVisible(false);
    m_strikeStep = ui::makeSpinBox(this, 0.01, 10000.0, 1.0, 2, 5.0);
    m_strikeStep->setToolTip("Strike interval used to place preset wings when no option chain is loaded");
    // One short line on the chain driving the presets; the explanation is its tooltip.
    m_chainInfo = new QLabel(this);
    m_chainInfo->setObjectName("muted");
    m_chainInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_multiplier = ui::makeSpinBox(this, 1.0, 100000.0, 1.0, 0, 100.0);
    m_multiplier->setToolTip("Contract multiplier (100 for US equity options)");
    m_loadPreset = ui::makeButton(this, "Load Preset", "primary", "Replace the legs with the selected preset");

    auto* presetBox = new QGroupBox("Presets", this);
    auto* presetForm = new QGridLayout(presetBox);
    presetForm->setHorizontalSpacing(12);
    presetForm->setVerticalSpacing(8);
    m_presetExpiryLabel = new QLabel("Expiry", presetBox);
    m_strikeStepLabel = new QLabel("Strike step", presetBox);
    auto* expiryStack = new QHBoxLayout;
    expiryStack->setContentsMargins(0, 0, 0, 0);
    expiryStack->addWidget(m_presetMaturity);
    expiryStack->addWidget(m_presetExpiry);
    presetForm->addWidget(new QLabel("Strategy", presetBox), 0, 0);
    presetForm->addWidget(m_presets, 0, 1);
    presetForm->addWidget(m_presetExpiryLabel, 0, 2);
    presetForm->addLayout(expiryStack, 0, 3);
    presetForm->addWidget(m_strikeStepLabel, 0, 4);
    presetForm->addWidget(m_strikeStep, 0, 5);
    presetForm->addWidget(new QLabel("Multiplier", presetBox), 0, 6);
    presetForm->addWidget(m_multiplier, 0, 7);
    presetForm->addWidget(m_loadPreset, 0, 8);
    presetForm->addWidget(m_chainInfo, 1, 0, 1, 9);
    presetForm->setColumnStretch(1, 2);
    presetForm->setColumnStretch(3, 1);

    // Legs table
    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Type", "Qty", "Strike", "Expiry", "IV %", "Entry price", "Mkt mid", "Model price", "Unit P&L", "Position P&L" });
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
    gGrid->setRowStretch(row + 1, 1);   // spare height (box stretched to the payoff chart) stays below the rows
    gGrid->setColumnStretch(0, 1);
    gGrid->setColumnStretch(1, 1);

    // Lower area: the summary cards and Net Greeks on the left, the payoff chart on the
    // right, the Greek chart full width underneath. The Net Greeks box takes the left
    // column's spare height, so its bottom edge meets the payoff box's bottom edge.
    auto* summaryColumn = new QVBoxLayout;
    summaryColumn->setSpacing(12);
    summaryColumn->addLayout(cards);
    summaryColumn->addWidget(greeksBox, 1);

    auto* chartsColumn = new QVBoxLayout;
    chartsColumn->setSpacing(12);
    buildCharts(chartsColumn);
    chartsColumn->removeWidget(m_greekBox);

    auto* lower = new QHBoxLayout;
    lower->setSpacing(12);
    lower->addLayout(summaryColumn, 2);
    lower->addLayout(chartsColumn, 3);

    auto* content = new QWidget;
    content->setObjectName("root");
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(12, 8, 12, 10);
    root->setSpacing(8);
    root->addWidget(presetBox);
    root->addWidget(legsBox);
    root->addLayout(lower, 1);
    root->addWidget(m_greekBox, 1);

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
    m_greekSelect->setToolTip("Greek to plot");
    m_greekMode = new QComboBox(this);
    m_greekMode->addItem("across spot", "spot");
    m_greekMode->addItem("over time", "time");
    m_greekMode->setToolTip("Across spot: the Greek against the underlying price today and at the slider's date. Over time: the Greek day by day to the first expiry, at spot and at ±5% / ±10% moves");
    auto* greekRow = new QHBoxLayout;
    greekRow->addWidget(new QLabel("Greek", this));
    greekRow->addWidget(m_greekSelect);
    greekRow->addWidget(m_greekMode);
    greekRow->addStretch(1);

    m_greekBox = new QGroupBox("Greeks across spot", this);
    auto* greekLayout = new QVBoxLayout(m_greekBox);
    greekLayout->addLayout(greekRow);
    greekLayout->addWidget(m_greekView, 1);
    column->addWidget(m_greekBox, 2);
}

void StrategyTab::wire()
{
    connect(m_presets, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        const auto& presets = strategyPresets();
        if (index >= 0 && static_cast<size_t>(index) < presets.size()) {
            m_presetDescription->setText(presets[static_cast<size_t>(index)].description);
            m_presets->setToolTip(m_presetDescription->text());
        }
    });
    m_presetDescription->setText(strategyPresets()[static_cast<size_t>(m_presets->currentIndex())].description);
    m_presets->setToolTip(m_presetDescription->text());

    connect(m_loadPreset, &QPushButton::clicked, this, [this] { loadPreset(); });
    connect(m_multiplier, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_position.multiplier = value;
        recompute();
    });
    connect(m_addLeg, &QPushButton::clicked, this, [this] {
        Leg leg;
        leg.kind = LegKind::Call;
        leg.quantity = 1.0;
        if (chainMode()) {
            leg.maturity = presetMaturity();
            leg.strike = m_chain.nearestStrike(leg.maturity, m_state.market.spot);
            markLegToChain(leg, m_chain, activityMarket());
            leg.entryPrice = leg.marketPrice > 0.0 ? leg.marketPrice : legValue(leg, valuationMarket(), m_state.market.spot, 0.0);
        } else {
            leg.strike = roundToStrike(m_state.market.spot, m_strikeStep->value());
            leg.maturity = m_presetMaturity->value();
            leg.entryPrice = legValue(leg, valuationMarket(), m_state.market.spot, 0.0);
        }
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
    connect(m_greekMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { updateCharts(); });
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
    auto* vol = ui::makeSpinBox(m_table, 0.0, 500.0, 1.0, 2, leg.volatility * 100.0, " %");
    auto* entry = ui::makeSpinBox(m_table, -1'000'000.0, 1'000'000.0, 0.05, 4, leg.entryPrice);
    for (QDoubleSpinBox* box : { quantity, vol, entry }) {
        box->setFrame(false);
    }
    vol->setSpecialValueText("market");
    vol->setToolTip(chainMode() ? "Implied vol from the chain mid; edit to override" : "Per-leg implied vol; 0 uses the market volatility");
    const bool isOption = leg.kind != LegKind::Underlying;
    vol->setEnabled(isOption);

    m_table->setCellWidget(row, ColKind, kind);
    m_table->setCellWidget(row, ColQuantity, quantity);
    m_table->setCellWidget(row, ColVol, vol);
    m_table->setCellWidget(row, ColEntry, entry);
    m_table->setItem(row, ColMarket, ui::makeCell("–"));
    m_table->setItem(row, ColModel, ui::makeCell("–"));
    m_table->setItem(row, ColUnitPnl, ui::makeCell("–"));
    m_table->setItem(row, ColPnl, ui::makeCell("–"));

    auto changed = [this] {
        if (m_updating) return;
        readTableIntoPosition();
        recompute();
    };

    if (chainMode()) {
        // Strike and expiry are chosen from the listed contracts of the loaded chain.
        auto* expiry = new QComboBox(m_table);
        expiry->setFrame(false);
        for (const ListedExpiry* e : m_chain.expiries()) {
            const QString date = e->key.expiryDate.empty() ? QStringLiteral("T %1y").arg(number(e->key.maturity, 3)) : QString::fromStdString(e->key.expiryDate);
            expiry->addItem(QStringLiteral("%1 · %2d").arg(date).arg(e->key.daysToExpiry), e->key.maturity);
        }
        int expiryIndex = -1;
        for (int i = 0; i < expiry->count(); ++i) {
            if (std::fabs(expiry->itemData(i).toDouble() - leg.maturity) < 1e-6) { expiryIndex = i; break; }
        }
        if (expiryIndex < 0 && isOption) {
            // Not listed (e.g. loaded from a workspace): keep it as an extra entry.
            expiry->addItem(QStringLiteral("%1 yrs (unlisted)").arg(number(leg.maturity, 3)), leg.maturity);
            expiryIndex = expiry->count() - 1;
        }
        expiry->setCurrentIndex(std::max(0, expiryIndex));
        auto* strike = new QComboBox(m_table);
        strike->setFrame(false);
        populateStrikeCombo(strike, expiry->currentData().toDouble(), leg.strike);
        expiry->setEnabled(isOption);
        strike->setEnabled(isOption);
        m_table->setCellWidget(row, ColStrike, strike);
        m_table->setCellWidget(row, ColExpiry, expiry);

        auto contractChanged = [this, kind, expiry, strike, row] {
            if (m_updating) return;
            const bool option = static_cast<LegKind>(kind->currentData().toInt()) != LegKind::Underlying;
            expiry->setEnabled(option);
            strike->setEnabled(option);
            readTableIntoPosition();
            updateLegFromChain(row);   // new contract: take its mid as entry and its IV
            recompute();
        };
        connect(expiry, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, expiry, strike, contractChanged](int) {
            if (m_updating) return;
            const double keep = strike->currentData().toDouble();
            m_updating = true;
            populateStrikeCombo(strike, expiry->currentData().toDouble(), keep);
            m_updating = false;
            contractChanged();
        });
        connect(strike, qOverload<int>(&QComboBox::currentIndexChanged), this, [contractChanged](int) { contractChanged(); });
        connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), this, [contractChanged, vol, kind](int) {
            vol->setEnabled(static_cast<LegKind>(kind->currentData().toInt()) != LegKind::Underlying);
            contractChanged();
        });
    } else {
        auto* strike = ui::makeSpinBox(m_table, 0.01, 1'000'000.0, 1.0, 2, leg.kind == LegKind::Underlying ? m_state.market.spot : leg.strike);
        auto* expiry = ui::makeSpinBox(m_table, 0.0001, 50.0, 0.25, 4, leg.kind == LegKind::Underlying ? 1.0 : leg.maturity, " yrs");
        strike->setFrame(false);
        expiry->setFrame(false);
        strike->setEnabled(isOption);
        expiry->setEnabled(isOption);
        m_table->setCellWidget(row, ColStrike, strike);
        m_table->setCellWidget(row, ColExpiry, expiry);
        connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed, strike, expiry, vol, kind](int) {
            const bool option = static_cast<LegKind>(kind->currentData().toInt()) != LegKind::Underlying;
            strike->setEnabled(option);
            expiry->setEnabled(option);
            vol->setEnabled(option);
            changed();
        });
        for (QDoubleSpinBox* box : { strike, expiry }) {
            connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
        }
    }
    for (QDoubleSpinBox* box : { quantity, vol, entry }) {
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    }
}

void StrategyTab::populateStrikeCombo(QComboBox* strikes, double maturity, double selected) const
{
    strikes->clear();
    const ListedExpiry* e = m_chain.expiry(maturity);
    int selectedIndex = -1;
    if (e) {
        for (double k : e->strikes) {
            strikes->addItem(number(k, 2), k);
            if (std::fabs(k - selected) < 1e-9) selectedIndex = strikes->count() - 1;
        }
    }
    if (selectedIndex < 0 && selected > 0.0) {
        // Keep an unlisted strike (workspace legs, or a strike absent from this expiry) visible.
        strikes->addItem(QStringLiteral("%1 (unlisted)").arg(number(selected, 2)), selected);
        selectedIndex = strikes->count() - 1;
    }
    if (selectedIndex < 0 && strikes->count() > 0) {
        // Default to the listed strike nearest spot.
        const double atm = m_chain.nearestStrike(maturity, m_state.market.spot);
        for (int i = 0; i < strikes->count(); ++i) {
            if (std::fabs(strikes->itemData(i).toDouble() - atm) < 1e-9) selectedIndex = i;
        }
    }
    strikes->setCurrentIndex(std::max(0, selectedIndex));
}

void StrategyTab::updateLegFromChain(int row)
{
    if (!chainMode() || row < 0 || static_cast<size_t>(row) >= m_position.legs.size()) return;
    Leg& leg = m_position.legs[static_cast<size_t>(row)];
    if (markLegToChain(leg, m_chain, activityMarket()) && leg.marketPrice > 0.0) {
        leg.entryPrice = leg.marketPrice;
    }
    m_updating = true;
    if (auto* entry = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColEntry))) entry->setValue(leg.entryPrice);
    if (auto* vol = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColVol))) vol->setValue(leg.volatility * 100.0);
    m_updating = false;
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
        auto* vol = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColVol));
        auto* entry = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColEntry));
        if (!kind || !quantity || !vol || !entry) continue;
        Leg leg;
        if (static_cast<size_t>(row) < m_position.legs.size()) {
            leg = m_position.legs[static_cast<size_t>(row)];   // keep market mark and expiry date
        }
        leg.kind = static_cast<LegKind>(kind->currentData().toInt());
        leg.quantity = quantity->value();
        if (auto* strikeBox = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColStrike))) {
            leg.strike = strikeBox->value();
        } else if (auto* strikeCombo = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColStrike))) {
            leg.strike = strikeCombo->currentData().toDouble();
        }
        if (auto* expiryBox = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColExpiry))) {
            leg.maturity = expiryBox->value();
        } else if (auto* expiryCombo = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColExpiry))) {
            leg.maturity = expiryCombo->currentData().toDouble();
        }
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

bool StrategyTab::selectPreset(const QString& name)
{
    const QString wanted = name.trimmed();
    for (int i = 0; i < m_presets->count(); ++i) {
        if (m_presets->itemText(i).compare(wanted, Qt::CaseInsensitive) == 0 || m_presets->itemText(i).contains(wanted, Qt::CaseInsensitive)) {
            m_presets->setCurrentIndex(i);
            loadPreset();
            return true;
        }
    }
    return false;
}

QStringList StrategyTab::presetNames() const
{
    QStringList out;
    for (int i = 0; i < m_presets->count(); ++i) out << m_presets->itemText(i);
    return out;
}

void StrategyTab::loadPreset()
{
    const auto preset = static_cast<StrategyPreset>(m_presets->currentData().toInt());
    Position pos;
    if (chainMode()) {
        pos = buildPresetFromChain(preset, m_chain, activityMarket(), presetMaturity(), m_multiplier->value());
    } else {
        pos = buildPreset(preset, valuationMarket(), m_presetMaturity->value(), m_strikeStep->value());
        pos.multiplier = m_multiplier->value();
    }
    setPosition(pos);
}

void StrategyTab::repriceEntries()
{
    const Market market = valuationMarket();
    for (Leg& leg : m_position.legs) {
        if (chainMode() && markLegToChain(leg, m_chain, activityMarket()) && leg.marketPrice > 0.0) {
            leg.entryPrice = leg.marketPrice;
        } else {
            leg.entryPrice = legValue(leg, market, market.spot, 0.0);
        }
    }
    rebuildTable();
    recompute();
}

double StrategyTab::presetMaturity() const
{
    if (chainMode() && m_presetExpiry->count() > 0) {
        return m_presetExpiry->currentData().toDouble();
    }
    return m_presetMaturity->value();
}

ActivityMarket StrategyTab::activityMarket() const
{
    ActivityMarket m;
    m.model = m_state.market.model;
    m.spot = m_state.market.spot;
    m.dividendYield = m_state.market.dividendYield;
    m.rateFor = [this](double maturity) { return m_state.rateFor(maturity); };
    return m;
}

void StrategyTab::refreshChainIndex()
{
    const bool wasChain = chainMode();
    m_chain.build(m_state.chainQuotes);
    m_chainTicker = m_state.underlyingTicker;

    const bool chain = chainMode();
    m_presetMaturity->setVisible(!chain);
    m_presetExpiry->setVisible(chain);
    m_strikeStep->setVisible(!chain);
    m_strikeStepLabel->setVisible(!chain);
    m_repriceEntries->setText(chain ? "Reprice Entries at Market" : "Reprice Entries at Model");
    m_repriceEntries->setToolTip(chain ? "Set each entry price to the current chain mid" : "Set each entry price to the current model price");

    if (chain) {
        populateExpiryCombo();
        const auto expiries = m_chain.expiries();
        // Dated expiries show their range; a synthetic or CSV chain without dates shows only the count.
        const QString first = expiries.empty() ? QString() : QString::fromStdString(expiries.front()->key.expiryDate);
        const QString last = expiries.empty() ? QString() : QString::fromStdString(expiries.back()->key.expiryDate);
        const QString range = first.isEmpty() ? QString() : QStringLiteral(" (%1 – %2)").arg(first, last);
        m_chainInfo->setText(QStringLiteral("%1 chain · %2 expir%3%4 · spot %5%6")
                                 .arg(m_chainTicker.isEmpty() ? QStringLiteral("Loaded") : m_chainTicker)
                                 .arg(expiries.size()).arg(expiries.size() == 1 ? "y" : "ies", range,
                                      number(m_state.market.spot, 2),
                                      m_state.spotSource.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_state.spotSource)));
        m_chainInfo->setToolTip("Presets are built from this option chain: strikes, expiries, entry prices and implied vols come from the listed contracts.");
        // Keep every leg marked to the latest chain (mids and implied vols).
        markPositionToChain(m_position, m_chain, activityMarket());
    } else {
        m_chainInfo->setText("No option chain · model prices at the market volatility");
        m_chainInfo->setToolTip("Fetch a chain on the Option Chain tab to build presets from listed contracts with their mids and implied vols.");
        for (Leg& leg : m_position.legs) leg.marketPrice = 0.0;
    }
    if (wasChain != chain) {
        rebuildTable();   // editors switch between free entry and listed-contract pickers
    } else if (chain) {
        // Refresh the visible IV cells with the latest marks without disturbing the editors.
        m_updating = true;
        for (int row = 0; row < m_table->rowCount() && static_cast<size_t>(row) < m_position.legs.size(); ++row) {
            if (auto* vol = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, ColVol))) {
                vol->setValue(m_position.legs[static_cast<size_t>(row)].volatility * 100.0);
            }
        }
        m_updating = false;
    }
}

void StrategyTab::populateExpiryCombo()
{
    const double previous = m_presetExpiry->count() > 0 ? m_presetExpiry->currentData().toDouble() : -1.0;
    const QSignalBlocker blocker(m_presetExpiry);
    m_presetExpiry->clear();
    int select = -1;
    const ListedExpiry* preferred = m_chain.firstExpiryAtLeast(25);
    for (const ListedExpiry* e : m_chain.expiries()) {
        const QString date = e->key.expiryDate.empty() ? QStringLiteral("T %1y").arg(number(e->key.maturity, 3)) : QString::fromStdString(e->key.expiryDate);
        m_presetExpiry->addItem(QStringLiteral("%1 · %2 DTE").arg(date).arg(e->key.daysToExpiry), e->key.maturity);
        if (previous > 0.0 && std::fabs(e->key.maturity - previous) < 1e-6) select = m_presetExpiry->count() - 1;
    }
    if (select < 0 && preferred) {
        for (int i = 0; i < m_presetExpiry->count(); ++i) {
            if (std::fabs(m_presetExpiry->itemData(i).toDouble() - preferred->key.maturity) < 1e-9) select = i;
        }
    }
    m_presetExpiry->setCurrentIndex(std::max(0, select));
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

    // Per-leg prices and P&L. With a chain loaded the P&L marks to the chain mid; otherwise to model.
    for (int row = 0; row < m_table->rowCount() && static_cast<size_t>(row) < m_position.legs.size(); ++row) {
        const Leg& leg = m_position.legs[static_cast<size_t>(row)];
        const double model = legValue(leg, market, market.spot, 0.0);
        const bool haveMarket = leg.marketPrice > 0.0;
        const double mark = haveMarket ? leg.marketPrice : model;
        const double unitPnl = mark - leg.entryPrice;
        const double pnl = unitPnl * leg.quantity * m_position.multiplier;
        QTableWidgetItem* marketItem = m_table->item(row, ColMarket);
        marketItem->setText(haveMarket ? number(leg.marketPrice) : QStringLiteral("–"));
        marketItem->setToolTip(haveMarket ? (leg.expiryDate.empty() ? QString() : QStringLiteral("Chain mid for %1 expiry").arg(QString::fromStdString(leg.expiryDate)))
                                          : (chainMode() && leg.kind != LegKind::Underlying ? QStringLiteral("Contract not listed in the loaded chain") : QString()));
        m_table->item(row, ColModel)->setText(number(model));
        m_table->item(row, ColUnitPnl)->setText(number(unitPnl));
        m_table->item(row, ColUnitPnl)->setToolTip(haveMarket ? "Chain mid minus entry" : "Model price minus entry");
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

    const int greekIndex = m_greekSelect->currentIndex();
    if (m_greekMode->currentData().toString() == "time") {
        // Greek along the calendar to the first expiry, at spot and at four shifted spots.
        const double shifts[] = { -0.10, -0.05, 0.0, 0.05, 0.10 };
        const QColor colours[] = { QColor(m_theme.down), QColor(m_theme.down).lighter(140), QColor(m_theme.accent), QColor(m_theme.up).lighter(140), QColor(m_theme.up) };
        double gMin = INFINITY, gMax = -INFINITY, maxDays = 0.0;
        for (int i = 0; i < 5; ++i) {
            auto* line = new QLineSeries;
            line->setName(shifts[i] == 0.0 ? QStringLiteral("at spot %1").arg(number(market.spot, 2)) : QStringLiteral("spot %1%2%").arg(shifts[i] > 0 ? "+" : "").arg(shifts[i] * 100.0, 0, 'f', 0));
            line->setPen(QPen(colours[i], shifts[i] == 0.0 ? 2.4 : 1.6, shifts[i] == 0.0 ? Qt::SolidLine : Qt::DashLine));
            for (const GreekTimePoint& pt : greeksOverTime(m_position, market, market.spot * (1.0 + shifts[i]), 80)) {
                const double v = greekValue(pt.greeks, greekIndex);
                if (!std::isfinite(v)) continue;
                line->append(pt.days, v);
                gMin = std::min(gMin, v); gMax = std::max(gMax, v); maxDays = std::max(maxDays, pt.days);
            }
            m_greekChart->addSeries(line);
            line->attachAxis(m_greekX);
            line->attachAxis(m_greekY);
        }
        if (!std::isfinite(gMin)) { gMin = -1; gMax = 1; }
        const double gPad = std::max((gMax - gMin) * 0.1, 1e-6);
        auto* zero = new QLineSeries;
        zero->append(0.0, 0.0); zero->append(std::max(maxDays, 1.0), 0.0);
        zero->setPen(QPen(QColor(m_theme.textMuted), 1.0, Qt::DashLine));
        m_greekChart->addSeries(zero);
        zero->attachAxis(m_greekX);
        zero->attachAxis(m_greekY);
        for (QLegendMarker* marker : m_greekChart->legend()->markers(zero)) marker->setVisible(false);
        m_greekX->setRange(0.0, std::max(maxDays, 1.0));
        m_greekX->setTitleText("Calendar days from today");
        m_greekX->setLabelFormat("%.0f");
        m_greekY->setRange(gMin - gPad, gMax + gPad);
        m_greekY->setTitleText(m_greekSelect->currentText());
        m_greekChart->setTitle(QStringLiteral("Position %1 over time to the first expiry").arg(m_greekSelect->currentText().toLower()));
        m_greekBox->setTitle("Greeks over time");
        styleChart(m_payoffChart, m_theme);
        styleChart(m_greekChart, m_theme);
        return;
    }
    m_greekBox->setTitle("Greeks across spot");
    m_greekX->setTitleText("Underlying price");
    m_greekX->setLabelFormat("%.2f");

    // Greek across spot
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
        if (leg.marketPrice > 0.0) l["marketPrice"] = leg.marketPrice;
        if (!leg.expiryDate.empty()) l["expiryDate"] = QString::fromStdString(leg.expiryDate);
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
        leg.marketPrice = l["marketPrice"].toDouble(0.0);
        leg.expiryDate = l["expiryDate"].toString().toStdString();
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
    s << "kind,quantity,strike,expiry_date,maturity_years,volatility,entry_price,market_mid,model_price,unit_pnl,position_pnl\n";
    for (const Leg& leg : m_position.legs) {
        const double model = legValue(leg, market, market.spot, 0.0);
        const double mark = leg.marketPrice > 0.0 ? leg.marketPrice : model;
        s << legKindName(leg.kind) << "," << leg.quantity << "," << leg.strike << "," << QString::fromStdString(leg.expiryDate) << "," << leg.maturity << ","
          << (leg.volatility > 0 ? leg.volatility : market.volatility) << "," << leg.entryPrice << ","
          << (leg.marketPrice > 0.0 ? QString::number(leg.marketPrice) : QString()) << "," << model << ","
          << (mark - leg.entryPrice) << "," << (mark - leg.entryPrice) * leg.quantity * m_position.multiplier << "\n";
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

// MARK: - Greek chart mode (assistant and tests)

bool StrategyTab::setGreekMode(const QString& mode)
{
    const QString wanted = mode.trimmed().toLower();
    const int index = wanted.contains("time") || wanted.contains("calendar") || wanted.contains("day") ? 1 : (wanted.contains("spot") || wanted.contains("price") ? 0 : -1);
    if (index < 0) return false;
    m_greekMode->setCurrentIndex(index);
    return true;
}

QString StrategyTab::greekMode() const { return m_greekMode->currentData().toString(); }

bool StrategyTab::setGreek(const QString& name)
{
    const int index = m_greekSelect->findText(name.trimmed(), Qt::MatchFixedString);
    if (index < 0) return false;
    m_greekSelect->setCurrentIndex(index);
    return true;
}
