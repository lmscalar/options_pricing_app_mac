//
//  ChainTab.cpp
//  OptionPricing
//

#include "ChainTab.h"
#include "Formatting.h"
#include "../Pricing/Activity.h"

#include <cmath>
#include <map>
#include <memory>
#include <string>

using namespace pricing;

namespace {
enum Column { ColStrike = 0, ColCallBid, ColCallAsk, ColCallMid, ColCallIv, ColFitIv, ColPutBid, ColPutAsk, ColPutMid, ColPutIv, ColFlags, ColumnCount };
}

ChainTab::ChainTab(MarketState& state, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
{
    buildUi();
    wire();
    refreshKeyStatus();
    m_state.subscribe([this] { rebuild(); });
    rebuild();
}

void ChainTab::buildUi()
{
    // Live data row
    m_ticker = new QLineEdit(this);
    m_ticker->setPlaceholderText("Ticker, e.g. AAPL");
    m_ticker->setMaxLength(12);
    m_ticker->setToolTip("Underlying symbol to download from Massive.com");
    m_ticker->setMaximumWidth(160);
    m_expiryCount = ui::makeIntSpinBox(this, 0, 60, 0);
    m_expiryCount->setSpecialValueText("All");
    m_expiryCount->setToolTip("Download every listed expiry (All) or only the N nearest ones");
    m_fetch = ui::makeButton(this, "Fetch Live Chain", "primary", "Download the underlying price and option chain from Massive.com");
    m_treasury = ui::makeButton(this, "Treasury Curve", "secondary", "Load the latest US Treasury yields into the rate curve and enable it");
    m_dividends = ui::makeButton(this, "Dividends", "secondary", "Project the ticker's regular cash dividends into the Pricer's dividend schedule");
    m_setKey = ui::makeButton(this, "Set Key…", "secondary", "Enter a Massive.com API key");
    m_keyStatus = new QLabel(this);
    m_keyStatus->setObjectName("muted");
    m_keyStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_keyStatus->setMinimumWidth(120);
    m_keyStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_liveStatus = new QLabel(this);
    m_liveStatus->setObjectName("muted");
    // One line: long messages are elided, the full text is the tooltip (see setLiveBusy / finishLive).
    m_liveStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* liveBox = new QGroupBox("Live Data (Massive.com)", this);
    auto* live = new QGridLayout(liveBox);
    live->setHorizontalSpacing(12);
    live->setVerticalSpacing(8);
    live->addWidget(new QLabel("Ticker", liveBox), 0, 0);
    live->addWidget(m_ticker, 0, 1);
    live->addWidget(new QLabel("Expiries", liveBox), 0, 2);
    live->addWidget(m_expiryCount, 0, 3);
    live->addWidget(m_fetch, 0, 4);
    live->addWidget(m_treasury, 0, 5);
    live->addWidget(m_dividends, 0, 6);
    live->addWidget(m_keyStatus, 0, 7, Qt::AlignRight);
    live->addWidget(m_setKey, 0, 8);
    m_autoRefresh = new QCheckBox("Auto-refresh", liveBox);
    m_autoRefresh->setToolTip("Re-download the chain and the underlying price on the intervals to the right while this stays checked");
    m_chainInterval = ui::makeIntSpinBox(liveBox, 30, 3600, 120);
    m_chainInterval->setSuffix(" s");
    m_chainInterval->setToolTip("Seconds between chain downloads (a full chain is about a dozen requests)");
    m_spotInterval = ui::makeIntSpinBox(liveBox, 15, 3600, 60);
    m_spotInterval->setSuffix(" s");
    m_spotInterval->setToolTip("Seconds between underlying price checks. The vendor stock feed itself may be delayed; see the spot label.");
    m_useImpliedSpot = new QCheckBox("Underlying from option parity (near real-time)", liveBox);
    m_useImpliedSpot->setChecked(true);
    m_useImpliedSpot->setToolTip("Option trades are disseminated with far less delay than the stock quote on delayed plans. "
                                 "When checked, the spot is implied from put-call parity on the nearest liquid expiry; "
                                 "otherwise the vendor's delayed stock price is used.");
    live->addWidget(m_autoRefresh, 1, 0, 1, 2);
    live->addWidget(new QLabel("chain every", liveBox), 1, 2, Qt::AlignRight);
    live->addWidget(m_chainInterval, 1, 3);
    live->addWidget(new QLabel("spot every", liveBox), 1, 4, Qt::AlignRight);
    live->addWidget(m_spotInterval, 1, 5);
    live->addWidget(m_useImpliedSpot, 1, 6, 1, 3);
    live->addWidget(m_liveStatus, 2, 0, 1, 9);
    live->setColumnStretch(7, 1);

    m_chainTimer = new QTimer(this);
    m_spotTimer = new QTimer(this);

    // File / sample row
    m_import = ui::makeButton(this, "Import CSV…", "secondary",
                              "Load a chain with columns: strike, type, bid, ask (or mid/last) and an expiration date, or expiry in years or days");
    m_sample = ui::makeButton(this, "Generate Sample Chain", "secondary", "Create a synthetic chain with skew and smile for exploration");
    m_clear = ui::makeButton(this, "Clear", "secondary", "Remove the loaded chain");
    m_defaultMaturity = ui::makeSpinBox(this, 0.001, 20.0, 0.25, 3, 0.25, " yrs");
    m_defaultMaturity->setToolTip("Expiry assumed for imported CSV rows that do not carry one");
    m_expiry = new QComboBox(this);
    m_expiry->setToolTip("Expiry slice to display");
    // Size to a fixed character count rather than the longest entry, so a long label
    // never widens the whole window.
    m_expiry->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_expiry->setMinimumContentsLength(30);
    m_expiry->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_useAtm = ui::makeButton(this, "Use ATM Vol as σ", "secondary", "Copy this expiry's at-the-money fitted volatility into the market inputs");
    m_useAtm->setEnabled(false);

    m_marketInfo = new QLabel(this);
    m_marketInfo->setObjectName("muted");
    m_marketInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_summary = new QLabel(this);
    m_summary->setObjectName("muted");
    m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* controlsBox = new QGroupBox("Chain", this);
    auto* controls = new QGridLayout(controlsBox);
    controls->setHorizontalSpacing(12);
    controls->setVerticalSpacing(8);
    controls->addWidget(m_import, 0, 0);
    controls->addWidget(m_sample, 0, 1);
    controls->addWidget(m_clear, 0, 2);
    controls->addWidget(new QLabel("CSV expiry", controlsBox), 0, 3);
    controls->addWidget(m_defaultMaturity, 0, 4);
    controls->addWidget(new QLabel("Show expiry", controlsBox), 0, 5);
    controls->addWidget(m_expiry, 0, 6);
    controls->addWidget(m_useAtm, 0, 7);
    // Strike range filter for the table and smile chart.
    m_strikeMode = new QComboBox(controlsBox);
    m_strikeMode->addItem("All strikes", 0);
    m_strikeMode->addItem("± % around forward", 1);
    m_strikeMode->addItem("Custom range", 2);
    m_strikeMode->setCurrentIndex(1);
    m_strikeMode->setToolTip("Which strikes to show in the table and smile chart. The SVI fit always uses every quote.");
    m_strikePercent = ui::makeSpinBox(controlsBox, 1.0, 95.0, 5.0, 0, 20.0, " %");
    m_strikePercent->setToolTip("Show strikes within this percentage of the forward price");
    m_strikeMin = ui::makeSpinBox(controlsBox, 0.0, 1'000'000.0, 1.0, 2, 0.0);
    m_strikeMin->setToolTip("Lowest strike to show");
    m_strikeMax = ui::makeSpinBox(controlsBox, 0.0, 1'000'000.0, 1.0, 2, 0.0);
    m_strikeMax->setSpecialValueText("no limit");
    m_strikeMax->setToolTip("Highest strike to show (0 = no limit)");
    m_strikeToLabel = new QLabel("to", controlsBox);
    controls->addWidget(new QLabel("Strikes", controlsBox), 1, 0);
    controls->addWidget(m_strikeMode, 1, 1, 1, 2);
    controls->addWidget(m_strikePercent, 1, 3);
    controls->addWidget(m_strikeMin, 1, 4);
    controls->addWidget(m_strikeToLabel, 1, 5, Qt::AlignCenter);
    controls->addWidget(m_strikeMax, 1, 6);
    controls->addWidget(m_marketInfo, 2, 0, 1, 4);
    controls->addWidget(m_summary, 2, 4, 1, 4);
    controls->setColumnStretch(6, 1);

    // One compact line above the table: slice title · logo · price and change · provenance · Hide Charts.
    // Every text label is single-line and elides; the full provenance is in the tooltips.
    m_tableTitle = new QLabel(this);
    m_tableTitle->setObjectName("columnHeader");
    m_tableTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_tableTitle->setMinimumWidth(160);
    m_toggleCharts = ui::makeButton(this, "Hide Charts", "secondary", "Give the strike table the whole tab, or bring the charts back");
    m_spotLabel = new QLabel(this);
    m_spotLabel->setObjectName("spotFlat");
    m_spotLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_spotLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_spotLabel->setToolTip("Current underlying price used to imply volatilities. Highlighted rows bracket this price.");
    m_spotDetail = new QLabel(this);
    m_spotDetail->setObjectName("muted");
    m_spotDetail->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);   // clips its tail, not its start, when squeezed
    m_spotDetail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_spotDetail->setMinimumWidth(120);
    m_logoLabel = new QLabel(this);
    m_logoLabel->setFixedSize(22, 22);
    m_logoLabel->setAlignment(Qt::AlignCenter);
    m_logoLabel->setVisible(false);
    auto* tableHeader = new QHBoxLayout;
    tableHeader->setContentsMargins(0, 0, 0, 0);
    tableHeader->setSpacing(8);
    tableHeader->addWidget(m_tableTitle, 3);
    tableHeader->addWidget(m_logoLabel);
    tableHeader->addWidget(m_spotLabel);
    tableHeader->addWidget(m_spotDetail, 2);
    tableHeader->addWidget(m_toggleCharts);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Strike", "Call Bid", "Call Ask", "Call Mid", "Call IV", "Fit IV", "Put Bid", "Put Ask", "Put Mid", "Put IV", "Flags" });
    // Columns are user-resizable (drag a divider, double-click to fit) and the last one
    // absorbs leftover width. The header is part of the table frame, so it stays fixed
    // while rows scroll and while the splitter changes the table height.
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setDefaultSectionSize(104);
    m_table->horizontalHeader()->setMinimumSectionSize(48);
    m_table->horizontalHeader()->setVisible(true);
    m_table->horizontalHeader()->setFixedHeight(30);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setToolTip("Double-click a strike to send it to the Pricer tab with its implied volatility");
    m_table->setMinimumHeight(140);   // header plus a few rows; drag the grip or hide the charts for more

    m_smileChart = new QChart;
    m_smileChart->setTitle("Implied volatility smile");
    m_smileX = new QValueAxis;
    m_smileX->setTitleText("Strike");
    m_smileX->setLabelFormat("%.0f");
    m_smileY = new QValueAxis;
    m_smileY->setTitleText("IV (%)");
    m_smileY->setLabelFormat("%.1f");
    m_smileChart->addAxis(m_smileX, Qt::AlignBottom);
    m_smileChart->addAxis(m_smileY, Qt::AlignLeft);
    auto* smileView = ui::makeChartView(this, m_smileChart, 220);

    m_termChart = new QChart;
    m_termChart->setTitle("Volatility term structure");
    m_termX = new QValueAxis;
    m_termX->setTitleText("Years to expiry");
    m_termX->setLabelFormat("%.2f");
    m_termY = new QValueAxis;
    m_termY->setTitleText("IV (%)");
    m_termY->setLabelFormat("%.1f");
    m_termChart->addAxis(m_termX, Qt::AlignBottom);
    m_termChart->addAxis(m_termY, Qt::AlignLeft);
    auto* termView = ui::makeChartView(this, m_termChart, 220);

    m_sviInfo = new QLabel(this);
    m_sviInfo->setObjectName("value");
    m_sviInfo->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_sviInfo->setWordWrap(true);
    m_issues = new QPlainTextEdit(this);
    m_issues->setReadOnly(true);
    m_issues->setMinimumHeight(48);
    m_issues->setMaximumHeight(140);
    m_issues->setPlaceholderText("No arbitrage issues detected for this expiry.");

    auto* fitBox = new QGroupBox("Smile fit (raw SVI) && arbitrage checks", this);
    auto* fitLayout = new QVBoxLayout(fitBox);
    fitLayout->addWidget(m_sviInfo);
    fitLayout->addWidget(m_issues, 1);

    m_smileHover = new QLabel("Hover a point for strike, implied vol and quote details.", this);
    m_smileHover->setObjectName("muted");
    m_smileHover->setAlignment(Qt::AlignCenter);
    m_smileHover->setWordWrap(true);
    m_termHover = new QLabel("Hover a point for the expiry's ATM and wing vols.", this);
    m_termHover->setObjectName("muted");
    m_termHover->setAlignment(Qt::AlignCenter);
    m_termHover->setWordWrap(true);
    // Fixed two-line height: the readout text changes on every hover, and a label that
    // grew or shrank with it would resize the chart above and make it jitter.
    for (QLabel* readout : { m_smileHover, m_termHover }) {
        readout->setFixedHeight(readout->fontMetrics().lineSpacing() * 2 + 8);
        readout->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        readout->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    auto* smileColumn = new QVBoxLayout;
    smileColumn->setContentsMargins(0, 0, 0, 0);
    smileColumn->setSpacing(2);
    smileColumn->addWidget(smileView, 1);
    smileColumn->addWidget(m_smileHover);
    auto* termColumn = new QVBoxLayout;
    termColumn->setContentsMargins(0, 0, 0, 0);
    termColumn->setSpacing(2);
    termColumn->addWidget(termView, 1);
    termColumn->addWidget(m_termHover);
    auto* charts = new QHBoxLayout;
    charts->setSpacing(12);
    charts->addLayout(smileColumn, 1);
    charts->addLayout(termColumn, 1);

    // Vertical splitter: drag the grip between the table and the charts to show more strikes,
    // or hide the charts entirely with the toggle button.
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(10);
    // The table never collapses; the charts pane may, so a short window shrinks the
    // charts instead of forcing a tall minimum window height.
    m_splitter->setChildrenCollapsible(false);
    // The table itself is the top pane; its title and the price header sit above the
    // splitter so they stay put however the panes are dragged.
    m_table->setParent(m_splitter);
    // The charts and fit summary live in a scroll area so a squeezed pane scrolls instead
    // of overlapping, which lets the pane's minimum height stay small.
    auto* bottomContent = new QWidget;
    auto* bottomLayout = new QVBoxLayout(bottomContent);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(12);
    bottomLayout->addLayout(charts, 1);
    bottomLayout->addWidget(fitBox);
    auto* bottomScroll = new QScrollArea(m_splitter);
    bottomScroll->setWidgetResizable(true);
    bottomScroll->setWidget(bottomContent);
    bottomScroll->setFrameShape(QFrame::NoFrame);
    bottomScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bottomScroll->setMinimumHeight(120);
    m_bottomPane = bottomScroll;
    m_splitter->addWidget(m_table);
    m_splitter->addWidget(m_bottomPane);
    m_splitter->setCollapsible(0, false);
    m_splitter->setCollapsible(1, true);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setSizes({ 380, 480 });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 10);
    root->setSpacing(8);
    root->addWidget(liveBox);
    root->addWidget(controlsBox);
    root->addLayout(tableHeader);
    root->addWidget(m_splitter, 1);
    updateStrikeControls();
}

