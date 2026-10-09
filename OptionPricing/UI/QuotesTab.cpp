//
//  QuotesTab.cpp
//  OptionPricing
//

#include "QuotesTab.h"
#include "ChartPage.h"
#include "Formatting.h"

#include <cmath>

namespace {
enum Column { ColTicker = 0, ColLast, ColChange, ColPercent, ColumnCount };
constexpr const char* kWatchlistKey = "quotes/watchlist";
constexpr const char* kTimeframeKey = "quotes/timeframe";
constexpr const char* kChartTypeKey = "quotes/chartType";
constexpr const char* kHeaderKey = "quotes/header";       ///< column widths and sort indicator
constexpr const char* kDrawingsPrefix = "quotes/drawings/"; ///< + symbol -> JSON array of drawings
constexpr int kValueRole = Qt::UserRole;                   ///< numeric value used for sorting
constexpr int kTickerRole = Qt::UserRole + 1;              ///< ticker symbol stored on the first column

/// Table cell that sorts by its numeric value rather than its display text, so "+3.86"
/// and "−6.46" order correctly.
class NumericItem : public QTableWidgetItem
{
public:
    NumericItem(const QString& text, double value)
        : QTableWidgetItem(text)
    {
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setData(kValueRole, value);
    }
    bool operator<(const QTableWidgetItem& other) const override
    {
        return data(kValueRole).toDouble() < other.data(kValueRole).toDouble();
    }
};
} // namespace

const std::vector<QuotesTab::Timeframe>& QuotesTab::timeframes()
{
    static const std::vector<Timeframe> list = {
        { "1m", 1, "minute", 3, true },
        { "2m", 2, "minute", 5, true },
        { "3m", 3, "minute", 7, true },
        { "5m", 5, "minute", 12, true },
        { "15m", 15, "minute", 35, true },
        { "1H", 1, "hour", 120, true },
        { "1D", 1, "day", 366, false },
        { "1W", 1, "week", 5 * 366, false },
    };
    return list;
}

