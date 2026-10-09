//
//  QuotesTab.cpp
//  OptionPricing
//

#include "QuotesTab.h"
#include "ChartPage.h"
#include "Formatting.h"
#include "../Pricing/TechnicalAnalysis.h"

#include <QtCore/QSet>
#include <QtWidgets/QTreeWidget>

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
        if (onQuotesRefreshed) onQuotesRefreshed(quotes);
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
    pushIndicators();
}

void QuotesTab::pushOptions()
{
    QJsonObject opts;
    opts["type"] = m_chartType->currentData().toString();
    opts["volume"] = m_volumeAction->isChecked();
    opts["priceLine"] = m_priceLineCheck->isChecked();
    opts["paneHeight"] = QSettings().value(kPaneHeightKey, 0.24).toDouble();   // indicator pane, fraction of the chart height
    runJs(QStringLiteral("chartApi.setOptions(%1);").arg(QString::fromUtf8(QJsonDocument(opts).toJson(QJsonDocument::Compact))));
}

// ---- Technical indicators (TA-Lib) ----------------------------------------------------

namespace {

/// Parameter aliases accepted in specs and commands, mapped to TA-Lib's optional-input names.
QString canonicalParamName(const ta::FunctionInfo& info, const QString& rawKey)
{
    QString key = rawKey.toLower();
    key.remove(QRegularExpression("[\\s_\\-]"));
    static const QHash<QString, QString> aliases{
        { "period", "optInTimePeriod" }, { "timeperiod", "optInTimePeriod" }, { "length", "optInTimePeriod" }, { "window", "optInTimePeriod" },
        { "fast", "optInFastPeriod" }, { "fastperiod", "optInFastPeriod" }, { "slow", "optInSlowPeriod" }, { "slowperiod", "optInSlowPeriod" },
        { "signal", "optInSignalPeriod" }, { "signalperiod", "optInSignalPeriod" }, { "nbdevup", "optInNbDevUp" }, { "up", "optInNbDevUp" }, { "devup", "optInNbDevUp" },
        { "nbdevdn", "optInNbDevDn" }, { "down", "optInNbDevDn" }, { "devdn", "optInNbDevDn" }, { "matype", "optInMAType" }, { "type", "optInMAType" },
        { "acceleration", "optInAcceleration" }, { "maximum", "optInMaximum" }, { "nbdev", "optInNbDev" }, { "deviations", "optInNbDev" },
        { "fastk", "optInFastK_Period" }, { "fastkperiod", "optInFastK_Period" }, { "slowk", "optInSlowK_Period" }, { "slowkperiod", "optInSlowK_Period" },
        { "slowd", "optInSlowD_Period" }, { "slowdperiod", "optInSlowD_Period" }, { "fastd", "optInFastD_Period" }, { "fastdperiod", "optInFastD_Period" },
    };
    for (const ta::ParamInfo& p : info.params) {
        QString name = QString::fromStdString(p.name);
        QString bare = name.startsWith("optIn") ? name.mid(5) : name;
        QString display = QString::fromStdString(p.displayName);
        for (QString candidate : { name, bare, display }) {
            candidate = candidate.toLower().remove(QRegularExpression("[\\s_\\-]"));
            if (candidate == key) return name;
        }
    }
    const QString alias = aliases.value(key);
    if (!alias.isEmpty()) {
        for (const ta::ParamInfo& p : info.params) if (QString::fromStdString(p.name) == alias) return alias;
    }
    return QString();
}

ta::Params paramsFromJson(const ta::FunctionInfo& info, const QJsonObject& object)
{
    ta::Params params;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const QString name = canonicalParamName(info, it.key());
        if (name.isEmpty() || !it.value().isDouble()) continue;
        params[name.toStdString()] = it.value().toDouble();
    }
    return params;
}

QJsonObject paramsToJson(const ta::Params& params)
{
    QJsonObject out;
    for (const auto& [name, value] : params) out[QString::fromStdString(name)] = value;
    return out;
}

/// "RealUpperBand" -> "Upper", "MACDSignal" -> "Signal", "SlowK" -> "SlowK", "Real" -> the function name.
QString shortOutputName(const ta::FunctionInfo& info, const ta::OutputInfo& out)
{
    QString name = QString::fromStdString(out.shortName);
    if (name == QString::fromStdString(info.name)) return name;
    name.remove(QRegularExpression("^Real"));
    name.remove("Band");
    if (name.startsWith(QString::fromStdString(info.name)) && name.size() > static_cast<int>(info.name.size())) name = name.mid(static_cast<int>(info.name.size()));
    return name.isEmpty() ? QString::fromStdString(out.shortName) : name;
}