void ChainTab::wire()
{
    connect(m_import, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "Import option chain", QString(), "CSV files (*.csv *.txt);;All files (*)");
        if (!path.isEmpty()) importCsvFile(path);
    });
    connect(m_sample, &QPushButton::clicked, this, [this] { generateSample(); });
    connect(m_clear, &QPushButton::clicked, this, [this] { clearChain(); });
    connect(m_expiry, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) { if (!m_updating) showSlice(index); });
    connect(m_useAtm, &QPushButton::clicked, this, [this] { setMarketVolToAtm(); });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const int index = m_expiry->currentIndex();
        if (!onSendToPricer || index < 0 || static_cast<size_t>(index) >= m_slices.size()) return;
        QTableWidgetItem* strikeItem = m_table->item(row, ColStrike);
        if (!strikeItem) return;
        const double strike = strikeItem->data(Qt::UserRole).toDouble();
        const double vol = strikeItem->data(Qt::UserRole + 1).toDouble();
        const ExpirySlice& slice = m_slices[static_cast<size_t>(index)];
        onSendToPricer(strike, slice.maturity, vol, QString::fromStdString(slice.expiryDate));
    });

    connect(m_strikeMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        updateStrikeControls();
        showSlice(m_expiry->currentIndex());
    });
    for (QDoubleSpinBox* box : { m_strikePercent, m_strikeMin, m_strikeMax }) {
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { showSlice(m_expiry->currentIndex()); });
    }
    connect(m_toggleCharts, &QPushButton::clicked, this, [this] {
        const bool hide = m_bottomPane->isVisible();
        m_bottomPane->setVisible(!hide);
        m_toggleCharts->setText(hide ? "Show Charts" : "Hide Charts");
    });

    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this, [this](int, int, int) {
        if (!m_fittingColumns) m_userResizedColumns = true;
    });
    connect(m_fetch, &QPushButton::clicked, this, [this] { fetchLiveChain(); });
    connect(m_ticker, &QLineEdit::returnPressed, this, [this] { fetchLiveChain(); });
    connect(m_autoRefresh, &QCheckBox::toggled, this, [this](bool) { updateTimers(); });
    connect(m_chainInterval, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { updateTimers(); });
    connect(m_spotInterval, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { updateTimers(); });
    connect(m_useImpliedSpot, &QCheckBox::toggled, this, [this](bool checked) {
        m_state.useImpliedSpot = checked;
        if (!m_state.chainQuotes.empty() || m_state.vendorSpot > 0.0) {
            applySpotPolicy();
            m_state.notify();
        }
    });
    connect(m_chainTimer, &QTimer::timeout, this, [this] {
        if (!m_busy && !ticker().isEmpty() && m_client.hasApiKey()) fetchLiveChain(true);
    });
    connect(m_spotTimer, &QTimer::timeout, this, [this] {
        if (!m_busy && !ticker().isEmpty() && m_client.hasApiKey()) refreshUnderlying(true);
    });
    connect(m_treasury, &QPushButton::clicked, this, [this] { loadTreasuryCurve(); });
    connect(m_dividends, &QPushButton::clicked, this, [this] { loadDividends(); });
    connect(m_setKey, &QPushButton::clicked, this, [this] { promptForApiKey(); });
}

