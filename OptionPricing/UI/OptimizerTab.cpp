//
//  OptimizerTab.cpp
//  OptionPricing
//

#include "OptimizerTab.h"
#include "Formatting.h"
#include "../Pricing/VolSurface.h"

#include <QtCharts/QCategoryAxis>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cmath>

using namespace pricing;
using pricing::opt::Candidate;

namespace {

constexpr const char* kObjectiveKey = "optimizer/objective";
constexpr const char* kFamilyKey = "optimizer/family";
constexpr const char* kMinDaysKey = "optimizer/minDays";
constexpr const char* kMaxDaysKey = "optimizer/maxDays";
constexpr const char* kDefinedRiskKey = "optimizer/definedRisk";
constexpr const char* kMaxLossKey = "optimizer/maxLoss";
constexpr const char* kSplitterKey = "optimizer/splitter";

enum Column { ColStrategy = 0, ColExpiry, ColLegs, ColNet, ColMaxProfit, ColMaxLoss, ColPopRn, ColPopView, ColExpected, ColExpectedReturn, ColAtTarget, ColRor, ColDelta, ColTheta, ColVega, ColScore, ColObjective, ColumnCount };

class SortableItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem& other) const override
    {
        const QVariant a = data(Qt::UserRole), b = other.data(Qt::UserRole);
        if (a.typeId() == QMetaType::Double && b.typeId() == QMetaType::Double) return a.toDouble() < b.toDouble();
        return QTableWidgetItem::operator<(other);
    }
};

QTableWidgetItem* cell(const QString& text, Qt::Alignment alignment = Qt::AlignRight | Qt::AlignVCenter)
{
    auto* item = new SortableItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QTableWidgetItem* numeric(double value, const QString& text, bool colour = false, const Theme* theme = nullptr)
{
    QTableWidgetItem* item = cell(text);
    item->setData(Qt::UserRole, value);
    if (colour && theme && value != 0.0 && std::isfinite(value)) item->setForeground(QColor(value > 0 ? theme->up : theme->down));
    return item;
}

QString pct(double fraction, int decimals = 0) { return ui::number(fraction * 100.0, decimals) + "%"; }

} // namespace

const std::vector<OptimizerTab::Family>& OptimizerTab::families()
{
    static const std::vector<Family> list = {
        { "All strategies", {} },
        { "Vertical spreads", { StrategyPreset::BullCallSpread, StrategyPreset::BearPutSpread, StrategyPreset::BullPutSpread, StrategyPreset::BearCallSpread } },
        { "Condors & butterflies", { StrategyPreset::IronCondor, StrategyPreset::IronButterfly, StrategyPreset::LongCallButterfly } },
        { "Straddles & strangles", { StrategyPreset::LongStraddle, StrategyPreset::ShortStraddle, StrategyPreset::LongStrangle, StrategyPreset::ShortStrangle } },
        { "Single options", { StrategyPreset::LongCall, StrategyPreset::LongPut } },
        { "Calendars", { StrategyPreset::CallCalendar, StrategyPreset::PutCalendar } },
        { "Shares: covered calls & collars", { StrategyPreset::CoveredCall, StrategyPreset::ProtectivePut, StrategyPreset::Collar } },
        { "Risk reversals", { StrategyPreset::RiskReversal } },
    };
    return list;
}

OptimizerTab::OptimizerTab(MarketState& state, QWidget* parent) : QWidget(parent), m_state(state)
{
    buildUi();
    wire();
    loadSettings();
    m_state.subscribe([this] { refreshUnderlying(); });
    refreshUnderlying();
}