const char* styleName(ta::OutputInfo::Style style)
{
    switch (style) {
    case ta::OutputInfo::Style::DashLine: return "dash";
    case ta::OutputInfo::Style::DotLine: return "dot";
    case ta::OutputInfo::Style::Dots: return "dots";
    case ta::OutputInfo::Style::Histogram: return "hist";
    case ta::OutputInfo::Style::Pattern: return "pattern";
    case ta::OutputInfo::Style::Line: break;
    }
    return "line";
}

/// Reference lines for bounded oscillators, drawn dashed in the indicator's pane.
QJsonArray referenceLevels(const QString& func)
{
    static const QHash<QString, QList<double>> levels{
        { "RSI", { 30, 70 } }, { "STOCH", { 20, 80 } }, { "STOCHF", { 20, 80 } }, { "STOCHRSI", { 20, 80 } }, { "MFI", { 20, 80 } },
        { "ULTOSC", { 30, 70 } }, { "WILLR", { -20, -80 } }, { "CCI", { 100, -100 } }, { "CMO", { 50, -50 } }, { "ADX", { 25 } }, { "ADXR", { 25 } }, { "DX", { 25 } },
    };
    QJsonArray out;
    for (double v : levels.value(func)) out.append(v);
    return out;
}

/// Spoken-name aliases for the most requested indicators (anything else resolves through the catalogue search).
QString aliasFunction(const QString& loweredName)
{
    static const QHash<QString, QString> aliases{
        { "moving average", "SMA" }, { "simple moving average", "SMA" }, { "sma", "SMA" }, { "exponential moving average", "EMA" }, { "ema", "EMA" }, { "ewma", "EMA" },
        { "weighted moving average", "WMA" }, { "hull", "HMA" }, { "bollinger", "BBANDS" }, { "bollinger bands", "BBANDS" }, { "bands", "BBANDS" },
        { "macd", "MACD" }, { "rsi", "RSI" }, { "relative strength", "RSI" }, { "stochastic", "STOCH" }, { "stochastics", "STOCH" }, { "stochastic rsi", "STOCHRSI" },
        { "atr", "ATR" }, { "average true range", "ATR" }, { "adx", "ADX" }, { "obv", "OBV" }, { "on balance volume", "OBV" }, { "parabolic sar", "SAR" }, { "sar", "SAR" },
        { "cci", "CCI" }, { "momentum", "MOM" }, { "roc", "ROC" }, { "rate of change", "ROC" }, { "williams", "WILLR" }, { "williams r", "WILLR" }, { "mfi", "MFI" },
        { "money flow", "MFI" }, { "aroon", "AROON" }, { "ichimoku", "" }, { "engulfing", "CDLENGULFING" }, { "doji", "CDLDOJI" }, { "hammer", "CDLHAMMER" },
        { "shooting star", "CDLSHOOTINGSTAR" }, { "morning star", "CDLMORNINGSTAR" }, { "evening star", "CDLEVENINGSTAR" }, { "three black crows", "CDL3BLACKCROWS" },
        { "three white soldiers", "CDL3WHITESOLDIERS" }, { "harami", "CDLHARAMI" }, { "hanging man", "CDLHANGINGMAN" }, { "typical price", "TYPPRICE" },
        { "standard deviation", "STDDEV" }, { "linear regression", "LINEARREG" }, { "trix", "TRIX" }, { "ppo", "PPO" }, { "apo", "APO" }, { "kama", "KAMA" }, { "tema", "TEMA" }, { "dema", "DEMA" },
    };
    return aliases.value(loweredName);
}

} // namespace

QString QuotesTab::resolveIndicatorName(const QString& text)
{
    QString name = text.trimmed().toLower();
    name.remove(QRegularExpression("\\s+(indicator|oscillator|study|line|lines)$"));
    name = name.simplified();
    if (name.isEmpty()) return QString();
    const ta::Catalog& catalog = ta::catalog();
    QString compact = name.toUpper();
    compact.remove(QRegularExpression("[^A-Z0-9]"));
    if (const ta::FunctionInfo* info = catalog.find(compact.toStdString())) return QString::fromStdString(info->name);
    const QString alias = aliasFunction(name);
    if (!alias.isEmpty()) return alias;
    // Exact description match first ("Relative Strength Index"), then a unique substring match.
    for (const auto& [key, info] : catalog.functions) {
        if (QString::fromStdString(info.hint).compare(name, Qt::CaseInsensitive) == 0) return QString::fromStdString(info.name);
    }
    const auto matches = catalog.search(name.toStdString());
    if (matches.size() == 1) return QString::fromStdString(matches.front()->name);
    return QString();
}

