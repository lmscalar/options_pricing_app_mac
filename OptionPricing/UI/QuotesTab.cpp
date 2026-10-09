//
//  QuotesTab.cpp
//  OptionPricing
//

#include "QuotesTab.h"
#include "ChartPage.h"
#include "Formatting.h"

#include <QtCore/QSet>

#include <algorithm>
#include <cmath>

namespace {
enum Column { ColTicker = 0, ColLast, ColChange, ColPercent, ColumnCount };
constexpr const char* kWatchlistKey = "quotes/watchlist";             ///< the active list (kept for compatibility)
constexpr const char* kWatchlistsKey = "quotes/watchlists";           ///< JSON object: name -> [tickers]
constexpr const char* kActiveWatchlistKey = "quotes/activeWatchlist";
constexpr const char* kDefaultWatchlistName = "Default";
constexpr const char* kTimeframeKey = "quotes/timeframe";
constexpr const char* kChartTypeKey = "quotes/chartType";
constexpr const char* kIndicatorsKey = "quotes/indicators";  ///< JSON array, see QuotesTab::indicators()
constexpr const char* kVolumeKey = "quotes/volume";
constexpr const char* kPriceLineKey = "quotes/priceLine";
constexpr const char* kPaneHeightKey = "quotes/paneHeight";   ///< indicator pane height as a fraction of the chart
/// Overlay colours handed out in order to new moving averages (first unused wins).
const QStringList& indicatorPalette()
{
    static const QStringList palette{ "#f59e0b", "#22d3ee", "#a78bfa", "#f472b6", "#34d399", "#fb923c" };
    return palette;
}
const QStringList& indicatorPaletteNames()
{
    static const QStringList names{ "Amber", "Cyan", "Violet", "Pink", "Green", "Orange" };
    return names;
}
QIcon swatchIcon(const QString& color)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(color));
    p.drawRoundedRect(QRectF(1, 1, 12, 12), 3, 3);
    return QIcon(pm);
}
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
/// Extracts ticker symbols from free text: "AAPL,NVDA,IBM", one per line, space-separated, or
/// cells copied from a spreadsheet (a row is tab-separated, a column is one per line). When a
/// copied block has a header such as "Ticker" or "Symbol", only that column is used.
QStringList parseTickers(const QString& rawText)
{
    static const QRegularExpression headerWord("^(ticker|tickers|symbol|symbols|stock|stocks|name|company|instrument)$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tickerShape("^[A-Z][A-Z0-9.\\-]{0,7}$");
    auto clean = [](QString token) {
        token = token.trimmed();
        token.remove(QRegularExpression("^=?[\"'$]+|[\"')]+$"));   // Excel ="AAPL", quotes, a leading $
        token.remove(QRegularExpression("[.:,;]+$"));
        return token.toUpper();
    };
    QString text = rawText;
    text.replace("\r\n", "\n").replace('\r', '\n');
    QStringList tokens;
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    const bool sheet = std::any_of(lines.begin(), lines.end(), [](const QString& l) { return l.contains('\t'); });
    if (sheet) {
        // Spreadsheet block: find a header cell naming the ticker column; otherwise take every cell.
        int column = -1;
        const QStringList header = lines.first().split('\t');
        for (int i = 0; i < header.size(); ++i) {
            if (headerWord.match(header[i].trimmed()).hasMatch() && !header[i].trimmed().toLower().startsWith("name") && !header[i].trimmed().toLower().startsWith("company")) { column = i; break; }
        }
        for (int r = 0; r < lines.size(); ++r) {
            const QStringList cells = lines[r].split('\t');
            if (column >= 0) { if (r > 0 && column < cells.size()) tokens << cells[column]; }
            else tokens << cells;
        }
    } else {
        tokens = text.split(QRegularExpression("[\\s,;|]+"), Qt::SkipEmptyParts);
    }
    QStringList tickers;
    for (const QString& token : tokens) {
        const QString symbol = clean(token);
        if (symbol.isEmpty() || headerWord.match(symbol).hasMatch()) continue;
        if (tickerShape.match(symbol).hasMatch() && !tickers.contains(symbol)) tickers << symbol;
    }
    return tickers;
}

/// Bars are stamped at the start of their window in New York time; daily bars therefore
/// carry a session date that must be read in that zone, not in the local one.
const QTimeZone& exchangeZone()
{
    static const QTimeZone zone("America/New_York");
    return zone;
}

QDate sessionDate(qint64 timeMs)
{
    return QDateTime::fromMSecsSinceEpoch(timeMs, exchangeZone()).date();
}

QDate exchangeToday()
{
    return QDateTime::currentDateTime().toTimeZone(exchangeZone()).date();
}

/// Human label for a bar: the session date for daily/weekly bars, local wall-clock time for intraday ones.
QString barLabel(qint64 timeMs, bool intraday)
{
    return intraday ? QDateTime::fromMSecsSinceEpoch(timeMs).toString("yyyy-MM-dd HH:mm") : sessionDate(timeMs).toString(Qt::ISODate);
}
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
        // Keep the active ticker's row and the chart's live readout in step with the headline.
        syncActiveQuote();
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

    // Named watchlists: pick one to load it (and preload its option chains); the menu
    // creates, copies, renames, deletes, imports and exports lists.
    m_watchlistCombo = new QComboBox(this);
    m_watchlistCombo->setToolTip("Saved watchlists. Loading one replaces the table, refreshes quotes and preloads its option chains into memory.");
    m_watchlistCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_watchlistMenu = new QToolButton(this);
    m_watchlistMenu->setText("Lists ▾");
    m_watchlistMenu->setPopupMode(QToolButton::InstantPopup);
    m_watchlistMenu->setCursor(Qt::PointingHandCursor);
    m_watchlistMenu->setToolTip("Create, copy, rename, delete, import or export watchlists");
    auto* menu = new QMenu(m_watchlistMenu);
    menu->addAction("New Watchlist…", this, [this] { promptNewWatchlist(false); });
    menu->addAction("Save Current As…", this, [this] { promptNewWatchlist(true); });
    menu->addAction("Rename…", this, [this] { promptRenameWatchlist(); });
    m_deleteWatchlistAction = menu->addAction("Delete Watchlist", this, [this] { promptDeleteWatchlist(); });
    menu->addSeparator();
    menu->addAction("New Watchlist from Clipboard…", this, [this] { createWatchlistFromClipboard(); });
    menu->addAction("Add Clipboard Tickers to Current List", this, [this] { addTickersFromClipboard(); });
    menu->addAction("Import Tickers from File…", this, [this] { importWatchlistFile(); });
    menu->addAction("Export Tickers to File…", this, [this] { exportWatchlistFile(); });
    m_watchlistMenu->setMenu(menu);
    auto* listRow = new QHBoxLayout;
    listRow->setSpacing(8);
    auto* listLabel = new QLabel("Watchlist", this);
    listLabel->setObjectName("muted");
    listRow->addWidget(listLabel);
    listRow->addWidget(m_watchlistCombo, 1);
    listRow->addWidget(m_watchlistMenu);

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
    watchLayout->addLayout(listRow);
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
    // Technical indicators live in a drop-down menu: add up to three SMAs and three EMAs
    // (each with its own period and colour), one MACD with editable parameters, and the
    // volume histogram. The chosen set is saved and restored on the next launch.
    m_indicatorsButton = new QToolButton(this);
    m_indicatorsButton->setText("Indicators ▾");
    m_indicatorsButton->setPopupMode(QToolButton::InstantPopup);
    m_indicatorsButton->setCursor(Qt::PointingHandCursor);
    m_indicatorsButton->setToolTip("Add, edit or remove moving averages and the MACD; toggle the volume histogram");
    m_indicatorsMenu = new QMenu(this);
    m_indicatorsButton->setMenu(m_indicatorsMenu);
    m_volumeAction = new QAction("Volume", this);
    m_volumeAction->setCheckable(true);
    m_volumeAction->setChecked(QSettings().value(kVolumeKey, true).toBool());
    m_openChain = ui::makeButton(this, "Open Option Chain", "secondary", "Fetch this ticker's option chain on the Option Chain tab");
    m_saveImage = ui::makeButton(this, "Save Image…", "secondary", "Save the chart as a PNG image");
    m_resetChart = ui::makeButton(this, "Reset", "secondary", "Reset the chart view: reload the bars, restore autoscale and the default zoom, return to the cursor tool (drawings are kept)");
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
    toolbarBottom->addWidget(m_indicatorsButton);
    m_priceLineCheck = new QCheckBox("Price line", this);
    m_priceLineCheck->setChecked(QSettings().value(kPriceLineKey, true).toBool());
    m_priceLineCheck->setToolTip("Horizontal line at the live price (or the last close): a chart marker, not a drawing");
    toolbarBottom->addWidget(m_priceLineCheck);
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
    m_drawHint = new QLabel("Right-click a drawing to delete it · saved per symbol · Esc returns to the cursor", this);
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
    footer->addWidget(m_resetChart);
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
    root->setContentsMargins(12, 8, 12, 10);
    root->addWidget(splitter, 1);
}

