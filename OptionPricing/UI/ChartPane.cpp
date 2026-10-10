//
//  ChartPane.cpp
//  OptionPricing
//

#include "ChartPane.h"

ChartPane::ChartPane(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("pane");
    setAttribute(Qt::WA_StyledBackground, true);

    // Header: link switch, symbol, timeframe, status, promote.
    m_link = new QCheckBox("Link", this);
    m_link->setToolTip("Follow the main chart's symbol (a second timeframe of the same stock)");
    m_link->setCursor(Qt::PointingHandCursor);
    m_symbol = new QLineEdit(this);
    m_symbol->setPlaceholderText("Symbol");
    m_symbol->setMaxLength(12);
    m_symbol->setMaximumWidth(96);
    m_symbol->setToolTip("Symbol for this chart (press Return to load)");
    m_timeframe = new QComboBox(this);
    m_timeframe->setToolTip("Bar size for this chart");
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_promote = new QToolButton(this);
    m_promote->setText("Main ↗");
    m_promote->setCursor(Qt::PointingHandCursor);
    m_promote->setToolTip("Show this symbol on the main chart (and everywhere else in the app)");

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    header->addWidget(m_link);
    header->addWidget(m_symbol);
    header->addWidget(m_timeframe);
    header->addWidget(m_status, 1);
    header->addWidget(m_promote);

    m_view = new QWebEngineView(this);
    m_view->setMinimumSize(320, 220);
    m_view->setContextMenuPolicy(Qt::NoContextMenu);
    m_page = new ChartWebPage(m_view);
    m_view->setPage(m_page);
    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        m_ready = ok;
        if (!ok) { setStatus("chart page failed to load"); return; }
        for (const QString& script : m_pending) m_view->page()->runJavaScript(script);
        m_pending.clear();
    });
    m_view->setHtml(chartPageHtml());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(6);
    root->addLayout(header);
    root->addWidget(m_view, 1);

    auto changed = [this] { if (!m_updating && onConfigChanged) onConfigChanged(); };
    connect(m_link, &QCheckBox::toggled, this, [this, changed](bool on) { m_symbol->setEnabled(!on); changed(); });
    connect(m_symbol, &QLineEdit::editingFinished, this, [this, changed] {
        const QString clean = m_symbol->text().trimmed().toUpper();
        if (clean != m_symbol->text()) m_symbol->setText(clean);
        changed();
    });
    connect(m_timeframe, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed](int) { changed(); });
    connect(m_promote, &QToolButton::clicked, this, [this] { if (onPromote && !symbol().isEmpty()) onPromote(symbol()); });
}

QString ChartPane::symbol() const { return m_symbol->text().trimmed().toUpper(); }

void ChartPane::setSymbol(const QString& symbol)
{
    const QString clean = symbol.trimmed().toUpper();
    if (clean == m_symbol->text()) return;
    m_updating = true;
    m_symbol->setText(clean);
    m_updating = false;
}

bool ChartPane::linked() const { return m_link->isChecked(); }

void ChartPane::setLinked(bool on)
{
    m_updating = true;
    m_link->setChecked(on);
    m_symbol->setEnabled(!on);
    m_updating = false;
}

QString ChartPane::timeframe() const { return m_timeframe->currentText(); }

void ChartPane::setTimeframe(const QString& label)
{
    const int index = m_timeframe->findText(label, Qt::MatchFixedString);
    if (index < 0 || index == m_timeframe->currentIndex()) return;
    m_updating = true;
    m_timeframe->setCurrentIndex(index);
    m_updating = false;
}

void ChartPane::setTimeframes(const QStringList& labels)
{
    m_updating = true;
    const QString current = m_timeframe->currentText();
    m_timeframe->clear();
    m_timeframe->addItems(labels);
    const int index = m_timeframe->findText(current, Qt::MatchFixedString);
    m_timeframe->setCurrentIndex(index >= 0 ? index : std::max(0, static_cast<int>(labels.indexOf("1D"))));
    m_updating = false;
}

QJsonObject ChartPane::toJson() const
{
    return QJsonObject{ { "symbol", symbol() }, { "linked", linked() }, { "timeframe", timeframe() } };
}

void ChartPane::fromJson(const QJsonObject& object)
{
    setLinked(object.value("linked").toBool(false));
    setSymbol(object.value("symbol").toString());
    setTimeframe(object.value("timeframe").toString("1D"));
}

void ChartPane::runJs(const QString& script)
{
    if (!m_ready) {
        m_pending << script;
        return;
    }
    m_view->page()->runJavaScript(script);
}

void ChartPane::legendText(std::function<void(const QString&)> done)
{
    if (!m_ready) { done(QString()); return; }
    m_view->page()->runJavaScript(QStringLiteral("document.getElementById('legend').innerText"),
                                  [done](const QVariant& result) { done(result.toString()); });
}

void ChartPane::setStatus(const QString& text)
{
    m_status->setText(text);
    m_status->setToolTip(text);
}