void OptimizerTab::buildUi()
{
    m_underlying = new QLabel(this);
    m_underlying->setObjectName("sectionTitle");
    m_target = ui::makeSpinBox(this, 0.01, 1e6, 1.0, 2, 100.0);
    m_target->setToolTip("Where you expect the spot to be at the first expiry (the centre of your view)");
    m_move = ui::makeSpinBox(this, -90.0, 500.0, 1.0, 1, 0.0, " %");
    m_move->setToolTip("The same target as a move from the current spot");
    m_viewVol = ui::makeSpinBox(this, 0.0, 300.0, 1.0, 1, 0.0, " %");
    m_viewVol->setToolTip("Annualised uncertainty around your target; 0 uses the chain's ATM implied vol");
    m_viewVol->setSpecialValueText("ATM implied");
    m_minDays = ui::makeIntSpinBox(this, 1, 365, 20);
    m_minDays->setSuffix(" d");
    m_maxDays = ui::makeIntSpinBox(this, 2, 730, 60);
    m_maxDays->setSuffix(" d");
    m_objective = new QComboBox(this);
    for (opt::Objective o : { opt::Objective::ExpectedReturnOnRisk, opt::Objective::ExpectedPnl, opt::Objective::PnlAtTarget, opt::Objective::ProbabilityOfProfit, opt::Objective::ReturnOnRisk, opt::Objective::Score }) {
        m_objective->addItem(opt::objectiveName(o), static_cast<int>(o));
    }
    m_objective->setMinimumWidth(240);
    m_objective->setToolTip("What the optimiser maximises");
    m_family = new QComboBox(this);
    for (const Family& f : families()) m_family->addItem(f.label);
    m_family->setMinimumWidth(200);
    m_maxLoss = ui::makeSpinBox(this, 0.0, 1e7, 100.0, 0, 0.0, "");
    m_maxLoss->setPrefix("$");
    m_maxLoss->setSpecialValueText("no cap");
    m_maxLoss->setToolTip("Largest acceptable loss per position (0 = no cap)");
    m_definedRisk = new QCheckBox("Defined risk only", this);
    m_definedRisk->setToolTip("Skip naked short options and share-based structures");
    m_compare = ui::makeButton(this, "Compare presets", "secondary", "One delta-targeted candidate per preset on the expiry in the middle of the window, side by side");
    m_run = ui::makeButton(this, "Optimize", "primary", "Enumerate listed strikes and expiries for the chosen family and rank by the objective");
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setWordWrap(true);

    auto label = [this](const QString& text) { auto* l = new QLabel(text, this); l->setObjectName("muted"); return l; };
    auto* row1 = new QHBoxLayout;
    row1->setSpacing(8);
    row1->addWidget(label("Target price"));
    row1->addWidget(m_target);
    row1->addWidget(label("Move"));
    row1->addWidget(m_move);
    row1->addWidget(label("View vol"));
    row1->addWidget(m_viewVol);
    row1->addSpacing(12);
    row1->addWidget(label("Expiry"));
    row1->addWidget(m_minDays);
    row1->addWidget(label("to"));
    row1->addWidget(m_maxDays);
    row1->addStretch(1);
    auto* row2 = new QHBoxLayout;
    row2->setSpacing(8);
    row2->addWidget(label("Objective"));
    row2->addWidget(m_objective);
    row2->addWidget(label("Family"));
    row2->addWidget(m_family);
    row2->addWidget(label("Max loss"));
    row2->addWidget(m_maxLoss);
    row2->addWidget(m_definedRisk);
    row2->addStretch(1);
    row2->addWidget(m_compare);
    row2->addWidget(m_run);
    auto* controls = new QFrame(this);
    controls->setObjectName("pane");
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(12, 10, 12, 10);
    controlsLayout->setSpacing(6);
    controlsLayout->addWidget(m_underlying);
    controlsLayout->addLayout(row1);
    controlsLayout->addLayout(row2);
    controlsLayout->addWidget(m_status);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Strategy", "Expiry", "Legs", "Net", "Max profit", "Max loss", "PoP (mkt)", "PoP (view)", "Exp. P&L (view)", "Exp. return", "P&L at target", "Return/risk", "Delta", "Theta/d", "Vega", "Score", "Objective" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setObjectName("heatmapHeader");
    m_table->horizontalHeader()->setFixedHeight(26);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(24);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setMinimumHeight(120);
    for (int c = 0; c < ColumnCount; ++c) {
        int w = 84;
        if (c == ColStrategy) w = 150; else if (c == ColExpiry) w = 120; else if (c == ColLegs) w = 260; else if (c == ColExpected || c == ColAtTarget) w = 100;
        m_table->setColumnWidth(c, w);
    }
    m_tableTitle = new QLabel("Candidates", this);
    m_tableTitle->setObjectName("sectionTitle");
    m_openStrategy = ui::makeButton(this, "Open in Strategy", "secondary", "Load the selected candidate's legs into the Strategy tab");
    m_addPortfolio = ui::makeButton(this, "Add to Portfolio", "secondary", "Book the selected candidate's legs as positions at the chain mids");
    m_exportCsv = ui::makeButton(this, "Export CSV…", "secondary", "Save the candidates table");
    auto* tableHeader = new QHBoxLayout;
    tableHeader->setSpacing(8);
    tableHeader->addWidget(m_tableTitle);
    tableHeader->addStretch(1);
    tableHeader->addWidget(m_openStrategy);
    tableHeader->addWidget(m_addPortfolio);
    tableHeader->addWidget(m_exportCsv);
    auto* tablePane = new QFrame(this);
    tablePane->setObjectName("pane");
    auto* tableLayout = new QVBoxLayout(tablePane);
    tableLayout->setContentsMargins(12, 10, 12, 10);
    tableLayout->setSpacing(6);
    tableLayout->addLayout(tableHeader);
    tableLayout->addWidget(m_table, 1);

    m_chart = new QChart;
    m_chart->setTitle("P&L at the first expiry");
    m_chart->legend()->setVisible(true);
    m_chart->legend()->setAlignment(Qt::AlignBottom);
    m_chartView = ui::makeHoverChartView(this, m_chart, 180);
    auto* chartPane = new QFrame(this);
    chartPane->setObjectName("pane");
    auto* chartLayout = new QVBoxLayout(chartPane);
    chartLayout->setContentsMargins(12, 10, 12, 10);
    chartLayout->addWidget(m_chartView, 1);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->addWidget(tablePane);
    m_splitter->addWidget(chartPane);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setChildrenCollapsible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    root->addWidget(controls);
    root->addWidget(m_splitter, 1);
}

