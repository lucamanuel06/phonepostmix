#include "TestSupport.h"

#include "core/HttpMessage.h"

PPM_TEST (parseRequestSplitsTheRequestLineAndHeaders)
{
    const auto request = ppm::http::parseRequest (
        "GET /app.js?v=3 HTTP/1.1\r\n"
        "Host: 192.168.1.50:8420\r\n"
        "User-Agent: probe\r\n"
        "\r\n");

    PPM_CHECK (request.has_value());
    PPM_CHECK (request && request->method == "GET");
    PPM_CHECK (request && request->target == "/app.js?v=3");
    PPM_CHECK (request && request->path == "/app.js");
    PPM_CHECK (request && request->header ("host") == "192.168.1.50:8420");
}

PPM_TEST (headerLookupIsCaseInsensitive)
{
    const auto request = ppm::http::parseRequest ("GET / HTTP/1.1\r\nSeC-WebSocket-Key: abc\r\n\r\n");

    PPM_CHECK (request && request->header ("sec-websocket-key") == "abc");
    PPM_CHECK (request && request->header ("SEC-WEBSOCKET-KEY") == "abc");
}

PPM_TEST (parseRequestReturnsNothingForAnIncompleteHead)
{
    PPM_CHECK (! ppm::http::parseRequest ("GET / HTTP/1.1\r\nHost: x\r\n").has_value());
    PPM_CHECK (! ppm::http::parseRequest ("GET /").has_value());
}

PPM_TEST (upgradeDetectionRequiresAllThreeSignals)
{
    const auto good = ppm::http::parseRequest (
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n");

    PPM_CHECK (good && good->isWebSocketUpgrade());

    // Chrome sends "keep-alive, Upgrade"; a naive substring match on the Connection
    // header would accept a plain "keep-alive" too, so the token list is parsed.
    const auto plain = ppm::http::parseRequest (
        "GET /ws HTTP/1.1\r\nConnection: keep-alive\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Key: k\r\n\r\n");

    PPM_CHECK (plain && ! plain->isWebSocketUpgrade());

    const auto noKey = ppm::http::parseRequest (
        "GET /ws HTTP/1.1\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");

    PPM_CHECK (noKey && ! noKey->isWebSocketUpgrade());
}

PPM_TEST (mimeTypesCoverTheAssetsWeActuallyServe)
{
    PPM_CHECK (ppm::http::mimeTypeForPath ("/index.html") == "text/html; charset=utf-8");
    PPM_CHECK (ppm::http::mimeTypeForPath ("/app.JS") == "text/javascript; charset=utf-8");
    PPM_CHECK (ppm::http::mimeTypeForPath ("/style.css") == "text/css; charset=utf-8");
    PPM_CHECK (ppm::http::mimeTypeForPath ("/no-extension") == "application/octet-stream");
}

PPM_TEST (responseHeadOmitsContentLengthWhenNegative)
{
    const auto head = ppm::http::makeResponseHead (101, "Switching Protocols", {}, -1,
                                                   { "Upgrade: websocket" });

    PPM_CHECK (head.find ("Content-Length") == std::string::npos);
    PPM_CHECK (head.find ("Content-Type") == std::string::npos);
    PPM_CHECK (head.rfind ("HTTP/1.1 101 Switching Protocols\r\n", 0) == 0);
    PPM_CHECK (head.size() >= 4 && head.compare (head.size() - 4, 4, "\r\n\r\n") == 0);
}