// MARK: - Data

void ChainTab::importCsvFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Import failed", QStringLiteral("Could not open %1.").arg(path));
        return;
    }
    const std::string text = file.readAll().toStdString();
    const QDate today = QDate::currentDate();
    const CivilDate valuation{ today.year(), today.month(), today.day() };
    const ChainParseResult parsed = parseChainCsv(text, m_defaultMaturity->value(), &valuation);
    if (!parsed.error.empty()) {
        QMessageBox::warning(this, "Import failed", QString::fromStdString(parsed.error));
        return;
    }
    m_state.chainQuotes = parsed.quotes;
    m_state.notify();

    QString message = QStringLiteral("Loaded %1 quotes from %2.").arg(parsed.quotes.size()).arg(QFileInfo(path).fileName());
    if (!parsed.warnings.empty()) {
        message += QStringLiteral(" %1 row%2 skipped.").arg(parsed.warnings.size()).arg(parsed.warnings.size() == 1 ? "" : "s");
        QStringList details;
        for (size_t i = 0; i < parsed.warnings.size() && i < 20; ++i) details << QString::fromStdString(parsed.warnings[i]);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle("Import summary");
        box.setText(message);
        box.setDetailedText(details.join("\n"));
        box.exec();
    }
    m_summary->setText(message);
    m_summary->setToolTip(m_summary->text());
}

void ChainTab::generateSample()
{
    const ChainMarket cm = m_state.chainMarket(0.5);
    const double step = std::max(0.5, std::round(cm.spot * 0.025 * 2.0) / 2.0);
    m_state.chainQuotes = syntheticChain(cm, { 1.0 / 12.0, 0.25, 0.5, 1.0 }, m_state.market.volatility, -0.08, 0.35, step, 10);
    m_state.notify();
    m_summary->setText("Synthetic chain: four expiries, 21 strikes each, with negative skew and a smile. Bid/ask spreads are 4% of mid.");
    m_summary->setToolTip(m_summary->text());
}

void ChainTab::clearChain()
{
    m_state.chainQuotes.clear();
    m_state.surface = VolSurface();
    m_state.notify();
    m_summary->clear();
}

void ChainTab::rebuild()
{
    const Market& m = m_state.market;
    m_marketInfo->setText(QStringLiteral("Implying from spot %1, r %2, q %3 (%4)")
                              .arg(ui::number(m.spot, 2), ui::percent(m_state.rateFor(0.5), 2), ui::percent(m.dividendYield, 2),
                                   m.model == Model::Black76 ? "Black-76" : "Black-Scholes-Merton"));
    updateSpotLabels();

    const int previous = m_expiry->currentIndex();
    if (m_state.chainQuotes.empty()) {
        m_slices.clear();
        m_state.surface = VolSurface();
    } else {
        // Group by expiry so each slice can use the curve rate for its own maturity.
        std::map<long long, std::vector<ChainQuote>> grouped;
        for (const ChainQuote& q : m_state.chainQuotes) {
            grouped[static_cast<long long>(std::llround(q.maturity * 3650.0))].push_back(q);
        }
        std::vector<ExpirySlice> slices;
        for (auto& [key, quotes] : grouped) {
            auto part = buildExpirySlices(quotes, m_state.chainMarket(quotes.front().maturity));
            slices.insert(slices.end(), part.begin(), part.end());
        }
        std::sort(slices.begin(), slices.end(), [](const ExpirySlice& a, const ExpirySlice& b) { return a.maturity < b.maturity; });
        m_slices = slices;
        m_state.surface.setSlices(m_slices);
    }

    m_updating = true;
    m_expiry->clear();
    for (const ExpirySlice& s : m_slices) {
        const QString date = s.expiryDate.empty() ? QStringLiteral("T = %1 yrs").arg(ui::number(s.maturity, 3)) : QString::fromStdString(s.expiryDate);
        m_expiry->addItem(QStringLiteral("%1 · %2 DTE · %3 quotes").arg(date).arg(s.daysToExpiry).arg(s.calls.size() + s.puts.size()));
    }
    const int index = (previous >= 0 && previous < m_expiry->count()) ? previous : 0;
    m_expiry->setCurrentIndex(m_expiry->count() > 0 ? index : -1);
    m_updating = false;
    showSlice(m_expiry->currentIndex());
}

