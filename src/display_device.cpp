/**
 * @file src/display_device.cpp
 * @brief Definitions for display device handling.
 */
// header include
#include "display_device.h"

// lib includes
#include <boost/algorithm/string.hpp>
#include <display_device/audio_context_interface.h>
#include <display_device/file_settings_persistence.h>
#include <display_device/json.h>
#include <display_device/retry_scheduler.h>
#include <display_device/settings_manager_interface.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <regex>

// local includes
#include "audio.h"
#include "platform/common.h"
#include "rtsp.h"

// platform-specific includes
#ifdef _WIN32
  #include <display_device/windows/settings_manager.h>
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <objbase.h>  // CoCreateGuid
  #include <dxgi.h>  // adapter enumeration for the render-GPU pick
  #include <wrl/client.h>
  // LumeN: per-session SudoVDA virtual display control (ADR 0025 / US-076).
  #include "platform/windows/virtual_display.h"
#endif

namespace display_device {
  namespace {
    constexpr std::chrono::milliseconds DEFAULT_RETRY_INTERVAL {5000};

    /**
     * @brief A global for the settings manager interface and other settings whose lifetime is managed by `display_device::init(...)`.
     */
    struct {
      std::mutex mutex {};
      std::chrono::milliseconds config_revert_delay {0};
      std::unique_ptr<RetryScheduler<SettingsManagerInterface>> sm_instance {nullptr};
    } DD_DATA;

#ifdef _WIN32
    /**
     * @brief LumeN per-session SudoVDA virtual display state (ADR 0025 /
     *        US-076). Guarded by DD_DATA.mutex. The monitor is created at
     *        the client's mode on configure_display and removed on
     *        revert_configuration; map_output_name("virtual") hands the
     *        capture layer the GDI name stored here.
     */
    struct {
      bool active {false};
      bool device_open {false};
      GUID guid {};
      std::wstring gdi_name;
    } VDD_STATE;

    /// Append a line to a crash-surviving SudoVDA log (sunshine.log is
    /// truncated on each host restart, so a host crash loses the evidence).
    /// `%USERPROFILE%\AppData\Local\LumeN\host\sudovda-lumen.log`.
    void lumen_vdd_log(const std::string &line) {
      BOOST_LOG(info) << "SudoVDA: " << line;
      if (const char *profile = std::getenv("USERPROFILE")) {
        try {
          std::ofstream f {std::string(profile) + "\\AppData\\Local\\LumeN\\host\\sudovda-lumen.log",
                           std::ios::app};
          f << line << '\n';
        } catch (...) {
        }
      }
    }

    /// Pick the GPU SudoVDA should render the virtual display on. Order:
    ///   1. the encoder adapter the user configured (`config::video.adapter_name`),
    ///   2. the adapter that actually drives an output (matches the GPU the
    ///      QSV/NVENC encoder captures from — critical on hybrid laptops, or
    ///      capture reads a display rendered on the wrong GPU and crashes),
    ///   3. the first hardware adapter.
    std::wstring lumen_pick_render_adapter() {
      std::wstring configured;
      {
        const auto &an = config::video.adapter_name;
        configured.assign(an.begin(), an.end());  // ASCII widen
      }
      Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return configured;
      }
      Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
      std::wstring configured_match, with_output, first_hw;
      for (UINT i = 0; SUCCEEDED(factory->EnumAdapters(i, adapter.ReleaseAndGetAddressOf())); ++i) {
        DXGI_ADAPTER_DESC desc {};
        if (FAILED(adapter->GetDesc(&desc))) {
          continue;
        }
        std::wstring name {desc.Description};
        if (name.find(L"Basic") != std::wstring::npos) {
          continue;  // skip Microsoft Basic Render Driver
        }
        if (!configured.empty() && name == configured) {
          configured_match = name;
        }
        if (first_hw.empty()) {
          first_hw = name;
        }
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (with_output.empty() && SUCCEEDED(adapter->EnumOutputs(0, output.GetAddressOf()))) {
          with_output = name;  // this GPU drives a physical display
        }
      }
      if (!configured_match.empty()) {
        return configured_match;
      }
      return !with_output.empty() ? with_output : first_hw;
    }

