// Searchable Sherpa-ONNX model browser for the Speech settings page —
// the GTK counterpart of Dasher-Apple's SherpaModelBrowser. Lists the
// wrapper's embedded registry (with install state), lets the user
// download a verified model on a worker thread, and hands the chosen
// model id back to the preferences page.
#pragma once

#include "Output/TtsService.h"

#include <atomic>
#include <gtkmm.h>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Dasher {

class SherpaModelBrowser : public Gtk::Window {
  public:
    // Called on the main loop with the registry id of the model the
    // user chose to speak with.
    using UseModelFn = std::function<void(const std::string& model_id)>;

    SherpaModelBrowser(Gtk::Window& parent, std::shared_ptr<TtsService> tts, UseModelFn on_use_model);
    ~SherpaModelBrowser() override;

  private:
    void rebuild_list();
    void show_details(const std::string& model_id, bool clear_error = true);
    void start_download(const std::string& model_id);
    void stop_download();
    void finish_download();
    void refresh_installed();

    // Lifetime guard shared by every async callback (poll timeout,
    // worker progress/completion). The destructor flips it, so a
    // callback that fires after the browser is freed — whatever path
    // freed it (idle delete, rebuild force-close, parent teardown) —
    // sees a dead guard, touches no widget, and disconnects itself.
    // This is the structural fix for the GTK_IS_LABEL/GTK_IS_WIDGET
    // criticals: no deletion path has to be enumerated to be safe.
    struct AsyncGuard {
        std::atomic_bool alive{true};
    };
    std::shared_ptr<AsyncGuard> m_guard = std::make_shared<AsyncGuard>();

    std::shared_ptr<TtsService> m_tts;
    UseModelFn m_on_use_model;

    std::vector<struct SherpaModel> m_catalog;
    std::set<std::string> m_installed;
    std::string m_selected_id;

    // Shared between the install worker and the UI poll: the worker
    // writes ONLY this; the poll (main loop) is the sole UI reader.
    // Freed-with-the-download, never with the browser.
    struct WorkerState {
        std::atomic<double> fraction{-1.0}; // <0: unknown total (pulse)
        std::atomic_bool done{false};
        std::mutex mutex;                   // guards stage (and error at completion)
        std::string stage;
        std::string error;                  // empty = success
    };
    std::shared_ptr<WorkerState> m_worker_state; // non-null while a download runs/awaits finish

    // Widgets (touched only on the main loop while m_guard->alive)
    Gtk::SearchEntry* m_search = nullptr;
    Gtk::Label* m_count_label = nullptr;
    Gtk::ListBox* m_list = nullptr;
    Gtk::Label* m_title = nullptr;
    Gtk::Label* m_subtitle = nullptr;
    Gtk::Label* m_details = nullptr;
    Gtk::Label* m_reason = nullptr;
    Gtk::Label* m_error = nullptr;
    Gtk::Button* m_download_btn = nullptr;
    Gtk::Button* m_cancel_btn = nullptr;
    Gtk::Button* m_use_btn = nullptr;
    Gtk::ProgressBar* m_progress = nullptr;

    // Worker thread. The download runs on m_worker; progress is
    // polled from a 150 ms main-loop timeout, so nothing touches GTK
    // from the worker.
    std::thread m_worker;
    sigc::connection m_poll_conn;
    std::atomic_bool m_cancel{false};
    std::string m_downloading_id;
};

} // namespace Dasher
