// sieda-mcp --connect: stdio ⇄ HTTP bridge to the app's live MCP endpoint (see http_bridge.hpp, docs/MCP.md).
#include "http_bridge.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "sieda/Json.hpp"

#ifndef _WIN32
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#endif

namespace sieda::mcpbridge {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

/// Decodes a chunked body; false when it is malformed or incomplete.
bool dechunk(const std::string& in, std::string* out) {
    size_t at = 0;
    out->clear();
    while (true) {
        const size_t eol = in.find("\r\n", at);
        if (eol == std::string::npos) return false;
        std::string sizeText = in.substr(at, eol - at);
        const size_t semi = sizeText.find(';');
        if (semi != std::string::npos) sizeText.resize(semi);
        sizeText = trim(sizeText);
        if (sizeText.empty()) return false;
        size_t size = 0;
        for (char c : sizeText) {
            const int v = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0 || size > (SIZE_MAX >> 4)) return false;
            size = size * 16 + static_cast<size_t>(v);
        }
        at = eol + 2;
        if (size == 0) return true;  // trailers ignored
        if (at + size > in.size()) return false;
        out->append(in, at, size);
        at += size + 2;  // data + CRLF
    }
}

/// JSON-RPC error responses for the requests (with an id) in `line`, so a client never waits for an answer that
/// will not come. "" when the line holds only notifications or is not JSON.
std::string errorLines(const std::string& line, const std::string& message) {
    Json msg;
    try {
        msg = Json::parse(line);
    } catch (...) {
        return std::string();
    }
    auto one = [&](const Json& m) -> Json {
        if (!m.isObject() || !m.has("id") || !m.has("method")) return Json();
        Json err = Json::object();
        err["code"] = -32000;
        err["message"] = message;
        Json r = Json::object();
        r["jsonrpc"] = "2.0";
        r["id"] = m.get("id");
        r["error"] = err;
        return r;
    };
    if (msg.isArray()) {
        Json list = Json::array();
        for (const auto& m : msg.items()) {
            Json r = one(m);
            if (!r.isNull()) list.push(r);
        }
        return list.size() > 0 ? list.dump() : std::string();
    }
    const Json r = one(msg);
    return r.isNull() ? std::string() : r.dump();
}

std::string withoutNewlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    // Raw CR / LF cannot occur inside a JSON string, so dropping them keeps the message intact.
    for (char c : s)
        if (c != '\n' && c != '\r') out += c;
    return out;
}
}  // namespace

constexpr size_t kMaxReplyBytes = 64u * 1024u * 1024u;

bool parseUrl(const std::string& text, Url* out) {
    const std::string scheme = "http://";
    if (lower(text.substr(0, scheme.size())) != scheme) return false;
    std::string rest = text.substr(scheme.size());
    const size_t slash = rest.find('/');
    Url u;
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    std::string portText;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        u.host = authority.substr(1, close - 1);
        const std::string after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return false;
            portText = after.substr(1);
        }
    } else {
        const size_t colon = authority.rfind(':');
        u.host = authority.substr(0, colon);
        if (colon != std::string::npos) portText = authority.substr(colon + 1);
    }
    if (u.host.empty()) return false;
    // The app's endpoint listens on this Mac only; the bearer token is never sent anywhere else (plain HTTP).
    const std::string h = lower(u.host);
    if (h != "127.0.0.1" && h != "localhost" && h != "::1") return false;
    if (!portText.empty()) {
        int port = 0;
        for (char c : portText) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            port = port * 10 + (c - '0');
            if (port > 65535) return false;
        }
        if (port <= 0) return false;
        u.port = port;
    }
    *out = u;
    return true;
}

std::string replyLines(const HttpReply& reply) {
    if (reply.contentType.find("text/event-stream") == std::string::npos) {
        const std::string body = trim(reply.body);
        return body.empty() ? std::string() : withoutNewlines(body) + "\n";
    }
    // Server-Sent Events: events are separated by a blank line; the data lines of an event join with "\n".
    std::string out, data;
    bool haveData = false;
    size_t at = 0;
    const std::string& b = reply.body;
    while (at <= b.size()) {
        size_t eol = b.find('\n', at);
        if (eol == std::string::npos) eol = b.size();
        std::string line = b.substr(at, eol - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (haveData && !trim(data).empty()) out += withoutNewlines(data) + "\n";
            data.clear();
            haveData = false;
        } else if (line.compare(0, 5, "data:") == 0) {
            std::string value = line.substr(5);
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
            if (haveData) data += "\n";
            data += value;
            haveData = true;
        }
        if (eol == b.size()) break;
        at = eol + 1;
    }
    if (haveData && !trim(data).empty()) out += withoutNewlines(data) + "\n";
    return out;
}

#ifndef _WIN32

namespace {
int connectTo(const Url& url, int timeoutSeconds, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string port = std::to_string(url.port);
    const int rc = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &list);
    if (rc != 0) {
        *error = std::string("cannot resolve ") + url.host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        timeval tv{};
        tv.tv_sec = timeoutSeconds;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0)
        *error = "cannot connect to " + url.host + ":" + port +
                 " (is SiEDA running with Settings > AI Access (MCP) > live server enabled?)";
    return fd;
}

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
}  // namespace

