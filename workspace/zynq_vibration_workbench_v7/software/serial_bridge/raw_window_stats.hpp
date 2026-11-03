#pragma once
#include "plot_canvas.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

namespace vibration::serial {
// Only call with the validated, original ADC-voltage preview samples. Display
// envelopes are unsuitable for RMS because their selection is not uniform.
inline QJsonObject rawWindowStats(const QVector<workbench::PlotSeries>& original,
                                 double start, double end, quint64 revision) {
    QJsonArray channels;
    for (int axis = 0; axis < original.size(); ++axis) {
        quint64 count = 0; bool gaps = false;
        double mean = 0, m2 = 0, squareSum = 0;
        double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
        for (const auto& point : original[axis].points) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) { gaps = true; continue; }
            if (point.x() < start || point.x() > end) continue;
            const double value = point.y(); ++count;
            const double delta = value - mean; mean += delta / double(count);
            m2 += delta * (value - mean); squareSum += value * value;
            minimum = std::min(minimum, value); maximum = std::max(maximum, value);
        }
        const auto number = [count](double value) { return count ? QJsonValue(value) : QJsonValue(QJsonValue::Null); };
        channels.append(QJsonObject{{"channel",axis+1},{"sample_count",double(count)}, {"has_gaps",gaps},
            {"mean",number(mean)},{"minimum",number(minimum)},{"maximum",number(maximum)},
            {"peak_to_peak",number(maximum-minimum)}, {"rms",number(count ? std::sqrt(squareSum/double(count)) : 0)},
            {"ac_rms",number(count ? std::sqrt(std::max(0.0,m2/double(count))) : 0)}});
    }
    return {{"scope","raw_window"},{"unit","V"},{"start_s",start},{"end_s",end},
        {"input_revision",double(revision)}, {"derived_before_display_decimation",true}, {"channels",channels}};
}
}