    /// Create (or replace) the SudoVDA monitor at the session's mode.
    /// Call with DD_DATA.mutex held.
    void lumen_create_virtual_display_unlocked(const rtsp_stream::launch_session_t &session) {
      using namespace VDISPLAY;
      if (!VDD_STATE.device_open) {
        if (openVDisplayDevice() != DRIVER_STATUS::OK) {
          lumen_vdd_log("openVDisplayDevice failed; virtual display unavailable");
          return;
        }
        VDD_STATE.device_open = true;
        startPingThread([]() {
          BOOST_LOG(warning) << "SudoVDA watchdog ping failed";
        });
      }

      // Set the render GPU before adding the display. Must match the GPU the
      // encoder captures from (hybrid laptops: capturing a display rendered
      // on the other GPU crashes the host), so pick the output-driving GPU.
      const auto render_adapter {lumen_pick_render_adapter()};
      if (!render_adapter.empty()) {
        const bool ok {setRenderAdapterByName(render_adapter)};
        lumen_vdd_log("render adapter '" +
                      std::string(render_adapter.begin(), render_adapter.end()) +
                      "' set=" + (ok ? "true" : "false"));
      } else {
        lumen_vdd_log("no render adapter found to set");
      }

      if (VDD_STATE.active) {
        removeVirtualDisplay(VDD_STATE.guid);
        VDD_STATE.active = false;
        VDD_STATE.gdi_name.clear();
      }
      GUID guid {};
      if (CoCreateGuid(&guid) != S_OK) {
        lumen_vdd_log("CoCreateGuid failed");
        return;
      }
      const uint32_t w {session.width > 0 ? static_cast<uint32_t>(session.width) : 1920u};
      const uint32_t h {session.height > 0 ? static_cast<uint32_t>(session.height) : 1080u};
      const uint32_t fps {session.fps > 0 ? static_cast<uint32_t>(session.fps) : 60u};
      // SudoVDA's RefreshRate field is in **milli-Hz**, not Hz (Apollo passes
      // fps * 1000). Passing Hz here yields an invalid mode and AddVirtualDisplay
      // fails.
      const uint32_t refresh_mhz {fps * 1000u};
      // Unique EDID serial per session so a re-create never collides with a
      // lingering monitor (ERROR_ALREADY_EXISTS / 183). Fits SudoVDA's 13-char
      // SerialNumber field.
      char serial[14];
      std::snprintf(serial, sizeof(serial), "LN%08lX%03X",
                    static_cast<unsigned long>(guid.Data1),
                    static_cast<unsigned>(guid.Data2 & 0xFFF));
      const std::string mode {std::to_string(w) + "x" + std::to_string(h) + "@" + std::to_string(fps)};
      lumen_vdd_log("createVirtualDisplay begin " + mode + " serial=" + serial);
      // Track for removal BEFORE the call: the monitor can be created even if
      // we fail to read its name back (Apollo tracks it regardless), so it must
      // be torn down on session end no matter what.
      VDD_STATE.guid = guid;
      VDD_STATE.active = true;
      VDD_STATE.gdi_name.clear();
      auto name {createVirtualDisplay(serial, "LumeN", w, h, refresh_mhz, guid)};
      if (name.empty()) {
        lumen_vdd_log("createVirtualDisplay returned empty " + mode +
                      " (GetLastError=" + std::to_string(GetLastError()) +
                      "); tracked for removal, capture falls back to primary");
        return;
      }
      VDD_STATE.gdi_name = std::move(name);
      lumen_vdd_log("virtual display created " + mode + " -> " +
                    std::string(VDD_STATE.gdi_name.begin(), VDD_STATE.gdi_name.end()));
    }

    /// Remove the active SudoVDA monitor. Call with DD_DATA.mutex held.
    void lumen_remove_virtual_display_unlocked() {
      using namespace VDISPLAY;
      if (VDD_STATE.active) {
        const bool ok {removeVirtualDisplay(VDD_STATE.guid)};
        VDD_STATE.active = false;
        VDD_STATE.gdi_name.clear();
        lumen_vdd_log(std::string("virtual display removed ok=") + (ok ? "true" : "false"));
      }
    }
#endif

    /**
     * @brief Helper class for capturing audio context when the API demands it.
     *
     * The capture is needed to be done in case some of the displays are going
     * to be deactivated before the stream starts. In this case the audio context
     * will be captured for this display and can be restored once it is turned back.
     */
    class sunshine_audio_context_t: public AudioContextInterface {
    public:
      [[nodiscard]] bool capture() override {
        return context_scheduler.execute([](auto &audio_context) {
          // Explicitly releasing the context first in case it was not release yet so that it can be potentially cleaned up.
          audio_context = boost::none;
          audio_context = audio_context_t {};

          // Always say that we have captured it successfully as otherwise the settings change procedure will be aborted.
          return true;
        });
      }

      [[nodiscard]] bool isCaptured() const override {
        return context_scheduler.execute([](const auto &audio_context) {
          if (audio_context) {
            // In case we still have context we need to check whether it was released or not.
            // If it was released we can pretend that we no longer have it as it will be immediately cleaned up in `capture` method before we acquire new context.
            return !audio_context->released;
          }

          return false;
        });
      }

