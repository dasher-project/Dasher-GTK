#include "SherpaModelBrowser.h"
#include "SherpaModelInstaller.h"
#include "SherpaModels.h"

#include <algorithm>
#include <set>
#include <sstream>

using Dasher::SherpaModel;
namespace SherpaStorage = Dasher::SherpaStorage;

namespace Dasher {

namespace {
// ListBoxRows are lightweight but not virtualised; cap the rendered
// rows and tell the user how many matched instead.
constexpr size_t kMaxRows = 400;
} // namespace

SherpaModelBrowser::SherpaModelBrowser(Gtk::Window& parent, std::shared_ptr<TtsService> tts, UseModelFn on_use_model)
    : m_tts(std::move(tts)), m_on_use_model(std::move(on_use_model)) {
    set_title("Sherpa-ONNX Models");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(760, 540);

    m_catalog = sherpa_catalog_from_json(m_tts->get_sherpa_models_json());
    refresh_installed();

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    outer->set_margin(8);
    set_child(*outer);

    m_search = Gtk::make_managed<Gtk::SearchEntry>();
    m_search->set_placeholder_text("Search models or languages");
    m_search->property_search_delay() = 200;
    outer->append(*m_search);

    m_count_label = Gtk::make_managed<Gtk::Label>("");
    m_count_label->set_halign(Gtk::Align::START);
    m_count_label->add_css_class("dim-label");
    outer->append(*m_count_label);

    auto* paned = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    paned->set_position(340);
    paned->set_vexpand(true);
    outer->append(*paned);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_hexpand(true);
    scroll->set_vexpand(true);
    m_list = Gtk::make_managed<Gtk::ListBox>();
    m_list->set_activate_on_single_click(true);
    m_list->set_selection_mode(Gtk::SelectionMode::SINGLE);
    scroll->set_child(*m_list);
    paned->set_start_child(*scroll);

    auto* details_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    details_box->set_margin(6);
    paned->set_end_child(*details_box);

    m_title = Gtk::make_managed<Gtk::Label>("");
    m_title->set_halign(Gtk::Align::START);
    m_title->set_wrap(true);
    m_title->add_css_class("heading");
    details_box->append(*m_title);

    m_subtitle = Gtk::make_managed<Gtk::Label>("");
    m_subtitle->set_halign(Gtk::Align::START);
    m_subtitle->set_wrap(true);
    m_subtitle->add_css_class("dim-label");
    details_box->append(*m_subtitle);

    m_details = Gtk::make_managed<Gtk::Label>("");
    m_details->set_halign(Gtk::Align::START);
    m_details->set_wrap(true);
    details_box->append(*m_details);

    m_reason = Gtk::make_managed<Gtk::Label>("");
    m_reason->set_halign(Gtk::Align::START);
    m_reason->set_wrap(true);
    m_reason->add_css_class("dim-label");
    details_box->append(*m_reason);

    m_progress = Gtk::make_managed<Gtk::ProgressBar>();
    m_progress->set_hexpand(true);
    m_progress->hide();
    details_box->append(*m_progress);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    details_box->append(*buttons);
    m_download_btn = Gtk::make_managed<Gtk::Button>("Download Model");
    m_download_btn->set_margin_top(4);
    m_download_btn->signal_clicked().connect([this]() {
        if (!m_selected_id.empty()) start_download(m_selected_id);
    });
    buttons->append(*m_download_btn);
    m_cancel_btn = Gtk::make_managed<Gtk::Button>("Cancel Download");
    m_cancel_btn->signal_clicked().connect([this]() { stop_download(); });
    buttons->append(*m_cancel_btn);
    m_use_btn = Gtk::make_managed<Gtk::Button>("Use Model");
    m_use_btn->add_css_class("suggested-action");
    m_use_btn->signal_clicked().connect([this]() {
        if (!m_selected_id.empty() && m_on_use_model) m_on_use_model(m_selected_id);
        close();
    });
    buttons->append(*m_use_btn);

    m_error = Gtk::make_managed<Gtk::Label>("");
    m_error->set_halign(Gtk::Align::START);
    m_error->set_wrap(true);
    m_error->add_css_class("error");
    details_box->append(*m_error);

    auto* close_btn = Gtk::make_managed<Gtk::Button>("Close");
    close_btn->set_halign(Gtk::Align::END);
    close_btn->signal_clicked().connect([this]() { close(); });
    outer->append(*close_btn);

    m_list->signal_row_activated().connect([this](Gtk::ListBoxRow* row) { show_details(row->get_name()); });
    m_search->signal_search_changed().connect([this]() { rebuild_list(); });

    rebuild_list();
    if (!m_catalog.empty()) {
        auto* first = m_list->get_row_at_index(0);
        if (first) {
            m_list->select_row(*first);
            show_details(first->get_name());
        }
    }
}

SherpaModelBrowser::~SherpaModelBrowser() {
    stop_download();
}

void SherpaModelBrowser::refresh_installed() {
    m_installed = SherpaStorage::installed_ids();
}

void SherpaModelBrowser::rebuild_list() {
    while (Gtk::Widget* child = m_list->get_first_child()) {
        m_list->remove(*child);
    }

    // Only offer models Dasher can actually install (or already has);
    // the 1781-model catalog is mostly blocked/needs-other-engine
    // entries that render as dead rows.
    std::string search = m_search->get_text();
    std::vector<const SherpaModel*> shown;
    size_t matches = 0;
    size_t hidden = 0;
    for (const auto& m : m_catalog) {
        bool installed = m_installed.count(m.id) != 0;
        if (!installed && SherpaStorage::download_unavailable_reason(m)) {
            hidden++;
            continue;
        }
        if (!m.matches(search)) continue;
        matches++;
        if (shown.size() < kMaxRows) shown.push_back(&m);
    }

    for (const SherpaModel* m : shown) {
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_name(m->id);
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        box->set_margin(4);
        auto* name = Gtk::make_managed<Gtk::Label>(m->display_name());
        name->set_halign(Gtk::Align::START);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        box->append(*name);
        auto* caption = Gtk::make_managed<Gtk::Label>(m->languages_label());
        caption->set_halign(Gtk::Align::START);
        caption->add_css_class("dim-label");
        caption->set_ellipsize(Pango::EllipsizeMode::END);
        box->append(*caption);
        if (m_installed.count(m->id)) {
            auto* installed = Gtk::make_managed<Gtk::Label>("✓ Installed");
            installed->set_halign(Gtk::Align::END);
            installed->add_css_class("dim-label");
            box->append(*installed);
        }
        row->set_child(*box);
        m_list->append(*row);
    }

    std::string count = std::to_string(matches) + (matches == 1 ? " model" : " models");
    if (hidden > 0) count += " — " + std::to_string(hidden) + " not installable hidden";
    if (matches > shown.size()) count += " — type to narrow the search";
    m_count_label->set_text(count);
}

void SherpaModelBrowser::show_details(const std::string& model_id, bool clear_error) {
    m_selected_id = model_id;
    if (clear_error) m_error->set_text("");
    const SherpaModel* found = nullptr;
    for (const auto& m : m_catalog) {
        if (m.id == model_id) {
            found = &m;
            break;
        }
    }
    if (!found) {
        m_title->set_text("");
        return;
    }
    const SherpaModel& m = *found;

    m_title->set_text(m.display_name());
    m_subtitle->set_text(m.id + " · " + m.model_type +
                         (m.quantization.empty() || m.quantization == "fp32" ? std::string() : " · " + m.quantization));

    std::vector<std::string> lines;
    std::string langs = m.languages_label();
    if (!langs.empty()) lines.push_back("Languages: " + langs);
    std::string size = m.size_label();
    if (!size.empty()) lines.push_back("Download size: " + size);
    if (m.num_speakers > 0) lines.push_back("Speakers: " + std::to_string(m.num_speakers));
    if (!m.license.empty()) lines.push_back("License: " + m.license);
    if (m.deprecated) lines.push_back("This model is deprecated.");
    std::ostringstream joined;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) joined << '\n';
        joined << lines[i];
    }
    m_details->set_text(joined.str());

    auto reason = SherpaStorage::download_unavailable_reason(m);
    m_reason->set_text(reason ? *reason : "");

    bool installed = m_installed.count(m.id) != 0;
    bool busy = !m_done;
    m_download_btn->set_visible(!installed && !reason && !busy);
    m_use_btn->set_visible(installed && !busy);
    m_cancel_btn->set_visible(busy);
}