QuotesTab::QuotesTab(MarketState& state, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
{
    buildUi();
    wire();
    loadWatchlist();
    rebuildTable();
    updateTimers();
    // A chain fetched elsewhere adds its ticker to the watchlist so quotes and chain stay in step.
    m_state.subscribe([this] {
        const QString symbol = m_state.underlyingTicker;
        if (!symbol.isEmpty() && !m_watchlist.contains(symbol)) {
            addTicker(symbol);
        }
        if (!m_state.companyName.isEmpty() && !symbol.isEmpty()) m_names[symbol] = m_state.companyName;
    });
    QTimer::singleShot(400, this, [this] { refreshQuotes(); });
}

// MARK: - Construction

void QuotesTab::buildUi()
{
    // ---- Watchlist (left) ----
    m_tickerEdit = new QLineEdit(this);
    m_tickerEdit->setPlaceholderText("Add ticker, e.g. MSFT");
    m_tickerEdit->setMaxLength(12);
    m_addButton = ui::makeButton(this, "Add", "primary", "Add the ticker to the watchlist");
    m_removeButton = ui::makeButton(this, "Remove", "secondary", "Remove the selected ticker");
    m_refreshButton = ui::makeButton(this, "Refresh", "secondary", "Refresh quotes now");
    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(8);
    addRow->addWidget(m_tickerEdit, 1);
    addRow->addWidget(m_addButton);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ "Ticker", "Last", "Price Chg.", "Pct Change" });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setMinimumSectionSize(56);
    m_table->horizontalHeader()->setFixedHeight(30);
    m_table->setColumnWidth(ColTicker, 84);
    m_table->setColumnWidth(ColLast, 96);
    m_table->setColumnWidth(ColChange, 96);
    // Click a header to sort (Last, Price Chg. and Pct Change sort numerically); a third
    // click clears the sort and restores watchlist order.
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    m_table->horizontalHeader()->setSortIndicatorClearable(true);
    m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(30);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setToolTip("Select a ticker to chart it. Double-click to open its option chain. Click a header to sort; drag a divider to resize.");

    m_autoRefresh = new QCheckBox("Auto-refresh every", this);
    m_autoRefresh->setChecked(true);
    m_refreshInterval = ui::makeIntSpinBox(this, 15, 600, 60);
    m_refreshInterval->setSuffix(" s");
    auto* refreshRow = new QHBoxLayout;
    refreshRow->setSpacing(8);
    refreshRow->addWidget(m_autoRefresh);
    refreshRow->addWidget(m_refreshInterval);
    refreshRow->addStretch(1);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_removeButton);
    buttonRow->addWidget(m_refreshButton);

    m_quoteStatus = new QLabel(this);
    m_quoteStatus->setObjectName("muted");
    m_quoteStatus->setWordWrap(true);

    auto* watchBox = new QFrame(this);
    watchBox->setObjectName("pane");
    auto* watchLayout = new QVBoxLayout(watchBox);
    watchLayout->setContentsMargins(12, 12, 12, 12);
    watchLayout->setSpacing(8);
    watchLayout->addLayout(addRow);
    watchLayout->addWidget(m_table, 1);
    watchLayout->addLayout(refreshRow);
    watchLayout->addLayout(buttonRow);
    watchLayout->addWidget(m_quoteStatus);
    watchBox->setMinimumWidth(360);

    // ---- Chart (right) ----
    m_chartSymbol = new QLabel("—", this);
    m_chartSymbol->setObjectName("tickerSymbol");
    m_chartName = new QLabel(this);
    m_chartName->setObjectName("muted");
    m_chartName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_timeframeGroup = new QButtonGroup(this);
    m_timeframeGroup->setExclusive(true);
    auto* tfRow = new QHBoxLayout;
    tfRow->setSpacing(4);
    int index = 0;
    for (const Timeframe& tf : timeframes()) {
        auto* button = new QToolButton(this);
        button->setText(tf.label);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tf.intraday ? QStringLiteral("%1 bars over the last %2 days").arg(tf.label).arg(tf.lookbackDays)
                                       : (QString(tf.label) == "1D" ? QStringLiteral("Daily bars over one year") : QStringLiteral("Weekly bars over five years")));
        m_timeframeGroup->addButton(button, index++);
        tfRow->addWidget(button);
    }

    m_chartType = new QComboBox(this);
    m_chartType->addItem("Candlesticks", "candles");
    m_chartType->addItem("Bars", "bars");
    m_chartType->addItem("Heikin-Ashi", "heikin");
    m_chartType->addItem("Line", "line");
    // Width comes from the theme stylesheet (QComboBox#chartTypeCombo): the global QComboBox
    // min-width rule is re-applied on every polish and would override setMinimumWidth here.
    m_chartType->setObjectName("chartTypeCombo");
    m_chartType->setToolTip("Chart style");
    m_smaCheck = new QCheckBox("SMA", this);
    m_smaCheck->setChecked(true);
    m_smaPeriod = ui::makeIntSpinBox(this, 2, 500, 20);
    m_emaCheck = new QCheckBox("EMA", this);
    m_emaCheck->setChecked(true);
    m_emaPeriod = ui::makeIntSpinBox(this, 2, 500, 50);
    m_volumeCheck = new QCheckBox("Volume", this);
    m_volumeCheck->setChecked(true);
    m_openChain = ui::makeButton(this, "Open Option Chain", "secondary", "Fetch this ticker's option chain on the Option Chain tab");
    m_saveImage = ui::makeButton(this, "Save Image…", "secondary", "Save the chart as a PNG image");
    for (QSpinBox* box : { m_smaPeriod, m_emaPeriod }) {
        box->setMaximumWidth(72);
        box->setToolTip("Period in bars");
    }

    // Two toolbar rows so the chart pane stays usable at laptop widths.
    auto* toolbarTop = new QHBoxLayout;
    toolbarTop->setSpacing(10);
    toolbarTop->addWidget(m_chartSymbol);
    toolbarTop->addWidget(m_chartName, 1);
    toolbarTop->addLayout(tfRow);
    auto* toolbarBottom = new QHBoxLayout;
    toolbarBottom->setSpacing(10);
    toolbarBottom->addWidget(m_chartType);
    toolbarBottom->addSpacing(6);
    toolbarBottom->addWidget(m_smaCheck);
    toolbarBottom->addWidget(m_smaPeriod);
    toolbarBottom->addWidget(m_emaCheck);
    toolbarBottom->addWidget(m_emaPeriod);
    toolbarBottom->addWidget(m_volumeCheck);
    toolbarBottom->addStretch(1);
    // Drawing tools: exclusive tool buttons plus undo / delete / clear.
    m_drawTools = new QButtonGroup(this);
    m_drawTools->setExclusive(true);
    auto* drawRow = new QHBoxLayout;
    drawRow->setSpacing(4);
    auto* drawLabel = new QLabel("Draw", this);
    drawLabel->setObjectName("muted");
    drawRow->addWidget(drawLabel);
    const char* toolLabels[] = { "Cursor", "Trend", "Support", "Resistance", "Edit" };
    const char* toolTips[] = {
        "Pan and zoom the chart; drawings are not editable",
        "Drag (or click, move, click) to draw a trend line; it extends to the right as a dashed ray",
        "Drag vertically to shade a support zone between two prices",
        "Drag vertically to shade a resistance zone between two prices",
        "Select drawings: drag handles or bodies to move them, press Delete to remove the selected one",
    };
    for (int i = 0; i < 5; ++i) {
        auto* button = new QToolButton(this);
        button->setText(toolLabels[i]);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(toolTips[i]);
        m_drawTools->addButton(button, i);
        drawRow->addWidget(button);
    }
    m_drawTools->button(0)->setChecked(true);
    drawRow->addSpacing(8);
    auto makeAction = [this](const char* text, const char* tip) {
        auto* button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tip);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    m_undoDraw = makeAction("Undo", "Remove the most recent drawing");
    m_deleteDraw = makeAction("Delete", "Remove the selected drawing (Edit tool)");
    m_clearDraw = makeAction("Clear", "Remove every drawing on this symbol");
    drawRow->addWidget(m_undoDraw);
    drawRow->addWidget(m_deleteDraw);
    drawRow->addWidget(m_clearDraw);
    m_drawHint = new QLabel("Saved per symbol · Esc returns to the cursor", this);
    m_drawHint->setObjectName("muted");
    m_drawHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    drawRow->addSpacing(8);
    drawRow->addWidget(m_drawHint, 1);

    auto* toolbar = new QVBoxLayout;
    toolbar->setSpacing(6);
    toolbar->addLayout(toolbarTop);
    toolbar->addLayout(toolbarBottom);
    toolbar->addLayout(drawRow);

    m_view = new QWebEngineView(this);
    m_view->setMinimumSize(420, 300);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    m_page = new ChartWebPage(m_view);
    m_page->onMessage = [this](const QString& kind, const QString& payload) { onPageMessage(kind, payload); };
    m_view->setPage(m_page);
    m_view->page()->setBackgroundColor(QColor(m_theme.window.isEmpty() ? "#0a0f1c" : m_theme.window));
    m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    m_view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    m_view->setHtml(chartPageHtml());

    m_chartStatus = new QLabel(this);
    m_chartStatus->setObjectName("muted");
    m_chartStatus->setWordWrap(true);

    // Status text on the left, actions on the right, under the chart.
    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    footer->addWidget(m_chartStatus, 1);
    footer->addWidget(m_saveImage);
    footer->addWidget(m_openChain);

    auto* chartBox = new QFrame(this);
    chartBox->setObjectName("pane");
    auto* chartLayout = new QVBoxLayout(chartBox);
    chartLayout->setContentsMargins(12, 12, 12, 12);
    chartLayout->setSpacing(8);
    chartLayout->addLayout(toolbar);
    chartLayout->addWidget(m_view, 1);
    chartLayout->addLayout(footer);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("quotesSplitter");   // own handle rule in Theme.cpp (thin vertical grip)
    splitter->setHandleWidth(6);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(watchBox);
    splitter->addWidget(chartBox);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 430, 900 });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->addWidget(splitter, 1);
}