      void release() override {
        context_scheduler.schedule([](auto &audio_context, auto &stop_token) {
          if (audio_context) {
            audio_context->released = true;

            const auto *audio_ctx_ptr = audio_context->audio_ctx_ref.get();
            if (audio_ctx_ptr && !audio::is_audio_ctx_sink_available(*audio_ctx_ptr) && audio_context->retry_counter > 0) {
              // It is possible that the audio sink is not immediately available after the display is turned on.
              // Therefore, we will hold on to the audio context a little longer, until it is either available
              // or we time out.
              --audio_context->retry_counter;
              return;
            }
          }

          audio_context = boost::none;
          stop_token.requestStop();
        },
                                   SchedulerOptions {.m_sleep_durations = {2s}});
      }

    private:
      struct audio_context_t {
        /**
         * @brief A reference to the audio context that will automatically extend the audio session.
         * @note It is auto-initialized here for convenience.
         */
        decltype(audio::get_audio_ctx_ref()) audio_ctx_ref {audio::get_audio_ctx_ref()};

        /**
         * @brief Will be set to true if the capture was released, but we still have to keep the context around, because the device is not available.
         */
        bool released {false};

        /**
         * @brief How many times to check if the audio sink is available before giving up.
         */
        int retry_counter {15};
      };

      RetryScheduler<boost::optional<audio_context_t>> context_scheduler {std::make_unique<boost::optional<audio_context_t>>(boost::none)};
    };

    /**
     * @brief Convert string to unsigned int.
     * @note For random reason there is std::stoi, but not std::stou...
     * @param value String to be converted
     * @return Parsed unsigned integer.
     */
    unsigned int stou(const std::string &value) {
      unsigned long result {std::stoul(value)};
      if (result > std::numeric_limits<unsigned int>::max()) {
        throw std::out_of_range("stou");
      }
      return (int) result;
    }

    /**
     * @brief Parse resolution value from the string.
     * @param input String to be parsed.
     * @param output Reference to output variable to fill in.
     * @returns True on successful parsing (empty string allowed), false otherwise.
     *
     * @examples
     * std::optional<Resolution> resolution;
     * if (parse_resolution_string("1920x1080", resolution)) {
     *   if (resolution) {
     *     BOOST_LOG(info) << "Value was specified";
     *   }
     *   else {
     *     BOOST_LOG(info) << "Value was empty";
     *   }
     * }
     * @examples_end
     */
    bool parse_resolution_string(const std::string &input, std::optional<Resolution> &output) {
      const std::string trimmed_input {boost::algorithm::trim_copy(input)};
      const std::regex resolution_regex {R"(^(\d+)x(\d+)$)"};

      if (std::smatch match; std::regex_match(trimmed_input, match, resolution_regex)) {
        try {
          output = Resolution {
            stou(match[1].str()),
            stou(match[2].str())
          };
          return true;
        } catch (const std::out_of_range &) {
          BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << " (number out of range).";
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << ":\n"
                           << err.what();
        }
      } else {
        if (trimmed_input.empty()) {
          output = std::nullopt;
          return true;
        }

        BOOST_LOG(error) << "Failed to parse resolution string " << trimmed_input << R"(. It must match a "1920x1080" pattern!)";
      }

      return false;
    }

    /**
     * @brief Parse refresh rate value from the string.
     * @param input String to be parsed.
     * @param output Reference to output variable to fill in.
     * @param allow_decimal_point Specify whether the decimal point is allowed or not.
     * @returns True on successful parsing (empty string allowed), false otherwise.
     *
     * @examples
     * std::optional<FloatingPoint> refresh_rate;
     * if (parse_refresh_rate_string("59.95", refresh_rate)) {
     *   if (refresh_rate) {
     *     BOOST_LOG(info) << "Value was specified";
     *   }
     *   else {
     *     BOOST_LOG(info) << "Value was empty";
     *   }
     * }
     * @examples_end
     */
    bool parse_refresh_rate_string(const std::string &input, std::optional<FloatingPoint> &output, const bool allow_decimal_point = true) {
      static const auto is_zero {[](const auto &character) {
        return character == '0';
      }};
      const std::string trimmed_input {boost::algorithm::trim_copy(input)};
      const std::regex refresh_rate_regex {allow_decimal_point ? R"(^(\d+)(?:\.(\d+))?$)" : R"(^(\d+)$)"};

      if (std::smatch match; std::regex_match(trimmed_input, match, refresh_rate_regex)) {
        try {
          // Here we are trimming zeros from the string to possibly reduce out of bounds case
          std::string trimmed_match_1 {boost::algorithm::trim_left_copy_if(match[1].str(), is_zero)};
          if (trimmed_match_1.empty()) {
            trimmed_match_1 = "0"s;  // Just in case ALL the string is full of zeros, we want to leave one
          }

          std::string trimmed_match_2;
          if (allow_decimal_point && match[2].matched) {
            trimmed_match_2 = boost::algorithm::trim_right_copy_if(match[2].str(), is_zero);
          }

          if (!trimmed_match_2.empty()) {
            // We have a decimal point and will have to split it into numerator and denominator.
            // For example:
            //   59.995:
            //     numerator = 59995
            //     denominator = 1000

            // We are essentially removing the decimal point here: 59.995 -> 59995
            const std::string numerator_str {trimmed_match_1 + trimmed_match_2};
            const auto numerator {stou(numerator_str)};

            // Here we are counting decimal places and calculating denominator: 10^decimal_places
            const auto denominator {static_cast<unsigned int>(std::pow(10, trimmed_match_2.size()))};

            output = Rational {numerator, denominator};
          } else {
            // We do not have a decimal point, just a valid number.
            // For example:
            //   60:
            //     numerator = 60
            //     denominator = 1
            output = Rational {stou(trimmed_match_1), 1};
          }
          return true;
        } catch (const std::out_of_range &) {
          BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << " (number out of range).";
        } catch (const std::exception &err) {
          BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << ":\n"
                           << err.what();
        }
      } else {
        if (trimmed_input.empty()) {
          output = std::nullopt;
          return true;
        }

        BOOST_LOG(error) << "Failed to parse refresh rate string " << trimmed_input << ". Must have a pattern of " << (allow_decimal_point ? R"("123" or "123.456")" : R"("123")") << "!";
      }

      return false;
    }

