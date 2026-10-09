//
//  VolatilityTab.cpp
//  OptionPricing
//

#include "VolatilityTab.h"
#include "Formatting.h"

#include <cmath>
#include <map>
#include <random>

using namespace pricing;

namespace {
constexpr int kPeriodsPerYear = 252;
constexpr double kEwmaLambda = 0.94;
constexpr int kForecastCurveDays = 252;
enum ConeColumn { ConeWindow = 0, ConeCurrent, ConePercentile, ConeMin, ConeMedian, ConeMax, ConeColumnCount };

QString pct(double vol, int decimals = 1)
{
    return std::isfinite(vol) ? ui::percent(vol, decimals) : QStringLiteral("–");
}

QString pts(double diff)
{
    if (!std::isfinite(diff)) return QStringLiteral("–");
    return QStringLiteral("%1%2 pts").arg(diff >= 0 ? "+" : "−", ui::number(std::fabs(diff) * 100.0, 1));
}

qint64 msecsForDate(const std::string& iso)
{
    const QDate d = QDate::fromString(QString::fromStdString(iso), Qt::ISODate);
    return d.isValid() ? QDateTime(d, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch() : 0;
}
} // namespace

const std::vector<int>& VolatilityTab::coneWindows()
{
    static const std::vector<int> windows = { 10, 20, 30, 60, 90, 120, 252 };
    return windows;
}

VolatilityTab::VolatilityTab(MarketState& state, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
{
    buildUi();
    wire();
    recompute();
    // Follow the Option Chain tab's ticker (the tabs are linked both ways: a chain fetched
    // elsewhere switches this tab, and a fetch here requests that chain); refresh the
    // implied overlays whenever the fitted surface changes.
    m_state.subscribe([this] {
        const QString chainTicker = m_state.underlyingTicker;
        if (!chainTicker.isEmpty() && chainTicker != m_lastChainTicker) {
            m_lastChainTicker = chainTicker;
            if (chainTicker != m_ticker && !m_loading) {
                setTicker(chainTicker);
                if (m_client.hasApiKey()) fetchHistory();
            }
        }
        const QString signature = surfaceSignature();
        if (signature != m_lastSurfaceSignature) {
            m_lastSurfaceSignature = signature;
            updateCards();
            updateCharts();
        }
    });
}

// MARK: - Construction

void VolatilityTab::buildUi()
{
    // ---- Controls ----
    m_tickerEdit = new QLineEdit(this);
    m_tickerEdit->setPlaceholderText("Ticker, e.g. NVDA");
    m_tickerEdit->setMaxLength(12);
    m_history = new QComboBox(this);
    for (int years : { 1, 2, 3, 5, 10 }) m_history->addItem(QStringLiteral("%1 year%2").arg(years).arg(years == 1 ? "" : "s"), years);
    m_history->setCurrentIndex(1);
    m_fetch = ui::makeButton(this, "Fetch History", "primary", "Download daily bars from Massive.com");
    m_sample = ui::makeButton(this, "Sample Data", "secondary", "Load a simulated GARCH price path");
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setWordWrap(true);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setText("Fetch daily bars for a ticker, or load sample data.");

    m_estimator = new QComboBox(this);
    for (RealizedEstimator e : { RealizedEstimator::CloseToClose, RealizedEstimator::Parkinson, RealizedEstimator::GarmanKlass,
                                 RealizedEstimator::RogersSatchell, RealizedEstimator::YangZhang }) {
        m_estimator->addItem(estimatorName(e), static_cast<int>(e));
    }
    m_estimator->setCurrentIndex(4);
    m_estimator->setObjectName("estimatorCombo");
    m_window = ui::makeIntSpinBox(this, 5, 252, 20);
    m_window->setSuffix(" d");
    m_window->setToolTip("Short realized-vol window (trading days)");
    m_windowLong = ui::makeIntSpinBox(this, 10, 504, 60);
    m_windowLong->setSuffix(" d");
    m_windowLong->setToolTip("Long realized-vol window (trading days)");
    m_model = new QComboBox(this);
    m_model->addItem(garchModelName(GarchModel::Garch11), static_cast<int>(GarchModel::Garch11));
    m_model->addItem(garchModelName(GarchModel::GjrGarch11), static_cast<int>(GarchModel::GjrGarch11));
    m_model->setToolTip("GJR adds a leverage term: negative shocks raise variance more than positive ones");
    m_horizon = ui::makeIntSpinBox(this, 1, 504, 21);
    m_horizon->setSuffix(" d");
    m_horizon->setToolTip("Forecast horizon in trading days (21 ≈ one month)");
    m_useRealized = ui::makeButton(this, "Use Realized as σ", "secondary", "Copy the short-window realized vol into the market inputs");
    m_useForecast = ui::makeButton(this, "Use Forecast as σ", "secondary", "Copy the GARCH forecast vol for the horizon into the market inputs");
    m_estimatorNote = new QLabel(this);
    m_estimatorNote->hide();   // the description is shown as the estimator combo's tooltip

    // Sidebar layout: label/widget pairs in a four-column grid, buttons two per row.
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel("Ticker", this), 0, 0);
    grid->addWidget(m_tickerEdit, 0, 1);
    grid->addWidget(new QLabel("History", this), 0, 2);
    grid->addWidget(m_history, 0, 3);
    grid->addWidget(new QLabel("Estimator", this), 1, 0);
    grid->addWidget(m_estimator, 1, 1, 1, 3);
    grid->addWidget(new QLabel("Model", this), 2, 0);
    grid->addWidget(m_model, 2, 1, 1, 3);
    grid->addWidget(new QLabel("Windows", this), 3, 0);
    grid->addWidget(m_window, 3, 1);
    grid->addWidget(new QLabel("Horizon", this), 3, 2);
    grid->addWidget(m_horizon, 3, 3);
    grid->addWidget(new QLabel("Long window", this), 4, 0);
    grid->addWidget(m_windowLong, 4, 1);
    // Button pairs split the width evenly regardless of the label columns above.
    auto* fetchRow = new QHBoxLayout;
    fetchRow->setSpacing(8);
    fetchRow->addWidget(m_fetch, 1);
    fetchRow->addWidget(m_sample, 1);
    grid->addLayout(fetchRow, 5, 0, 1, 4);
    auto* useRow = new QHBoxLayout;
    useRow->setSpacing(8);
    useRow->addWidget(m_useRealized, 1);
    useRow->addWidget(m_useForecast, 1);
    grid->addLayout(useRow, 6, 0, 1, 4);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    m_tickerEdit->setMaximumWidth(QWIDGETSIZE_MAX);
    for (QWidget* w : std::initializer_list<QWidget*>{ m_tickerEdit, m_history, m_estimator, m_window, m_windowLong, m_model, m_horizon, m_fetch, m_sample, m_useRealized, m_useForecast }) {
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        w->setMinimumWidth(60);
    }
    for (QPushButton* b : { m_fetch, m_sample, m_useRealized, m_useForecast }) b->setMinimumWidth(150);

    auto* controls = new QGroupBox("Realized and Forecast Volatility", this);
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setSpacing(8);
    m_modelLabel = new QLabel(this);
    m_modelLabel->setObjectName("muted");
    m_modelLabel->setWordWrap(true);
    m_modelLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_modelLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controlsLayout->addLayout(grid);
    controlsLayout->addWidget(m_status);
    controlsLayout->addWidget(m_modelLabel);

    // ---- Cards: two columns in the sidebar ----
    m_cardRealized = ui::makeCard(this, "Realized σ (20d)");
    m_cardLong = ui::makeCard(this, "Realized σ (60d)");
    m_cardEwma = ui::makeCard(this, "EWMA σ (λ 0.94)");
    m_cardGarchNow = ui::makeCard(this, "GARCH σ (next day)");
    m_cardForecast = ui::makeCard(this, "Forecast σ (21d)");
    m_cardLongRun = ui::makeCard(this, "Long-run σ");
    m_cardImplied = ui::makeCard(this, "Implied ATM σ (21d)");
    m_cardPersistence = ui::makeCard(this, "Persistence");
    auto* cards = new QGridLayout;
    cards->setSpacing(8);
    int index = 0;
    for (const ui::Card& card : { m_cardRealized, m_cardLong, m_cardEwma, m_cardImplied, m_cardGarchNow, m_cardForecast, m_cardLongRun, m_cardPersistence }) {
        card.frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        card.frame->setMinimumWidth(120);
        card.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        card.value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        card.subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        cards->addWidget(card.frame, index / 2, index % 2);
        cards->setColumnStretch(index % 2, 1);
        ++index;
    }

    auto* sidebarContent = new QWidget(this);
    auto* sidebarLayout = new QVBoxLayout(sidebarContent);
    sidebarLayout->setContentsMargins(0, 0, 8, 0);
    sidebarLayout->setSpacing(10);
    sidebarLayout->addWidget(controls);
    sidebarLayout->addLayout(cards);
    sidebarLayout->addStretch(1);
    auto* sidebar = new QScrollArea(this);
    sidebar->setWidgetResizable(true);
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebar->setWidget(sidebarContent);
    sidebar->setMinimumWidth(300);
    sidebar->setMinimumHeight(280);

    // ---- Charts ----
    auto readout = [this] {
        auto* label = new QLabel("Hover a line for the value at that point.", this);
        label->setObjectName("muted");
        label->setFixedHeight(20);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        return label;
    };

    m_historyChart = new QChart;
    m_historyChart->setTitle("Realized volatility history");
    m_historyX = new QCategoryAxis;
    m_historyX->setLabelsPosition(QCategoryAxis::AxisLabelsPositionOnValue);
    m_historyX->setGridLineVisible(true);
    m_historyY = new QValueAxis;
    m_historyY->setTitleText("σ (%)");
    m_historyY->setLabelFormat("%.0f");
    m_historyChart->addAxis(m_historyX, Qt::AlignBottom);
    m_historyChart->addAxis(m_historyY, Qt::AlignLeft);
    QChartView* historyView = ui::makeChartView(this, m_historyChart, 240);
    m_historyReadout = readout();

    m_coneChart = new QChart;
    m_coneChart->setTitle("Volatility cone");
    m_coneX = new QValueAxis;
    m_coneX->setTitleText("Window (trading days)");
    m_coneX->setLabelFormat("%.0f");
    m_coneY = new QValueAxis;
    m_coneY->setTitleText("σ (%)");
    m_coneY->setLabelFormat("%.0f");
    m_coneChart->addAxis(m_coneX, Qt::AlignBottom);
    m_coneChart->addAxis(m_coneY, Qt::AlignLeft);
    QChartView* coneView = ui::makeChartView(this, m_coneChart, 220);
    m_coneReadout = readout();

    m_forecastChart = new QChart;
    m_forecastChart->setTitle("Forecast term structure vs implied");
    m_forecastX = new QValueAxis;
    m_forecastX->setTitleText("Horizon (trading days)");
    m_forecastX->setLabelFormat("%.0f");
    m_forecastY = new QValueAxis;
    m_forecastY->setTitleText("σ (%)");
    m_forecastY->setLabelFormat("%.0f");
    m_forecastChart->addAxis(m_forecastX, Qt::AlignBottom);
    m_forecastChart->addAxis(m_forecastY, Qt::AlignLeft);
    QChartView* forecastView = ui::makeChartView(this, m_forecastChart, 220);
    m_forecastReadout = readout();

    // Small charts: never elide axis labels to "…", and keep legends compact.
    for (QAbstractAxis* axis : std::initializer_list<QAbstractAxis*>{ m_historyX, m_historyY, m_coneX, m_coneY, m_forecastX, m_forecastY }) {
        axis->setTruncateLabels(false);
    }
    for (QChart* chart : { m_historyChart, m_coneChart, m_forecastChart }) {
        QFont legendFont = chart->legend()->font();
        legendFont.setPointSize(10);
        chart->legend()->setFont(legendFont);
        chart->legend()->setMarkerShape(QLegend::MarkerShapeCircle);
    }
    m_coneX->setTickCount(6);
    m_forecastX->setTickCount(6);

    m_coneTable = new QTableWidget(0, ConeColumnCount, this);
    m_coneTable->setHorizontalHeaderLabels({ "Days", "Now", "Pctl", "Min", "Median", "Max" });
    m_coneTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_coneTable->horizontalHeader()->setStretchLastSection(true);
    m_coneTable->horizontalHeader()->setMinimumSectionSize(40);
    for (int c = 0; c < ConeColumnCount; ++c) m_coneTable->setColumnWidth(c, c == ConeWindow ? 46 : 54);
    m_coneTable->horizontalHeader()->setObjectName("heatmapHeader");
    m_coneTable->horizontalHeader()->setFixedHeight(26);
    m_coneTable->verticalHeader()->setVisible(false);
    m_coneTable->verticalHeader()->setDefaultSectionSize(24);
    m_coneTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_coneTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_coneTable->setAlternatingRowColors(true);
    m_coneTable->setMinimumWidth(200);
    m_coneTable->setToolTip("Rolling realized vol per window: latest value, its percentile rank in the history, and the range");

    auto* historyPane = new QWidget(this);
    auto* historyLayout = new QVBoxLayout(historyPane);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(2);
    historyLayout->addWidget(historyView, 1);
    historyLayout->addWidget(m_historyReadout);

    auto* conePane = new QWidget(this);
    auto* coneLayout = new QVBoxLayout(conePane);
    coneLayout->setContentsMargins(0, 0, 0, 0);
    coneLayout->setSpacing(2);
    coneLayout->addWidget(coneView, 1);
    coneLayout->addWidget(m_coneReadout);

    auto* forecastPane = new QWidget(this);
    auto* forecastLayout = new QVBoxLayout(forecastPane);
    forecastLayout->setContentsMargins(0, 0, 0, 0);
    forecastLayout->setSpacing(2);
    forecastLayout->addWidget(forecastView, 1);
    forecastLayout->addWidget(m_forecastReadout);

    // Every pane is resizable: table | cone | forecast, history / lower row, sidebar | charts.
    m_lowerSplitter = new QSplitter(Qt::Horizontal, this);
    m_lowerSplitter->setObjectName("quotesSplitter");   // thin vertical grip style
    m_lowerSplitter->setHandleWidth(8);
    m_lowerSplitter->setChildrenCollapsible(false);
    m_lowerSplitter->addWidget(m_coneTable);
    m_lowerSplitter->addWidget(conePane);
    m_lowerSplitter->addWidget(forecastPane);
    m_lowerSplitter->setStretchFactor(0, 0);
    m_lowerSplitter->setStretchFactor(1, 1);
    m_lowerSplitter->setStretchFactor(2, 1);
    m_lowerSplitter->setSizes({ 360, 500, 500 });

    m_chartSplitter = new QSplitter(Qt::Vertical, this);
    m_chartSplitter->setChildrenCollapsible(false);
    m_chartSplitter->addWidget(historyPane);
    m_chartSplitter->addWidget(m_lowerSplitter);
    m_chartSplitter->setStretchFactor(0, 1);
    m_chartSplitter->setStretchFactor(1, 1);
    m_chartSplitter->setSizes({ 400, 400 });

    // Sidebar (controls + cards, scrolls when short) on the left; charts take the rest.
    m_rootSplitter = new QSplitter(Qt::Horizontal, this);
    m_rootSplitter->setObjectName("quotesSplitter");
    m_rootSplitter->setHandleWidth(8);
    m_rootSplitter->setChildrenCollapsible(false);
    m_rootSplitter->addWidget(sidebar);
    m_rootSplitter->addWidget(m_chartSplitter);
    m_rootSplitter->setStretchFactor(0, 0);
    m_rootSplitter->setStretchFactor(1, 1);
    m_rootSplitter->setSizes({ 384, 1000 });

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 10);
    root->addWidget(m_rootSplitter);