void ChainTab::showSlice(int index)
{
    const int savedScroll = m_table->verticalScrollBar()->value();
    m_table->setRowCount(0);
    m_issues->clear();
    if (index < 0 || static_cast<size_t>(index) >= m_slices.size()) {
        m_sviInfo->setText(m_state.chainQuotes.empty()
                               ? "Fetch a live chain, import a CSV or generate a sample to see implied volatilities, a fitted smile and arbitrage checks."
                               : "No usable quotes for this expiry.");
        m_useAtm->setEnabled(false);
        m_tableTitle->setText("Strikes");
        updateCharts(nullptr);
        return;
    }
    const ExpirySlice& slice = m_slices[static_cast<size_t>(index)];
    m_useAtm->setEnabled(slice.fitted);

    struct RowData {
        const ImpliedQuote* call = nullptr;
        const ImpliedQuote* put = nullptr;
    };
    std::map<double, RowData> rows;
    for (const ImpliedQuote& q : slice.calls) rows[q.quote.strike].call = &q;
    for (const ImpliedQuote& q : slice.puts) rows[q.quote.strike].put = &q;
    std::map<double, QStringList> flags;
    for (const ChainIssue& issue : slice.issues) {
        flags[issue.strike] << QString::fromStdString(issue.message);
    }

    // Apply the strike filter; the full set is kept for the forward-neighbour highlight.
    int hidden = 0;
    std::vector<std::map<double, RowData>::const_iterator> visible;
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        if (strikeInRange(it->first, slice.forward)) visible.push_back(it);
        else ++hidden;
    }
    m_table->setRowCount(static_cast<int>(visible.size()));
    int r = 0;
    for (const auto& it : visible) {
        const double strike = it->first;
        const RowData& data = it->second;
        auto* strikeItem = ui::makeCell(ui::number(strike, 2));
        strikeItem->setData(Qt::UserRole, strike);
        // Prefer the out-of-the-money side's vol for hand-off; it is the better-defined one.
        double handoffVol = 0.0;
        if (strike >= slice.forward && data.call && data.call->solved) handoffVol = data.call->impliedVol;
        else if (data.put && data.put->solved) handoffVol = data.put->impliedVol;
        else if (data.call && data.call->solved) handoffVol = data.call->impliedVol;
        strikeItem->setData(Qt::UserRole + 1, handoffVol);
        const auto nextIt = std::next(it);
        const bool nearestBelowForward = strike <= slice.forward && (nextIt == rows.end() || nextIt->first > slice.forward);
        if (nearestBelowForward) {
            strikeItem->setToolTip(QStringLiteral("Nearest strike at or below the forward %1").arg(ui::number(slice.forward, 2)));
        }
        m_table->setItem(r, ColStrike, strikeItem);

        const double spotForMoneyness = m_state.market.spot;
        auto fill = [&](const ImpliedQuote* q, int bid, int ask, int mid, int iv, const QString& colour, bool isCall) {
            // In-the-money cells get the side's tint, as on exchange and broker chains.
            const bool inTheMoney = isCall ? strike < spotForMoneyness : strike > spotForMoneyness;
            const QColor tint(isCall ? m_theme.itmCall : m_theme.itmPut);
            auto place = [&](int col, QTableWidgetItem* item) {
                if (inTheMoney) item->setBackground(QBrush(tint));
                m_table->setItem(r, col, item);
            };
            if (!q) {
                for (int c : { bid, ask, mid, iv }) place(c, ui::makeCell("–"));
                return;
            }
            auto* bidItem = ui::makeCell(q->quote.bid > 0 ? ui::number(q->quote.bid, 2) : "–");
            if (q->quote.bid > 0) bidItem->setForeground(QBrush(QColor(m_theme.up)));
            auto* askItem = ui::makeCell(q->quote.ask > 0 ? ui::number(q->quote.ask, 2) : "–");
            if (q->quote.ask > 0) askItem->setForeground(QBrush(QColor(m_theme.down)));
            place(bid, bidItem);
            place(ask, askItem);
            auto* midItem = ui::makeCell(ui::number(q->quote.mid, 2));
            if (q->quote.bid <= 0 && q->quote.ask <= 0) midItem->setToolTip("Last trade / day close (no bid-ask quote available)");
            place(mid, midItem);
            auto* ivItem = ui::makeCell(q->solved ? ui::percent(q->impliedVol, 2) : QString::fromStdString(q->note));
            ivItem->setForeground(QBrush(QColor(q->solved ? colour : m_theme.textMuted)));
            QStringList tip;
            if (q->quote.vendorImpliedVol > 0) tip << QStringLiteral("Vendor implied vol %1").arg(ui::percent(q->quote.vendorImpliedVol, 2));
            if (q->quote.volume > 0) tip << QStringLiteral("Volume %1").arg(ui::number(q->quote.volume, 0));
            if (q->quote.openInterest > 0) tip << QStringLiteral("Open interest %1").arg(ui::number(q->quote.openInterest, 0));
            if (!tip.isEmpty()) ivItem->setToolTip(tip.join("\n"));
            place(iv, ivItem);
        };
        fill(data.call, ColCallBid, ColCallAsk, ColCallMid, ColCallIv, m_theme.call, true);
        fill(data.put, ColPutBid, ColPutAsk, ColPutMid, ColPutIv, m_theme.put, false);
        m_table->setItem(r, ColFitIv, ui::makeCell(slice.fitted ? ui::percent(slice.smileVol(strike), 2) : "–"));

        const auto flagIt = flags.find(strike);
        auto* flagItem = ui::makeCell(flagIt == flags.end() ? QString() : QStringLiteral("⚠ %1").arg(flagIt->second.size()), Qt::AlignCenter);
        if (flagIt != flags.end()) {
            flagItem->setToolTip(flagIt->second.join("\n"));
            flagItem->setForeground(QBrush(QColor(m_theme.warning)));
        }
        m_table->setItem(r, ColFlags, flagItem);
        ++r;
    }

    // Highlight the at-the-money strikes: the nearest listed strike at or below spot and
    // the nearest above it. The closer of the two gets the stronger tint.
    const double spot = m_state.market.spot;
    int belowRow = -1, aboveRow = -1;
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        const double k = visible[static_cast<size_t>(i)]->first;
        if (k <= spot) belowRow = i;                    // strikes ascend, so the last one wins
        if (k > spot && aboveRow < 0) aboveRow = i;
    }
    int nearestRow = -1;
    if (belowRow >= 0 && aboveRow >= 0) {
        nearestRow = (spot - visible[static_cast<size_t>(belowRow)]->first) <= (visible[static_cast<size_t>(aboveRow)]->first - spot) ? belowRow : aboveRow;
    } else {
        nearestRow = belowRow >= 0 ? belowRow : aboveRow;
    }
    const QColor atmStrong = blend(QColor(m_theme.surface), QColor(m_theme.accent), 0.32);
    const QColor atmSoft = blend(QColor(m_theme.surface), QColor(m_theme.accent), 0.16);
    for (int row : { belowRow, aboveRow }) {
        if (row < 0) continue;
        const QColor tint = row == nearestRow ? atmStrong : atmSoft;
        for (int col = 0; col < ColumnCount; ++col) {
            if (QTableWidgetItem* item = m_table->item(row, col)) {
                item->setBackground(QBrush(tint));
                if (col == ColStrike) {
                    QFont f = item->font();
                    f.setBold(true);
                    item->setFont(f);
                    item->setForeground(QBrush(QColor(m_theme.textStrong)));
                    item->setToolTip(QStringLiteral("%1 strike: spot is %2").arg(row == nearestRow ? "At-the-money" : "Bracketing", ui::number(spot, 2)));
                }
            }
        }
    }
    const bool sameSlice = std::fabs(slice.maturity - m_lastSliceKey) < 1e-9;
    if (!sameSlice) {
        m_atmCentered = false;
        m_userResizedColumns = false;   // a new slice starts from content-fitted widths
        fitColumns();
    }
    m_lastSliceKey = slice.maturity;
    if (sameSlice && m_atmCentered) {
        // Auto-refresh of the slice already on screen: keep the user's scroll position.
        m_table->verticalScrollBar()->setValue(savedScroll);
    } else if (nearestRow >= 0) {
        if (QTableWidgetItem* anchor = m_table->item(nearestRow, ColStrike)) {
            m_table->scrollToItem(anchor, QAbstractItemView::PositionAtCenter);
            // Centring only takes effect once the table has been laid out on screen.
            m_atmCentered = m_table->isVisible() && m_table->viewport()->height() > 0;
        }
    }

    if (!visible.empty()) {
        m_tableTitle->setText(QStringLiteral("%1 · %2 DTE · strikes %3 – %4 (%5 shown%6)")
                                  .arg(slice.expiryDate.empty() ? QStringLiteral("T = %1 yrs").arg(ui::number(slice.maturity, 3)) : QString::fromStdString(slice.expiryDate))
                                  .arg(slice.daysToExpiry)
                                  .arg(ui::number(visible.front()->first, 2), ui::number(visible.back()->first, 2))
                                  .arg(visible.size())
                                  .arg(hidden > 0 ? QStringLiteral(", %1 hidden by the strike filter").arg(hidden) : QString()));
    } else {
        m_tableTitle->setText(QStringLiteral("No strikes in the selected range (%1 hidden). Widen the strike filter.").arg(hidden));
    }

    const QString expiryText = slice.expiryDate.empty()
        ? QStringLiteral("%1 DTE · T = %2 yrs").arg(slice.daysToExpiry).arg(ui::number(slice.maturity, 4))
        : QStringLiteral("Expiry %1 · %2 DTE · T = %3 yrs").arg(QString::fromStdString(slice.expiryDate)).arg(slice.daysToExpiry).arg(ui::number(slice.maturity, 4));
    if (slice.fitted) {
        m_sviInfo->setText(QStringLiteral("%1 · forward %2 · ATM vol %3 · fit RMSE %4 vol pts on %5 OTM quotes\n"
                                          "SVI: a = %6, b = %7, ρ = %8, m = %9, σ = %10")
                               .arg(expiryText, ui::number(slice.forward, 2), ui::percent(slice.atmVol(), 2),
                                    ui::number(slice.fitRmse * 100.0, 3))
                               .arg(slice.pointsUsed)
                               .arg(ui::number(slice.svi.a, 5), ui::number(slice.svi.b, 4), ui::number(slice.svi.rho, 3),
                                    ui::number(slice.svi.m, 4), ui::number(slice.svi.sigma, 4)));
    } else {
        m_sviInfo->setText(QStringLiteral("%1 · forward %2 · not enough solved quotes to fit a smile (need 3).")
                               .arg(expiryText, ui::number(slice.forward, 2)));
    }
    QStringList issueLines;
    for (const ChainIssue& issue : slice.issues) issueLines << QString::fromStdString(issue.message);
    m_issues->setPlainText(issueLines.join("\n"));

    updateCharts(&slice);
}

