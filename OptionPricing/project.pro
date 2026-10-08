# qmake project file for building the application outside Xcode (macOS, Linux, Windows).
#   qmake project.pro && make
# Requires Qt 6 with the Widgets, Charts and Network modules.

QT += core gui widgets charts network
CONFIG += c++20
TEMPLATE = app
TARGET = OptionPricing

SOURCES += \
    main.cpp \
    UI/BatchDialog.cpp \
    UI/ChainTab.cpp \
    UI/HeatmapTab.cpp \
    UI/MainWindow.cpp \
    UI/MarketDataClient.cpp \
    UI/MarketState.cpp \
    UI/PricerTab.cpp \
    UI/RateCurveDialog.cpp \
    UI/ScenarioTab.cpp \
    UI/StrategyTab.cpp \
    UI/Theme.cpp \
    UI/Widgets.cpp

HEADERS += \
    Pricing/Activity.h \
    Pricing/American.h \
    Pricing/BlackScholes.h \
    Pricing/Csv.h \
    Pricing/DayCount.h \
    Pricing/FiniteDifference.h \
    Pricing/MonteCarlo.h \
    Pricing/Normal.h \
    Pricing/RateCurve.h \
    Pricing/Scenario.h \
    Pricing/Strategy.h \
    Pricing/VolSurface.h \
    UI/BatchDialog.h \
    UI/ChainTab.h \
    UI/Formatting.h \
    UI/HeatmapTab.h \
    UI/MainWindow.h \
    UI/MarketDataClient.h \
    UI/MarketState.h \
    UI/PricerTab.h \
    UI/QtHeaders.h \
    UI/RateCurveDialog.h \
    UI/ScenarioTab.h \
    UI/StrategyTab.h \
    UI/Theme.h \
    UI/Widgets.h