    // Remember the pane sizes and table columns between sessions.
    const QSettings settings;
    for (QSplitter* splitter : { m_rootSplitter, m_chartSplitter, m_lowerSplitter }) {
        const QByteArray state = settings.value(QStringLiteral("volatility/%1").arg(splitter == m_rootSplitter ? "root" : (splitter == m_chartSplitter ? "charts" : "lower"))).toByteArray();
        if (!state.isEmpty()) splitter->restoreState(state);
        connect(splitter, &QSplitter::splitterMoved, this, [this](int, int) { saveLayoutState(); });
    }
    const QByteArray header = settings.value("volatility/coneHeader").toByteArray();
    if (!header.isEmpty()) m_coneTable->horizontalHeader()->restoreState(header);
    connect(m_coneTable->horizontalHeader(), &QHeaderView::sectionResized, this, [this](int, int, int) { saveLayoutState(); });
}

void VolatilityTab::wire()
{
    connect(m_fetch, &QPushButton::clicked, this, [this] { setTicker(m_tickerEdit->text()); fetchAll(); });
    connect(m_tickerEdit, &QLineEdit::returnPressed, this, [this] { setTicker(m_tickerEdit->text()); fetchAll(); });
    connect(m_sample, &QPushButton::clicked, this, [this] { loadSample(); });
    connect(m_estimator, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { recompute(); });
    connect(m_model, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { recompute(); });
    connect(m_window, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { recompute(); });
    connect(m_windowLong, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { recompute(); });
    connect(m_horizon, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        updateCards();
        updateForecastChart();
    });
    connect(m_useRealized, &QPushButton::clicked, this, [this] {
        if (!m_series.empty()) applyVol(m_series.back().vol, QStringLiteral("%1 %2d realized").arg(estimatorName(estimator())).arg(m_window->value()));
    });
    connect(m_useForecast, &QPushButton::clicked, this, [this] {
        if (m_garch.converged) {
            const auto term = m_garch.forecastTermVol(m_horizon->value());
            if (!term.empty()) applyVol(term.back(), QStringLiteral("%1 %2d forecast").arg(garchModelName(model())).arg(m_horizon->value()));
        }
    });
}