void QuotesTab::wire()
{
    connect(m_watchlistCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_updatingWatchlists || index < 0) return;
        loadWatchlistNamed(m_watchlistCombo->itemText(index));
    });
    connect(m_addButton, &QPushButton::clicked, this, [this] { addTicker(m_tickerEdit->text()); m_tickerEdit->clear(); });
    connect(m_tickerEdit, &QLineEdit::returnPressed, this, [this] { addTicker(m_tickerEdit->text()); m_tickerEdit->clear(); });
    connect(m_removeButton, &QPushButton::clicked, this, [this] { removeSelectedTicker(); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refreshQuotes(); });
    connect(m_autoRefresh, &QCheckBox::toggled, this, [this](bool) { updateTimers(); });
    connect(m_refreshInterval, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { updateTimers(); });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {
        if (m_updating) return;
        const QString symbol = tickerAtRow(row);
        if (symbol.isEmpty() || symbol == m_chartTicker) return;
        loadChart(symbol);
        // Cascade to the other tabs only for user-driven changes (the table has focus) or an
        // explicit showTicker(); re-sorts, refreshes and the start-up selection never cascade.
        const bool userDriven = (m_table->hasFocus() && !m_autoSelecting) || m_cascadeSelection;
        if (onTickerSelected && userDriven) {
            qInfo("[ticker] watchlist selection %s (row %d)", qPrintable(symbol), row);
            onTickerSelected(symbol);
        }
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
    connect(m_resetChart, &QPushButton::clicked, this, [this] { resetChart(); });
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
    connect(m_priceLineCheck, &QCheckBox::toggled, this, [this](bool on) { QSettings().setValue(kPriceLineKey, on); pushOptions(); });
    connect(m_volumeAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(kVolumeKey, on);
        rebuildIndicatorsMenu();
        pushOptions();
    });
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
    loadIndicators();
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
    m_watchlists.clear();
    const QJsonObject stored = QJsonDocument::fromJson(settings.value(kWatchlistsKey).toString().toUtf8()).object();
    for (auto it = stored.begin(); it != stored.end(); ++it) {
        QStringList tickers;
        for (const QJsonValue v : it.value().toArray()) tickers << v.toString().trimmed().toUpper();
        tickers.removeAll(QString());
        if (!it.key().trimmed().isEmpty()) m_watchlists.insert(it.key().trimmed(), tickers);
    }
    if (m_watchlists.isEmpty()) {
        // First run with named lists: the single legacy list becomes "Default".
        m_watchlists.insert(kDefaultWatchlistName, settings.value(kWatchlistKey, QStringList{ "AAPL", "NVDA", "MSFT", "AMZN", "TSLA", "SPY", "QQQ" }).toStringList());
    }
    m_activeWatchlist = settings.value(kActiveWatchlistKey).toString();
    if (!m_watchlists.contains(m_activeWatchlist)) m_activeWatchlist = m_watchlists.firstKey();
    m_watchlist = m_watchlists.value(m_activeWatchlist);
    refreshWatchlistCombo();
}

void QuotesTab::saveWatchlist() const
{
    QSettings settings;
    settings.setValue(kWatchlistKey, m_watchlist);   // the active list, as before
    QJsonObject stored;
    for (auto it = m_watchlists.begin(); it != m_watchlists.end(); ++it) {
        stored[it.key()] = QJsonArray::fromStringList(it.key() == m_activeWatchlist ? m_watchlist : it.value());
    }
    settings.setValue(kWatchlistsKey, QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));
    settings.setValue(kActiveWatchlistKey, m_activeWatchlist);
}

// MARK: - Named watchlists

QStringList QuotesTab::watchlistNames() const
{
    return m_watchlists.keys();
}

void QuotesTab::refreshWatchlistCombo()
{
    m_updatingWatchlists = true;
    m_watchlistCombo->clear();
    m_watchlistCombo->addItems(m_watchlists.keys());
    m_watchlistCombo->setCurrentText(m_activeWatchlist);
    m_updatingWatchlists = false;
    if (m_deleteWatchlistAction) m_deleteWatchlistAction->setEnabled(m_watchlists.size() > 1);
}

bool QuotesTab::loadWatchlistNamed(const QString& rawName)
{
    const QString name = rawName.trimmed();
    if (!m_watchlists.contains(name)) return false;
    // Remember the outgoing list's current tickers before switching.
    m_watchlists[m_activeWatchlist] = m_watchlist;
    m_activeWatchlist = name;
    m_watchlist = m_watchlists.value(name);
    saveWatchlist();
    refreshWatchlistCombo();
    rebuildTable();
    if (!m_watchlist.contains(m_chartTicker)) {
        // Chart the first row of the new list (no cascade; the user's click decides that).
        m_autoSelecting = true;
        if (m_table->rowCount() > 0) m_table->selectRow(0);
        m_autoSelecting = false;
    }
    refreshQuotes();
    setStatus(QStringLiteral("Watchlist “%1” loaded: %2 ticker%3. Refreshing quotes and preloading option chains…").arg(name).arg(m_watchlist.size()).arg(m_watchlist.size() == 1 ? "" : "s"),
              ui::StatusKind::Info);
    if (onWatchlistChanged) onWatchlistChanged(m_watchlist);
    return true;
}

