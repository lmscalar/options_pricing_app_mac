//
//  ChartPage.h
//  OptionPricing
//
//  The HTML document hosted by the Quotes tab's web view: TradingView Lightweight Charts
//  plus a small controller exposing `chartApi` (init, setTheme, setBars, setOptions) that
//  the C++ side drives through runJavaScript.
//

#pragma once

#include "QtHeaders.h"

/// Full HTML page including the embedded charting library.
QString chartPageHtml();
