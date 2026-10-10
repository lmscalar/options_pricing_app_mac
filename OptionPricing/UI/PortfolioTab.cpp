//
//  PortfolioTab.cpp
//  OptionPricing
//

#include "PortfolioTab.h"
#include "Formatting.h"

#include <QtCharts/QAreaSeries>
#include <QtCharts/QCategoryAxis>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <set>

namespace {

constexpr const char* kPositionsKey = "portfolio/positions";     ///< legacy single book, migrated into kBooksKey
constexpr const char* kBooksKey = "portfolio/books";             ///< JSON object: strategy name -> [positions]
constexpr const char* kActiveBookKey = "portfolio/activeBook";
constexpr const char* kAllBooks = "All strategies";
constexpr const char* kDefaultBook = "Default";
constexpr const char* kConfidenceKey = "portfolio/confidence";
constexpr const char* kHorizonKey = "portfolio/horizon";
constexpr const char* kPathsKey = "portfolio/paths";
constexpr const char* kSplitterKey = "portfolio/splitter";
constexpr const char* kCurrencyKey = "portfolio/currency";
constexpr const char* kFxOverridesKey = "portfolio/fxOverrides";

QString currencySymbol(const QString& code)
{
    static const QHash<QString, QString> symbols{ { "USD", "$" }, { "EUR", "€" }, { "GBP", "£" }, { "JPY", "¥" }, { "CHF", "CHF " }, { "CAD", "C$" }, { "AUD", "A$" },
                                                  { "HKD", "HK$" }, { "SGD", "S$" }, { "MXN", "MX$" }, { "BRL", "R$" }, { "INR", "₹" }, { "CNY", "CN¥" }, { "SEK", "kr " }, { "NOK", "kr " }, { "DKK", "kr " } };
    return symbols.value(code.toUpper(), code.toUpper() + " ");
}

QString normalizeCurrency(const QString& code)
{
    const QString c = code.trimmed().toUpper();
    return c.size() == 3 ? c : QStringLiteral("USD");
}

enum Column { ColSymbol = 0, ColBook, ColTrade, ColType, ColCurrency, ColExpiry, ColStrike, ColQty, ColEntry, ColMark, ColIv, ColDelta, ColGamma, ColVega, ColTheta, ColValue, ColPnl, ColPnlPct, ColSource, ColumnCount };

QString kindName(pricing::LegKind kind)
{
    switch (kind) {
    case pricing::LegKind::Call: return "Call";
    case pricing::LegKind::Put: return "Put";
    case pricing::LegKind::Underlying: break;
    }
    return "Stock";
}

pricing::LegKind kindFromName(const QString& text)
{
    const QString t = text.trimmed().toLower();
    if (t.startsWith("c")) return pricing::LegKind::Call;
    if (t.startsWith("p")) return pricing::LegKind::Put;
    return pricing::LegKind::Underlying;
}

QTableWidgetItem* numericCell(double value, int decimals, bool colour = false, const Theme* theme = nullptr, const QString& text = QString())
{
    auto* item = ui::makeCell(text.isEmpty() ? ui::number(value, decimals) : text);
    item->setData(Qt::UserRole, value);
    if (colour && theme && value != 0.0) item->setForeground(QBrush(QColor(value > 0 ? (theme->up.isEmpty() ? "#22c55e" : theme->up) : (theme->down.isEmpty() ? "#ef4444" : theme->down))));
    return item;
}

QJsonObject holdingToJson(const pricing::Holding& h)
{
    return QJsonObject{ { "symbol", QString::fromStdString(h.symbol) }, { "type", kindName(h.kind).toLower() }, { "quantity", h.quantity }, { "strike", h.strike },
                        { "expiry", QString::fromStdString(h.expiryDate) }, { "entry", h.entryPrice }, { "iv", h.volatility * 100.0 }, { "multiplier", h.multiplier },
                        { "currency", QString::fromStdString(h.currency) }, { "tradeDate", QString::fromStdString(h.tradeDate) } };
}

} // namespace

PortfolioTab::PortfolioTab(MarketState& state, QWidget* parent) : QWidget(parent), m_state(state)
{
    buildUi();
    wire();
    loadPositions();
    fillTable();
    fillCards();
    fillWhatIf();
    fillRiskTables();
    fillDistributionChart();
}

// MARK: - UI

void PortfolioTab::buildUi()
{
    // ---- Cards ----
    m_cardValue = ui::makeCard(this, "Net value");
    m_cardPnl = ui::makeCard(this, "Open P&L");
    m_cardDelta = ui::makeCard(this, "Delta per 1%");
    m_cardGamma = ui::makeCard(this, "Gamma per 1%");
    m_cardVega = ui::makeCard(this, "Vega per vol pt");
    m_cardTheta = ui::makeCard(this, "Theta per day");
    m_cardVar = ui::makeCard(this, "VaR");
    m_cardCvar = ui::makeCard(this, "CVaR");
    auto* cards = new QHBoxLayout;
    cards->setSpacing(8);
    for (const ui::Card& card : { m_cardValue, m_cardPnl, m_cardDelta, m_cardGamma, m_cardVega, m_cardTheta, m_cardVar, m_cardCvar }) {
        card.frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        card.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        card.value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        card.subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        cards->addWidget(card.frame, 1);
    }

    // ---- Positions ----
    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Symbol", "Strategy", "Trade date", "Type", "Ccy", "Expiry", "Strike", "Qty", "Entry", "Mark", "IV %", "Delta", "Gamma/1%", "Vega", "Theta/d", "Value", "P&L", "P&L %", "Mark source" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setObjectName("heatmapHeader");
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(24);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->setMinimumHeight(110);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    for (int c = 0; c < ColumnCount; ++c) m_table->setColumnWidth(c, c == ColSymbol ? 70 : (c == ColBook ? 110 : (c == ColCurrency ? 48 : (c == ColExpiry || c == ColTrade ? 92 : (c == ColSource ? 110 : 84)))));

    m_add = ui::makeButton(this, "Add…", "secondary", "Add a stock or option position");
    m_edit = ui::makeButton(this, "Edit…", "secondary", "Edit the selected position");
    m_remove = ui::makeButton(this, "Remove", "secondary", "Remove the selected positions");
    m_importStrategy = ui::makeButton(this, "Import Strategy legs", "secondary", "Add the Strategy tab's legs for the current underlying as positions");
    m_importCsv = ui::makeButton(this, "Import CSV…", "secondary", "Columns: symbol, type (stock/call/put), quantity, strike, expiry (YYYY-MM-DD), entry, iv %, currency (optional), strategy (optional), trade date (optional, YYYY-MM-DD)");
    m_exportCsv = ui::makeButton(this, "Export CSV…", "secondary", "Save the positions with their marks, Greeks and P&L");
    m_refresh = ui::makeButton(this, "Refresh marks", "secondary", "Re-price every position from the latest quotes and stored chains");
    m_currencyBox = new QComboBox(this);
    for (const char* code : { "USD", "EUR", "GBP", "JPY", "CHF", "CAD", "AUD", "HKD", "SGD", "MXN", "BRL", "INR" }) m_currencyBox->addItem(QStringLiteral("%1 %2").arg(currencySymbol(code), code), code);
    m_currencyBox->setToolTip("Reporting currency: values, P&L, Greeks and risk are converted into it at the FX rates (trade prices stay in their own currency)");
    m_fxButton = ui::makeButton(this, "FX rates…", "secondary", "View or override the exchange rates used for conversion");
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    for (QPushButton* b : { m_add, m_edit, m_remove, m_importStrategy, m_importCsv, m_exportCsv }) buttons->addWidget(b);
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_currencyBox);
    buttons->addWidget(m_fxButton);
    buttons->addWidget(m_refresh);

    // What-if row
    auto* whatIfLabel = new QLabel("What if", this);
    whatIfLabel->setObjectName("muted");
    m_whatIfSpot = ui::makeSpinBox(this, -50.0, 50.0, 1.0, 1, 0.0, " % spot");
    m_whatIfVol = ui::makeSpinBox(this, -90.0, 200.0, 5.0, 0, 0.0, " % vol");
    m_whatIfDays = ui::makeIntSpinBox(this, 0, 365, 0);
    m_whatIfDays->setSuffix(" days");
    m_whatIfResult = new QLabel(this);
    m_whatIfResult->setObjectName("value");
    m_whatIfResult->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* whatIf = new QHBoxLayout;
    whatIf->setSpacing(8);
    whatIf->addWidget(whatIfLabel);
    whatIf->addWidget(m_whatIfSpot);
    whatIf->addWidget(m_whatIfVol);
    whatIf->addWidget(m_whatIfDays);
    whatIf->addWidget(m_whatIfResult, 1);

    // Strategy books: one named book per strategy, plus the global view that unions them all.
    auto* bookLabel = new QLabel("Strategy", this);
    bookLabel->setObjectName("muted");
    m_bookBox = new QComboBox(this);
    m_bookBox->setMinimumWidth(200);
    m_bookBox->setToolTip("Which strategy book to show. “All strategies” is the global portfolio: every book together, with a per-strategy breakdown in the risk panel");
    m_bookMenu = new QToolButton(this);
    m_bookMenu->setText("Books ▾");
    m_bookMenu->setPopupMode(QToolButton::InstantPopup);
    m_bookMenu->setCursor(Qt::PointingHandCursor);
    auto* bookMenu = new QMenu(this);
    bookMenu->addAction("New strategy…", this, [this] { promptNewBook(false); });
    bookMenu->addAction("Save as new strategy…", this, [this] { promptNewBook(true); });
    bookMenu->addAction("Rename strategy…", this, [this] { promptRenameBook(); });
    m_deleteBookAction = bookMenu->addAction("Delete strategy", this, [this] { promptDeleteBook(); });
    m_bookMenu->setMenu(bookMenu);
    auto* bookRow = new QHBoxLayout;
    bookRow->setSpacing(8);
    bookRow->addWidget(bookLabel);
    bookRow->addWidget(m_bookBox);
    bookRow->addWidget(m_bookMenu);
    bookRow->addStretch(1);

    auto* positionsPane = new QFrame(this);
    positionsPane->setObjectName("pane");
    auto* positionsLayout = new QVBoxLayout(positionsPane);
    positionsLayout->setContentsMargins(12, 10, 12, 10);
    positionsLayout->setSpacing(8);
    positionsLayout->addLayout(bookRow);
    positionsLayout->addWidget(m_table, 1);
    positionsLayout->addLayout(buttons);
    positionsLayout->addLayout(whatIf);

    // ---- Risk ----
    m_confidence = new QComboBox(this);
    m_confidence->addItem("95%", 0.95);
    m_confidence->addItem("97.5%", 0.975);
    m_confidence->addItem("99%", 0.99);
    m_confidence->setToolTip("Confidence level: VaR is the loss not exceeded with this probability; CVaR the average loss beyond it");
    m_horizon = ui::makeIntSpinBox(this, 1, 60, 1);
    m_horizon->setSuffix(" day horizon");
    m_horizon->setToolTip("Holding period in trading days");
    m_paths = ui::makeIntSpinBox(this, 1000, 200000, 20000);
    m_paths->setSingleStep(5000);
    m_paths->setSuffix(" paths");
    m_paths->setToolTip("Monte Carlo paths");
    m_chartMethod = new QComboBox(this);
    m_chartMethod->addItem("Monte Carlo distribution", "mc");
    m_chartMethod->addItem("Historical distribution", "hist");
    m_run = ui::makeButton(this, "Run risk", "primary", "Compute parametric, historical and Monte Carlo VaR / CVaR, the stress grid and the decay ladder");
    m_riskStatus = new QLabel(this);
    m_riskStatus->setObjectName("muted");
    m_riskStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* riskControls = new QHBoxLayout;
    riskControls->setSpacing(8);
    riskControls->addWidget(m_confidence);
    riskControls->addWidget(m_horizon);
    riskControls->addWidget(m_paths);
    riskControls->addWidget(m_chartMethod);
    riskControls->addWidget(m_riskStatus, 1);
    riskControls->addWidget(m_run);

    auto makeTable = [this](const QStringList& headers) {
        auto* t = new QTableWidget(0, static_cast<int>(headers.size()), this);
        t->setHorizontalHeaderLabels(headers);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        t->horizontalHeader()->setObjectName("heatmapHeader");
        t->horizontalHeader()->setFixedHeight(26);
        t->verticalHeader()->setVisible(false);
        t->verticalHeader()->setDefaultSectionSize(24);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->setSelectionMode(QAbstractItemView::NoSelection);
        t->setAlternatingRowColors(true);
        t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        t->setMinimumHeight(50);
        t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        return t;
    };
    m_varTable = makeTable({ "Method", "VaR", "CVaR", "Mean P&L", "Std dev", "Note" });
    m_varTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_componentTable = makeTable({ "Underlying", "Value", "Delta/1%", "Gamma/1%", "Vega", "Theta/d", "Component VaR" });
    m_stressTable = makeTable({ "Spot \\ Vol", "−50%", "−25%", "0%", "+25%", "+50%" });
    m_stressTable->verticalHeader()->setVisible(false);
    m_decayLabel = new QLabel(this);
    m_decayLabel->setObjectName("muted");
    m_decayLabel->setWordWrap(true);

    m_distributionChart = new QChart;
    m_distributionChart->setTitle("Simulated P&L distribution");
    m_distributionChart->legend()->setVisible(false);
    m_distributionView = ui::makeHoverChartView(this, m_distributionChart, 120);
    m_distributionView->setToolTip("Hover to read the P&L, the paths in that bin and the probability of a worse outcome");

    auto* leftRisk = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftRisk);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);
    auto* varTitle = new QLabel("Value at risk and expected shortfall", this);
    varTitle->setObjectName("sectionTitle");
    leftLayout->addWidget(varTitle);
    leftLayout->addWidget(m_varTable);
    m_strategyTitle = new QLabel("By strategy", this);
    m_strategyTitle->setObjectName("sectionTitle");
    m_strategyTable = makeTable({ "Strategy", "Positions", "Value", "P&L", "Delta/1%", "Vega", "Theta/d", "Standalone VaR" });
    m_strategyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_strategyTable->setColumnWidth(0, 150);
    m_strategyNote = new QLabel(this);
    m_strategyNote->setObjectName("muted");
    m_strategyNote->setWordWrap(true);
    leftLayout->addWidget(m_strategyTitle);
    leftLayout->addWidget(m_strategyTable);
    leftLayout->addWidget(m_strategyNote);
    auto* compTitle = new QLabel("By underlying", this);
    compTitle->setObjectName("sectionTitle");
    leftLayout->addWidget(compTitle);
    leftLayout->addWidget(m_componentTable, 1);

    auto* rightRisk = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightRisk);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(8);
    rightLayout->addWidget(m_distributionView, 1);
    auto* stressTitle = new QLabel("Stress P&L: spot shock (rows) × vol shock (columns)", this);
    stressTitle->setObjectName("sectionTitle");
    rightLayout->addWidget(stressTitle);
    rightLayout->addWidget(m_stressTable);
    rightLayout->addWidget(m_decayLabel);

    auto* riskSplit = new QSplitter(Qt::Horizontal, this);
    riskSplit->setObjectName("quotesSplitter");
    riskSplit->setHandleWidth(8);
    riskSplit->setChildrenCollapsible(false);
    riskSplit->addWidget(leftRisk);
    riskSplit->addWidget(rightRisk);
    riskSplit->setStretchFactor(0, 1);
    riskSplit->setStretchFactor(1, 1);

    auto* riskPane = new QFrame(this);
    riskPane->setObjectName("pane");
    auto* riskLayout = new QVBoxLayout(riskPane);
    riskLayout->setContentsMargins(12, 10, 12, 10);
    riskLayout->setSpacing(8);
    riskLayout->addLayout(riskControls);
    riskLayout->addWidget(riskSplit, 1);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(positionsPane);
    m_splitter->addWidget(riskPane);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({ 360, 440 });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 10);
    root->setSpacing(8);
    root->addLayout(cards);
    root->addWidget(m_splitter, 1);

    const QSettings settings;
    const int confidenceIndex = m_confidence->findData(settings.value(kConfidenceKey, 0.95).toDouble());
    m_confidence->setCurrentIndex(std::max(0, confidenceIndex));
    m_horizon->setValue(settings.value(kHorizonKey, 1).toInt());
    m_paths->setValue(settings.value(kPathsKey, 20000).toInt());
    const QByteArray splitterState = settings.value(kSplitterKey).toByteArray();
    if (!splitterState.isEmpty()) m_splitter->restoreState(splitterState);
    m_currency = normalizeCurrency(settings.value(kCurrencyKey, "USD").toString());
    m_currencyBox->setCurrentIndex(std::max(0, m_currencyBox->findData(m_currency)));
    const QJsonObject overrides = QJsonDocument::fromJson(settings.value(kFxOverridesKey).toByteArray()).object();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) if (it.value().toDouble() > 0.0) m_fxOverrides[it.key()] = it.value().toDouble();
    updateCurrencyHeaders();
}

