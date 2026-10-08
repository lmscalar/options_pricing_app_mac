//
//  main.cpp
//  OptionPricing
//
//  Created by Luis Molina on 1/30/23.
//
//  Options analytics workstation with a Qt Widgets front end.
//
//  The pricing library lives in Pricing/ and has no Qt dependency so it can be unit
//  tested on its own. The user interface lives in UI/:
//   - PricerTab    single-contract valuation, Greeks, implied vol, probabilities, engine cross-check
//   - StrategyTab  multi-leg positions with P&L and Greek charts
//   - ScenarioTab  spot x volatility / spot x time heatmaps
//   - ChainTab     option-chain implied vols, SVI smile fit, arbitrage checks, term structure
//   - MainWindow   menus, theme, workspace files, CSV import/export, batch pricing
//

#include "UI/MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Option Pricing");
    QApplication::setOrganizationName("Luis Molina");

    // Fusion renders the custom stylesheet and palettes consistently in both themes.
    if (QStyle* fusion = QStyleFactory::create("Fusion")) {
        QApplication::setStyle(fusion);
    }

    MainWindow window;
    window.show();

    // Developer aid: `OptionPricing --screenshot <dir>` renders every tab to PNG and exits.
    const QStringList args = QCoreApplication::arguments();
    const qsizetype flag = args.indexOf("--screenshot");
    if (flag >= 0 && flag + 1 < args.size()) {
        const QString directory = args.at(flag + 1);
        QTimer::singleShot(800, &window, [&window, directory] {
            const QStringList written = window.captureTabs(directory);
            for (const QString& path : written) {
                qInfo("%s", qPrintable(path));
            }
            QCoreApplication::exit(written.isEmpty() ? 1 : 0);
        });
    }

    // Developer aid: `OptionPricing --live-smoke AAPL` exercises the Massive.com client and exits.
    const qsizetype live = args.indexOf("--live-smoke");
    if (live >= 0 && live + 1 < args.size()) {
        const QString ticker = args.at(live + 1);
        QTimer::singleShot(300, &window, [&window, ticker] { window.runLiveSmoke(ticker); });
    }
    return app.exec();
}
