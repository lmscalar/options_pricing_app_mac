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
#include "VolatilityTab.h"
#include "AssistantPanel.h"

#include <QtWidgets/QDockWidget>

#include <QtCore/QElapsedTimer>
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
        restoreState(settings.value("window/state").toByteArray());
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
    m_volatility = new VolatilityTab(m_state, this);
    m_chain->setStore(&m_store);
    // Every ticker entry point funnels through showTicker(): stored chains apply instantly,
    // missing or stale ones are downloaded, and all tabs follow the shared market state.
    m_volatility->onRequestChain = [this](const QString& ticker) { showTicker(ticker, false); };
    m_quotes->onOpenInChain = [this](const QString& ticker) { showTicker(ticker, true); };
    m_quotes->onTickerSelected = [this](const QString& ticker) { showTicker(ticker, false); };
    m_quotes->onWatchlistChanged = [this](const QStringList& watchlist) { m_store.preload(watchlist); };
    m_store.onProgress = [this](const QString& ticker, int done, int total) {
        statusBar()->showMessage(QStringLiteral("Preloading option chains into memory: %1 of %2 (%3)").arg(done).arg(total).arg(ticker), 6000);
    };
    m_store.onIdle = [this] {
        if (m_store.tickerCount() > 0) {
            const bool saved = m_store.saveTo(ChainStore::defaultCachePath());
            statusBar()->showMessage(QStringLiteral("Option chains for %1 tickers in memory: %2 contracts, %3 MB%4.")
                                         .arg(m_store.tickerCount()).arg(m_store.contractCount()).arg(m_store.approximateBytes() / 1048576.0, 0, 'f', 1)
                                         .arg(saved ? QStringLiteral(", saved for the next session") : QString()), 10000);
        }
    };
    m_store.onChainStored = [this](const QString& ticker, bool ok, const QString& message) {
        if (!ok) qWarning("Chain preload failed: %s", qPrintable(message));
        // If this is the ticker the user is looking at and nothing is loaded yet, apply it.
        if (ok && m_chain->ticker() == ticker && m_state.underlyingTicker != ticker) m_chain->applyStoredChain(ticker);
    };
    m_quotes->setStore(&m_store);
    // Restore the previous session's chains and prices from disk so the app is populated at
    // once, then refresh everything older than ten minutes in the background. Each completed
    // preload pass is written back to disk, as is the store on exit.
    const int restored = m_store.loadFrom(ChainStore::defaultCachePath());
    qInfo("[store] restored %d chains (%d contracts, %zu quotes) from %s%s", restored, m_store.contractCount(), m_store.quotes().size(),
          qPrintable(ChainStore::defaultCachePath()), m_store.lastError().isEmpty() ? "" : qPrintable(" · " + m_store.lastError()));
    if (restored > 0) {
        m_quotes->loadStoredQuotes();
        statusBar()->showMessage(QStringLiteral("Restored %1 option chains (%2 contracts) saved %3; refreshing in the background…")
                                     .arg(restored).arg(m_store.contractCount())
                                     .arg(m_store.savedAt().isValid() ? m_store.savedAt().toString("yyyy-MM-dd HH:mm") : QStringLiteral("earlier")), 10000);
    }
    QTimer::singleShot(800, this, [this] { m_store.preload(m_quotes->watchlist(), 10 * 60); });

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
    m_heatmap->onRequestFetch = [this](const QString& ticker) { showTicker(ticker, false); };
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
    m_tabs->addTab(m_volatility, "Volatility");

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
    // Assistant toggle: a checkable icon button bound to the dock's view action, so the
    // dock's own close button and the menu item keep it in sync.
    m_assistantToggle = new QToolButton(this);
    m_assistantToggle->setObjectName("assistantToggle");
    m_assistantToggle->setText("✦ Assistant");
    m_assistantToggle->setCursor(Qt::PointingHandCursor);
    m_assistantToggle->setToolTip("Show or hide the AI assistant panel (⌘⇧A). Hiding it gives the tabs the full width.");
    m_assistantBusy = new ui::SpinningDiamond(this, 20);
    m_assistantBusy->setToolTip("The assistant is thinking…");
    header->addWidget(m_assistantBusy, 0, Qt::AlignRight);
    header->addWidget(m_assistantToggle, 0, Qt::AlignRight);

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
    buildAssistant();
    // Keep the button's own label; mirror the dock's view action both ways.
    QAction* dockAction = m_assistantDock->toggleViewAction();
    m_assistantToggle->setCheckable(true);
    m_assistantToggle->setChecked(dockAction->isChecked());
    connect(m_assistantToggle, &QToolButton::clicked, dockAction, &QAction::trigger);
    connect(dockAction, &QAction::toggled, m_assistantToggle, &QToolButton::setChecked);
    m_assistant->onBusyChanged = [this](bool busy) {
        if (busy) m_assistantBusy->start();
        else m_assistantBusy->stop();
    };
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

    QMenu* assistant = menuBar()->addMenu("&Assistant");
    QAction* toggleDock = m_assistantDock->toggleViewAction();
    toggleDock->setText("Show &Assistant Panel");
    toggleDock->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    assistant->addAction(toggleDock);
    assistant->addAction("Analyze This &Screen", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L), this, [this] {
        m_assistantDock->show();
        m_assistant->analyzeScreen();
    });
    assistant->addAction("&Listen (Dictate)", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this, [this] {
        m_assistantDock->show();
        m_assistant->toggleListening();
    });
    assistant->addAction("Ask the Assistant…", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), this, [this] {
        m_assistantDock->show();
        m_assistant->focusInput();
    });
    assistant->addSeparator();
    assistant->addAction("Set AI &Key…", this, [this] { m_assistant->promptForApiKey(); });

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
    m_volatility->applyTheme(theme);
    m_assistant->applyTheme(theme);
    m_assistantBusy->setColor(QColor(theme.accent3.isEmpty() ? "#22d3ee" : theme.accent3));

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
    settings.setValue("window/state", saveState());
    settings.setValue("ai.panelVisible", m_assistantDock && m_assistantDock->isVisible());
    // Persist the in-memory chains and prices so the next launch starts populated.
    if (m_store.tickerCount() > 0 && !m_store.saveTo(ChainStore::defaultCachePath())) {
        qWarning("Could not save the option chain store: %s", qPrintable(m_store.lastError()));
    }
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
    if (current == m_volatility) return m_volatility->resultsCsv();
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
    m_volatility->loadSample();
    {
        // Typed requests through the real input widgets (local commands, so no AI key is needed):
        // Enter key, then the Send button.
        m_assistantDock->show();
        m_assistant->debugTypeAndSend("switch to the volatility tab", false);
        qInfo("[screenshot] typed + Enter -> current tab %s; transcript: %s", qPrintable(tabNameOf(m_tabs->currentWidget())), qPrintable(m_assistant->debugLastTranscriptLine()));
        m_assistant->debugTypeAndSend("switch to the quotes tab", true);
        qInfo("[screenshot] typed + Send  -> current tab %s; transcript: %s", qPrintable(tabNameOf(m_tabs->currentWidget())), qPrintable(m_assistant->debugLastTranscriptLine()));
        // Rendering of an analyst-style note (tables, headings, lists, callout).
        m_assistant->debugRenderSample();
        for (int i = 0; i < 2; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (grab().save(directory + "/assistant-sample.png")) qInfo("[screenshot] wrote assistant-sample.png");
        m_assistant->debugScrollTranscriptToTop();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (grab().save(directory + "/assistant-sample-top.png")) qInfo("[screenshot] wrote assistant-sample-top.png");
        // Thinking indicators: capture a frame with them spinning.
        m_assistant->debugSetBusy(true);
        for (int i = 0; i < 4; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (grab().save(directory + "/assistant-busy.png")) qInfo("[screenshot] wrote assistant-busy.png (header diamond visible: %s)", m_assistantBusy->isVisible() ? "yes" : "no");
        m_assistant->debugSetBusy(false);
    }
    {
        // Store persistence smoke: save whatever is in memory to the screenshot folder and read it back.
        const QString scratch = directory + "/chains-roundtrip.sqlite";
        const bool saved = m_store.saveTo(scratch);
        ChainStore copy;
        const int restored = copy.loadFrom(scratch);
        qInfo("[screenshot] store save %s%s, %lld KB; reloaded %d chains, %d contracts, %zu quotes%s", saved ? "ok" : "FAILED",
              saved ? "" : qPrintable(" (" + m_store.lastError() + ")"), static_cast<long long>(QFileInfo(scratch).size() / 1024), restored, copy.contractCount(),
              copy.quotes().size(), copy.lastError().isEmpty() ? "" : qPrintable(" (" + copy.lastError() + ")"));
    }
    for (int i = 0; i < m_tabs->count(); ++i) {
        const QSize hint = m_tabs->widget(i)->minimumSizeHint();
        qInfo("[screenshot] tab %-12s minimumSizeHint %dx%d", qPrintable(m_tabs->tabText(i)), hint.width(), hint.height());
    }
    qInfo("[screenshot] window %dx%d minimumSizeHint %dx%d", width(), height(), minimumSizeHint().width(), minimumSizeHint().height());
    QStringList paths;
    const char* names[] = { "quotes", "pricer", "strategy", "scenarios", "chain", "heatmap", "volatility" };
    for (int i = 0; i < m_tabs->count() && i < 7; ++i) {
        m_tabs->setCurrentIndex(i);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
        if (m_tabs->widget(i) == m_chain || m_tabs->widget(i) == m_volatility) {
            // Exercise the chart hover readouts so the screenshot shows them populated.
            for (QChartView* view : m_tabs->widget(i)->findChildren<QChartView*>()) {
                for (QAbstractSeries* series : view->chart()->series()) {
                    if (auto* scatter = qobject_cast<QScatterSeries*>(series); scatter && scatter->count() > 0) {
                        emit scatter->hovered(scatter->at(scatter->count() / 2), true);
                        break;
                    }
                    if (auto* line = qobject_cast<QLineSeries*>(series); line && line->count() > 0 && (line->name() == "ATM" || m_tabs->widget(i) == m_volatility)) {
                        emit line->hovered(line->at(line->count() / 2), true);
                        if (m_tabs->widget(i) == m_volatility) break;
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
    // The volatility tab reports through the same callback chain as the chain operations.
    m_volatility->onFetchFinished = [this](bool ok, const QString& message) {
        if (m_chain->onLiveOperationFinished) m_chain->onLiveOperationFinished(ok, message);
    };
    m_chain->onLiveOperationFinished = [this, step, ticker](bool ok, const QString& message) {
        qInfo("[live-smoke] step %d %s: %s", *step, ok ? "ok" : "FAILED", qPrintable(message));
        if (!ok) {
            QCoreApplication::exit(1);
            return;
        }
        ++*step;
        if (*step > 6) return;   // the final stage below runs once; later completions are ignored
        if (*step == 1) {
            m_chain->loadTreasuryCurve();
        } else if (*step == 2) {
            m_chain->loadDividends();
        } else if (*step == 3) {
            m_volatility->setTicker(ticker);
            m_volatility->fetchHistory();
        } else if (*step == 4) {
            qInfo("[live-smoke] volatility: %s", qPrintable(m_volatility->summaryText()));
            // Reverse cascade: a ticker entered on the Volatility tab must pull the option
            // chain (and through it the other tabs). Two completions follow: bars and chain.
            const QString other = ticker == "AAPL" ? QStringLiteral("MSFT") : QStringLiteral("AAPL");
            qInfo("[live-smoke] reverse cascade: fetching %s from the Volatility tab", qPrintable(other));
            m_volatility->setTicker(other);
            m_volatility->fetchAll();
        } else if (*step == 5) {
            qInfo("[live-smoke] reverse cascade: first half done, waiting for the second");
        } else {
            qInfo("[live-smoke] reverse cascade result: chain ticker %s (%zu quotes), state ticker %s, volatility ticker %s, quotes watchlist has it: %s",
                  qPrintable(m_chain->ticker()), m_state.chainQuotes.size(), qPrintable(m_state.underlyingTicker), qPrintable(m_volatility->ticker()),
                  m_quotes->hasTicker(m_state.underlyingTicker) ? "yes" : "no");
            qInfo("[live-smoke] volatility: %s", qPrintable(m_volatility->summaryText()));
            {
                QElapsedTimer clock;
                clock.start();
                const bool served = m_chain->applyStoredChain(m_chain->ticker());
                qInfo("[live-smoke] chain store: %d tickers, %d contracts, %.1f MB, %d downloads pending; re-applied %s from memory in %lld ms (%s)",
                      m_store.tickerCount(), m_store.contractCount(), m_store.approximateBytes() / 1048576.0, m_store.pending(),
                      qPrintable(m_chain->ticker()), static_cast<long long>(clock.elapsed()), served ? "ok" : "not stored");
                for (const ChainStore::Summary& row : m_store.summaries()) {
                    qInfo("[live-smoke]   stored %-6s %5d contracts %3d expiries spot %8.2f at %s", qPrintable(row.ticker), row.contracts, row.expiries, row.spot,
                          qPrintable(row.fetchedAt.toString("HH:mm:ss")));
                }
                // Persistence round trip into a scratch file and a fresh store (the user's cache is untouched).
                QDir().mkpath(QDir::tempPath() + "/optshots-live");
                const QString scratch = QDir::tempPath() + "/optshots-live/chains-roundtrip.sqlite";
                QElapsedTimer saveClock;
                saveClock.start();
                const bool saved = m_store.saveTo(scratch);
                const qint64 saveMs = saveClock.elapsed();
                ChainStore copy;
                QElapsedTimer loadClock;
                loadClock.start();
                const int restored = copy.loadFrom(scratch);
                qInfo("[live-smoke] persistence: saved %s%s in %lld ms (%lld KB); fresh store restored %d chains, %d contracts, %zu quotes in %lld ms; %s",
                      saved ? "ok" : "FAILED", saved ? "" : qPrintable(" (" + m_store.lastError() + ")"), static_cast<long long>(saveMs), static_cast<long long>(QFileInfo(scratch).size() / 1024), restored, copy.contractCount(),
                      copy.quotes().size(), static_cast<long long>(loadClock.elapsed()),
                      (restored == m_store.tickerCount() && copy.contractCount() == m_store.contractCount()) ? "counts match" : "COUNTS DIFFER");
                // Also write the real cache so the next launch can be checked for a restore.
                qInfo("[live-smoke] persistence: cache %s written to %s", m_store.saveTo(ChainStore::defaultCachePath()) ? "ok" : "FAILED", qPrintable(ChainStore::defaultCachePath()));
            }
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
            for (QWidget* tab : std::initializer_list<QWidget*>{ m_heatmap, m_chain, m_strategy, m_volatility, m_quotes }) {
                m_tabs->setCurrentWidget(tab);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
                if (tab == m_quotes) {
                    // Give the web view time to fetch bars and paint.
                    for (int i = 0; i < 12; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
                }
                const QString path = shotDir + (tab == m_heatmap ? "/heatmap.png" : (tab == m_chain ? "/chain.png" : (tab == m_strategy ? "/strategy.png" : (tab == m_volatility ? "/volatility.png" : "/quotes.png"))));
                if (grab().save(path)) qInfo("[live-smoke] wrote %s", qPrintable(path));
            }
            // Exercise the drawing tools through the page's mouse handlers, then export the
            // chart (the web view cannot be captured by QWidget::grab; the library renders it,
            // with the drawing overlay composited on top).
            auto runDrawingTests = [this, shotDir] {
            m_quotes->debugSimulateDrawings([this, shotDir](int count) {
                qInfo("[live-smoke] drawings placed through the page: %d", count);
                m_quotes->debugSimulateContextDelete([](int remaining) {
                    qInfo("[live-smoke] right-click delete on a drawing: %s (%d remaining)", remaining >= 0 ? "ok" : "FAILED", remaining);
                });
                QTimer::singleShot(600, this, [this, shotDir] {
                    qInfo("[live-smoke] drawings persisted for %s: %s", qPrintable(m_chain->ticker()), m_quotes->hasStoredDrawings(m_chain->ticker()) ? "yes" : "no");
                    // Reset the view (bars re-sent, autoscale restored) before exporting; drawings must survive.
                    m_quotes->resetChart();
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 300);
                    qInfo("[live-smoke] chart reset; drawings after reset: %s", qPrintable(m_quotes->drawingsJson().left(200)));
                    m_quotes->saveChartImage(shotDir + "/chart.png", [this](const QString& written) {
                        qInfo("[live-smoke] chart image %s", written.isEmpty() ? "FAILED" : qPrintable(written));
                        // Put the user's own drawings back (the test ones were only stashed over them).
                        m_quotes->debugRestoreDrawings([written](int count) {
                            qInfo("[live-smoke] restored %d user drawings", count);
                            QTimer::singleShot(300, [written] { QCoreApplication::exit(written.isEmpty() ? 1 : 0); });
                        });
                    });
                });
            });
            };   // runDrawingTests

            // Assistant: a real turn against the API when a key is available. Local commands
            // are checked first (no key needed), then the model is asked to annotate the chart.
            {
                QString feedback;
                const bool handledChain = handleLocalCommand(QStringLiteral("pull up option chains for %1").arg(m_chain->ticker()), feedback);
                qInfo("[live-smoke] local command (chain): %s -> %s", handledChain ? "handled" : "NOT handled", qPrintable(feedback));
                const bool handledTab = handleLocalCommand("switch to the volatility tab", feedback);
                qInfo("[live-smoke] local command (tab): %s -> %s (current tab %s)", handledTab ? "handled" : "NOT handled", qPrintable(feedback), qPrintable(tabNameOf(m_tabs->currentWidget())));
                const bool handledOther = handleLocalCommand("what is the implied volatility skew here", feedback);
                qInfo("[live-smoke] local command (free text): %s (expected not handled)", handledOther ? "handled" : "not handled");
            }
            // Quote synchronisation: headline, watchlist row and chart legend must agree.
            {
                const QString symbol = m_state.underlyingTicker;
                const auto& bars = m_quotes->bars().bars;
                qInfo("[live-smoke] quote sync: headline %s %.2f (%+.2f, %+.2f%%) prev close %.2f via %s; watchlist row: %s; chart bars %zu, last bar close %.2f, bar before %.2f",
                      qPrintable(symbol), m_state.market.spot, m_state.dayChange(), m_state.dayChangePercent(), m_state.previousClose, qPrintable(m_state.spotSource),
                      qPrintable(m_quotes->debugRowText(symbol)), bars.size(), bars.empty() ? 0.0 : bars.back().close, bars.size() > 1 ? bars[bars.size() - 2].close : 0.0);
                m_quotes->debugLegendText([](const QString& legend) {
                    QString oneLine = legend;
                    oneLine.replace('\n', ' ');
                    qInfo("[live-smoke] quote sync: chart legend: %s", qPrintable(oneLine.left(300)));
                });
            }
            // OPTION_PRICER_AI="OpenAI/gpt-4.1-mini" or "Ollama/llama3.2:latest" selects the provider under test.
            const QString aiOverride = qEnvironmentVariable("OPTION_PRICER_AI");
            if (!aiOverride.isEmpty()) {
                m_assistant->client().setPersistSelection(false);   // a test selection must not change the user's preference
                const QString providerName = aiOverride.section('/', 0, 0).trimmed().toLower();
                const QString model = aiOverride.section('/', 1).trimmed();
                m_assistant->client().setProvider(providerName == "openai" ? AssistantClient::Provider::OpenAI
                                                  : (providerName.startsWith("ollama") ? AssistantClient::Provider::Ollama : AssistantClient::Provider::Anthropic));
                if (!model.isEmpty()) m_assistant->client().setModel(model);
                qInfo("[live-smoke] assistant provider override: %s / %s", qPrintable(AssistantClient::providerName(m_assistant->client().provider())), qPrintable(m_assistant->client().model()));
            }
            if (m_assistant->client().hasApiKey()) {
                m_tabs->setCurrentWidget(m_quotes);
                auto finished = std::make_shared<bool>(false);
                auto aiClock = std::make_shared<QElapsedTimer>();
                aiClock->start();
                m_assistant->onReply = [this, shotDir, runDrawingTests, finished, aiClock](const QString& reply, bool ok) {
                    if (*finished) return;
                    *finished = true;
                    QString oneLine = reply;
                    oneLine.replace('\n', ' ');
                    qInfo("[live-smoke] assistant %s in %lld ms (%s, %d in / %d out tokens): %s", ok ? "replied" : "FAILED", static_cast<long long>(aiClock->elapsed()),
                          qPrintable(m_assistant->client().model()), m_assistant->client().lastInputTokens(), m_assistant->client().lastOutputTokens(), qPrintable(oneLine.left(700)));
                    QTimer::singleShot(500, this, [this, shotDir, runDrawingTests] {
                        qInfo("[live-smoke] assistant drawings now: %s", qPrintable(m_quotes->drawingsJson().left(500)));
                        m_assistantDock->show();
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
                        if (grab().save(shotDir + "/assistant-reply.png")) qInfo("[live-smoke] wrote assistant-reply.png");
                        m_quotes->saveChartImage(shotDir + "/chart-ai.png", [this, runDrawingTests](const QString& written) {
                            qInfo("[live-smoke] assistant chart image %s", written.isEmpty() ? "FAILED" : qPrintable(written));
                            m_quotes->clearDrawings();   // the model's test drawings are not kept
                            runDrawingTests();
                        });
                    });
                };
                QTimer::singleShot(150000, this, [finished, runDrawingTests] {
                    if (!*finished) { *finished = true; qInfo("[live-smoke] assistant FAILED: timed out"); runDrawingTests(); }
                });
                // Set the user's drawings aside so the model starts from a clean chart; the
                // drawing tests restore them at the very end.
                m_quotes->debugStashDrawings([this](int stashed) {
                    qInfo("[live-smoke] stashed %d user drawings before the assistant turn", stashed);
                    m_assistant->submit("Mark the two or three most important support and resistance zones on this chart and draw the dominant trend line. Reply in under 80 words.");
                });
            } else {
                qInfo("[live-smoke] assistant: no ANTHROPIC_API_KEY, skipping the model turn");
                runDrawingTests();
            }
        }
    };
    m_chain->fetchLiveChain();
}

void MainWindow::showTicker(const QString& rawSymbol, bool switchToChainTab)
{
    const QString symbol = rawSymbol.trimmed().toUpper();
    if (symbol.isEmpty()) return;
    qInfo("[ticker] showTicker %s (stored: %s, chain tab busy: %s)", qPrintable(symbol), m_store.contains(symbol) ? "yes" : "no", m_chain->isBusy() ? "yes" : "no");
    constexpr qint64 kStaleSeconds = 10 * 60;
    if (switchToChainTab) m_tabs->setCurrentWidget(m_chain);
    m_chain->setTicker(symbol);
    // Store-served switches report through the same completion callback as live downloads
    // (status bar message; the live smoke counts on it).
    auto report = [this, symbol, kStaleSeconds](bool refreshing) {
        const qint64 age = m_store.ageSeconds(symbol);
        const QString message = QStringLiteral("%1: %2 contracts applied from memory (downloaded %3 min ago)%4.")
                                    .arg(symbol).arg(m_state.chainQuotes.size()).arg(age / 60)
                                    .arg(refreshing ? QStringLiteral(", refreshing in the background") : QString());
        if (m_chain->onLiveOperationFinished) m_chain->onLiveOperationFinished(true, message);
        else statusBar()->showMessage(message, 8000);
    };
    if (m_chain->applyStoredChain(symbol)) {
        const bool stale = m_store.ageSeconds(symbol) > kStaleSeconds;
        report(stale);
        if (stale) {
            m_store.refresh(symbol, [this, symbol](bool ok, const QString&) {
                if (ok && m_chain->ticker() == symbol) m_chain->applyStoredChain(symbol);
            });
        }
        return;
    }
    if (!m_store.hasApiKey()) {
        m_chain->fetchLiveChain();   // prompts for a key, then downloads directly
        return;
    }
    statusBar()->showMessage(QStringLiteral("Downloading %1 option chain from Massive.com…").arg(symbol), 8000);
    m_store.refresh(symbol, [this, symbol, report](bool ok, const QString& message) {
        if (ok && m_chain->ticker() == symbol) {
            if (m_chain->applyStoredChain(symbol)) report(false);
        } else if (!ok) {
            if (m_chain->onLiveOperationFinished) m_chain->onLiveOperationFinished(false, message);
            else statusBar()->showMessage(message, 15000);
        }
    });
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
