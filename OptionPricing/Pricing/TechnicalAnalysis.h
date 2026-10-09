//
//  TechnicalAnalysis.h
//  OptionPricing
//
//  Qt-free wrapper around TA-Lib's abstract interface. The library describes every one of
//  its ~160 functions (name, group, parameters with ranges and defaults, outputs with
//  display hints), so the catalogue below is read from TA-Lib itself rather than typed by
//  hand, and any function can be evaluated generically on a bar series. Groups become the
//  indicator categories shown in the application: Overlap Studies, Momentum Indicators,
//  Volume Indicators, Volatility Indicators, Price Transform, Cycle Indicators, Pattern
//  Recognition and Statistic Functions (the arithmetic groups are left out).
//
//  Requires the TA-Lib C library (Homebrew: brew install ta-lib).
//

#pragma once

#include <ta-lib/ta_libc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ta {

/// One optional (tunable) parameter of a function.
struct ParamInfo {
    enum class Kind { Integer, Real, Choice };
    std::string name;          ///< TA-Lib parameter name, e.g. "optInTimePeriod"
    std::string displayName;   ///< e.g. "Time Period"
    std::string hint;
    Kind kind = Kind::Integer;
    double defaultValue = 0.0;
    double min = 0.0;
    double max = 0.0;
    int precision = 0;         ///< decimals for Real parameters
    bool advanced = false;     ///< rarely changed (TA_OPTIN_ADVANCED)
    bool percent = false;
    std::vector<std::pair<int, std::string>> choices;   ///< for Kind::Choice (e.g. MA types)
};

/// One output line of a function with TA-Lib's display suggestion.
struct OutputInfo {
    enum class Style { Line, DashLine, DotLine, Dots, Histogram, Pattern };
    std::string name;          ///< e.g. "outMACDSignal"
    std::string shortName;     ///< e.g. "MACDSignal"
    bool integer = false;
    Style style = Style::Line;
    bool upperLimit = false;
    bool lowerLimit = false;
    bool canBeNegative = false;
    bool hasZero = false;
    int flags = 0;
};

struct FunctionInfo {
    std::string name;          ///< e.g. "RSI"
    std::string group;         ///< e.g. "Momentum Indicators"
    std::string hint;          ///< e.g. "Relative Strength Index"
    bool overlay = false;      ///< drawn on the price scale (TA_FUNC_FLG_OVERLAP)
    bool volume = false;       ///< drawn on the volume scale
    bool candlestick = false;  ///< candlestick pattern (integer +/-100 outputs)
    bool unstablePeriod = false;
    unsigned realInputs = 0;   ///< number of plain real-series inputs (fed with the close)
    unsigned priceInputs = 0;
    int priceFlags = 0;        ///< TA_IN_PRICE_* components required
    std::vector<ParamInfo> params;
    std::vector<OutputInfo> outputs;
    const TA_FuncHandle* handle = nullptr;

    bool usesVolume() const { return (priceFlags & TA_IN_PRICE_VOLUME) != 0; }
    /// "RSI – Relative Strength Index"
    std::string displayName() const { return hint.empty() ? name : name + " \xE2\x80\x93 " + hint; }
};

struct Catalog {
    std::vector<std::string> groups;                 ///< in TA-Lib's order, arithmetic groups removed
    std::map<std::string, FunctionInfo> functions;   ///< by name

    const FunctionInfo* find(const std::string& name) const
    {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const auto it = functions.find(key);
        return it == functions.end() ? nullptr : &it->second;
    }
    std::vector<const FunctionInfo*> inGroup(const std::string& group) const
    {
        std::vector<const FunctionInfo*> out;
        for (const auto& [name, info] : functions) {
            if (info.group == group) out.push_back(&info);
        }
        return out;
    }
    /// Case-insensitive search over name and description, e.g. "boll" finds BBANDS.
    std::vector<const FunctionInfo*> search(const std::string& text) const
    {
        std::string needle = text;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<const FunctionInfo*> out;
        for (const auto& [name, info] : functions) {
            std::string hay = info.name + " " + info.hint;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(needle) != std::string::npos) out.push_back(&info);
        }
        return out;
    }
};

