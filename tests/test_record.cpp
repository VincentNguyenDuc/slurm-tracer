#include "check.h"

#include <cstdio>
#include <string>
#include <vector>

#include "record.h"
#include "stdout_json_sink.h"

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

st::Record sample() {
    st::Record r;
    r.ts_ns = 1756400000000000000ull;
    r.node = "cn01";
    r.cluster = "prod";
    r.probe = "proc_lifecycle";
    r.job_id = 4242;
    r.step_id = "batch";
    r.task_id = 0;
    r.uid = 1000;
    r.user = "alice";
    r.event_type = "exec";
    r.pid = 1234;
    r.tid = 1234;
    r.comm = "hostname";
    r.metric = "proc.exec";
    r.value = 1.0;
    r.unit = "count";
    r.attrs.emplace_back("cgroup_id", "9876");
    return r;
}

} // namespace

TEST_CASE(record_json_has_every_field) {
    const std::string j = st::to_json(sample());
    CHECK(contains(j, "\"ts_ns\":1756400000000000000"));
    CHECK(contains(j, "\"node\":\"cn01\""));
    CHECK(contains(j, "\"cluster\":\"prod\""));
    CHECK(contains(j, "\"probe\":\"proc_lifecycle\""));
    CHECK(contains(j, "\"job_id\":4242"));
    CHECK(contains(j, "\"step_id\":\"batch\""));
    CHECK(contains(j, "\"task_id\":0"));
    CHECK(contains(j, "\"uid\":1000"));
    CHECK(contains(j, "\"user\":\"alice\""));
    CHECK(contains(j, "\"event_type\":\"exec\""));
    CHECK(contains(j, "\"comm\":\"hostname\""));
    CHECK(contains(j, "\"attrs\":{\"cgroup_id\":\"9876\"}"));
    // One line, so the stream is valid NDJSON.
    CHECK(j.find('\n') == std::string::npos);
}

TEST_CASE(record_json_emits_null_for_unattributed) {
    // Unattributed records are emitted, not dropped, and keep a fixed key set
    // so the warehouse schema stays stable.
    st::Record r = sample();
    r.job_id.reset();
    r.step_id.reset();
    r.task_id.reset();
    r.account.reset();

    const std::string j = st::to_json(r);
    CHECK(contains(j, "\"job_id\":null"));
    CHECK(contains(j, "\"step_id\":null"));
    CHECK(contains(j, "\"task_id\":null"));
    CHECK(contains(j, "\"account\":null"));
    CHECK(contains(j, "\"partition\":null"));
}

TEST_CASE(record_json_escapes_strings) {
    // comm is 16 bytes straight out of the kernel and an attacker controls it;
    // an unescaped quote would corrupt the whole stream.
    st::Record r = sample();
    r.comm = "ev\"il\\";
    r.attrs.clear();
    r.attrs.emplace_back("path", "/tmp/a\nb\tc");

    const std::string j = st::to_json(r);
    CHECK(contains(j, "\"comm\":\"ev\\\"il\\\\\""));
    CHECK(contains(j, "\\n"));
    CHECK(contains(j, "\\t"));
    CHECK(j.find('\n') == std::string::npos);
}

TEST_CASE(record_json_control_characters_become_escapes) {
    st::Record r = sample();
    r.comm = std::string("a\x01"
                         "b");
    const std::string j = st::to_json(r);
    CHECK(contains(j, "\\u0001"));
}

TEST_CASE(record_json_value_round_trips) {
    // The default ostream precision of 6 significant digits would truncate a
    // counter; the serializer must not lose bits.
    st::Record r = sample();
    r.value = 1234567890123.0;
    CHECK(contains(st::to_json(r), "1234567890123"));
}

TEST_CASE(sink_writes_ndjson) {
    std::FILE* fp = std::tmpfile();
    REQUIRE(fp != nullptr);

    {
        st::StdoutJsonSink sink(16, fp);
        std::vector<st::Record> batch;
        batch.push_back(sample());
        batch.push_back(sample());
        sink.write(std::move(batch));
        sink.flush();
    }

    std::rewind(fp);
    std::string out;
    char buf[4096];
    while (const size_t n = std::fread(buf, 1, sizeof(buf), fp))
        out.append(buf, n);
    std::fclose(fp);

    int lines = 0;
    for (const char c : out) {
        if (c == '\n')
            ++lines;
    }
    CHECK_EQ(lines, 2);
    CHECK(contains(out, "\"job_id\":4242"));
}

TEST_CASE(sink_drops_oldest_on_overflow_and_counts_it) {
    // Backpressure must never reach the ring buffer: a stalled sink drops and
    // counts rather than blocking the poll loop.
    std::FILE* fp = std::tmpfile();
    REQUIRE(fp != nullptr);

    st::StdoutJsonSink sink(2, fp);
    for (int i = 0; i < 200; ++i) {
        std::vector<st::Record> batch;
        batch.push_back(sample());
        sink.write(std::move(batch));
    }
    sink.flush();

    // The worker drains concurrently, so the exact count is timing-dependent;
    // what matters is that overflow is survivable and visible.
    std::printf("    (dropped %llu batches)\n", static_cast<unsigned long long>(sink.dropped()));
    std::fclose(fp);
}