QString QuotesTab::indicatorCatalogText()
{
    const ta::Catalog& catalog = ta::catalog();
    QString out;
    QTextStream s(&out);
    for (const std::string& group : catalog.groups) {
        s << QString::fromStdString(group) << ":\n";
        auto functions = catalog.inGroup(group);
        std::sort(functions.begin(), functions.end(), [](const ta::FunctionInfo* a, const ta::FunctionInfo* b) { return a->name < b->name; });
        for (const ta::FunctionInfo* info : functions) {
            s << "  " << QString::fromStdString(info->name) << " – " << QString::fromStdString(info->hint);
            QStringList params;
            for (const ta::ParamInfo& p : info->params) {
                if (p.advanced) continue;
                QString name = QString::fromStdString(p.name);
                if (name.startsWith("optIn")) name = name.mid(5);
                params << QStringLiteral("%1=%2").arg(name).arg(p.kind == ta::ParamInfo::Kind::Real ? QString::number(p.defaultValue) : QString::number(static_cast<int>(p.defaultValue)));
            }
            if (!params.isEmpty()) s << " (" << params.join(", ") << ")";
            s << (info->overlay ? " [overlay]" : (info->candlestick ? " [pattern]" : " [pane]")) << "\n";
        }
    }
    return out;
}

QString QuotesTab::indicatorLabel(const QJsonObject& rawSpec)
{
    QJsonObject spec = rawSpec;
    if (!normalizeIndicator(spec, nullptr)) return rawSpec.value("func").toString(rawSpec.value("type").toString());
    const ta::FunctionInfo* info = ta::catalog().find(spec.value("func").toString().toStdString());
    if (!info) return spec.value("func").toString();
    return QString::fromStdString(ta::label(*info, paramsFromJson(*info, spec.value("params").toObject())));
}

bool QuotesTab::normalizeIndicator(QJsonObject& spec, QString* error)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    QJsonObject in = spec;
    // Older specs: {"type":"sma","period":20,"color":c} / {"type":"macd","fast":..,"slow":..,"signal":..}
    if (!in.contains("func") && in.contains("type")) {
        const QString type = in.value("type").toString().trimmed().toLower();
        QJsonObject params = in.value("params").toObject();
        if (type == "sma" || type == "ema") {
            in["func"] = type.toUpper();
            if (in.contains("period")) params["optInTimePeriod"] = in.value("period");
        } else if (type == "macd") {
            in["func"] = "MACD";
            if (in.contains("fast")) params["optInFastPeriod"] = in.value("fast");
            if (in.contains("slow")) params["optInSlowPeriod"] = in.value("slow");
            if (in.contains("signal")) params["optInSignalPeriod"] = in.value("signal");
        } else {
            in["func"] = in.value("type").toString();
        }
        in["params"] = params;
        if (in.contains("color") && !in.contains("colors")) in["colors"] = QJsonArray{ in.value("color") };
    }
    // Loose top-level parameters (assistant tools): period / fast / slow / signal …
    QJsonObject params = in.value("params").toObject();
    for (const char* key : { "period", "fast", "slow", "signal", "nbdevup", "nbdevdn", "matype", "timeperiod" }) {
        if (in.contains(key) && !params.contains(key)) params[key] = in.value(key);
    }
    const QString funcName = in.value("func").toString().trimmed();
    if (funcName.isEmpty()) return fail("An indicator needs a TA-Lib function name (e.g. SMA, RSI, BBANDS, MACD).");
    const ta::FunctionInfo* info = ta::catalog().find(funcName.toStdString());
    if (!info) {
        const QString resolved = resolveIndicatorName(funcName);
        if (resolved.isEmpty()) return fail(QStringLiteral("Unknown indicator '%1'. Use a TA-Lib function name such as SMA, EMA, BBANDS, RSI, MACD, ATR, OBV or a candlestick pattern like CDLENGULFING.").arg(funcName));
        info = ta::catalog().find(resolved.toStdString());
    }
    const ta::Params normalized = ta::normalizedParams(*info, paramsFromJson(*info, params));
    // MACD-style sanity: a fast period at or above the slow one is almost certainly a mistake.
    if (normalized.count("optInFastPeriod") && normalized.count("optInSlowPeriod") && normalized.at("optInFastPeriod") >= normalized.at("optInSlowPeriod")) {
        return fail(QStringLiteral("%1: the fast period must be shorter than the slow period.").arg(QString::fromStdString(info->name)));
    }
    QJsonArray colors;
    for (const QJsonValue v : in.value("colors").toArray()) {
        const QColor c(v.toString());
        colors.append(c.isValid() ? c.name() : QString());
    }
    while (colors.size() > static_cast<int>(info->outputs.size())) colors.removeLast();
    QJsonObject out;
    out["func"] = QString::fromStdString(info->name);
    out["params"] = paramsToJson(normalized);
    if (!colors.isEmpty()) out["colors"] = colors;
    spec = out;
    return true;
}

