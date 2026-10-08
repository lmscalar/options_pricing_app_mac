//
//  HeatmapTab.cpp
//  OptionPricing
//

#include "HeatmapTab.h"
#include "Formatting.h"

#include <cmath>

using namespace pricing;

namespace {

QString expiryLabel(const ExpiryKey& key, bool multiline)
{
    const QString date = key.expiryDate.empty() ? QStringLiteral("T %1y").arg(ui::number(key.maturity, 2)) : QString::fromStdString(key.expiryDate);
    return multiline ? QStringLiteral("%1\n%2 DTE").arg(date).arg(key.daysToExpiry)
                     : QStringLiteral("%1 (%2 DTE)").arg(date).arg(key.daysToExpiry);
}

QString countText(double value)
{
    if (!std::isfinite(value)) return QStringLiteral("–");
    if (std::fabs(value) >= 1e6) return QStringLiteral("%1M").arg(QString::number(value / 1e6, 'f', 2));
    if (std::fabs(value) >= 1e4) return QStringLiteral("%1k").arg(QString::number(value / 1e3, 'f', 1));
    return QLocale(QLocale::English).toString(value, 'f', 0);
}

} // namespace

HeatmapTab::HeatmapTab(MarketState& state, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
{
    buildUi();
    wire();
    m_state.subscribe([this] { refresh(); });
    refresh();
}

// MARK: - Construction

void HeatmapTab::buildUi()
{
    m_ticker = new QLineEdit(this);
    m_ticker->setPlaceholderText("Ticker, e.g. AAPL");
    m_ticker->setMaxLength(12);
    m_ticker->setMaximumWidth(160);
    m_ticker->setToolTip("Download the chain for this ticker from Massive.com (same as the Option Chain tab)");
    m_fetch = ui::makeButton(this, "Fetch Chain", "primary", "Download every expiry for the ticker and analyse its activity");

    m_metric = new QComboBox(this);
    for (ActivityMetric m : { ActivityMetric::Volume, ActivityMetric::OpenInterest, ActivityMetric::VolumePlusOpenInterest, ActivityMetric::Turnover }) {
        m_metric->addItem(activityMetricName(m), static_cast<int>(m));
    }
    m_metric->setToolTip("What the heat and the ranking measure");
    m_side = new QComboBox(this);
    m_side->addItem("Calls + puts", static_cast<int>(SideFilter::Both));
    m_side->addItem("Calls only", static_cast<int>(SideFilter::Calls));
    m_side->addItem("Puts only", static_cast<int>(SideFilter::Puts));
    m_band = ui::makeSpinBox(this, 0.0, 95.0, 5.0, 0, 25.0, " %");
    m_band->setSpecialValueText("all strikes");
    m_band->setToolTip("Strike band around spot shown in the heatmap (0 = all strikes)");
    m_topCount = ui::makeIntSpinBox(this, 5, 200, 25);
    m_topCount->setToolTip("How many contracts to list in the most-active table");

    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setWordWrap(true);

    auto* controlsBox = new QGroupBox("Activity", this);
    auto* controls = new QGridLayout(controlsBox);
    controls->setHorizontalSpacing(12);
    controls->setVerticalSpacing(8);
    controls->addWidget(new QLabel("Ticker", controlsBox), 0, 0);
    controls->addWidget(m_ticker, 0, 1);
    controls->addWidget(m_fetch, 0, 2);
    controls->addWidget(new QLabel("Metric", controlsBox), 0, 3);
    controls->addWidget(m_metric, 0, 4);
    controls->addWidget(new QLabel("Side", controlsBox), 0, 5);
    controls->addWidget(m_side, 0, 6);
    controls->addWidget(new QLabel("Strike band", controlsBox), 0, 7);
    controls->addWidget(m_band, 0, 8);
    controls->addWidget(new QLabel("Top", controlsBox), 0, 9);
    controls->addWidget(m_topCount, 0, 10);
    controls->addWidget(m_status, 1, 0, 1, 11);
    controls->setColumnStretch(4, 1);

    // Summary cards
    m_cardVolume = ui::makeCard(this, "VOLUME (C / P)");
    m_cardOpenInterest = ui::makeCard(this, "OPEN INTEREST (C / P)");
    m_cardPutCall = ui::makeCard(this, "PUT / CALL RATIO");
    m_cardBusiestExpiry = ui::makeCard(this, "MOST ACTIVE EXPIRY");
    m_cardBusiestStrike = ui::makeCard(this, "MOST ACTIVE STRIKE");
    m_cardParity = ui::makeCard(this, "PUT-CALL PARITY");
    m_cardPutCall.frame->setToolTip("Put volume divided by call volume (open interest ratio in the subtitle)");
    m_cardParity.frame->setToolTip("Mean absolute parity gap across all strike pairs, in dollars per share");
    auto* cards = new QHBoxLayout;
    cards->setSpacing(10);
    for (ui::Card* card : { &m_cardVolume, &m_cardOpenInterest, &m_cardPutCall, &m_cardBusiestExpiry, &m_cardBusiestStrike, &m_cardParity }) {
        cards->addWidget(card->frame, 1);
    }

    // Heatmap grid
    m_heatmap = new QTableWidget(this);
    m_heatmap->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_heatmap->setSelectionMode(QAbstractItemView::SingleSelection);
    // Columns are user-resizable (drag the header divider, double-click it to fit) with a
    // default wide enough for a two-line date / DTE label; extra columns scroll horizontally.
    m_heatmap->horizontalHeader()->setObjectName("heatmapHeader");
    m_heatmap->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_heatmap->horizontalHeader()->setDefaultSectionSize(92);
    m_heatmap->horizontalHeader()->setMinimumSectionSize(44);
    m_heatmap->horizontalHeader()->setStretchLastSection(false);
    m_heatmap->horizontalHeader()->setMinimumHeight(38);
    m_heatmap->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_heatmap->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_heatmap->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_heatmap->verticalHeader()->setObjectName("heatmapHeader");
    m_heatmap->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_heatmap->verticalHeader()->setDefaultSectionSize(24);
    m_heatmap->verticalHeader()->setMinimumWidth(64);
    m_heatmap->setAlternatingRowColors(false);
    m_heatmap->setMinimumHeight(160);
    m_heatmap->setToolTip("Rows are strikes, columns are expiries. Double-click a cell to price that strike and expiry.");
    m_toggleTables = ui::makeButton(this, "Hide Tables", "secondary", "Give the heatmap the whole tab, or bring the tables back");
    auto* heatHeader = new QHBoxLayout;
    auto* heatTitle = new QLabel("Activity by strike and expiry", this);
    heatTitle->setObjectName("columnHeader");
    heatHeader->addWidget(heatTitle, 1);
    heatHeader->addWidget(m_toggleTables);

    // Most active table
    m_active = new QTableWidget(0, 12, this);
    m_active->setHorizontalHeaderLabels({ "#", "Expiry", "DTE", "Strike", "Type", "Volume", "Open Int", "Vol/OI", "Mid", "IV", "Share", "Parity Gap" });
    m_active->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_active->horizontalHeader()->setStretchLastSection(true);
    m_active->horizontalHeader()->setMinimumSectionSize(36);
    m_active->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_active->verticalHeader()->setVisible(false);
    m_active->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_active->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_active->setAlternatingRowColors(true);
    m_active->setToolTip("Most active contracts. Double-click to send one to the Pricer.");
    auto* activeBox = new QGroupBox("Most active contracts", this);
    auto* activeLayout = new QVBoxLayout(activeBox);
    activeLayout->addWidget(m_active);

    // Parity table
    m_parityTable = new QTableWidget(0, 10, this);
    m_parityTable->setHorizontalHeaderLabels({ "Expiry", "DTE", "Pairs", "Implied Fwd", "Model Fwd", "Fwd Gap", "Implied Yield", "Mean |Gap|", "Worst Gap", "P/C Volume" });
    m_parityTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_parityTable->horizontalHeader()->setStretchLastSection(true);
    m_parityTable->horizontalHeader()->setMinimumSectionSize(36);
    m_parityTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_parityTable->setWordWrap(false);
    m_parityTable->verticalHeader()->setVisible(false);
    m_parityTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_parityTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_parityTable->setAlternatingRowColors(true);
    m_parityTable->setToolTip("Put-call parity by expiry. Implied forward is the median of K + (C − P)·e^(rT) across strike pairs; "
                              "the implied yield is the dividend yield that would reconcile it with spot.");
    auto* parityBox = new QGroupBox("Put-call parity by expiry", this);
    auto* parityLayout = new QVBoxLayout(parityBox);
    parityLayout->addWidget(m_parityTable);

    auto* tables = new QHBoxLayout;
    tables->setSpacing(12);
    tables->addWidget(activeBox, 3);
    tables->addWidget(parityBox, 2);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(10);
    m_splitter->setChildrenCollapsible(false);
    auto* upper = new QWidget(m_splitter);
    auto* upperLayout = new QVBoxLayout(upper);
    upperLayout->setContentsMargins(0, 0, 0, 0);
    upperLayout->setSpacing(6);
    upperLayout->addLayout(heatHeader);
    upperLayout->addWidget(m_heatmap, 1);
    m_lowerPane = new QWidget(m_splitter);
    auto* lowerLayout = new QVBoxLayout(m_lowerPane);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    lowerLayout->addLayout(tables);
    m_lowerPane->setMinimumHeight(120);
    m_active->setMinimumHeight(80);
    m_parityTable->setMinimumHeight(80);
    m_splitter->addWidget(upper);
    m_splitter->addWidget(m_lowerPane);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, true);   // tables may collapse in a short window; the grid never does
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setSizes({ 420, 320 });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);
    root->addWidget(controlsBox);
    root->addLayout(cards);
    root->addWidget(m_splitter, 1);
}

