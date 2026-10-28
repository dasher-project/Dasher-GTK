#pragma once

// RFC 0001 2026-10-28 amendment: UI-interaction telemetry for the settings
// widgets. setting_changed fires ONLY at user commit sites (the Synced*
// widgets' change handlers) — never for programmatic bridge updates
// (update_from_bridge paths are guarded by their m_refreshing flags).
//
// Privacy rules (see the amendment):
// - values only for bool / number / enum-from-fixed-vocabulary
// - string settings: the change is recorded WITHOUT the value
// - no file paths, no free text
// All capture() calls are opt-in gated inside AnalyticsClient.

#include "Analytics/AnalyticsClient.h"
#include <string>
#include <map>

namespace analytics {

inline void track_setting_bool(const std::string& setting_id, bool value, const std::string& surface = "settings") {
    AnalyticsClient::instance().capture("setting_changed", {
                                                               {"setting_id", setting_id},
                                                               {"value_type", "bool"},
                                                               {"value", value ? "true" : "false"},
                                                               {"surface", surface},
                                                           });
}

inline void track_setting_number(const std::string& setting_id, long value, const std::string& surface = "settings") {
    AnalyticsClient::instance().capture("setting_changed", {
                                                               {"setting_id", setting_id},
                                                               {"value_type", "number"},
                                                               {"value", std::to_string(value)},
                                                               {"surface", surface},
                                                           });
}

// Enum values must come from a fixed vocabulary (engine permitted values,
// palette names) — never free text.
inline void track_setting_enum(const std::string& setting_id, const std::string& value,
                               const std::string& surface = "settings") {
    AnalyticsClient::instance().capture("setting_changed", {
                                                               {"setting_id", setting_id},
                                                               {"value_type", "enum"},
                                                               {"value", value},
                                                               {"surface", surface},
                                                           });
}

// Free-text/string setting: the value is NEVER collected (RFC 0001
// amendment — file paths and custom text are explicitly excluded).
inline void track_setting_string_changed(const std::string& setting_id, const std::string& surface = "settings") {
    AnalyticsClient::instance().capture("setting_changed", {
                                                               {"setting_id", setting_id},
                                                               {"value_type", "string"},
                                                               {"surface", "settings"},
                                                           });
}

} // namespace analytics
