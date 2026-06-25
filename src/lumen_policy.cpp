// engine/host/upstream/src/lumen_policy.cpp

#include "lumen_policy.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <boost/log/trivial.hpp>

namespace lumen {

  std::string default_policy_path() {
    if (const char *env = std::getenv("LUMEN_POLICY_PATH")) {
      if (env[0] != '\0') return env;
    }
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif
    if (!home || home[0] == '\0') return ".lumen/policy.txt";
    return (std::filesystem::path(home) / ".lumen" / "policy.txt").string();
  }

  static std::string trim(const std::string &s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }

  policy_t load_policy(const std::string &path) {
    policy_t p;
    std::ifstream f(path);
    if (!f.is_open()) {
      BOOST_LOG_TRIVIAL(info)
        << "lumen_policy: " << path
        << " not found; defaulting to deny-all.";
      return p;  // default-deny
    }

    std::string line;
    while (std::getline(f, line)) {
      auto t = trim(line);
      if (t.empty() || t[0] == '#') continue;
      if (t == "default=admit") {
        p.default_admit = true;
      } else if (t == "default=deny") {
        p.default_admit = false;
      } else if (t.rfind("tag:", 0) == 0) {
        p.allow_tags.push_back(t);
      } else if (t.rfind("nodekey:", 0) == 0) {
        p.allow_node_keys.push_back(t);
      } else if (t.rfind("user:", 0) == 0) {
        p.allow_users.push_back(t.substr(5));
      } else {
        BOOST_LOG_TRIVIAL(warning)
          << "lumen_policy: ignoring unrecognised line: " << t;
      }
    }
    BOOST_LOG_TRIVIAL(info)
      << "lumen_policy: loaded from " << path
      << " (tags=" << p.allow_tags.size()
      << ", node_keys=" << p.allow_node_keys.size()
      << ", users=" << p.allow_users.size()
      << ", default=" << (p.default_admit ? "admit" : "deny") << ")";
    return p;
  }

  decision_t admit(const policy_t &policy, const tailscale_identity_t &id) {
    decision_t d;

    for (const auto &node : policy.allow_node_keys) {
      if (node == id.node_key) {
        d.admit = true;
        d.rule = "node:" + node;
        d.reason = "explicit node-key allowlist";
        return d;
      }
    }
    for (const auto &tag : policy.allow_tags) {
      if (std::find(id.tags.begin(), id.tags.end(), tag) != id.tags.end()) {
        d.admit = true;
        d.rule = tag;
        d.reason = "matching tag";
        return d;
      }
    }
    if (!policy.allow_users.empty()) {
      for (const auto &user : policy.allow_users) {
        if (user == id.user) {
          d.admit = true;
          d.rule = "user:" + user;
          d.reason = "matching tailnet user";
          return d;
        }
      }
    }
    d.admit = policy.default_admit;
    d.rule = "default";
    d.reason = policy.default_admit ? "default-admit" : "default-deny";
    return d;
  }

}  // namespace lumen