void ChainTab::updateCharts(const ExpirySlice* slice)
{
    m_smileChart->removeAllSeries();
    m_termChart->removeAllSeries();

    if (slice) {
        auto* calls = new QScatterSeries;
        calls->setName("Call IV");
        calls->setMarkerSize(8.0);
        calls->setColor(QColor(m_theme.call));
        calls->setBorderColor(Qt::transparent);
        auto* puts = new QScatterSeries;
        puts->setName("Put IV");
        puts->setMarkerSize(8.0);
        puts->setColor(QColor(m_theme.put));
        puts->setBorderColor(Qt::transparent);
        double kMin = INFINITY, kMax = -INFINITY, vMin = INFINITY, vMax = -INFINITY;
        // Hover readouts keyed by strike, built alongside the points.
        auto callTips = std::make_shared<std::map<double, QString>>();
        auto putTips = std::make_shared<std::map<double, QString>>();
        const QString expiryText = slice->expiryDate.empty() ? QStringLiteral("%1 DTE").arg(slice->daysToExpiry)
                                                             : QStringLiteral("%1 (%2 DTE)").arg(QString::fromStdString(slice->expiryDate)).arg(slice->daysToExpiry);
        auto describe = [&](const ImpliedQuote& q, const char* side) {
            QString text = QStringLiteral("%1 %2 · strike %3 · IV %4 · mid %5")
                               .arg(side, expiryText, ui::number(q.quote.strike, 2), ui::percent(q.impliedVol, 2), ui::number(q.quote.mid, 2));
            if (q.quote.bid > 0 || q.quote.ask > 0) text += QStringLiteral(" (bid %1 / ask %2)").arg(ui::number(q.quote.bid, 2), ui::number(q.quote.ask, 2));
            if (slice->fitted) text += QStringLiteral(" · fit %1").arg(ui::percent(slice->smileVol(q.quote.strike), 2));
            if (q.quote.vendorImpliedVol > 0) text += QStringLiteral(" · vendor IV %1").arg(ui::percent(q.quote.vendorImpliedVol, 2));
            if (q.quote.volume > 0) text += QStringLiteral(" · vol %1").arg(ui::number(q.quote.volume, 0));
            if (q.quote.openInterest > 0) text += QStringLiteral(" · OI %1").arg(ui::number(q.quote.openInterest, 0));
            return text;
        };
        for (const ImpliedQuote& q : slice->calls) {
            if (!q.solved || !strikeInRange(q.quote.strike, slice->forward)) continue;
            calls->append(q.quote.strike, q.impliedVol * 100.0);
            (*callTips)[q.quote.strike] = describe(q, "Call");
            kMin = std::min(kMin, q.quote.strike); kMax = std::max(kMax, q.quote.strike);
            vMin = std::min(vMin, q.impliedVol * 100.0); vMax = std::max(vMax, q.impliedVol * 100.0);
        }
        for (const ImpliedQuote& q : slice->puts) {
            if (!q.solved || !strikeInRange(q.quote.strike, slice->forward)) continue;
            puts->append(q.quote.strike, q.impliedVol * 100.0);
            (*putTips)[q.quote.strike] = describe(q, "Put");
            kMin = std::min(kMin, q.quote.strike); kMax = std::max(kMax, q.quote.strike);
            vMin = std::min(vMin, q.impliedVol * 100.0); vMax = std::max(vMax, q.impliedVol * 100.0);
        }
        auto nearestTip = [](const std::map<double, QString>& tips, double strike) -> QString {
            if (tips.empty()) return QString();
            auto it = tips.lower_bound(strike);
            if (it == tips.end()) return std::prev(it)->second;
            if (it != tips.begin() && std::fabs(std::prev(it)->first - strike) < std::fabs(it->first - strike)) return std::prev(it)->second;
            return it->second;
        };
        connect(calls, &QScatterSeries::hovered, this, [this, callTips, nearestTip](const QPointF& point, bool state) {
            showChartHover(m_smileHover, nearestTip(*callTips, point.x()), state);
        });
        connect(puts, &QScatterSeries::hovered, this, [this, putTips, nearestTip](const QPointF& point, bool state) {
            showChartHover(m_smileHover, nearestTip(*putTips, point.x()), state);
        });
        if (std::isfinite(kMin)) {
            auto* fit = new QLineSeries;
            fit->setName("SVI fit");
            fit->setPen(QPen(QColor(m_theme.accent), 2.0));
            if (slice->fitted) {
                for (int i = 0; i <= 100; ++i) {
                    const double k = kMin + (kMax - kMin) * i / 100.0;
                    const double v = slice->smileVol(k) * 100.0;
                    fit->append(k, v);
                    vMin = std::min(vMin, v); vMax = std::max(vMax, v);
                }
                const SviParams svi = slice->svi;
                const double forward = slice->forward, maturity = slice->maturity;
                connect(fit, &QLineSeries::hovered, this, [this, svi, forward, maturity, expiryText](const QPointF& point, bool state) {
                    const double k = point.x();
                    const double vol = k > 0.0 ? sviVol(svi, std::log(k / forward), maturity) : 0.0;
                    showChartHover(m_smileHover,
                                   QStringLiteral("SVI fit %1 · strike %2 · IV %3 · log-moneyness %4")
                                       .arg(expiryText, ui::number(k, 2), ui::percent(vol, 2), ui::number(std::log(k / forward), 4)),
                                   state);
                });
            }
            auto* forwardLine = new QLineSeries;
            const double pad = std::max((vMax - vMin) * 0.15, 0.5);
            forwardLine->append(slice->forward, vMin - pad);
            forwardLine->append(slice->forward, vMax + pad);
            forwardLine->setPen(QPen(QColor(m_theme.textMuted), 1.0, Qt::DashLine));
            for (QAbstractSeries* s : std::initializer_list<QAbstractSeries*>{ forwardLine, fit, calls, puts }) {
                m_smileChart->addSeries(s);
                s->attachAxis(m_smileX);
                s->attachAxis(m_smileY);
            }
            for (QLegendMarker* marker : m_smileChart->legend()->markers(forwardLine)) marker->setVisible(false);
            const double kPad = std::max((kMax - kMin) * 0.05, 0.5);
            m_smileX->setRange(kMin - kPad, kMax + kPad);
            m_smileY->setRange(vMin - pad, vMax + pad);
        }
        m_smileChart->setTitle(slice->expiryDate.empty()
                                   ? QStringLiteral("Implied volatility smile · %1 DTE").arg(slice->daysToExpiry)
                                   : QStringLiteral("Implied volatility smile · %1 (%2 DTE)").arg(QString::fromStdString(slice->expiryDate)).arg(slice->daysToExpiry));
    } else {
        m_smileChart->setTitle("Implied volatility smile");
        m_smileX->setRange(0, 1);
        m_smileY->setRange(0, 1);
    }

    // Term structure from every fitted slice: 90% strike, ATM and 110% strike.
    const std::vector<ExpirySlice>& fitted = m_state.surface.slices();
    if (!fitted.empty()) {
        auto* atm = new QLineSeries;
        atm->setName("ATM");
        atm->setPen(QPen(QColor(m_theme.accent), 2.0));
        atm->setPointsVisible(true);
        auto* low = new QLineSeries;
        low->setName("90% strike");
        low->setPen(QPen(QColor(m_theme.put), 1.6, Qt::DashLine));
        low->setPointsVisible(true);
        auto* high = new QLineSeries;
        high->setName("110% strike");
        high->setPen(QPen(QColor(m_theme.call), 1.6, Qt::DashLine));
        high->setPointsVisible(true);
        double vMin = INFINITY, vMax = -INFINITY;
        auto termTips = std::make_shared<std::map<double, QString>>();
        for (const ExpirySlice& s : fitted) {
            const double a = s.atmVol() * 100.0;
            const double l = s.smileVol(s.forward * 0.9) * 100.0;
            const double h = s.smileVol(s.forward * 1.1) * 100.0;
            atm->append(s.maturity, a);
            low->append(s.maturity, l);
            high->append(s.maturity, h);
            for (double v : { a, l, h }) { vMin = std::min(vMin, v); vMax = std::max(vMax, v); }
            const QString date = s.expiryDate.empty() ? QStringLiteral("T = %1 yrs").arg(ui::number(s.maturity, 3)) : QString::fromStdString(s.expiryDate);
            (*termTips)[s.maturity] = QStringLiteral("%1 · %2 DTE · ATM %3 · 90% strike %4 · 110% strike %5 · forward %6 · fit RMSE %7 pts")
                                          .arg(date).arg(s.daysToExpiry)
                                          .arg(ui::number(a, 2) + "%", ui::number(l, 2) + "%", ui::number(h, 2) + "%", ui::number(s.forward, 2),
                                               ui::number(s.fitRmse * 100.0, 2));
        }
        auto nearestTerm = [termTips](double maturity) -> QString {
            if (termTips->empty()) return QString();
            auto it = termTips->lower_bound(maturity);
            if (it == termTips->end()) return std::prev(it)->second;
            if (it != termTips->begin() && std::fabs(std::prev(it)->first - maturity) < std::fabs(it->first - maturity)) return std::prev(it)->second;
            return it->second;
        };
        for (QLineSeries* s : { low, high, atm }) {
            m_termChart->addSeries(s);
            s->attachAxis(m_termX);
            s->attachAxis(m_termY);
            connect(s, &QLineSeries::hovered, this, [this, nearestTerm](const QPointF& point, bool state) {
                showChartHover(m_termHover, nearestTerm(point.x()), state);
            });
        }
        const double pad = std::max((vMax - vMin) * 0.15, 0.5);
        m_termX->setRange(0.0, fitted.back().maturity * 1.05);
        m_termY->setRange(vMin - pad, vMax + pad);
    } else {
        m_termX->setRange(0, 1);
        m_termY->setRange(0, 1);
    }

    styleChart(m_smileChart, m_theme);
    styleChart(m_termChart, m_theme);
}