    /**
     * @brief Parse device preparation option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @returns Parsed device preparation value we need to use.
     *          Empty optional if no preparation nor configuration shall take place.
     *
     * @examples
     * const config::video_t &video_config { config::video };
     * const auto device_prep_option = parse_device_prep_option(video_config);
     * @examples_end
     */
    std::optional<SingleDisplayConfiguration::DevicePreparation> parse_device_prep_option(const config::video_t &video_config) {
      using enum config::video_t::dd_t::config_option_e;
      using enum SingleDisplayConfiguration::DevicePreparation;

      switch (video_config.dd.configuration_option) {
        case verify_only:
          return VerifyOnly;
        case ensure_active:
          return EnsureActive;
        case ensure_primary:
          return EnsurePrimary;
        case ensure_only_display:
          return EnsureOnlyDisplay;
        case disabled:
          break;
      }

      return std::nullopt;
    }

    /**
     * @brief Parse resolution option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a display config object that will be modified on success.
     * @returns True on successful parsing, false otherwise.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = parse_resolution_option(video_config, *launch_session, config);
     * @examples_end
     */
    bool parse_resolution_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      using resolution_option_e = config::video_t::dd_t::resolution_option_e;

      switch (video_config.dd.resolution_option) {
        case resolution_option_e::automatic:
          {
            if (!session.enable_sops) {
              BOOST_LOG(warning) << R"(Sunshine is configured to change resolution automatically, but the "Optimize game settings" is not set in the client! Resolution will not be changed.)";
            } else if (session.width >= 0 && session.height >= 0) {
              config.m_resolution = Resolution {
                static_cast<unsigned int>(session.width),
                static_cast<unsigned int>(session.height)
              };
            } else {
              BOOST_LOG(error) << "Resolution provided by client session config is invalid: " << session.width << "x" << session.height;
              return false;
            }
            break;
          }
        case resolution_option_e::manual:
          {
            if (!session.enable_sops) {
              BOOST_LOG(warning) << R"(Sunshine is configured to change resolution manually, but the "Optimize game settings" is not set in the client! Resolution will not be changed.)";
            } else {
              if (!parse_resolution_string(video_config.dd.manual_resolution, config.m_resolution)) {
                BOOST_LOG(error) << "Failed to parse manual resolution string!";
                return false;
              }

              if (!config.m_resolution) {
                BOOST_LOG(error) << "Manual resolution must be specified!";
                return false;
              }
            }
            break;
          }
        case resolution_option_e::disabled:
          break;
      }

