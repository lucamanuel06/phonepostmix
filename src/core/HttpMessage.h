#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ppm::http
{

/** A parsed HTTP/1.1 request line plus headers.

    Only what a static-file-and-WebSocket-upgrade server needs: no bodies, no chunked
    transfer, no keep-alive. Every response this server sends closes the connection.
*/
struct Request
{
    std::string method;
    std::string target;   ///< the raw request target, query string included
    std::string path;     ///< `target` with the query string stripped
    std::string version;
    std::map<std::string, std::string> headers;  ///< keys lower-cased

    /** Returns a header value, or an empty string if absent. Names are case-insensitive. */
    std::string header (std::string name) const;

    /** True if this request is a valid RFC 6455 WebSocket upgrade. */
    bool isWebSocketUpgrade() const;
};

/** Parses a complete request head (everything up to and including the blank line).

    Returns nullopt if `text` does not contain a complete, well-formed head, so the caller
    can keep reading from the socket and try again.
*/
std::optional<Request> parseRequest (const std::string& text);

/** Guesses a Content-Type from a file extension, defaulting to application/octet-stream. */
std::string mimeTypeForPath (const std::string& path);

/** Builds a complete response head. `contentLength` of -1 omits the header entirely. */
std::string makeResponseHead (int statusCode,
                              const std::string& reasonPhrase,
                              const std::string& contentType,
                              long long contentLength,
                              const std::vector<std::string>& extraHeaders = {});

} // namespace ppm::http
