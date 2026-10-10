//
//  ScannerTab.cpp
//  OptionPricing
//

#include "ScannerTab.h"
#include "HelpContent.h"
#include "Formatting.h"
#include "../Pricing/Activity.h"
#include "../Pricing/IvHistory.h"
#include "../Pricing/VolSurface.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cmath>

using namespace pricing;
using pricing::scan::Idea;
using pricing::scan::Criteria;
using pricing::scan::Bias;

namespace {

constexpr const char* kScreenKey = "scanner/screen";
constexpr const char* kBiasKey = "scanner/bias";
constexpr const char* kUniverseKey = "scanner/universe";
constexpr const char* kMinDaysKey = "scanner/minDays";
constexpr const char* kMaxDaysKey = "scanner/maxDays";
constexpr const char* kMinPopKey = "scanner/minPop";
constexpr const char* kMinRorKey = "scanner/minRor";
constexpr const char* kMaxSpreadKey = "scanner/maxSpread";
constexpr const char* kMinOiKey = "scanner/minOi";
constexpr const char* kDefinedRiskKey = "scanner/definedRisk";
constexpr const char* kSplitterKey = "scanner/splitter";

enum MetricColumn { McTicker = 0, McSpot, McIv, McIvRank, McFarIv, McTerm, McSkew, McMove, McPcVol, McPcOi, McVolume, McOi, McSpread, McExpiry, McIdeas, MetricColumnCount };
enum IdeaColumn { IcTicker = 0, IcStrategy, IcBias, IcExpiry, IcLegs, IcNet, IcMaxProfit, IcMaxLoss, IcPop, IcRor, IcExpected, IcBreakevens, IcDelta, IcTheta, IcIv, IcSpread, IcScore, IcRationale, IdeaColumnCount };

/// Sorts by the numeric payload when both cells carry one.
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

const std::vector<ScannerTab::Screen>& ScannerTab::screens()
{
    static const std::vector<Screen> list = {
        { "Premium selling", "Credit structures at ~30-delta short strikes: bull put / bear call spreads, iron condors and butterflies (short strangles when undefined risk is allowed). Ranked by probability-weighted return on risk.",
          { StrategyPreset::BullPutSpread, StrategyPreset::BearCallSpread, StrategyPreset::IronCondor, StrategyPreset::IronButterfly, StrategyPreset::ShortStrangle, StrategyPreset::ShortStraddle }, 55.0, 15.0, true },
        { "Directional debit", "Defined-risk directional trades: bull call and bear put spreads plus outright calls and puts at the money.",
          { StrategyPreset::BullCallSpread, StrategyPreset::BearPutSpread, StrategyPreset::LongCall, StrategyPreset::LongPut }, 0.0, 50.0, true },
        { "Volatility", "Long straddles and strangles for a move, calendars for a quiet near term with richer far-dated vol.",
          { StrategyPreset::LongStraddle, StrategyPreset::LongStrangle, StrategyPreset::CallCalendar, StrategyPreset::PutCalendar }, 0.0, 0.0, false },
        { "Income on shares", "Covered calls and collars against 100 shares, calls written at ~30 delta.",
          { StrategyPreset::CoveredCall, StrategyPreset::Collar, StrategyPreset::ProtectivePut }, 0.0, 0.0, false },
        { "All strategies", "Every preset in the library, best three per ticker.", {}, 0.0, 0.0, false },
    };
    return list;
}

ScannerTab::ScannerTab(MarketState& state, QWidget* parent) : QWidget(parent), m_state(state)
{
    buildUi();
    wire();
    loadSettings();
}

void ScannerTab::buildUi()
{
    // ---- Controls ----
    m_screen = new QComboBox(this);
    for (const Screen& s : screens()) m_screen->addItem(s.label);
    m_screen->setMinimumWidth(170);
    m_screen->setToolTip("Which family of strategies to build; also sets sensible default filters");
    m_screenHint = new QLabel(this);
    m_screenHint->setObjectName("muted");
    m_screenHint->setWordWrap(true);
    m_bias = new QComboBox(this);
    for (Bias b : { Bias::Any, Bias::Bullish, Bias::Bearish, Bias::Neutral, Bias::Volatile }) m_bias->addItem(scan::biasName(b), static_cast<int>(b));
    m_bias->setToolTip("Keep only strategies with this directional view");
    m_universe = new QComboBox(this);
    m_universe->addItem("All stored chains", "all");
    m_universe->addItem("Watchlist", "watchlist");
    m_universe->addItem("Current ticker", "current");
    m_universe->setToolTip("Which option chains to scan (chains are the ones already held in memory)");
    m_minDays = ui::makeIntSpinBox(this, 1, 365, 20);
    m_minDays->setSuffix(" d");
    m_minDays->setToolTip("Earliest expiry to use");
    m_maxDays = ui::makeIntSpinBox(this, 2, 730, 60);
    m_maxDays->setSuffix(" d");
    m_maxDays->setToolTip("Latest expiry to use");
    m_minPop = ui::makeSpinBox(this, 0.0, 99.0, 5.0, 0, 0.0, " %");
    m_minPop->setToolTip("Minimum probability of profit at the first expiry (risk-neutral, ATM implied vol)");
    m_minRor = ui::makeSpinBox(this, 0.0, 1000.0, 5.0, 0, 0.0, " %");
    m_minRor->setToolTip("Minimum max profit / max loss. Open-ended sides are measured two expected moves away");
    m_maxSpread = ui::makeSpinBox(this, 0.0, 100.0, 5.0, 0, 25.0, " %");
    m_maxSpread->setToolTip("Widest acceptable bid-ask spread of any leg as a share of its mid (0 = ignore)");
    m_minOi = ui::makeIntSpinBox(this, 0, 100000, 0);
    m_minOi->setToolTip("Minimum open interest on every leg (0 = ignore)");
    m_definedRisk = new QCheckBox("Defined risk only", this);
    m_definedRisk->setToolTip("Skip naked short options and share-based structures");
    m_run = ui::makeButton(this, "Scan", "primary", "Build and rank ideas across the selected chains");
    m_help = ui::makeButton(this, "How to use", "secondary", "Step-by-step instructions, what each column means, and how to read the results");
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setWordWrap(true);

    auto label = [this](const QString& text) { auto* l = new QLabel(text, this); l->setObjectName("muted"); return l; };
    auto* row1 = new QHBoxLayout;
    row1->setSpacing(8);
    row1->addWidget(label("Screen"));
    row1->addWidget(m_screen);
    row1->addWidget(label("Bias"));
    row1->addWidget(m_bias);
    row1->addWidget(label("Universe"));
    row1->addWidget(m_universe);
    row1->addSpacing(12);
    row1->addWidget(label("Expiry"));
    row1->addWidget(m_minDays);
    row1->addWidget(label("to"));
    row1->addWidget(m_maxDays);
    row1->addStretch(1);
    row1->addWidget(m_help);
    row1->addWidget(m_run);
    auto* row2 = new QHBoxLayout;
    row2->setSpacing(8);
    row2->addWidget(label("Min PoP"));
    row2->addWidget(m_minPop);
    row2->addWidget(label("Min return on risk"));
    row2->addWidget(m_minRor);
    row2->addWidget(label("Max spread"));
    row2->addWidget(m_maxSpread);
    row2->addWidget(label("Min open int"));
    row2->addWidget(m_minOi);
    row2->addWidget(m_definedRisk);
    row2->addStretch(1);
    auto* controls = new QFrame(this);
    controls->setObjectName("pane");
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(12, 10, 12, 10);
    controlsLayout->setSpacing(6);
    controlsLayout->addLayout(row1);
    controlsLayout->addLayout(row2);
    controlsLayout->addWidget(m_screenHint);

    // ---- Tables ----
    auto makeTable = [this](const QStringList& headers) {
        auto* t = new QTableWidget(0, static_cast<int>(headers.size()), this);
        t->setHorizontalHeaderLabels(headers);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        t->horizontalHeader()->setStretchLastSection(true);
        t->horizontalHeader()->setObjectName("heatmapHeader");
        t->horizontalHeader()->setFixedHeight(26);
        t->verticalHeader()->setVisible(false);
        t->verticalHeader()->setDefaultSectionSize(24);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        t->setAlternatingRowColors(true);
        t->setSortingEnabled(true);
        t->setMinimumHeight(90);
        t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        return t;
    };
    m_metricsTable = makeTable({ "Ticker", "Spot", "ATM IV", "IV rank", "Far IV", "Term", "Skew 25d", "1 sd move", "P/C vol", "P/C OI", "Volume", "Open int", "Spread", "Expiry", "Ideas" });
    for (int c = 0; c < MetricColumnCount; ++c) m_metricsTable->setColumnWidth(c, c == McTicker ? 70 : (c == McExpiry ? 150 : 82));
    m_metricsTable->horizontalHeader()->setStretchLastSection(false);
    m_ideasTable = makeTable({ "Ticker", "Strategy", "Bias", "Expiry", "Legs", "Net", "Max profit", "Max loss", "PoP", "Return/risk", "Exp. P&L", "Breakevens", "Delta", "Theta/d", "IV", "Spread", "Score", "Why" });
    for (int c = 0; c < IdeaColumnCount; ++c) {
        int w = 82;
        if (c == IcTicker) w = 66; else if (c == IcStrategy) w = 140; else if (c == IcBias) w = 70; else if (c == IcExpiry) w = 120; else if (c == IcLegs) w = 300; else if (c == IcBreakevens) w = 130;
        m_ideasTable->setColumnWidth(c, w);
    }
    m_ideasTable->sortByColumn(IcScore, Qt::DescendingOrder);

    auto* metricsTitle = new QLabel("Market scan", this);
    metricsTitle->setObjectName("sectionTitle");
    auto* metricsPane = new QFrame(this);
    metricsPane->setObjectName("pane");
    auto* metricsLayout = new QVBoxLayout(metricsPane);
    metricsLayout->setContentsMargins(12, 10, 12, 10);
    metricsLayout->setSpacing(6);
    metricsLayout->addWidget(metricsTitle);
    metricsLayout->addWidget(m_metricsTable, 1);

    m_ideasTitle = new QLabel("Ideas", this);
    m_ideasTitle->setObjectName("sectionTitle");
    m_clearFilter = ui::makeButton(this, "All tickers", "secondary", "Show ideas for every scanned ticker");
    m_clearFilter->setVisible(false);
    m_openStrategy = ui::makeButton(this, "Open in Strategy", "secondary", "Load the selected idea's legs into the Strategy tab");
    m_addPortfolio = ui::makeButton(this, "Add to Portfolio", "secondary", "Book the selected idea's legs as positions at the chain mids");
    m_exportCsv = ui::makeButton(this, "Export CSV…", "secondary", "Save the ideas table");
    auto* ideasHeader = new QHBoxLayout;
    ideasHeader->setSpacing(8);
    ideasHeader->addWidget(m_ideasTitle);
    ideasHeader->addWidget(m_clearFilter);
    ideasHeader->addStretch(1);
    ideasHeader->addWidget(m_openStrategy);
    ideasHeader->addWidget(m_addPortfolio);
    ideasHeader->addWidget(m_exportCsv);
    auto* ideasPane = new QFrame(this);
    ideasPane->setObjectName("pane");
    auto* ideasLayout = new QVBoxLayout(ideasPane);
    ideasLayout->setContentsMargins(12, 10, 12, 10);
    ideasLayout->setSpacing(6);
    ideasLayout->addLayout(ideasHeader);
    ideasLayout->addWidget(m_ideasTable, 1);
    ideasLayout->addWidget(m_status);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->addWidget(metricsPane);
    m_splitter->addWidget(ideasPane);
    m_splitter->setStretchFactor(0, 2);
    m_splitter->setStretchFactor(1, 3);
    m_splitter->setChildrenCollapsible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    root->addWidget(controls);
    root->addWidget(m_splitter, 1);
}

void ScannerTab::wire()
{
    connect(m_screen, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || index >= static_cast<int>(screens().size())) return;
        const Screen& s = screens()[static_cast<size_t>(index)];
        m_screenHint->setText(s.hint);
        if (!m_updating) {
            m_updating = true;
            m_minPop->setValue(s.minProbability);
            m_minRor->setValue(s.minReturnOnRisk);
            m_definedRisk->setChecked(s.definedRiskOnly);
            m_updating = false;
            saveSettings();
        }
    });
    for (QComboBox* box : { m_bias, m_universe }) connect(box, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { if (!m_updating) saveSettings(); });
    for (QSpinBox* box : { m_minDays, m_maxDays, m_minOi }) connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { if (!m_updating) saveSettings(); });
    for (QDoubleSpinBox* box : { m_minPop, m_minRor, m_maxSpread }) connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { if (!m_updating) saveSettings(); });
    connect(m_definedRisk, &QCheckBox::toggled, this, [this](bool) { if (!m_updating) saveSettings(); });
    connect(m_run, &QPushButton::clicked, this, [this] { runScan(); });
    connect(m_help, &QPushButton::clicked, this, [this] { showHelp(); });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int) { QSettings().setValue(kSplitterKey, m_splitter->saveState()); });

    connect(m_metricsTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = m_metricsTable->selectionModel()->selectedRows();
        m_filterTicker = rows.isEmpty() ? QString() : m_metricsTable->item(rows.first().row(), McTicker)->text();
        fillIdeas();
    });
    connect(m_metricsTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        if (item && onTickerSelected) onTickerSelected(m_metricsTable->item(item->row(), McTicker)->text());
    });
    connect(m_clearFilter, &QPushButton::clicked, this, [this] { m_metricsTable->clearSelection(); m_filterTicker.clear(); fillIdeas(); });
    connect(m_ideasTable, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) { openIdea(selectedIdea()); });
    connect(m_openStrategy, &QPushButton::clicked, this, [this] {
        if (!openIdea(selectedIdea())) setStatus("Select an idea first.", ui::StatusKind::Warning);
    });
    connect(m_addPortfolio, &QPushButton::clicked, this, [this] {
        const int index = selectedIdea();
        if (index < 0 || !onAddToPortfolio) { setStatus("Select an idea first.", ui::StatusKind::Warning); return; }
        const Idea& idea = m_ideas[static_cast<size_t>(index)];
        onAddToPortfolio(QString::fromStdString(idea.ticker), idea.position);
        setStatus(QStringLiteral("%1 %2 booked in the Portfolio at the chain mids.").arg(QString::fromStdString(idea.ticker), QString::fromStdString(idea.strategy)), ui::StatusKind::Info);
    });
    connect(m_exportCsv, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, "Export ideas", QDir::homePath() + "/trade-ideas.csv", "CSV (*.csv)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { setStatus("Could not write the file.", ui::StatusKind::Error); return; }
        file.write(resultsCsv().toUtf8());
        setStatus(QStringLiteral("Exported %1 idea(s) to %2.").arg(m_ideas.size()).arg(QFileInfo(path).fileName()), ui::StatusKind::Info);
    });
}

