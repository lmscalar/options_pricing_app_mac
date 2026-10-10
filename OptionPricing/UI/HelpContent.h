//
//  HelpContent.h
//  OptionPricing
//
//  In-app user guide: one HTML page per tab ("How to use" steps, "Understanding the
//  analysis" definitions, and for the Pricer an explanation of the option models and
//  numerical methods). Shown by the How to use button in the header for the current tab.
//

#pragma once

#include "QtHeaders.h"

namespace help {

/// Tab names with a help page (the tab texts used in the main window).
QStringList topics();

/// HTML for a tab (matched case-insensitively, substrings allowed); empty when unknown.
QString htmlFor(const QString& tabName);

} // namespace help