bool QuotesTab::createWatchlist(const QString& rawName, const QStringList& tickers, bool activate)
{
    const QString name = rawName.trimmed();
    if (name.isEmpty()) return false;
    QStringList clean;
    for (const QString& t : tickers) {
        const QString symbol = t.trimmed().toUpper();
        if (!symbol.isEmpty() && !clean.contains(symbol)) clean << symbol;
    }
    m_watchlists[m_activeWatchlist] = m_watchlist;
    m_watchlists.insert(name, clean);
    saveWatchlist();
    refreshWatchlistCombo();
    if (activate) return loadWatchlistNamed(name);
    return true;
}

bool QuotesTab::renameWatchlist(const QString& from, const QString& rawTo)
{
    const QString to = rawTo.trimmed();
    if (to.isEmpty() || !m_watchlists.contains(from) || (m_watchlists.contains(to) && to != from)) return false;
    const QStringList tickers = from == m_activeWatchlist ? m_watchlist : m_watchlists.value(from);
    m_watchlists.remove(from);
    m_watchlists.insert(to, tickers);
    if (m_activeWatchlist == from) m_activeWatchlist = to;
    saveWatchlist();
    refreshWatchlistCombo();
    return true;
}

bool QuotesTab::deleteWatchlist(const QString& name)
{
    if (!m_watchlists.contains(name) || m_watchlists.size() <= 1) return false;
    m_watchlists.remove(name);
    if (m_activeWatchlist == name) {
        m_activeWatchlist = m_watchlists.firstKey();
        m_watchlist = m_watchlists.value(m_activeWatchlist);
        saveWatchlist();
        refreshWatchlistCombo();
        rebuildTable();
        refreshQuotes();
        if (onWatchlistChanged) onWatchlistChanged(m_watchlist);
    } else {
        saveWatchlist();
        refreshWatchlistCombo();
    }
    return true;
}

void QuotesTab::promptNewWatchlist(bool copyCurrent)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, copyCurrent ? "Save Watchlist As" : "New Watchlist",
                                               copyCurrent ? QStringLiteral("Name for a copy of “%1”:").arg(m_activeWatchlist) : QStringLiteral("Name for the new (empty) watchlist:"),
                                               QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (m_watchlists.contains(name)) {
        QMessageBox::warning(this, "Watchlist exists", QStringLiteral("A watchlist named “%1” already exists.").arg(name));
        return;
    }
    createWatchlist(name, copyCurrent ? m_watchlist : QStringList(), true);
}

void QuotesTab::promptRenameWatchlist()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Rename Watchlist", QStringLiteral("New name for “%1”:").arg(m_activeWatchlist), QLineEdit::Normal, m_activeWatchlist, &ok).trimmed();
    if (!ok || name.isEmpty() || name == m_activeWatchlist) return;
    if (!renameWatchlist(m_activeWatchlist, name)) QMessageBox::warning(this, "Rename failed", QStringLiteral("A watchlist named “%1” already exists.").arg(name));
}

void QuotesTab::promptDeleteWatchlist()
{
    if (m_watchlists.size() <= 1) {
        QMessageBox::information(this, "Delete Watchlist", "The last watchlist cannot be deleted; create another one first.");
        return;
    }
    if (QMessageBox::question(this, "Delete Watchlist", QStringLiteral("Delete the watchlist “%1” (%2 tickers)?").arg(m_activeWatchlist).arg(m_watchlist.size())) == QMessageBox::Yes) {
        deleteWatchlist(m_activeWatchlist);
    }
}