void ScannerTab::loadSettings()
{
    QSettings settings;
    m_updating = true;
    const int screen = std::max(0, m_screen->findText(settings.value(kScreenKey, "Premium selling").toString()));
    m_screen->setCurrentIndex(screen);
    m_screenHint->setText(screens()[static_cast<size_t>(screen)].hint);
    m_bias->setCurrentIndex(std::max(0, m_bias->findText(settings.value(kBiasKey, "Any").toString())));
    m_universe->setCurrentIndex(std::max(0, m_universe->findData(settings.value(kUniverseKey, "all").toString())));
    m_minDays->setValue(settings.value(kMinDaysKey, 20).toInt());
    m_maxDays->setValue(settings.value(kMaxDaysKey, 60).toInt());
    const Screen& s = screens()[static_cast<size_t>(screen)];
    m_minPop->setValue(settings.value(kMinPopKey, s.minProbability).toDouble());
    m_minRor->setValue(settings.value(kMinRorKey, s.minReturnOnRisk).toDouble());
    m_maxSpread->setValue(settings.value(kMaxSpreadKey, 25.0).toDouble());
    m_minOi->setValue(settings.value(kMinOiKey, 0).toInt());
    m_definedRisk->setChecked(settings.value(kDefinedRiskKey, s.definedRiskOnly).toBool());
    if (settings.contains(kSplitterKey)) m_splitter->restoreState(settings.value(kSplitterKey).toByteArray());
    m_updating = false;
    setStatus("Press Scan to build ideas from the option chains in memory.", ui::StatusKind::Info);
}