void PortfolioTab::wire()
{
    connect(m_add, &QPushButton::clicked, this, [this] { promptPosition(-1); });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        const auto rows = m_table->selectionModel()->selectedRows();
        if (!rows.isEmpty()) promptPosition(m_table->item(rows.first().row(), ColSymbol)->data(Qt::UserRole + 1).toInt());
    });
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        if (item) promptPosition(m_table->item(item->row(), ColSymbol)->data(Qt::UserRole + 1).toInt());
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = m_table->selectionModel()->selectedRows();
        if (rows.size() == 1 && onTickerSelected && m_table->hasFocus()) onTickerSelected(m_table->item(rows.first().row(), ColSymbol)->text());
    });
    connect(m_remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(m_importStrategy, &QPushButton::clicked, this, [this] {
        if (!strategyProvider) return;
        const int added = importStrategy(strategyProvider(), m_state.underlyingTicker);
        setStatus(added ? QStringLiteral("Imported %1 leg(s) from the Strategy tab for %2.").arg(added).arg(m_state.underlyingTicker)
                        : QStringLiteral("The Strategy tab has no legs to import (load a chain and a preset first)."), added ? ui::StatusKind::Info : ui::StatusKind::Warning);
    });
    connect(m_importCsv, &QPushButton::clicked, this, [this] { importCsv(); });
    connect(m_exportCsv, &QPushButton::clicked, this, [this] { exportCsv(); });
    connect(m_refresh, &QPushButton::clicked, this, [this] { refreshMarks(); });
    connect(m_currencyBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        m_currency = m_currencyBox->currentData().toString();
        QSettings().setValue(kCurrencyKey, m_currency);
        updateCurrencyHeaders();
        fetchFx();
        rebuildRows();
        if (m_riskRunAt.isValid()) runRisk();
    });
    connect(m_fxButton, &QPushButton::clicked, this, [this] { promptFxRates(); });
    connect(m_bookBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updatingBooks) return;
        loadPortfolio(m_bookBox->currentData().toString());
    });
    for (QDoubleSpinBox* box : { m_whatIfSpot, m_whatIfVol }) connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { fillWhatIf(); });
    connect(m_whatIfDays, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { fillWhatIf(); });
    connect(m_run, &QPushButton::clicked, this, [this] { runRisk(); });
    connect(m_confidence, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { QSettings().setValue(kConfidenceKey, m_confidence->currentData().toDouble()); });
    connect(m_horizon, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { QSettings().setValue(kHorizonKey, v); });
    connect(m_paths, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { QSettings().setValue(kPathsKey, v); });
    connect(m_chartMethod, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { fillDistributionChart(); });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int) { QSettings().setValue(kSplitterKey, m_splitter->saveState()); });
    m_state.subscribe([this] {
        // The headline spot moved: keep that underlying's marks in step without a network round trip.
        if (m_state.underlyingTicker.isEmpty() || m_state.market.spot <= 0.0) return;
        const auto it = m_spots.find(m_state.underlyingTicker);
        if (it == m_spots.end() || std::fabs(it->second - m_state.market.spot) < 1e-9) return;
        it->second = m_state.market.spot;
        fillTable(); fillCards(); fillWhatIf();
    });
}

void PortfolioTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    styleChart(m_distributionChart, theme);
    m_distributionView->lineColour = QColor(theme.textMuted);
    m_distributionView->boxBorder = QColor(theme.border);
    m_distributionView->textColour = QColor(theme.textStrong);
    QColor bg(theme.surface); bg.setAlpha(235);
    m_distributionView->boxBackground = bg;
    fillTable();
    fillCards();
    fillRiskTables();
    fillDistributionChart();
}

void PortfolioTab::setStatus(const QString& text, ui::StatusKind kind) { ui::setStatus(m_status, text, kind); }

// MARK: - Positions

pricing::Holding PortfolioTab::holdingFromJson(const QJsonObject& o)
{
    pricing::Holding h;
    h.symbol = o.value("symbol").toString().toUpper().toStdString();
    h.kind = kindFromName(o.value("type").toString());
    h.quantity = o.value("quantity").toDouble();
    h.strike = o.value("strike").toDouble();
    h.expiryDate = o.value("expiry").toString().toStdString();
    h.entryPrice = o.value("entry").toDouble();
    h.volatility = o.value("iv").toDouble() / 100.0;
    h.multiplier = o.value("multiplier").toDouble(h.isOption() ? 100.0 : 1.0);
    h.currency = normalizeCurrency(o.value("currency").toString()).toStdString();
    h.tradeDate = o.value("tradeDate").toString().toStdString();
    return h;
}

void PortfolioTab::loadPositions()
{
    m_books.clear();
    QSettings settings;
    const QJsonObject books = QJsonDocument::fromJson(settings.value(kBooksKey).toByteArray()).object();
    for (auto it = books.begin(); it != books.end(); ++it) {
        std::vector<pricing::Holding> list;
        for (const QJsonValue v : it.value().toArray()) {
            pricing::Holding h = holdingFromJson(v.toObject());
            if (!h.symbol.empty() && h.quantity != 0.0) list.push_back(h);
        }
        m_books[it.key()] = list;
    }
    if (m_books.isEmpty()) {
        // First run with books, or an upgrade: the old single list becomes the Default strategy.
        std::vector<pricing::Holding> legacy;
        for (const QJsonValue v : QJsonDocument::fromJson(settings.value(kPositionsKey).toByteArray()).array()) {
            pricing::Holding h = holdingFromJson(v.toObject());
            if (!h.symbol.empty() && h.quantity != 0.0) legacy.push_back(h);
        }
        m_books[kDefaultBook] = legacy;
    }
    m_activeBook = settings.value(kActiveBookKey, m_books.size() > 1 ? QString(kAllBooks) : m_books.firstKey()).toString();
    if (m_activeBook != kAllBooks && !m_books.contains(m_activeBook)) m_activeBook = m_books.firstKey();
    rebuildWorkingBook();
    refreshBookCombo();
}