void OptimizerTab::wire()
{
    connect(m_target, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (m_updating || m_spot <= 0.0) return;
        m_updating = true;
        m_move->setValue((v / m_spot - 1.0) * 100.0);
        m_updating = false;
    });
    connect(m_move, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (m_updating || m_spot <= 0.0) return;
        m_updating = true;
        m_target->setValue(m_spot * (1.0 + v / 100.0));
        m_updating = false;
    });
    for (QComboBox* box : { m_objective, m_family }) connect(box, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { if (!m_updating) saveSettings(); });
    for (QSpinBox* box : { m_minDays, m_maxDays }) connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { if (!m_updating) saveSettings(); });
    connect(m_maxLoss, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { if (!m_updating) saveSettings(); });
    connect(m_definedRisk, &QCheckBox::toggled, this, [this](bool) { if (!m_updating) saveSettings(); });
    connect(m_run, &QPushButton::clicked, this, [this] { runOptimizer(); });
    connect(m_compare, &QPushButton::clicked, this, [this] { comparePresets(); });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int) { QSettings().setValue(kSplitterKey, m_splitter->saveState()); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { fillChart(); });
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) {
        const auto sel = selectedCandidates();
        if (!sel.empty()) openCandidate(sel.front());
    });
    connect(m_openStrategy, &QPushButton::clicked, this, [this] {
        const auto sel = selectedCandidates();
        if (sel.empty() || !openCandidate(sel.front())) setStatus("Select a candidate first.", ui::StatusKind::Warning);
    });
    connect(m_addPortfolio, &QPushButton::clicked, this, [this] {
        const auto sel = selectedCandidates();
        if (sel.empty() || !onAddToPortfolio) { setStatus("Select a candidate first.", ui::StatusKind::Warning); return; }
        const Candidate& c = m_candidates[static_cast<size_t>(sel.front())];
        onAddToPortfolio(m_ticker, c.position);
        setStatus(QStringLiteral("%1 %2 booked in the Portfolio at the chain mids.").arg(m_ticker, QString::fromStdString(c.strategy)), ui::StatusKind::Info);
    });
    connect(m_exportCsv, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, "Export candidates", QDir::homePath() + QStringLiteral("/%1-strategies.csv").arg(m_ticker.isEmpty() ? QStringLiteral("optimizer") : m_ticker), "CSV (*.csv)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { setStatus("Could not write the file.", ui::StatusKind::Error); return; }
        file.write(resultsCsv().toUtf8());
        setStatus(QStringLiteral("Exported %1 candidate(s).").arg(m_candidates.size()), ui::StatusKind::Info);
    });
}

void OptimizerTab::loadSettings()
{
    QSettings settings;
    m_updating = true;
    m_objective->setCurrentIndex(std::max(0, m_objective->findData(settings.value(kObjectiveKey, static_cast<int>(opt::Objective::ExpectedReturnOnRisk)).toInt())));
    m_family->setCurrentIndex(std::max(0, m_family->findText(settings.value(kFamilyKey, "All strategies").toString())));
    m_minDays->setValue(settings.value(kMinDaysKey, 20).toInt());
    m_maxDays->setValue(settings.value(kMaxDaysKey, 60).toInt());
    m_definedRisk->setChecked(settings.value(kDefinedRiskKey, true).toBool());
    m_maxLoss->setValue(settings.value(kMaxLossKey, 0.0).toDouble());
    if (settings.contains(kSplitterKey)) m_splitter->restoreState(settings.value(kSplitterKey).toByteArray());
    m_updating = false;
    setStatus(QString(), ui::StatusKind::Info);
}

void OptimizerTab::saveSettings() const
{
    QSettings settings;
    settings.setValue(kObjectiveKey, m_objective->currentData().toInt());
    settings.setValue(kFamilyKey, m_family->currentText());
    settings.setValue(kMinDaysKey, m_minDays->value());
    settings.setValue(kMaxDaysKey, m_maxDays->value());
    settings.setValue(kDefinedRiskKey, m_definedRisk->isChecked());
    settings.setValue(kMaxLossKey, m_maxLoss->value());
}

void OptimizerTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    styleChart(m_chart, theme);
    m_chartView->lineColour = QColor(theme.textMuted);
    m_chartView->boxBorder = QColor(theme.border);
    m_chartView->textColour = QColor(theme.textStrong);
    QColor bg(theme.surface); bg.setAlpha(235);
    m_chartView->boxBackground = bg;
    fillTable();
    fillChart();
}

