//
//  RateCurveDialog.h
//  OptionPricing
//
//  Editor for the zero-rate term structure.
//

#pragma once

#include "QtHeaders.h"
#include "../Pricing/RateCurve.h"

class RateCurveDialog : public QDialog
{
public:
    RateCurveDialog(const pricing::RateCurve& curve, QWidget* parent = nullptr);

    pricing::RateCurve curve() const;

private:
    void addRow(double tenor, double ratePercent);
    void loadSample();

    QTableWidget* m_table = nullptr;
};
