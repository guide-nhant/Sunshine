// engine/host/upstream/src/lumen_policy.h
//
// LumeN local policy: a small text-format allowlist that decides
// whether a Tailscale-identified peer is admitted by this host without
// going through the PIN-pairing dance.
//
// Default location: ~/.lumen/policy.txt (overridable via the
// `lumen_policy_path` config key). Format documented in
// engine/host/POLICY.md (see PAIRING-MAP.md).

#pragma once

#include <string>
#include <vector>

#include "lumen_tailscale_identity.h"

namespace lumen {

  struct policy_t {
    // Allow any peer whose tags include at least one of these.
    std::vector<std::string> allow_tags;
    // Allow specific node keys (escape hatch for untagged personal devices).
    std::vector<std::string> allow_node_keys;
    // If non-empty, only peers belonging to these tailnet users are admitted.
    // Empty means "any user in the tailnet".
    std::vector<std::string> allow_users;
    // Default decision when no rule matches. Today: "deny" (safer).
    bool default_admit = false;
  };

  // Loads policy from `path`. Returns a default-deny policy if the file
  // is missing or unreadable (which is the conservative safe behaviour
  // when LumeN has just been installed and no admin config exists yet).
  policy_t load_policy(const std::string &path);

  // Conventional location of the LumeN policy file:
  //   $LUMEN_POLICY_PATH if set, else
  //   $HOME/.lumen/policy.txt on POSIX, %USERPROFILE%\.lumen\policy.txt on Windows.
  std::string default_policy_path();

  // Decides admission. Pure function over (policy, identity).
  // Returns the matching rule as a human-readable string for the audit
  // log; empty string when default branch ran.
  struct decision_t {
    bool admit = false;
    std::string rule;       // "tag:lumen-client" | "node:nodekey:..." | "default"
    std::string reason;     // "matched", "no rule matched", ...
  };

  decision_t admit(const policy_t &policy, const tailscale_identity_t &id);

}  // namespace lumen