// MARK: - Data

void VolatilityTab::setTicker(const QString& ticker)
{
    m_ticker = ticker.trimmed().toUpper();
    if (m_tickerEdit->text() != m_ticker) m_tickerEdit->setText(m_ticker);
}

void VolatilityTab::fetchHistory()
{
    if (m_ticker.isEmpty()) {
        setStatus("Enter a ticker first.", ui::StatusKind::Warning);
        if (onFetchFinished) onFetchFinished(false, "No ticker");
        return;
    }
    if (!m_client.hasApiKey()) {
        setStatus("No Massive API key. Set MASSIVE_API_KEY or POLYGON_API_KEY, or use Market > Set Massive API Key.", ui::StatusKind::Error);
        if (onFetchFinished) onFetchFinished(false, "No API key");
        return;
    }
    if (m_loading) return;
    m_loading = true;
    m_fetch->setEnabled(false);
    const int years = m_history->currentData().toInt();
    const QDate to = QDate::currentDate();
    // A little extra history so the longest cone window has a full first value.
    const QDate from = to.addYears(-years).addDays(-30);
    setStatus(QStringLiteral("Downloading %1 daily bars (%2 year%3)…").arg(m_ticker).arg(years).arg(years == 1 ? "" : "s"), ui::StatusKind::Info);
    const QString ticker = m_ticker;
    m_client.fetchAggregates(ticker, 1, "day", from, to, [this, ticker, years](const MarketDataClient::BarSeries& series) {
        m_loading = false;
        m_fetch->setEnabled(true);
        std::vector<DailyBar> bars;
        bars.reserve(series.bars.size());
        for (const MarketDataClient::Bar& b : series.bars) {
            DailyBar d;
            d.date = QDateTime::fromMSecsSinceEpoch(b.timeMs, QTimeZone::UTC).date().toString(Qt::ISODate).toStdString();
            d.open = b.open;
            d.high = b.high;
            d.low = b.low;
            d.close = b.close;
            d.volume = b.volume;
            bars.push_back(d);
        }
        if (bars.size() < 70) {
            setStatus(QStringLiteral("Only %1 daily bars returned for %2; at least 70 are needed.").arg(bars.size()).arg(ticker), ui::StatusKind::Error);
            if (onFetchFinished) onFetchFinished(false, "Too few bars");
            return;
        }
        m_ticker = ticker;
        setBars(std::move(bars), QStringLiteral("Massive.com, %1 year%2").arg(years).arg(years == 1 ? "" : "s"));
        if (onFetchFinished) onFetchFinished(true, QStringLiteral("%1 daily bars loaded for %2").arg(m_bars.size()).arg(ticker));
    }, [this, ticker](const QString& message) {
        m_loading = false;
        m_fetch->setEnabled(true);
        setStatus(QStringLiteral("%1: %2").arg(ticker, message), ui::StatusKind::Error);
        if (onFetchFinished) onFetchFinished(false, message);
    });
}

