//
//  ScenarioTab.cpp
//  OptionPricing
//

#include "ScenarioTab.h"
#include "Formatting.h"

#include <cmath>

using namespace pricing;

ScenarioTab::ScenarioTab(MarketState& state, PositionProvider positionProvider, InputsProvider inputsProvider, QWidget* parent)
    : QWidget(parent)
    , m_state(state)
    , m_positionProvider(std::move(positionProvider))
    , m_inputsProvider(std::move(inputsProvider))
{
    buildUi();
    wire();
    m_state.subscribe([this] { refresh(); });
    refresh();
}

void ScenarioTab::buildUi()
{
    m_source = new QComboBox(this);
    m_source->addItem("Strategy position", 0);
    m_source->addItem("Pricer contract: long call ×1", 1);
    m_source->addItem("Pricer contract: long put ×1", 2);
    m_source->setToolTip("What to evaluate: the Strategy tab's legs or the single contract from the Pricer tab");

    m_metric = new QComboBox(this);
    for (ScenarioMetric m : { ScenarioMetric::Pnl, ScenarioMetric::Value, ScenarioMetric::Delta, ScenarioMetric::Gamma,
                              ScenarioMetric::Vega, ScenarioMetric::Theta, ScenarioMetric::Rho, ScenarioMetric::Vanna, ScenarioMetric::Charm }) {
        m_metric->addItem(scenarioMetricName(m), static_cast<int>(m));
    }
    m_metric->setToolTip("Quantity shown in each cell");

    m_axis = new QComboBox(this);
    m_axis->addItem("Volatility shift (columns)", static_cast<int>(ScenarioAxis::Volatility));
    m_axis->addItem("Days elapsed (columns)", static_cast<int>(ScenarioAxis::Time));
    m_axis->setToolTip("Second dimension of the grid; rows are always the underlying price");

    m_spotRange = ui::makeSpinBox(this, 1.0, 95.0, 5.0, 0, 20.0, " %");
    m_spotRange->setToolTip("Rows span spot × (1 ± range)");
    m_rows = ui::makeIntSpinBox(this, 3, 61, 13);
    m_columns = ui::makeIntSpinBox(this, 1, 25, 7);
    m_volRange = ui::makeSpinBox(this, 0.5, 100.0, 1.0, 1, 10.0, " pts");
    m_volRange->setToolTip("Columns span volatility ± this many percentage points");
    m_maxDays = ui::makeIntSpinBox(this, 0, 3650, 0);
    m_maxDays->setSpecialValueText("to first expiry");
    m_maxDays->setToolTip("Columns span 0 to this many days (0 = up to the first expiry)");

    auto* controls = new QGroupBox("Scenario", this);
    auto* grid = new QGridLayout(controls);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel("Source", controls), 0, 0);
    grid->addWidget(m_source, 0, 1);
    grid->addWidget(new QLabel("Metric", controls), 0, 2);
    grid->addWidget(m_metric, 0, 3);
    grid->addWidget(new QLabel("Axis", controls), 0, 4);
    grid->addWidget(m_axis, 0, 5);
    grid->addWidget(new QLabel("Spot range", controls), 1, 0);
    grid->addWidget(m_spotRange, 1, 1);
    grid->addWidget(new QLabel("Rows", controls), 1, 2);
    grid->addWidget(m_rows, 1, 3);
    grid->addWidget(new QLabel("Columns", controls), 1, 4);
    grid->addWidget(m_columns, 1, 5);
    m_volRangeLabel = new QLabel("Vol range", controls);
    m_maxDaysLabel = new QLabel("Max days", controls);
    grid->addWidget(m_volRangeLabel, 2, 0);
    grid->addWidget(m_volRange, 2, 1);
    grid->addWidget(m_maxDaysLabel, 2, 2);
    grid->addWidget(m_maxDays, 2, 3);
    m_copy = ui::makeButton(controls, "Copy as CSV", "secondary", "Copy the grid to the clipboard");
    grid->addWidget(m_copy, 2, 5);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    grid->setColumnStretch(5, 1);

    m_table = new QTableWidget(this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::ContiguousSelection);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->setAlternatingRowColors(false);

    m_summary = new QLabel(this);
    m_summary->setObjectName("muted");
    m_summary->setWordWrap(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);
    root->addWidget(controls);
    root->addWidget(m_table, 1);
    root->addWidget(m_summary);
}