// MARK: - Inputs

bool OptimizerTab::chain(std::vector<ChainQuote>& quotes, double& spot, QString& ticker) const
{
    if (!m_sampleQuotes.empty()) { quotes = m_sampleQuotes; spot = m_sampleSpot; ticker = "SAMPLE"; return true; }
    if (!m_state.chainQuotes.empty() && m_state.market.spot > 0.0) { quotes = m_state.chainQuotes; spot = m_state.market.spot; ticker = m_state.underlyingTicker; return true; }
    if (m_store && !m_state.underlyingTicker.isEmpty()) {
        if (const auto stored = m_store->get(m_state.underlyingTicker); stored && !stored->download.quotes.empty() && stored->snapshot.price > 0.0) {
            quotes = stored->download.quotes; spot = stored->snapshot.price; ticker = m_state.underlyingTicker; return true;
        }
    }
    return false;
}

ActivityMarket OptimizerTab::activityMarket(double spot) const
{
    ActivityMarket am;
    am.model = m_state.market.model;
    am.spot = spot;
    am.dividendYield = m_state.market.dividendYield;
    am.rateFor = [this](double t) { return m_state.rateFor(t); };
    return am;
}

opt::Options OptimizerTab::options() const
{
    opt::Options o;
    o.minDays = std::min(m_minDays->value(), m_maxDays->value());
    o.maxDays = std::max(m_minDays->value(), m_maxDays->value());
    o.objective = static_cast<opt::Objective>(m_objective->currentData().toInt());
    o.families = families()[static_cast<size_t>(std::max(0, m_family->currentIndex()))].presets;
    o.definedRiskOnly = m_definedRisk->isChecked();
    o.maxLoss = m_maxLoss->value();
    o.maxResults = 30;
    return o;
}

opt::View OptimizerTab::view() const
{
    opt::View v;
    v.targetSpot = m_target->value();
    v.volAnnual = m_viewVol->value() / 100.0;
    return v;
}

void OptimizerTab::refreshUnderlying()
{
    std::vector<ChainQuote> quotes;
    double spot = 0.0;
    QString ticker;
    if (!chain(quotes, spot, ticker)) {
        m_ticker.clear();
        m_spot = 0.0;
        m_underlying->setText("No option chain loaded: pick a ticker on the Quotes or Option Chain tab.");
        return;
    }
    const bool changed = ticker != m_ticker || std::fabs(spot - m_spot) > 1e-9;
    m_ticker = ticker;
    m_spot = spot;
    if (changed) {
        const scan::ChainMetrics metrics = scan::chainMetrics(quotes, activityMarket(spot), (m_minDays->value() + m_maxDays->value()) / 2);
        m_atmIv = metrics.ok ? metrics.atmIv : 0.0;
        // Keep the target as a move when the spot changes (a fresh ticker starts at the money).
        m_updating = true;
        const double move = m_move->value();
        m_target->setValue(spot * (1.0 + move / 100.0));
        m_updating = false;
    }
    m_underlying->setText(QStringLiteral("%1 · spot %2%3 · %4 contracts").arg(ticker, ui::number(spot, 2), m_atmIv > 0.0 ? QStringLiteral(" · ATM IV %1").arg(pct(m_atmIv, 1)) : QString()).arg(quotes.size()));
}

// MARK: - Runs

void OptimizerTab::runOptimizer()
{
    std::vector<ChainQuote> quotes;
    double spot = 0.0;
    QString ticker;
    if (!chain(quotes, spot, ticker)) { setStatus("No option chain loaded for the current ticker.", ui::StatusKind::Warning); return; }
    QElapsedTimer timer;
    timer.start();
    const opt::Options o = options();
    m_candidates = opt::optimize(quotes, activityMarket(spot), m_state.market, view(), o);
    m_mode = "optimizer";
    m_ranAt = QDateTime::currentDateTime();
    fillTable();
    setStatus(QStringLiteral("%1: %2 candidate(s) ranked by %3 in %4 ms · view: %5 at the first expiry (%6%), vol %7 · %8, %9-%10 days%11.")
                  .arg(ticker).arg(m_candidates.size()).arg(opt::objectiveName(o.objective)).arg(timer.elapsed())
                  .arg(ui::number(m_target->value(), 2), (m_move->value() >= 0 ? "+" : "") + ui::number(m_move->value(), 1), m_viewVol->value() > 0 ? pct(m_viewVol->value() / 100.0, 1) : QStringLiteral("ATM implied"), m_family->currentText())
                  .arg(o.minDays).arg(o.maxDays).arg(o.definedRiskOnly ? QStringLiteral(", defined risk only") : QString()),
              m_candidates.empty() ? ui::StatusKind::Warning : ui::StatusKind::Info);
}