void VolatilityTab::fetchAll()
{
    if (m_ticker.isEmpty() || m_ticker == "SAMPLE") {
        fetchHistory();
        return;
    }
    // Request the chain first so its ticker is already the shared one when our bars land;
    // the Option Chain tab's download then cascades to Quotes, Heatmap and Strategy.
    if (onRequestChain && m_state.underlyingTicker != m_ticker && m_client.hasApiKey()) {
        m_lastChainTicker = m_ticker;   // avoid re-fetching our own history when the chain arrives
        onRequestChain(m_ticker);
    }
    fetchHistory();
}

void VolatilityTab::loadSample()
{
    // Two years of a GJR-GARCH path with a mild upward drift, so the tab demonstrates
    // clustering and the leverage effect without a data connection.
    std::mt19937 rng(20261008u);
    std::normal_distribution<double> z(0.0, 1.0);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double omega = 3e-6, alpha = 0.05, beta = 0.88, gamma = 0.08;
    double s2 = omega / (1.0 - alpha - beta - 0.5 * gamma);
    double close = 100.0;
    std::vector<DailyBar> bars;
    QDate date = QDate::currentDate().addDays(-760);
    for (int i = 0; i < 760 && bars.size() < 520; ++i, date = date.addDays(1)) {
        if (date.dayOfWeek() > 5) continue;
        const double e = std::sqrt(s2) * z(rng);
        const double ret = 0.0003 + e;
        DailyBar b;
        b.date = date.toString(Qt::ISODate).toStdString();
        b.open = close * std::exp(0.3 * std::sqrt(s2) * z(rng));
        b.close = close * std::exp(ret);
        const double range = std::sqrt(s2) * (0.6 + 0.8 * u(rng));
        b.high = std::max(b.open, b.close) * std::exp(range * u(rng));
        b.low = std::min(b.open, b.close) * std::exp(-range * u(rng));
        b.volume = 1e6 * (0.5 + u(rng));
        bars.push_back(b);
        close = b.close;
        s2 = omega + alpha * e * e + (e < 0.0 ? gamma * e * e : 0.0) + beta * s2;
    }
    if (m_ticker.isEmpty()) setTicker("SAMPLE");
    setBars(std::move(bars), "simulated GJR-GARCH path (ω 3e-6, α 0.05, β 0.88, γ 0.08)");
}

void VolatilityTab::setBars(std::vector<DailyBar> bars, const QString& source)
{
    m_bars = std::move(bars);
    m_barsSource = source;
    recompute();
    if (!m_bars.empty()) {
        setStatus(QStringLiteral("%1 · %2 daily bars from %3 to %4 · %5 · last close %6")
                      .arg(m_ticker).arg(m_bars.size()).arg(QString::fromStdString(m_bars.front().date), QString::fromStdString(m_bars.back().date), source)
                      .arg(ui::number(m_bars.back().close, 2)),
                  ui::StatusKind::Info);
    }
}

// MARK: - Computation

RealizedEstimator VolatilityTab::estimator() const
{
    return static_cast<RealizedEstimator>(m_estimator->currentData().toInt());
}

GarchModel VolatilityTab::model() const
{
    return static_cast<GarchModel>(m_model->currentData().toInt());
}

void VolatilityTab::recompute()
{
    m_estimator->setToolTip(estimatorDescription(estimator()));
    m_series.clear();
    m_seriesLong.clear();
    m_cone.clear();
    m_returns.clear();
    m_ewma = EwmaResult{};
    m_garch = GarchFit{};
    if (m_bars.size() >= 3) {
        m_series = rollingRealizedVol(m_bars, estimator(), m_window->value(), kPeriodsPerYear);
        m_seriesLong = rollingRealizedVol(m_bars, estimator(), m_windowLong->value(), kPeriodsPerYear);
        m_cone = volCone(m_bars, estimator(), coneWindows(), kPeriodsPerYear);
        m_returns = logReturns(m_bars);
        m_ewma = ewmaVariance(m_returns, kEwmaLambda, kPeriodsPerYear);
        m_garch = fitGarch(m_returns, model(), kPeriodsPerYear);
    }
    m_useRealized->setEnabled(!m_series.empty());
    m_useForecast->setEnabled(m_garch.converged);
    updateCards();
    updateModelLabel();
    updateConeTable();
    updateCharts();
}

bool VolatilityTab::chainMatches() const
{
    return !m_state.surface.empty() && (m_state.underlyingTicker.isEmpty() || m_state.underlyingTicker == m_ticker);
}

double VolatilityTab::impliedAtmVol(double maturity) const
{
    if (!chainMatches() || maturity <= 0.0) return 0.0;
    const ChainMarket cm = m_state.chainMarket(maturity);
    const double carry = cm.model == Model::Black76 ? 0.0 : cm.riskFreeRate - cm.dividendYield;
    const double forward = cm.spot * std::exp(carry * maturity);
    return m_state.surface.impliedVol(forward, maturity, cm);
}

std::vector<std::pair<double, double>> VolatilityTab::impliedTermStructure() const
{
    std::vector<std::pair<double, double>> out;
    if (!chainMatches()) return out;
    for (const ExpirySlice& s : m_state.surface.slices()) {
        const double vol = s.atmVol();
        if (vol > 0.0 && s.maturity > 0.0) out.emplace_back(s.maturity * kPeriodsPerYear, vol);
    }
    return out;
}

QString VolatilityTab::surfaceSignature() const
{
    const auto& slices = m_state.surface.slices();
    return QStringLiteral("%1|%2|%3|%4").arg(m_state.underlyingTicker).arg(slices.size())
        .arg(slices.empty() ? 0.0 : slices.front().atmVol(), 0, 'f', 6).arg(m_state.market.spot, 0, 'f', 4);
}

// MARK: - Presentation

