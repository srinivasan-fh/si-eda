// sieda-mcp --connect: a stdio ⇄ HTTP bridge to the SiEDA app's live MCP endpoint (docs/MCP.md, "Live app mode").
//
// Stdio-only MCP clients (Claude Desktop and others) launch `sieda-mcp --connect http://127.0.0.1:39717/mcp` and
// talk to the design open in the app window. Each stdin line (one JSON-RPC message) is POSTed to the endpoint with
// the bearer token; the JSON response (or each Server-Sent Event's data) is written to stdout as one line.
// Plain POSIX sockets and HTTP/1.1, no TLS: the endpoint only listens on this Mac's loopback interface.
#pragma once

#include <string>

namespace sieda::mcpbridge {

struct Url {
    std::string host;  // "127.0.0.1", "localhost", "::1"
    int port = 80;
    std::string path = "/";
};

/// Parses "http://host[:port][/path]". IPv6 hosts in brackets ("http://[::1]:39717/mcp"). False for anything else
/// (https is not supported: the endpoint is loopback only). Hosts other than 127.0.0.1, localhost and ::1 are
/// refused, so the bearer token is never sent off this Mac.
bool parseUrl(const std::string& text, Url* out);

struct HttpReply {
    int status = 0;               // 0: no reply (connection failed); see `error`
    std::string contentType;      // lower case
    std::string sessionId;        // Mcp-Session-Id header, if any
    std::string body;             // de-chunked
    std::string error;            // transport failure
};

/// One POST with Connection: close. Headers: Content-Type / Accept for MCP, Authorization: Bearer <token> when
/// `token` is not empty, Mcp-Session-Id when `sessionId` is not empty. Times out after `timeoutSeconds`.
HttpReply httpPost(const Url& url, const std::string& token, const std::string& sessionId, const std::string& body,
                   int timeoutSeconds);

/// The JSON-RPC messages carried by a reply as stdout lines: the JSON body itself (newlines removed), or the `data:`
/// payload of every Server-Sent Event in a text/event-stream body.
std::string replyLines(const HttpReply& reply);

/// Runs the bridge until stdin ends. Returns the process exit code.
int runBridge(const std::string& url, const std::string& token, int timeoutSeconds);

}  // namespace sieda::mcpbridge
