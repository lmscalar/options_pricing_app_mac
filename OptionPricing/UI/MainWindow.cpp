//
//  MainWindow.cpp
//  OptionPricing
//

#include "MainWindow.h"
#include "BatchDialog.h"
#include "ChainTab.h"
#include "HeatmapTab.h"
#include "PricerTab.h"
#include "QuotesTab.h"
#include "RateCurveDialog.h"
#include "ScenarioTab.h"
#include "StrategyTab.h"
#include "Widgets.h"

namespace {
constexpr int kMaxRecentFiles = 8;
constexpr const char* kWorkspaceFilter = "Option Pricer workspace (*.optws *.json);;All files (*)";
} // namespace

MainWindow::MainWindow()
{
    setWindowTitle("Option Pricer");
    buildUi();
    buildMenus();

    const QSettings settings;
    applyTheme(settings.value("appearance/darkMode", false).toBool());
    // A --window WxH argument (developer aid) wins over the remembered geometry.
    if (!QCoreApplication::arguments().contains("--window")) {
        restoreGeometry(settings.value("window/geometry").toByteArray());
    }
    if (size().width() < 900) {
        resize(1360, 900);
    }
    // No explicit minimum: Qt derives it from the densest tab so the window can never be
    // shrunk into a state where panes overlap.
    rebuildRecentMenu();
    statusBar()->showMessage("Ready. Edit the market on the Pricer tab; every other tab follows it.", 8000);
}

// MARK: - Construction