void VolatilityTab::updateCards()
{
    const int window = m_window->value();
    const int windowLong = m_windowLong->value();
    const int horizon = m_horizon->value();
    const double realized = m_series.empty() ? NAN : m_series.back().vol;
    const double realizedLong = m_seriesLong.empty() ? NAN : m_seriesLong.back().vol;

    m_cardRealized.title->setText(QStringLiteral("Realized σ (%1d)").arg(window));
    m_cardRealized.value->setText(pct(realized));
    QString rank;
    for (const ConeRow& row : m_cone) {
        if (row.window == window) rank = QStringLiteral(" · %1th pctl").arg(qRound(row.percentile * 100.0));
    }
    m_cardRealized.subtitle->setText(m_series.empty() ? QStringLiteral("load daily bars") : QString(estimatorName(estimator())) + rank);

    m_cardLong.title->setText(QStringLiteral("Realized σ (%1d)").arg(windowLong));
    m_cardLong.value->setText(pct(realizedLong));
    m_cardLong.subtitle->setText(std::isfinite(realized) && std::isfinite(realizedLong) ? QStringLiteral("%1d − %2d: %3").arg(window).arg(windowLong).arg(pts(realized - realizedLong)) : QString());

    m_cardEwma.value->setText(pct(m_ewma.variance.empty() ? NAN : m_ewma.currentVol()));
    m_cardEwma.subtitle->setText(m_ewma.variance.empty() ? QString() : QStringLiteral("RiskMetrics next-day"));

    const QString modelName = garchModelName(model());
    m_cardGarchNow.title->setText(QStringLiteral("%1 σ (next day)").arg(model() == GarchModel::Garch11 ? "GARCH" : "GJR"));
    m_cardGarchNow.value->setText(pct(m_garch.converged ? m_garch.currentVol() : NAN));
    m_cardGarchNow.subtitle->setText(m_garch.converged ? QStringLiteral("%1 conditional").arg(modelName) : (m_bars.empty() ? QString() : QStringLiteral("fit did not converge")));

    double forecast = NAN;
    if (m_garch.converged) {
        const auto term = m_garch.forecastTermVol(horizon);
        if (!term.empty()) forecast = term.back();
    }
    m_cardForecast.title->setText(QStringLiteral("Forecast σ (%1d)").arg(horizon));
    m_cardForecast.value->setText(pct(forecast));
    m_cardForecast.subtitle->setText(m_garch.converged ? QStringLiteral("avg variance, %1 trading days").arg(horizon) : QString());

    m_cardLongRun.value->setText(pct(m_garch.converged ? m_garch.unconditionalVol() : NAN));
    m_cardLongRun.subtitle->setText(m_garch.converged && std::isfinite(m_garch.halfLife()) ? QStringLiteral("half-life %1 days").arg(ui::number(m_garch.halfLife(), 1)) : QString());

    const double implied = impliedAtmVol(horizon / static_cast<double>(kPeriodsPerYear));
    m_cardImplied.title->setText(QStringLiteral("Implied ATM σ (%1d)").arg(horizon));
    m_cardImplied.value->setText(implied > 0.0 ? pct(implied) : QStringLiteral("–"));
    if (implied > 0.0) {
        QString sub = std::isfinite(forecast) ? QStringLiteral("IV−fcst %1").arg(pts(implied - forecast)) : QString();
        if (std::isfinite(realized)) sub += (sub.isEmpty() ? "" : " · ") + QStringLiteral("IV−RV %1").arg(pts(implied - realized));
        m_cardImplied.subtitle->setText(sub);
    } else if (!m_state.surface.empty() && !m_state.underlyingTicker.isEmpty() && m_state.underlyingTicker != m_ticker) {
        m_cardImplied.subtitle->setText(QStringLiteral("chain is %1").arg(m_state.underlyingTicker));
    } else {
        m_cardImplied.subtitle->setText("load a chain to compare");
    }

    m_cardPersistence.title->setText(model() == GarchModel::Garch11 ? "Persistence α + β" : "Persistence α + β + γ/2");
    m_cardPersistence.frame->setToolTip(m_garch.converged ? QStringLiteral("ω %1 per day² · mean %2 bp/day · last shock %3%")
                                                               .arg(QString::number(m_garch.omega, 'e', 2), ui::number(m_garch.mean * 1e4, 2), ui::number(m_garch.lastResidual * 100.0, 2))
                                                         : QString());
    m_cardPersistence.value->setText(m_garch.converged ? ui::number(m_garch.persistence(), 4) : QStringLiteral("–"));
    if (m_garch.converged) {
        QString sub = QStringLiteral("α %1 · β %2").arg(ui::number(m_garch.alpha, 4), ui::number(m_garch.beta, 4));
        if (model() == GarchModel::GjrGarch11) sub += QStringLiteral(" · γ %1").arg(ui::number(m_garch.gamma, 4));
        m_cardPersistence.subtitle->setText(sub);
    } else {
        m_cardPersistence.subtitle->setText(QString());
    }
}

void VolatilityTab::updateModelLabel()
{
    if (!m_garch.converged) {
        m_modelLabel->setText(m_bars.empty() ? QString() : QStringLiteral("The %1 fit did not converge on %2 returns.").arg(garchModelName(model())).arg(m_returns.size()));
        return;
    }
    QString text = QStringLiteral("%1 · %2 returns · ω %3 · α %4 · β %5").arg(garchModelName(model())).arg(m_garch.observations)
                       .arg(QString::number(m_garch.omega, 'e', 2), ui::number(m_garch.alpha, 4), ui::number(m_garch.beta, 4));
    if (model() == GarchModel::GjrGarch11) text += QStringLiteral(" · γ %1").arg(ui::number(m_garch.gamma, 4));
    text += QStringLiteral(" · LL %1 · AIC %2 · BIC %3").arg(ui::number(m_garch.logLikelihood, 1), ui::number(m_garch.aic, 1), ui::number(m_garch.bic, 1));
    m_modelLabel->setText(text);
    m_modelLabel->setToolTip(QStringLiteral("Data: %1 · mean %2 bp/day · last shock %3%").arg(m_barsSource, ui::number(m_garch.mean * 1e4, 2), ui::number(m_garch.lastResidual * 100.0, 2)));
}

void VolatilityTab::updateConeTable()
{
    m_coneTable->setRowCount(static_cast<int>(m_cone.size()));
    const QColor up(m_theme.up.isEmpty() ? "#22c55e" : m_theme.up);
    const QColor down(m_theme.down.isEmpty() ? "#ef4444" : m_theme.down);
    for (size_t i = 0; i < m_cone.size(); ++i) {
        const ConeRow& row = m_cone[i];
        const int r = static_cast<int>(i);
        m_coneTable->setItem(r, ConeWindow, ui::makeCell(QStringLiteral("%1 d").arg(row.window), Qt::AlignLeft | Qt::AlignVCenter));
        auto* current = ui::makeCell(pct(row.current));
        QFont f = current->font();
        f.setBold(true);
        current->setFont(f);
        m_coneTable->setItem(r, ConeCurrent, current);
        auto* percentile = ui::makeCell(QStringLiteral("%1%").arg(qRound(row.percentile * 100.0)));
        // High percentile = vol rich versus its own history (red); low = cheap (green).
        if (row.percentile >= 0.8) percentile->setForeground(QBrush(down));
        else if (row.percentile <= 0.2) percentile->setForeground(QBrush(up));
        m_coneTable->setItem(r, ConePercentile, percentile);
        m_coneTable->setItem(r, ConeMin, ui::makeCell(pct(row.min)));
        m_coneTable->setItem(r, ConeMedian, ui::makeCell(pct(row.median)));
        m_coneTable->setItem(r, ConeMax, ui::makeCell(pct(row.max)));
        const QString tip = QStringLiteral("%1-day %2 vol over %3 samples\n10th %4 · 25th %5 · 75th %6 · 90th percentile %7")
                                .arg(row.window).arg(estimatorName(estimator())).arg(row.samples).arg(pct(row.p10), pct(row.p25), pct(row.p75), pct(row.p90));
        for (int c = 0; c < ConeColumnCount; ++c) {
            if (QTableWidgetItem* item = m_coneTable->item(r, c)) item->setToolTip(tip);
        }
    }
}