void ChainTab::showChartHover(QLabel* readout, const QString& text, bool state)
{
    // Only re-polish the style when the state flips; polishing on every mouse move
    // forces a relayout and visibly shakes the chart.
    const QString wanted = (state && !text.isEmpty()) ? QStringLiteral("value") : QStringLiteral("muted");
    if (state && !text.isEmpty()) {
        if (readout->text() != text) readout->setText(text);
        QToolTip::showText(QCursor::pos(), text, readout);
    } else {
        QToolTip::hideText();
    }
    if (readout->objectName() != wanted) {
        readout->setObjectName(wanted);
        ui::restyle(readout);
    }
}

bool ChainTab::strikeInRange(double strike, double reference) const
{
    switch (m_strikeMode->currentData().toInt()) {
    case 1: {
        const double band = m_strikePercent->value() / 100.0;
        return reference > 0.0 && strike >= reference * (1.0 - band) && strike <= reference * (1.0 + band);
    }
    case 2: {
        if (strike < m_strikeMin->value()) return false;
        return m_strikeMax->value() <= 0.0 || strike <= m_strikeMax->value();
    }
    default:
        return true;
    }
}

void ChainTab::updateStrikeControls()
{
    const int mode = m_strikeMode->currentData().toInt();
    m_strikePercent->setVisible(mode == 1);
    m_strikeMin->setVisible(mode == 2);
    m_strikeToLabel->setVisible(mode == 2);
    m_strikeMax->setVisible(mode == 2);
    if (mode == 2 && m_strikeMin->value() <= 0.0 && m_strikeMax->value() <= 0.0 && m_state.market.spot > 0.0) {
        // Seed the custom range around the current spot the first time it is chosen.
        const QSignalBlocker a(m_strikeMin), b(m_strikeMax);
        m_strikeMin->setValue(std::floor(m_state.market.spot * 0.8));
        m_strikeMax->setValue(std::ceil(m_state.market.spot * 1.2));
    }
}

void ChainTab::setMarketVolToAtm()
{
    const int index = m_expiry->currentIndex();
    if (index < 0 || static_cast<size_t>(index) >= m_slices.size() || !m_slices[static_cast<size_t>(index)].fitted) return;
    m_state.market.volatility = m_slices[static_cast<size_t>(index)].atmVol();
    m_state.notify();
}

// MARK: - Live data

void ChainTab::setTicker(const QString& ticker)
{
    m_ticker->setText(ticker.trimmed().toUpper());
}

QString ChainTab::ticker() const
{
    return m_ticker->text().trimmed().toUpper();
}

void ChainTab::refreshKeyStatus()
{
    if (m_client.hasApiKey()) {
        const QString source = m_client.apiKeySource();
        m_keyStatus->setText(source.startsWith("environment") ? QStringLiteral("Key: environment")
                                                                : (source.startsWith("application") ? QStringLiteral("Key: preferences") : QStringLiteral("Key: %1").arg(source)));
        m_keyStatus->setToolTip(QStringLiteral("API key source: %1").arg(source));
        m_keyStatus->setObjectName("muted");
    } else {
        m_keyStatus->setText("No API key");
        m_keyStatus->setObjectName("warning");
    }
    ui::restyle(m_keyStatus);
}

bool ChainTab::promptForApiKey()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Massive.com API Key");
    auto* intro = new QLabel("Paste your Massive.com (Polygon.io) API key. The key is sent only to api.massive.com. "
                             "Setting the MASSIVE_API_KEY or POLYGON_API_KEY environment variable avoids this dialog.", &dialog);
    intro->setWordWrap(true);
    intro->setObjectName("muted");
    auto* edit = new QLineEdit(&dialog);
    edit->setEchoMode(QLineEdit::Password);
    edit->setPlaceholderText("API key");
    edit->setMinimumWidth(360);
    auto* remember = new QCheckBox("Remember on this Mac (stored in the application preferences, unencrypted)", &dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addWidget(intro);
    layout->addWidget(edit);
    layout->addWidget(remember);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted && !edit->text().trimmed().isEmpty()) {
        m_client.setApiKey(edit->text(), remember->isChecked());
    }
    refreshKeyStatus();
    return m_client.hasApiKey();
}

void ChainTab::setLiveBusy(bool busy, const QString& status)
{
    m_busy = busy;
    if (!m_automatic) {
        for (QWidget* w : std::initializer_list<QWidget*>{ m_fetch, m_treasury, m_dividends, m_ticker, m_expiryCount }) {
            w->setEnabled(!busy);
        }
    }
    if (!busy) m_automatic = false;
    ui::setStatus(m_liveStatus, status, ui::StatusKind::Info);
    m_liveStatus->setToolTip(status);
}

void ChainTab::finishLive(bool ok, const QString& message)
{
    setLiveBusy(false, message);
    ui::setStatus(m_liveStatus, message, ok ? ui::StatusKind::Info : ui::StatusKind::Error);
    m_liveStatus->setToolTip(message);
    if (onLiveOperationFinished) onLiveOperationFinished(ok, message);
}

void ChainTab::fetchLiveChain(bool automatic)
{
    if (m_busy) return;
    const QString symbol = ticker();
    if (symbol.isEmpty()) {
        finishLive(false, "Enter a ticker symbol first.");
        return;
    }
    if (!m_client.hasApiKey() && (automatic || !promptForApiKey())) {
        finishLive(false, "No API key available.");
        return;
    }
    const int maxExpiries = m_expiryCount->value();   // 0 = every listed expiry
    const QDate today = QDate::currentDate();
    m_automatic = automatic;
    setLiveBusy(true, automatic ? QStringLiteral("Refreshing %1…").arg(symbol) : QStringLiteral("Fetching %1 underlying price…").arg(symbol));

    auto fail = [this](const QString& message) { finishLive(false, message); };

    m_client.fetchUnderlying(symbol, [this, symbol, maxExpiries, today, fail, automatic](const MarketDataClient::UnderlyingSnapshot& snap) {
        const QString scope = maxExpiries > 0 ? QStringLiteral("%1 nearest expir%2").arg(maxExpiries).arg(maxExpiries == 1 ? "y" : "ies")
                                              : QStringLiteral("all expiries");
        ui::setStatus(m_liveStatus, QStringLiteral("%1 = %2 (%3). Downloading %4…").arg(symbol, ui::number(snap.price, 2), snap.priceSource, scope),
                      ui::StatusKind::Info);
        m_client.fetchChains(
            symbol, today, maxExpiries,
            [this, symbol, scope](int pages, int contracts) {
                ui::setStatus(m_liveStatus, QStringLiteral("Downloading %1 chain (%2): %3 contracts in %4 page%5…")
                                                .arg(symbol, scope).arg(contracts).arg(pages).arg(pages == 1 ? "" : "s"),
                              ui::StatusKind::Info);
            },
            [this, symbol, snap, today, automatic, maxExpiries](const MarketDataClient::ChainDownload& download) {
                const QDateTime now = QDateTime::currentDateTime();
                // Full downloads go into the in-memory database so other tabs can switch to
                // this ticker instantly; partial (N-expiry) downloads are not representative.
                if (m_store && maxExpiries <= 0) {
                    ChainStore::StoredChain stored;
                    stored.ticker = symbol;
                    stored.snapshot = snap;
                    stored.download = download;
                    stored.fetchedAt = now;
                    m_store->put(stored);
                }
                applyDownload(symbol, snap, download, now);
                if (automatic) {
                    finishLive(true, QStringLiteral("%1 refreshed at %2: %3 contracts, spot %4 (%5).")
                                         .arg(symbol, m_state.chainTime.toString("HH:mm:ss")).arg(download.quotes.size())
                                         .arg(ui::number(m_state.market.spot, 2), m_state.spotSource));
                    return;
                }
                m_summary->setText(QStringLiteral("Live chain for %1 from Massive.com as of %2.").arg(symbol, now.toString("yyyy-MM-dd HH:mm")));
                m_summary->setToolTip(m_summary->text());
                finishLive(true, downloadSummary(symbol, download, today));
            },
            fail);
    }, fail);
}