      return true;
    }

    /**
     * @brief Parse refresh rate option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a config object that will be modified on success.
     * @returns True on successful parsing, false otherwise.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = parse_refresh_rate_option(video_config, *launch_session, config);
     * @examples_end
     */
    bool parse_refresh_rate_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      using refresh_rate_option_e = config::video_t::dd_t::refresh_rate_option_e;

      switch (video_config.dd.refresh_rate_option) {
        case refresh_rate_option_e::automatic:
          {
            if (session.fps >= 0) {
              config.m_refresh_rate = Rational {static_cast<unsigned int>(session.fps), 1};
            } else {
              BOOST_LOG(error) << "FPS value provided by client session config is invalid: " << session.fps;
              return false;
            }
            break;
          }
        case refresh_rate_option_e::manual:
          {
            if (!parse_refresh_rate_string(video_config.dd.manual_refresh_rate, config.m_refresh_rate)) {
              BOOST_LOG(error) << "Failed to parse manual refresh rate string!";
              return false;
            }

            if (!config.m_refresh_rate) {
              BOOST_LOG(error) << "Manual refresh rate must be specified!";
              return false;
            }
            break;
          }
        case refresh_rate_option_e::disabled:
          break;
      }

      return true;
    }

    /**
     * @brief Parse HDR option from the user configuration and the session information.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @returns Parsed HDR state value we need to switch to.
     *          Empty optional if no action is required.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     * const auto hdr_option = parse_hdr_option(video_config, *launch_session);
     * @examples_end
     */
    std::optional<HdrState> parse_hdr_option(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
      using hdr_option_e = config::video_t::dd_t::hdr_option_e;

      switch (video_config.dd.hdr_option) {
        case hdr_option_e::automatic:
          return session.enable_hdr ? HdrState::Enabled : HdrState::Disabled;
        case hdr_option_e::disabled:
          break;
      }

      return std::nullopt;
    }

    /**
     * @brief Indicates which remapping fields and config structure shall be used.
     */
    enum class remapping_type_e {
      mixed,  ///! Both reseolution and refresh rate may be remapped
      resolution_only,  ///! Only resolution will be remapped
      refresh_rate_only  ///! Only refresh rate will be remapped
    };

    /**
     * @brief Determine the ramapping type from the user config.
     * @param video_config User's video related configuration.
     * @returns Enum value if remapping can be performed, null optional if remapping shall be skipped.
     */
    std::optional<remapping_type_e> determine_remapping_type(const config::video_t &video_config) {
      using dd_t = config::video_t::dd_t;
      const bool auto_resolution {video_config.dd.resolution_option == dd_t::resolution_option_e::automatic};
      const bool auto_refresh_rate {video_config.dd.refresh_rate_option == dd_t::refresh_rate_option_e::automatic};

      if (auto_resolution && auto_refresh_rate) {
        return remapping_type_e::mixed;
      }

      if (auto_resolution) {
        return remapping_type_e::resolution_only;
      }

      if (auto_refresh_rate) {
        return remapping_type_e::refresh_rate_only;
      }

      return std::nullopt;
    }

    /**
     * @brief Contains remapping data parsed from the string values.
     */
    struct parsed_remapping_entry_t {
      std::optional<Resolution> requested_resolution;
      std::optional<FloatingPoint> requested_fps;
      std::optional<Resolution> final_resolution;
      std::optional<FloatingPoint> final_refresh_rate;
    };

    /**
     * @brief Check if resolution is to be mapped based on remmaping type.
     * @param type Remapping type to check.
     * @returns True if resolution is to be mapped, false otherwise.
     */
    bool is_resolution_mapped(const remapping_type_e type) {
      return type == remapping_type_e::resolution_only || type == remapping_type_e::mixed;
    }

    /**
     * @brief Check if FPS is to be mapped based on remmaping type.
     * @param type Remapping type to check.
     * @returns True if FPS is to be mapped, false otherwise.
     */
    bool is_fps_mapped(const remapping_type_e type) {
      return type == remapping_type_e::refresh_rate_only || type == remapping_type_e::mixed;
    }

    /**
     * @brief Parse the remapping entry from the config into an internal structure.
     * @param entry Entry to parse.
     * @param type Specify which entry fields should be parsed.
     * @returns Parsed structure or null optional if a necessary field could not be parsed.
     */
    std::optional<parsed_remapping_entry_t> parse_remapping_entry(const config::video_t::dd_t::mode_remapping_entry_t &entry, const remapping_type_e type) {
      parsed_remapping_entry_t result {};

      if (is_resolution_mapped(type) && (!parse_resolution_string(entry.requested_resolution, result.requested_resolution) ||
                                         !parse_resolution_string(entry.final_resolution, result.final_resolution))) {
        return std::nullopt;
      }

      if (is_fps_mapped(type) && (!parse_refresh_rate_string(entry.requested_fps, result.requested_fps, false) ||
                                  !parse_refresh_rate_string(entry.final_refresh_rate, result.final_refresh_rate))) {
        return std::nullopt;
      }

      return result;
    }

    /**
     * @brief Remap the the requested display mode based on the config.
     * @param video_config User's video related configuration.
     * @param session Session information.
     * @param config A reference to a config object that will be modified on success.
     * @returns True if the remapping was performed or skipped, false if remapping has failed due to invalid config.
     *
     * @examples
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session;
     * const config::video_t &video_config { config::video };
     *
     * SingleDisplayConfiguration config;
     * const bool success = remap_display_mode_if_needed(video_config, *launch_session, config);
     * @examples_end
     */
    bool remap_display_mode_if_needed(const config::video_t &video_config, const rtsp_stream::launch_session_t &session, SingleDisplayConfiguration &config) {
      const auto remapping_type {determine_remapping_type(video_config)};
      if (!remapping_type) {
        return true;
      }

      const auto &remapping_list {[&]() {
        using enum remapping_type_e;

        switch (*remapping_type) {
          case resolution_only:
            return video_config.dd.mode_remapping.resolution_only;
          case refresh_rate_only:
            return video_config.dd.mode_remapping.refresh_rate_only;
          case mixed:
          default:
            return video_config.dd.mode_remapping.mixed;
        }
      }()};

      if (remapping_list.empty()) {
        BOOST_LOG(debug) << "No values are available for display mode remapping.";
        return true;
      }
      BOOST_LOG(debug) << "Trying to remap display modes...";

      const auto entry_to_string {[type = *remapping_type](const config::video_t::dd_t::mode_remapping_entry_t &entry) {
        const bool mapping_resolution {is_resolution_mapped(type)};
        const bool mapping_fps {is_fps_mapped(type)};

        // clang-format off
        return (mapping_resolution ? "  - requested resolution: "s + entry.requested_resolution + "\n" : "") +
               (mapping_fps ?        "  - requested FPS: "s + entry.requested_fps + "\n" : "") +
               (mapping_resolution ? "  - final resolution: "s + entry.final_resolution + "\n" : "") +
               (mapping_fps ?        "  - final refresh rate: "s + entry.final_refresh_rate : "");
        // clang-format on
      }};

      for (const auto &entry : remapping_list) {
        const auto parsed_entry {parse_remapping_entry(entry, *remapping_type)};
        if (!parsed_entry) {
          BOOST_LOG(error) << "Failed to parse remapping entry from:\n"
                           << entry_to_string(entry);
          return false;
        }

        if (!parsed_entry->final_resolution && !parsed_entry->final_refresh_rate) {
          BOOST_LOG(error) << "At least one final value must be set for remapping display modes! Entry:\n"
                           << entry_to_string(entry);
          return false;
        }

        if (!session.enable_sops && (parsed_entry->requested_resolution || parsed_entry->final_resolution)) {
          BOOST_LOG(warning) << R"(Skipping remapping entry, because the "Optimize game settings" is not set in the client! Entry:\n)"
                             << entry_to_string(entry);
          continue;
        }

        // Note: at this point config should already have parsed resolution set.
        if (parsed_entry->requested_resolution && parsed_entry->requested_resolution != config.m_resolution) {
          BOOST_LOG(verbose) << "Skipping remapping because requested resolutions do not match! Entry:\n"
                             << entry_to_string(entry);
          continue;
        }

        // Note: at this point config should already have parsed refresh rate set.
        if (parsed_entry->requested_fps && parsed_entry->requested_fps != config.m_refresh_rate) {
          BOOST_LOG(verbose) << "Skipping remapping because requested FPS do not match! Entry:\n"
                             << entry_to_string(entry);
          continue;
        }

        BOOST_LOG(info) << "Remapping requested display mode. Entry:\n"
                        << entry_to_string(entry);
        if (parsed_entry->final_resolution) {
          config.m_resolution = parsed_entry->final_resolution;
        }
        if (parsed_entry->final_refresh_rate) {
          config.m_refresh_rate = parsed_entry->final_refresh_rate;
        }
        break;
      }

      return true;
    }

    /**
     * @brief Construct a settings manager interface to manage display device settings.
     * @param persistence_filepath File location for saving persistent state.
     * @param video_config User's video related configuration.
     * @return An interface or nullptr if the OS does not support the interface.
     */
    std::unique_ptr<SettingsManagerInterface> make_settings_manager([[maybe_unused]] const std::filesystem::path &persistence_filepath, [[maybe_unused]] const config::video_t &video_config) {
#ifdef _WIN32
      return std::make_unique<SettingsManager>(
        std::make_shared<WinDisplayDevice>(std::make_shared<WinApiLayer>()),
        std::make_shared<sunshine_audio_context_t>(),
        std::make_unique<PersistentState>(
          std::make_shared<FileSettingsPersistence>(persistence_filepath)
        ),
        WinWorkarounds {
          .m_hdr_blank_delay = video_config.dd.wa.hdr_toggle_delay != std::chrono::milliseconds::zero() ? std::make_optional(video_config.dd.wa.hdr_toggle_delay) : std::nullopt
        }
      );
#else
      return nullptr;
#endif
    }

    /**
     * @brief Defines the "revert config" algorithms.
     */
    enum class revert_option_e {
      try_once,  ///< Try reverting once and then abort.
      try_indefinitely,  ///< Keep trying to revert indefinitely.
      try_indefinitely_with_delay  ///< Keep trying to revert indefinitely, but delay the first try by some amount of time.
    };

    /**
     * @brief Reverts the configuration based on the provided option.
     * @note This is function does not lock mutex.
     */
    void revert_configuration_unlocked(const revert_option_e option) {
      if (!DD_DATA.sm_instance) {
        // Platform is not supported, nothing to do.
        return;
      }

      // Note: by default the executor function is immediately executed in the calling thread. With delay, we want to avoid that.
      SchedulerOptions scheduler_option {.m_sleep_durations = {DEFAULT_RETRY_INTERVAL}};
      if (option == revert_option_e::try_indefinitely_with_delay && DD_DATA.config_revert_delay > std::chrono::milliseconds::zero()) {
        scheduler_option.m_sleep_durations = {DD_DATA.config_revert_delay, DEFAULT_RETRY_INTERVAL};
        scheduler_option.m_execution = SchedulerOptions::Execution::ScheduledOnly;
      }

      DD_DATA.sm_instance->schedule([try_once = (option == revert_option_e::try_once), tried_out_devices = StringSet {}](auto &settings_iface, auto &stop_token) mutable {
        if (try_once) {
          std::ignore = settings_iface.revertSettings();
          stop_token.requestStop();
          return;
        }

        auto available_devices {[&settings_iface]() {
          const auto devices {settings_iface.enumAvailableDevices()};
          StringSet parsed_devices;

          std::transform(
            std::begin(devices),
            std::end(devices),
            std::inserter(parsed_devices, std::end(parsed_devices)),
            [](const auto &device) {
              return device.m_device_id + " - " + device.m_friendly_name;
            }
          );

          return parsed_devices;
        }()};
        if (available_devices == tried_out_devices) {
          BOOST_LOG(debug) << "Skipping reverting configuration, because no newly added/removed devices were detected since last check. Currently available devices:\n"
                           << toJson(available_devices);
          return;
        }

        using enum SettingsManagerInterface::RevertResult;
        if (const auto result {settings_iface.revertSettings()}; result == Ok) {
          stop_token.requestStop();
          return;
        } else if (result == ApiTemporarilyUnavailable) {
          // Do nothing and retry next time
          return;
        }

        // If we have failed to revert settings then we will try to do it next time only if a device was added/removed
        BOOST_LOG(warning) << "Failed to revert display device configuration (will retry once devices are added or removed). Enabling all of the available devices:\n"
                           << toJson(available_devices);
        tried_out_devices.swap(available_devices);
      },
                                    scheduler_option);
    }
  }  // namespace

  std::unique_ptr<platf::deinit_t> init(const std::filesystem::path &persistence_filepath, const config::video_t &video_config) {
    std::lock_guard lock {DD_DATA.mutex};
    // We can support re-init without any issues, however we should make sure to clean up first!
    revert_configuration_unlocked(revert_option_e::try_once);
    DD_DATA.config_revert_delay = video_config.dd.config_revert_delay;
    DD_DATA.sm_instance = nullptr;

    // If we fail to create settings manager, this means platform is not supported, and
    // we will need to provided error-free pass-trough in other methods
    if (auto settings_manager {make_settings_manager(persistence_filepath, video_config)}) {
      DD_DATA.sm_instance = std::make_unique<RetryScheduler<SettingsManagerInterface>>(std::move(settings_manager));

      const auto available_devices {DD_DATA.sm_instance->execute([](auto &settings_iface) {
        return settings_iface.enumAvailableDevices();
      })};
      BOOST_LOG(info) << "Currently available display devices:\n"
                      << toJson(available_devices);

      // In case we have failed to revert configuration before shutting down, we should
      // do it now.
      revert_configuration_unlocked(revert_option_e::try_indefinitely);
    }

    class deinit_t: public platf::deinit_t {
    public:
      ~deinit_t() override {
        std::lock_guard lock {DD_DATA.mutex};
        try {
          // This may throw if used incorrectly. At the moment this will not happen, however
          // in case some unforeseen changes are made that could raise an exception,
          // we definitely don't want this to happen in destructor. Especially in the
          // deinit_t where the outcome does not really matter.
          revert_configuration_unlocked(revert_option_e::try_once);
        } catch (std::exception &err) {
          BOOST_LOG(fatal) << err.what();
        }

        DD_DATA.sm_instance = nullptr;
      }
    };

    return std::make_unique<deinit_t>();
  }

  std::string map_output_name(const std::string &output_name) {
    std::lock_guard lock {DD_DATA.mutex};
    if (!DD_DATA.sm_instance) {
      // Fallback to giving back the output name if the platform is not supported.
      return output_name;
    }

    // LumeN sentinel (ADR 0025 / US-076): "virtual" means "capture the
    // per-session SudoVDA monitor". The monitor itself is created in
    // configure_display() (which has the client's requested mode); here we
    // just hand back its GDI \\.\DISPLAYx name so the capture layer targets
    // it. If no virtual display is active we log + return "" so capture
    // falls back to the physical primary rather than failing the stream.
    if (boost::iequals(output_name, "virtual")) {
#ifdef _WIN32
      if (VDD_STATE.active && !VDD_STATE.gdi_name.empty()) {
        // GDI device names are ASCII, so the narrowing is safe.
        return std::string(VDD_STATE.gdi_name.begin(), VDD_STATE.gdi_name.end());
      }
      BOOST_LOG(warning) << "output_name=virtual but no SudoVDA display is active; "
                            "falling back to the physical primary.";
#endif
      return {};
    }

    return DD_DATA.sm_instance->execute([&output_name](auto &settings_iface) {
      return settings_iface.getDisplayName(output_name);
    });
  }

  void configure_display(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
#ifdef _WIN32
    // LumeN (ADR 0025 / US-076): output_name=="virtual" spins up a SudoVDA
    // monitor at the client's exact mode for this session. No physical
    // libdisplaydevice reconfig is needed in that case — the virtual
    // monitor is already at the requested resolution/refresh.
    if (boost::iequals(video_config.output_name, "virtual")) {
      std::lock_guard lock {DD_DATA.mutex};
      lumen_create_virtual_display_unlocked(session);
      return;
    }
#endif
    const auto result {parse_configuration(video_config, session)};
    if (const auto *parsed_config {std::get_if<SingleDisplayConfiguration>(&result)}; parsed_config) {
      configure_display(*parsed_config);
      return;
    }

    if (const auto *disabled {std::get_if<configuration_disabled_tag_t>(&result)}; disabled) {
      revert_configuration();
      return;
    }

    // Error already logged for failed_to_parse_tag_t case, and we also don't
    // want to revert active configuration in case we have any
  }

  void configure_display(const SingleDisplayConfiguration &config) {
    std::lock_guard lock {DD_DATA.mutex};
    if (!DD_DATA.sm_instance) {
      // Platform is not supported, nothing to do.
      return;
    }

    DD_DATA.sm_instance->schedule([config](auto &settings_iface, auto &stop_token) {
      // We only want to keep retrying in case of a transient errors.
      // In other cases, when we either fail or succeed we just want to stop...
      if (settings_iface.applySettings(config) != SettingsManagerInterface::ApplyResult::ApiTemporarilyUnavailable) {
        stop_token.requestStop();
      }
    },
                                  {.m_sleep_durations = {DEFAULT_RETRY_INTERVAL}});
  }

  void revert_configuration() {
    std::lock_guard lock {DD_DATA.mutex};
#ifdef _WIN32
    // Tear down the per-session SudoVDA monitor (no-op if none active).
    lumen_remove_virtual_display_unlocked();
#endif
    revert_configuration_unlocked(revert_option_e::try_indefinitely_with_delay);
  }

  void revert_virtual_display() {
#ifdef _WIN32
    // Independent of the physical-config revert gate in stream.cpp — the
    // virtual monitor must always be dropped when the stream ends, else it
    // leaks and lags the host at idle (ADR 0025 / US-076).
    std::lock_guard lock {DD_DATA.mutex};
    lumen_remove_virtual_display_unlocked();
#endif
  }

  bool reset_persistence() {
    std::lock_guard lock {DD_DATA.mutex};
    if (!DD_DATA.sm_instance) {
      // Platform is not supported, assume success.
      return true;
    }

    return DD_DATA.sm_instance->execute([](auto &settings_iface, auto &stop_token) {
      // Whatever the outcome is we want to stop interfering with the user,
      // so any schedulers need to be stopped.
      stop_token.requestStop();
      return settings_iface.resetPersistence();
    });
  }

  EnumeratedDeviceList enumerate_devices() {
    std::lock_guard lock {DD_DATA.mutex};
    if (!DD_DATA.sm_instance) {
      // Platform is not supported.
      return {};
    }

    return DD_DATA.sm_instance->execute([](auto &settings_iface) {
      return settings_iface.enumAvailableDevices();
    });
  }

  std::variant<failed_to_parse_tag_t, configuration_disabled_tag_t, SingleDisplayConfiguration> parse_configuration(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    const auto device_prep {parse_device_prep_option(video_config)};
    if (!device_prep) {
      return configuration_disabled_tag_t {};
    }

    SingleDisplayConfiguration config;
    config.m_device_id = video_config.output_name;
    config.m_device_prep = *device_prep;
    config.m_hdr_state = parse_hdr_option(video_config, session);

    if (!parse_resolution_option(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    if (!parse_refresh_rate_option(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    if (!remap_display_mode_if_needed(video_config, session, config)) {
      // Error already logged
      return failed_to_parse_tag_t {};
    }

    return config;
  }
}  // namespace display_device