void HeatmapTab::wire()
{
    auto changed = [this] { if (!m_updating) refresh(); };
    connect(m_metric, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed](int) { changed(); });
    connect(m_side, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed](int) { changed(); });
    connect(m_band, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(m_topCount, qOverload<int>(&QSpinBox::valueChanged), this, [changed](int) { changed(); });
    auto fetch = [this] {
        const QString symbol = m_ticker->text().trimmed().toUpper();
        if (symbol.isEmpty()) {
            ui::setStatus(m_status, "Enter a ticker symbol first.", ui::StatusKind::Error);
            return;
        }
        if (onRequestFetch) onRequestFetch(symbol);
    };
    connect(m_fetch, &QPushButton::clicked, this, fetch);
    connect(m_ticker, &QLineEdit::returnPressed, this, fetch);
    connect(m_toggleTables, &QPushButton::clicked, this, [this] {
        const bool hide = m_lowerPane->isVisible();
        m_lowerPane->setVisible(!hide);
        m_toggleTables->setText(hide ? "Show Tables" : "Hide Tables");
    });
    connect(m_heatmap, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (!onSendToPricer || row < 0 || col < 0 || static_cast<size_t>(row) >= m_grid.strikes.size() || static_cast<size_t>(col) >= m_grid.expiries.size()) return;
        const ExpiryKey& key = m_grid.expiries[static_cast<size_t>(col)];
        onSendToPricer(m_grid.strikes[static_cast<size_t>(row)], key.maturity, 0.0, QString::fromStdString(key.expiryDate));
    });
    connect(m_active, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (!onSendToPricer || row < 0 || static_cast<size_t>(row) >= m_topContracts.size()) return;
        const ActiveContract& c = m_topContracts[static_cast<size_t>(row)];
        onSendToPricer(c.quote.strike, c.quote.maturity, c.impliedVol, QString::fromStdString(c.quote.expiryDate));
    });
}

