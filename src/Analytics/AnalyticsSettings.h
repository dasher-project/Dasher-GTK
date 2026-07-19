#pragma once

#include <string>

namespace analytics {

// Frontend-local analytics consent state, persisted to
// <XDG_CONFIG_HOME>/dasher/analytics.conf. Analytics is opt-IN and defaults to
// OFF (RFC 0001): no events are sent until the user makes a choice. The
// anonymous id is a locally-generated UUID used as PostHog's distinct_id; it is
// not derived from hardware or any account and can be reset by the user.
class AnalyticsSettings {
  public:
    // Load from `path` (defaults to the real config file). A missing file
    // yields defaults (opted-out, prompt not shown, no id).
    static AnalyticsSettings load(const std::string& path = default_path());
    void save() const;

    bool opted_in() const { return m_opted_in; }
    void set_opted_in(bool v) { m_opted_in = v; }

    // Whether the first-run consent prompt has been shown yet.
    bool prompt_shown() const { return m_prompt_shown; }
    void set_prompt_shown(bool v) { m_prompt_shown = v; }

    // Return the anonymous id, generating and persisting one on first use.
    const std::string& anonymous_id();
    // Generate a fresh anonymous id and persist it.
    void reset_anonymous_id();

    static std::string default_path();

  private:
    std::string m_path;
    bool m_opted_in = false;
    bool m_prompt_shown = false;
    std::string m_anonymous_id;
};

} // namespace analytics
