// Copyright (c) 2014 baidu-rpc authors.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: Ge,Jun (gejun@baidu.com)
// Date: 2014/09/22 11:57:43

#include "bvar/latency_recorder.h"
#include "base/unique_ptr.h"

namespace bvar {
namespace detail {

typedef PercentileSamples<1022> CombinedPercentileSamples;

CDF::CDF(PercentileWindow* w) : _w(w) {}

CDF::~CDF() {
    hide();
}
    
void CDF::describe(std::ostream& os, bool) const {
    os << "\"click to view\"";
}

int CDF::describe_series(
    std::ostream& os, const SeriesOptions& options) const {
    if (_w == NULL) {
        return 1;
    }
    if (options.test_only) {
        return 0;
    }
    std::unique_ptr<CombinedPercentileSamples> cb(new CombinedPercentileSamples);
    std::vector<GlobalPercentileSamples> buckets;
    _w->get_samples(&buckets);
    for (size_t i = 0; i < buckets.size(); ++i) {
        cb->combine_of(buckets.begin(), buckets.end());
    }
    std::pair<int, int> values[20];
    size_t n = 0;
    for (int i = 1; i < 10; ++i) {
        values[n++] = std::make_pair(i*10, cb->get_number(i * 0.1));
    }
    for (int i = 91; i < 100; ++i) {
        values[n++] = std::make_pair(i, cb->get_number(i * 0.01));
    }
    values[n++] = std::make_pair(100, cb->get_number(0.999));
    values[n++] = std::make_pair(101, cb->get_number(0.9999));
    CHECK_EQ(n, arraysize(values));
    os << "{\"label\":\"cdf\",\"data\":[";
    for (size_t i = 0; i < n; ++i) {
        if (i) {
            os << ',';
        }
        os << '[' << values[i].first << ',' << values[i].second << ']';
    }
    os << "]}";
    return 0;
}

static int64_t get_window_recorder_qps(void* arg) {
    detail::Sample<Stat> s;
    static_cast<RecorderWindow*>(arg)->get_span(1, &s);
    // Use floating point to avoid overflow.
    if (s.time_us <= 0) {
        return 0;
    }
    return static_cast<int64_t>(round(s.data.num * 1000000.0 / s.time_us));
}

static int64_t get_recorder_count(void* arg) {
    return static_cast<IntRecorder*>(arg)->get_value().num;
}

// Caller is responsible for deleting the return value.
static CombinedPercentileSamples* combine(PercentileWindow* w) {
    CombinedPercentileSamples* cb = new CombinedPercentileSamples;
    std::vector<GlobalPercentileSamples> buckets;
    w->get_samples(&buckets);
    cb->combine_of(buckets.begin(), buckets.end());
    return cb;
}

template <int64_t numerator, int64_t denominator>
int64_t get_percetile(void* arg) {
    return ((LatencyRecorder*)arg)->latency_percentile(
            (double)numerator / double(denominator));
}

static Vector<int64_t, 4> get_latencies(void *arg) {
    std::unique_ptr<CombinedPercentileSamples> cb(
        combine((PercentileWindow*)arg));
    // NOTE: We don't show 99.99% since it's often significantly larger than
    // other values and make other curves on the plotted graph small and
    // hard to read.
    Vector<int64_t, 4> result;
    result[0] = cb->get_number(0.5);
    result[1] = cb->get_number(0.90);
    result[2] = cb->get_number(0.99);
    result[3] = cb->get_number(0.999);
    return result;
}

LatencyRecorderBase::LatencyRecorderBase(time_t window_size)
    : _max_latency(0)
    , _latency_window(&_latency, window_size)
    , _max_latency_window(&_max_latency, window_size)
    , _count(get_recorder_count, &_latency)
    , _qps(get_window_recorder_qps, &_latency_window)
    , _latency_percentile_window(&_latency_percentile, window_size)
    , _latency_50(get_percetile<50, 100>, this)
    , _latency_90(get_percetile<90, 100>, this)
    , _latency_99(get_percetile<99, 100>, this)
    , _latency_999(get_percetile<999, 1000>, this)
    , _latency_9999(get_percetile<9999, 10000>, this)
    , _latency_cdf(&_latency_percentile_window)
    , _latency_percentiles(get_latencies, &_latency_percentile_window)
{}

}  // namespace detail

Vector<int64_t, 4> LatencyRecorder::latency_percentiles() const {
    // const_cast here is just to adapt parameter type and safe.
    return detail::get_latencies(
        const_cast<detail::PercentileWindow*>(&_latency_percentile_window));
}

int64_t LatencyRecorder::qps(time_t window_size) const {
    detail::Sample<Stat> s;
    _latency_window.get_span(window_size, &s);
    // Use floating point to avoid overflow.
    if (s.time_us <= 0) {
        return 0;
    }
    return static_cast<int64_t>(round(s.data.num * 1000000.0 / s.time_us));
}

int LatencyRecorder::expose(const base::StringPiece& prefix1,
                            const base::StringPiece& prefix2) {
    if (prefix2.empty()) {
        LOG(ERROR) << "Parameter[prefix2] is empty";
        return -1;
    }
    base::StringPiece prefix = prefix2;
    // User may add "_latency" as the suffix, remove it.
    if (prefix.ends_with("latency") || prefix.ends_with("Latency")) {
        prefix.remove_suffix(7);
        if (prefix.empty()) {
            LOG(ERROR) << "Invalid prefix2=" << prefix2;
            return -1;
        }
    }
    std::string tmp;
    if (!prefix1.empty()) {
        tmp.reserve(prefix1.size() + prefix.size() + 1);
        tmp.append(prefix1.data(), prefix1.size());
        tmp.push_back('_'); // prefix1 ending with _ is good.
        tmp.append(prefix.data(), prefix.size());
        prefix = tmp;
    }

    // set debug names for printing helpful error log.
    _latency.set_debug_name(prefix);
    _latency_percentile.set_debug_name(prefix);

    if (_latency_window.expose_as(prefix, "latency") != 0) {
        return -1;
    }
    if (_max_latency_window.expose_as(prefix, "max_latency") != 0) {
        return -1;
    }
    if (_count.expose_as(prefix, "count") != 0) {
        return -1;
    }
    if (_qps.expose_as(prefix, "qps") != 0) {
        return -1;
    }
    if (_latency_50.expose_as(prefix, "latency_50", DISPLAY_ON_PLAIN_TEXT) != 0) {
        return -1;
    }
    if (_latency_90.expose_as(prefix, "latency_90", DISPLAY_ON_PLAIN_TEXT) != 0) {
        return -1;
    }
    if (_latency_99.expose_as(prefix, "latency_99", DISPLAY_ON_PLAIN_TEXT) != 0) {
        return -1;
    }
    if (_latency_999.expose_as(prefix, "latency_999", DISPLAY_ON_PLAIN_TEXT) != 0) {
        return -1;
    }
    if (_latency_9999.expose_as(prefix, "latency_9999") != 0) {
        return -1;
    }
    if (_latency_cdf.expose_as(prefix, "latency_cdf", DISPLAY_ON_HTML) != 0) {
        return -1;
    }
    if (_latency_percentiles.expose_as(prefix, "latency_percentiles", DISPLAY_ON_HTML) != 0) {
        return -1;
    }
    CHECK_EQ(0, _latency_percentiles.set_vector_names("50%,90%,99%,99.9%"));
    return 0;
}

int64_t LatencyRecorder::latency_percentile(double ratio) const {
    std::unique_ptr<detail::CombinedPercentileSamples> cb(
        combine((detail::PercentileWindow*)&_latency_percentile_window));
    return cb->get_number(ratio);
}

void LatencyRecorder::hide() {
    _latency_window.hide();
    _max_latency_window.hide();
    _count.hide();
    _qps.hide();
    _latency_50.hide();
    _latency_90.hide();
    _latency_99.hide();
    _latency_999.hide();
    _latency_9999.hide();
}

LatencyRecorder& LatencyRecorder::operator<<(int64_t latency) {
    _latency << latency;
    _max_latency << latency;
    _latency_percentile << latency;
    return *this;
}

std::ostream& operator<<(std::ostream& os, const LatencyRecorder& rec) {
    return os << "{latency=" << rec.latency()
              << " max" << rec.window_size() << '=' << rec.max_latency()
              << " qps=" << rec.qps()
              << " count=" << rec.count() << '}';
}

}  // namespace bvar