namespace detail {

inline bool ensureInitialised()
{
    static std::once_flag flag;
    static bool ok = false;
    std::call_once(flag, [] { ok = TA_Initialize() == TA_SUCCESS; });
    return ok;
}

inline bool excludedGroup(const std::string& group)
{
    return group == "Math Operators" || group == "Math Transform";
}

inline OutputInfo::Style styleFromFlags(int flags)
{
    if (flags & (TA_OUT_PATTERN_BOOL | TA_OUT_PATTERN_BULL_BEAR | TA_OUT_PATTERN_STRENGTH)) return OutputInfo::Style::Pattern;
    if (flags & TA_OUT_HISTO) return OutputInfo::Style::Histogram;
    if (flags & TA_OUT_DASH_LINE) return OutputInfo::Style::DashLine;
    if (flags & TA_OUT_DOT_LINE) return OutputInfo::Style::DotLine;
    if (flags & TA_OUT_DOT) return OutputInfo::Style::Dots;
    return OutputInfo::Style::Line;
}

inline FunctionInfo describe(const TA_FuncInfo* fi)
{
    FunctionInfo info;
    info.name = fi->name ? fi->name : "";
    info.group = fi->group ? fi->group : "";
    info.hint = fi->hint ? fi->hint : "";
    info.overlay = (fi->flags & TA_FUNC_FLG_OVERLAP) != 0;
    info.volume = (fi->flags & TA_FUNC_FLG_VOLUME) != 0;
    info.candlestick = (fi->flags & TA_FUNC_FLG_CANDLESTICK) != 0;
    info.unstablePeriod = (fi->flags & TA_FUNC_FLG_UNST_PER) != 0;
    info.handle = fi->handle;
    for (unsigned i = 0; i < fi->nbInput; ++i) {
        const TA_InputParameterInfo* in = nullptr;
        if (TA_GetInputParameterInfo(fi->handle, i, &in) != TA_SUCCESS || !in) continue;
        if (in->type == TA_Input_Price) { ++info.priceInputs; info.priceFlags |= in->flags; }
        else ++info.realInputs;
    }
    for (unsigned i = 0; i < fi->nbOptInput; ++i) {
        const TA_OptInputParameterInfo* op = nullptr;
        if (TA_GetOptInputParameterInfo(fi->handle, i, &op) != TA_SUCCESS || !op) continue;
        ParamInfo p;
        p.name = op->paramName ? op->paramName : "";
        p.displayName = op->displayName ? op->displayName : p.name;
        p.hint = op->hint ? op->hint : "";
        p.defaultValue = op->defaultValue;
        p.advanced = (op->flags & TA_OPTIN_ADVANCED) != 0;
        p.percent = (op->flags & TA_OPTIN_IS_PERCENT) != 0;
        switch (op->type) {
        case TA_OptInput_IntegerRange: {
            const auto* r = static_cast<const TA_IntegerRange*>(op->dataSet);
            p.kind = ParamInfo::Kind::Integer;
            p.min = r ? r->min : 1;
            p.max = r ? r->max : 100000;
            break;
        }
        case TA_OptInput_RealRange: {
            const auto* r = static_cast<const TA_RealRange*>(op->dataSet);
            p.kind = ParamInfo::Kind::Real;
            p.min = r ? r->min : -1e9;
            p.max = r ? r->max : 1e9;
            p.precision = r ? r->precision : 2;
            break;
        }
        case TA_OptInput_IntegerList: {
            const auto* l = static_cast<const TA_IntegerList*>(op->dataSet);
            p.kind = ParamInfo::Kind::Choice;
            if (l) for (unsigned k = 0; k < l->nbElement; ++k) p.choices.emplace_back(l->data[k].value, l->data[k].string ? l->data[k].string : "");
            break;
        }
        case TA_OptInput_RealList: {
            const auto* l = static_cast<const TA_RealList*>(op->dataSet);
            p.kind = ParamInfo::Kind::Choice;
            if (l) for (unsigned k = 0; k < l->nbElement; ++k) p.choices.emplace_back(static_cast<int>(l->data[k].value), l->data[k].string ? l->data[k].string : "");
            break;
        }
        }
        info.params.push_back(std::move(p));
    }
    for (unsigned i = 0; i < fi->nbOutput; ++i) {
        const TA_OutputParameterInfo* out = nullptr;
        if (TA_GetOutputParameterInfo(fi->handle, i, &out) != TA_SUCCESS || !out) continue;
        OutputInfo o;
        o.name = out->paramName ? out->paramName : "";
        o.shortName = o.name.rfind("out", 0) == 0 ? o.name.substr(3) : o.name;
        if (o.shortName == "Real" || o.shortName == "Integer") o.shortName = info.name;
        o.integer = out->type == TA_Output_Integer;
        o.flags = out->flags;
        o.style = styleFromFlags(out->flags);
        // TA-Lib 0.4 marks candlestick outputs as plain lines; they are +/-100 pattern signals.
        if (info.candlestick && o.integer) o.style = OutputInfo::Style::Pattern;
        o.upperLimit = (out->flags & TA_OUT_UPPER_LIMIT) != 0;
        o.lowerLimit = (out->flags & TA_OUT_LOWER_LIMIT) != 0;
        o.canBeNegative = (out->flags & TA_OUT_NEGATIVE) != 0;
        o.hasZero = (out->flags & TA_OUT_ZERO) != 0;
        info.outputs.push_back(std::move(o));
    }
    return info;
}

inline void collect(const TA_FuncInfo* fi, void* opaque)
{
    auto* catalog = static_cast<Catalog*>(opaque);
    if (!fi || !fi->group || excludedGroup(fi->group)) return;
    FunctionInfo info = describe(fi);
    // Functions with two independent series inputs (BETA, CORREL) have no single-symbol meaning.
    if (info.realInputs > 1) return;
    if (std::find(catalog->groups.begin(), catalog->groups.end(), info.group) == catalog->groups.end()) catalog->groups.push_back(info.group);
    catalog->functions.emplace(info.name, std::move(info));
}

} // namespace detail