void VolatilityTab::updateCharts()
{
    updateHistoryChart();
    updateConeChart();
    updateForecastChart();
}

void VolatilityTab::showHover(QLabel* readout, const QString& text, bool state)
{
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

void VolatilityTab::updateHistoryChart()
{
    m_historyChart->removeAllSeries();
    if (m_series.empty()) {
        m_historyChart->setTitle("Realized volatility history");
        return;
    }
    m_historyChart->setTitle(QStringLiteral("%1 realized volatility · %2").arg(m_ticker, estimatorName(estimator())));

    double minVol = INFINITY, maxVol = 0.0;
    qint64 minT = std::numeric_limits<qint64>::max(), maxT = 0;
    auto addLine = [&](const QString& name, const QColor& color, Qt::PenStyle style, qreal width, const std::vector<std::pair<qint64, double>>& points) {
        if (points.empty()) return;
        auto* line = new QLineSeries;
        line->setName(name);
        QPen pen(color);
        pen.setWidthF(width);
        pen.setStyle(style);
        line->setPen(pen);
        for (const auto& [t, v] : points) {
            if (!std::isfinite(v)) continue;
            line->append(static_cast<qreal>(t), v * 100.0);
            minVol = std::min(minVol, v * 100.0);
            maxVol = std::max(maxVol, v * 100.0);
            minT = std::min(minT, t);
            maxT = std::max(maxT, t);
        }
        m_historyChart->addSeries(line);
        line->attachAxis(m_historyX);
        line->attachAxis(m_historyY);
        connect(line, &QLineSeries::hovered, this, [this, name](const QPointF& p, bool state) {
            const QString date = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(p.x()), QTimeZone::UTC).date().toString("yyyy-MM-dd");
            showHover(m_historyReadout, QStringLiteral("%1 · %2 · σ %3%").arg(name, date, ui::number(p.y(), 2)), state);
        });
    };

    std::vector<std::pair<qint64, double>> shortPts, longPts, garchPts, ewmaPts;
    for (const VolPoint& p : m_series) shortPts.emplace_back(msecsForDate(p.date), p.vol);
    for (const VolPoint& p : m_seriesLong) longPts.emplace_back(msecsForDate(p.date), p.vol);
    if (m_garch.converged) {
        const std::vector<double> cond = m_garch.conditionalVol();
        for (size_t i = 0; i < cond.size() && i + 1 < m_bars.size(); ++i) garchPts.emplace_back(msecsForDate(m_bars[i + 1].date), cond[i]);
    }
    for (size_t i = 0; i < m_ewma.variance.size() && i + 1 < m_bars.size(); ++i) {
        ewmaPts.emplace_back(msecsForDate(m_bars[i + 1].date), annualiseVariance(m_ewma.variance[i], kPeriodsPerYear));
    }
    // Start every series where the long window first has a value so the axes are not
    // dominated by the burn-in of the GARCH/EWMA recursions.
    const qint64 startT = longPts.empty() ? (shortPts.empty() ? 0 : shortPts.front().first) : longPts.front().first;
    auto trim = [startT](std::vector<std::pair<qint64, double>>& pts) {
        pts.erase(std::remove_if(pts.begin(), pts.end(), [startT](const auto& p) { return p.first < startT; }), pts.end());
    };
    trim(shortPts);
    trim(garchPts);
    trim(ewmaPts);

    addLine(QStringLiteral("EWMA σ"), QColor(m_theme.accent3.isEmpty() ? "#22d3ee" : m_theme.accent3), Qt::DotLine, 1.4, ewmaPts);
    addLine(QStringLiteral("Realized %1d").arg(m_windowLong->value()), QColor(m_theme.textMuted), Qt::DashLine, 1.6, longPts);
    addLine(QStringLiteral("Realized %1d").arg(m_window->value()), QColor(m_theme.accent), Qt::SolidLine, 2.0, shortPts);
    addLine(QStringLiteral("%1 σ").arg(garchModelName(model())), QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2), Qt::SolidLine, 1.6, garchPts);

    if (minT < maxT) {
        // Month-start labels, spaced so that roughly six to eight fit across the span.
        for (const QString& label : m_historyX->categoriesLabels()) m_historyX->remove(label);
        const QDate first = QDateTime::fromMSecsSinceEpoch(minT, QTimeZone::UTC).date();
        const QDate last = QDateTime::fromMSecsSinceEpoch(maxT, QTimeZone::UTC).date();
        const int months = std::max(1, static_cast<int>(first.daysTo(last) / 30));
        const int stepMonths = months <= 8 ? 1 : (months <= 16 ? 2 : (months <= 30 ? 3 : (months <= 72 ? 6 : 12)));
        QDate tick(first.year(), first.month(), 1);
        if (tick < first) tick = tick.addMonths(1);
        // Align multi-month steps to calendar quarters / half-years for familiar labels.
        if (stepMonths > 1) while ((tick.month() - 1) % stepMonths != 0) tick = tick.addMonths(1);
        m_historyX->setStartValue(static_cast<qreal>(minT));
        for (; tick <= last; tick = tick.addMonths(stepMonths)) {
            const qint64 t = QDateTime(tick, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
            m_historyX->append(tick.toString(stepMonths >= 12 ? "yyyy" : "MMM yy"), static_cast<qreal>(t));
        }
        m_historyX->setRange(static_cast<qreal>(minT), static_cast<qreal>(maxT));
    }
    if (std::isfinite(minVol) && maxVol > 0.0) {
        m_historyY->setRange(std::max(0.0, std::floor(minVol / 5.0) * 5.0 - 5.0), std::ceil(maxVol / 5.0) * 5.0 + 5.0);
    }
    styleChart(m_historyChart, m_theme);
}