void ScannerTab::saveSettings() const
{
    QSettings settings;
    settings.setValue(kScreenKey, m_screen->currentText());
    settings.setValue(kBiasKey, m_bias->currentText());
    settings.setValue(kUniverseKey, m_universe->currentData().toString());
    settings.setValue(kMinDaysKey, m_minDays->value());
    settings.setValue(kMaxDaysKey, m_maxDays->value());
    settings.setValue(kMinPopKey, m_minPop->value());
    settings.setValue(kMinRorKey, m_minRor->value());
    settings.setValue(kMaxSpreadKey, m_maxSpread->value());
    settings.setValue(kMinOiKey, m_minOi->value());
    settings.setValue(kDefinedRiskKey, m_definedRisk->isChecked());
}

void ScannerTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    fillMetrics();
    fillIdeas();
}

// MARK: - Settings as criteria

Criteria ScannerTab::criteria() const
{
    Criteria c;
    c.minDays = std::min(m_minDays->value(), m_maxDays->value());
    c.maxDays = std::max(m_minDays->value(), m_maxDays->value());
    c.targetDays = (c.minDays + c.maxDays) / 2;
    c.minProbability = m_minPop->value() / 100.0;
    c.minReturnOnRisk = m_minRor->value() / 100.0;
    c.maxSpread = m_maxSpread->value() / 100.0;
    c.minOpenInterest = m_minOi->value();
    c.bias = static_cast<Bias>(m_bias->currentData().toInt());
    c.strategies = screens()[static_cast<size_t>(std::max(0, m_screen->currentIndex()))].presets;
    c.definedRiskOnly = m_definedRisk->isChecked();
    c.maxPerTicker = 3;
    return c;
}