void OptimizerTab::comparePresets()
{
    std::vector<ChainQuote> quotes;
    double spot = 0.0;
    QString ticker;
    if (!chain(quotes, spot, ticker)) { setStatus("No option chain loaded for the current ticker.", ui::StatusKind::Warning); return; }
    opt::Options o = options();
    o.families.clear();
    o.maxResults = 40;
    m_candidates = opt::presetsOnChain(quotes, activityMarket(spot), m_state.market, view(), (o.minDays + o.maxDays) / 2, o);
    m_mode = "presets";
    m_ranAt = QDateTime::currentDateTime();
    fillTable();
    setStatus(QStringLiteral("%1: %2 preset(s) built with 30-delta shorts and 15-delta wings on %3, ranked by %4 under your view.")
                  .arg(ticker).arg(m_candidates.size()).arg(m_candidates.empty() ? QStringLiteral("the chosen expiry") : QString::fromStdString(m_candidates.front().expiry.expiryDate), opt::objectiveName(o.objective)),
              m_candidates.empty() ? ui::StatusKind::Warning : ui::StatusKind::Info);
}

void OptimizerTab::loadSampleData()
{
    ChainMarket cm;
    cm.spot = 245.0;
    cm.riskFreeRate = m_state.market.riskFreeRate;
    m_sampleQuotes = syntheticChain(cm, { 24.0 / 365.0, 45.0 / 365.0, 80.0 / 365.0, 171.0 / 365.0 }, 0.42, -0.35, 0.8, 5.0, 16, 0.05);
    const QDate today = QDate::currentDate();
    for (ChainQuote& q : m_sampleQuotes) {
        q.daysToExpiry = static_cast<int>(std::lround(q.maturity * 365.0));
        q.expiryDate = today.addDays(q.daysToExpiry).toString(Qt::ISODate).toStdString();
    }
    m_sampleSpot = 245.0;
    refreshUnderlying();
    m_updating = true;
    m_move->setValue(8.0);
    m_target->setValue(m_sampleSpot * 1.08);
    m_updating = false;
    runOptimizer();
}

// MARK: - Presentation

QString OptimizerTab::fmtMoney(double v) const
{
    if (!std::isfinite(v)) return v > 0 ? QStringLiteral("open") : QStringLiteral("open");
    const QString body = "$" + ui::number(std::fabs(v), 0);
    return v < 0 ? QStringLiteral("−") + body : body;
}

