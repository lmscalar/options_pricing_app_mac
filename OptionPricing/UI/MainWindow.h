//
//  MainWindow.h
//  OptionPricing
//
//  Application shell: menu bar, tabbed workspace (Pricer, Strategy, Scenarios, Option
//  Chain), theme switching, workspace save/load, imports and exports.
//

#pragma once

#include "QtHeaders.h"
#include "AssistantClient.h"
#include "ChainStore.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"

class PricerTab;
class StrategyTab;
class ScenarioTab;
class ChainTab;
class HeatmapTab;
class SectorHeatmapTab;
class PortfolioTab;
class AlertsTab;
class ScannerTab;
class OptimizerTab;
class QuotesTab;
class VolatilityTab;
class ChartPopup;
class AssistantPanel;
class QDockWidget;

class MainWindow : public QMainWindow
{
public:
    MainWindow();

    /// Developer aid: populates the tabs with sample data, renders each one to a PNG in
    /// `directory`, and returns the written paths. Used by the --screenshot flag.
    QStringList captureTabs(const QString& directory);

    /// Developer aid: downloads a live chain, the Treasury curve and dividends for
    /// `ticker` from Massive.com, prints a summary and quits. Used by --live-smoke.
    void runLiveSmoke(const QString& ticker);

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void buildMenus();
    void applyTheme(bool dark);

    // Assistant (MainWindowAssistant.cpp)
    void buildAssistant();
    std::vector<AssistantClient::Tool> assistantTools() const;
    void executeAssistantTool(const QString& name, const QJsonObject& input, AssistantClient::ToolDone done);
    void assistantContext(std::function<void(const QString& text, const QImage& image)> done);
    bool handleLocalCommand(const QString& text, QString& feedback);
    QString chainSummary(int maxExpiries) const;
    QString tabNameOf(QWidget* tab) const;
    QWidget* tabByName(const QString& name) const;

    // Workspace
    void newWorkspace();
    void openWorkspace();
    void openWorkspacePath(const QString& path);
    bool saveWorkspace();
    bool saveWorkspaceAs();
    QJsonObject workspaceJson() const;
    void loadWorkspaceJson(const QJsonObject& json);
    void setCurrentPath(const QString& path);
    void addRecentFile(const QString& path);
    void rebuildRecentMenu();

    // Data
    /// Makes `symbol` the app-wide ticker: applies its chain from the in-memory store when
    /// present (refreshing stale ones in the background) or downloads it, then every tab
    /// follows through the shared market state.
    void showTicker(const QString& symbol, bool switchToChainTab);
    /// Shows `symbol` in the Quotes chart and opens the pop-out chart window with a copy of it
    /// (used when a stock is picked on the Sector Heatmap).
    void showChartPopup(const QString& symbol);
    void importChain();
    void batchPrice();
    void exportResults();
    void copyResults();
    void editRateCurve();
    void showAbout();
    QString currentResultsCsv() const;

    MarketState m_state;
    ChainStore m_store;                ///< in-memory SQLite database of watchlist option chains
    bool m_darkMode = false;
    QString m_currentPath;

    QTabWidget* m_tabs = nullptr;
    PricerTab* m_pricer = nullptr;
    StrategyTab* m_strategy = nullptr;
    ScenarioTab* m_scenario = nullptr;
    ChainTab* m_chain = nullptr;
    HeatmapTab* m_heatmap = nullptr;
    SectorHeatmapTab* m_sectorHeatmap = nullptr;
    ChartPopup* m_chartPopup = nullptr;        ///< pop-out copy of the Quotes chart, created on first use
    PortfolioTab* m_portfolio = nullptr;
    AlertsTab* m_alerts = nullptr;
    ScannerTab* m_scanner = nullptr;
    OptimizerTab* m_optimizer = nullptr;
    QFrame* m_alertBanner = nullptr;        ///< in-app toast shown when an alert fires (top-right of the central area)
    QLabel* m_alertBannerLabel = nullptr;
    QTimer* m_alertBannerTimer = nullptr;
    QString m_alertBannerTicker;
    /// Shows the alert toast for ten seconds; clicking it loads the ticker everywhere.
    void showAlertBanner(const QString& ticker, const QString& message);
    bool eventFilter(QObject* watched, QEvent* event) override;
    QuotesTab* m_quotes = nullptr;
    VolatilityTab* m_volatility = nullptr;
    AssistantPanel* m_assistant = nullptr;
    QDockWidget* m_assistantDock = nullptr;
    QVBoxLayout* m_rootLayout = nullptr;   ///< central layout; its outer margin shrinks on the side the assistant dock occupies
    /// Narrows the central margin next to a visible assistant dock so the chart and the chat sit close together.
    void updateCentralMargins();
    QPushButton* m_themeToggle = nullptr;
    QPushButton* m_helpButton = nullptr;   ///< "How to use": the guide page for the current tab
    /// Opens the in-app guide for the tab on screen (Help menu, header button, F1).
    void showHelpForCurrentTab();
    QToolButton* m_assistantToggle = nullptr;
    ui::SpinningDiamond* m_assistantBusy = nullptr;   ///< spins in the header while the assistant works

    // Quote banner in the header: ticker, price, daily change, provenance
    QFrame* m_banner = nullptr;
    QLabel* m_bannerLogo = nullptr;
    QLabel* m_bannerName = nullptr;
    QLabel* m_bannerSymbol = nullptr;
    QLabel* m_bannerPrice = nullptr;
    QLabel* m_bannerChange = nullptr;
    QLabel* m_bannerMeta = nullptr;
    QString m_bannerMetaFull;
    QString m_bannerNameFull;
    void updateBanner();
    /// Elides the banner's name and meta text to the space available so the header never
    /// grows wider than the window.
    void elideBanner();
    QMenu* m_recentMenu = nullptr;
    QAction* m_darkAction = nullptr;
};