void QuotesTab::wire()
{
    connect(m_addButton, &QPushButton::clicked, this, [this] { addTicker(m_tickerEdit->text()); m_tickerEdit->clear(); });
    connect(m_tickerEdit, &QLineEdit::returnPressed, this, [this] { addTicker(m_tickerEdit->text()); m_tickerEdit->clear(); });
    connect(m_removeButton, &QPushButton::clicked, this, [this] { removeSelectedTicker(); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refreshQuotes(); });
    connect(m_autoRefresh, &QCheckBox::toggled, this, [this](bool) { updateTimers(); });
    connect(m_refreshInterval, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { updateTimers(); });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {
        if (m_updating) return;
        const QString symbol = tickerAtRow(row);
        if (!symbol.isEmpty() && symbol != m_chartTicker) loadChart(symbol);
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString symbol = tickerAtRow(row);
        if (!symbol.isEmpty() && onOpenInChain) onOpenInChain(symbol);
    });
    // Remember column widths and the sort choice.
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this, [this](int, int, int) { saveHeaderState(); });
    connect(m_table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, [this](int, Qt::SortOrder) { saveHeaderState(); });
    connect(m_openChain, &QPushButton::clicked, this, [this] {
        if (!m_chartTicker.isEmpty() && onOpenInChain) onOpenInChain(m_chartTicker);
    });
    connect(m_saveImage, &QPushButton::clicked, this, [this] {
        if (m_chartTicker.isEmpty()) return;
        const QString suggested = QStringLiteral("%1_%2.png").arg(m_chartTicker, timeframes()[static_cast<size_t>(m_timeframeIndex)].label);
        const QString path = QFileDialog::getSaveFileName(this, "Save chart image", suggested, "PNG image (*.png)");
        if (path.isEmpty()) return;
        saveChartImage(path, [this](const QString& written) {
            ui::setStatus(m_chartStatus, written.isEmpty() ? QStringLiteral("The chart could not be rendered to an image.") : QStringLiteral("Saved %1.").arg(written),
                          written.isEmpty() ? ui::StatusKind::Error : ui::StatusKind::Info);
        });
    });
    connect(m_drawTools, &QButtonGroup::idClicked, this, [this](int id) {
        if (id >= 0 && id < drawToolNames().size()) runJs(QStringLiteral("chartApi.setTool('%1');").arg(drawToolNames().at(id)));
        m_view->setFocus();   // so Delete / Esc reach the page
    });
    connect(m_undoDraw, &QToolButton::clicked, this, [this] { runJs(QStringLiteral("chartApi.undoDrawing();")); });
    connect(m_deleteDraw, &QToolButton::clicked, this, [this] { runJs(QStringLiteral("chartApi.deleteSelected();")); });
    connect(m_clearDraw, &QToolButton::clicked, this, [this] { runJs(QStringLiteral("chartApi.clearDrawings();")); });
    connect(m_timeframeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_timeframeIndex = id;
        QSettings().setValue(kTimeframeKey, id);
        if (!m_chartTicker.isEmpty()) loadChart(m_chartTicker);
    });
    connect(m_chartType, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        QSettings().setValue(kChartTypeKey, m_chartType->currentData().toString());
        pushOptions();
    });
    for (QCheckBox* box : { m_smaCheck, m_emaCheck, m_volumeCheck }) {
        connect(box, &QCheckBox::toggled, this, [this](bool) { pushOptions(); });
    }
    for (QSpinBox* box : { m_smaPeriod, m_emaPeriod }) {
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { pushOptions(); });
    }
    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        m_pageReady = ok;
        if (!ok) {
            setStatus("The chart page failed to load.", ui::StatusKind::Error);
            return;
        }
        runJs(QStringLiteral("chartApi.init(%1);").arg(themeJson()));
        pushOptions();
        for (const QString& script : m_pendingJs) m_view->page()->runJavaScript(script);
        m_pendingJs.clear();
        if (!m_bars.bars.empty()) { pushBars(); pushDrawings(); }
    });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        refreshQuotes();
        // Intraday charts move; re-pull the bars with the quotes.
        if (!m_chartTicker.isEmpty() && timeframes()[static_cast<size_t>(m_timeframeIndex)].intraday && !m_loadingChart) loadChart(m_chartTicker);
    });

    const QSettings settings;
    m_timeframeIndex = std::clamp(settings.value(kTimeframeKey, 6).toInt(), 0, static_cast<int>(timeframes().size()) - 1);
    if (QAbstractButton* button = m_timeframeGroup->button(m_timeframeIndex)) button->setChecked(true);
    const int typeIndex = m_chartType->findData(settings.value(kChartTypeKey, "candles").toString());
    m_chartType->setCurrentIndex(std::max(0, typeIndex));
    const QByteArray header = settings.value(kHeaderKey).toByteArray();
    if (!header.isEmpty()) m_table->horizontalHeader()->restoreState(header);
}

