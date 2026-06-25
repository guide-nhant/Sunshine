// engine/host/upstream/src/lumen_tailscale_identity.cpp
//
// Implementation note: we shell out to `tailscale status --json` via
// popen() because Dart's `http` package can't speak Unix sockets and
// neither can boost::asio in Sunshine without significant work. The
// CLI is available on every platform Sunshine supports (macOS install
// paths, Linux pkg, Windows install dir).

#include "lumen_tailscale_identity.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>

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

  // ─── popen helper ──────────────────────────────────────────────────────────

  static std::optional<std::string> run_cli(const std::string &exec) {
    // We avoid std::system / boost::process to keep dependencies tight.
    // The stderr sink differs per platform: cmd.exe has no /dev/null, and
    // redirecting to it fails the whole command ("The system cannot find
    // the path specified"), which leaves the host with no tailnet identity
    // and denies every pairing on Windows. Use NUL there.
#ifdef _WIN32
    std::string cmd = "\"" + exec + "\" status --json 2>NUL";
    FILE *pipe = _popen(cmd.c_str(), "r");
#else
    std::string cmd = "\"" + exec + "\" status --json 2>/dev/null";
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return std::nullopt;

    std::stringstream ss;
    std::array<char, 4096> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), pipe)) {
      ss.write(buf.data(), static_cast<std::streamsize>(n));
    }
#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif
    if (rc != 0) {
      BOOST_LOG_TRIVIAL(warning)
        << "lumen_tailscale: `" << exec << " status --json` exited " << rc;
      return std::nullopt;
    }
    return ss.str();
  }

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