void PortfolioTab::savePositions() const
{
    QJsonObject books;
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it) {
        QJsonArray list;
        for (const pricing::Holding& h : it.value()) list.append(holdingToJson(h));
        books[it.key()] = list;
    }
    QSettings settings;
    settings.setValue(kBooksKey, QJsonDocument(books).toJson(QJsonDocument::Compact));
    settings.setValue(kActiveBookKey, m_activeBook);
    settings.remove(kPositionsKey);
}

QString PortfolioTab::targetBook() const
{
    return m_activeBook == kAllBooks ? (m_books.isEmpty() ? QString(kDefaultBook) : m_books.firstKey()) : m_activeBook;
}

void PortfolioTab::rebuildWorkingBook()
{
    m_book.holdings.clear();
    if (m_activeBook == kAllBooks) {
        for (auto it = m_books.cbegin(); it != m_books.cend(); ++it) {
            for (pricing::Holding h : it.value()) { h.group = it.key().toStdString(); m_book.holdings.push_back(h); }
        }
    } else {
        for (pricing::Holding h : m_books.value(m_activeBook)) { h.group = m_activeBook.toStdString(); m_book.holdings.push_back(h); }
    }
}

void PortfolioTab::commitWorkingBook()
{
    // The working set is the view; write it back into the books it belongs to.
    if (m_activeBook == kAllBooks) {
        for (auto it = m_books.begin(); it != m_books.end(); ++it) it.value().clear();
        for (const pricing::Holding& h : m_book.holdings) {
            const QString book = h.group.empty() ? targetBook() : QString::fromStdString(h.group);
            m_books[book].push_back(h);
        }
    } else {
        m_books[m_activeBook] = m_book.holdings;
    }
    savePositions();
    refreshBookCombo();
}

void PortfolioTab::refreshBookCombo()
{
    m_updatingBooks = true;
    m_bookBox->clear();
    size_t total = 0;
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it) total += it.value().size();
    m_bookBox->addItem(QStringLiteral("%1 (%2 positions)").arg(kAllBooks).arg(total), kAllBooks);
    for (auto it = m_books.cbegin(); it != m_books.cend(); ++it) m_bookBox->addItem(QStringLiteral("%1 (%2)").arg(it.key()).arg(it.value().size()), it.key());
    m_bookBox->setCurrentIndex(std::max(0, m_bookBox->findData(m_activeBook)));
    m_updatingBooks = false;
    m_deleteBookAction->setEnabled(m_activeBook != kAllBooks && m_books.size() > 1);
    // The book selector explains itself in its tooltip rather than in a line of text.
    m_bookBox->setToolTip(m_activeBook == kAllBooks ? QStringLiteral("Global portfolio: %1 strateg%2 together. New positions go to “%3” unless a strategy is chosen in the dialog.")
                                                           .arg(m_books.size()).arg(m_books.size() == 1 ? "y" : "ies", targetBook())
                                                     : QStringLiteral("Showing one strategy. Pick “%1” for the whole book.").arg(kAllBooks));
    m_strategyTitle->setVisible(m_activeBook == kAllBooks);
    m_strategyTable->setVisible(m_activeBook == kAllBooks);
    m_strategyNote->setVisible(m_activeBook == kAllBooks);
}

QStringList PortfolioTab::portfolioNames() const { return m_books.keys(); }

bool PortfolioTab::loadPortfolio(const QString& name)
{
    QString wanted = name.trimmed();
    if (wanted.compare("all", Qt::CaseInsensitive) == 0 || wanted.compare("global", Qt::CaseInsensitive) == 0 || wanted.compare(kAllBooks, Qt::CaseInsensitive) == 0) wanted = kAllBooks;
    else {
        bool found = false;
        for (const QString& key : m_books.keys()) if (key.compare(wanted, Qt::CaseInsensitive) == 0) { wanted = key; found = true; break; }
        if (!found) return false;
    }
    if (wanted == m_activeBook) return true;
    m_activeBook = wanted;
    QSettings().setValue(kActiveBookKey, m_activeBook);
    rebuildWorkingBook();
    refreshBookCombo();
    m_riskRunAt = QDateTime();
    m_parametric = m_historical = m_monteCarlo = pricing::risk::VarResult{};
    m_stress = pricing::risk::StressGrid{};
    m_decay.clear();
    refreshMarks();
    fillRiskTables();
    fillDistributionChart();
    return true;
}

bool PortfolioTab::createPortfolio(const QString& name, bool copyCurrent, bool activate)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty() || clean.compare(kAllBooks, Qt::CaseInsensitive) == 0 || m_books.contains(clean)) return false;
    m_books[clean] = copyCurrent ? m_book.holdings : std::vector<pricing::Holding>{};
    for (pricing::Holding& h : m_books[clean]) h.group.clear();
    savePositions();
    refreshBookCombo();
    if (activate) loadPortfolio(clean);
    return true;
}

bool PortfolioTab::renamePortfolio(const QString& from, const QString& to)
{
    const QString clean = to.trimmed();
    if (!m_books.contains(from) || clean.isEmpty() || m_books.contains(clean) || clean.compare(kAllBooks, Qt::CaseInsensitive) == 0) return false;
    m_books[clean] = m_books.take(from);
    if (m_activeBook == from) m_activeBook = clean;
    savePositions();
    rebuildWorkingBook();
    refreshBookCombo();
    rebuildRows();
    return true;
}

bool PortfolioTab::deletePortfolio(const QString& name)
{
    if (!m_books.contains(name) || m_books.size() <= 1) return false;
    m_books.remove(name);
    if (m_activeBook == name) m_activeBook = kAllBooks;
    savePositions();
    rebuildWorkingBook();
    refreshBookCombo();
    refreshMarks();
    return true;
}

void PortfolioTab::promptNewBook(bool copyCurrent)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, copyCurrent ? "Save as new strategy" : "New strategy", "Strategy name:", QLineEdit::Normal,
                                               copyCurrent && m_activeBook != kAllBooks ? m_activeBook + " copy" : QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!createPortfolio(name, copyCurrent, true)) setStatus(QStringLiteral("A strategy named “%1” already exists (or the name is reserved).").arg(name), ui::StatusKind::Error);
    else setStatus(QStringLiteral("Strategy “%1” created%2.").arg(name, copyCurrent ? QStringLiteral(" with %1 position(s)").arg(m_book.holdings.size()) : QString()), ui::StatusKind::Info);
}

void PortfolioTab::promptRenameBook()
{
    if (m_activeBook == kAllBooks) { setStatus("Choose a strategy first (the global view cannot be renamed).", ui::StatusKind::Warning); return; }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Rename strategy", "New name:", QLineEdit::Normal, m_activeBook, &ok).trimmed();
    if (!ok || name.isEmpty() || name == m_activeBook) return;
    if (!renamePortfolio(m_activeBook, name)) setStatus(QStringLiteral("Cannot rename to “%1”.").arg(name), ui::StatusKind::Error);
}

void PortfolioTab::promptDeleteBook()
{
    if (m_activeBook == kAllBooks || m_books.size() <= 1) return;
    if (QMessageBox::question(this, "Delete strategy", QStringLiteral("Delete the strategy “%1” and its %2 position(s)?").arg(m_activeBook).arg(m_book.holdings.size())) != QMessageBox::Yes) return;
    deletePortfolio(m_activeBook);
}

bool PortfolioTab::addHolding(const QJsonObject& spec, QString* error)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    pricing::Holding h;
    h.symbol = spec.value("symbol").toString().trimmed().toUpper().toStdString();
    if (h.symbol.empty()) return fail("A position needs a symbol.");
    h.kind = kindFromName(spec.value("type").toString("stock"));
    h.quantity = spec.value("quantity").toDouble(spec.value("qty").toDouble());
    if (h.quantity == 0.0) return fail("Quantity must be non-zero (negative for short).");
    h.entryPrice = spec.value("entry").toDouble(spec.value("price").toDouble());
    h.multiplier = h.isOption() ? spec.value("multiplier").toDouble(100.0) : 1.0;
    h.currency = normalizeCurrency(spec.value("currency").toString(m_currency)).toStdString();
    const QString tradeDate = spec.value("tradeDate").toString(spec.value("trade_date").toString()).trimmed();
    if (!tradeDate.isEmpty() && !QDate::fromString(tradeDate, Qt::ISODate).isValid()) return fail("The trade date must be an ISO date (YYYY-MM-DD).");
    h.tradeDate = (tradeDate.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : tradeDate).toStdString();
    if (h.isOption()) {
        h.strike = spec.value("strike").toDouble();
        h.expiryDate = spec.value("expiry").toString().trimmed().toStdString();
        if (h.strike <= 0.0) return fail("An option needs a strike.");
        if (!QDate::fromString(QString::fromStdString(h.expiryDate), Qt::ISODate).isValid()) return fail("An option needs an ISO expiry date (YYYY-MM-DD).");
        h.volatility = spec.value("iv").toDouble() / 100.0;
    }
    QString book = spec.value("portfolio").toString(spec.value("strategy").toString()).trimmed();
    if (book.isEmpty()) book = targetBook();
    else {
        bool found = false;
        for (const QString& key : m_books.keys()) if (key.compare(book, Qt::CaseInsensitive) == 0) { book = key; found = true; break; }
        if (!found) m_books[book] = {};   // a new strategy named in the request
    }
    h.group = book.toStdString();
    if (m_activeBook != kAllBooks && book != m_activeBook) {
        // Added to another strategy than the one on screen: store it there and leave the view alone.
        m_books[book].push_back(h);
        savePositions();
        refreshBookCombo();
        return true;
    }
    m_book.holdings.push_back(h);
    commitWorkingBook();
    refreshMarks();
    return true;
}

int PortfolioTab::removeHoldings(const QString& symbol, const QString& type, double strike)
{
    const std::string sym = symbol.trimmed().toUpper().toStdString();
    const bool anyType = type.trimmed().isEmpty();
    const pricing::LegKind kind = kindFromName(type);
    int removed = 0;
    std::vector<pricing::Holding> keep;
    for (const pricing::Holding& h : m_book.holdings) {
        const bool match = (sym == "ALL" || h.symbol == sym) && (anyType || h.kind == kind) && (strike <= 0.0 || std::fabs(h.strike - strike) < 1e-9);
        if (match) ++removed; else keep.push_back(h);
    }
    if (removed) { m_book.holdings = keep; commitWorkingBook(); refreshMarks(); }
    return removed;
}

int PortfolioTab::importStrategy(const pricing::Position& position, const QString& symbol)
{
    if (symbol.isEmpty()) return 0;
    int added = 0;
    const QDate today = QDate::currentDate();
    for (const pricing::Leg& leg : position.legs) {
        if (leg.quantity == 0.0) continue;
        pricing::Holding h;
        h.symbol = symbol.toUpper().toStdString();
        h.kind = leg.kind;
        h.quantity = leg.quantity;
        h.strike = leg.strike;
        h.maturity = leg.maturity;
        h.expiryDate = leg.expiryDate.empty() ? today.addDays(qRound(leg.maturity * 365.0)).toString(Qt::ISODate).toStdString() : leg.expiryDate;
        h.entryPrice = leg.entryPrice > 0.0 ? leg.entryPrice : leg.marketPrice;
        h.volatility = leg.volatility;
        h.multiplier = leg.kind == pricing::LegKind::Underlying ? 1.0 : position.multiplier;
        if (leg.kind == pricing::LegKind::Underlying) h.entryPrice = leg.entryPrice > 0.0 ? leg.entryPrice : m_state.market.spot;
        h.currency = "USD";   // chains and quotes from the vendor are US-listed
        h.group = targetBook().toStdString();
        h.tradeDate = QDate::currentDate().toString(Qt::ISODate).toStdString();
        m_book.holdings.push_back(h);
        ++added;
    }
    if (added) { commitWorkingBook(); refreshMarks(); }
    return added;
}