void QuotesTab::importWatchlistFile()
{
    const QString path = QFileDialog::getOpenFileName(this, "Import tickers", QString(), "Text or CSV (*.txt *.csv);;All files (*)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QStringList tickers = parseTickers(QString::fromUtf8(file.readAll()));
    if (tickers.isEmpty()) {
        QMessageBox::warning(this, "Import tickers", "No ticker symbols were found in that file.");
        return;
    }
    const QString suggested = QFileInfo(path).completeBaseName();
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Import tickers", QStringLiteral("%1 tickers found. Name for the new watchlist:").arg(tickers.size()), QLineEdit::Normal, suggested, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    createWatchlist(m_watchlists.contains(name) ? name + " (imported)" : name, tickers, true);
}

void QuotesTab::exportWatchlistFile()
{
    const QString path = QFileDialog::getSaveFileName(this, "Export tickers", m_activeWatchlist + ".txt", "Text (*.txt);;CSV (*.csv)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write((m_watchlist.join(path.endsWith(".csv", Qt::CaseInsensitive) ? "," : "\n") + "\n").toUtf8());
        setStatus(QStringLiteral("Exported %1 tickers to %2.").arg(m_watchlist.size()).arg(path), ui::StatusKind::Info);
    }
}

void QuotesTab::addTicker(const QString& ticker)
{
    // Accepts one symbol or several ("AAPL, NVDA IBM"), e.g. pasted into the Add box.
    addTickers(parseTickers(ticker));
}

int QuotesTab::addTickers(const QStringList& tickers)
{
    int added = 0;
    for (const QString& symbol : tickers) {
        if (!symbol.isEmpty() && !m_watchlist.contains(symbol)) {
            m_watchlist << symbol;
            ++added;
        }
    }
    if (added > 0) {
        saveWatchlist();
        rebuildTable();
        refreshQuotes();
        if (onWatchlistChanged) onWatchlistChanged(m_watchlist);
    }
    return added;
}

QStringList QuotesTab::clipboardTickers() const
{
    return parseTickers(QGuiApplication::clipboard()->text());
}

bool QuotesTab::createWatchlistFromText(const QString& name, const QString& text)
{
    const QStringList tickers = parseTickers(text);
    if (tickers.isEmpty()) return false;
    return createWatchlist(name, tickers, true);
}

void QuotesTab::createWatchlistFromClipboard()
{
    const QStringList tickers = clipboardTickers();
    if (tickers.isEmpty()) {
        QMessageBox::information(this, "New Watchlist from Clipboard", "The clipboard holds no ticker symbols. Copy text such as “AAPL,NVDA,IBM,ORCL” first.");
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New Watchlist from Clipboard",
                                               QStringLiteral("%1 ticker%2 found: %3\n\nName for the new watchlist:").arg(tickers.size()).arg(tickers.size() == 1 ? "" : "s", tickers.join(", ")),
                                               QLineEdit::Normal, QStringLiteral("Pasted %1").arg(QDate::currentDate().toString("MMM d")), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (m_watchlists.contains(name)) {
        QMessageBox::warning(this, "Watchlist exists", QStringLiteral("A watchlist named “%1” already exists.").arg(name));
        return;
    }
    createWatchlist(name, tickers, true);
}

void QuotesTab::addTickersFromClipboard()
{
    const QStringList tickers = clipboardTickers();
    if (tickers.isEmpty()) {
        QMessageBox::information(this, "Add Tickers from Clipboard", "The clipboard holds no ticker symbols. Copy text such as “AAPL,NVDA,IBM,ORCL” first.");
        return;
    }
    const int added = addTickers(tickers);
    setStatus(QStringLiteral("%1 ticker%2 added to “%3” from the clipboard%4.").arg(added).arg(added == 1 ? "" : "s", m_activeWatchlist,
                                                                                      added < tickers.size() ? QStringLiteral(" (%1 already present)").arg(tickers.size() - added) : QString()),
              ui::StatusKind::Info);
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
    if (onWatchlistChanged) onWatchlistChanged(m_watchlist);
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

void QuotesTab::fillQuoteRow(int row, const MarketDataClient::Quote& vendorQuote)
{
    // The app-wide ticker shows the same figures as the headline (parity-implied spot when
    // enabled, one previous close); other rows show the vendor snapshot.
    MarketDataClient::Quote quote = vendorQuote;
    const bool shared = quote.ticker == m_state.underlyingTicker && m_state.market.spot > 0.0 && !m_state.spotSource.isEmpty();
    if (shared) {
        quote.last = m_state.market.spot;
        if (m_state.previousClose > 0.0) quote.previousClose = m_state.previousClose;
        quote.change = quote.previousClose > 0.0 ? quote.last - quote.previousClose : 0.0;
        quote.changePercent = quote.previousClose > 0.0 ? (quote.last / quote.previousClose - 1.0) * 100.0 : 0.0;
        if (m_state.spotAsOf.isValid()) quote.asOf = m_state.spotAsOf;
    }
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
    QString tip = QStringLiteral("%1\nOpen %2 · High %3 · Low %4 · Volume %5\nPrevious close %6 · as of %7")
                      .arg(m_names.count(quote.ticker) ? m_names.at(quote.ticker) : quote.ticker,
                           ui::number(quote.dayOpen, 2), ui::number(quote.dayHigh, 2), ui::number(quote.dayLow, 2),
                           QLocale(QLocale::English).toString(quote.dayVolume, 'f', 0), ui::number(quote.previousClose, 2),
                           quote.asOf.isValid() ? quote.asOf.toString("HH:mm") : QStringLiteral("–"));
    if (shared) {
        tip += QStringLiteral("\nSynchronized with the headline: %1%2")
                   .arg(m_state.spotSource == "option parity" ? QStringLiteral("parity-implied from options") : QStringLiteral("Massive %1").arg(m_state.spotSource),
                        m_state.vendorSpot > 0.0 && std::fabs(m_state.vendorSpot - quote.last) > 0.005 ? QStringLiteral(" · vendor %1").arg(ui::number(m_state.vendorSpot, 2)) : QString());
        QFont f = last->font();
        f.setBold(true);
        last->setFont(f);
    }
    for (QTableWidgetItem* item : { last, change, percent }) item->setToolTip(tip);
    if (QTableWidgetItem* tickerItem = m_table->item(row, ColTicker)) tickerItem->setToolTip(tip);
    m_table->setItem(row, ColLast, last);
    m_table->setItem(row, ColChange, change);
    m_table->setItem(row, ColPercent, percent);
}

void QuotesTab::syncActiveQuote()
{
    const QString symbol = m_state.underlyingTicker;
    if (symbol.isEmpty()) return;
    const int row = rowForTicker(symbol);
    if (row >= 0) {
        m_updating = true;
        m_table->setSortingEnabled(false);
        const auto it = m_quotes.find(symbol);
        MarketDataClient::Quote quote = it != m_quotes.end() ? it->second : MarketDataClient::Quote{};
        quote.ticker = symbol;
        if (quote.last <= 0.0) quote.last = m_state.vendorSpot > 0.0 ? m_state.vendorSpot : m_state.market.spot;
        if (quote.previousClose <= 0.0) quote.previousClose = m_state.previousClose;
        fillQuoteRow(row, quote);
        m_table->setSortingEnabled(true);
        m_updating = false;
    }
    if (symbol == m_chartTicker) pushLive();
}

void QuotesTab::pushLive()
{
    if (m_chartTicker.isEmpty()) return;
    QJsonObject live;
    if (m_chartTicker == m_state.underlyingTicker && m_state.market.spot > 0.0 && !m_state.spotSource.isEmpty()) {
        live["price"] = m_state.market.spot;
        if (m_state.previousClose > 0.0) live["previousClose"] = m_state.previousClose;
        live["source"] = m_state.spotSource == "option parity" ? QStringLiteral("parity-implied") : m_state.spotSource;
        if (m_state.spotAsOf.isValid()) live["asOf"] = m_state.spotAsOf.toString("HH:mm");
        else if (m_state.spotTime.isValid()) live["asOf"] = m_state.spotTime.toString("HH:mm");
    }
    runJs(QStringLiteral("chartApi.setLive(%1);").arg(live.isEmpty() ? QStringLiteral("null") : QString::fromUtf8(QJsonDocument(live).toJson(QJsonDocument::Compact))));
}

void QuotesTab::pinPreviousCloseFromBars()
{
    // Daily bars give the last completed session's close directly; that is the correct
    // basis for today's change, whatever the vendor's prevDay says around the overnight roll.
    const Timeframe& tf = timeframes()[static_cast<size_t>(m_timeframeIndex)];
    if (QString(tf.timespan) != "day" || m_bars.bars.size() < 2 || m_chartTicker != m_state.underlyingTicker) return;
    const QDate today = exchangeToday();
    const auto& bars = m_bars.bars;
    const QDate lastDate = sessionDate(bars.back().timeMs);
    // Today's (partial) bar is never the previous close; the bar before it is.
    const double previousClose = lastDate < today ? bars.back().close : bars[bars.size() - 2].close;
    if (m_state.setPreviousCloseFromBars(m_chartTicker, previousClose)) m_state.notify();
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
        if (m_store) m_store->putQuotes(quotes);
        // The active ticker's vendor quote also feeds the shared state (headline, chain, chart).
        bool stateChanged = false;
        for (const MarketDataClient::Quote& q : quotes) {
            if (q.ticker == m_state.underlyingTicker) stateChanged = m_state.updateVendorQuote(q.ticker, q.last, q.previousClose, q.asOf, "last minute bar") || stateChanged;
        }
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
        if (stateChanged) m_state.notify();
        if (m_chartTicker.isEmpty() && m_table->rowCount() > 0 && m_table->currentRow() < 0) {
            // Initial selection: chart the first row but do not cascade it to the other tabs,
            // which may hold a workspace the user opened; only user clicks cascade.
            m_autoSelecting = true;
            m_table->selectRow(0);
            m_autoSelecting = false;
        }
    }, [this](const QString& message) { setStatus(message, ui::StatusKind::Error); });
}

void QuotesTab::loadStoredQuotes()
{
    if (!m_store) return;
    const std::vector<MarketDataClient::Quote> stored = m_store->quotes();
    if (stored.empty()) return;
    for (const MarketDataClient::Quote& q : stored) {
        if (m_watchlist.contains(q.ticker)) m_quotes[q.ticker] = q;
    }
    rebuildTable();
    const QDateTime at = m_store->quotesFetchedAt();
    setStatus(QStringLiteral("Prices from the last session (saved %1) · refreshing…").arg(at.isValid() ? at.toString("yyyy-MM-dd HH:mm") : QStringLiteral("earlier")),
              ui::StatusKind::Info);
}

void QuotesTab::showTicker(const QString& ticker)
{
    const QString symbol = ticker.trimmed().toUpper();
    if (symbol.isEmpty()) return;
    addTicker(symbol);
    const int row = rowForTicker(symbol);
    m_cascadeSelection = true;
    if (row >= 0) m_table->selectRow(row);   // selection change loads the chart and cascades
    m_cascadeSelection = false;
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
    const int sequence = ++m_loadSequence;   // responses arriving out of order are ignored
    ui::setStatus(m_chartStatus, QStringLiteral("Loading %1 %2 bars…").arg(symbol, tf.label), ui::StatusKind::Info);
    m_client.fetchAggregates(symbol, tf.multiplier, tf.timespan, today.addDays(-tf.lookbackDays), today,
        [this, symbol, tf, sequence](const MarketDataClient::BarSeries& series) {
            if (sequence != m_loadSequence) return;   // a newer load (symbol or timeframe) superseded this one
            m_loadingChart = false;
            if (symbol != m_chartTicker) return;   // user moved on
            m_bars = series;
            pushBars();
            pushDrawings();
            pinPreviousCloseFromBars();
            pushLive();
            if (m_chartLoaded) { auto cb = std::move(m_chartLoaded); m_chartLoaded = nullptr; cb(true); }
            const MarketDataClient::Bar& last = series.bars.back();
            ui::setStatus(m_chartStatus, QStringLiteral("%1 · %2 bars (%3) from %4 to %5 · last %6 at %7")
                                             .arg(symbol).arg(series.bars.size()).arg(tf.label)
                                             .arg(barLabel(series.bars.front().timeMs, false), barLabel(last.timeMs, false),
                                                  ui::number(last.close, 2), barLabel(last.timeMs, tf.intraday)),
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
        [this, symbol, sequence](const QString& message) {
            if (sequence != m_loadSequence) return;
            m_loadingChart = false;
            if (symbol == m_chartTicker) ui::setStatus(m_chartStatus, message, ui::StatusKind::Error);
            if (m_chartLoaded) { auto cb = std::move(m_chartLoaded); m_chartLoaded = nullptr; cb(false); }
        });
}

// MARK: - Assistant hooks

QString QuotesTab::timeframeLabel() const
{
    return QString::fromLatin1(timeframes()[static_cast<size_t>(m_timeframeIndex)].label);
}

QStringList QuotesTab::timeframeLabels() const
{
    QStringList out;
    for (const Timeframe& tf : timeframes()) out << QString::fromLatin1(tf.label);
    return out;
}

bool QuotesTab::setTimeframe(const QString& label)
{
    const int index = static_cast<int>(timeframeLabels().indexOf(label.trimmed(), 0, Qt::CaseInsensitive));
    if (index < 0) return false;
    if (index == m_timeframeIndex) return true;
    m_timeframeIndex = index;
    if (QAbstractButton* button = m_timeframeGroup->button(index)) button->setChecked(true);
    QSettings().setValue(kTimeframeKey, index);
    if (!m_chartTicker.isEmpty()) loadChart(m_chartTicker);
    return true;
}

bool QuotesTab::setChartType(const QString& type)
{
    const QString wanted = type.trimmed().toLower();
    const QString key = wanted.startsWith("candle") ? "candles" : (wanted.startsWith("bar") ? "bars" : (wanted.startsWith("heik") ? "heikin" : (wanted == "line" ? "line" : QString())));
    const int index = m_chartType->findData(key);
    if (index < 0) return false;
    m_chartType->setCurrentIndex(index);
    return true;
}

void QuotesTab::loadChartThen(const QString& rawSymbol, const QString& timeframe, std::function<void(bool)> done)
{
    const QString symbol = rawSymbol.trimmed().toUpper().isEmpty() ? m_chartTicker : rawSymbol.trimmed().toUpper();
    if (symbol.isEmpty()) { done(false); return; }
    bool reload = false;
    if (!timeframe.isEmpty() && timeframe.compare(timeframeLabel(), Qt::CaseInsensitive) != 0) {
        const int index = static_cast<int>(timeframeLabels().indexOf(timeframe.trimmed(), 0, Qt::CaseInsensitive));
        if (index < 0) { done(false); return; }
        m_timeframeIndex = index;
        if (QAbstractButton* button = m_timeframeGroup->button(index)) button->setChecked(true);
        QSettings().setValue(kTimeframeKey, index);
        reload = true;
    }
    if (!reload && symbol == m_chartTicker && !m_bars.bars.empty() && !m_loadingChart) { done(true); return; }
    m_chartLoaded = std::move(done);
    showTicker(symbol);                       // adds to the watchlist and selects the row
    if (symbol == m_chartTicker && (reload || m_bars.bars.empty()) && !m_loadingChart) loadChart(symbol);
}

void QuotesTab::resetChart()
{
    if (m_chartTicker.isEmpty()) return;
    // Re-send the bars the tab holds (so the chart matches the status line), then reset the view.
    pushBars();
    pushDrawings();
    pushLive();
    runJs(QStringLiteral("chartApi.reset();"));
    if (QAbstractButton* cursor = m_drawTools->button(0)) cursor->setChecked(true);
    ui::setStatus(m_chartStatus, QStringLiteral("%1 chart reset: bars reloaded, autoscale and default zoom restored.").arg(m_chartTicker), ui::StatusKind::Info);
}

void QuotesTab::addDrawing(const QJsonObject& spec)
{
    runJs(QStringLiteral("chartApi.addDrawing(%1);").arg(QString::fromUtf8(QJsonDocument(spec).toJson(QJsonDocument::Compact))));
}

void QuotesTab::clearDrawings()
{
    runJs(QStringLiteral("chartApi.clearDrawings();"));
}

QString QuotesTab::drawingsJson() const
{
    const auto it = m_drawings.find(m_chartTicker);
    if (it != m_drawings.end()) return it->second;
    return QSettings().value(kDrawingsPrefix + m_chartTicker, "[]").toString();
}

void QuotesTab::chartImage(std::function<void(const QImage&)> done)
{
    if (!m_pageReady) { done(QImage()); return; }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.screenshot()"), [done](const QVariant& result) {
        const QString dataUrl = result.toString();
        const qsizetype comma = dataUrl.indexOf(',');
        if (!dataUrl.startsWith("data:image/png;base64,") || comma < 0) { done(QImage()); return; }
        QImage image;
        image.loadFromData(QByteArray::fromBase64(dataUrl.mid(comma + 1).toLatin1()), "PNG");
        done(image);
    });
}

QString QuotesTab::contextSummary(int maxBars) const
{
    QString out;
    QTextStream s(&out);
    if (m_chartTicker.isEmpty()) {
        s << "Quotes tab: no chart loaded. Watchlist: " << m_watchlist.join(", ") << "\n";
        return out;
    }
    const Timeframe& tf = timeframes()[static_cast<size_t>(m_timeframeIndex)];
    s << "Quotes tab. Chart: " << m_chartTicker;
    if (m_names.count(m_chartTicker)) s << " (" << m_names.at(m_chartTicker) << ")";
    s << ", timeframe " << tf.label << " (" << tf.multiplier << " " << tf.timespan << " bars), style " << m_chartType->currentData().toString()
      << ". Indicators: " << indicatorsSummary() << ".\n";
    const auto quote = m_quotes.find(m_chartTicker);
    if (quote != m_quotes.end()) {
        s << "Latest quote: last " << ui::number(quote->second.last, 2) << ", change " << ui::number(quote->second.change, 2) << " ("
          << ui::number(quote->second.changePercent, 2) << "%), previous close " << ui::number(quote->second.previousClose, 2) << ".\n";
    }
    s << "Drawings on this chart (JSON; times are epoch seconds of the anchoring bar): " << drawingsJson() << "\n";
    const auto& bars = m_bars.bars;
    if (bars.empty()) {
        s << "No bars loaded yet.\n";
        return out;
    }
    double hi = 0.0, lo = 1e300;
    for (const MarketDataClient::Bar& b : bars) { hi = std::max(hi, b.high); lo = std::min(lo, b.low); }
    s << bars.size() << " bars from " << barLabel(bars.front().timeMs, tf.intraday) << " to "
      << barLabel(bars.back().timeMs, tf.intraday) << "; range low " << ui::number(lo, 2) << " high " << ui::number(hi, 2)
      << "; last close " << ui::number(bars.back().close, 2) << ".\n";
    const size_t count = std::min(bars.size(), static_cast<size_t>(std::max(10, maxBars)));
    s << "Most recent " << count << " bars (time,open,high,low,close,volume):\n";
    for (size_t i = bars.size() - count; i < bars.size(); ++i) {
        const MarketDataClient::Bar& b = bars[i];
        s << barLabel(b.timeMs, tf.intraday) << "," << ui::number(b.open, 2) << "," << ui::number(b.high, 2)
          << "," << ui::number(b.low, 2) << "," << ui::number(b.close, 2) << "," << QString::number(b.volume, 'f', 0) << "\n";
    }
    return out;
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
    if (m_chartTicker == m_state.underlyingTicker && m_state.previousClose > 0.0) meta["previousClose"] = m_state.previousClose;
    else if (quote != m_quotes.end() && quote->second.previousClose > 0.0) meta["previousClose"] = quote->second.previousClose;
    if (!m_bars.bars.empty()) {
        const QDate last = sessionDate(m_bars.bars.back().timeMs);
        meta["asOf"] = tf.intraday ? QStringLiteral("as of %1").arg(barLabel(m_bars.bars.back().timeMs, true))
                                   : QStringLiteral("%1 %2").arg(last == exchangeToday() ? "today's session, " : "session ", barLabel(m_bars.bars.back().timeMs, false));
    }
    QJsonObject payload;
    payload["bars"] = bars;
    payload["meta"] = meta;
    runJs(QStringLiteral("chartApi.setBars(%1);").arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
}

void QuotesTab::pushOptions()
{
    QJsonObject opts;
    opts["type"] = m_chartType->currentData().toString();
    opts["indicators"] = m_indicators;
    opts["volume"] = m_volumeAction->isChecked();
    opts["priceLine"] = m_priceLineCheck->isChecked();
    opts["paneHeight"] = QSettings().value(kPaneHeightKey, 0.24).toDouble();   // indicator pane, fraction of the chart height
    runJs(QStringLiteral("chartApi.setOptions(%1);").arg(QString::fromUtf8(QJsonDocument(opts).toJson(QJsonDocument::Compact))));
}

// ---- Technical indicators -------------------------------------------------------------

QString QuotesTab::indicatorLabel(const QJsonObject& spec)
{
    const QString type = spec.value("type").toString();
    if (type == "macd") {
        return QStringLiteral("MACD %1/%2/%3").arg(spec.value("fast").toInt(12)).arg(spec.value("slow").toInt(26)).arg(spec.value("signal").toInt(9));
    }
    return QStringLiteral("%1 %2").arg(type.toUpper()).arg(spec.value("period").toInt());
}

bool QuotesTab::normalizeIndicator(QJsonObject& spec, QString* error)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    const QString type = spec.value("type").toString().trimmed().toLower();
    QJsonObject out;
    if (type == "sma" || type == "ema") {
        const int period = spec.value("period").toInt(spec.value("length").toInt());
        if (period < 2 || period > 500) return fail(QStringLiteral("%1 period must be between 2 and 500 bars.").arg(type.toUpper()));
        out["type"] = type;
        out["period"] = period;
        const QColor color(spec.value("color").toString());
        if (color.isValid()) out["color"] = color.name();
    } else if (type == "macd") {
        const int fast = spec.value("fast").toInt(12), slow = spec.value("slow").toInt(26), signal = spec.value("signal").toInt(9);
        if (fast < 2 || fast > 200 || slow < 3 || slow > 500 || signal < 1 || signal > 200) return fail("MACD parameters out of range (fast 2-200, slow 3-500, signal 1-200).");
        if (fast >= slow) return fail("MACD fast period must be shorter than the slow period.");
        out["type"] = type;
        out["fast"] = fast;
        out["slow"] = slow;
        out["signal"] = signal;
    } else {
        return fail(QStringLiteral("Unknown indicator type '%1'. Use sma, ema or macd.").arg(type));
    }
    spec = out;
    return true;
}

QString QuotesTab::nextIndicatorColor() const
{
    QSet<QString> used;
    for (const QJsonValue v : m_indicators) used.insert(v.toObject().value("color").toString().toLower());
    for (const QString& color : indicatorPalette()) {
        if (!used.contains(color)) return color;
    }
    return indicatorPalette().at(m_indicators.size() % indicatorPalette().size());
}

bool QuotesTab::setIndicators(const QJsonArray& indicators, QString* error)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    QJsonArray clean;
    int smas = 0, emas = 0, macds = 0;
    QSet<QString> seen;
    for (const QJsonValue v : indicators) {
        QJsonObject spec = v.toObject();
        if (!normalizeIndicator(spec, error)) return false;
        const QString type = spec.value("type").toString();
        if (type == "sma" && ++smas > kMaxMovingAverages) return fail(QStringLiteral("At most %1 SMAs can be shown.").arg(kMaxMovingAverages));
        if (type == "ema" && ++emas > kMaxMovingAverages) return fail(QStringLiteral("At most %1 EMAs can be shown.").arg(kMaxMovingAverages));
        if (type == "macd" && ++macds > 1) return fail("Only one MACD can be shown.");
        const QString key = indicatorLabel(spec);
        if (seen.contains(key)) continue;   // duplicates collapse silently
        seen.insert(key);
        clean.append(spec);
    }
    // Hand out colours to moving averages that have none, avoiding the ones already taken.
    m_indicators = QJsonArray();
    for (const QJsonValue v : clean) {
        QJsonObject spec = v.toObject();
        if (spec.value("type").toString() != "macd" && !spec.contains("color")) spec["color"] = nextIndicatorColor();
        m_indicators.append(spec);
    }
    saveIndicators();
    rebuildIndicatorsMenu();
    pushOptions();
    return true;
}