QStringList ScannerTab::universe() const
{
    QStringList tickers;
    const QString mode = m_universe->currentData().toString();
    if (!m_samples.empty()) {
        for (const auto& [ticker, unused] : m_samples) tickers << ticker;
    } else if (mode == "current") {
        if (!m_state.underlyingTicker.isEmpty()) tickers << m_state.underlyingTicker;
    } else if (mode == "watchlist") {
        if (watchlistProvider) tickers = watchlistProvider();
        if (m_store) tickers.erase(std::remove_if(tickers.begin(), tickers.end(), [this](const QString& t) { return !m_store->contains(t) && t != m_state.underlyingTicker; }), tickers.end());
    } else if (m_store) {
        for (const ChainStore::Summary& s : m_store->summaries()) if (s.contracts > 0) tickers << s.ticker;
        if (!m_state.underlyingTicker.isEmpty() && !tickers.contains(m_state.underlyingTicker) && !m_state.chainQuotes.empty()) tickers << m_state.underlyingTicker;
    } else if (!m_state.underlyingTicker.isEmpty()) {
        tickers << m_state.underlyingTicker;
    }
    tickers.removeDuplicates();
    tickers.sort();
    return tickers;
}

bool ScannerTab::chainFor(const QString& ticker, std::vector<ChainQuote>& quotes, double& spot) const
{
    if (const auto it = m_samples.find(ticker); it != m_samples.end()) { quotes = it->second.quotes; spot = it->second.spot; return !quotes.empty(); }
    // The live chain on screen is the freshest for the current ticker.
    if (ticker == m_state.underlyingTicker && !m_state.chainQuotes.empty()) { quotes = m_state.chainQuotes; spot = m_state.market.spot; return true; }
    if (!m_store) return false;
    const auto stored = m_store->get(ticker);
    if (!stored || stored->download.quotes.empty()) return false;
    quotes = stored->download.quotes;
    spot = stored->snapshot.price;
    return spot > 0.0;
}

ActivityMarket ScannerTab::activityMarket(const QString& ticker, double spot, const std::vector<ChainQuote>& quotes) const
{
    ActivityMarket am;
    am.model = m_state.market.model;
    am.spot = spot;
    am.dividendYield = ticker == m_state.underlyingTicker ? m_state.market.dividendYield : 0.0;
    am.rateFor = [this](double t) { return m_state.rateFor(t); };
    if (ticker != m_state.underlyingTicker) {
        // Fresher than a delayed stock print: the forward implied by near-dated put-call parity.
        const ImpliedSpotEstimate implied = impliedSpotFromParity(quotes, am, spot, {});
        if (implied.valid && std::fabs(implied.spot / spot - 1.0) < 0.05) am.spot = implied.spot;
    }
    return am;
}

// MARK: - Scanning