void PortfolioTab::promptPosition(int editRow)
{
    const bool editing = editRow >= 0 && editRow < static_cast<int>(m_book.holdings.size());
    pricing::Holding current = editing ? m_book.holdings[static_cast<size_t>(editRow)] : pricing::Holding{};
    QDialog dialog(this);
    dialog.setWindowTitle(editing ? "Edit position" : "Add position");
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(14, 12, 14, 10);
    form->setSpacing(8);
    auto* symbol = new QLineEdit(QString::fromStdString(current.symbol), &dialog);
    symbol->setPlaceholderText("e.g. AAPL");
    symbol->setMaxLength(8);
    auto* type = new QComboBox(&dialog);
    type->addItems({ "Stock", "Call", "Put" });
    type->setCurrentIndex(current.kind == pricing::LegKind::Call ? 1 : (current.kind == pricing::LegKind::Put ? 2 : 0));
    auto* quantity = ui::makeSpinBox(&dialog, -1e7, 1e7, 1.0, 0, current.quantity == 0.0 ? 1.0 : current.quantity);
    quantity->setToolTip("Shares for stock, contracts for options; negative = short");
    auto* strike = ui::makeSpinBox(&dialog, 0.0, 1e6, 1.0, 2, current.strike);
    auto* expiry = new QDateEdit(&dialog);
    expiry->setCalendarPopup(true);
    expiry->setDisplayFormat("yyyy-MM-dd");
    const QDate currentExpiry = QDate::fromString(QString::fromStdString(current.expiryDate), Qt::ISODate);
    expiry->setDate(currentExpiry.isValid() ? currentExpiry : QDate::currentDate().addDays(30));
    auto* tradeDate = new QDateEdit(&dialog);
    tradeDate->setCalendarPopup(true);
    tradeDate->setDisplayFormat("yyyy-MM-dd");
    tradeDate->setMaximumDate(QDate::currentDate());
    const QDate currentTrade = QDate::fromString(QString::fromStdString(current.tradeDate), Qt::ISODate);
    tradeDate->setDate(currentTrade.isValid() ? currentTrade : QDate::currentDate());
    tradeDate->setToolTip("Date the trade was entered");
    auto* entry = ui::makeSpinBox(&dialog, 0.0, 1e7, 0.01, 2, current.entryPrice);
    entry->setToolTip("Price paid (or received) per share / per contract unit");
    auto* iv = ui::makeSpinBox(&dialog, 0.0, 500.0, 1.0, 1, current.volatility * 100.0, " %");
    iv->setToolTip("Implied volatility used to value the option when no stored chain quote matches (0 = use the chain or the market vol)");
    auto* currency = new QComboBox(&dialog);
    for (int i = 0; i < m_currencyBox->count(); ++i) currency->addItem(m_currencyBox->itemText(i), m_currencyBox->itemData(i));
    currency->setEditable(true);
    currency->setCurrentIndex(std::max(0, currency->findData(editing ? QString::fromStdString(current.currency) : m_currency)));
    currency->setToolTip("Currency of the entry price and the quotes for this instrument");
    auto* book = new QComboBox(&dialog);
    for (const QString& name : m_books.keys()) book->addItem(name);
    book->setEditable(true);
    book->setCurrentText(editing && !current.group.empty() ? QString::fromStdString(current.group) : targetBook());
    book->setToolTip("Strategy book the position belongs to (type a new name to create one)");
    form->addRow("Symbol", symbol);
    form->addRow("Strategy", book);
    form->addRow("Trade date", tradeDate);
    form->addRow("Type", type);
    form->addRow("Currency", currency);
    form->addRow("Quantity", quantity);
    form->addRow("Strike", strike);
    form->addRow("Expiry", expiry);
    form->addRow("Entry price", entry);
    form->addRow("Implied vol", iv);
    auto syncEnabled = [&] { const bool option = type->currentIndex() > 0; strike->setEnabled(option); expiry->setEnabled(option); iv->setEnabled(option); };
    syncEnabled();
    connect(type, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) { syncEnabled(); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(editing ? "Apply" : "Add");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString chosenCurrency = currency->currentData().isValid() && currency->currentText() == currency->itemText(currency->currentIndex()) ? currency->currentData().toString() : currency->currentText().right(3);
    QJsonObject spec{ { "symbol", symbol->text() }, { "type", type->currentText().toLower() }, { "quantity", quantity->value() }, { "strike", strike->value() },
                      { "expiry", expiry->date().toString(Qt::ISODate) }, { "entry", entry->value() }, { "iv", iv->value() }, { "currency", chosenCurrency },
                      { "portfolio", book->currentText().trimmed() }, { "tradeDate", tradeDate->date().toString(Qt::ISODate) } };
    if (editing) m_book.holdings.erase(m_book.holdings.begin() + editRow);
    QString error;
    if (!addHolding(spec, &error)) {
        if (editing) m_book.holdings.insert(m_book.holdings.begin() + editRow, current);
        setStatus(error, ui::StatusKind::Error);
    }
}

void PortfolioTab::removeSelected()
{
    std::set<int> indices;
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) indices.insert(m_table->item(index.row(), ColSymbol)->data(Qt::UserRole + 1).toInt());
    if (indices.empty()) return;
    std::vector<pricing::Holding> keep;
    for (size_t i = 0; i < m_book.holdings.size(); ++i) if (!indices.count(static_cast<int>(i))) keep.push_back(m_book.holdings[i]);
    m_book.holdings = keep;
    commitWorkingBook();
    refreshMarks();
}

void PortfolioTab::importCsv()
{
    const QString path = QFileDialog::getOpenFileName(this, "Import positions", QString(), "CSV files (*.csv);;All files (*)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { setStatus("Could not open the file.", ui::StatusKind::Error); return; }
    int added = 0, skipped = 0;
    QString error;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QStringList cells = line.split(',');
        if (cells.size() < 3 || cells[0].trimmed().compare("symbol", Qt::CaseInsensitive) == 0) continue;
        QJsonObject spec{ { "symbol", cells.value(0).trimmed() }, { "type", cells.value(1).trimmed() }, { "quantity", cells.value(2).trimmed().toDouble() },
                          { "strike", cells.value(3).trimmed().toDouble() }, { "expiry", cells.value(4).trimmed() }, { "entry", cells.value(5).trimmed().toDouble() },
                          { "iv", cells.value(6).trimmed().toDouble() } };
        // Add without refreshing marks per line; one refresh at the end.
        pricing::Holding h;
        h.symbol = spec.value("symbol").toString().toUpper().toStdString();
        h.kind = kindFromName(spec.value("type").toString());
        h.quantity = spec.value("quantity").toDouble();
        h.strike = spec.value("strike").toDouble();
        h.expiryDate = spec.value("expiry").toString().toStdString();
        h.entryPrice = spec.value("entry").toDouble();
        h.volatility = spec.value("iv").toDouble() / 100.0;
        h.multiplier = h.isOption() ? 100.0 : 1.0;
        h.currency = normalizeCurrency(cells.value(7).trimmed().isEmpty() ? m_currency : cells.value(7).trimmed()).toStdString();
        if (h.symbol.empty() || h.quantity == 0.0 || (h.isOption() && (h.strike <= 0.0 || !QDate::fromString(QString::fromStdString(h.expiryDate), Qt::ISODate).isValid()))) { ++skipped; continue; }
        h.group = (cells.size() > 8 && !cells.value(8).trimmed().isEmpty() ? cells.value(8).trimmed() : targetBook()).toStdString();
        const QDate traded = QDate::fromString(cells.value(9).trimmed(), Qt::ISODate);
        h.tradeDate = (traded.isValid() ? traded : QDate::currentDate()).toString(Qt::ISODate).toStdString();
        if (!m_books.contains(QString::fromStdString(h.group))) m_books[QString::fromStdString(h.group)] = {};
        m_book.holdings.push_back(h);
        ++added;
    }
    commitWorkingBook();
    refreshMarks();
    setStatus(QStringLiteral("Imported %1 position(s)%2.").arg(added).arg(skipped ? QStringLiteral(", skipped %1 malformed line(s)").arg(skipped) : QString()), ui::StatusKind::Info);
}

void PortfolioTab::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, "Export positions", "portfolio.csv", "CSV files (*.csv)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { setStatus("Could not write the file.", ui::StatusKind::Error); return; }
    file.write(resultsCsv().toUtf8());
    setStatus(QStringLiteral("Saved %1.").arg(path), ui::StatusKind::Info);
}

// MARK: - Marks and markets

pricing::Markets PortfolioTab::markets() const
{
    pricing::Markets out;
    for (const pricing::Holding& h : m_book.holdings) {
        const QString symbol = QString::fromStdString(h.symbol);
        pricing::Market& m = out[h.symbol];
        m.model = pricing::Model::BlackScholesMerton;
        m.dayBasis = m_state.market.dayBasis;
        const auto spot = m_spots.find(symbol);
        if (spot != m_spots.end() && spot->second > 0.0) m.spot = spot->second;
        else if (symbol == m_state.underlyingTicker && m_state.market.spot > 0.0) m.spot = m_state.market.spot;
        else if (!h.isOption() && h.entryPrice > 0.0) m.spot = h.entryPrice;
        else if (h.isOption() && h.strike > 0.0) m.spot = h.strike;
        m.riskFreeRate = m_state.rateFor(0.5);
        m.dividendYield = symbol == m_state.underlyingTicker ? m_state.market.dividendYield : 0.0;
    }
    // Default vol per underlying: the average implied vol of its option holdings, else the market vol.
    for (auto& [symbol, m] : out) {
        double sum = 0.0;
        int n = 0;
        for (const pricing::Holding& h : m_book.holdings) if (h.symbol == symbol && h.isOption() && h.volatility > 0.0) { sum += h.volatility; ++n; }
        m.volatility = n ? sum / n : (m_state.market.volatility > 0.0 ? m_state.market.volatility : 0.30);
    }
    return out;
}

void PortfolioTab::markFromChain(pricing::Holding& h, QString* source) const
{
    *source = h.isOption() ? "model" : "snapshot";
    h.markPrice = 0.0;
    if (!h.isOption() || !m_store) return;
    const auto chain = m_store->get(QString::fromStdString(h.symbol));
    if (!chain) return;
    const pricing::OptionType type = h.kind == pricing::LegKind::Put ? pricing::OptionType::Put : pricing::OptionType::Call;
    for (const pricing::ChainQuote& q : chain->download.quotes) {
        if (q.type != type || std::fabs(q.strike - h.strike) > 1e-6 || q.expiryDate != h.expiryDate) continue;
        if (q.mid > 0.0) { h.markPrice = q.mid; *source = "chain mid"; }
        if (q.vendorImpliedVol > 0.0) h.volatility = q.vendorImpliedVol;
        else if (q.mid > 0.0) {
            pricing::Inputs in;
            in.spot = chain->snapshot.price > 0.0 ? chain->snapshot.price : m_state.market.spot;
            in.strike = q.strike;
            in.riskFreeRate = m_state.rateFor(q.maturity);
            in.dividendYield = 0.0;
            in.maturity = std::max(q.maturity, 1e-4);
            in.volatility = 0.3;
            const pricing::ImpliedVolResult iv = pricing::impliedVolatility(in, type, q.mid);
            if (iv.status == pricing::ImpliedVolResult::Status::Converged) h.volatility = iv.volatility;
        }
        return;
    }
}