bool QuotesTab::addIndicator(QJsonObject spec, QString* error)
{
    if (!normalizeIndicator(spec, error)) return false;
    QJsonArray next = m_indicators;
    // Re-adding an existing MACD replaces its parameters; a same-period MA is a no-op.
    for (int i = 0; i < next.size(); ++i) {
        const QJsonObject existing = next.at(i).toObject();
        if (spec.value("type") == "macd" && existing.value("type") == "macd") { next.removeAt(i); break; }
        if (indicatorLabel(existing) == indicatorLabel(spec)) return true;
    }
    next.append(spec);
    return setIndicators(next, error);
}

int QuotesTab::removeIndicators(const QString& rawType, int period)
{
    const QString type = rawType.trimmed().toLower();
    QJsonArray next;
    int removed = 0;
    for (const QJsonValue v : m_indicators) {
        const QJsonObject spec = v.toObject();
        const bool typeMatch = type == "all" || type.isEmpty() || spec.value("type").toString() == type;
        const bool periodMatch = period <= 0 || spec.value("period").toInt() == period;
        if (typeMatch && periodMatch) { ++removed; continue; }
        next.append(spec);
    }
    if (removed) setIndicators(next);
    return removed;
}

bool QuotesTab::volumeShown() const { return m_volumeAction->isChecked(); }
void QuotesTab::setVolumeShown(bool on) { m_volumeAction->setChecked(on); }