void MainWindow::buildUi()
{
    m_pricer = new PricerTab(m_state, this);
    m_strategy = new StrategyTab(m_state, this);
    m_scenario = new ScenarioTab(m_state, [this] { return m_strategy->position(); }, [this] { return m_pricer->currentInputs(); }, this);
    m_chain = new ChainTab(m_state, this);
    m_heatmap = new HeatmapTab(m_state, this);
    m_quotes = new QuotesTab(m_state, this);
    m_quotes->onOpenInChain = [this](const QString& ticker) {
        m_chain->setTicker(ticker);
        m_tabs->setCurrentWidget(m_chain);
        m_chain->fetchLiveChain();
    };

    m_pricer->onAddLeg = [this](const pricing::Leg& leg) {
        m_strategy->addLeg(leg);
        m_tabs->setCurrentWidget(m_strategy);
        statusBar()->showMessage("Leg added to the strategy.", 4000);
    };
    m_pricer->onEditRateCurve = [this] { editRateCurve(); };
    m_strategy->onPositionChanged = [this] { m_scenario->refresh(); };
    auto sendToPricer = [this](double strike, double maturity, double vol, const QString& expiryDate) {
        const QDate expiry = QDate::fromString(expiryDate, Qt::ISODate);
        if (expiry.isValid()) {
            m_pricer->setContractWithDate(strike, expiry, vol);
            statusBar()->showMessage(QStringLiteral("Pricer set to strike %1 expiring %2 (%3 DTE), σ = %4%.")
                                         .arg(QString::number(strike, 'f', 2), expiryDate).arg(QDate::currentDate().daysTo(expiry))
                                         .arg(QString::number(vol * 100.0, 'f', 2)), 6000);
        } else {
            m_pricer->setContract(strike, maturity, vol);
            statusBar()->showMessage(QStringLiteral("Pricer set to strike %1, T = %2 years, σ = %3%.")
                                         .arg(QString::number(strike, 'f', 2), QString::number(maturity, 'f', 4), QString::number(vol * 100.0, 'f', 2)), 6000);
        }
        m_tabs->setCurrentWidget(m_pricer);
    };
    m_chain->onSendToPricer = sendToPricer;
    m_heatmap->onSendToPricer = sendToPricer;
    m_heatmap->onRequestFetch = [this](const QString& ticker) {
        // The Option Chain tab owns the download; the heatmap refreshes when the state changes.
        m_chain->setTicker(ticker);
        m_chain->fetchLiveChain();
        statusBar()->showMessage(QStringLiteral("Downloading %1 option chain from Massive.com…").arg(ticker), 8000);
    };
    m_chain->onLiveOperationFinished = [this](bool ok, const QString& message) {
        statusBar()->showMessage(message, ok ? 10000 : 20000);
    };

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(m_quotes, "Quotes");
    m_tabs->addTab(m_pricer, "Pricer");
    m_tabs->addTab(m_strategy, "Strategy");
    m_tabs->addTab(m_scenario, "Scenarios");
    m_tabs->addTab(m_chain, "Option Chain");
    m_tabs->addTab(m_heatmap, "Heatmap");

    auto* title = new QLabel("Option Pricer", this);
    title->setObjectName("title");
    m_subtitle = new QLabel("European and American valuation with Greeks, implied volatility, strategy analysis, scenario grids and "
                            "volatility surfaces. Black-Scholes-Merton for spot assets, Black-76 for futures.", this);
    m_subtitle->setObjectName("muted");
    m_subtitle->setWordWrap(true);

    m_themeToggle = new QPushButton(this);
    m_themeToggle->setObjectName("themeToggle");
    m_themeToggle->setCheckable(true);
    m_themeToggle->setCursor(Qt::PointingHandCursor);
    m_themeToggle->setToolTip("Switch between the light and dark appearance");
    connect(m_themeToggle, &QPushButton::toggled, this, [this](bool checked) { applyTheme(checked); });

    // Quote banner: visible on every tab, driven by the shared market state.
    m_banner = new QFrame(this);
    m_banner->setObjectName("tickerBanner");
    m_banner->setAttribute(Qt::WA_StyledBackground, true);
    m_bannerLogo = new QLabel(m_banner);
    m_bannerLogo->setFixedSize(44, 44);
    m_bannerLogo->setAlignment(Qt::AlignCenter);
    m_bannerName = new QLabel(m_banner);
    m_bannerName->setObjectName("tickerMeta");
    // Text labels in the banner must not dictate the window's minimum width: they shrink
    // and elide instead (see elideBanner).
    m_bannerName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_bannerName->setMinimumWidth(60);
    m_bannerSymbol = new QLabel(m_banner);
    m_bannerSymbol->setObjectName("tickerSymbol");
    m_bannerPrice = new QLabel(m_banner);
    m_bannerPrice->setObjectName("tickerPrice");
    m_bannerPrice->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bannerChange = new QLabel(m_banner);
    m_bannerChange->setObjectName("tickerFlat");
    m_bannerMeta = new QLabel(m_banner);
    m_bannerMeta->setObjectName("tickerMeta");
    m_bannerMeta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_bannerMeta->setMinimumWidth(120);
    auto* identity = new QVBoxLayout;
    identity->setContentsMargins(0, 0, 0, 0);
    identity->setSpacing(0);
    identity->addWidget(m_bannerSymbol);
    identity->addWidget(m_bannerName);
    auto* bannerTop = new QHBoxLayout;
    bannerTop->setContentsMargins(0, 0, 0, 0);
    bannerTop->setSpacing(12);
    bannerTop->addLayout(identity);
    bannerTop->addWidget(m_bannerPrice);
    bannerTop->addWidget(m_bannerChange);
    auto* bannerText = new QVBoxLayout;
    bannerText->setContentsMargins(0, 0, 0, 0);
    bannerText->setSpacing(0);
    bannerText->addLayout(bannerTop);
    bannerText->addWidget(m_bannerMeta);
    auto* bannerLayout = new QHBoxLayout(m_banner);
    bannerLayout->setContentsMargins(12, 6, 14, 6);
    bannerLayout->setSpacing(12);
    bannerLayout->addWidget(m_bannerLogo);
    bannerLayout->addLayout(bannerText);
    m_state.subscribe([this] { updateBanner(); });
    updateBanner();

    m_banner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_banner->setMaximumWidth(760);
    auto* header = new QHBoxLayout;
    header->addWidget(title);
    header->addSpacing(20);
    header->addWidget(m_banner, 1);
    header->addStretch(0);
    header->addSpacing(12);
    header->addWidget(m_themeToggle, 0, Qt::AlignRight);

    auto* central = new QWidget(this);
    central->setObjectName("root");
    central->setAttribute(Qt::WA_StyledBackground, true);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(24, 16, 24, 12);
    root->setSpacing(8);
    root->addLayout(header);
    root->addWidget(m_subtitle);
    root->addSpacing(4);
    root->addWidget(m_tabs, 1);
    setCentralWidget(central);
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu("&File");
    QAction* newAction = file->addAction("&New Workspace", QKeySequence::New, this, [this] { newWorkspace(); });
    newAction->setStatusTip("Reset every tab to the defaults");
    file->addAction("&Open Workspace…", QKeySequence::Open, this, [this] { openWorkspace(); });
    m_recentMenu = file->addMenu("Open &Recent");
    file->addAction("&Save Workspace", QKeySequence::Save, this, [this] { saveWorkspace(); });
    file->addAction("Save Workspace &As…", QKeySequence::SaveAs, this, [this] { saveWorkspaceAs(); });
    file->addSeparator();
    file->addAction("&Import Option Chain CSV…", QKeySequence(Qt::CTRL | Qt::Key_I), this, [this] { importChain(); });
    file->addAction("&Batch Price CSV…", QKeySequence(Qt::CTRL | Qt::Key_B), this, [this] { batchPrice(); });
    file->addSeparator();
    file->addAction("&Export Current Tab as CSV…", QKeySequence(Qt::CTRL | Qt::Key_E), this, [this] { exportResults(); });
    file->addAction("&Copy Current Tab as CSV", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, [this] { copyResults(); });
    file->addSeparator();
    file->addAction("&Quit", QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu* market = menuBar()->addMenu("&Market");
    market->addAction("Fetch &Live Option Chain…", QKeySequence(Qt::CTRL | Qt::Key_L), this, [this] {
        m_tabs->setCurrentWidget(m_chain);
        if (m_chain->ticker().isEmpty()) {
            bool ok = false;
            const QString symbol = QInputDialog::getText(this, "Fetch live option chain", "Ticker symbol:", QLineEdit::Normal, QString(), &ok);
            if (!ok || symbol.trimmed().isEmpty()) return;
            m_chain->setTicker(symbol);
        }
        m_chain->fetchLiveChain();
    });
    market->addAction("Load &Treasury Curve from Massive", this, [this] {
        m_tabs->setCurrentWidget(m_chain);
        m_chain->loadTreasuryCurve();
    });
    market->addAction("Load &Dividends from Massive", this, [this] {
        m_tabs->setCurrentWidget(m_chain);
        if (m_chain->ticker().isEmpty()) {
            bool ok = false;
            const QString symbol = QInputDialog::getText(this, "Load dividends", "Ticker symbol:", QLineEdit::Normal, QString(), &ok);
            if (!ok || symbol.trimmed().isEmpty()) return;
            m_chain->setTicker(symbol);
        }
        m_chain->loadDividends();
    });
    market->addAction("Set Massive &API Key…", this, [this] { m_chain->promptForApiKey(); });
    market->addAction("Forget Stored API Key", this, [this] {
        MarketDataClient::forgetStoredApiKey();
        m_chain->client().setApiKey(MarketDataClient::discoverApiKey(), false);
        statusBar()->showMessage("Stored API key removed from preferences.", 4000);
    });
    market->addSeparator();
    market->addAction("Edit &Rate Curve…", QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] { editRateCurve(); });
    market->addAction("Generate &Sample Option Chain", this, [this] {
        m_tabs->setCurrentWidget(m_chain);
        m_chain->generateSample();
    });

    QMenu* view = menuBar()->addMenu("&View");
    m_darkAction = view->addAction("&Dark Mode", QKeySequence(Qt::CTRL | Qt::Key_D), this, [this] { applyTheme(!m_darkMode); });
    m_darkAction->setCheckable(true);
    view->addSeparator();
    const char* tabNames[] = { "&Quotes", "&Pricer", "&Strategy", "S&cenarios", "Option C&hain", "&Heatmap" };
    for (int i = 0; i < 6; ++i) {
        view->addAction(tabNames[i], QKeySequence(Qt::CTRL | (Qt::Key_1 + i)), this, [this, i] { m_tabs->setCurrentIndex(i); });
    }

    QMenu* help = menuBar()->addMenu("&Help");
    help->addAction("&About Option Pricer", this, [this] { showAbout(); });
}

