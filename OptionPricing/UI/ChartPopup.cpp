//
//  ChartPopup.cpp
//  OptionPricing
//

#include "ChartPopup.h"
#include "Widgets.h"

#include <algorithm>

namespace {
const char* kGeometryKey = "quotes/popupGeometry";
}

ChartPopup::ChartPopup()
    : QWidget(nullptr, Qt::Window)
{
    setWindowTitle("Chart");
    setObjectName("root");
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_QuitOnClose, false);   // closing the main window ends the app, not this one
    setMinimumSize(720, 440);

    m_title = new QLabel(this);
    m_title->setObjectName("tickerSymbol");
    m_name = new QLabel(this);
    m_name->setObjectName("muted");
    m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_name->setMinimumWidth(80);

    // Timeframes: filled by setTimeframes with the Quotes tab's labels.
    m_timeframes = new QButtonGroup(this);
    m_timeframes->setExclusive(true);
    m_timeframeRow = new QHBoxLayout;
    m_timeframeRow->setContentsMargins(0, 0, 0, 0);
    m_timeframeRow->setSpacing(4);
    connect(m_timeframes, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_updating || !onTimeframe) return;
        if (QAbstractButton* button = m_timeframes->button(id)) onTimeframe(button->text());
    });

    m_live = new QCheckBox("Live", this);
    m_live->setCursor(Qt::PointingHandCursor);
    connect(m_live, &QCheckBox::toggled, this, [this](bool on) { if (!m_updating && onLiveToggled) onLiveToggled(on); });

    // Chart type and earnings cone: the same choices as the Quotes toolbar, acting on the
    // Quotes chart (which this window mirrors).
    m_chartType = new QComboBox(this);
    m_chartType->addItem("Candlesticks", "candles");
    m_chartType->addItem("Bars", "bars");
    m_chartType->addItem("Heikin-Ashi", "heikin");
    m_chartType->addItem("Line", "line");
    m_chartType->setObjectName("chartTypeCombo");
    m_chartType->setToolTip("Chart type (the Quotes chart follows)");
    connect(m_chartType, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_updating && onChartType) onChartType(m_chartType->currentData().toString());
    });
    m_cone = new QCheckBox("Earnings cone", this);
    m_cone->setCursor(Qt::PointingHandCursor);
    m_cone->setToolTip("Draw the option-implied move cone through the next earnings date: ±1 sd solid (beat side green, miss side red), ±2 sd dotted");
    connect(m_cone, &QCheckBox::toggled, this, [this](bool on) { if (!m_updating && onEventCone) onEventCone(on); });

    auto* openQuotes = ui::makeButton(this, "Open in Quotes", "secondary",
                                      "Switch to the Quotes tab, which shows this same chart with the watchlist, indicators and drawing tools");
    auto* openChain = ui::makeButton(this, "Option Chain", "secondary", "Switch to the Option Chain tab for this stock");
    connect(openQuotes, &QPushButton::clicked, this, [this] { if (onOpenQuotes) onOpenQuotes(); });
    connect(openChain, &QPushButton::clicked, this, [this] { if (onOpenChain) onOpenChain(); });

    // Two header rows so the controls keep their labels at the window's minimum width:
    // identity and navigation on top, chart controls underneath.
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);
    header->addWidget(m_title);
    header->addWidget(m_name, 1);
    header->addWidget(openQuotes);
    header->addWidget(openChain);
    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(10);
    controls->addLayout(m_timeframeRow);
    controls->addSpacing(6);
    controls->addWidget(m_chartType);
    controls->addWidget(m_cone);
    controls->addWidget(m_live);
    controls->addStretch(1);

    // The same page as the Quotes chart; QuotesTab::mirrorChartTo drives it.
    m_view = new QWebEngineView(this);
    m_view->setMinimumSize(640, 320);
    m_page = new ChartWebPage(m_view);
    m_view->setPage(m_page);
    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        m_ready = ok;
        if (!ok) return;
        for (const QString& script : m_pending) m_view->page()->runJavaScript(script);
        m_pending.clear();
    });
    m_view->setHtml(chartPageHtml());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 12);
    root->setSpacing(8);
    root->addLayout(header);
    root->addLayout(controls);
    root->addWidget(m_view, 1);

    // Default to about three fifths of the screen; a saved geometry is reused unless it is
    // (nearly) the whole screen, which would defeat the point of a pop-out.
    const QRect available = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->availableGeometry() : QRect(0, 0, 1440, 900);
    const QSize fallback(std::min(1100, available.width() * 3 / 5), std::min(680, available.height() * 3 / 5));
    const QByteArray geometry = QSettings().value(kGeometryKey).toByteArray();
    m_restoredGeometry = !geometry.isEmpty() && restoreGeometry(geometry)
                         && width() < available.width() * 95 / 100 && height() < available.height() * 95 / 100;
    if (!m_restoredGeometry) resize(fallback);
}