void QuotesTab::saveHeaderState() const
{
    if (m_updating) return;
    QSettings().setValue(kHeaderKey, m_table->horizontalHeader()->saveState());
}

QString QuotesTab::tickerAtRow(int row) const
{
    const QTableWidgetItem* item = (row >= 0 && row < m_table->rowCount()) ? m_table->item(row, ColTicker) : nullptr;
    return item ? item->data(kTickerRole).toString() : QString();
}

int QuotesTab::rowForTicker(const QString& symbol) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (tickerAtRow(row) == symbol) return row;
    }
    return -1;
}

// MARK: - Watchlist

void QuotesTab::loadWatchlist()
{
    const QSettings settings;
    m_watchlist = settings.value(kWatchlistKey, QStringList{ "AAPL", "NVDA", "MSFT", "AMZN", "TSLA", "SPY", "QQQ" }).toStringList();
}

void QuotesTab::saveWatchlist() const
{
    QSettings settings;
    settings.setValue(kWatchlistKey, m_watchlist);
}

void QuotesTab::addTicker(const QString& ticker)
{
    const QString symbol = ticker.trimmed().toUpper();
    if (symbol.isEmpty()) return;
    if (!m_watchlist.contains(symbol)) {
        m_watchlist << symbol;
        saveWatchlist();
        rebuildTable();
        refreshQuotes();
    }
}