void ChainTab::applyDownload(const QString& symbol, const MarketDataClient::UnderlyingSnapshot& snap, const MarketDataClient::ChainDownload& download,
                             const QDateTime& fetchedAt)
{
    m_state.chainQuotes = download.quotes;
    if (m_state.underlyingTicker != symbol) {
        m_state.companyName.clear();
        m_state.exchange.clear();
        m_state.industry.clear();
        m_state.companyDescription.clear();
        m_state.logo = QImage();
        m_brandingRequested.clear();
        m_state.resetPreviousCloseSource();
    }
    m_state.underlyingTicker = symbol;
    m_state.vendorSpot = snap.price;
    m_state.previousClose = snap.previousClose;
    m_state.vendorSource = snap.priceSource;
    m_state.spotAsOf = snap.asOf;
    m_state.spotTime = fetchedAt;
    m_state.chainTime = fetchedAt;
    m_state.market.model = Model::BlackScholesMerton;
    applySpotPolicy();
    m_state.notify();
    updateTimers();
}

QString ChainTab::downloadSummary(const QString& symbol, const MarketDataClient::ChainDownload& download, const QDate& today) const
{
    QString range;
    if (!download.expiries.empty()) {
        const QDate first = *download.expiries.begin();
        const QDate last = *download.expiries.rbegin();
        range = QStringLiteral(" from %1 (%2 DTE) to %3 (%4 DTE)")
                    .arg(first.toString(Qt::ISODate)).arg(today.daysTo(first))
                    .arg(last.toString(Qt::ISODate)).arg(today.daysTo(last));
    }
    QString message = QStringLiteral("%1: spot %2 (%3), %4 expir%5%6, %7 contracts loaded, %8 skipped (no price or expired).")
                          .arg(symbol, ui::number(m_state.market.spot, 2), m_state.spotSource)
                          .arg(download.expiries.size()).arg(download.expiries.size() == 1 ? "y" : "ies")
                          .arg(range).arg(download.quotes.size()).arg(download.skipped);
    if (download.withQuotes == 0) {
        message += " No bid/ask quotes on this plan: mids are last trades or day closes, so implied vols can be stale for illiquid strikes.";
    }
    message += " Contracts are American-style; implied vols use the European model.";
    return message;
}

bool ChainTab::applyStoredChain(const QString& rawSymbol)
{
    if (!m_store) return false;
    const QString symbol = rawSymbol.trimmed().toUpper();
    const std::optional<ChainStore::StoredChain> stored = m_store->get(symbol);
    if (!stored) return false;
    if (m_busy) return false;   // a download for the current ticker is mid-flight; let it finish
    setTicker(symbol);
    // Honour the expiry-count control: keep only the N nearest expiries of the full chain.
    MarketDataClient::ChainDownload download = stored->download;
    const int maxExpiries = m_expiryCount->value();
    if (maxExpiries > 0 && static_cast<int>(download.expiries.size()) > maxExpiries) {
        std::set<QDate> keep;
        for (const QDate& d : download.expiries) {
            if (static_cast<int>(keep.size()) >= maxExpiries) break;
            keep.insert(d);
        }
        download.quotes.erase(std::remove_if(download.quotes.begin(), download.quotes.end(), [&keep](const ChainQuote& q) {
                                  return !keep.count(QDate::fromString(QString::fromStdString(q.expiryDate), Qt::ISODate));
                              }), download.quotes.end());
        download.expiries = keep;
    }
    applyDownload(symbol, stored->snapshot, download, stored->fetchedAt);
    const qint64 age = stored->fetchedAt.secsTo(QDateTime::currentDateTime());
    m_summary->setText(QStringLiteral("Chain for %1 from the in-memory store, downloaded %2 (%3 min ago).")
                           .arg(symbol, stored->fetchedAt.toString("HH:mm:ss")).arg(age / 60));
    ui::setStatus(m_liveStatus, downloadSummary(symbol, download, QDate::currentDate()) + QStringLiteral(" Served from memory, fetched %1 min ago.").arg(age / 60),
                  ui::StatusKind::Info);
    return true;
}

void ChainTab::refreshUnderlying(bool automatic)
{
    if (m_busy) return;
    const QString symbol = ticker();
    if (symbol.isEmpty() || !m_client.hasApiKey()) return;
    m_automatic = automatic;
    setLiveBusy(true, QStringLiteral("Checking %1 price…").arg(symbol));
    m_client.fetchUnderlying(symbol, [this, symbol](const MarketDataClient::UnderlyingSnapshot& snap) {
        m_state.underlyingTicker = symbol;
        m_state.vendorSpot = snap.price;
        m_state.previousClose = snap.previousClose;
        m_state.vendorSource = snap.priceSource;
        m_state.spotAsOf = snap.asOf;
        m_state.spotTime = QDateTime::currentDateTime();
        applySpotPolicy();
        m_state.notify();
        finishLive(true, QStringLiteral("%1 price checked at %2: vendor %3 (%4), using %5 (%6).")
                             .arg(symbol, m_state.spotTime.toString("HH:mm:ss"), ui::number(snap.price, 2), snap.priceSource,
                                  ui::number(m_state.market.spot, 2), m_state.spotSource));
    }, [this](const QString& message) { finishLive(false, message); });
}

void ChainTab::applySpotPolicy()
{
    m_state.applySpotPolicy();
}

void ChainTab::updateSpotLabels()
{
    const Market& m = m_state.market;
    const QString symbol = m_state.underlyingTicker.isEmpty() ? QStringLiteral("Underlying") : m_state.underlyingTicker;

    // Company identity: real icon when we have one, otherwise a monogram badge.
    if (m_state.underlyingTicker.isEmpty()) {
        m_logoLabel->setVisible(false);
    } else {
        const qreal dpr = devicePixelRatioF();
        m_logoLabel->setPixmap(m_state.logo.isNull()
                                   ? ui::monogramBadge(m_state.underlyingTicker, QColor(m_theme.accent2), QColor(m_theme.window), 22, dpr)
                                   : ui::roundedLogo(m_state.logo, 22, dpr));
        m_logoLabel->setToolTip(m_state.companyName.isEmpty() ? m_state.underlyingTicker
                                                              : QStringLiteral("%1 · %2%3").arg(m_state.companyName, m_state.underlyingTicker,
                                                                                                 m_state.exchange.isEmpty() ? QString() : " · " + m_state.exchange));
        m_logoLabel->setVisible(true);
    }
    ensureBranding();
    if (m_state.hasDayChange()) {
        const double change = m_state.dayChange();
        m_spotLabel->setText(QStringLiteral("%1  %2  %3%4 (%5%6%)")
                                 .arg(symbol, ui::number(m.spot, 2), change >= 0 ? "+" : "−", ui::number(std::fabs(change), 2),
                                      change >= 0 ? "+" : "−", ui::number(std::fabs(m_state.dayChangePercent()), 2)));
        m_spotLabel->setObjectName(change >= 0 ? "spotUp" : "spotDown");
    } else {
        m_spotLabel->setText(QStringLiteral("%1  %2").arg(symbol, ui::number(m.spot, 2)));
        m_spotLabel->setObjectName("spotFlat");
    }
    ui::restyle(m_spotLabel);

    QStringList parts;
    QStringList tip;
    tip << QStringLiteral("Current underlying price: %1").arg(ui::number(m.spot, 2));
    if (!m_state.companyName.isEmpty()) parts << m_state.companyName;
    const QDateTime now = QDateTime::currentDateTime();
    if (m_state.spotSource == "option parity" && !m_state.impliedSpotNote.isEmpty()) {
        parts << m_state.impliedSpotNote;
        tip << QStringLiteral("Derived from fresh option prices: %1.").arg(m_state.impliedSpotNote);
    }
    if (m_state.vendorSpot > 0.0) {
        QString vendor = QStringLiteral("vendor %1 %2").arg(m_state.vendorSource.isEmpty() ? QStringLiteral("quote") : m_state.vendorSource, ui::number(m_state.vendorSpot, 2));
        if (m_state.spotAsOf.isValid()) {
            const qint64 delaySeconds = m_state.spotAsOf.secsTo(now);
            vendor += QStringLiteral(" as of %1 (%2 min delayed)").arg(m_state.spotAsOf.toString("HH:mm")).arg(std::max<qint64>(0, delaySeconds / 60));
        }
        parts << vendor;
        tip << QStringLiteral("Vendor stock price from Massive.com: %1.").arg(vendor);
    }
    if (m_state.chainTime.isValid()) {
        parts << QStringLiteral("chain %1").arg(m_state.chainTime.toString("HH:mm:ss"));
    }
    if (parts.isEmpty()) {
        parts << (m_state.spotSource.isEmpty() ? QStringLiteral("set on the Pricer tab") : m_state.spotSource);
        tip << "Set on the Pricer tab, or fetch a live chain to use the market price.";
    }
    m_spotDetail->setText(parts.join(" · "));
    tip << "The highlighted table rows are the strikes bracketing this price.";
    m_spotLabel->setToolTip(tip.join("\n"));
    m_spotDetail->setToolTip(m_spotLabel->toolTip());
}