/// The TA-Lib function catalogue, built once from the library's own metadata.
inline const Catalog& catalog()
{
    static const Catalog instance = [] {
        Catalog c;
        if (!detail::ensureInitialised()) return c;
        TA_ForEachFunc(&detail::collect, &c);
        // Present the groups in the order a trader expects.
        const std::vector<std::string> preferred{ "Overlap Studies", "Momentum Indicators", "Volume Indicators", "Volatility Indicators",
                                                  "Price Transform", "Cycle Indicators", "Pattern Recognition", "Statistic Functions" };
        std::vector<std::string> ordered;
        for (const std::string& g : preferred) {
            if (std::find(c.groups.begin(), c.groups.end(), g) != c.groups.end()) ordered.push_back(g);
        }
        for (const std::string& g : c.groups) {
            if (std::find(ordered.begin(), ordered.end(), g) == ordered.end()) ordered.push_back(g);
        }
        c.groups = ordered;
        return c;
    }();
    return instance;
}

/// Open/high/low/close/volume arrays of equal length, oldest first.
struct Bars {
    std::vector<double> open, high, low, close, volume;
    size_t size() const { return close.size(); }
};

/// One output series aligned with the input bars (NaN before the lookback).
struct Series {
    OutputInfo info;
    std::vector<double> values;
};

struct Result {
    bool ok = false;
    std::string error;
    int lookback = 0;
    std::vector<Series> outputs;
};

using Params = std::map<std::string, double>;

/// Fills `params` with defaults for anything missing and clamps to the documented ranges.
inline Params normalizedParams(const FunctionInfo& info, const Params& given)
{
    Params out;
    for (const ParamInfo& p : info.params) {
        double v = p.defaultValue;
        const auto it = given.find(p.name);
        if (it != given.end() && std::isfinite(it->second)) v = it->second;
        if (p.kind == ParamInfo::Kind::Choice) {
            bool valid = false;
            for (const auto& [value, label] : p.choices) valid = valid || value == static_cast<int>(v);
            if (!valid) v = p.defaultValue;
        } else {
            v = std::clamp(v, p.min, p.max);
            if (p.kind == ParamInfo::Kind::Integer) v = std::round(v);
        }
        out[p.name] = v;
    }
    return out;
}

