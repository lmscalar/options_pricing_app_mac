//
//  BatchDialog.h
//  OptionPricing
//
//  Prices a CSV of contracts and shows the results with an option to save them.
//

#pragma once

#include "QtHeaders.h"
#include "../Pricing/Csv.h"

#include <vector>

class BatchDialog : public QDialog
{
public:
    BatchDialog(const pricing::BatchParseResult& parsed, const QString& sourceName, QWidget* parent = nullptr);

private:
    void price(const std::vector<pricing::BatchContract>& contracts);
    void save();

    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QString m_csv;
    QString m_sourceName;
};