void QuotesTab::removeSelectedTicker()
{
    const int row = m_table->currentRow();
    const QString removed = tickerAtRow(row);
    if (removed.isEmpty()) return;
    m_watchlist.removeAll(removed);
    m_quotes.erase(removed);
    saveWatchlist();
    rebuildTable();
    if (m_chartTicker == removed && m_table->rowCount() > 0) {
        m_table->selectRow(std::min(row, m_table->rowCount() - 1));
    }
}

void QuotesTab::rebuildTable()
{
    m_updating = true;
    const QString selected = selectedTicker();
    // Fill with sorting off so rows do not move mid-fill; re-enabling re-applies the
    // header's sort indicator (none by default, i.e. watchlist order).
    m_table->setSortingEnabled(false);
    m_table->setRowCount(static_cast<int>(m_watchlist.size()));
    for (int row = 0; row < static_cast<int>(m_watchlist.size()); ++row) {
        const QString& symbol = m_watchlist.at(row);
        auto* tickerItem = ui::makeCell(symbol, Qt::AlignLeft | Qt::AlignVCenter);
        tickerItem->setData(kTickerRole, symbol);
        QFont f = tickerItem->font();
        f.setBold(true);
        tickerItem->setFont(f);
        tickerItem->setForeground(QBrush(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2)));
        m_table->setItem(row, ColTicker, tickerItem);
        const auto it = m_quotes.find(symbol);
        if (it != m_quotes.end()) {
            fillQuoteRow(row, it->second);
        } else {
            for (int col : { ColLast, ColChange, ColPercent }) m_table->setItem(row, col, new NumericItem("–", 0.0));
        }
    }
    m_table->setSortingEnabled(true);
    const int selectedRow = rowForTicker(selected);
    if (selectedRow >= 0) m_table->selectRow(selectedRow);
    m_updating = false;
}

void QuotesTab::fillQuoteRow(int row, const MarketDataClient::Quote& quote)
{
    const QColor color(changeColor(m_theme, quote.change));
    auto* last = new NumericItem(ui::number(quote.last, 2), quote.last);
    QFont f = last->font();
    f.setBold(true);
    last->setFont(f);
    auto* change = new NumericItem(QStringLiteral("%1%2").arg(quote.change >= 0 ? "+" : "−", ui::number(std::fabs(quote.change), 2)), quote.change);
    auto* percent = new NumericItem(QStringLiteral("%1%2%").arg(quote.changePercent >= 0 ? "+" : "−", ui::number(std::fabs(quote.changePercent), 2)), quote.changePercent);
    change->setForeground(QBrush(color));
    percent->setForeground(QBrush(color));
    last->setForeground(QBrush(color));
    const QString tip = QStringLiteral("%1\nOpen %2 · High %3 · Low %4 · Volume %5\nPrevious close %6 · as of %7")
                            .arg(m_names.count(quote.ticker) ? m_names.at(quote.ticker) : quote.ticker,
                                 ui::number(quote.dayOpen, 2), ui::number(quote.dayHigh, 2), ui::number(quote.dayLow, 2),
                                 QLocale(QLocale::English).toString(quote.dayVolume, 'f', 0), ui::number(quote.previousClose, 2),
                                 quote.asOf.isValid() ? quote.asOf.toString("HH:mm") : QStringLiteral("–"));
    for (QTableWidgetItem* item : { last, change, percent }) item->setToolTip(tip);
    if (QTableWidgetItem* tickerItem = m_table->item(row, ColTicker)) tickerItem->setToolTip(tip);
    m_table->setItem(row, ColLast, last);
    m_table->setItem(row, ColChange, change);
    m_table->setItem(row, ColPercent, percent);
}

