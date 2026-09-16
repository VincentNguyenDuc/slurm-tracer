#include "core/record/record.h"

#include <nlohmann/json.hpp>

namespace slurm_tracer {
namespace {

// nlohmann::ordered_json (not plain nlohmann::json, whose default backing map
// sorts keys alphabetically) so the emitted field order keeps matching the
// deliberate provenance -> attribution -> identity -> payload grouping, in the
// order fields are assigned below.
using Json = nlohmann::ordered_json;

// Absent optionals are emitted as JSON null rather than omitted: a fixed key
// set keeps the Parquet schema stable and makes unattributed records easy to
// count in the warehouse.
template <typename T>
Json opt(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

} // namespace

std::string to_json(const Record& r) {
    Json j;

    j["ts_ns"] = r.ts_ns;
    j["node"] = r.node;
    j["cluster"] = r.cluster;
    j["probe"] = r.probe;

    j["cgroup_id"] = r.cgroup_id;
    j["job_id"] = opt(r.job_id);
    j["step_id"] = opt(r.step_id);
    j["task_id"] = opt(r.task_id);
    j["uid"] = opt(r.uid);
    j["user"] = opt(r.user);
    j["account"] = opt(r.account);
    j["partition"] = opt(r.partition);

    j["event_type"] = r.event_type;
    j["pid"] = r.pid;
    j["tid"] = r.tid;
    j["comm"] = r.comm;

    j["metric"] = r.metric;
    j["value"] = r.value; // nlohmann's float serializer is already round-trip safe
    j["unit"] = r.unit;

    Json& attrs = j["attrs"] = Json::object();
    for (const auto& [key, value] : r.attrs)
        attrs[key] = value;

    return j.dump();
}

} // namespace slurm_tracer