void OptimizerTab::fillTable()
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(static_cast<int>(m_candidates.size()));
    for (int r = 0; r < static_cast<int>(m_candidates.size()); ++r) {
        const Candidate& c = m_candidates[static_cast<size_t>(r)];
        const StrategyAnalysis& a = c.analysis;
        auto* name = cell(QString::fromStdString(c.strategy), Qt::AlignLeft | Qt::AlignVCenter);
        name->setData(Qt::UserRole + 1, r);
        QFont bold = name->font(); bold.setBold(true); name->setFont(bold);
        m_table->setItem(r, ColStrategy, name);
        m_table->setItem(r, ColExpiry, numeric(c.expiry.daysToExpiry, QStringLiteral("%1 (%2d)").arg(QString::fromStdString(c.expiry.expiryDate)).arg(c.expiry.daysToExpiry)));
        m_table->setItem(r, ColLegs, cell(QString::fromStdString(c.legs), Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(r, ColNet, numeric(-a.netPremium, (a.netPremium < 0 ? "+" : "−") + fmtMoney(std::fabs(a.netPremium)), true, &m_theme));
        m_table->setItem(r, ColMaxProfit, numeric(a.unboundedProfit ? 1e18 : a.maxProfit, a.unboundedProfit ? "open" : fmtMoney(a.maxProfit)));
        m_table->setItem(r, ColMaxLoss, numeric(a.unboundedLoss ? -1e18 : a.maxLoss, a.unboundedLoss ? "open" : fmtMoney(a.maxLoss)));
        m_table->setItem(r, ColPopRn, numeric(a.probabilityOfProfit, pct(a.probabilityOfProfit)));
        m_table->setItem(r, ColPopView, numeric(c.view.probabilityOfProfit, pct(c.view.probabilityOfProfit)));
        m_table->setItem(r, ColExpected, numeric(c.view.expectedPnl, (c.view.expectedPnl >= 0 ? "+" : "") + fmtMoney(c.view.expectedPnl), true, &m_theme));
        m_table->setItem(r, ColExpectedReturn, numeric(c.expectedReturnOnRisk, (c.expectedReturnOnRisk >= 0 ? "+" : "") + pct(c.expectedReturnOnRisk), true, &m_theme));
        m_table->setItem(r, ColAtTarget, numeric(c.view.pnlAtTarget, (c.view.pnlAtTarget >= 0 ? "+" : "") + fmtMoney(c.view.pnlAtTarget), true, &m_theme));
        m_table->setItem(r, ColRor, numeric(c.returnOnRisk, pct(c.returnOnRisk)));
        m_table->setItem(r, ColDelta, numeric(a.greeks.delta, ui::number(a.greeks.delta, 1), true, &m_theme));
        m_table->setItem(r, ColTheta, numeric(a.greeks.theta, (a.greeks.theta >= 0 ? "+" : "") + fmtMoney(a.greeks.theta), true, &m_theme));
        m_table->setItem(r, ColVega, numeric(a.greeks.vega, (a.greeks.vega >= 0 ? "+" : "") + fmtMoney(a.greeks.vega)));
        m_table->setItem(r, ColScore, numeric(c.score, ui::number(c.score, 2)));
        m_table->setItem(r, ColObjective, numeric(c.objective, ui::number(c.objective, 3)));
    }
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(ColObjective, Qt::DescendingOrder);
    m_tableTitle->setText(m_mode == "presets" ? QStringLiteral("Presets compared (%1)").arg(m_candidates.size()) : QStringLiteral("Candidates (%1)").arg(m_candidates.size()));
    // Overlay the top three by default.
    m_table->clearSelection();
    for (int r = 0; r < std::min(3, m_table->rowCount()); ++r) {
        for (int c = 0; c < ColumnCount; ++c) if (QTableWidgetItem* item = m_table->item(r, c)) item->setSelected(true);
    }
    fillChart();
}

std::vector<int> OptimizerTab::selectedCandidates() const
{
    std::vector<int> out;
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) {
        if (const QTableWidgetItem* item = m_table->item(index.row(), ColStrategy)) out.push_back(item->data(Qt::UserRole + 1).toInt());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void OptimizerTab::fillChart()
{
    m_chartView->probe = nullptr;
    m_chartView->readout = nullptr;
    m_chart->removeAllSeries();
    for (QAbstractAxis* axis : m_chart->axes()) m_chart->removeAxis(axis);
    styleChart(m_chart, m_theme);
    std::vector<int> sel = selectedCandidates();
    if (sel.size() > 6) sel.resize(6);
    if (sel.empty() || m_candidates.empty()) { m_chart->setTitle("P&L at the first expiry: select candidates above"); return; }
    const QStringList palette{ m_theme.accent.isEmpty() ? "#3b82f6" : m_theme.accent, m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2,
                               m_theme.accent3.isEmpty() ? "#22d3ee" : m_theme.accent3, "#e879f9", m_theme.up.isEmpty() ? "#22c55e" : m_theme.up, m_theme.down.isEmpty() ? "#ef4444" : m_theme.down };
    double lo = INFINITY, hi = -INFINITY, pnlLo = 0.0, pnlHi = 0.0;
    struct Curve { QString name; std::vector<double> spots, pnl; };
    std::vector<Curve> curves;
    QAbstractSeries* probe = nullptr;
    int k = 0;
    for (int index : sel) {
        const Candidate& c = m_candidates[static_cast<size_t>(index)];
        auto* line = new QLineSeries;
        line->setName(QStringLiteral("%1 %2").arg(QString::fromStdString(c.strategy), QString::fromStdString(c.legs)));
        QPen pen(QColor(palette.at(k % palette.size())));
        pen.setWidthF(2.0);
        line->setPen(pen);
        Curve curve{ QString::fromStdString(c.strategy), {}, {} };
        for (size_t i = 0; i < c.analysis.spots.size(); ++i) {
            // Keep the chart to ±35% of spot so wide strikes do not flatten the interesting region.
            if (c.analysis.spots[i] < m_spot * 0.65 || c.analysis.spots[i] > m_spot * 1.35) continue;
            line->append(c.analysis.spots[i], c.analysis.pnlAtHorizon[i]);
            curve.spots.push_back(c.analysis.spots[i]);
            curve.pnl.push_back(c.analysis.pnlAtHorizon[i]);
            lo = std::min(lo, c.analysis.spots[i]); hi = std::max(hi, c.analysis.spots[i]);
            pnlLo = std::min(pnlLo, c.analysis.pnlAtHorizon[i]); pnlHi = std::max(pnlHi, c.analysis.pnlAtHorizon[i]);
        }
        m_chart->addSeries(line);
        if (!probe) probe = line;
        curves.push_back(std::move(curve));
        ++k;
    }
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo >= hi) return;
    // Markers: the current spot and the target.
    auto vertical = [&](double x, const QColor& colour, const QString& name, Qt::PenStyle style) {
        auto* line = new QLineSeries;
        line->setName(name);
        line->append(x, pnlLo * 1.05 - 1.0);
        line->append(x, pnlHi * 1.05 + 1.0);
        QPen pen(colour); pen.setWidthF(1.2); pen.setStyle(style);
        line->setPen(pen);
        m_chart->addSeries(line);
    };
    vertical(m_spot, QColor(m_theme.textMuted), QStringLiteral("Spot %1").arg(ui::number(m_spot, 2)), Qt::DashLine);
    if (std::fabs(m_target->value() - m_spot) > 1e-9) vertical(m_target->value(), QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2), QStringLiteral("Target %1").arg(ui::number(m_target->value(), 2)), Qt::DotLine);
    auto* zero = new QLineSeries;
    zero->append(lo, 0.0); zero->append(hi, 0.0);
    QPen zeroPen(QColor(m_theme.border)); zeroPen.setWidthF(1.0);
    zero->setPen(zeroPen);
    m_chart->addSeries(zero);
    m_chart->legend()->markers(zero).first()->setVisible(false);

    auto* x = new QValueAxis;
    x->setTitleText("Spot at expiry");
    x->setLabelFormat("%.0f");
    x->setRange(lo, hi);
    x->setTickCount(8);
    x->setTruncateLabels(false);
    auto* y = new QValueAxis;
    y->setTitleText("P&L ($)");
    y->setLabelFormat("%.0f");
    y->setRange(pnlLo * 1.08 - 1.0, pnlHi * 1.08 + 1.0);
    y->setTickCount(6);
    y->setTruncateLabels(false);
    m_chart->addAxis(x, Qt::AlignBottom);
    m_chart->addAxis(y, Qt::AlignLeft);
    for (QAbstractSeries* s : m_chart->series()) { s->attachAxis(x); s->attachAxis(y); }
    m_chart->setTitle(QStringLiteral("%1 · P&L at the first expiry").arg(m_ticker));
    styleChart(m_chart, m_theme);
    m_chartView->probe = probe;
    m_chartView->readout = [curves, this](double spot) -> QStringList {
        QStringList lines{ QStringLiteral("Spot %1 (%2%)").arg(ui::number(spot, 2), (spot >= m_spot ? "+" : "") + ui::number((spot / std::max(m_spot, 1e-9) - 1.0) * 100.0, 1)) };
        for (const Curve& c : curves) {
            if (c.spots.empty()) continue;
            auto it = std::lower_bound(c.spots.begin(), c.spots.end(), spot);
            if (it == c.spots.end()) --it;
            const size_t i = static_cast<size_t>(it - c.spots.begin());
            lines << QStringLiteral("%1  %2").arg(c.name, (c.pnl[i] >= 0 ? "+" : "") + fmtMoney(c.pnl[i]));
        }
        return lines;
    };
}