void ScannerTab::runScan()
{
    const QStringList tickers = universe();
    m_rows.clear();
    m_ideas.clear();
    if (tickers.isEmpty()) {
        fillMetrics();
        fillIdeas();
        setStatus("No option chains in memory for this universe. Load a ticker on the Option Chain tab or let the watchlist preload finish.", ui::StatusKind::Warning);
        return;
    }
    const Criteria c = criteria();
    QElapsedTimer timer;
    timer.start();
    int skipped = 0;
    for (const QString& ticker : tickers) {
        std::vector<ChainQuote> quotes;
        double spot = 0.0;
        if (!chainFor(ticker, quotes, spot)) { ++skipped; continue; }
        const ActivityMarket am = activityMarket(ticker, spot, quotes);
        Row row;
        row.ticker = ticker;
        row.metrics = scan::chainMetrics(quotes, am, c.targetDays, std::min(7, c.minDays), std::max(c.maxDays, 120));
        if (m_store) {
            std::vector<pricing::ivhist::Sample> history;
            for (const ChainStore::IvPoint& p : m_store->ivHistory(ticker, 300)) { pricing::ivhist::Sample s; s.date = p.date.toString(Qt::ISODate).toStdString(); s.iv30 = p.iv30; history.push_back(s); }
            const pricing::ivhist::Stats st = pricing::ivhist::stats(history, row.metrics.atmIv, 252);
            row.ivSamples = static_cast<int>(history.size());
            if (st.ok && st.samples >= 20) row.ivRank = st.rank;
        }
        if (!row.metrics.ok) { m_rows.push_back(row); continue; }
        Market base = m_state.market;
        if (ticker != m_state.underlyingTicker) base.dividends.clear();
        std::vector<Idea> ideas = scan::scanChain(ticker.toStdString(), quotes, am, base, c);
        row.ideas = static_cast<int>(ideas.size());
        m_rows.push_back(row);
        for (Idea& idea : ideas) m_ideas.push_back(std::move(idea));
    }
    std::sort(m_ideas.begin(), m_ideas.end(), [](const Idea& a, const Idea& b) { return a.score > b.score; });
    m_scannedAt = QDateTime::currentDateTime();
    m_filterTicker.clear();
    fillMetrics();
    fillIdeas();
    setStatus(QStringLiteral("Scanned %1 chain(s) in %2 ms: %3 idea(s) pass the filters%4. Double-click an idea to open it in the Strategy tab; click a ticker above to focus on it.")
                  .arg(m_rows.size()).arg(timer.elapsed()).arg(m_ideas.size()).arg(skipped ? QStringLiteral(", %1 ticker(s) without a chain skipped").arg(skipped) : QString()),
              m_ideas.empty() ? ui::StatusKind::Warning : ui::StatusKind::Info);
}

// MARK: - Tables

QString ScannerTab::fmtMoney(double v) const
{
    if (!std::isfinite(v)) return v > 0 ? QStringLiteral("∞") : QStringLiteral("−∞");
    const QString body = "$" + ui::number(std::fabs(v), 0);
    return v < 0 ? QStringLiteral("−") + body : body;
}

void ScannerTab::fillMetrics()
{
    m_metricsTable->setSortingEnabled(false);
    m_metricsTable->setRowCount(static_cast<int>(m_rows.size()));
    for (int r = 0; r < static_cast<int>(m_rows.size()); ++r) {
        const Row& row = m_rows[static_cast<size_t>(r)];
        const scan::ChainMetrics& m = row.metrics;
        auto* ticker = cell(row.ticker, Qt::AlignLeft | Qt::AlignVCenter);
        ticker->setForeground(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2));
        QFont bold = ticker->font(); bold.setBold(true); ticker->setFont(bold);
        m_metricsTable->setItem(r, McTicker, ticker);
        m_metricsTable->setItem(r, McSpot, numeric(m.spot, ui::number(m.spot, 2)));
        if (!m.ok) {
            for (int c = McIv; c < MetricColumnCount; ++c) m_metricsTable->setItem(r, c, cell("–"));
            m_metricsTable->setItem(r, McExpiry, cell("no usable quotes", Qt::AlignLeft | Qt::AlignVCenter));
            continue;
        }
        m_metricsTable->setItem(r, McIv, numeric(m.atmIv, pct(m.atmIv, 1)));
        m_metricsTable->setItem(r, McIvRank, numeric(std::isfinite(row.ivRank) ? row.ivRank : -1.0, std::isfinite(row.ivRank) ? pct(row.ivRank, 0) : QStringLiteral("–")));
        m_metricsTable->setItem(r, McFarIv, numeric(m.farAtmIv, m.farAtmIv > 0 ? pct(m.farAtmIv, 1) : "–"));
        m_metricsTable->setItem(r, McTerm, numeric(m.termSlope, m.farAtmIv > 0 ? (m.termSlope >= 0 ? "+" : "") + ui::number(m.termSlope * 100.0, 1) + " pt" : "–", true, &m_theme));
        m_metricsTable->setItem(r, McSkew, numeric(m.skew, m.putIv25 > 0 && m.callIv25 > 0 ? (m.skew >= 0 ? "+" : "") + ui::number(m.skew * 100.0, 1) + " pt" : "–"));
        m_metricsTable->setItem(r, McMove, numeric(m.expectedMove, m.expectedMove > 0 ? "±" + pct(m.expectedMove, 1) : "–"));
        m_metricsTable->setItem(r, McPcVol, numeric(m.putCallVolume, m.putCallVolume > 0 ? ui::number(m.putCallVolume, 2) : "–"));
        m_metricsTable->setItem(r, McPcOi, numeric(m.putCallOpenInterest, m.putCallOpenInterest > 0 ? ui::number(m.putCallOpenInterest, 2) : "–"));
        m_metricsTable->setItem(r, McVolume, numeric(m.totalVolume, m.totalVolume > 0 ? ui::compact(m.totalVolume) : "–"));
        m_metricsTable->setItem(r, McOi, numeric(m.totalOpenInterest, m.totalOpenInterest > 0 ? ui::compact(m.totalOpenInterest) : "–"));
        m_metricsTable->setItem(r, McSpread, numeric(m.medianSpread, m.medianSpread > 0 ? pct(m.medianSpread, 1) : "–"));
        m_metricsTable->setItem(r, McExpiry, cell(QStringLiteral("%1 (%2d)").arg(QString::fromStdString(m.expiry.expiryDate)).arg(m.expiry.daysToExpiry), Qt::AlignLeft | Qt::AlignVCenter));
        m_metricsTable->setItem(r, McIdeas, numeric(row.ideas, QString::number(row.ideas)));
    }
    m_metricsTable->setSortingEnabled(true);
}

