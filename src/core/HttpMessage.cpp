#include "core/HttpMessage.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ppm::http
{

namespace
{

std::string toLower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return s;
}

std::string trim (const std::string& s)
{
    const auto first = s.find_first_not_of (" \t\r\n");

    if (first == std::string::npos)
        return {};

    const auto last = s.find_last_not_of (" \t\r\n");
    return s.substr (first, last - first + 1);
}

bool containsToken (const std::string& headerValue, const std::string& token)
{
    // Header values such as "keep-alive, Upgrade" are comma-separated token lists, so a
    // substring test on the whole value would both over- and under-match.
    std::istringstream stream (toLower (headerValue));
    std::string item;

    while (std::getline (stream, item, ','))
        if (trim (item) == token)
            return true;

    return false;
}

} // namespace

std::string Request::header (std::string name) const
{
    const auto it = headers.find (toLower (std::move (name)));
    return it == headers.end() ? std::string {} : it->second;
}

bool Request::isWebSocketUpgrade() const
{
    return method == "GET"
        && containsToken (header ("upgrade"), "websocket")
        && containsToken (header ("connection"), "upgrade")
        && ! header ("sec-websocket-key").empty();
}

std::optional<Request> parseRequest (const std::string& text)
{
    const auto headEnd = text.find ("\r\n\r\n");

    if (headEnd == std::string::npos)
        return std::nullopt;

    std::istringstream stream (text.substr (0, headEnd + 2));
    std::string line;

    if (! std::getline (stream, line))
        return std::nullopt;

    Request request;

    {
        std::istringstream requestLine (line);

        if (! (requestLine >> request.method >> request.target >> request.version))
            return std::nullopt;
    }

    while (std::getline (stream, line))
    {
        const auto colon = line.find (':');

        if (colon == std::string::npos)
            continue;

        request.headers[toLower (trim (line.substr (0, colon)))] = trim (line.substr (colon + 1));
    }

    const auto query = request.target.find ('?');
    request.path = query == std::string::npos ? request.target : request.target.substr (0, query);

    return request;
}

std::string mimeTypeForPath (const std::string& path)
{
    static const std::map<std::string, std::string> types {
        { ".html", "text/html; charset=utf-8" },
        { ".js",   "text/javascript; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".json", "application/json; charset=utf-8" },
        { ".svg",  "image/svg+xml" },
        { ".png",  "image/png" },
        { ".ico",  "image/x-icon" },
        { ".txt",  "text/plain; charset=utf-8" },
    };

    const auto dot = path.rfind ('.');

    if (dot != std::string::npos)
        if (const auto it = types.find (toLower (path.substr (dot))); it != types.end())
            return it->second;

    return "application/octet-stream";
}

std::string makeResponseHead (int statusCode,
                              const std::string& reasonPhrase,
                              const std::string& contentType,
                              long long contentLength,
                              const std::vector<std::string>& extraHeaders)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << statusCode << ' ' << reasonPhrase << "\r\n";

    if (! contentType.empty())
        out << "Content-Type: " << contentType << "\r\n";

    if (contentLength >= 0)
        out << "Content-Length: " << contentLength << "\r\n";

    for (const auto& h : extraHeaders)
        out << h << "\r\n";

    out << "\r\n";
    return out.str();
}

} // namespace ppm::http