void MainWindow::applyTheme(bool dark)
{
    m_darkMode = dark;
    const Theme theme = dark ? darkTheme() : lightTheme();
    QApplication::setPalette(paletteFor(theme));
    qApp->setStyleSheet(styleSheetFor(theme));

    {
        const QSignalBlocker blocker(m_themeToggle);
        m_themeToggle->setChecked(dark);
    }
    m_themeToggle->setText(dark ? "☀  Light Mode" : "☾  Dark Mode");
    if (m_darkAction) {
        m_darkAction->setChecked(dark);
    }

    updateBanner();
    m_pricer->applyTheme(theme);
    m_strategy->applyTheme(theme);
    m_scenario->applyTheme(theme);
    m_chain->applyTheme(theme);
    m_heatmap->applyTheme(theme);
    m_quotes->applyTheme(theme);

    QSettings settings;
    settings.setValue("appearance/darkMode", dark);
}

void MainWindow::updateBanner()
{
    const bool live = !m_state.underlyingTicker.isEmpty();
    m_bannerSymbol->setText(live ? m_state.underlyingTicker : QStringLiteral("UNDERLYING"));
    m_bannerNameFull = live ? (m_state.companyName.isEmpty() ? QStringLiteral("…") : m_state.companyName) : QStringLiteral("no chain loaded");
    {
        const Theme theme = m_darkMode ? darkTheme() : lightTheme();
        const qreal dpr = devicePixelRatioF();
        if (!live) {
            m_bannerLogo->setPixmap(ui::monogramBadge(QStringLiteral("$"), QColor(theme.surfaceAlt), QColor(theme.textMuted), 44, dpr));
        } else if (m_state.logo.isNull()) {
            m_bannerLogo->setPixmap(ui::monogramBadge(m_state.underlyingTicker, QColor(theme.accent2), QColor(theme.window), 44, dpr));
        } else {
            m_bannerLogo->setPixmap(ui::roundedLogo(m_state.logo, 44, dpr));
        }
        m_bannerLogo->setToolTip(m_state.companyName.isEmpty() ? m_state.underlyingTicker
                                                               : QStringLiteral("%1%2").arg(m_state.companyName, m_state.exchange.isEmpty() ? QString() : " · " + m_state.exchange));
    }
    m_bannerPrice->setText(QString::number(m_state.market.spot, 'f', 2));

    if (m_state.hasDayChange()) {
        const double change = m_state.dayChange();
        const double pct = m_state.dayChangePercent();
        const QString sign = change > 1e-9 ? "+" : (change < -1e-9 ? "−" : "");
        const QString arrow = change > 1e-9 ? "▲" : (change < -1e-9 ? "▼" : "•");
        m_bannerChange->setText(QStringLiteral("%1 %2%3  (%2%4%)").arg(arrow, sign, QString::number(std::fabs(change), 'f', 2), QString::number(std::fabs(pct), 'f', 2)));
        m_bannerChange->setObjectName(change > 1e-9 ? "tickerUp" : (change < -1e-9 ? "tickerDown" : "tickerFlat"));
        m_bannerChange->setToolTip(QStringLiteral("Change versus the previous close of %1").arg(QString::number(m_state.previousClose, 'f', 2)));
    } else {
        m_bannerChange->setText(live ? QStringLiteral("—") : QStringLiteral("model spot"));
        m_bannerChange->setObjectName("tickerFlat");
        m_bannerChange->setToolTip("Daily change appears after a live fetch supplies the previous close");
    }
    ui::restyle(m_bannerChange);

    QStringList meta;
    if (!m_state.spotSource.isEmpty()) {
        meta << (m_state.spotSource == "option parity" ? QStringLiteral("parity-implied from options") : QStringLiteral("Massive %1").arg(m_state.spotSource));
    } else if (!live) {
        meta << "set on the Pricer tab";
    }
    if (m_state.spotAsOf.isValid() && m_state.vendorSpot > 0.0) {
        const qint64 delay = m_state.spotAsOf.secsTo(QDateTime::currentDateTime()) / 60;
        meta << QStringLiteral("vendor %1 as of %2 (%3 min delayed)").arg(QString::number(m_state.vendorSpot, 'f', 2), m_state.spotAsOf.toString("HH:mm")).arg(std::max<qint64>(0, delay));
    }
    if (m_state.chainTime.isValid()) meta << QStringLiteral("chain %1").arg(m_state.chainTime.toString("HH:mm:ss"));
    if (!m_state.chainQuotes.empty()) meta << QStringLiteral("%1 contracts").arg(m_state.chainQuotes.size());
    m_bannerMetaFull = meta.join("  ·  ");
    m_bannerMeta->setToolTip(m_bannerMetaFull);
    m_bannerName->setToolTip(m_bannerNameFull);
    elideBanner();
}

