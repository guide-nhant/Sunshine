// engine/host/upstream/src/lumen_tailscale_identity.h
//
// LumeN fork addition: queries the local Tailscale daemon (via the
// `tailscale status --json` CLI) to map a peer's IP back to its
// Tailscale identity (node key + user + hostname + tags).
//
// This is the data source for the Tailscale-native auth path that
// ADR 0003 commits to. Until US-005 Step 2b wires the result into
// nvhttp.cpp's pair() flow, the module is hooked up only to logging.

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lumen {

  struct tailscale_identity_t {
    std::string node_key;     // "nodekey:..."
    std::string user;         // "alice@guide.inc"
    std::string hostname;     // "MacBook-Alice"
    std::vector<std::string> tags;  // ["tag:lumen-client", ...]
    std::string tailscale_ip;       // "100.x.y.z"
  };

  // Look up the Tailscale identity for a peer IP. Returns nullopt if
  // the local daemon is unreachable, the IP isn't in the tailnet, or
  // any parsing failure. Never throws.
  //
  // Caches the parsed `tailscale status --json` for ~1 second so a
  // burst of lookups (e.g., one per HTTP request during a session
  // start) doesn't spawn 30 subprocesses.
  std::optional<tailscale_identity_t> lookup_identity(const std::string &peer_ip);

  // Convenience: returns the local node's identity (the "Self" peer).
  // Used at startup to log who LumeN's host is from the daemon's POV.
  std::optional<tailscale_identity_t> lookup_self();

  // Forces the next lookup to re-query the daemon (drops the cache).
  // Mainly for tests / explicit refresh.
  void invalidate_cache();

}  // namespace lumen