QStringList QuotesTab::unusedIndicatorColors(int count) const
{
    QSet<QString> used;
    for (const QJsonValue v : m_indicators) {
        for (const QJsonValue c : v.toObject().value("colors").toArray()) used.insert(c.toString().toLower());
    }
    QStringList out;
    for (const QString& color : indicatorPalette()) {
        if (!used.contains(color)) out << color;
        if (out.size() >= count) return out;
    }
    int i = 0;
    while (out.size() < count) out << indicatorPalette().at((m_indicators.size() + i++) % indicatorPalette().size());
    return out;
}

bool QuotesTab::setIndicators(const QJsonArray& indicators, QString* error)
{
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    QJsonArray clean;
    QHash<QString, int> perFunction;
    int panes = 0;
    QSet<QString> seen;
    for (const QJsonValue v : indicators) {
        QJsonObject spec = v.toObject();
        if (!normalizeIndicator(spec, error)) return false;
        const QString func = spec.value("func").toString();
        const ta::FunctionInfo* info = ta::catalog().find(func.toStdString());
        const QString key = indicatorLabel(spec);
        if (seen.contains(key)) continue;   // duplicates collapse silently
        seen.insert(key);
        if (++perFunction[func] > kMaxPerFunction) return fail(QStringLiteral("At most %1 copies of %2 can be shown.").arg(kMaxPerFunction).arg(func));
        if (info && !info->overlay && !info->candlestick && ++panes > kMaxPanes) return fail(QStringLiteral("At most %1 indicators can have their own pane; remove one first.").arg(kMaxPanes));
        if (clean.size() >= kMaxIndicators) return fail(QStringLiteral("At most %1 indicators can be shown.").arg(kMaxIndicators));
        clean.append(spec);
    }
    // Colours: keep what was chosen, fill the rest from the palette avoiding colours in use.
    m_indicators = QJsonArray();
    for (const QJsonValue v : clean) {
        QJsonObject spec = v.toObject();
        const ta::FunctionInfo* info = ta::catalog().find(spec.value("func").toString().toStdString());
        const int outputs = info ? static_cast<int>(info->outputs.size()) : 1;
        QJsonArray colors = spec.value("colors").toArray();
        int missing = 0;
        for (int k = 0; k < outputs; ++k) if (k >= colors.size() || colors.at(k).toString().isEmpty()) ++missing;
        if (missing) {
            m_indicators.append(spec);   // so unusedIndicatorColors() sees the colours already fixed
            const QStringList fresh = unusedIndicatorColors(missing);
            m_indicators.removeLast();
            int next = 0;
            QJsonArray filled;
            for (int k = 0; k < outputs; ++k) {
                const QString c = k < colors.size() ? colors.at(k).toString() : QString();
                filled.append(c.isEmpty() ? fresh.value(next++) : c);
            }
            spec["colors"] = filled;
        }
        m_indicators.append(spec);
    }
    saveIndicators();
    rebuildIndicatorsMenu();
    pushIndicators();
    return true;
}