void PortfolioTab::refreshMarks()
{
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    QStringList symbols;
    for (pricing::Holding& h : m_book.holdings) {
        const QString s = QString::fromStdString(h.symbol);
        if (!symbols.contains(s)) symbols << s;
        if (h.isOption()) h.maturity = pricing::maturityFromDates(today.toStdString(), h.expiryDate);
    }
    auto rebuild = [this] { rebuildRows(); };
    rebuild();
    fetchFx();
    if (symbols.isEmpty()) { setStatus("No positions.", ui::StatusKind::Info); return; }
    setStatus(QStringLiteral("Refreshing marks for %1 symbol(s)…").arg(symbols.size()), ui::StatusKind::Info);
    m_client.fetchQuotes(symbols, [this, rebuild](const std::vector<MarketDataClient::Quote>& quotes) {
        for (const MarketDataClient::Quote& q : quotes) {
            if (q.last > 0.0) m_spots[q.ticker] = q.last;
            if (q.previousClose > 0.0) m_previousCloses[q.ticker] = q.previousClose;
            if (q.asOf.isValid() && q.asOf > m_marksAsOf) m_marksAsOf = q.asOf;
        }
        rebuild();
        int chainMarks = 0;
        for (const Row& r : m_rows) if (r.markSource == "chain mid") ++chainMarks;
        setStatus(QStringLiteral("%1 position(s) marked%2 · %3 option(s) priced from stored chains · 15-minute delayed data")
                      .arg(m_rows.size()).arg(m_marksAsOf.isValid() ? QStringLiteral(" at %1").arg(m_marksAsOf.toLocalTime().toString("HH:mm")) : QString()).arg(chainMarks), ui::StatusKind::Info);
        fetchHistories();
        if (m_riskRunAt.isValid()) runRisk();
    }, [this](const QString& message) { setStatus(message, ui::StatusKind::Error); fetchHistories(); });
}

void PortfolioTab::fetchHistories()
{
    for (const pricing::Holding& h : m_book.holdings) {
        const QString s = QString::fromStdString(h.symbol);
        if (!m_histories.count(s) && !m_historyQueue.contains(s)) m_historyQueue << s;
    }
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [this, pump] {
        while (m_historyInFlight < 2 && !m_historyQueue.isEmpty()) {
            const QString symbol = m_historyQueue.takeFirst();
            ++m_historyInFlight;
            const QDate to = QDate::currentDate();
            m_client.fetchAggregates(symbol, 1, "day", to.addDays(-400), to, [this, symbol, pump](const MarketDataClient::BarSeries& series) {
                History h;
                for (const MarketDataClient::Bar& b : series.bars) {
                    h.dates.push_back(QDateTime::fromMSecsSinceEpoch(b.timeMs, QTimeZone("America/New_York")).date());
                    h.closes.push_back(b.close);
                }
                m_histories[symbol] = h;
                --m_historyInFlight;
                (*pump)();
            }, [this, symbol, pump](const QString&) {
                m_histories[symbol] = History{};   // remember the failure so we do not retry every refresh
                --m_historyInFlight;
                (*pump)();
            });
        }
        if (m_historyInFlight == 0 && m_historyQueue.isEmpty() && m_riskRunAt.isValid()) runRisk();
    };
    (*pump)();
}

pricing::risk::ReturnHistory PortfolioTab::returnHistory() const
{
    pricing::risk::ReturnHistory out;
    // Symbols with at least 40 closes; dates common to all of them.
    std::vector<QString> symbols;
    std::set<QDate> common;
    bool first = true;
    for (const pricing::Holding& h : m_book.holdings) {
        const QString s = QString::fromStdString(h.symbol);
        if (std::find(symbols.begin(), symbols.end(), s) != symbols.end()) continue;
        const auto it = m_histories.find(s);
        if (it == m_histories.end() || it->second.closes.size() < 40) continue;
        symbols.push_back(s);
        std::set<QDate> dates(it->second.dates.begin(), it->second.dates.end());
        if (first) { common = dates; first = false; }
        else { std::set<QDate> inter; std::set_intersection(common.begin(), common.end(), dates.begin(), dates.end(), std::inserter(inter, inter.begin())); common = inter; }
    }
    if (symbols.empty() || common.size() < 40) return out;
    for (const QString& s : symbols) out.symbols.push_back(s.toStdString());
    std::vector<std::map<QDate, double>> closeByDate;
    for (const QString& s : symbols) {
        const History& h = m_histories.at(s);
        std::map<QDate, double> m;
        for (size_t i = 0; i < h.dates.size(); ++i) m[h.dates[i]] = h.closes[i];
        closeByDate.push_back(m);
    }
    std::vector<QDate> dates(common.begin(), common.end());
    for (size_t t = 1; t < dates.size(); ++t) {
        std::vector<double> row;
        for (size_t i = 0; i < symbols.size(); ++i) {
            const double prev = closeByDate[i].at(dates[t - 1]), cur = closeByDate[i].at(dates[t]);
            row.push_back(prev > 0.0 ? cur / prev - 1.0 : 0.0);
        }
        out.returns.push_back(row);
    }
    return out;
}

// MARK: - Risk

QString PortfolioTab::riskSettingsLabel() const
{
    return QStringLiteral("%1 confidence, %2-day horizon").arg(m_confidence->currentText()).arg(m_horizon->value());
}

bool PortfolioTab::setRiskSettings(const QString& method, double confidence, int horizonDays)
{
    Q_UNUSED(method);
    if (confidence > 0.0) {
        const double c = confidence > 1.0 ? confidence / 100.0 : confidence;
        int best = 0;
        double bestDiff = 1.0;
        for (int i = 0; i < m_confidence->count(); ++i) { const double d = std::fabs(m_confidence->itemData(i).toDouble() - c); if (d < bestDiff) { bestDiff = d; best = i; } }
        m_confidence->setCurrentIndex(best);
    }
    if (horizonDays > 0) m_horizon->setValue(std::clamp(horizonDays, 1, 60));
    return true;
}

void PortfolioTab::runRisk()
{
    if (m_book.holdings.empty()) { ui::setStatus(m_riskStatus, "No positions to measure.", ui::StatusKind::Warning); return; }
    const pricing::Markets mk = markets();
    const pricing::risk::ReturnHistory history = returnHistory();
    const double confidence = m_confidence->currentData().toDouble();
    const int horizon = m_horizon->value();
    const pricing::Portfolio book = reportingBook();
    m_parametric = pricing::risk::parametricVar(book, mk, history, confidence, horizon);
    m_historical = pricing::risk::historicalVar(book, mk, history, confidence, horizon);
    m_monteCarlo = pricing::risk::monteCarloVar(book, mk, history, confidence, horizon, m_paths->value());
    m_stress = pricing::risk::stressGrid(book, mk);
    m_decay = pricing::risk::decayLadder(book, mk);
    m_riskRunAt = QDateTime::currentDateTime();
    fillRiskTables();
    fillCards();
    fillDistributionChart();
    QString note = QStringLiteral("Computed %1 · %2 · return history: %3 symbol(s), %4 days").arg(m_riskRunAt.toString("HH:mm:ss"), riskSettingsLabel()).arg(history.symbols.size()).arg(history.days());
    if (history.symbols.empty()) note += " (none yet: parametric and Monte Carlo use implied vols; historical needs daily bars)";
    ui::setStatus(m_riskStatus, note, ui::StatusKind::Info);
}

// MARK: - Filling