// MARK: - Data

ActivityMetric HeatmapTab::metric() const
{
    return static_cast<ActivityMetric>(m_metric->currentData().toInt());
}

SideFilter HeatmapTab::side() const
{
    return static_cast<SideFilter>(m_side->currentData().toInt());
}

ActivityMarket HeatmapTab::activityMarket() const
{
    ActivityMarket m;
    m.model = m_state.market.model;
    m.spot = m_state.market.spot;
    m.dividendYield = m_state.market.dividendYield;
    m.rateFor = [this](double maturity) { return m_state.rateFor(maturity); };
    return m;
}

void HeatmapTab::refresh()
{
    const std::vector<ChainQuote>& quotes = m_state.chainQuotes;
    bool anyActivity = false;
    for (const ChainQuote& q : quotes) {
        if (q.volume > 0.0 || q.openInterest > 0.0) { anyActivity = true; break; }
    }

    if (quotes.empty() || !anyActivity) {
        m_grid = ActivityGrid();
        m_topContracts.clear();
        m_parity.clear();
        m_summary = ActivitySummary();
        m_heatmap->clear();
        m_heatmap->setRowCount(0);
        m_heatmap->setColumnCount(0);
        m_active->setRowCount(0);
        m_parityTable->setRowCount(0);
        for (ui::Card* card : { &m_cardVolume, &m_cardOpenInterest, &m_cardPutCall, &m_cardBusiestExpiry, &m_cardBusiestStrike, &m_cardParity }) {
            card->value->setText("–");
            card->subtitle->clear();
        }
        ui::setStatus(m_status, quotes.empty()
                                    ? "No chain loaded. Enter a ticker and fetch, or load a chain on the Option Chain tab."
                                    : "The loaded chain has no volume or open interest (synthetic and most CSV chains do not). Fetch a live chain to see activity.",
                      ui::StatusKind::Info);
        return;
    }

    const ActivityMarket market = activityMarket();
    m_grid = buildActivityGrid(quotes, metric(), side(), m_state.market.spot, m_band->value() / 100.0);
    m_topContracts = mostActiveContracts(quotes, metric(), side(), market, static_cast<size_t>(m_topCount->value()));
    m_parity = parityByExpiry(quotes, market);
    m_summary = summarizeActivity(quotes, metric(), side());

    fillGrid();
    fillMostActive();
    fillParity();
    fillSummary();

    ui::setStatus(m_status, QStringLiteral("%1 contracts across %2 expir%3 at spot %4. Heat shows %5 for %6; the ranking and parity use the whole chain.")
                                .arg(m_summary.contracts).arg(m_parity.size()).arg(m_parity.size() == 1 ? "y" : "ies")
                                .arg(ui::number(m_state.market.spot, 2), QString(activityMetricName(metric())).toLower(), m_side->currentText().toLower()),
                  ui::StatusKind::Info);
}

