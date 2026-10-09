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

#include <QtWebEngineCore/QWebEnginePage>
#include <QtWebEngineCore/QWebEngineProfile>

#include <functional>

/// Full HTML page including the embedded charting library.
QString chartPageHtml();

/// Web page that forwards the controller's events (drawings changed, tool changed) to C++.
/// The page signals through console messages, which avoids QWebChannel and moc.
class ChartWebPage : public QWebEnginePage
{
public:
    explicit ChartWebPage(QObject* parent = nullptr);

    /// kind is "drawings" (payload: JSON {symbol, items}) or "tool" (payload: tool name).
    std::function<void(const QString& kind, const QString& payload)> onMessage;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString& message, int lineNumber, const QString& sourceID) override;
};
