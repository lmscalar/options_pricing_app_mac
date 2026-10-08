//
//  ScenarioTab.h
//  OptionPricing
//
//  Scenario grid: a chosen metric for the strategy position (or the pricer's single
//  option) across spot on the rows and volatility shift or elapsed days on the columns,
//  rendered as a colour-coded table.
//

#pragma once

#include "QtHeaders.h"
#include "MarketState.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Pricing/Scenario.h"

#include <functional>

class ScenarioTab : public QWidget
{
public:
    using PositionProvider = std::function<pricing::Position()>;
    using InputsProvider = std::function<pricing::Inputs()>;

    ScenarioTab(MarketState& state, PositionProvider positionProvider, InputsProvider inputsProvider, QWidget* parent = nullptr);

    void refresh();
    void applyTheme(const Theme& theme);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& json);
    QString resultsCsv() const;

private:
    void buildUi();
    void wire();
    pricing::Position sourcePosition() const;
    pricing::ScenarioRequest request() const;

    MarketState& m_state;
    PositionProvider m_positionProvider;
    InputsProvider m_inputsProvider;
    Theme m_theme;
    pricing::ScenarioGrid m_grid;
    bool m_updating = false;

    QComboBox* m_source = nullptr;
    QComboBox* m_metric = nullptr;
    QComboBox* m_axis = nullptr;
    QDoubleSpinBox* m_spotRange = nullptr;
    QSpinBox* m_rows = nullptr;
    QSpinBox* m_columns = nullptr;
    QDoubleSpinBox* m_volRange = nullptr;
    QSpinBox* m_maxDays = nullptr;
    QLabel* m_volRangeLabel = nullptr;
    QLabel* m_maxDaysLabel = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QPushButton* m_copy = nullptr;
};