void OptimizerTab::setStatus(const QString& text, ui::StatusKind kind) { ui::setStatus(m_status, text, kind); }

// MARK: - Assistant hooks

QStringList OptimizerTab::objectiveNames() const
{
    QStringList out;
    for (int i = 0; i < m_objective->count(); ++i) out << m_objective->itemText(i);
    return out;
}

bool OptimizerTab::setObjective(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    for (int i = 0; i < m_objective->count(); ++i) {
        const QString text = m_objective->itemText(i).toLower();
        if (text == wanted || text.contains(wanted) || (wanted.contains("expected") && wanted.contains("return") && text.startsWith("expected return"))
            || (wanted.contains("expected") && !wanted.contains("return") && text.startsWith("expected p")) || (wanted.contains("target") && text.contains("target"))
            || (wanted.contains("prob") && text.contains("probability")) || ((wanted.contains("return") || wanted.contains("ror")) && text.startsWith("return")) || (wanted.contains("score") && text.contains("weighted"))) {
            m_objective->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

QStringList OptimizerTab::familyNames() const
{
    QStringList out;
    for (const Family& f : families()) out << f.label;
    return out;
}

bool OptimizerTab::setFamily(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    for (int i = 0; i < m_family->count(); ++i) {
        const QString text = m_family->itemText(i).toLower();
        if (text == wanted || text.contains(wanted) || (wanted.contains("spread") && text.contains("vertical")) || (wanted.contains("condor") && text.contains("condor"))
            || (wanted.contains("butterfl") && text.contains("butterfl")) || (wanted.contains("straddle") && text.contains("straddle")) || (wanted.contains("strangle") && text.contains("strangle"))
            || ((wanted.contains("single") || wanted.contains("outright")) && text.contains("single")) || (wanted.contains("calendar") && text.contains("calendar"))
            || ((wanted.contains("covered") || wanted.contains("collar") || wanted.contains("share")) && text.contains("shares")) || (wanted == "all" && text.contains("all"))) {
            m_family->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

void OptimizerTab::setTargetPrice(double price) { if (price > 0.0) m_target->setValue(price); }
void OptimizerTab::setTargetMovePercent(double percent) { m_move->setValue(std::clamp(percent, -90.0, 500.0)); }
void OptimizerTab::setViewVolPercent(double percent) { m_viewVol->setValue(std::clamp(percent, 0.0, 300.0)); }

void OptimizerTab::setDays(int minDays, int maxDays)
{
    m_updating = true;
    if (minDays > 0) m_minDays->setValue(minDays);
    if (maxDays > 0) m_maxDays->setValue(maxDays);
    m_updating = false;
    saveSettings();
}

bool OptimizerTab::openCandidate(int index)
{
    if (index < 0 || index >= static_cast<int>(m_candidates.size()) || !onOpenInStrategy) return false;
    onOpenInStrategy(m_ticker, m_candidates[static_cast<size_t>(index)].position);
    return true;
}

QString OptimizerTab::summaryText() const
{
    QString out;
    QTextStream s(&out);
    s << "Optimizer: " << (m_ticker.isEmpty() ? QStringLiteral("no chain loaded") : m_ticker) << ", spot " << ui::number(m_spot, 2) << ". View: target " << ui::number(m_target->value(), 2)
      << " (" << (m_move->value() >= 0 ? "+" : "") << ui::number(m_move->value(), 1) << "%) at the first expiry, uncertainty " << (m_viewVol->value() > 0 ? pct(m_viewVol->value() / 100.0, 1) : QStringLiteral("ATM implied"))
      << "; objective " << m_objective->currentText() << "; family " << m_family->currentText() << "; expiries " << m_minDays->value() << "-" << m_maxDays->value() << " days"
      << (m_definedRisk->isChecked() ? "; defined risk only" : "") << (m_maxLoss->value() > 0 ? QStringLiteral("; max loss $%1").arg(ui::number(m_maxLoss->value(), 0)) : QString()) << ".";
    if (!m_ranAt.isValid()) { s << " Not run yet."; return out; }
    s << " " << (m_mode == "presets" ? "Preset comparison" : "Optimisation") << " at " << m_ranAt.toString("HH:mm") << ": " << m_candidates.size() << " candidate(s).";
    for (size_t i = 0; i < std::min<size_t>(5, m_candidates.size()); ++i) {
        const Candidate& c = m_candidates[i];
        s << "\n" << (i + 1) << ". " << QString::fromStdString(c.strategy) << " " << QString::fromStdString(c.expiry.expiryDate) << ": " << QString::fromStdString(c.legs)
          << " — " << (c.analysis.netPremium < 0 ? "credit " : "debit ") << fmtMoney(std::fabs(c.analysis.netPremium)) << ", max profit " << (c.analysis.unboundedProfit ? QStringLiteral("open") : fmtMoney(c.analysis.maxProfit))
          << ", max loss " << (c.analysis.unboundedLoss ? QStringLiteral("open") : fmtMoney(c.analysis.maxLoss)) << ", PoP market " << pct(c.analysis.probabilityOfProfit) << " / view " << pct(c.view.probabilityOfProfit)
          << ", expected P&L under view " << (c.view.expectedPnl >= 0 ? "+" : "") << fmtMoney(c.view.expectedPnl) << " (" << (c.expectedReturnOnRisk >= 0 ? "+" : "") << pct(c.expectedReturnOnRisk) << " of risk), P&L at target " << (c.view.pnlAtTarget >= 0 ? "+" : "") << fmtMoney(c.view.pnlAtTarget)
          << ", return on risk " << pct(c.returnOnRisk);
    }
    return out;
}

QString OptimizerTab::resultsCsv() const
{
    QString out = "ticker,strategy,expiry,days,legs,net_premium,max_profit,max_loss,pop_market_pct,pop_view_pct,expected_pnl_view,expected_return_on_risk_pct,pnl_at_target,return_on_risk_pct,delta,theta_day,vega,score,objective\n";
    for (const Candidate& c : m_candidates) {
        out += QStringLiteral("%1,%2,%3,%4,\"%5\",%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17,%18,%19\n")
                   .arg(m_ticker, QString::fromStdString(c.strategy), QString::fromStdString(c.expiry.expiryDate)).arg(c.expiry.daysToExpiry)
                   .arg(QString::fromStdString(c.legs), QString::number(c.analysis.netPremium, 'f', 0), c.analysis.unboundedProfit ? QStringLiteral("inf") : QString::number(c.analysis.maxProfit, 'f', 0),
                        c.analysis.unboundedLoss ? QStringLiteral("-inf") : QString::number(c.analysis.maxLoss, 'f', 0))
                   .arg(QString::number(c.analysis.probabilityOfProfit * 100.0, 'f', 1), QString::number(c.view.probabilityOfProfit * 100.0, 'f', 1), QString::number(c.view.expectedPnl, 'f', 0),
                        QString::number(c.expectedReturnOnRisk * 100.0, 'f', 1), QString::number(c.view.pnlAtTarget, 'f', 0), QString::number(c.returnOnRisk * 100.0, 'f', 1))
                   .arg(QString::number(c.analysis.greeks.delta, 'f', 1), QString::number(c.analysis.greeks.theta, 'f', 1), QString::number(c.analysis.greeks.vega, 'f', 1), QString::number(c.score, 'f', 3), QString::number(c.objective, 'f', 3));
    }
    return out;
}