void MainWindow::elideBanner()
{
    const int nameWidth = std::max(40, m_bannerName->width());
    const int metaWidth = std::max(80, m_bannerMeta->width());
    m_bannerName->setText(m_bannerName->fontMetrics().elidedText(m_bannerNameFull, Qt::ElideRight, nameWidth));
    m_bannerMeta->setText(m_bannerMeta->fontMetrics().elidedText(m_bannerMetaFull, Qt::ElideRight, metaWidth));
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    elideBanner();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // A remembered geometry narrower than the content's minimum would clip the header.
    QTimer::singleShot(0, this, [this] {
        const QSize minimum = minimumSizeHint();
        if (width() < minimum.width() || height() < minimum.height()) {
            resize(std::max(width(), minimum.width()), std::max(height(), minimum.height()));
        }
        elideBanner();
    });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    QMainWindow::closeEvent(event);
}

// MARK: - Workspace

QJsonObject MainWindow::workspaceJson() const
{
    QJsonObject root;
    root["format"] = "option-pricer-workspace";
    root["version"] = 1;
    root["savedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["marketState"] = m_state.toJson();
    root["pricer"] = m_pricer->toJson();
    root["strategy"] = m_strategy->toJson();
    root["scenario"] = m_scenario->toJson();
    root["activeTab"] = m_tabs->currentIndex();
    return root;
}

void MainWindow::loadWorkspaceJson(const QJsonObject& root)
{
    m_state.fromJson(root["marketState"].toObject());
    m_pricer->fromJson(root["pricer"].toObject());
    m_strategy->fromJson(root["strategy"].toObject());
    m_scenario->fromJson(root["scenario"].toObject());
    m_state.notify();
    m_tabs->setCurrentIndex(std::clamp(root["activeTab"].toInt(0), 0, m_tabs->count() - 1));
}

void MainWindow::newWorkspace()
{
    m_state.market = pricing::Market();
    m_state.rateCurve = pricing::RateCurve();
    m_state.useRateCurve = false;
    m_state.chainQuotes.clear();
    m_state.surface = pricing::VolSurface();
    m_pricer->fromJson(QJsonObject());
    m_strategy->fromJson(QJsonObject());
    m_scenario->fromJson(QJsonObject());
    m_state.notify();
    setCurrentPath(QString());
    statusBar()->showMessage("New workspace.", 3000);
}

void MainWindow::openWorkspace()
{
    const QString path = QFileDialog::getOpenFileName(this, "Open workspace", QString(), kWorkspaceFilter);
    if (!path.isEmpty()) {
        openWorkspacePath(path);
    }
}

void MainWindow::openWorkspacePath(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Open failed", QStringLiteral("Could not open %1.").arg(path));
        return;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object()["format"].toString() != "option-pricer-workspace") {
        QMessageBox::warning(this, "Open failed", QStringLiteral("%1 is not an Option Pricer workspace file.").arg(QFileInfo(path).fileName()));
        return;
    }
    loadWorkspaceJson(doc.object());
    setCurrentPath(path);
    addRecentFile(path);
    statusBar()->showMessage(QStringLiteral("Opened %1.").arg(QFileInfo(path).fileName()), 4000);
}