void HeatmapTab::fillGrid()
{
    m_heatmap->clear();
    const int rows = static_cast<int>(m_grid.strikes.size());
    const int cols = static_cast<int>(m_grid.expiries.size());
    m_heatmap->setRowCount(rows);
    m_heatmap->setColumnCount(cols);
    QStringList headers;
    for (const ExpiryKey& key : m_grid.expiries) headers << expiryLabel(key, true);
    m_heatmap->setHorizontalHeaderLabels(headers);
    QStringList rowHeaders;
    for (double strike : m_grid.strikes) rowHeaders << ui::number(strike, 2);
    m_heatmap->setVerticalHeaderLabels(rowHeaders);

    const QColor base(m_theme.surface);
    const QColor hot(m_theme.accent);
    const double spot = m_state.market.spot;
    // Highlight the row nearest to spot.
    int spotRow = -1;
    double bestDistance = INFINITY;
    for (int r = 0; r < rows; ++r) {
        const double d = std::fabs(m_grid.strikes[static_cast<size_t>(r)] - spot);
        if (d < bestDistance) { bestDistance = d; spotRow = r; }
    }
    const bool turnover = metric() == ActivityMetric::Turnover;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const double v = m_grid.cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
            const int n = m_grid.contracts[static_cast<size_t>(r)][static_cast<size_t>(c)];
            QTableWidgetItem* item = ui::makeCell(n == 0 ? QString() : (turnover ? ui::number(v, 2) : countText(v)), Qt::AlignCenter);
            // Square-root scaling keeps mid-sized cells visible next to the busiest strike.
            const double t = m_grid.maxValue > 0.0 ? std::sqrt(v / m_grid.maxValue) : 0.0;
            const QColor background = blend(base, hot, 0.85 * t);
            item->setBackground(QBrush(background));
            item->setForeground(QBrush(QColor(background.lightnessF() < 0.45 ? "#f8fafc" : m_theme.textStrong)));
            if (n > 0) {
                item->setToolTip(QStringLiteral("Strike %1 · %2\n%3: %4 from %5 contract%6")
                                     .arg(ui::number(m_grid.strikes[static_cast<size_t>(r)], 2), expiryLabel(m_grid.expiries[static_cast<size_t>(c)], false),
                                          activityMetricName(metric()), turnover ? ui::number(v, 3) : QLocale(QLocale::English).toString(v, 'f', 0))
                                     .arg(n).arg(n == 1 ? "" : "s"));
            }
            m_heatmap->setItem(r, c, item);
        }
        if (r == spotRow) {
            if (QTableWidgetItem* header = m_heatmap->verticalHeaderItem(r)) {
                header->setForeground(QBrush(QColor(m_theme.accent)));
                header->setToolTip(QStringLiteral("Nearest strike to spot %1").arg(ui::number(spot, 2)));
            }
        }
    }
    // Open the view centred on the at-the-money row rather than the highest strike.
    if (spotRow >= 0 && cols > 0) {
        if (QTableWidgetItem* anchor = m_heatmap->item(spotRow, 0)) {
            m_heatmap->scrollToItem(anchor, QAbstractItemView::PositionAtCenter);
        }
    }
}

