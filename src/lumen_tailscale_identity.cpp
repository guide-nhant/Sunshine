// engine/host/upstream/src/lumen_tailscale_identity.cpp
//
// Implementation note: we shell out to `tailscale status --json` via
// popen() because Dart's `http` package can't speak Unix sockets and
// neither can boost::asio in Sunshine without significant work. The
// CLI is available on every platform Sunshine supports (macOS install
// paths, Linux pkg, Windows install dir).

#include "lumen_tailscale_identity.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace lumen {

  namespace pt = boost::property_tree;
  using clock_t = std::chrono::steady_clock;

  // ─── Cache ─────────────────────────────────────────────────────────────────

  namespace {
    constexpr auto k_cache_ttl = std::chrono::milliseconds(1000);

    struct cache_t {
      std::mutex mu;
      clock_t::time_point fetched_at;
      std::string raw_json;
      bool valid = false;
    };

    cache_t &cache() {
      static cache_t instance;
      return instance;
    }
  }  // namespace

  // ─── Locate the tailscale binary ───────────────────────────────────────────

  static std::string resolve_tailscale_path() {
    namespace fs = std::filesystem;
    static const std::array<const char *, 5> candidates = {
      "/usr/local/bin/tailscale",
      "/opt/homebrew/bin/tailscale",
      "/Applications/Tailscale.app/Contents/MacOS/Tailscale",
      "/usr/bin/tailscale",
      "C:\\Program Files\\Tailscale\\tailscale.exe",
    };
    for (const auto *p : candidates) {
      std::error_code ec;
      if (fs::exists(p, ec)) return p;
    }
    // Fall back to PATH lookup.
#ifdef _WIN32
    return "tailscale.exe";
#else
    return "tailscale";
#endif
  }

  // ─── Status fetch ────────────────────────────────────────────────────────
  //
  // The status JSON is identical to `tailscale status --json`. How we get
  // it differs by platform:
  //   - Windows: read it straight from the Tailscale LocalAPI over its
  //     named pipe. We must NOT shell out to the CLI here: when LumeN is
  //     installed as an MSIX package the host runs inside the app container,
  //     where spawning a child process (cmd.exe → tailscale.exe) hangs and
  //     the pairing admit gate blocks forever. A named pipe has no such
  //     restriction.
  //   - macOS/Linux: shell out to the CLI (the LocalAPI sits behind a Unix
  //     socket that's more work to speak, and there's no container issue).

#ifdef _WIN32
  // De-chunk an HTTP/1.1 `Transfer-Encoding: chunked` body.
  static std::string dechunk(const std::string &body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
      size_t eol = body.find("\r\n", pos);
      if (eol == std::string::npos) break;
      std::string size_hex = body.substr(pos, eol - pos);
      if (auto sc = size_hex.find(';'); sc != std::string::npos) {
        size_hex.resize(sc);  // drop chunk extensions
      }
      size_t chunk = 0;
      try {
        chunk = std::stoul(size_hex, nullptr, 16);
      } catch (...) {
        break;
      }
      if (chunk == 0) break;  // last chunk
      size_t data = eol + 2;
      if (data + chunk > body.size()) break;
      out.append(body, data, chunk);
      pos = data + chunk + 2;  // skip data + trailing CRLF
    }
    return out;
  }

  static std::optional<std::string> run_cli(const std::string & /*exec*/) {
    constexpr const wchar_t *kPipe =
      L"\\\\.\\pipe\\ProtectedPrefix\\Administrators\\Tailscale\\tailscaled";
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
      // SECURITY_IMPERSONATION lets tailscaled impersonate us to authorize
      // the LocalAPI call — without it the API replies 401.
      h = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING,
                      SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
      if (h != INVALID_HANDLE_VALUE) break;
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipe, 2000)) {
        BOOST_LOG_TRIVIAL(warning)
          << "lumen_tailscale: cannot open Tailscale LocalAPI pipe (err="
          << GetLastError() << ")";
        return std::nullopt;
      }
    }
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    static const std::string req =
      "GET /localapi/v0/status HTTP/1.1\r\n"
      "Host: local-tailscaled.sock\r\n"
      "Sec-Tailscale: localapi\r\n"
      "Connection: close\r\n\r\n";
    DWORD written = 0;
    if (!WriteFile(h, req.data(), static_cast<DWORD>(req.size()), &written,
                   nullptr)) {
      CloseHandle(h);
      return std::nullopt;
    }
    // Drain with PeekNamedPipe + a hard deadline so a slow/silent tailscaled
    // can never block the pairing thread indefinitely (the read-until-close
    // loop did). Stop at the chunked terminator / Content-Length.
    std::string resp;
    std::array<char, 8192> buf{};
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
      DWORD avail = 0;
      if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;
      if (avail == 0) {
        Sleep(10);
        continue;
      }
      DWORD got = 0;
      DWORD want = avail < buf.size() ? avail : static_cast<DWORD>(buf.size());
      if (!ReadFile(h, buf.data(), want, &got, nullptr) || got == 0) break;
      resp.append(buf.data(), got);
      if (resp.find("\r\n0\r\n\r\n") != std::string::npos) break;
      if (auto he = resp.find("\r\n\r\n"); he != std::string::npos) {
        if (auto cl = resp.find("Content-Length:");
            cl != std::string::npos && cl < he) {
          size_t len = std::stoul(resp.substr(cl + 15));
          if (resp.size() - (he + 4) >= len) break;
        }
      }
    }
    CloseHandle(h);

    auto hdr_end = resp.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return std::nullopt;
    std::string headers = resp.substr(0, hdr_end);
    std::string body = resp.substr(hdr_end + 4);
    if (headers.find(" 200") == std::string::npos) {
      BOOST_LOG_TRIVIAL(warning)
        << "lumen_tailscale: LocalAPI status non-200: "
        << headers.substr(0, headers.find("\r\n"));
      return std::nullopt;
    }
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
      body = dechunk(body);
    }
    return body;
  }