HttpReply httpPost(const Url& url, const std::string& token, const std::string& sessionId, const std::string& body,
                   int timeoutSeconds) {
    HttpReply reply;
    const int fd = connectTo(url, timeoutSeconds, &reply.error);
    if (fd < 0) return reply;
    const bool v6 = url.host.find(':') != std::string::npos;
    std::string req = "POST " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + (v6 ? "[" + url.host + "]" : url.host) + ":" + std::to_string(url.port) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Accept: application/json, text/event-stream\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    if (!sessionId.empty()) req += "Mcp-Session-Id: " + sessionId + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    if (!sendAll(fd, req)) {
        close(fd);
        reply.error = "the connection to the SiEDA app was closed while sending";
        return reply;
    }
    std::string raw;
    char buffer[16384];
    size_t headerEnd = std::string::npos;
    long long contentLength = -1;
    bool chunked = false;
    while (true) {
        const ssize_t n = recv(fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            close(fd);
            reply.error = errno == EAGAIN || errno == EWOULDBLOCK ? "the SiEDA app did not answer in time"
                                                                  : std::string("receive failed: ") + std::strerror(errno);
            return reply;
        }
        if (n == 0) break;
        raw.append(buffer, static_cast<size_t>(n));
        if (raw.size() > kMaxReplyBytes) {
            close(fd);
            reply.error = "reply from the SiEDA app is larger than 64 MB";
            return reply;
        }
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                // Parse the status line and the headers we need.
                size_t at = raw.find("\r\n");
                const std::string status = raw.substr(0, at);
                const size_t sp = status.find(' ');
                if (status.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos) {
                    close(fd);
                    reply.error = "not an HTTP reply";
                    return reply;
                }
                reply.status = std::atoi(status.c_str() + sp + 1);
                while (at < headerEnd) {
                    const size_t next = raw.find("\r\n", at + 2);
                    const std::string line = raw.substr(at + 2, next - at - 2);
                    at = next;
                    const size_t colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    const std::string name = lower(trim(line.substr(0, colon)));
                    const std::string value = trim(line.substr(colon + 1));
                    if (name == "content-type") reply.contentType = lower(value);
                    else if (name == "mcp-session-id") reply.sessionId = value;
                    else if (name == "content-length") contentLength = std::atoll(value.c_str());
                    else if (name == "transfer-encoding") chunked = lower(value).find("chunked") != std::string::npos;
                }
            }
        }
        // Stop as soon as a Content-Length body is complete (the server may keep the socket open).
        if (headerEnd != std::string::npos && !chunked && contentLength >= 0 &&
            raw.size() >= headerEnd + 4 + static_cast<size_t>(contentLength))
            break;
    }
    close(fd);
    if (headerEnd == std::string::npos) {
        reply.status = 0;
        reply.error = "incomplete reply from the SiEDA app";
        return reply;
    }
    std::string payload = raw.substr(headerEnd + 4);
    if (chunked) {
        std::string decoded;
        if (!dechunk(payload, &decoded)) {
            reply.error = "malformed chunked reply";
            reply.status = 0;
            return reply;
        }
        payload = decoded;
    } else if (contentLength >= 0 && payload.size() > static_cast<size_t>(contentLength)) {
        payload.resize(static_cast<size_t>(contentLength));
    }
    reply.body = payload;
    return reply;
}

int runBridge(const std::string& urlText, const std::string& token, int timeoutSeconds) {
    Url url;
    if (!parseUrl(urlText, &url)) {
        std::fprintf(stderr, "sieda-mcp: --connect needs an http://127.0.0.1:port/path (or localhost / [::1]) URL, got %s\n",
                     urlText.c_str());
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::fprintf(stderr, "sieda-mcp: bridging stdio to %s%s\n", urlText.c_str(),
                 token.empty() ? " (no token: set --token or SIEDA_MCP_TOKEN)" : "");
    std::string session;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        HttpReply reply = httpPost(url, token, session, line, timeoutSeconds);
        if (reply.status == 404 && !session.empty()) {
            // The app restarted (or dropped the session): forget it; the client re-initializes on the error.
            session.clear();
        }
        if (!reply.sessionId.empty()) session = reply.sessionId;
        std::string out;
        if (reply.status == 200) {
            out = replyLines(reply);
        } else if (reply.status == 202) {
            out.clear();  // a notification or response was accepted
        } else {
            std::string message;
            if (reply.status == 0) message = "SiEDA app unreachable: " + reply.error;
            else if (reply.status == 401) message = "SiEDA app refused the token (HTTP 401): copy it from Settings > AI Access (MCP)";
            else if (reply.status == 403) message = "SiEDA app refused the request (HTTP 403)";
            else if (reply.status == 404) message = "SiEDA app: session expired (HTTP 404), initialize again";
            else message = "SiEDA app answered HTTP " + std::to_string(reply.status) + ": " + trim(reply.body).substr(0, 200);
            std::fprintf(stderr, "sieda-mcp: %s\n", message.c_str());
            const std::string err = errorLines(line, message);
            if (!err.empty()) out = err + "\n";
        }
        if (!out.empty()) {
            std::fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);
        }
    }
    return 0;
}

#else  // _WIN32

HttpReply httpPost(const Url&, const std::string&, const std::string&, const std::string&, int) {
    HttpReply reply;
    reply.error = "--connect is not available on Windows";
    return reply;
}

int runBridge(const std::string&, const std::string&, int) {
    std::fprintf(stderr, "sieda-mcp: --connect is not available on Windows (the live app endpoint is macOS only)\n");
    return 2;
}

#endif

}  // namespace sieda::mcpbridge
