#include "plugins/sinks/http/endpoint.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string_view>

namespace slurm_tracer {

std::optional<HttpEndpoint> parse_http_endpoint(const std::string& endpoint) {
    constexpr std::string_view kPrefix = "http://";
    if (endpoint.compare(0, kPrefix.size(), kPrefix) != 0)
        return std::nullopt;

    const std::string rest = endpoint.substr(kPrefix.size());
    if (rest.empty())
        return std::nullopt;

    const size_t slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty())
        return std::nullopt;

    HttpEndpoint out;
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);

    const size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        out.host = authority;
    } else {
        out.host = authority.substr(0, colon);
        const std::string port_str = authority.substr(colon + 1);
        if (out.host.empty() || port_str.empty())
            return std::nullopt;
        for (const char c : port_str) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0)
                return std::nullopt;
        }

        errno = 0;
        const unsigned long port = std::strtoul(port_str.c_str(), nullptr, 10);
        if (errno != 0 || port == 0 || port > 65535)
            return std::nullopt;
        out.port = static_cast<uint16_t>(port);
    }

    return out;
}

} // namespace slurm_tracer