bool QuotesTab::addIndicator(QJsonObject spec, QString* error)
{
    if (!normalizeIndicator(spec, error)) return false;
    QJsonArray next = m_indicators;
    const QString label = indicatorLabel(spec);
    const QString func = spec.value("func").toString();
    const ta::FunctionInfo* info = ta::catalog().find(func.toStdString());
    for (int i = 0; i < next.size(); ++i) {
        const QJsonObject existing = next.at(i).toObject();
        if (indicatorLabel(existing) == label) return true;   // already there
        // A single-copy pane indicator (MACD, RSI …) re-added with new parameters replaces the old one.
        if (info && !info->overlay && !info->candlestick && existing.value("func").toString() == func) { next.removeAt(i); break; }
    }
    next.append(spec);
    return setIndicators(next, error);
}

int QuotesTab::removeIndicators(const QString& rawFunc, int period)
{
    QString func = rawFunc.trimmed().toUpper();
    if (func != "ALL" && !func.isEmpty() && !ta::catalog().find(func.toStdString())) func = resolveIndicatorName(rawFunc).toUpper();
    QJsonArray next;
    int removed = 0;
    for (const QJsonValue v : m_indicators) {
        const QJsonObject spec = v.toObject();
        const bool funcMatch = func == "ALL" || func.isEmpty() || spec.value("func").toString() == func;
        const bool periodMatch = period <= 0 || qRound(spec.value("params").toObject().value("optInTimePeriod").toDouble()) == period;
        if (funcMatch && periodMatch) { ++removed; continue; }
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
        list.append(QJsonObject{ { "func", "SMA" }, { "params", QJsonObject{ { "optInTimePeriod", 20 } } } });
        list.append(QJsonObject{ { "func", "EMA" }, { "params", QJsonObject{ { "optInTimePeriod", 50 } } } });
    }
    if (!setIndicators(list)) setIndicators(QJsonArray());
}

void QuotesTab::saveIndicators() const
{
    QSettings().setValue(kIndicatorsKey, QJsonDocument(m_indicators).toJson(QJsonDocument::Compact));
}

void QuotesTab::pushIndicators()
{
    QJsonArray payload;
    if (!m_bars.bars.empty()) {
        ta::Bars bars;
        bars.open.reserve(m_bars.bars.size());
        for (const MarketDataClient::Bar& b : m_bars.bars) {
            bars.open.push_back(b.open);
            bars.high.push_back(b.high);
            bars.low.push_back(b.low);
            bars.close.push_back(b.close);
            bars.volume.push_back(b.volume);
        }
        for (int i = 0; i < m_indicators.size(); ++i) {
            const QJsonObject spec = m_indicators.at(i).toObject();
            const QString func = spec.value("func").toString();
            const ta::FunctionInfo* info = ta::catalog().find(func.toStdString());
            if (!info) continue;
            const ta::Params params = paramsFromJson(*info, spec.value("params").toObject());
            const ta::Result result = ta::compute(func.toStdString(), bars, params);
            if (!result.ok) {
                qWarning("[indicators] %s: %s", qPrintable(func), result.error.c_str());
                continue;
            }
            const QJsonArray colors = spec.value("colors").toArray();
            double maxAbs = 0.0;
            QJsonArray outputs;
            for (size_t k = 0; k < result.outputs.size(); ++k) {
                const ta::Series& series = result.outputs[k];
                QJsonArray values;
                for (double v : series.values) {
                    if (std::isnan(v)) { values.append(QJsonValue::Null); continue; }
                    maxAbs = std::max(maxAbs, std::fabs(v));
                    values.append(std::round(v * 1e5) / 1e5);
                }
                QJsonObject out;
                out["name"] = QString::fromStdString(series.info.shortName);
                out["short"] = shortOutputName(*info, series.info);
                out["style"] = styleName(series.info.style);
                out["color"] = k < static_cast<size_t>(colors.size()) ? colors.at(static_cast<int>(k)).toString() : indicatorPalette().at(static_cast<int>(k) % indicatorPalette().size());
                out["zero"] = series.info.hasZero;
                out["negative"] = series.info.canBeNegative;
                out["upper"] = series.info.upperLimit;
                out["lower"] = series.info.lowerLimit;
                out["values"] = values;
                outputs.append(out);
            }
            QJsonObject item;
            item["id"] = i;
            item["func"] = func;
            item["label"] = QString::fromStdString(ta::label(*info, params));
            QString shortLabel = QString::fromStdString(info->hint);
            if (shortLabel.size() > 16) shortLabel = shortLabel.left(15).trimmed() + "…";
            item["shortLabel"] = shortLabel;
            item["placement"] = info->candlestick ? "markers" : (info->overlay ? "overlay" : "pane");
            item["precision"] = info->overlay ? 2 : (maxAbs >= 1000.0 ? 0 : (maxAbs >= 100.0 ? 1 : (maxAbs >= 10.0 ? 2 : 3)));
            item["levels"] = referenceLevels(func);
            item["outputs"] = outputs;
            payload.append(item);
        }
    }
    runJs(QStringLiteral("chartApi.setIndicators(%1);").arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))));
}