void VolatilityTab::updateConeChart()
{
    m_coneChart->removeAllSeries();
    if (m_cone.empty()) return;

    double maxVol = 0.0, minVol = INFINITY;
    double maxWindow = 0.0;
    auto addBand = [&](const QString& name, const QColor& color, Qt::PenStyle style, qreal width, double ConeRow::*member) {
        auto* line = new QLineSeries;
        line->setName(name);
        QPen pen(color);
        pen.setWidthF(width);
        pen.setStyle(style);
        line->setPen(pen);
        for (const ConeRow& row : m_cone) {
            const double v = (row.*member) * 100.0;
            line->append(row.window, v);
            maxVol = std::max(maxVol, v);
            minVol = std::min(minVol, v);
            maxWindow = std::max(maxWindow, static_cast<double>(row.window));
        }
        m_coneChart->addSeries(line);
        line->attachAxis(m_coneX);
        line->attachAxis(m_coneY);
        connect(line, &QLineSeries::hovered, this, [this, name](const QPointF& p, bool state) {
            showHover(m_coneReadout, QStringLiteral("%1 · %2-day window · σ %3%").arg(name.trimmed()).arg(qRound(p.x())).arg(ui::number(p.y(), 2)), state);
        });
    };
    const QColor muted(m_theme.textMuted);
    const QColor border(m_theme.borderHover);
    // 10th-90th percentile band: the absolute min/max (in the table) are dominated by a
    // few extreme short windows and would flatten the rest of the cone.
    addBand("p10–p90", border, Qt::DotLine, 1.2, &ConeRow::p90);
    addBand("p25–p75", muted, Qt::DashLine, 1.4, &ConeRow::p75);
    addBand("Median", QColor(m_theme.text), Qt::SolidLine, 1.8, &ConeRow::median);
    addBand("p25–p75 ", muted, Qt::DashLine, 1.4, &ConeRow::p25);
    addBand("p10–p90 ", border, Qt::DotLine, 1.2, &ConeRow::p10);
    // One legend entry per band: hide the lower lines' markers (their names carry a trailing space).
    for (QLegendMarker* marker : m_coneChart->legend()->markers()) {
        if (marker->series() && marker->series()->name().endsWith(' ')) marker->setVisible(false);
    }

    auto* current = new QScatterSeries;
    current->setName("Now");
    current->setMarkerSize(10.0);
    current->setColor(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2));
    current->setBorderColor(QColor(m_theme.surface));
    for (const ConeRow& row : m_cone) {
        current->append(row.window, row.current * 100.0);
        maxVol = std::max(maxVol, row.current * 100.0);
        minVol = std::min(minVol, row.current * 100.0);
    }
    m_coneChart->addSeries(current);
    current->attachAxis(m_coneX);
    current->attachAxis(m_coneY);
    connect(current, &QScatterSeries::hovered, this, [this](const QPointF& p, bool state) {
        QString rank;
        for (const ConeRow& row : m_cone) {
            if (row.window == qRound(p.x())) rank = QStringLiteral(" · %1th percentile").arg(qRound(row.percentile * 100.0));
        }
        showHover(m_coneReadout, QStringLiteral("Current %1-day realized σ %2%%3").arg(qRound(p.x())).arg(ui::number(p.y(), 2), rank), state);
    });

    const auto implied = impliedTermStructure();
    if (!implied.empty()) {
        auto* iv = new QScatterSeries;
        iv->setName("IV");
        iv->setMarkerSize(9.0);
        iv->setMarkerShape(QScatterSeries::MarkerShapeRectangle);
        iv->setColor(QColor(m_theme.accent3.isEmpty() ? "#22d3ee" : m_theme.accent3));
        iv->setBorderColor(QColor(m_theme.surface));
        for (const auto& [days, vol] : implied) {
            if (days <= maxWindow * 1.1) {
                iv->append(days, vol * 100.0);
                maxVol = std::max(maxVol, vol * 100.0);
                minVol = std::min(minVol, vol * 100.0);
            }
        }
        if (iv->count() > 0) {
            m_coneChart->addSeries(iv);
            iv->attachAxis(m_coneX);
            iv->attachAxis(m_coneY);
            connect(iv, &QScatterSeries::hovered, this, [this](const QPointF& p, bool state) {
                showHover(m_coneReadout, QStringLiteral("Implied ATM σ %1% at %2 trading days to expiry").arg(ui::number(p.y(), 2)).arg(qRound(p.x())), state);
            });
        } else {
            delete iv;
        }
    }
    m_coneX->setRange(0.0, std::ceil(maxWindow / 50.0) * 50.0 + 10.0);
    if (std::isfinite(minVol)) m_coneY->setRange(std::max(0.0, std::floor(minVol / 5.0) * 5.0 - 5.0), std::ceil(maxVol / 5.0) * 5.0 + 5.0);
    styleChart(m_coneChart, m_theme);
    m_coneChart->legend()->setAlignment(Qt::AlignRight);   // the chart is tall and narrow: stack the legend
}

void VolatilityTab::updateForecastChart()
{
    m_forecastChart->removeAllSeries();
    if (!m_garch.converged) return;

    const int horizon = m_horizon->value();
    const int days = std::max(kForecastCurveDays, horizon);
    const std::vector<double> term = m_garch.forecastTermVol(days);
    double minVol = INFINITY, maxVol = 0.0;
    auto note = [&](double v) { minVol = std::min(minVol, v); maxVol = std::max(maxVol, v); };

    auto* forecast = new QLineSeries;
    forecast->setName(model() == GarchModel::Garch11 ? "GARCH" : "GJR");
    QPen pen(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2));
    pen.setWidthF(2.0);
    forecast->setPen(pen);
    for (size_t i = 0; i < term.size(); ++i) {
        forecast->append(static_cast<double>(i + 1), term[i] * 100.0);
        note(term[i] * 100.0);
    }
    m_forecastChart->addSeries(forecast);
    forecast->attachAxis(m_forecastX);
    forecast->attachAxis(m_forecastY);
    connect(forecast, &QLineSeries::hovered, this, [this](const QPointF& p, bool state) {
        showHover(m_forecastReadout, QStringLiteral("Forecast σ %1% · %2-day average").arg(ui::number(p.y(), 2)).arg(qRound(p.x())), state);
    });

    auto* longRun = new QLineSeries;
    longRun->setName("Long-run");
    QPen lrPen(QColor(m_theme.textMuted));
    lrPen.setStyle(Qt::DashLine);
    lrPen.setWidthF(1.4);
    longRun->setPen(lrPen);
    longRun->append(1.0, m_garch.unconditionalVol() * 100.0);
    longRun->append(static_cast<double>(days), m_garch.unconditionalVol() * 100.0);
    note(m_garch.unconditionalVol() * 100.0);
    m_forecastChart->addSeries(longRun);
    longRun->attachAxis(m_forecastX);
    longRun->attachAxis(m_forecastY);

    if (!m_series.empty()) {
        auto* realized = new QLineSeries;
        realized->setName(QStringLiteral("RV %1d").arg(m_window->value()));
        QPen rvPen(QColor(m_theme.accent));
        rvPen.setStyle(Qt::DotLine);
        rvPen.setWidthF(1.4);
        realized->setPen(rvPen);
        realized->append(1.0, m_series.back().vol * 100.0);
        realized->append(static_cast<double>(days), m_series.back().vol * 100.0);
        note(m_series.back().vol * 100.0);
        m_forecastChart->addSeries(realized);
        realized->attachAxis(m_forecastX);
        realized->attachAxis(m_forecastY);
    }

    // The chosen horizon.
    auto* marker = new QScatterSeries;
    marker->setName(QStringLiteral("h = %1d").arg(horizon));
    marker->setMarkerSize(11.0);
    marker->setColor(QColor(m_theme.accent2.isEmpty() ? "#f59e0b" : m_theme.accent2));
    marker->setBorderColor(QColor(m_theme.textStrong));
    if (horizon >= 1 && static_cast<size_t>(horizon) <= term.size()) marker->append(horizon, term[static_cast<size_t>(horizon) - 1] * 100.0);
    m_forecastChart->addSeries(marker);
    marker->attachAxis(m_forecastX);
    marker->attachAxis(m_forecastY);
    for (QLegendMarker* legendMarker : m_forecastChart->legend()->markers(marker)) legendMarker->setVisible(false);

    const auto implied = impliedTermStructure();
    if (!implied.empty()) {
        auto* iv = new QLineSeries;
        iv->setName("IV");
        QPen ivPen(QColor(m_theme.accent3.isEmpty() ? "#22d3ee" : m_theme.accent3));
        ivPen.setWidthF(1.8);
        iv->setPen(ivPen);
        iv->setPointsVisible(true);
        for (const auto& [d, vol] : implied) {
            if (d <= days * 1.05) {
                iv->append(d, vol * 100.0);
                note(vol * 100.0);
            }
        }
        if (iv->count() > 0) {
            m_forecastChart->addSeries(iv);
            iv->attachAxis(m_forecastX);
            iv->attachAxis(m_forecastY);
            connect(iv, &QLineSeries::hovered, this, [this](const QPointF& p, bool state) {
                showHover(m_forecastReadout, QStringLiteral("Implied ATM σ %1% · %2 trading days to expiry").arg(ui::number(p.y(), 2)).arg(qRound(p.x())), state);
            });
        } else {
            delete iv;
        }
    }
    m_forecastX->setRange(0.0, static_cast<double>(days) + 5.0);
    if (std::isfinite(minVol)) m_forecastY->setRange(std::max(0.0, std::floor(minVol / 5.0) * 5.0 - 5.0), std::ceil(maxVol / 5.0) * 5.0 + 5.0);
    styleChart(m_forecastChart, m_theme);
    m_forecastChart->legend()->setAlignment(Qt::AlignRight);
}