void ChainTab::fitColumns()
{
    if (m_userResizedColumns || m_table->rowCount() == 0) return;
    m_fittingColumns = true;
    m_table->resizeColumnsToContents();
    int total = 0;
    for (int col = 0; col < ColumnCount; ++col) {
        m_table->setColumnWidth(col, std::max(m_table->columnWidth(col) + 18, col == ColStrike ? 96 : 84));
        total += m_table->columnWidth(col);
    }
    // Share any spare width across the columns instead of leaving a gap on the right.
    const int available = m_table->viewport()->width();
    if (available > total && total > 0) {
        const double factor = static_cast<double>(available) / total;
        for (int col = 0; col < ColumnCount; ++col) {
            m_table->setColumnWidth(col, static_cast<int>(m_table->columnWidth(col) * factor));
        }
    }
    m_fittingColumns = false;
}

void ChainTab::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    fitColumns();
}

void ChainTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // The viewport only has its real width once the tab is on screen.
    QTimer::singleShot(0, this, [this] { fitColumns(); });
}

QString ChainTab::logoCachePath(const QString& ticker)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/logos";
    QDir().mkpath(dir);
    return dir + "/" + ticker.toUpper() + ".png";
}

void ChainTab::ensureBranding()
{
    const QString symbol = m_state.underlyingTicker;
    if (symbol.isEmpty() || symbol == m_brandingRequested) return;
    m_brandingRequested = symbol;

    // Disk cache first: icons and company details rarely change and the API is rate limited.
    const QString cachePath = logoCachePath(symbol);
    const QString detailsPath = cachePath + ".json";
    QImage cached(cachePath);
    {
        QFile file(detailsPath);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject d = QJsonDocument::fromJson(file.readAll()).object();
            if (!d["name"].toString().isEmpty()) {
                m_state.companyName = d["name"].toString();
                m_state.exchange = d["exchange"].toString();
                m_state.industry = d["industry"].toString();
                m_state.companyDescription = d["description"].toString();
            }
        }
    }
    if (!cached.isNull() && !m_state.companyName.isEmpty() && !m_state.companyDescription.isEmpty()) {
        m_state.logo = cached;
        updateSpotLabels();
        m_state.notify();
        return;
    }
    if (!m_client.hasApiKey()) return;

    m_client.fetchTickerDetails(symbol, [this, symbol, cachePath, detailsPath, cached](const MarketDataClient::TickerDetails& details) {
        if (m_state.underlyingTicker != symbol) return;
        m_state.companyName = details.name;
        m_state.exchange = details.exchange;
        m_state.industry = details.sicDescription;
        m_state.companyDescription = details.description;
        {
            QFile file(detailsPath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(QJsonObject{ { "name", details.name }, { "exchange", details.exchange }, { "industry", details.sicDescription }, { "description", details.description } })
                               .toJson(QJsonDocument::Compact));
            }
        }
        if (!cached.isNull()) {
            m_state.logo = cached;
            m_state.notify();
            return;
        }
        if (details.iconUrl.isEmpty()) {
            m_state.notify();   // name only; the badge stays a monogram
            return;
        }
        m_client.fetchImage(details.iconUrl, [this, symbol, cachePath](const QImage& image) {
            if (m_state.underlyingTicker != symbol) return;
            m_state.logo = image;
            image.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(cachePath, "PNG");
            m_state.notify();
        }, [](const QString&) { /* icon is decorative; silently keep the monogram */ });
    }, [this, symbol](const QString&) {
        // Allow a retry on the next refresh if the lookup failed.
        if (m_brandingRequested == symbol) m_brandingRequested.clear();
    });
}

void ChainTab::updateTimers()
{
    const bool on = m_autoRefresh->isChecked() && !ticker().isEmpty();
    m_chainTimer->setInterval(m_chainInterval->value() * 1000);
    m_spotTimer->setInterval(m_spotInterval->value() * 1000);
    if (on) {
        if (!m_chainTimer->isActive()) m_chainTimer->start();
        if (!m_spotTimer->isActive()) m_spotTimer->start();
    } else {
        m_chainTimer->stop();
        m_spotTimer->stop();
    }
}

void ChainTab::loadTreasuryCurve()
{
    if (m_busy) return;
    if (!m_client.hasApiKey() && !promptForApiKey()) {
        finishLive(false, "No API key available.");
        return;
    }
    setLiveBusy(true, "Fetching the latest US Treasury yields…");
    m_client.fetchTreasuryCurve([this](const MarketDataClient::TreasuryCurve& curve) {
        m_state.rateCurve.setPoints(curve.points);
        m_state.useRateCurve = true;
        m_state.notify();
        QStringList parts;
        for (const RatePoint& p : curve.points) {
            parts << QStringLiteral("%1y %2").arg(ui::number(p.tenor, p.tenor < 1 ? 2 : 0), ui::percent(p.rate, 2));
        }
        finishLive(true, QStringLiteral("Treasury curve as of %1 loaded and enabled (continuously compounded): %2")
                             .arg(curve.date.toString(Qt::ISODate), parts.join(", ")));
    }, [this](const QString& message) { finishLive(false, message); });
}

void ChainTab::loadDividends()
{
    if (m_busy) return;
    const QString symbol = ticker();
    if (symbol.isEmpty()) {
        finishLive(false, "Enter a ticker symbol first.");
        return;
    }
    if (!m_client.hasApiKey() && !promptForApiKey()) {
        finishLive(false, "No API key available.");
        return;
    }
    setLiveBusy(true, QStringLiteral("Fetching %1 dividend history…").arg(symbol));
    m_client.fetchDividends(symbol, QDate::currentDate(), 3.0, [this, symbol](const MarketDataClient::DividendInfo& info) {
        if (info.frequency == 0) {
            m_state.market.dividends.clear();
            m_state.notify();
            finishLive(true, QStringLiteral("%1 pays no regular cash dividend; the dividend schedule was cleared.").arg(symbol));
            return;
        }
        m_state.market.dividends = info.projected;
        m_state.market.dividendYield = 0.0;   // cash dividends replace the continuous yield
        m_state.notify();
        finishLive(true, QStringLiteral("%1: %2 per share, %3× per year (last ex-date %4). %5 projected dividends over three years "
                                        "were placed in the Pricer's cash-dividend schedule and the continuous yield set to zero.")
                             .arg(symbol, ui::number(info.cashAmount, 4)).arg(info.frequency)
                             .arg(info.lastExDate.toString(Qt::ISODate)).arg(info.projected.size()));
    }, [this](const QString& message) { finishLive(false, message); });
}

// MARK: - Theme, export

void ChainTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    showSlice(m_expiry->currentIndex());
}

QString ChainTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    s << "expiry_date,dte,maturity_years,type,strike,bid,ask,mid,implied_vol,fitted_vol,vendor_implied_vol,volume,open_interest,log_moneyness\n";
    for (const ExpirySlice& slice : m_slices) {
        for (const std::vector<ImpliedQuote>* strip : { &slice.calls, &slice.puts }) {
            for (const ImpliedQuote& q : *strip) {
                s << QString::fromStdString(slice.expiryDate) << "," << slice.daysToExpiry << "," << slice.maturity << "," << (q.quote.type == OptionType::Call ? "call" : "put") << "," << q.quote.strike << ","
                  << q.quote.bid << "," << q.quote.ask << "," << q.quote.mid << ","
                  << (q.solved ? QString::number(q.impliedVol) : QString()) << ","
                  << (slice.fitted ? QString::number(slice.smileVol(q.quote.strike)) : QString()) << ","
                  << (q.quote.vendorImpliedVol > 0 ? QString::number(q.quote.vendorImpliedVol) : QString()) << ","
                  << q.quote.volume << "," << q.quote.openInterest << "," << q.logMoneyness << "\n";
            }
        }
    }
    return out;
}