void ScenarioTab::wire()
{
    auto changed = [this] { if (!m_updating) refresh(); };
    for (QComboBox* box : { m_source, m_metric, m_axis }) {
        connect(box, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed](int) { changed(); });
    }
    connect(m_spotRange, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    connect(m_volRange, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [changed](double) { changed(); });
    for (QSpinBox* box : { m_rows, m_columns, m_maxDays }) {
        connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [changed](int) { changed(); });
    }
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(resultsCsv());
    });
}

Position ScenarioTab::sourcePosition() const
{
    const int source = m_source->currentData().toInt();
    if (source == 0 && m_positionProvider) {
        return m_positionProvider();
    }
    Position pos;
    pos.multiplier = 100.0;
    if (m_inputsProvider) {
        const Inputs in = m_inputsProvider();
        Leg leg;
        leg.kind = source == 1 ? LegKind::Call : LegKind::Put;
        leg.quantity = 1.0;
        leg.strike = in.strike;
        leg.maturity = in.maturity;
        leg.volatility = 0.0;
        if (isValid(in)) {
            leg.entryPrice = legPrice(in, source == 1 ? OptionType::Call : OptionType::Put);
        }
        pos.legs.push_back(leg);
    }
    return pos;
}

ScenarioRequest ScenarioTab::request() const
{
    ScenarioRequest req;
    req.metric = static_cast<ScenarioMetric>(m_metric->currentData().toInt());
    req.axis = static_cast<ScenarioAxis>(m_axis->currentData().toInt());
    req.spotRangePercent = m_spotRange->value();
    req.rows = m_rows->value();
    req.columns = m_columns->value();
    req.volRangePoints = m_volRange->value();
    req.maxDays = m_maxDays->value();
    return req;
}

void ScenarioTab::refresh()
{
    const ScenarioRequest req = request();
    const bool byVol = req.axis == ScenarioAxis::Volatility;
    m_volRange->setEnabled(byVol);
    m_maxDays->setEnabled(!byVol);

    const Position pos = sourcePosition();
    const double horizon = latestExpiry(pos);
    const Market market = m_state.marketFor(horizon > 0.0 ? horizon : 1.0);

    m_table->clear();
    if (pos.legs.empty()) {
        m_table->setRowCount(0);
        m_table->setColumnCount(0);
        m_summary->setText("The selected source has no legs. Load a preset or add legs on the Strategy tab.");
        m_grid = ScenarioGrid();
        return;
    }

    m_grid = buildScenarioGrid(pos, market, req);
    const int rows = static_cast<int>(m_grid.spots.size());
    const int cols = static_cast<int>(m_grid.columnValues.size());
    m_table->setRowCount(rows);
    m_table->setColumnCount(cols);

    QStringList headers;
    for (double c : m_grid.columnValues) {
        if (byVol) {
            headers << QStringLiteral("σ %1%2 pts (%3%)").arg(c >= 0 ? "+" : "").arg(ui::number(c, 1)).arg(ui::number(market.volatility * 100.0 + c, 1));
        } else {
            headers << (c == 0 ? QStringLiteral("today") : QStringLiteral("+%1 d").arg(static_cast<int>(std::lround(c))));
        }
    }
    m_table->setHorizontalHeaderLabels(headers);
    QStringList rowHeaders;
    for (double s : m_grid.spots) {
        const double pct = (s / market.spot - 1.0) * 100.0;
        rowHeaders << QStringLiteral("%1  (%2%3%)").arg(ui::number(s, 2)).arg(pct >= 0 ? "+" : "").arg(ui::number(pct, 1));
    }
    m_table->setVerticalHeaderLabels(rowHeaders);

    const bool diverging = req.metric != ScenarioMetric::Value;
    const QColor neutral(m_theme.surface);
    const QColor positive(m_theme.profit);
    const QColor negative(m_theme.loss);
    const QColor accent(m_theme.accent);
    const double maxAbs = std::max(std::fabs(m_grid.minValue), std::fabs(m_grid.maxValue));
    const double span = m_grid.maxValue - m_grid.minValue;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const double v = m_grid.cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
            QTableWidgetItem* item = ui::makeCell(req.metric == ScenarioMetric::Pnl || req.metric == ScenarioMetric::Value
                                                      ? ui::signedMoney(v, 0) : ui::compact(v),
                                                  Qt::AlignCenter);
            QColor background = neutral;
            if (std::isfinite(v)) {
                if (diverging) {
                    const double t = maxAbs > 0 ? std::sqrt(std::fabs(v) / maxAbs) : 0.0;   // sqrt makes small values visible
                    background = blend(neutral, v >= 0 ? positive : negative, 0.75 * t);
                } else {
                    const double t = span > 0 ? (v - m_grid.minValue) / span : 0.0;
                    background = blend(neutral, accent, 0.7 * t);
                }
            }
            item->setBackground(QBrush(background));
            const bool darkCell = background.lightnessF() < 0.45;
            item->setForeground(QBrush(QColor(darkCell ? "#f8fafc" : m_theme.textStrong)));
            m_table->setItem(r, c, item);
        }
    }

    const QString metricName = scenarioMetricName(req.metric);
    m_summary->setText(QStringLiteral("%1 for %2 leg%3 at spot %4, vol %5. Range across the grid: %6 to %7. %8")
                           .arg(metricName)
                           .arg(pos.legs.size()).arg(pos.legs.size() == 1 ? "" : "s")
                           .arg(ui::number(market.spot, 2), ui::percent(market.volatility, 1),
                                ui::compact(m_grid.minValue), ui::compact(m_grid.maxValue))
                           .arg(byVol ? "Columns shift the volatility of every leg by the same amount."
                                      : "Columns move the valuation date forward; legs that have expired are held at intrinsic value."));
}