void HeatmapTab::fillMostActive()
{
    m_active->setRowCount(static_cast<int>(m_topContracts.size()));
    int row = 0;
    for (const ActiveContract& c : m_topContracts) {
        const ChainQuote& q = c.quote;
        const QString expiry = q.expiryDate.empty() ? QStringLiteral("T %1y").arg(ui::number(q.maturity, 3)) : QString::fromStdString(q.expiryDate);
        const int dte = q.daysToExpiry > 0 ? q.daysToExpiry : static_cast<int>(std::lround(q.maturity * 365.0));
        auto* typeItem = ui::makeCell(q.type == OptionType::Call ? "Call" : "Put", Qt::AlignCenter);
        typeItem->setForeground(QBrush(QColor(q.type == OptionType::Call ? m_theme.call : m_theme.put)));
        auto* gapItem = ui::makeCell(std::isfinite(c.parityGap) ? ui::signedMoney(c.parityGap, 2) : QStringLiteral("no pair"));
        if (std::isfinite(c.parityGap)) {
            gapItem->setToolTip(QStringLiteral("C − P − (F − K)·e^(−rT) using the opposite-type mid %1").arg(ui::number(c.counterpartMid, 2)));
            if (std::fabs(c.parityGap) > 0.02 * std::max(1.0, q.mid)) gapItem->setForeground(QBrush(QColor(m_theme.warning)));
        }
        const QList<QTableWidgetItem*> items = {
            ui::makeCell(QString::number(row + 1), Qt::AlignCenter),
            ui::makeCell(expiry, Qt::AlignCenter),
            ui::makeCell(QString::number(dte)),
            ui::makeCell(ui::number(q.strike, 2)),
            typeItem,
            ui::makeCell(countText(q.volume)),
            ui::makeCell(countText(q.openInterest)),
            ui::makeCell(q.openInterest > 0 ? ui::number(q.volume / q.openInterest, 2) : QStringLiteral("–")),
            ui::makeCell(ui::number(q.mid, 2)),
            ui::makeCell(c.impliedVol > 0 ? ui::percent(c.impliedVol, 1) : QStringLiteral("–")),
            ui::makeCell(ui::percent(c.shareOfTotal, 1)),
            gapItem,
        };
        for (int col = 0; col < items.size(); ++col) m_active->setItem(row, col, items[col]);
        ++row;
    }
    m_active->resizeColumnsToContents();
    m_active->resizeRowsToContents();
}