QString QuotesTab::indicatorsSummary() const
{
    QStringList parts;
    for (const QJsonValue v : m_indicators) parts << indicatorLabel(v.toObject());
    return (parts.isEmpty() ? QStringLiteral("none") : parts.join(", ")) + (m_volumeAction->isChecked() ? "; volume on" : "; volume off");
}

void QuotesTab::loadIndicators()
{
    const QSettings settings;
    QJsonArray list;
    if (settings.contains(kIndicatorsKey)) {
        list = QJsonDocument::fromJson(settings.value(kIndicatorsKey).toByteArray()).array();
    } else {
        // First run (or upgrade from the fixed SMA/EMA pair): the previous defaults.
        list.append(QJsonObject{ { "type", "sma" }, { "period", 20 } });
        list.append(QJsonObject{ { "type", "ema" }, { "period", 50 } });
    }
    if (!setIndicators(list)) setIndicators(QJsonArray());
}

void QuotesTab::saveIndicators() const
{
    QSettings().setValue(kIndicatorsKey, QJsonDocument(m_indicators).toJson(QJsonDocument::Compact));
}

void QuotesTab::rebuildIndicatorsMenu()
{
    if (!m_indicatorsMenu) return;
    m_indicatorsMenu->clear();
    int smas = 0, emas = 0;
    bool hasMacd = false;
    for (const QJsonValue v : m_indicators) {
        const QString type = v.toObject().value("type").toString();
        if (type == "sma") ++smas;
        else if (type == "ema") ++emas;
        else if (type == "macd") hasMacd = true;
    }
    QAction* addSma = m_indicatorsMenu->addAction("Add SMA…", this, [this] { promptAddIndicator("sma"); });
    addSma->setEnabled(smas < kMaxMovingAverages);
    addSma->setToolTip(QStringLiteral("Simple moving average of the close (up to %1)").arg(kMaxMovingAverages));
    QAction* addEma = m_indicatorsMenu->addAction("Add EMA…", this, [this] { promptAddIndicator("ema"); });
    addEma->setEnabled(emas < kMaxMovingAverages);
    addEma->setToolTip(QStringLiteral("Exponentially weighted moving average of the close (up to %1)").arg(kMaxMovingAverages));
    QAction* addMacd = m_indicatorsMenu->addAction(hasMacd ? "Edit MACD…" : "Add MACD…", this, [this] { promptAddIndicator("macd"); });
    addMacd->setToolTip("Moving average convergence/divergence in its own pane: MACD line, signal line and histogram");
    m_indicatorsMenu->addSeparator();
    m_indicatorsMenu->addAction(m_volumeAction);
    if (!m_indicators.isEmpty()) {
        m_indicatorsMenu->addSeparator();
        for (int i = 0; i < m_indicators.size(); ++i) {
            const QJsonObject spec = m_indicators.at(i).toObject();
            QMenu* sub = m_indicatorsMenu->addMenu(indicatorLabel(spec));
            if (spec.contains("color")) sub->setIcon(swatchIcon(spec.value("color").toString()));
            sub->addAction(spec.value("type").toString() == "macd" ? "Edit parameters…" : "Edit period and colour…", this, [this, i] {
                if (i >= m_indicators.size()) return;
                QJsonObject edited = m_indicators.at(i).toObject();
                if (!editIndicatorDialog(edited, false)) return;
                QJsonArray next = m_indicators;
                next[i] = edited;
                QString error;
                if (!setIndicators(next, &error)) setStatus(error, ui::StatusKind::Error);
            });
            sub->addAction("Remove", this, [this, i] {
                if (i >= m_indicators.size()) return;
                QJsonArray next = m_indicators;
                next.removeAt(i);
                setIndicators(next);
            });
        }
        m_indicatorsMenu->addSeparator();
        m_indicatorsMenu->addAction("Remove all indicators", this, [this] { setIndicators(QJsonArray()); });
    }
    m_indicatorsMenu->addAction("Restore defaults (SMA 20, EMA 50)", this, [this] {
        setIndicators(QJsonArray{ QJsonObject{ { "type", "sma" }, { "period", 20 } }, QJsonObject{ { "type", "ema" }, { "period", 50 } } });
    });
    m_indicatorsButton->setText(m_indicators.isEmpty() ? QStringLiteral("Indicators ▾") : QStringLiteral("Indicators (%1) ▾").arg(m_indicators.size()));
    m_indicatorsButton->setToolTip(QStringLiteral("Indicators: %1\nAdd, edit or remove moving averages and the MACD; toggle the volume histogram").arg(indicatorsSummary()));
}

