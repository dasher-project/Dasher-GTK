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

    std::shared_ptr<TtsService> m_tts;
    UseModelFn m_on_use_model;

    std::vector<struct SherpaModel> m_catalog;
    std::set<std::string> m_installed;
    std::string m_selected_id;

    // Widgets
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

    // Worker thread state. The download runs on m_worker; progress is
    // polled from a 150 ms main-loop timeout, so nothing touches GTK
    // from the worker.
    std::thread m_worker;
    sigc::connection m_poll_conn;
    std::atomic_bool m_cancel{false};
    std::atomic<double> m_fraction{-2.0}; // < -1: idle, [-1,0): unknown total
    std::mutex m_stage_mutex;
    std::string m_stage;
    std::atomic_bool m_done{true};
    std::string m_result_error;
    std::string m_downloading_id;
};

} // namespace Dasher