void PortfolioTab::fillTable()
{
    m_updating = true;
    m_table->setSortingEnabled(false);
    m_table->setRowCount(static_cast<int>(m_rows.size()));
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const Row& row = m_rows[i];
        const pricing::Holding& h = row.holding;
        const int r = static_cast<int>(i);
        auto* symbol = ui::makeCell(QString::fromStdString(h.symbol), Qt::AlignLeft | Qt::AlignVCenter);
        symbol->setData(Qt::UserRole + 1, r);
        QFont bold = symbol->font();
        bold.setBold(true);
        symbol->setFont(bold);
        symbol->setForeground(QBrush(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2)));
        m_table->setItem(r, ColSymbol, symbol);
        m_table->setItem(r, ColBook, ui::makeCell(QString::fromStdString(h.group), Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(r, ColTrade, ui::makeCell(h.tradeDate.empty() ? QStringLiteral("–") : QString::fromStdString(h.tradeDate), Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(r, ColType, ui::makeCell(kindName(h.kind), Qt::AlignLeft | Qt::AlignVCenter));
        const QString ccy = QString::fromStdString(h.currency);
        m_table->setItem(r, ColCurrency, ui::makeCell(ccy, Qt::AlignCenter));
        m_table->setItem(r, ColExpiry, ui::makeCell(h.isOption() ? QString::fromStdString(h.expiryDate) : QStringLiteral("–"), Qt::AlignLeft | Qt::AlignVCenter));
        m_table->setItem(r, ColStrike, h.isOption() ? numericCell(h.strike, 2) : ui::makeCell("–"));
        m_table->setItem(r, ColQty, numericCell(h.quantity, 0, true, &m_theme, (h.quantity > 0 ? "+" : "") + ui::number(h.quantity, 0)));
        m_table->setItem(r, ColEntry, numericCell(h.entryPrice, 2, false, nullptr, fmtMoney(h.entryPrice, 2, ccy)));
        // exposure is in the reporting currency; the per-unit mark is shown in the trade currency.
        const double unit = h.quantity * h.multiplier != 0.0 ? row.exposure.marketValue / (h.quantity * h.multiplier) / fx(ccy) : 0.0;
        m_table->setItem(r, ColMark, numericCell(unit, 2, false, nullptr, fmtMoney(unit, 2, ccy)));
        m_table->setItem(r, ColIv, h.isOption() ? numericCell(h.volatility * 100.0, 1) : ui::makeCell("–"));
        m_table->setItem(r, ColDelta, numericCell(row.exposure.delta, 0));
        m_table->setItem(r, ColGamma, numericCell(row.exposure.gammaDollars, 0, false, nullptr, fmtSigned(row.exposure.gammaDollars)));
        m_table->setItem(r, ColVega, numericCell(row.exposure.vega, 0, false, nullptr, fmtSigned(row.exposure.vega)));
        m_table->setItem(r, ColTheta, numericCell(row.exposure.theta, 0, true, &m_theme, fmtSigned(row.exposure.theta)));
        m_table->setItem(r, ColValue, numericCell(row.exposure.marketValue, 0, false, nullptr, fmtMoney(row.exposure.marketValue)));
        m_table->setItem(r, ColPnl, numericCell(row.exposure.pnl, 0, true, &m_theme, fmtSigned(row.exposure.pnl)));
        const double pct = row.exposure.cost != 0.0 ? row.exposure.pnl / std::fabs(row.exposure.cost) * 100.0 : 0.0;
        m_table->setItem(r, ColPnlPct, numericCell(pct, 1, true, &m_theme, (pct >= 0 ? "+" : "") + ui::number(pct, 1) + "%"));
        m_table->setItem(r, ColSource, ui::makeCell(row.markSource, Qt::AlignLeft | Qt::AlignVCenter));
    }
    m_table->setSortingEnabled(true);
    m_updating = false;
    m_edit->setEnabled(!m_rows.empty());
    m_remove->setEnabled(!m_rows.empty());
}

void PortfolioTab::fillCards()
{
    pricing::Exposure total;
    for (const Row& r : m_rows) total += r.exposure;
    auto setCard = [&](const ui::Card& card, const QString& value, const QString& subtitle, int sign) {
        card.value->setText(value);
        card.subtitle->setText(subtitle);
        card.value->setObjectName(sign > 0 ? "spotUp" : (sign < 0 ? "spotDown" : "bigValue"));
        ui::restyle(card.value);
    };
    const bool any = !m_rows.empty();
    setCard(m_cardValue, any ? fmtMoney(total.marketValue) : "–", any ? QStringLiteral("%1 position(s), cost %2").arg(m_rows.size()).arg(fmtMoney(total.cost)) : "no positions", 0);
    setCard(m_cardPnl, any ? fmtSigned(total.pnl) : "–", any && total.cost != 0.0 ? QStringLiteral("%1% of cost").arg(ui::number(total.pnl / std::fabs(total.cost) * 100.0, 1)) : QString(), total.pnl > 0 ? 1 : (total.pnl < 0 ? -1 : 0));
    setCard(m_cardDelta, any ? fmtSigned(total.deltaDollars / 100.0) : "–", any ? QStringLiteral("%1 share-equivalent delta").arg(ui::number(total.delta, 0)) : QString(), 0);
    setCard(m_cardGamma, any ? fmtSigned(total.gammaDollars) : "–", "change in delta per 1% move", 0);
    setCard(m_cardVega, any ? fmtSigned(total.vega) : "–", "all vols +1 point", 0);
    setCard(m_cardTheta, any ? fmtSigned(total.theta) : "–", "one calendar day", total.theta > 0 ? 1 : (total.theta < 0 ? -1 : 0));
    const bool risk = m_riskRunAt.isValid() && m_monteCarlo.ok;
    const pricing::risk::VarResult& show = m_monteCarlo.ok ? m_monteCarlo : m_parametric;
    setCard(m_cardVar, risk ? fmtMoney(show.var) : "–", risk ? QStringLiteral("%1 · %2").arg(riskSettingsLabel(), show.method) : "run risk below", risk ? -1 : 0);
    setCard(m_cardCvar, risk ? fmtMoney(show.cvar) : "–", risk ? "expected loss beyond VaR" : "run risk below", risk ? -1 : 0);
}

void PortfolioTab::fillWhatIf()
{
    if (m_book.holdings.empty()) { m_whatIfResult->setText(QString()); return; }
    const pricing::Markets mk = markets();
    std::map<std::string, double> shocks;
    for (const std::string& s : pricing::underlyings(m_book)) shocks[s] = m_whatIfSpot->value() / 100.0;
    const double volRel = m_whatIfVol->value() / 100.0;
    const double elapsed = m_whatIfDays->value() / 365.0;
    const pricing::Portfolio book = reportingBook();
    const double pnl = pricing::revaluePnl(book, mk, shocks, volRel, elapsed);
    // Greeks after the shock.
    double delta = 0.0, vega = 0.0, theta = 0.0;
    for (const pricing::Holding& h : book.holdings) {
        const pricing::Market m = pricing::marketFor(mk, h.symbol);
        const double spot = m.spot * (1.0 + shocks[h.symbol]);
        const pricing::Greeks g = pricing::holdingUnitGreeks(h, m, spot, 1.0 + volRel, elapsed);
        delta += h.quantity * h.multiplier * g.delta * spot;
        vega += h.quantity * h.multiplier * g.vega;
        theta += h.quantity * h.multiplier * g.theta;
    }
    const QColor colour(pnl >= 0 ? (m_theme.up.isEmpty() ? "#22c55e" : m_theme.up) : (m_theme.down.isEmpty() ? "#ef4444" : m_theme.down));
    m_whatIfResult->setText(QStringLiteral("<span style=\"color:%1\"><b>%2</b></span> P&amp;L · then delta/1% %3 · vega %4 · theta/day %5")
                                .arg(colour.name(), fmtSigned(pnl), fmtSigned(delta / 100.0), fmtSigned(vega), fmtSigned(theta)));
}

void PortfolioTab::fillRiskTables()
{
    const bool run = m_riskRunAt.isValid();
    const std::vector<const pricing::risk::VarResult*> results{ &m_parametric, &m_historical, &m_monteCarlo };
    m_varTable->setRowCount(run ? 3 : 0);
    for (int r = 0; run && r < 3; ++r) {
        const pricing::risk::VarResult& v = *results[static_cast<size_t>(r)];
        m_varTable->setItem(r, 0, ui::makeCell(QString::fromStdString(v.method), Qt::AlignLeft | Qt::AlignVCenter));
        m_varTable->setItem(r, 1, v.ok ? numericCell(v.var, 0, false, nullptr, fmtMoney(v.var)) : ui::makeCell("–"));
        m_varTable->setItem(r, 2, v.ok ? numericCell(v.cvar, 0, false, nullptr, fmtMoney(v.cvar)) : ui::makeCell("–"));
        m_varTable->setItem(r, 3, v.ok ? numericCell(v.meanPnl, 0, true, &m_theme, fmtSigned(v.meanPnl)) : ui::makeCell("–"));
        m_varTable->setItem(r, 4, v.ok ? numericCell(v.stdevPnl, 0, false, nullptr, fmtMoney(v.stdevPnl)) : ui::makeCell("–"));
        m_varTable->setItem(r, 5, ui::makeCell(QString::fromStdString(v.note), Qt::AlignLeft | Qt::AlignVCenter));
    }
    m_varTable->setFixedHeight(26 + 3 * 24 + 4);
    m_varTable->setMinimumHeight(26 + 3 * 24 + 4);

    const pricing::Markets mk = markets();
    const pricing::BookExposure book = pricing::bookExposure(reportingBook(), mk, true);
    fillStrategyTable(mk);
    m_componentTable->setRowCount(static_cast<int>(book.byUnderlying.size()));
    int r = 0;
    for (const auto& [symbol, e] : book.byUnderlying) {
        m_componentTable->setItem(r, 0, ui::makeCell(QString::fromStdString(symbol), Qt::AlignLeft | Qt::AlignVCenter));
        m_componentTable->setItem(r, 1, numericCell(e.marketValue, 0, false, nullptr, fmtMoney(e.marketValue)));
        m_componentTable->setItem(r, 2, numericCell(e.deltaDollars / 100.0, 0, true, &m_theme, fmtSigned(e.deltaDollars / 100.0)));
        m_componentTable->setItem(r, 3, numericCell(e.gammaDollars, 0, false, nullptr, fmtSigned(e.gammaDollars)));
        m_componentTable->setItem(r, 4, numericCell(e.vega, 0, false, nullptr, fmtSigned(e.vega)));
        m_componentTable->setItem(r, 5, numericCell(e.theta, 0, true, &m_theme, fmtSigned(e.theta)));
        const auto comp = m_parametric.componentVar.find(symbol);
        m_componentTable->setItem(r, 6, run && comp != m_parametric.componentVar.end() ? numericCell(comp->second, 0, false, nullptr, fmtSigned(comp->second)) : ui::makeCell("–"));
        ++r;
    }

    m_stressTable->setRowCount(static_cast<int>(m_stress.spotShocks.size()));
    for (size_t i = 0; i < m_stress.spotShocks.size(); ++i) {
        m_stressTable->setItem(static_cast<int>(i), 0, ui::makeCell(QStringLiteral("%1%").arg(m_stress.spotShocks[i] * 100.0, 0, 'f', 0), Qt::AlignLeft | Qt::AlignVCenter));
        for (size_t j = 0; j < m_stress.volShocks.size() && j + 1 < static_cast<size_t>(m_stressTable->columnCount()); ++j) {
            const double pnl = m_stress.pnl[i][j];
            auto* cell = numericCell(pnl, 0, true, &m_theme, fmtSigned(pnl));
            // Shade the cell by sign and size (relative to the largest absolute P&L in the grid).
            double maxAbs = 1.0;
            for (const auto& row : m_stress.pnl) for (double v : row) maxAbs = std::max(maxAbs, std::fabs(v));
            const QColor base(pnl >= 0 ? (m_theme.up.isEmpty() ? "#22c55e" : m_theme.up) : (m_theme.down.isEmpty() ? "#ef4444" : m_theme.down));
            QColor tint = base;
            tint.setAlphaF(std::min(0.55, 0.08 + 0.5 * std::fabs(pnl) / maxAbs));
            cell->setBackground(QBrush(tint));
            cell->setForeground(QBrush(QColor(m_theme.textStrong.isEmpty() ? "#f3f6fb" : m_theme.textStrong)));
            m_stressTable->setItem(static_cast<int>(i), static_cast<int>(j) + 1, cell);
        }
    }
    m_stressTable->setMaximumHeight(26 + static_cast<int>(std::max<size_t>(1, m_stress.spotShocks.size())) * 24 + 4);
    m_stressTable->setMinimumHeight(26 + 3 * 24);
    m_stressTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QStringList decay;
    for (const auto& [days, pnl] : m_decay) decay << QStringLiteral("%1d %2").arg(days).arg(fmtSigned(pnl));
    m_decayLabel->setText(run ? QStringLiteral("Time decay alone (spots and vols unchanged): %1").arg(decay.join(" · ")) : QStringLiteral("Run risk to compute the stress grid and the decay ladder."));
}

void PortfolioTab::fillStrategyTable(const pricing::Markets& mk)
{
    if (m_activeBook != kAllBooks) { m_strategyTable->setRowCount(0); return; }
    // Group the reporting-currency book by strategy; standalone parametric VaR per strategy with the
    // same settings, so the diversification benefit of holding them together is visible.
    std::map<QString, pricing::Portfolio> byBook;
    for (const pricing::Holding& h : reportingBook().holdings) byBook[QString::fromStdString(h.group)].holdings.push_back(h);
    const bool run = m_riskRunAt.isValid();
    const pricing::risk::ReturnHistory history = run ? returnHistory() : pricing::risk::ReturnHistory{};
    const double confidence = m_confidence->currentData().toDouble();
    const int horizon = m_horizon->value();
    m_strategyTable->setRowCount(static_cast<int>(byBook.size()));
    int r = 0;
    double standaloneSum = 0.0;
    for (const auto& [name, part] : byBook) {
        const pricing::BookExposure e = pricing::bookExposure(part, mk, true);
        m_strategyTable->setItem(r, 0, ui::makeCell(name, Qt::AlignLeft | Qt::AlignVCenter));
        m_strategyTable->setItem(r, 1, numericCell(static_cast<double>(part.holdings.size()), 0));
        m_strategyTable->setItem(r, 2, numericCell(e.total.marketValue, 0, false, nullptr, fmtMoney(e.total.marketValue)));
        m_strategyTable->setItem(r, 3, numericCell(e.total.pnl, 0, true, &m_theme, fmtSigned(e.total.pnl)));
        m_strategyTable->setItem(r, 4, numericCell(e.total.deltaDollars / 100.0, 0, true, &m_theme, fmtSigned(e.total.deltaDollars / 100.0)));
        m_strategyTable->setItem(r, 5, numericCell(e.total.vega, 0, false, nullptr, fmtSigned(e.total.vega)));
        m_strategyTable->setItem(r, 6, numericCell(e.total.theta, 0, true, &m_theme, fmtSigned(e.total.theta)));
        if (run) {
            const pricing::risk::VarResult v = pricing::risk::parametricVar(part, mk, history, confidence, horizon);
            standaloneSum += v.var;
            m_strategyTable->setItem(r, 7, numericCell(v.var, 0, false, nullptr, fmtMoney(v.var)));
        } else {
            m_strategyTable->setItem(r, 7, ui::makeCell("–"));
        }
        ++r;
    }
    m_strategyTable->setMaximumHeight(26 + static_cast<int>(std::max<size_t>(1, byBook.size())) * 24 + 4);
    if (run && m_parametric.ok) {
        const double benefit = standaloneSum - m_parametric.var;
        m_strategyNote->setText(QStringLiteral("Standalone parametric VaR adds up to %1 versus %2 for the whole book: diversification benefit %3 (%4%).")
                                    .arg(fmtMoney(standaloneSum), fmtMoney(m_parametric.var), fmtMoney(benefit)).arg(standaloneSum > 0 ? QString::number(benefit / standaloneSum * 100.0, 'f', 0) : QStringLiteral("0")));
    } else {
        m_strategyNote->setText("Run risk to compare each strategy's standalone VaR with the global book.");
    }
}

void PortfolioTab::fillDistributionChart()
{
    m_distributionChart->removeAllSeries();
    for (QAbstractAxis* axis : m_distributionChart->axes()) m_distributionChart->removeAxis(axis);
    const pricing::risk::VarResult& v = m_chartMethod->currentData().toString() == "hist" ? m_historical : m_monteCarlo;
    styleChart(m_distributionChart, m_theme);
    if (!v.ok || v.pnl.size() < 10) {
        m_distributionChart->setTitle(QStringLiteral("Simulated P&L distribution%1").arg(v.ok ? QString() : (m_riskRunAt.isValid() ? QStringLiteral(" · %1").arg(QString::fromStdString(v.note)) : QStringLiteral(" · run risk"))));
        m_distributionView->probe = nullptr;
        m_distributionView->readout = nullptr;
        return;
    }
    m_distributionChart->setTitle(QStringLiteral("%1 P&L distribution · %2").arg(QString::fromStdString(v.method), riskSettingsLabel()));
    const int bins = 48;
    const double lo = v.pnl.front(), hi = v.pnl.back();
    const double width = (hi - lo) / bins;
    std::vector<int> counts(static_cast<size_t>(bins), 0);
    for (double p : v.pnl) counts[static_cast<size_t>(std::min(bins - 1, static_cast<int>((p - lo) / std::max(width, 1e-9))))]++;
    int maxCount = 1;
    for (int c : counts) maxCount = std::max(maxCount, c);
    auto* upper = new QLineSeries;
    auto* tail = new QLineSeries;   // the loss tail beyond VaR, filled in the down colour
    for (int b = 0; b < bins; ++b) {
        const double x0 = lo + b * width, x1 = x0 + width, y = counts[static_cast<size_t>(b)];
        upper->append(x0, y); upper->append(x1, y);
        const double tailY = x1 <= -v.var ? y : (x0 < -v.var ? y : 0.0);
        tail->append(x0, tailY); tail->append(x1, tailY);
    }
    auto* area = new QAreaSeries(upper);
    const QColor accent(m_theme.accent.isEmpty() ? "#3b82f6" : m_theme.accent);
    area->setColor(QColor(accent.red(), accent.green(), accent.blue(), 120));
    area->setBorderColor(accent);
    area->setName("P&L");
    auto* tailArea = new QAreaSeries(tail);
    const QColor down(m_theme.down.isEmpty() ? "#ef4444" : m_theme.down);
    tailArea->setColor(QColor(down.red(), down.green(), down.blue(), 170));
    tailArea->setBorderColor(down);
    tailArea->setName("Beyond VaR");
    m_distributionChart->addSeries(area);
    m_distributionChart->addSeries(tailArea);
    auto* varLine = new QLineSeries;
    varLine->append(-v.var, 0); varLine->append(-v.var, maxCount * 1.05);
    varLine->setPen(QPen(down, 2, Qt::DashLine));
    varLine->setName("VaR");
    auto* cvarLine = new QLineSeries;
    cvarLine->append(-v.cvar, 0); cvarLine->append(-v.cvar, maxCount * 1.05);
    cvarLine->setPen(QPen(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2), 2, Qt::DotLine));
    cvarLine->setName("CVaR");
    m_distributionChart->addSeries(varLine);
    m_distributionChart->addSeries(cvarLine);
    // Currency-formatted x axis: a category axis with one label per "nice" tick value.
    auto* x = new QCategoryAxis;
    x->setTitleText(QStringLiteral("P&L (%1)").arg(m_currency));
    x->setLabelsPosition(QCategoryAxis::AxisLabelsPositionOnValue);
    x->setTruncateLabels(false);   // short charts otherwise elide the labels to "…"
    const double rawStep = (hi - lo) / 5.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(std::max(rawStep, 1e-9))));
    const double unit = rawStep / magnitude;
    const double step = (unit < 1.5 ? 1.0 : unit < 3.5 ? 2.0 : unit < 7.5 ? 5.0 : 10.0) * magnitude;
    const int decimals = step < 1.0 ? 2 : 0;
    x->setStartValue(lo);
    for (double tick = std::ceil(lo / step) * step; tick <= hi + step * 1e-6; tick += step) {
        const double value = std::abs(tick) < step * 1e-6 ? 0.0 : tick;
        x->append(fmtMoney(value, decimals), value);
    }
    x->setRange(lo, hi);
    auto* y = new QValueAxis;
    y->setTitleText("paths");
    y->setLabelFormat("%d");
    y->setRange(0, maxCount * 1.05);
    y->setTickCount(4);
    y->setTruncateLabels(false);
    m_distributionChart->addAxis(x, Qt::AlignBottom);
    m_distributionChart->addAxis(y, Qt::AlignLeft);
    for (QAbstractSeries* s : m_distributionChart->series()) { s->attachAxis(x); s->attachAxis(y); }
    styleChart(m_distributionChart, m_theme);

    // Hover readout: the bin under the cursor and the probability of doing worse than that P&L.
    m_distributionView->probe = area;
    const std::vector<double> sorted = v.pnl;   // VarResult keeps the simulated P&Ls sorted ascending
    const double var = v.var, cvar = v.cvar;
    const QString method = QString::fromStdString(v.method);
    m_distributionView->readout = [this, sorted, lo, width, bins, counts, var, cvar, method](double px) -> QStringList {
        if (sorted.empty() || width <= 0.0) return {};
        const int bin = std::clamp(static_cast<int>((px - lo) / width), 0, bins - 1);
        const double x0 = lo + bin * width, x1 = x0 + width;
        const int inBin = counts[static_cast<size_t>(bin)];
        const double total = static_cast<double>(sorted.size());
        const double worse = static_cast<double>(std::upper_bound(sorted.begin(), sorted.end(), px) - sorted.begin()) / total * 100.0;
        return { QStringLiteral("P&L %1").arg(fmtSigned(px)),
                 QStringLiteral("Bin %1 to %2: %3 paths (%4%)").arg(fmtSigned(x0), fmtSigned(x1)).arg(inBin).arg(inBin / total * 100.0, 0, 'f', 1),
                 QStringLiteral("Chance of a worse outcome: %1%").arg(worse, 0, 'f', worse < 1.0 ? 2 : 1),
                 QStringLiteral("%1 · VaR %2 · CVaR %3").arg(method, fmtSigned(-var), fmtSigned(-cvar)) };
    };
}