void QuotesTab::promptAddIndicator(const QString& type)
{
    QJsonObject spec{ { "type", type } };
    if (type == "macd") {
        for (const QJsonValue v : m_indicators) {
            if (v.toObject().value("type").toString() == "macd") { spec = v.toObject(); break; }
        }
        if (!spec.contains("fast")) { spec["fast"] = 12; spec["slow"] = 26; spec["signal"] = 9; }
    } else {
        // Suggest the first common period not already on the chart.
        QSet<int> used;
        for (const QJsonValue v : m_indicators) {
            if (v.toObject().value("type").toString() == type) used.insert(v.toObject().value("period").toInt());
        }
        const std::vector<int> common = type == "sma" ? std::vector<int>{ 20, 50, 100, 200, 10 } : std::vector<int>{ 50, 21, 9, 200, 100 };
        int period = common.front();
        for (int p : common) { if (!used.contains(p)) { period = p; break; } }
        spec["period"] = period;
        spec["color"] = nextIndicatorColor();
    }
    if (!editIndicatorDialog(spec, true)) return;
    QString error;
    if (!addIndicator(spec, &error)) setStatus(error, ui::StatusKind::Error);
}

bool QuotesTab::editIndicatorDialog(QJsonObject& spec, bool adding)
{
    const QString type = spec.value("type").toString();
    QDialog dialog(this);
    dialog.setWindowTitle((adding ? "Add " : "Edit ") + (type == "macd" ? QStringLiteral("MACD") : type.toUpper()));
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(14, 12, 14, 10);
    form->setSpacing(8);
    QSpinBox* period = nullptr;
    QComboBox* color = nullptr;
    QSpinBox* fast = nullptr;
    QSpinBox* slow = nullptr;
    QSpinBox* signal = nullptr;
    if (type == "macd") {
        fast = ui::makeIntSpinBox(&dialog, 2, 200, spec.value("fast").toInt(12));
        slow = ui::makeIntSpinBox(&dialog, 3, 500, spec.value("slow").toInt(26));
        signal = ui::makeIntSpinBox(&dialog, 1, 200, spec.value("signal").toInt(9));
        fast->setToolTip("Fast EMA period (bars)");
        slow->setToolTip("Slow EMA period (bars); must be longer than the fast period");
        signal->setToolTip("EMA period of the signal line (bars)");
        form->addRow("Fast EMA", fast);
        form->addRow("Slow EMA", slow);
        form->addRow("Signal", signal);
    } else {
        period = ui::makeIntSpinBox(&dialog, 2, 500, spec.value("period").toInt(20));
        period->setToolTip("Look-back in bars of the current timeframe");
        form->addRow("Period (bars)", period);
        color = new QComboBox(&dialog);
        for (int i = 0; i < indicatorPalette().size(); ++i) color->addItem(swatchIcon(indicatorPalette().at(i)), indicatorPaletteNames().at(i), indicatorPalette().at(i));
        const int current = static_cast<int>(indicatorPalette().indexOf(spec.value("color").toString().toLower()));
        color->setCurrentIndex(current >= 0 ? current : 0);
        form->addRow("Colour", color);
    }
    auto* note = new QLabel(&dialog);
    note->setObjectName("muted");
    note->setWordWrap(true);
    note->setText(type == "macd" ? "Drawn in its own pane below the price: MACD line, signal line and histogram. The settings become the default for the next launch."
                                 : "Drawn over the price. Up to three SMAs and three EMAs can be shown; the set becomes the default for the next launch.");
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(adding ? "Add" : "Apply");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (fast && slow && fast->value() >= slow->value()) {
            note->setText("The fast period must be shorter than the slow period.");
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    if (type == "macd") {
        spec["fast"] = fast->value();
        spec["slow"] = slow->value();
        spec["signal"] = signal->value();
    } else {
        spec["period"] = period->value();
        spec["color"] = color->currentData().toString();
    }
    return true;
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
    } else if (kind == "options") {
        // The page toggled an option from its menu; mirror it without re-sending.
        const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
        if (obj.contains("priceLine")) {
            const QSignalBlocker blocker(m_priceLineCheck);
            m_priceLineCheck->setChecked(obj.value("priceLine").toBool(true));
            QSettings().setValue(kPriceLineKey, m_priceLineCheck->isChecked());
        }
        // The user dragged the indicator pane's handle (or toggled expand): remember the height.
        if (obj.contains("paneHeight")) QSettings().setValue(kPaneHeightKey, std::clamp(obj.value("paneHeight").toDouble(0.24), 0.1, 0.7));
    } else if (kind == "menu") {
        qInfo("[chart] context menu opened on %s", qPrintable(payload));
    } else if (kind == "tool") {
        // The page returns to the cursor after a drawing is placed or on Esc; mirror it.
        const int id = static_cast<int>(drawToolNames().indexOf(payload));
        if (QAbstractButton* button = id >= 0 ? m_drawTools->button(id) : nullptr) button->setChecked(true);
    }
}

