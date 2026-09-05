// Parsing for the http sink's `endpoint` config value.
//
// Kept as a plain function over a plain struct, free of sockets, so it can be
// tested without a listener anywhere -- same reasoning as
// plugins/probes/proc_lifecycle/translate.h for keeping I/O out of the parts
// worth testing on their own.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace slurm_tracer {

struct HttpEndpoint {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

// Parses "http://host[:port][/path]". Only plain http is accepted: this sink
// hand-rolls its own client rather than take on a TLS dependency (see
// sink.h), so an https:// endpoint is rejected outright rather than silently
// sent in the clear.
std::optional<HttpEndpoint> parse_http_endpoint(const std::string& endpoint);

} // namespace slurm_tracer