// MARK: - Actions

void VolatilityTab::applyVol(double vol, const QString& source)
{
    if (!std::isfinite(vol) || vol <= 0.0) return;
    m_state.market.volatility = vol;
    m_state.notify();
    setStatus(QStringLiteral("Market σ set to %1 from the %2.").arg(pct(vol, 2), source), ui::StatusKind::Info);
}

void VolatilityTab::saveLayoutState() const
{
    QSettings settings;
    settings.setValue("volatility/root", m_rootSplitter->saveState());
    settings.setValue("volatility/charts", m_chartSplitter->saveState());
    settings.setValue("volatility/lower", m_lowerSplitter->saveState());
    settings.setValue("volatility/coneHeader", m_coneTable->horizontalHeader()->saveState());
}

void VolatilityTab::setStatus(const QString& text, ui::StatusKind kind)
{
    ui::setStatus(m_status, text, kind);
    m_status->setToolTip(text);
}

void VolatilityTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    updateConeTable();
    updateCharts();
}

QString VolatilityTab::summaryText() const
{
    QString text = QStringLiteral("%1: %2 bars, %3 realized %4d %5 / %6d %7, EWMA %8")
                       .arg(m_ticker).arg(m_bars.size()).arg(estimatorName(estimator())).arg(m_window->value())
                       .arg(pct(m_series.empty() ? NAN : m_series.back().vol, 2)).arg(m_windowLong->value())
                       .arg(pct(m_seriesLong.empty() ? NAN : m_seriesLong.back().vol, 2), pct(m_ewma.variance.empty() ? NAN : m_ewma.currentVol(), 2));
    if (m_garch.converged) {
        const auto term = m_garch.forecastTermVol(m_horizon->value());
        text += QStringLiteral("; %1 ω %2 α %3 β %4 γ %5 persistence %6 next-day %7 forecast(%8d) %9 long-run %10 LL %11")
                    .arg(garchModelName(model()), QString::number(m_garch.omega, 'e', 2), ui::number(m_garch.alpha, 4), ui::number(m_garch.beta, 4),
                         ui::number(m_garch.gamma, 4), ui::number(m_garch.persistence(), 4), pct(m_garch.currentVol(), 2))
                    .arg(m_horizon->value()).arg(pct(term.empty() ? NAN : term.back(), 2), pct(m_garch.unconditionalVol(), 2), ui::number(m_garch.logLikelihood, 1));
    } else {
        text += "; GARCH not fitted";
    }
    const double implied = impliedAtmVol(m_horizon->value() / static_cast<double>(kPeriodsPerYear));
    if (implied > 0.0) text += QStringLiteral("; implied ATM(%1d) %2").arg(m_horizon->value()).arg(pct(implied, 2));
    return text;
}

QString VolatilityTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    s << "ticker,estimator,model\n" << m_ticker << "," << estimatorName(estimator()) << "," << garchModelName(model()) << "\n\n";
    s << "garch_parameter,value\n";
    if (m_garch.converged) {
        s << "omega," << m_garch.omega << "\nalpha," << m_garch.alpha << "\nbeta," << m_garch.beta << "\ngamma," << m_garch.gamma
          << "\nmean," << m_garch.mean << "\npersistence," << m_garch.persistence() << "\nhalf_life_days," << m_garch.halfLife()
          << "\nnext_day_vol," << m_garch.currentVol() << "\nlong_run_vol," << m_garch.unconditionalVol()
          << "\nlog_likelihood," << m_garch.logLikelihood << "\naic," << m_garch.aic << "\nbic," << m_garch.bic << "\nobservations," << m_garch.observations << "\n";
    }
    s << "\nwindow,current,percentile,min,p10,p25,median,p75,p90,max,samples\n";
    for (const ConeRow& r : m_cone) {
        s << r.window << "," << r.current << "," << r.percentile << "," << r.min << "," << r.p10 << "," << r.p25 << "," << r.median << "," << r.p75 << "," << r.p90 << "," << r.max << "," << r.samples << "\n";
    }
    s << "\nhorizon_days,forecast_vol,implied_atm_vol\n";
    if (m_garch.converged) {
        const auto term = m_garch.forecastTermVol(std::max(kForecastCurveDays, m_horizon->value()));
        for (size_t i = 0; i < term.size(); ++i) {
            const double iv = impliedAtmVol(static_cast<double>(i + 1) / kPeriodsPerYear);
            s << (i + 1) << "," << term[i] << "," << (iv > 0.0 ? QString::number(iv) : QString()) << "\n";
        }
    }
    s << "\ndate,close,realized_short,realized_long,garch_vol,ewma_vol\n";
    const std::vector<double> cond = m_garch.converged ? m_garch.conditionalVol() : std::vector<double>{};
    std::map<std::string, double> shortByDate, longByDate;
    for (const VolPoint& p : m_series) shortByDate[p.date] = p.vol;
    for (const VolPoint& p : m_seriesLong) longByDate[p.date] = p.vol;
    for (size_t i = 1; i < m_bars.size(); ++i) {
        const std::string& date = m_bars[i].date;
        s << QString::fromStdString(date) << "," << m_bars[i].close << ",";
        if (shortByDate.count(date)) s << shortByDate.at(date);
        s << ",";
        if (longByDate.count(date)) s << longByDate.at(date);
        s << ",";
        if (i - 1 < cond.size()) s << cond[i - 1];
        s << ",";
        if (i - 1 < m_ewma.variance.size()) s << annualiseVariance(m_ewma.variance[i - 1], kPeriodsPerYear);
        s << "\n";
    }
    return out;
}