void ScannerTab::fillIdeas()
{
    m_ideasTable->setSortingEnabled(false);
    m_ideasTable->setRowCount(0);
    int shown = 0;
    for (size_t i = 0; i < m_ideas.size(); ++i) {
        const Idea& idea = m_ideas[i];
        const QString ticker = QString::fromStdString(idea.ticker);
        if (!m_filterTicker.isEmpty() && ticker != m_filterTicker) continue;
        const int r = m_ideasTable->rowCount();
        m_ideasTable->insertRow(r);
        auto* tickerItem = cell(ticker, Qt::AlignLeft | Qt::AlignVCenter);
        tickerItem->setData(Qt::UserRole + 1, static_cast<int>(i));
        tickerItem->setForeground(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2));
        QFont bold = tickerItem->font(); bold.setBold(true); tickerItem->setFont(bold);
        m_ideasTable->setItem(r, IcTicker, tickerItem);
        m_ideasTable->setItem(r, IcStrategy, cell(QString::fromStdString(idea.strategy), Qt::AlignLeft | Qt::AlignVCenter));
        m_ideasTable->setItem(r, IcBias, cell(scan::biasName(idea.bias), Qt::AlignLeft | Qt::AlignVCenter));
        m_ideasTable->setItem(r, IcExpiry, numeric(idea.expiry.daysToExpiry, QStringLiteral("%1 (%2d)").arg(QString::fromStdString(idea.expiry.expiryDate)).arg(idea.expiry.daysToExpiry)));
        m_ideasTable->setItem(r, IcLegs, cell(QString::fromStdString(idea.legs), Qt::AlignLeft | Qt::AlignVCenter));
        m_ideasTable->setItem(r, IcNet, numeric(-idea.netPremium, (idea.netPremium < 0 ? "+" : "−") + fmtMoney(std::fabs(idea.netPremium)), true, &m_theme));
        m_ideasTable->setItem(r, IcMaxProfit, numeric(idea.unboundedProfit ? 1e18 : idea.maxProfit, idea.unboundedProfit ? "open" : fmtMoney(idea.maxProfit)));
        m_ideasTable->setItem(r, IcMaxLoss, numeric(idea.unboundedLoss ? -1e18 : idea.maxLoss, idea.unboundedLoss ? "open" : fmtMoney(idea.maxLoss)));
        m_ideasTable->setItem(r, IcPop, numeric(idea.probabilityOfProfit, pct(idea.probabilityOfProfit)));
        m_ideasTable->setItem(r, IcRor, numeric(idea.returnOnRisk, pct(idea.returnOnRisk)));
        m_ideasTable->setItem(r, IcExpected, numeric(idea.expectedPnl, (idea.expectedPnl >= 0 ? "+" : "") + fmtMoney(idea.expectedPnl), true, &m_theme));
        QStringList be;
        for (double b : idea.breakevens) be << ui::number(b, 2);
        m_ideasTable->setItem(r, IcBreakevens, cell(be.isEmpty() ? QStringLiteral("–") : be.join(" / ")));
        m_ideasTable->setItem(r, IcDelta, numeric(idea.greeks.delta, ui::number(idea.greeks.delta, 1), true, &m_theme));
        m_ideasTable->setItem(r, IcTheta, numeric(idea.greeks.theta, (idea.greeks.theta >= 0 ? "+" : "") + fmtMoney(idea.greeks.theta), true, &m_theme));
        m_ideasTable->setItem(r, IcIv, numeric(idea.atmIv, pct(idea.atmIv, 1)));
        m_ideasTable->setItem(r, IcSpread, numeric(idea.worstSpread, pct(idea.worstSpread, 1)));
        m_ideasTable->setItem(r, IcScore, numeric(idea.score, ui::number(idea.score, 2)));
        m_ideasTable->setItem(r, IcRationale, cell(QString::fromStdString(idea.rationale), Qt::AlignLeft | Qt::AlignVCenter));
        ++shown;
    }
    m_ideasTable->setSortingEnabled(true);
    m_ideasTable->sortByColumn(IcScore, Qt::DescendingOrder);
    m_ideasTitle->setText(m_filterTicker.isEmpty() ? QStringLiteral("Ideas (%1)").arg(shown) : QStringLiteral("Ideas · %1 (%2)").arg(m_filterTicker).arg(shown));
    m_clearFilter->setVisible(!m_filterTicker.isEmpty());
}