void SherpaModelBrowser::start_download(const std::string& model_id) {
    if (!m_done) return;
    const SherpaModel* found = nullptr;
    for (const auto& m : m_catalog) {
        if (m.id == model_id) {
            found = &m;
            break;
        }
    }
    if (!found) return;

    m_downloading_id = model_id;
    m_result_error.clear();
    m_error->set_text("");
    m_done = false;
    m_cancel = false;
    m_fraction = -1.0;
    {
        std::lock_guard<std::mutex> lock(m_stage_mutex);
        m_stage = "Downloading model…";
    }
    m_progress->show();
    m_download_btn->set_visible(false);
    m_use_btn->set_visible(false);
    m_cancel_btn->set_visible(true);

    SherpaModel model = *found;
    auto installer = std::make_shared<SherpaModelInstaller>(
        [tts = m_tts](const std::string& id) { return tts->provision_sherpa_g2p(id); });

    m_worker = std::thread([this, installer, model]() {
        std::string error;
        bool ok = installer->install(
            model, [this]() { return m_cancel.load(); },
            [this](double fraction, const std::string& stage) {
                m_fraction = fraction;
                std::lock_guard<std::mutex> lock(m_stage_mutex);
                m_stage = stage;
            },
            error);
        if (!ok) m_result_error = error;
        m_done = true;
    });

    m_poll_conn = Glib::signal_timeout().connect(
        [this]() -> bool {
            if (m_done) {
                finish_download();
                return false;
            }
            double fraction = m_fraction;
            if (fraction >= 0.0) {
                m_progress->set_fraction(std::min(fraction, 1.0));
            } else if (fraction >= -1.0) {
                m_progress->pulse();
            }
            std::string stage;
            {
                std::lock_guard<std::mutex> lock(m_stage_mutex);
                stage = m_stage;
            }
            m_progress->set_text(stage);
            return true;
        },
        150);
}

void SherpaModelBrowser::stop_download() {
    // Disconnect the poll source BEFORE joining: it captures `this`,
    // so a timeout firing between window teardown and thread join
    // would be a use-after-free.
    if (m_poll_conn.connected()) m_poll_conn.disconnect();
    if (m_done) {
        if (m_worker.joinable()) m_worker.join();
        return;
    }
    m_cancel = true;
    if (m_worker.joinable()) m_worker.join();
    m_done = true;
    finish_download();
}

void SherpaModelBrowser::finish_download() {
    if (m_worker.joinable()) m_worker.join();
    m_progress->hide();
    bool ok = m_result_error.empty();
    m_downloading_id.clear();
    // show_details BEFORE setting the error label — it resets the
    // details pane, and used to wipe an error set here in the same
    // tick, so failures looked like silent flip-backs to Download.
    show_details(m_selected_id);
    if (ok) {
        refresh_installed();
        rebuild_list();
    } else if (m_result_error == "Download cancelled.") {
        // Quietly return to the pre-download state.
    } else {
        g_printerr("sherpa: model install failed: %s\n", m_result_error.c_str());
        m_error->set_text(m_result_error);
    }
}

} // namespace Dasher