QString QuotesTab::debugRowText(const QString& symbol) const
{
    const int row = rowForTicker(symbol.trimmed().toUpper());
    if (row < 0) return QStringLiteral("(not in watchlist)");
    QStringList parts;
    for (int col : { ColLast, ColChange, ColPercent }) {
        const QTableWidgetItem* item = m_table->item(row, col);
        parts << (item ? item->text() : QStringLiteral("?"));
    }
    return parts.join(' ');
}

void QuotesTab::debugLegendText(std::function<void(const QString&)> done)
{
    if (!m_pageReady) { done(QString()); return; }
    m_view->page()->runJavaScript(QStringLiteral("document.getElementById('legend').innerText"), [done](const QVariant& result) { done(result.toString()); });
}

void QuotesTab::debugTogglePane(std::function<void(int)> done)
{
    if (!m_pageReady) { done(-1); return; }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.togglePaneExpanded()"), [done](const QVariant& result) { done(result.toInt()); });
}

void QuotesTab::debugStashDrawings(std::function<void(int)> done)
{
    if (!m_pageReady) { done(-1); return; }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.stashDrawings()"), [done](const QVariant& result) { done(result.toInt()); });
}

void QuotesTab::debugRestoreDrawings(std::function<void(int)> done)
{
    if (!m_pageReady) { done(-1); return; }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.restoreDrawings()"), [done](const QVariant& result) { done(result.toInt()); });
}

void QuotesTab::debugSimulateContextDelete(std::function<void(int)> done)
{
    if (!m_pageReady) { done(-1); return; }
    m_view->page()->runJavaScript(QStringLiteral("chartApi.simulateContextDelete()"), [done](const QVariant& result) { done(result.toInt()); });
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
            s << m_chartTicker << "," << tf.label << "," << (tf.intraday ? QDateTime::fromMSecsSinceEpoch(b.timeMs).toString(Qt::ISODate) : barLabel(b.timeMs, false)) << ","
              << b.open << "," << b.high << "," << b.low << "," << b.close << "," << b.volume << "\n";
        }
    }
    return out;
}