QString QuotesTab::selectedTicker() const
{
    return tickerAtRow(m_table->currentRow());
}

void QuotesTab::refreshQuotes()
{
    if (m_watchlist.isEmpty()) {
        setStatus("Watchlist is empty. Add a ticker above.", ui::StatusKind::Info);
        return;
    }
    if (!m_client.hasApiKey()) {
        setStatus("No Massive API key. Set MASSIVE_API_KEY or POLYGON_API_KEY, or use Market > Set Massive API Key.", ui::StatusKind::Error);
        return;
    }
    m_client.fetchQuotes(m_watchlist, [this](const std::vector<MarketDataClient::Quote>& quotes) {
        QDateTime newest;
        for (const MarketDataClient::Quote& q : quotes) {
            m_quotes[q.ticker] = q;
            if (q.asOf.isValid() && (!newest.isValid() || q.asOf > newest)) newest = q.asOf;
        }
        m_updating = true;
        const QString selected = selectedTicker();
        m_table->setSortingEnabled(false);
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const auto it = m_quotes.find(tickerAtRow(row));
            if (it != m_quotes.end()) fillQuoteRow(row, it->second);
        }
        m_table->setSortingEnabled(true);   // re-sorts with the fresh values
        const int selectedRow = rowForTicker(selected);
        if (selectedRow >= 0 && selectedRow != m_table->currentRow()) m_table->selectRow(selectedRow);
        m_updating = false;
        QString status = QStringLiteral("%1 quote%2 refreshed at %3").arg(quotes.size()).arg(quotes.size() == 1 ? "" : "s").arg(QTime::currentTime().toString("HH:mm:ss"));
        if (newest.isValid()) {
            const qint64 delay = newest.secsTo(QDateTime::currentDateTime()) / 60;
            status += QStringLiteral(" · vendor data as of %1 (%2 min delayed)").arg(newest.toString("HH:mm")).arg(std::max<qint64>(0, delay));
        }
        setStatus(status, ui::StatusKind::Info);
        if (m_chartTicker.isEmpty() && m_table->rowCount() > 0 && m_table->currentRow() < 0) {
            m_table->selectRow(0);
        }
    }, [this](const QString& message) { setStatus(message, ui::StatusKind::Error); });
}

void QuotesTab::showTicker(const QString& ticker)
{
    const QString symbol = ticker.trimmed().toUpper();
    if (symbol.isEmpty()) return;
    addTicker(symbol);
    const int row = rowForTicker(symbol);
    if (row >= 0) m_table->selectRow(row);   // selection change loads the chart
    if (symbol != m_chartTicker) loadChart(symbol);   // e.g. the row was already current
}

void QuotesTab::updateTimers()
{
    m_timer->setInterval(m_refreshInterval->value() * 1000);
    if (m_autoRefresh->isChecked()) m_timer->start();
    else m_timer->stop();
}

void QuotesTab::setStatus(const QString& text, ui::StatusKind kind)
{
    ui::setStatus(m_quoteStatus, text, kind);
}

// MARK: - Chart