int ScannerTab::selectedIdea() const
{
    const auto rows = m_ideasTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) return -1;
    const QTableWidgetItem* item = m_ideasTable->item(rows.first().row(), IcTicker);
    return item ? item->data(Qt::UserRole + 1).toInt() : -1;
}

bool ScannerTab::openIdea(int index)
{
    if (index < 0 || index >= static_cast<int>(m_ideas.size()) || !onOpenInStrategy) return false;
    const Idea& idea = m_ideas[static_cast<size_t>(index)];
    onOpenInStrategy(QString::fromStdString(idea.ticker), idea.position);
    return true;
}

void ScannerTab::setStatus(const QString& text, ui::StatusKind kind) { ui::setStatus(m_status, text, kind); }

void ScannerTab::showHelp()
{
    ui::showHelpDialog(this, m_theme, "Trade Ideas: how to use", help::htmlFor("Trade Ideas"));
}

// MARK: - Assistant hooks

QStringList ScannerTab::screenNames() const
{
    QStringList out;
    for (const Screen& s : screens()) out << s.label;
    return out;
}

QString ScannerTab::screenName() const { return m_screen->currentText(); }

bool ScannerTab::setScreen(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    for (int i = 0; i < m_screen->count(); ++i) {
        const QString text = m_screen->itemText(i).toLower();
        if (text == wanted || text.contains(wanted) || (wanted.contains("premium") && text.contains("premium")) || (wanted.contains("credit") && text.contains("premium"))
            || (wanted.contains("debit") && text.contains("debit")) || (wanted.contains("vol") && text.contains("volatility")) || (wanted.contains("income") && text.contains("income"))
            || (wanted.contains("covered") && text.contains("income")) || (wanted == "all" && text.contains("all"))) {
            m_screen->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

bool ScannerTab::setBias(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    for (int i = 0; i < m_bias->count(); ++i) {
        const QString text = m_bias->itemText(i).toLower();
        if (text == wanted || text.startsWith(wanted) || (wanted.contains("bull") && text == "bullish") || (wanted.contains("bear") && text == "bearish")
            || (wanted.contains("neutral") && text == "neutral") || ((wanted.contains("vol") || wanted.contains("move")) && text == "volatile")) {
            m_bias->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

void ScannerTab::setDays(int minDays, int maxDays)
{
    m_updating = true;
    if (minDays > 0) m_minDays->setValue(minDays);
    if (maxDays > 0) m_maxDays->setValue(maxDays);
    m_updating = false;
    saveSettings();
}

bool ScannerTab::setUniverse(const QString& name)
{
    const QString wanted = name.trimmed().toLower();
    const int index = wanted.contains("watch") ? 1 : (wanted.contains("current") || wanted.contains("this") ? 2 : (wanted.contains("all") || wanted.contains("every") || wanted.contains("store") ? 0 : -1));
    if (index < 0) return false;
    m_universe->setCurrentIndex(index);
    return true;
}

void ScannerTab::setMinProbability(double percent) { m_minPop->setValue(std::clamp(percent, 0.0, 99.0)); }
void ScannerTab::setMinReturnOnRisk(double percent) { m_minRor->setValue(std::clamp(percent, 0.0, 1000.0)); }

QString ScannerTab::summaryText() const
{
    QString out;
    QTextStream s(&out);
    s << "Trade Ideas scanner: screen " << m_screen->currentText() << ", bias " << m_bias->currentText() << ", universe " << m_universe->currentText()
      << ", expiries " << m_minDays->value() << "-" << m_maxDays->value() << " days, min PoP " << m_minPop->value() << "%, min return on risk " << m_minRor->value()
      << "%, max spread " << m_maxSpread->value() << "%" << (m_definedRisk->isChecked() ? ", defined risk only" : "") << ".";
    if (!m_scannedAt.isValid()) { s << " Not scanned yet."; return out; }
    s << " Scanned " << m_rows.size() << " chain(s) at " << m_scannedAt.toString("HH:mm") << ": " << m_ideas.size() << " idea(s).";
    int richest = -1, cheapest = -1;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const auto& m = m_rows[static_cast<size_t>(i)].metrics;
        if (!m.ok) continue;
        if (richest < 0 || m.atmIv > m_rows[static_cast<size_t>(richest)].metrics.atmIv) richest = i;
        if (cheapest < 0 || m.atmIv < m_rows[static_cast<size_t>(cheapest)].metrics.atmIv) cheapest = i;
    }
    if (richest >= 0 && cheapest >= 0) {
        s << " Highest ATM IV " << m_rows[static_cast<size_t>(richest)].ticker << " " << pct(m_rows[static_cast<size_t>(richest)].metrics.atmIv, 1)
          << ", lowest " << m_rows[static_cast<size_t>(cheapest)].ticker << " " << pct(m_rows[static_cast<size_t>(cheapest)].metrics.atmIv, 1) << ".";
    }
    s << "\nTop ideas:";
    for (size_t i = 0; i < std::min<size_t>(5, m_ideas.size()); ++i) {
        const Idea& idea = m_ideas[i];
        s << "\n" << (i + 1) << ". " << QString::fromStdString(idea.ticker) << " " << QString::fromStdString(idea.strategy) << " " << QString::fromStdString(idea.expiry.expiryDate)
          << ": " << QString::fromStdString(idea.legs) << " — " << QString::fromStdString(idea.rationale) << " (score " << ui::number(idea.score, 2) << ")";
    }
    return out;
}

QString ScannerTab::resultsCsv() const
{
    QString out = "ticker,strategy,bias,expiry,days,legs,net_premium,max_profit,max_loss,probability_of_profit_pct,return_on_risk_pct,expected_pnl,breakevens,delta,theta_day,atm_iv_pct,worst_spread_pct,min_open_interest,score,rationale\n";
    for (const Idea& idea : m_ideas) {
        QStringList be;
        for (double b : idea.breakevens) be << QString::number(b, 'f', 2);
        out += QStringLiteral("%1,%2,%3,%4,%5,\"%6\",%7,%8,%9,%10,%11,%12,\"%13\",%14,%15,%16,%17,%18,%19,\"%20\"\n")
                   .arg(QString::fromStdString(idea.ticker), QString::fromStdString(idea.strategy), scan::biasName(idea.bias), QString::fromStdString(idea.expiry.expiryDate))
                   .arg(idea.expiry.daysToExpiry)
                   .arg(QString::fromStdString(idea.legs), QString::number(idea.netPremium, 'f', 0), idea.unboundedProfit ? QStringLiteral("inf") : QString::number(idea.maxProfit, 'f', 0),
                        idea.unboundedLoss ? QStringLiteral("-inf") : QString::number(idea.maxLoss, 'f', 0))
                   .arg(QString::number(idea.probabilityOfProfit * 100.0, 'f', 1), QString::number(idea.returnOnRisk * 100.0, 'f', 1), QString::number(idea.expectedPnl, 'f', 0), be.join(" / "))
                   .arg(QString::number(idea.greeks.delta, 'f', 1), QString::number(idea.greeks.theta, 'f', 1), QString::number(idea.atmIv * 100.0, 'f', 1), QString::number(idea.worstSpread * 100.0, 'f', 1),
                        QString::number(idea.minOpenInterest, 'f', 0), QString::number(idea.score, 'f', 3))
                   .arg(QString::fromStdString(idea.rationale));
    }
    return out;
}

QString ScannerTab::metricsCsv() const
{
    QString out = "ticker,spot,atm_iv_pct,iv_rank_pct,iv_samples,far_iv_pct,term_slope_pts,skew_25d_pts,expected_move_pct,put_call_volume,put_call_oi,volume,open_interest,median_spread_pct,expiry,days,ideas\n";
    for (const Row& row : m_rows) {
        const auto& m = row.metrics;
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17\n")
                   .arg(row.ticker, QString::number(m.spot, 'f', 2), QString::number(m.atmIv * 100.0, 'f', 1),
                        std::isfinite(row.ivRank) ? QString::number(row.ivRank * 100.0, 'f', 0) : QString(), QString::number(row.ivSamples), QString::number(m.farAtmIv * 100.0, 'f', 1),
                        QString::number(m.termSlope * 100.0, 'f', 1), QString::number(m.skew * 100.0, 'f', 1), QString::number(m.expectedMove * 100.0, 'f', 1),
                        QString::number(m.putCallVolume, 'f', 2), QString::number(m.putCallOpenInterest, 'f', 2))
                   .arg(QString::number(m.totalVolume, 'f', 0), QString::number(m.totalOpenInterest, 'f', 0), QString::number(m.medianSpread * 100.0, 'f', 1),
                        QString::fromStdString(m.expiry.expiryDate), QString::number(m.expiry.daysToExpiry), QString::number(row.ideas));
    }
    return out;
}

// MARK: - Sample data

void ScannerTab::loadSampleData()
{
    m_samples.clear();
    struct Spec { const char* ticker; double spot; double vol; double skew; double smile; double step; };
    const Spec specs[] = { { "AAPL", 336.0, 0.27, -0.25, 0.6, 5.0 }, { "NVDA", 245.0, 0.48, -0.35, 0.9, 5.0 }, { "SPY", 778.0, 0.15, -0.30, 0.4, 5.0 } };
    const QDate today = QDate::currentDate();
    for (const Spec& spec : specs) {
        ChainMarket cm;
        cm.spot = spec.spot;
        cm.riskFreeRate = m_state.market.riskFreeRate;
        std::vector<ChainQuote> quotes = syntheticChain(cm, { 24.0 / 365.0, 45.0 / 365.0, 80.0 / 365.0, 171.0 / 365.0 }, spec.vol, spec.skew, spec.smile, spec.step, 16, 0.05);
        unsigned seed = 7;
        for (ChainQuote& q : quotes) {
            q.daysToExpiry = static_cast<int>(std::lround(q.maturity * 365.0));
            q.expiryDate = today.addDays(q.daysToExpiry).toString(Qt::ISODate).toStdString();
            seed = seed * 1103515245u + 12345u;
            const double moneyness = std::fabs(q.strike / spec.spot - 1.0);
            q.openInterest = std::floor((300.0 + (seed >> 16) % 4000) * std::exp(-moneyness * 12.0));
            seed = seed * 1103515245u + 12345u;
            q.volume = std::floor(q.openInterest * (0.05 + ((seed >> 16) % 50) / 100.0));
        }
        m_samples[spec.ticker] = { quotes, spec.spot };
    }
    runScan();
}