void QuotesTab::rebuildIndicatorsMenu()
{
    if (!m_indicatorsMenu) return;
    m_indicatorsMenu->clear();
    m_indicatorsMenu->addAction("Browse indicators…", this, [this] { promptBrowseIndicators(); });
    m_indicatorsMenu->addSeparator();
    // One submenu per TA-Lib category.
    const ta::Catalog& catalog = ta::catalog();
    for (const std::string& group : catalog.groups) {
        QMenu* sub = m_indicatorsMenu->addMenu(QString::fromStdString(group));
        auto functions = catalog.inGroup(group);
        std::sort(functions.begin(), functions.end(), [](const ta::FunctionInfo* a, const ta::FunctionInfo* b) { return a->name < b->name; });
        for (const ta::FunctionInfo* info : functions) {
            const QString name = QString::fromStdString(info->name);
            QAction* action = sub->addAction(QString::fromStdString(info->displayName()), this, [this, name] { promptAddIndicator(name); });
            action->setToolTip(info->overlay ? "Drawn over the price" : (info->candlestick ? "Marks bars where the pattern occurs" : "Drawn in its own pane below the price"));
        }
    }
    m_indicatorsMenu->addSeparator();
    m_indicatorsMenu->addAction(m_volumeAction);
    if (!m_indicators.isEmpty()) {
        m_indicatorsMenu->addSeparator();
        for (int i = 0; i < m_indicators.size(); ++i) {
            const QJsonObject spec = m_indicators.at(i).toObject();
            QMenu* sub = m_indicatorsMenu->addMenu(indicatorLabel(spec));
            const QJsonArray colors = spec.value("colors").toArray();
            if (!colors.isEmpty()) sub->setIcon(swatchIcon(colors.first().toString()));
            sub->addAction("Edit parameters and colours…", this, [this, i] {
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
        setIndicators(QJsonArray{ QJsonObject{ { "func", "SMA" }, { "params", QJsonObject{ { "optInTimePeriod", 20 } } } },
                                  QJsonObject{ { "func", "EMA" }, { "params", QJsonObject{ { "optInTimePeriod", 50 } } } } });
    });
    m_indicatorsButton->setText(QStringLiteral("Indicators ▾"));   // the active set is listed in the menu and the tooltip
    m_indicatorsButton->setToolTip(QStringLiteral("%1 active: %2\nTA-Lib catalogue by category; browse, add, edit or remove indicators; toggle the volume histogram")
                                       .arg(m_indicators.size()).arg(indicatorsSummary()));
}

void QuotesTab::promptAddIndicator(const QString& func)
{
    const ta::FunctionInfo* info = ta::catalog().find(func.toStdString());
    if (!info) { setStatus(QStringLiteral("Unknown indicator %1.").arg(func), ui::StatusKind::Error); return; }
    QJsonObject spec{ { "func", QString::fromStdString(info->name) }, { "params", paramsToJson(ta::normalizedParams(*info, {})) } };
    // Suggest a period not already on the chart for moving averages (20, 50, 100, 200 …).
    if (info->overlay && info->params.size() == 1 && info->params[0].name == "optInTimePeriod") {
        QSet<int> used;
        for (const QJsonValue v : m_indicators) {
            const QJsonObject other = v.toObject();
            if (other.value("func").toString() == spec.value("func").toString()) used.insert(qRound(other.value("params").toObject().value("optInTimePeriod").toDouble()));
        }
        for (int p : { 20, 50, 100, 200, 10 }) { if (!used.contains(p)) { QJsonObject params = spec.value("params").toObject(); params["optInTimePeriod"] = p; spec["params"] = params; break; } }
    }
    QJsonArray colors;
    for (const QString& c : unusedIndicatorColors(static_cast<int>(info->outputs.size()))) colors.append(c);
    spec["colors"] = colors;
    if (!editIndicatorDialog(spec, true)) return;
    QString error;
    if (!addIndicator(spec, &error)) setStatus(error, ui::StatusKind::Error);
    else setStatus(QStringLiteral("Added %1.").arg(indicatorLabel(spec)), ui::StatusKind::Info);
}

bool QuotesTab::editIndicatorDialog(QJsonObject& spec, bool adding)
{
    const ta::FunctionInfo* info = ta::catalog().find(spec.value("func").toString().toStdString());
    if (!info) return false;
    const ta::Params current = ta::normalizedParams(*info, paramsFromJson(*info, spec.value("params").toObject()));
    QDialog dialog(this);
    dialog.setWindowTitle((adding ? "Add " : "Edit ") + QString::fromStdString(info->displayName()));
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(14, 12, 14, 10);
    form->setSpacing(8);
    auto* header = new QLabel(QStringLiteral("<b>%1</b> · %2 · %3").arg(QString::fromStdString(info->name), QString::fromStdString(info->group),
                                                                      info->overlay ? "drawn over the price" : (info->candlestick ? "marks bars where the pattern occurs" : "drawn in its own pane")), &dialog);
    header->setWordWrap(true);
    form->addRow(header);
    struct Editor { const ta::ParamInfo* param; QSpinBox* integer = nullptr; QDoubleSpinBox* real = nullptr; QComboBox* choice = nullptr; };
    std::vector<Editor> editors;
    for (const ta::ParamInfo& p : info->params) {
        Editor e;
        e.param = &p;
        const double value = current.at(p.name);
        QString label = QString::fromStdString(p.displayName);
        if (p.advanced) label += " (advanced)";
        QWidget* widget = nullptr;
        if (p.kind == ta::ParamInfo::Kind::Integer) {
            e.integer = ui::makeIntSpinBox(&dialog, static_cast<int>(p.min), static_cast<int>(std::min(p.max, 100000.0)), static_cast<int>(std::lround(value)));
            widget = e.integer;
        } else if (p.kind == ta::ParamInfo::Kind::Real) {
            e.real = new QDoubleSpinBox(&dialog);
            e.real->setDecimals(std::max(1, std::min(p.precision, 4)));
            e.real->setRange(p.min, std::min(p.max, 1e9));
            e.real->setSingleStep(p.precision >= 2 ? 0.1 : 1.0);
            e.real->setValue(value);
            widget = e.real;
        } else {
            e.choice = new QComboBox(&dialog);
            for (const auto& [v, name] : p.choices) e.choice->addItem(QString::fromStdString(name), v);
            const int idx = e.choice->findData(static_cast<int>(value));
            e.choice->setCurrentIndex(std::max(0, idx));
            widget = e.choice;
        }
        if (!p.hint.empty()) widget->setToolTip(QString::fromStdString(p.hint));
        form->addRow(label, widget);
        editors.push_back(e);
    }
    std::vector<QComboBox*> colorBoxes;
    const QJsonArray colors = spec.value("colors").toArray();
    if (!info->candlestick) {
        for (size_t k = 0; k < info->outputs.size(); ++k) {
            auto* box = new QComboBox(&dialog);
            for (int i = 0; i < indicatorPalette().size(); ++i) box->addItem(swatchIcon(indicatorPalette().at(i)), indicatorPaletteNames().at(i), indicatorPalette().at(i));
            const int idx = static_cast<int>(indicatorPalette().indexOf(colors.at(static_cast<int>(k)).toString().toLower()));
            box->setCurrentIndex(idx >= 0 ? idx : static_cast<int>(k % static_cast<size_t>(indicatorPalette().size())));
            form->addRow(info->outputs.size() > 1 ? QStringLiteral("Colour · %1").arg(shortOutputName(*info, info->outputs[k])) : QStringLiteral("Colour"), box);
            colorBoxes.push_back(box);
        }
    }
    auto* note = new QLabel(&dialog);
    note->setObjectName("muted");
    note->setWordWrap(true);
    note->setText(QStringLiteral("%1. Computed with TA-Lib on the chart's bars; the settings become the default for the next launch.").arg(QString::fromStdString(info->hint)));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(adding ? "Add" : "Apply");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    QJsonObject params;
    for (const Editor& e : editors) {
        const QString name = QString::fromStdString(e.param->name);
        if (e.integer) params[name] = e.integer->value();
        else if (e.real) params[name] = e.real->value();
        else if (e.choice) params[name] = e.choice->currentData().toInt();
    }
    spec["params"] = params;
    QJsonArray chosen;
    for (QComboBox* box : colorBoxes) chosen.append(box->currentData().toString());
    if (!chosen.isEmpty()) spec["colors"] = chosen;
    return true;
}

void QuotesTab::promptBrowseIndicators()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Technical indicators (TA-Lib)");
    dialog.resize(640, 520);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 12, 14, 10);
    layout->setSpacing(8);
    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText("Search by name or description, e.g. rsi, bollinger, engulfing…");
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto* tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels({ "Indicator", "Parameters" });
    tree->setRootIsDecorated(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(true);
    tree->setColumnWidth(0, 360);
    layout->addWidget(tree, 1);
    auto* description = new QLabel(&dialog);
    description->setObjectName("muted");
    description->setWordWrap(true);
    description->setMinimumHeight(36);
    layout->addWidget(description);
    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* addButton = buttons->addButton("Add…", QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    addButton->setEnabled(false);

    const ta::Catalog& catalog = ta::catalog();
    auto populate = [tree, &catalog](const QString& needle) {
        tree->clear();
        const QString wanted = needle.trimmed();
        for (const std::string& group : catalog.groups) {
            auto functions = catalog.inGroup(group);
            std::sort(functions.begin(), functions.end(), [](const ta::FunctionInfo* a, const ta::FunctionInfo* b) { return a->name < b->name; });
            auto* groupItem = new QTreeWidgetItem(QStringList{ QString::fromStdString(group) });
            groupItem->setFlags(Qt::ItemIsEnabled);
            QFont bold = groupItem->font(0);
            bold.setBold(true);
            groupItem->setFont(0, bold);
            for (const ta::FunctionInfo* info : functions) {
                const QString name = QString::fromStdString(info->name), hint = QString::fromStdString(info->hint);
                if (!wanted.isEmpty() && !name.contains(wanted, Qt::CaseInsensitive) && !hint.contains(wanted, Qt::CaseInsensitive)) continue;
                QStringList params;
                for (const ta::ParamInfo& p : info->params) {
                    if (p.advanced) continue;
                    params << QStringLiteral("%1 %2").arg(QString::fromStdString(p.displayName)).arg(p.kind == ta::ParamInfo::Kind::Real ? QString::number(p.defaultValue) : QString::number(static_cast<int>(p.defaultValue)));
                }
                auto* item = new QTreeWidgetItem(groupItem, QStringList{ QString::fromStdString(info->displayName()), params.join(", ") });
                item->setData(0, Qt::UserRole, name);
                item->setToolTip(0, info->overlay ? "Drawn over the price" : (info->candlestick ? "Marks bars where the pattern occurs" : "Drawn in its own pane below the price"));
            }
            if (groupItem->childCount()) { tree->addTopLevelItem(groupItem); groupItem->setExpanded(!wanted.isEmpty() || group == "Overlap Studies" || group == "Momentum Indicators"); }
            else delete groupItem;
        }
    };
    populate(QString());
    auto selectedFunction = [tree]() -> QString {
        QTreeWidgetItem* item = tree->currentItem();
        return item ? item->data(0, Qt::UserRole).toString() : QString();
    };
    connect(filter, &QLineEdit::textChanged, &dialog, [populate](const QString& text) { populate(text); });
    connect(tree, &QTreeWidget::currentItemChanged, &dialog, [&, selectedFunction](QTreeWidgetItem*, QTreeWidgetItem*) {
        const QString func = selectedFunction();
        addButton->setEnabled(!func.isEmpty());
        const ta::FunctionInfo* info = func.isEmpty() ? nullptr : catalog.find(func.toStdString());
        description->setText(info ? QStringLiteral("%1 · %2 · %3 output%4 · %5").arg(QString::fromStdString(info->hint), QString::fromStdString(info->group))
                                                                              .arg(info->outputs.size()).arg(info->outputs.size() == 1 ? "" : "s")
                                                                              .arg(info->overlay ? "drawn over the price" : (info->candlestick ? "bar markers (bullish below, bearish above)" : "own pane"))
                                  : QString());
    });
    auto addSelected = [&, selectedFunction] {
        const QString func = selectedFunction();
        if (!func.isEmpty()) promptAddIndicator(func);
    };
    connect(addButton, &QPushButton::clicked, &dialog, addSelected);
    connect(tree, &QTreeWidget::itemActivated, &dialog, [addSelected](QTreeWidgetItem*, int) { addSelected(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
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
    } else if (kind == "log") {
        qWarning("[chart] %s", qPrintable(payload));
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