void HeatmapTab::fillParity()
{
    m_parityTable->setRowCount(static_cast<int>(m_parity.size()));
    int row = 0;
    for (const ParityRow& p : m_parity) {
        auto* gapItem = ui::makeCell(p.pairs > 0 ? ui::signedMoney(p.forwardGap, 2) : QStringLiteral("–"));
        if (p.pairs > 0 && std::fabs(p.forwardGap) > 0.005 * m_state.market.spot) gapItem->setForeground(QBrush(QColor(m_theme.warning)));
        auto* worstItem = ui::makeCell(p.pairs > 0 ? QStringLiteral("%1 @ %2").arg(ui::signedMoney(p.worstGap, 2), ui::number(p.worstGapStrike, 2)) : QStringLiteral("–"));
        const QList<QTableWidgetItem*> items = {
            ui::makeCell(p.expiry.expiryDate.empty() ? QStringLiteral("T %1y").arg(ui::number(p.expiry.maturity, 2)) : QString::fromStdString(p.expiry.expiryDate), Qt::AlignCenter),
            ui::makeCell(QString::number(p.expiry.daysToExpiry)),
            ui::makeCell(QString::number(p.pairs)),
            ui::makeCell(p.pairs > 0 ? ui::number(p.impliedForward, 2) : QStringLiteral("–")),
            ui::makeCell(ui::number(p.modelForward, 2)),
            gapItem,
            ui::makeCell(p.pairs > 0 && p.expiry.daysToExpiry >= 7 ? ui::percent(p.impliedYield, 2) : QStringLiteral("–")),
            ui::makeCell(p.pairs > 0 ? ui::number(p.meanAbsGap, 3) : QStringLiteral("–")),
            worstItem,
            ui::makeCell(p.callVolume > 0 ? ui::number(p.putVolume / p.callVolume, 2) : QStringLiteral("–")),
        };
        for (int col = 0; col < items.size(); ++col) m_parityTable->setItem(row, col, items[col]);
        ++row;
    }
    m_parityTable->resizeColumnsToContents();
    m_parityTable->resizeRowsToContents();
}