#else
  static std::optional<std::string> run_cli(const std::string &exec) {
    std::string cmd = "\"" + exec + "\" status --json 2>/dev/null";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::stringstream ss;
    std::array<char, 4096> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), pipe)) {
      ss.write(buf.data(), static_cast<std::streamsize>(n));
    }
    int rc = pclose(pipe);
    if (rc != 0) {
      BOOST_LOG_TRIVIAL(warning)
        << "lumen_tailscale: `" << exec << " status --json` exited " << rc;
      return std::nullopt;
    }
    return ss.str();
  }
#endif

  // ─── Refresh + parse ───────────────────────────────────────────────────────

  static bool refresh_cache_locked(cache_t &c) {
    auto exec = resolve_tailscale_path();
    auto out = run_cli(exec);
    if (!out) {
      c.valid = false;
      return false;
    }
    c.raw_json = std::move(*out);
    c.fetched_at = clock_t::now();
    c.valid = true;
    return true;
  }

  static bool ensure_cached() {
    auto &c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.valid && (clock_t::now() - c.fetched_at) < k_cache_ttl) {
      return true;
    }
    return refresh_cache_locked(c);
  }

  static tailscale_identity_t to_identity(const pt::ptree &peer) {
    tailscale_identity_t id;
    id.node_key = peer.get<std::string>("PublicKey", "");
    id.user = peer.get<std::string>("UserID", "");
    id.hostname = peer.get<std::string>("HostName", "");
    if (auto ips = peer.get_child_optional("TailscaleIPs")) {
      for (const auto &kv : *ips) {
        auto ip = kv.second.get_value<std::string>();
        if (!ip.empty()) {
          id.tailscale_ip = ip;
          break;
        }
      }
    }
    if (auto tags = peer.get_child_optional("Tags")) {
      for (const auto &kv : *tags) {
        id.tags.push_back(kv.second.get_value<std::string>());
      }
    }
    return id;
  }

  // ─── Public API ────────────────────────────────────────────────────────────

  std::optional<tailscale_identity_t> lookup_identity(const std::string &peer_ip) {
    if (!ensure_cached()) return std::nullopt;
    auto &c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    try {
      pt::ptree root;
      std::stringstream ss(c.raw_json);
      pt::read_json(ss, root);

      // Check Self first.
      if (auto self = root.get_child_optional("Self")) {
        auto id = to_identity(*self);
        if (id.tailscale_ip == peer_ip) return id;
      }
      // Then iterate Peer map (keyed by node-key).
      if (auto peers = root.get_child_optional("Peer")) {
        for (const auto &kv : *peers) {
          auto id = to_identity(kv.second);
          if (id.tailscale_ip == peer_ip) return id;
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG_TRIVIAL(warning)
        << "lumen_tailscale: parse failed: " << e.what();
    }
    return std::nullopt;
  }

  std::optional<tailscale_identity_t> lookup_self() {
    if (!ensure_cached()) return std::nullopt;
    auto &c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    try {
      pt::ptree root;
      std::stringstream ss(c.raw_json);
      pt::read_json(ss, root);
      if (auto self = root.get_child_optional("Self")) {
        return to_identity(*self);
      }
    } catch (...) {}
    return std::nullopt;
  }

  void invalidate_cache() {
    auto &c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    c.valid = false;
  }

}  // namespace lumen