bool MainWindow::saveWorkspace()
{
    if (m_currentPath.isEmpty()) {
        return saveWorkspaceAs();
    }
    QFile file(m_currentPath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Save failed", QStringLiteral("Could not write %1.").arg(m_currentPath));
        return false;
    }
    file.write(QJsonDocument(workspaceJson()).toJson(QJsonDocument::Indented));
    addRecentFile(m_currentPath);
    statusBar()->showMessage(QStringLiteral("Saved %1.").arg(QFileInfo(m_currentPath).fileName()), 4000);
    return true;
}

bool MainWindow::saveWorkspaceAs()
{
    QString path = QFileDialog::getSaveFileName(this, "Save workspace", m_currentPath.isEmpty() ? "workspace.optws" : m_currentPath, kWorkspaceFilter);
    if (path.isEmpty()) {
        return false;
    }
    if (!path.endsWith(".optws", Qt::CaseInsensitive) && !path.endsWith(".json", Qt::CaseInsensitive)) {
        path += ".optws";
    }
    setCurrentPath(path);
    return saveWorkspace();
}

void MainWindow::setCurrentPath(const QString& path)
{
    m_currentPath = path;
    setWindowTitle(path.isEmpty() ? "Option Pricer" : QStringLiteral("Option Pricer — %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::addRecentFile(const QString& path)
{
    QSettings settings;
    QStringList recent = settings.value("files/recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentFiles) {
        recent.removeLast();
    }
    settings.setValue("files/recent", recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QSettings settings;
    const QStringList recent = settings.value("files/recent").toStringList();
    for (const QString& path : recent) {
        if (!QFileInfo::exists(path)) continue;
        m_recentMenu->addAction(QFileInfo(path).fileName(), this, [this, path] { openWorkspacePath(path); })->setToolTip(path);
    }
    if (m_recentMenu->isEmpty()) {
        m_recentMenu->addAction("No recent workspaces")->setEnabled(false);
    } else {
        m_recentMenu->addSeparator();
        m_recentMenu->addAction("Clear Menu", this, [this] {
            QSettings s;
            s.remove("files/recent");
            rebuildRecentMenu();
        });
    }
}

// MARK: - Data

void MainWindow::importChain()
{
    const QString path = QFileDialog::getOpenFileName(this, "Import option chain", QString(), "CSV files (*.csv *.txt);;All files (*)");
    if (path.isEmpty()) return;
    m_tabs->setCurrentWidget(m_chain);
    m_chain->importCsvFile(path);
}

void MainWindow::batchPrice()
{
    const QString path = QFileDialog::getOpenFileName(this, "Batch price contracts", QString(), "CSV files (*.csv *.txt);;All files (*)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Batch pricing failed", QStringLiteral("Could not open %1.").arg(path));
        return;
    }
    const pricing::Inputs defaults = m_pricer->currentInputs();
    const pricing::BatchParseResult parsed = pricing::parseBatchCsv(file.readAll().toStdString(), defaults);
    if (!parsed.error.empty()) {
        QMessageBox::warning(this, "Batch pricing failed",
                             QString::fromStdString(parsed.error) +
                                 "\n\nExpected columns: label (optional), type (call/put), spot, strike, rate, dividend (optional), vol, "
                                 "expiry (years) or days, model (optional: futures), exercise (optional: american). Missing market "
                                 "columns fall back to the Pricer tab's inputs.");
        return;
    }
    BatchDialog dialog(parsed, QFileInfo(path).fileName(), this);
    dialog.exec();
}

QString MainWindow::currentResultsCsv() const
{
    QWidget* current = m_tabs->currentWidget();
    if (current == m_pricer) return m_pricer->resultsCsv();
    if (current == m_strategy) return m_strategy->resultsCsv();
    if (current == m_scenario) return m_scenario->resultsCsv();
    if (current == m_chain) return m_chain->resultsCsv();
    if (current == m_heatmap) return m_heatmap->resultsCsv();
    if (current == m_quotes) return m_quotes->resultsCsv();
    return QString();
}

void MainWindow::exportResults()
{
    const QString csv = currentResultsCsv();
    if (csv.isEmpty()) {
        statusBar()->showMessage("Nothing to export on this tab.", 3000);
        return;
    }
    const QString suggested = m_tabs->tabText(m_tabs->currentIndex()).toLower().replace(' ', '_') + ".csv";
    const QString path = QFileDialog::getSaveFileName(this, "Export as CSV", suggested, "CSV files (*.csv)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export failed", QStringLiteral("Could not write %1.").arg(path));
        return;
    }
    file.write(csv.toUtf8());
    statusBar()->showMessage(QStringLiteral("Exported %1.").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::copyResults()
{
    const QString csv = currentResultsCsv();
    if (csv.isEmpty()) {
        statusBar()->showMessage("Nothing to copy on this tab.", 3000);
        return;
    }
    QGuiApplication::clipboard()->setText(csv);
    statusBar()->showMessage("Copied the current tab's results as CSV.", 3000);
}

void MainWindow::editRateCurve()
{
    RateCurveDialog dialog(m_state.rateCurve, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_state.rateCurve = dialog.curve();
        m_state.useRateCurve = !m_state.rateCurve.empty();
        m_state.notify();
        statusBar()->showMessage(m_state.useRateCurve ? "Rate curve updated and enabled." : "Rate curve cleared; using the flat rate.", 4000);
    }
}

QStringList MainWindow::captureTabs(const QString& directory)
{
    QDir().mkpath(directory);
    m_chain->generateSample();
    m_strategy->loadPreset();
    for (int i = 0; i < m_tabs->count(); ++i) {
        const QSize hint = m_tabs->widget(i)->minimumSizeHint();
        qInfo("[screenshot] tab %-12s minimumSizeHint %dx%d", qPrintable(m_tabs->tabText(i)), hint.width(), hint.height());
    }
    qInfo("[screenshot] window %dx%d minimumSizeHint %dx%d", width(), height(), minimumSizeHint().width(), minimumSizeHint().height());
    QStringList paths;
    const char* names[] = { "quotes", "pricer", "strategy", "scenarios", "chain", "heatmap" };
    for (int i = 0; i < m_tabs->count() && i < 6; ++i) {
        m_tabs->setCurrentIndex(i);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
        if (m_tabs->widget(i) == m_chain) {
            // Exercise the chart hover readouts so the screenshot shows them populated.
            for (QChartView* view : m_chain->findChildren<QChartView*>()) {
                for (QAbstractSeries* series : view->chart()->series()) {
                    if (auto* scatter = qobject_cast<QScatterSeries*>(series); scatter && scatter->count() > 0) {
                        emit scatter->hovered(scatter->at(scatter->count() / 2), true);
                        break;
                    }
                    if (auto* line = qobject_cast<QLineSeries*>(series); line && line->count() > 0 && line->name() == "ATM") {
                        emit line->hovered(line->at(0), true);
                    }
                }
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        }
        const QString path = directory + "/" + names[i] + ".png";
        if (grab().save(path)) {
            paths << path;
        }
    }

    // Round-trip the workspace file format as part of the same smoke test.
    const QString workspacePath = directory + "/workspace.optws";
    QFile file(workspacePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(workspaceJson()).toJson(QJsonDocument::Indented));
        file.close();
        const QJsonObject before = workspaceJson();
        openWorkspacePath(workspacePath);
        const QJsonObject after = workspaceJson();
        if (before["marketState"] == after["marketState"] && before["strategy"] == after["strategy"]
            && before["pricer"] == after["pricer"] && before["scenario"] == after["scenario"]) {
            paths << workspacePath;
        } else {
            qWarning("Workspace round-trip changed the state.");
        }
    }
    return paths;
}

void MainWindow::runLiveSmoke(const QString& ticker)
{
    m_tabs->setCurrentWidget(m_chain);
    m_chain->setTicker(ticker);
    auto step = std::make_shared<int>(0);
    m_chain->onLiveOperationFinished = [this, step](bool ok, const QString& message) {
        qInfo("[live-smoke] step %d %s: %s", *step, ok ? "ok" : "FAILED", qPrintable(message));
        if (!ok) {
            QCoreApplication::exit(1);
            return;
        }
        ++*step;
        if (*step == 1) {
            m_chain->loadTreasuryCurve();
        } else if (*step == 2) {
            m_chain->loadDividends();
        } else {
            const auto& slices = m_state.surface.slices();
            const pricing::ExpirySlice* first = slices.empty() ? nullptr : &slices.front();
            const pricing::ExpirySlice* last = slices.empty() ? nullptr : &slices.back();
            qInfo("[live-smoke] spot %.2f, %zu chain quotes, %zu fitted expiries (%s %d DTE .. %s %d DTE), first ATM vol %.2f%%, %zu curve points, r(1y) %.3f%%, %zu dividends",
                  m_state.market.spot, m_state.chainQuotes.size(), slices.size(),
                  first ? first->expiryDate.c_str() : "-", first ? first->daysToExpiry : 0,
                  last ? last->expiryDate.c_str() : "-", last ? last->daysToExpiry : 0,
                  first ? first->atmVol() * 100.0 : 0.0, m_state.rateCurve.points().size(), m_state.rateFor(1.0) * 100.0,
                  m_state.market.dividends.size());
            for (const pricing::ExpirySlice& s : slices) {
                qInfo("[live-smoke]   %s  %3d DTE  quotes %3zu  fitted %d  ATM %.2f%%  issues %zu", s.expiryDate.c_str(), s.daysToExpiry,
                      s.calls.size() + s.puts.size(), s.fitted, s.atmVol() * 100.0, s.issues.size());
            }
            qInfo("[live-smoke] spot policy: using %.2f (%s); vendor %.2f as of %s; parity-implied %.2f %s",
                  m_state.market.spot, qPrintable(m_state.spotSource), m_state.vendorSpot,
                  qPrintable(m_state.spotAsOf.toString("HH:mm:ss")), m_state.impliedSpot, qPrintable(m_state.impliedSpotNote));
            pricing::ActivityMarket am;
            am.spot = m_state.market.spot;
            am.dividendYield = m_state.market.dividendYield;
            am.rateFor = [this](double t) { return m_state.rateFor(t); };
            const auto top = pricing::mostActiveContracts(m_state.chainQuotes, pricing::ActivityMetric::Volume, pricing::SideFilter::Both, am, 5);
            for (const pricing::ActiveContract& c : top) {
                qInfo("[live-smoke] active: %s %3d DTE K=%.2f %s vol %.0f oi %.0f mid %.2f iv %.1f%% parity gap %.3f", c.quote.expiryDate.c_str(),
                      c.quote.daysToExpiry, c.quote.strike, c.quote.type == pricing::OptionType::Call ? "call" : "put ", c.quote.volume,
                      c.quote.openInterest, c.quote.mid, c.impliedVol * 100.0, c.parityGap);
            }
            const auto parity = pricing::parityByExpiry(m_state.chainQuotes, am);
            for (size_t i = 0; i < parity.size() && i < 5; ++i) {
                const pricing::ParityRow& p = parity[i];
                qInfo("[live-smoke] parity: %s %3d DTE pairs %3d implied fwd %.2f model fwd %.2f gap %+.2f implied yield %.2f%% mean|gap| %.3f",
                      p.expiry.expiryDate.c_str(), p.expiry.daysToExpiry, p.pairs, p.impliedForward, p.modelForward, p.forwardGap, p.impliedYield * 100.0, p.meanAbsGap);
            }
            // Load a chain-driven preset and report its legs.
            m_strategy->loadPreset();
            for (int i = 0; i < m_tabs->count(); ++i) {
                const QSize hint = m_tabs->widget(i)->minimumSizeHint();
                qInfo("[live-smoke] tab %-12s minimumSizeHint %dx%d", qPrintable(m_tabs->tabText(i)), hint.width(), hint.height());
            }
            qInfo("[live-smoke] window %dx%d minimumSizeHint %dx%d", width(), height(), minimumSizeHint().width(), minimumSizeHint().height());
            for (const pricing::Leg& leg : m_strategy->position().legs) {
                qInfo("[live-smoke] leg: %-10s qty %+.0f strike %.2f expiry %s (T %.4f) entry %.2f mid %.2f iv %.1f%%",
                      leg.kind == pricing::LegKind::Call ? "call" : (leg.kind == pricing::LegKind::Put ? "put" : "underlying"),
                      leg.quantity, leg.strike, leg.expiryDate.c_str(), leg.maturity, leg.entryPrice, leg.marketPrice, leg.volatility * 100.0);
            }
            // Render the live-data tabs so the result can be inspected offline.
            const QString shotDir = QDir::tempPath() + "/optshots-live";
            QDir().mkpath(shotDir);
            m_quotes->showTicker(m_chain->ticker());
            for (QWidget* tab : std::initializer_list<QWidget*>{ m_heatmap, m_chain, m_strategy, m_quotes }) {
                m_tabs->setCurrentWidget(tab);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
                if (tab == m_quotes) {
                    // Give the web view time to fetch bars and paint.
                    for (int i = 0; i < 12; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
                }
                const QString path = shotDir + (tab == m_heatmap ? "/heatmap.png" : (tab == m_chain ? "/chain.png" : (tab == m_strategy ? "/strategy.png" : "/quotes.png")));
                if (grab().save(path)) qInfo("[live-smoke] wrote %s", qPrintable(path));
            }
            // The web view cannot be captured by QWidget::grab; ask the chart library for its own image.
            m_quotes->saveChartImage(shotDir + "/chart.png", [](const QString& written) {
                qInfo("[live-smoke] chart image %s", written.isEmpty() ? "FAILED" : qPrintable(written));
                QCoreApplication::exit(written.isEmpty() ? 1 : 0);
            });
        }
    };
    m_chain->fetchLiveChain();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, "About Option Pricer",
                       "<b>Option Pricer</b><br>"
                       "Black-Scholes-Merton and Black-76 closed forms, Cox-Ross-Rubinstein and Bjerksund-Stensland American pricing, "
                       "Crank-Nicolson finite differences and Monte Carlo cross-checks.<br><br>"
                       "Strategy builder with P&amp;L and Greek charts, scenario heatmaps, option-chain implied volatilities with "
                       "raw SVI smile fitting and static-arbitrage checks, zero-rate curves, batch pricing and workspace files.<br><br>"
                       "Built with Qt " QT_VERSION_STR ".");
}