void HeatmapTab::fillSummary()
{
    const ActivitySummary& s = m_summary;
    m_cardVolume.value->setText(QStringLiteral("%1 / %2").arg(countText(s.callVolume), countText(s.putVolume)));
    m_cardVolume.subtitle->setText(QStringLiteral("total %1 contracts traded").arg(countText(s.callVolume + s.putVolume)));
    m_cardOpenInterest.value->setText(QStringLiteral("%1 / %2").arg(countText(s.callOpenInterest), countText(s.putOpenInterest)));
    m_cardOpenInterest.subtitle->setText(QStringLiteral("total %1 open").arg(countText(s.callOpenInterest + s.putOpenInterest)));
    m_cardPutCall.value->setText(ui::number(s.putCallVolumeRatio, 2));
    m_cardPutCall.subtitle->setText(QStringLiteral("by volume · %1 by open interest").arg(ui::number(s.putCallOpenInterestRatio, 2)));
    m_cardPutCall.value->setObjectName(s.putCallVolumeRatio > 1.0 ? "lossValue" : (s.putCallVolumeRatio > 0 ? "profitValue" : "bigValue"));
    ui::restyle(m_cardPutCall.value);

    const bool turnover = metric() == ActivityMetric::Turnover;
    m_cardBusiestExpiry.value->setText(s.busiestExpiryValue > 0 ? QString::fromStdString(s.busiestExpiry.expiryDate.empty()
                                                                      ? std::string("T ") + ui::number(s.busiestExpiry.maturity, 2).toStdString() + "y"
                                                                      : s.busiestExpiry.expiryDate)
                                                              : QStringLiteral("–"));
    m_cardBusiestExpiry.subtitle->setText(s.busiestExpiryValue > 0
                                              ? QStringLiteral("%1 DTE · %2 %3").arg(s.busiestExpiry.daysToExpiry).arg(turnover ? ui::number(s.busiestExpiryValue, 2) : countText(s.busiestExpiryValue), QString(activityMetricName(metric())).toLower())
                                              : QString());
    m_cardBusiestStrike.value->setText(s.busiestStrikeValue > 0 ? ui::number(s.busiestStrike, 2) : QStringLiteral("–"));
    m_cardBusiestStrike.subtitle->setText(s.busiestStrikeValue > 0
                                              ? QStringLiteral("%1 %2 across all expiries").arg(turnover ? ui::number(s.busiestStrikeValue, 2) : countText(s.busiestStrikeValue), QString(activityMetricName(metric())).toLower())
                                              : QString());

    double gapSum = 0.0;
    int pairs = 0;
    for (const ParityRow& p : m_parity) {
        gapSum += p.meanAbsGap * p.pairs;
        pairs += p.pairs;
    }
    m_cardParity.value->setText(pairs > 0 ? ui::money(gapSum / pairs, 3) : QStringLiteral("–"));
    m_cardParity.subtitle->setText(pairs > 0 ? QStringLiteral("mean |C − P − (F − K)e^(−rT)| over %1 strike pairs").arg(pairs) : QStringLiteral("no call/put pairs"));
}

// MARK: - Theme, export

void HeatmapTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    refresh();
}

QString HeatmapTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    s << "rank,expiry_date,dte,strike,type,volume,open_interest,mid,implied_vol,share,parity_gap\n";
    int rank = 1;
    for (const ActiveContract& c : m_topContracts) {
        s << rank++ << "," << QString::fromStdString(c.quote.expiryDate) << "," << c.quote.daysToExpiry << "," << c.quote.strike << ","
          << (c.quote.type == OptionType::Call ? "call" : "put") << "," << c.quote.volume << "," << c.quote.openInterest << ","
          << c.quote.mid << "," << c.impliedVol << "," << c.shareOfTotal << "," << (std::isfinite(c.parityGap) ? QString::number(c.parityGap) : QString()) << "\n";
    }
    s << "\nexpiry_date,dte,pairs,implied_forward,model_forward,forward_gap,implied_yield,mean_abs_gap,worst_gap,worst_gap_strike,call_volume,put_volume,call_open_interest,put_open_interest\n";
    for (const ParityRow& p : m_parity) {
        s << QString::fromStdString(p.expiry.expiryDate) << "," << p.expiry.daysToExpiry << "," << p.pairs << "," << p.impliedForward << ","
          << p.modelForward << "," << p.forwardGap << "," << p.impliedYield << "," << p.meanAbsGap << "," << p.worstGap << "," << p.worstGapStrike << ","
          << p.callVolume << "," << p.putVolume << "," << p.callOpenInterest << "," << p.putOpenInterest << "\n";
    }
    s << "\nstrike";
    for (const ExpiryKey& key : m_grid.expiries) s << "," << QString::fromStdString(key.expiryDate.empty() ? std::to_string(key.maturity) : key.expiryDate);
    s << "\n";
    for (size_t r = 0; r < m_grid.strikes.size(); ++r) {
        s << m_grid.strikes[r];
        for (double v : m_grid.cells[r]) s << "," << v;
        s << "\n";
    }
    return out;
}