void ChartPopup::showSymbol(const QString& symbol, const QString& name)
{
    m_title->setText(symbol);
    m_name->setText(name);
    m_name->setToolTip(name);
    setWindowTitle(name.isEmpty() ? QStringLiteral("%1 · Chart").arg(symbol) : QStringLiteral("%1 · %2").arg(symbol, name));
}

void ChartPopup::runJs(const QString& script)
{
    if (!m_ready) {
        m_pending << script;
        return;
    }
    m_view->page()->runJavaScript(script);
}

void ChartPopup::legendText(std::function<void(const QString&)> done)
{
    if (!m_ready) { done(QString()); return; }
    m_view->page()->runJavaScript(QStringLiteral("document.getElementById('legend').innerText"),
                                  [done](const QVariant& result) { done(result.toString()); });
}

void ChartPopup::setTimeframes(const QStringList& labels)
{
    for (QAbstractButton* button : m_timeframes->buttons()) {
        m_timeframes->removeButton(button);
        button->deleteLater();
    }
    int id = 0;
    for (const QString& label : labels) {
        auto* button = new QToolButton(this);
        button->setText(label);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setMinimumWidth(button->fontMetrics().horizontalAdvance(label) + 18);   // never squeezed to a blank square
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QStringLiteral("Show %1 bars (the Quotes chart follows)").arg(label));
        m_timeframes->addButton(button, id++);
        m_timeframeRow->addWidget(button);
    }
}

void ChartPopup::setTimeframe(const QString& label)
{
    m_updating = true;
    for (QAbstractButton* button : m_timeframes->buttons()) {
        if (button->text().compare(label, Qt::CaseInsensitive) == 0) { button->setChecked(true); break; }
    }
    m_updating = false;
}

QString ChartPopup::timeframe() const
{
    const QAbstractButton* button = m_timeframes->checkedButton();
    return button ? button->text() : QString();
}

void ChartPopup::setLive(bool on, int intervalSeconds)
{
    m_updating = true;
    m_live->setChecked(on);
    m_live->setText(on ? QStringLiteral("Live · %1 s").arg(intervalSeconds) : QStringLiteral("Live"));
    m_live->setToolTip(on ? QStringLiteral("Tracking: the price line updates and intraday bars reload every %1 s (the Quotes tab's auto-refresh)").arg(intervalSeconds)
                          : QStringLiteral("Off: turn on to update the price line and reload intraday bars on the Quotes tab's refresh interval"));
    m_updating = false;
}

bool ChartPopup::liveChecked() const
{
    return m_live->isChecked();
}

void ChartPopup::setChartType(const QString& key)
{
    const int index = m_chartType->findData(key);
    if (index < 0 || index == m_chartType->currentIndex()) return;
    m_updating = true;
    m_chartType->setCurrentIndex(index);
    m_updating = false;
}

QString ChartPopup::chartType() const
{
    return m_chartType->currentData().toString();
}

void ChartPopup::setEventCone(bool on)
{
    m_updating = true;
    m_cone->setChecked(on);
    m_updating = false;
}

bool ChartPopup::eventConeChecked() const
{
    return m_cone->isChecked();
}

void ChartPopup::saveGeometry()
{
    QSettings().setValue(kGeometryKey, QWidget::saveGeometry());
}

void ChartPopup::closeEvent(QCloseEvent* event)
{
    saveGeometry();
    QWidget::closeEvent(event);
}

void ChartPopup::hideEvent(QHideEvent* event)
{
    saveGeometry();
    QWidget::hideEvent(event);
}