void ScenarioTab::applyTheme(const Theme& theme)
{
    m_theme = theme;
    refresh();
}

QJsonObject ScenarioTab::toJson() const
{
    QJsonObject o;
    o["source"] = m_source->currentData().toInt();
    o["metric"] = m_metric->currentData().toInt();
    o["axis"] = m_axis->currentData().toInt();
    o["spotRange"] = m_spotRange->value();
    o["rows"] = m_rows->value();
    o["columns"] = m_columns->value();
    o["volRange"] = m_volRange->value();
    o["maxDays"] = m_maxDays->value();
    return o;
}

void ScenarioTab::fromJson(const QJsonObject& o)
{
    m_updating = true;
    m_source->setCurrentIndex(std::max(0, m_source->findData(o["source"].toInt(0))));
    m_metric->setCurrentIndex(std::max(0, m_metric->findData(o["metric"].toInt(0))));
    m_axis->setCurrentIndex(std::max(0, m_axis->findData(o["axis"].toInt(0))));
    m_spotRange->setValue(o["spotRange"].toDouble(20.0));
    m_rows->setValue(o["rows"].toInt(13));
    m_columns->setValue(o["columns"].toInt(7));
    m_volRange->setValue(o["volRange"].toDouble(10.0));
    m_maxDays->setValue(o["maxDays"].toInt(0));
    m_updating = false;
    refresh();
}

QString ScenarioTab::resultsCsv() const
{
    QString out;
    QTextStream s(&out);
    const bool byVol = request().axis == ScenarioAxis::Volatility;
    s << "spot";
    for (double c : m_grid.columnValues) {
        s << "," << (byVol ? QStringLiteral("vol%1%2").arg(c >= 0 ? "+" : "").arg(c) : QStringLiteral("day%1").arg(static_cast<int>(std::lround(c))));
    }
    s << "\n";
    for (size_t r = 0; r < m_grid.spots.size(); ++r) {
        s << m_grid.spots[r];
        for (double v : m_grid.cells[r]) s << "," << v;
        s << "\n";
    }
    return out;
}