/// Evaluates `functionName` over the bars. Outputs are aligned with the bars.
inline Result compute(const std::string& functionName, const Bars& bars, const Params& given = {})
{
    Result result;
    const FunctionInfo* info = catalog().find(functionName);
    if (!info) { result.error = "Unknown TA-Lib function: " + functionName; return result; }
    const int n = static_cast<int>(bars.size());
    if (n <= 0) { result.error = "No bars."; return result; }
    if (bars.open.size() != bars.close.size() || bars.high.size() != bars.close.size() || bars.low.size() != bars.close.size() || bars.volume.size() != bars.close.size()) {
        result.error = "Bar arrays differ in length.";
        return result;
    }

    TA_ParamHolder* holder = nullptr;
    if (TA_ParamHolderAlloc(info->handle, &holder) != TA_SUCCESS || !holder) { result.error = "TA-Lib could not allocate parameters."; return result; }
    struct Guard { TA_ParamHolder* h; ~Guard() { if (h) TA_ParamHolderFree(h); } } guard{ holder };

    const TA_FuncInfo* fi = nullptr;
    TA_GetFuncInfo(info->handle, &fi);
    if (!fi) { result.error = "TA-Lib function info unavailable."; return result; }

    for (unsigned i = 0; i < fi->nbInput; ++i) {
        const TA_InputParameterInfo* in = nullptr;
        if (TA_GetInputParameterInfo(fi->handle, i, &in) != TA_SUCCESS || !in) continue;
        TA_RetCode rc;
        if (in->type == TA_Input_Price) {
            rc = TA_SetInputParamPricePtr(holder, i, bars.open.data(), bars.high.data(), bars.low.data(), bars.close.data(), bars.volume.data(), nullptr);
        } else {
            rc = TA_SetInputParamRealPtr(holder, i, bars.close.data());
        }
        if (rc != TA_SUCCESS) { result.error = "TA-Lib rejected an input series."; return result; }
    }

    const Params params = normalizedParams(*info, given);
    for (unsigned i = 0; i < fi->nbOptInput; ++i) {
        const TA_OptInputParameterInfo* op = nullptr;
        if (TA_GetOptInputParameterInfo(fi->handle, i, &op) != TA_SUCCESS || !op) continue;
        const double v = params.at(op->paramName);
        const TA_RetCode rc = (op->type == TA_OptInput_RealRange || op->type == TA_OptInput_RealList)
                                  ? TA_SetOptInputParamReal(holder, i, v)
                                  : TA_SetOptInputParamInteger(holder, i, static_cast<TA_Integer>(std::lround(v)));
        if (rc != TA_SUCCESS) { result.error = std::string("Parameter out of range: ") + op->paramName; return result; }
    }

    std::vector<std::vector<double>> realOut(fi->nbOutput);
    std::vector<std::vector<TA_Integer>> intOut(fi->nbOutput);
    for (unsigned i = 0; i < fi->nbOutput; ++i) {
        if (info->outputs[i].integer) { intOut[i].assign(static_cast<size_t>(n), 0); TA_SetOutputParamIntegerPtr(holder, i, intOut[i].data()); }
        else { realOut[i].assign(static_cast<size_t>(n), 0.0); TA_SetOutputParamRealPtr(holder, i, realOut[i].data()); }
    }

    TA_Integer lookback = 0;
    TA_GetLookback(holder, &lookback);
    result.lookback = lookback;
    TA_Integer outBegin = 0, outCount = 0;
    const TA_RetCode rc = TA_CallFunc(holder, 0, n - 1, &outBegin, &outCount);
    if (rc != TA_SUCCESS) {
        TA_RetCodeInfo rci;
        TA_SetRetCodeInfo(rc, &rci);
        result.error = std::string("TA-Lib: ") + rci.infoStr;
        return result;
    }

    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (unsigned i = 0; i < fi->nbOutput; ++i) {
        Series s;
        s.info = info->outputs[i];
        s.values.assign(static_cast<size_t>(n), nan);
        for (TA_Integer k = 0; k < outCount; ++k) {
            const size_t idx = static_cast<size_t>(outBegin + k);
            if (idx >= s.values.size()) break;
            s.values[idx] = info->outputs[i].integer ? static_cast<double>(intOut[i][static_cast<size_t>(k)]) : realOut[i][static_cast<size_t>(k)];
        }
        result.outputs.push_back(std::move(s));
    }
    result.ok = true;
    return result;
}

/// Short chart label: "RSI 14", "MACD 12/26/9", "BBANDS 20/2/2/SMA", "SMA 20", "CDLDOJI".
inline std::string label(const FunctionInfo& info, const Params& given)
{
    const Params params = normalizedParams(info, given);
    std::string out = info.name;
    std::string suffix;
    for (const ParamInfo& p : info.params) {
        if (p.advanced) continue;
        const double v = params.at(p.name);
        std::string text;
        if (p.kind == ParamInfo::Kind::Choice) {
            for (const auto& [value, name] : p.choices) if (value == static_cast<int>(v)) text = name;
            if (text.empty()) text = std::to_string(static_cast<int>(v));
        } else if (p.kind == ParamInfo::Kind::Integer) {
            text = std::to_string(static_cast<long>(std::lround(v)));
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.*f", std::max(0, std::min(p.precision, 4)), v);
            text = buf;
            // trim trailing zeros
            if (text.find('.') != std::string::npos) {
                while (!text.empty() && text.back() == '0') text.pop_back();
                if (!text.empty() && text.back() == '.') text.pop_back();
            }
        }
        suffix += (suffix.empty() ? " " : "/") + text;
    }
    return out + suffix;
}

/// Human-readable candlestick pattern name from its TA-Lib name: "CDL3BLACKCROWS" -> "3 Black Crows".
inline std::string patternName(const FunctionInfo& info)
{
    return info.hint.empty() ? info.name : info.hint;
}

} // namespace ta