void QuotesTab::loadChart(const QString& ticker)
{
    const QString symbol = ticker.trimmed().toUpper();
    if (symbol.isEmpty() || !m_client.hasApiKey()) return;
    m_chartTicker = symbol;
    m_chartSymbol->setText(symbol);
    m_chartName->setText(m_names.count(symbol) ? m_names.at(symbol) : QString());
    const Timeframe& tf = timeframes()[static_cast<size_t>(m_timeframeIndex)];
    const QDate today = QDate::currentDate();
    m_loadingChart = true;
    ui::setStatus(m_chartStatus, QStringLiteral("Loading %1 %2 bars…").arg(symbol, tf.label), ui::StatusKind::Info);
    m_client.fetchAggregates(symbol, tf.multiplier, tf.timespan, today.addDays(-tf.lookbackDays), today,
        [this, symbol, tf](const MarketDataClient::BarSeries& series) {
            m_loadingChart = false;
            if (symbol != m_chartTicker) return;   // user moved on
            m_bars = series;
            pushBars();
            pushDrawings();
            const MarketDataClient::Bar& last = series.bars.back();
            ui::setStatus(m_chartStatus, QStringLiteral("%1 · %2 bars (%3) from %4 to %5 · last %6 at %7")
                                             .arg(symbol).arg(series.bars.size()).arg(tf.label)
                                             .arg(QDateTime::fromMSecsSinceEpoch(series.bars.front().timeMs).toString("yyyy-MM-dd"),
                                                  QDateTime::fromMSecsSinceEpoch(last.timeMs).toString("yyyy-MM-dd"),
                                                  ui::number(last.close, 2), QDateTime::fromMSecsSinceEpoch(last.timeMs).toString("yyyy-MM-dd HH:mm")),
                          ui::StatusKind::Info);
            if (!m_names.count(symbol)) {
                m_client.fetchTickerDetails(symbol, [this, symbol](const MarketDataClient::TickerDetails& details) {
                    m_names[symbol] = details.name;
                    if (symbol == m_chartTicker) {
                        m_chartName->setText(details.name);
                        pushBars();
                    }
                }, [](const QString&) {});
            }
        },
        [this, symbol](const QString& message) {
            m_loadingChart = false;
            if (symbol == m_chartTicker) ui::setStatus(m_chartStatus, message, ui::StatusKind::Error);
        });
}

void QuotesTab::pushBars()
{
    const Timeframe& tf = timeframes()[static_cast<size_t>(m_timeframeIndex)];
    QJsonArray bars;
    for (const MarketDataClient::Bar& b : m_bars.bars) {
        QJsonObject o;
        o["t"] = static_cast<double>(b.timeMs / 1000);
        o["o"] = b.open;
        o["h"] = b.high;
        o["l"] = b.low;
        o["c"] = b.close;
        o["v"] = b.volume;
        bars.append(o);
    }
    QJsonObject meta;
    meta["symbol"] = m_chartTicker;
    meta["name"] = m_names.count(m_chartTicker) ? m_names.at(m_chartTicker) : QString();
    meta["timeframe"] = tf.label;
    meta["intraday"] = tf.intraday;
    const auto quote = m_quotes.find(m_chartTicker);
    if (quote != m_quotes.end() && quote->second.previousClose > 0.0 && tf.intraday) meta["previousClose"] = quote->second.previousClose;
    if (!m_bars.bars.empty()) {
        meta["asOf"] = QStringLiteral("as of %1").arg(QDateTime::fromMSecsSinceEpoch(m_bars.bars.back().timeMs).toString("yyyy-MM-dd HH:mm"));
    }
    QJsonObject payload;
    payload["bars"] = bars;
    payload["meta"] = meta;
    runJs(QStringLiteral("chartApi.setBars(%1);").arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
}

void QuotesTab::pushOptions()
{
    QJsonObject sma;
    sma["on"] = m_smaCheck->isChecked();
    sma["period"] = m_smaPeriod->value();
    QJsonObject ema;
    ema["on"] = m_emaCheck->isChecked();
    ema["period"] = m_emaPeriod->value();
    QJsonObject opts;
    opts["type"] = m_chartType->currentData().toString();
    opts["sma"] = sma;
    opts["ema"] = ema;
    opts["volume"] = m_volumeCheck->isChecked();
    runJs(QStringLiteral("chartApi.setOptions(%1);").arg(QString::fromUtf8(QJsonDocument(opts).toJson(QJsonDocument::Compact))));
}

QString QuotesTab::themeJson() const
{
    QJsonObject t;
    t["bg"] = m_theme.surface.isEmpty() ? "#121a2b" : m_theme.surface;
    t["text"] = m_theme.text.isEmpty() ? "#c7d2e3" : m_theme.text;
    t["grid"] = m_theme.gridLine.isEmpty() ? "#1f2a3f" : m_theme.gridLine;
    t["border"] = m_theme.border.isEmpty() ? "#273449" : m_theme.border;
    t["up"] = m_theme.up.isEmpty() ? "#22c55e" : m_theme.up;
    t["down"] = m_theme.down.isEmpty() ? "#ef4444" : m_theme.down;
    t["accent"] = m_theme.accent.isEmpty() ? "#3b82f6" : m_theme.accent;
    t["accent2"] = m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2;
    t["accent3"] = m_theme.accent3.isEmpty() ? "#22d3ee" : m_theme.accent3;
    t["crosshair"] = m_theme.textMuted.isEmpty() ? "#8294ad" : m_theme.textMuted;
    return QString::fromUtf8(QJsonDocument(t).toJson(QJsonDocument::Compact));
}

void QuotesTab::pushTheme()
{
    runJs(QStringLiteral("chartApi.setTheme(%1);").arg(themeJson()));
}

const QStringList& QuotesTab::drawToolNames()
{
    static const QStringList names = { "cursor", "trend", "support", "resistance", "edit" };
    return names;
}

void QuotesTab::pushDrawings()
{
    if (m_chartTicker.isEmpty()) return;
    auto it = m_drawings.find(m_chartTicker);
    if (it == m_drawings.end()) {
        const QString stored = QSettings().value(kDrawingsPrefix + m_chartTicker).toString();
        it = m_drawings.emplace(m_chartTicker, stored.isEmpty() ? QStringLiteral("[]") : stored).first;
    }
    runJs(QStringLiteral("chartApi.setDrawings(%1);").arg(it->second));
}

void QuotesTab::onPageMessage(const QString& kind, const QString& payload)
{
    if (kind == "drawings") {
        const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
        const QString symbol = obj.value("symbol").toString();
        if (symbol.isEmpty()) return;
        const QString items = QString::fromUtf8(QJsonDocument(obj.value("items").toArray()).toJson(QJsonDocument::Compact));
        m_drawings[symbol] = items;
        QSettings settings;
        if (items == "[]") settings.remove(kDrawingsPrefix + symbol);
        else settings.setValue(kDrawingsPrefix + symbol, items);
    } else if (kind == "tool") {
        // The page returns to the cursor after a drawing is placed or on Esc; mirror it.
        const int id = static_cast<int>(drawToolNames().indexOf(payload));
        if (QAbstractButton* button = id >= 0 ? m_drawTools->button(id) : nullptr) button->setChecked(true);
    }
}

void QuotesTab::debugSimulateDrawings(std::function<void(int)> done)
{
    if (!m_pageReady) { done(0); return; }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.simulateDrawings()"), [done](const QVariant& result) { done(result.toInt()); });
}

bool QuotesTab::hasStoredDrawings(const QString& symbol) const
{
    return QSettings().contains(kDrawingsPrefix + symbol.trimmed().toUpper());
}

void QuotesTab::runJs(const QString& script)
{
    if (!m_pageReady) {
        m_pendingJs << script;
        return;
    }
    m_view->page()->runJavaScript(script);
}

void QuotesTab::saveChartImage(const QString& path, std::function<void(const QString&)> done)
{
    if (!m_pageReady) {
        done(QString());
        return;
    }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.screenshot()"), [path, done](const QVariant& result) {
        const QString dataUrl = result.toString();
        const int comma = static_cast<int>(dataUrl.indexOf(','));
        if (!dataUrl.startsWith("data:image/png;base64,") || comma < 0) {
            done(QString());
            return;
        }
        const QByteArray bytes = QByteArray::fromBase64(dataUrl.mid(comma + 1).toLatin1());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            done(QString());
            return;
        }
        done(path);
    });
}

void QuotesTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    m_view->page()->setBackgroundColor(QColor(theme.surface));
    pushTheme();
    rebuildTable();
}

QString QuotesTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    s << "ticker,last,change,change_percent,previous_close,day_open,day_high,day_low,day_volume,as_of\n";
    for (const QString& symbol : m_watchlist) {
        const auto it = m_quotes.find(symbol);
        if (it == m_quotes.end()) { s << symbol << "\n"; continue; }
        const MarketDataClient::Quote& q = it->second;
        s << symbol << "," << q.last << "," << q.change << "," << q.changePercent << "," << q.previousClose << "," << q.dayOpen << ","
          << q.dayHigh << "," << q.dayLow << "," << q.dayVolume << "," << q.asOf.toString(Qt::ISODate) << "\n";
    }
    if (!m_bars.bars.empty()) {
        s << "\nticker,timeframe,time,open,high,low,close,volume\n";
        const Timeframe& tf = timeframes()[static_cast<size_t>(m_timeframeIndex)];
        for (const MarketDataClient::Bar& b : m_bars.bars) {
            s << m_chartTicker << "," << tf.label << "," << QDateTime::fromMSecsSinceEpoch(b.timeMs).toString(Qt::ISODate) << ","
              << b.open << "," << b.high << "," << b.low << "," << b.close << "," << b.volume << "\n";
        }
    }
    return out;
}
