//
//  MainWindow.h
//  OptionPricing
//
//  Application shell: menu bar, tabbed workspace (Pricer, Strategy, Scenarios, Option
//  Chain), theme switching, workspace save/load, imports and exports.
//

#pragma once

#include "QtHeaders.h"
#include "MarketState.h"
#include "Theme.h"

class PricerTab;
class StrategyTab;
class ScenarioTab;
class ChainTab;
class HeatmapTab;

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

private:
    void buildUi();
    void buildMenus();
    void applyTheme(bool dark);

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
    void importChain();
    void batchPrice();
    void exportResults();
    void copyResults();
    void editRateCurve();
    void showAbout();
    QString currentResultsCsv() const;

    MarketState m_state;
    bool m_darkMode = false;
    QString m_currentPath;

    QTabWidget* m_tabs = nullptr;
    PricerTab* m_pricer = nullptr;
    StrategyTab* m_strategy = nullptr;
    ScenarioTab* m_scenario = nullptr;
    ChainTab* m_chain = nullptr;
    HeatmapTab* m_heatmap = nullptr;
    QPushButton* m_themeToggle = nullptr;
    QLabel* m_subtitle = nullptr;
    QMenu* m_recentMenu = nullptr;
    QAction* m_darkAction = nullptr;
};