// MARK: - Currency

QString PortfolioTab::fmtMoney(double v, int decimals, const QString& currency) const
{
    const QString code = currency.isEmpty() ? m_currency : currency;
    const QString body = QLocale(QLocale::English).toString(std::fabs(v), 'f', decimals);
    return (v < 0 ? QStringLiteral("−") : QString()) + currencySymbol(code) + body;
}

QString PortfolioTab::fmtSigned(double v, int decimals, const QString& currency) const
{
    const QString code = currency.isEmpty() ? m_currency : currency;
    const QString body = QLocale(QLocale::English).toString(std::fabs(v), 'f', decimals);
    return (v < 0 ? QStringLiteral("−") : QStringLiteral("+")) + currencySymbol(code) + body;
}

double PortfolioTab::toUsd(const QString& currency) const
{
    const QString code = normalizeCurrency(currency);
    if (code == "USD") return 1.0;
    const auto o = m_fxOverrides.find(code);
    if (o != m_fxOverrides.end() && o->second > 0.0) return o->second;
    const auto it = m_fxToUsd.find(code);
    return it != m_fxToUsd.end() && it->second > 0.0 ? it->second : 1.0;
}

double PortfolioTab::fx(const QString& fromCurrency) const
{
    // Units of the reporting currency per unit of `fromCurrency`.
    return toUsd(fromCurrency) / toUsd(m_currency);
}

pricing::Portfolio PortfolioTab::reportingBook() const
{
    pricing::Portfolio out = m_book;
    for (pricing::Holding& h : out.holdings) h.multiplier *= fx(QString::fromStdString(h.currency));
    return out;
}

void PortfolioTab::rebuildRows()
{
    m_rows.clear();
    const pricing::Markets mk = markets();
    for (pricing::Holding& h : m_book.holdings) {
        Row row;
        QString source;
        markFromChain(h, &source);
        row.holding = h;
        pricing::Holding reporting = h;
        reporting.multiplier *= fx(QString::fromStdString(h.currency));
        row.exposure = pricing::exposure(reporting, pricing::marketFor(mk, h.symbol), true);
        row.markSource = source;
        m_rows.push_back(row);
    }
    fillTable();
    fillCards();
    fillWhatIf();
}

void PortfolioTab::updateCurrencyHeaders()
{
    const QStringList labels{ "Symbol", "Strategy", "Trade date", "Type", "Ccy", "Expiry", "Strike", "Qty", "Entry", "Mark", "IV %", "Delta",
                              QStringLiteral("Gamma %1/1%").arg(m_currency), QStringLiteral("Vega %1").arg(m_currency), QStringLiteral("Theta %1/d").arg(m_currency),
                              QStringLiteral("Value %1").arg(m_currency), QStringLiteral("P&L %1").arg(m_currency), "P&L %", "Mark source" };
    m_table->setHorizontalHeaderLabels(labels);
    m_componentTable->setHorizontalHeaderLabels({ "Underlying", QStringLiteral("Value %1").arg(m_currency), "Delta/1%", "Gamma/1%", "Vega", "Theta/d", "Component VaR" });
    m_cardValue.title->setText(QStringLiteral("Net value (%1)").arg(m_currency));
    m_cardPnl.title->setText(QStringLiteral("Open P&L (%1)").arg(m_currency));
}

void PortfolioTab::fetchFx()
{
    QStringList needed;
    auto want = [&](const QString& code) { const QString c = normalizeCurrency(code); if (c != "USD" && !m_fxOverrides.count(c) && !m_fxToUsd.count(c) && !needed.contains(c)) needed << c; };
    want(m_currency);
    for (const pricing::Holding& h : m_book.holdings) want(QString::fromStdString(h.currency));
    for (const QString& code : needed) {
        // Massive quotes forex as C:EURUSD (USD per EUR); some pairs are only quoted the other way round.
        const QDate to = QDate::currentDate();
        m_client.fetchAggregates(QStringLiteral("C:%1USD").arg(code), 1, "day", to.addDays(-7), to, [this, code](const MarketDataClient::BarSeries& series) {
            if (!series.bars.empty() && series.bars.back().close > 0.0) { m_fxToUsd[code] = series.bars.back().close; m_fxAsOf = QDateTime::currentDateTime(); rebuildRows(); }
        }, [this, code](const QString&) {
            const QDate to = QDate::currentDate();
            m_client.fetchAggregates(QStringLiteral("C:USD%1").arg(code), 1, "day", to.addDays(-7), to, [this, code](const MarketDataClient::BarSeries& series) {
                if (!series.bars.empty() && series.bars.back().close > 0.0) { m_fxToUsd[code] = 1.0 / series.bars.back().close; m_fxAsOf = QDateTime::currentDateTime(); rebuildRows(); }
            }, [this, code](const QString& message) {
                setStatus(QStringLiteral("No FX rate for %1 (%2); using 1.0 until you set one under FX rates…").arg(code, message.left(80)), ui::StatusKind::Warning);
            });
        });
    }
}

