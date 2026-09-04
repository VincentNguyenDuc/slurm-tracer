#include "core/record.h"

#include <cstdio>
#include <sstream>

namespace slurm_tracer {
namespace {

void write_escaped(std::ostringstream& os, const std::string& s) {
    os << '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"':
            os << "\\\"";
            break;
        case '\\':
            os << "\\\\";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                os << buf;
            } else {
                os << static_cast<char>(c);
            }
        }
    }
    os << '"';
}

void write_key(std::ostringstream& os, bool& first, const char* key) {
    if (!first)
        os << ',';
    first = false;
    os << '"' << key << "\":";
}

void write_string(std::ostringstream& os, bool& first, const char* key, const std::string& value) {
    write_key(os, first, key);
    write_escaped(os, value);
}

// Absent optionals are emitted as JSON null rather than omitted: a fixed key
// set keeps the Parquet schema stable and makes unattributed records easy to
// count in the warehouse.
template <typename T>
void write_opt(
    std::ostringstream& os, bool& first, const char* key, const std::optional<T>& value
) {
    write_key(os, first, key);
    if (!value) {
        os << "null";
        return;
    }
    if constexpr (std::is_same_v<T, std::string>)
        write_escaped(os, *value);
    else
        os << *value;
}

} // namespace

std::string to_json(const Record& r) {
    std::ostringstream os;
    bool first = true;

    os << '{';

    write_key(os, first, "ts_ns");
    os << r.ts_ns;
    write_string(os, first, "node", r.node);
    write_string(os, first, "cluster", r.cluster);
    write_string(os, first, "probe", r.probe);

    write_key(os, first, "cgroup_id");
    os << r.cgroup_id;
    write_opt(os, first, "job_id", r.job_id);
    write_opt(os, first, "step_id", r.step_id);
    write_opt(os, first, "task_id", r.task_id);
    write_opt(os, first, "uid", r.uid);
    write_opt(os, first, "user", r.user);
    write_opt(os, first, "account", r.account);
    write_opt(os, first, "partition", r.partition);

    write_string(os, first, "event_type", r.event_type);
    write_key(os, first, "pid");
    os << r.pid;
    write_key(os, first, "tid");
    os << r.tid;
    write_string(os, first, "comm", r.comm);

    write_string(os, first, "metric", r.metric);
    write_key(os, first, "value");
    // %.17g round-trips an IEEE double exactly; the default ostream precision
    // of 6 significant digits would quietly truncate counters.
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", r.value);
        os << buf;
    }
    write_string(os, first, "unit", r.unit);

    write_key(os, first, "attrs");
    os << '{';
    bool first_attr = true;
    for (const auto& [key, value] : r.attrs) {
        if (!first_attr)
            os << ',';
        first_attr = false;
        write_escaped(os, key);
        os << ':';
        write_escaped(os, value);
    }
    os << '}';

    os << '}';
    return os.str();
}

} // namespace slurm_tracer