void PortfolioTab::promptFxRates()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Exchange rates");
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(14, 12, 14, 10);
    form->setSpacing(8);
    auto* intro = new QLabel(QStringLiteral("Rates are quoted as US dollars per unit of currency and fetched from the data vendor's forex bars%1. "
                                            "Enter a value to override a rate; 0 clears the override.").arg(m_fxAsOf.isValid() ? QStringLiteral(" (last fetched %1)").arg(m_fxAsOf.toString("HH:mm")) : QString()), &dialog);
    intro->setWordWrap(true);
    intro->setObjectName("muted");
    form->addRow(intro);
    QStringList codes;
    for (int i = 0; i < m_currencyBox->count(); ++i) { const QString c = m_currencyBox->itemData(i).toString(); if (c != "USD") codes << c; }
    for (const pricing::Holding& h : m_book.holdings) { const QString c = QString::fromStdString(h.currency); if (c != "USD" && !codes.contains(c)) codes << c; }
    std::vector<std::pair<QString, QDoubleSpinBox*>> editors;
    for (const QString& code : codes) {
        auto* box = ui::makeSpinBox(&dialog, 0.0, 100000.0, 0.0001, 4, m_fxOverrides.count(code) ? m_fxOverrides.at(code) : 0.0);
        box->setSpecialValueText(m_fxToUsd.count(code) ? QStringLiteral("vendor %1").arg(QString::number(m_fxToUsd.at(code), 'f', 4)) : QStringLiteral("not available (1.0)"));
        form->addRow(QStringLiteral("%1 → USD").arg(code), box);
        editors.emplace_back(code, box);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QJsonObject saved;
    m_fxOverrides.clear();
    for (const auto& [code, box] : editors) {
        if (box->value() > 0.0) { m_fxOverrides[code] = box->value(); saved[code] = box->value(); }
    }
    QSettings().setValue(kFxOverridesKey, QJsonDocument(saved).toJson(QJsonDocument::Compact));
    rebuildRows();
    if (m_riskRunAt.isValid()) runRisk();
}

// MARK: - Assistant and tests

QString PortfolioTab::summaryText() const
{
    if (m_book.holdings.empty()) return QStringLiteral("Portfolio: no positions.");
    pricing::Exposure total;
    for (const Row& r : m_rows) total += r.exposure;
    QString out;
    QTextStream s(&out);
    s << "Portfolio view: " << m_activeBook << (m_activeBook == kAllBooks ? QStringLiteral(" (global, %1 strategies: %2)").arg(m_books.size()).arg(m_books.keys().join(", ")) : QString())
      << ". Reporting currency " << m_currency << ": " << m_rows.size() << " positions across " << pricing::underlyings(m_book).size() << " underlyings. Net value " << fmtMoney(total.marketValue)
      << ", open P&L " << fmtSigned(total.pnl) << ", delta per 1% " << fmtSigned(total.deltaDollars / 100.0) << " (" << ui::number(total.delta, 0) << " share-equivalent), gamma per 1% "
      << fmtSigned(total.gammaDollars) << ", vega " << fmtSigned(total.vega) << " per vol point, theta " << fmtSigned(total.theta) << " per day.";
    if (m_marksAsOf.isValid()) s << " Marks as of " << m_marksAsOf.toLocalTime().toString("yyyy-MM-dd HH:mm") << " (15-minute delayed).";
    if (m_riskRunAt.isValid()) {
        s << "\nRisk (" << riskSettingsLabel() << ", computed " << m_riskRunAt.toString("HH:mm") << "): ";
        for (const pricing::risk::VarResult* v : { &m_parametric, &m_historical, &m_monteCarlo }) {
            s << QString::fromStdString(v->method) << " VaR " << (v->ok ? fmtMoney(v->var) : QStringLiteral("n/a")) << " / CVaR " << (v->ok ? fmtMoney(v->cvar) : QStringLiteral("n/a")) << "; ";
        }
        if (!m_parametric.componentVar.empty()) {
            s << "component VaR by underlying: ";
            QStringList parts;
            for (const auto& [sym, c] : m_parametric.componentVar) parts << QStringLiteral("%1 %2").arg(QString::fromStdString(sym), fmtSigned(c));
            s << parts.join(", ") << ". ";
        }
        if (!m_stress.pnl.empty()) {
            s << "Stress P&L (spot −10%/+10% at unchanged vol): " << fmtSigned(m_stress.pnl[1][2]) << " / " << fmtSigned(m_stress.pnl[5][2])
              << "; worst cell " << fmtSigned([&] { double w = 0; for (const auto& row : m_stress.pnl) for (double v : row) w = std::min(w, v); return w; }()) << ".";
        }
    } else {
        s << "\nRisk not computed yet (run_portfolio_risk).";
    }
    return out;
}

QString PortfolioTab::resultsCsv() const
{
    QString out = QStringLiteral("symbol,type,quantity,strike,expiry,entry,iv_pct,currency,strategy,trade_date,mark,delta,gamma_1pct_%1,vega_%1,theta_day_%1,value_%1,pnl_%1,mark_source\n").arg(m_currency);
    for (const Row& r : m_rows) {
        const pricing::Holding& h = r.holding;
        const QString ccy = QString::fromStdString(h.currency);
        const double unit = h.quantity * h.multiplier != 0.0 ? r.exposure.marketValue / (h.quantity * h.multiplier) / fx(ccy) : 0.0;
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17,%18\n")
                   .arg(QString::fromStdString(h.symbol), kindName(h.kind).toLower(), QString::number(h.quantity, 'f', 0), h.isOption() ? QString::number(h.strike, 'f', 2) : QString(),
                        h.isOption() ? QString::fromStdString(h.expiryDate) : QString(), QString::number(h.entryPrice, 'f', 2), h.isOption() ? QString::number(h.volatility * 100.0, 'f', 1) : QString(), ccy,
                        QString::fromStdString(h.group))
                   .arg(QString::fromStdString(h.tradeDate), QString::number(unit, 'f', 2))
                   .arg(QString::number(r.exposure.delta, 'f', 1), QString::number(r.exposure.gammaDollars, 'f', 0), QString::number(r.exposure.vega, 'f', 0), QString::number(r.exposure.theta, 'f', 0),
                        QString::number(r.exposure.marketValue, 'f', 0), QString::number(r.exposure.pnl, 'f', 0), r.markSource);
    }
    return out;
}

QString PortfolioTab::riskCsv() const
{
    QString out = "method,confidence,horizon_days,var,cvar,mean_pnl,stdev_pnl,note\n";
    for (const pricing::risk::VarResult* v : { &m_parametric, &m_historical, &m_monteCarlo }) {
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8\n").arg(QString::fromStdString(v->method)).arg(v->confidence).arg(v->horizonDays)
                   .arg(v->ok ? QString::number(v->var, 'f', 0) : QString(), v->ok ? QString::number(v->cvar, 'f', 0) : QString(), QString::number(v->meanPnl, 'f', 0), QString::number(v->stdevPnl, 'f', 0), QString::fromStdString(v->note));
    }
    out += "\nstress spot_shock";
    for (double v : m_stress.volShocks) out += QStringLiteral(",vol%1%").arg(v * 100.0, 0, 'f', 0);
    out += "\n";
    for (size_t i = 0; i < m_stress.spotShocks.size(); ++i) {
        out += QStringLiteral("%1%").arg(m_stress.spotShocks[i] * 100.0, 0, 'f', 0);
        for (double pnl : m_stress.pnl[i]) out += "," + QString::number(pnl, 'f', 0);
        out += "\n";
    }
    return out;
}

void PortfolioTab::loadSampleData(bool synthetic)
{
    // Two sample strategies shown together in the global view (not written to the user's books when synthetic).
    const QDate today = QDate::currentDate();
    std::vector<pricing::Holding> core, income;
    auto add = [](std::vector<pricing::Holding>& into, const std::string& symbol, pricing::LegKind kind, double qty, double strike, const QDate& expiry, double entry, double iv) {
        pricing::Holding h;
        h.symbol = symbol; h.kind = kind; h.quantity = qty; h.strike = strike;
        h.expiryDate = kind == pricing::LegKind::Underlying ? std::string() : expiry.toString(Qt::ISODate).toStdString();
        h.entryPrice = entry; h.volatility = iv; h.multiplier = kind == pricing::LegKind::Underlying ? 1.0 : 100.0;
        into.push_back(h);
    };
    add(core, "AAPL", pricing::LegKind::Underlying, 200, 0, today, 310.0, 0);
    add(core, "AAPL", pricing::LegKind::Put, 2, 320.0, today.addDays(45), 6.5, 0.27);
    add(core, "NVDA", pricing::LegKind::Call, 3, 240.0, today.addDays(60), 12.0, 0.45);
    add(core, "NVDA", pricing::LegKind::Call, -3, 270.0, today.addDays(60), 4.5, 0.47);
    add(income, "SPY", pricing::LegKind::Underlying, 50, 0, today, 760.0, 0);
    add(income, "SPY", pricing::LegKind::Put, -1, 740.0, today.addDays(30), 7.0, 0.16);
    for (pricing::Holding& h : core) h.tradeDate = today.addDays(-35).toString(Qt::ISODate).toStdString();
    for (pricing::Holding& h : income) h.tradeDate = today.addDays(-12).toString(Qt::ISODate).toStdString();
    m_books.clear();
    m_books["Core hedged"] = core;
    m_books["Index income"] = income;
    m_activeBook = kAllBooks;
    rebuildWorkingBook();
    refreshBookCombo();
    if (synthetic) {
        m_spots = { { "AAPL", 336.0 }, { "NVDA", 229.0 }, { "SPY", 778.0 } };
        uint seed = 11;
        auto rnd = [&seed] { seed = seed * 1103515245u + 12345u; return ((seed >> 8) % 20001) / 10000.0 - 1.0; };
        for (const auto& [symbol, spot] : m_spots) {
            History h;
            double price = spot * 0.8;
            const double daily = symbol == "SPY" ? 0.009 : 0.02;
            for (int d = 300; d >= 1; --d) {
                price *= 1.0 + daily * (rnd() + 0.3 * rnd());
                h.dates.push_back(today.addDays(-d));
                h.closes.push_back(price);
            }
            m_histories[symbol] = h;
        }
        m_marksAsOf = QDateTime::currentDateTime();
        const QString todayIso = today.toString(Qt::ISODate);
        m_rows.clear();
        const pricing::Markets mk = markets();
        for (pricing::Holding& h : m_book.holdings) {
            if (h.isOption()) h.maturity = pricing::maturityFromDates(todayIso.toStdString(), h.expiryDate);
            Row row;
            row.holding = h;
            pricing::Holding reporting = h;
            reporting.multiplier *= fx(QString::fromStdString(h.currency));
            row.exposure = pricing::exposure(reporting, pricing::marketFor(mk, h.symbol), false);
            row.markSource = h.isOption() ? "model" : "snapshot";
            m_rows.push_back(row);
        }
        fillTable();
        fillCards();
        fillWhatIf();
        runRisk();
    } else {
        savePositions();
        refreshMarks();
    }
}
