#include <FL/Fl.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

struct VersionInfo {
    std::string version;
    std::set<std::string> dat_crcs;
    std::set<std::string> blob_crcs;
};

struct DepotInfo {
    std::string id;
    std::map<std::string, VersionInfo> versions;
    std::size_t dat_count{};
    std::size_t blob_count{};
    bool likely_reset{};
};

struct State {
    fs::path tool;
    std::map<std::string, DepotInfo> depots;

    Fl_Input *mirror{};
    Fl_Input *index_file{};
    Fl_Input *data_dir{};
    Fl_Input *output_dir{};
    Fl_Input *search{};
    Fl_Input *depot{};
    Fl_Input *version{};
    Fl_Input *blob_crc{};

    Fl_Browser *depots_browser{};
    Fl_Browser *versions_browser{};
    Fl_Multiline_Output *details{};
    Fl_Multiline_Output *log{};
    Fl_Progress *progress{};

    Fl_Button *index_button{};
    Fl_Button *download_button{};
    Fl_Button *extract_button{};
    Fl_Button *download_extract_button{};

    Fl_Tabs *tabs{};
    Fl_Group *browser_tab{};
    Fl_Group *actions_tab{};
};

static std::string value(Fl_Input *input) {
    return input && input->value() ? input->value() : "";
}

static std::string trim(std::string text) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

static std::string quote_shell(const std::string &text) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
#else
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
#endif
}

static void append_log(State &state, const std::string &text) {
    std::string current = state.log->value() ? state.log->value() : "";
    current += text;
    if (current.empty() || current.back() != '\n') current += '\n';
    state.log->value(current.c_str());
    state.log->redraw();
    Fl::check();
}

static std::string run_tool(State &state, const std::string &arguments, int &rc) {
    const fs::path temp = fs::temp_directory_path() / "steam2tool-gui-output.txt";
    const std::string command = quote_shell(state.tool.string()) + " " + arguments +
        " > " + quote_shell(temp.string()) + " 2>&1";
    rc = std::system(command.c_str());

    std::ifstream input(temp, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    std::error_code ec;
    fs::remove(temp, ec);
    return output.str();
}

static void clear_selection(State &state) {
    state.depot->value("");
    state.version->value("");
    state.blob_crc->value("");
    state.versions_browser->clear();
    state.details->value("");
}

static std::string browser_depot_id(const State &state) {
    if (!state.depots_browser->value()) return {};
    const char *text = state.depots_browser->text(state.depots_browser->value());
    if (!text) return {};
    std::istringstream iss(text);
    std::string id;
    iss >> id;
    return id;
}

static std::string browser_version_value(const State &state) {
    if (!state.versions_browser->value()) return {};
    const char *text = state.versions_browser->text(state.versions_browser->value());
    if (!text) return {};
    std::istringstream iss(text);
    std::string version;
    iss >> version;
    return version;
}

static void rebuild_depot_list(State &state) {
    const std::string filter = value(state.search);
    state.depots_browser->clear();
    for (const auto &[id, depot] : state.depots) {
        if (!filter.empty() && id.find(filter) == std::string::npos) continue;
        std::ostringstream label;
        label << id << "  |  " << depot.versions.size() << " versions  |  "
              << depot.dat_count << " DAT  |  " << depot.blob_count << " blobs";
        if (depot.likely_reset) label << "  |  RESET";
        state.depots_browser->add(label.str().c_str());
    }
}

static void show_depot(State &state, const std::string &id) {
    const auto it = state.depots.find(id);
    if (it == state.depots.end()) return;

    state.depot->value(id.c_str());
    state.versions_browser->clear();
    state.version->value("");
    state.blob_crc->value("");

    for (const auto &[version, info] : it->second.versions) {
        std::ostringstream label;
        label << version << "  |  DAT CRCs: " << info.dat_crcs.size()
              << "  |  Blob CRCs: " << info.blob_crcs.size();
        state.versions_browser->add(label.str().c_str());
    }

    std::ostringstream detail;
    detail << "Depot " << id << "\n"
           << "Versions: " << it->second.versions.size() << "\n"
           << "DAT records: " << it->second.dat_count << "\n"
           << "Blob records: " << it->second.blob_count << "\n"
           << "Likely reset: " << (it->second.likely_reset ? "yes" : "no") << "\n\n"
           << "Select a version to inspect CRCs.";
    state.details->value(detail.str().c_str());
}

static void show_version(State &state, const std::string &depot_id, const std::string &version) {
    const auto depot_it = state.depots.find(depot_id);
    if (depot_it == state.depots.end()) return;
    const auto version_it = depot_it->second.versions.find(version);
    if (version_it == depot_it->second.versions.end()) return;

    state.version->value(version.c_str());
    const auto &info = version_it->second;

    std::ostringstream detail;
    detail << "Depot " << depot_id << " | Version " << version << "\n\n"
           << "DAT CRCs:\n";
    if (info.dat_crcs.empty()) detail << "  none\n";
    else for (const auto &crc : info.dat_crcs) detail << "  " << crc << "\n";
    detail << "\nBlob CRCs:\n";
    if (info.blob_crcs.empty()) detail << "  none\n";
    else for (const auto &crc : info.blob_crcs) detail << "  " << crc << "\n";

    state.details->value(detail.str().c_str());
    if (info.blob_crcs.size() == 1) state.blob_crc->value((*info.blob_crcs.begin()).c_str());
}

static void import_index(State &state) {
    state.depots.clear();
    std::ifstream in(value(state.index_file));
    if (!in) {
        fl_alert("The index file could not be opened.");
        return;
    }

    append_log(state, "Loading index: " + value(state.index_file));
    std::string line;
    std::size_t rows = 0;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string id, kind, filename;
        if (!(iss >> id >> kind >> filename) || id == "depot") continue;

        const auto first = filename.find('_');
        const auto second = first == std::string::npos ? std::string::npos : filename.find('_', first + 1);
        const auto third = second == std::string::npos ? std::string::npos : filename.find('_', second + 1);
        if (first == std::string::npos || second == std::string::npos || third == std::string::npos) continue;

        const std::string version = filename.substr(first + 1, second - first - 1);
        const std::string crc = filename.substr(second + 1, third - second - 1);
        auto depot_it = state.depots.try_emplace(id, DepotInfo{id}).first;
        auto &depot = depot_it->second;
        auto &info = depot.versions[version];
        info.version = version;

        if (kind == "dat") {
            info.dat_crcs.insert(crc);
            ++depot.dat_count;
        } else if (kind == "blob") {
            info.blob_crcs.insert(crc);
            ++depot.blob_count;
        } else {
            continue;
        }
        ++rows;
    }

    for (auto &[id, depot] : state.depots) {
        depot.likely_reset = false;
        for (const auto &[version, info] : depot.versions) {
            if (info.dat_crcs.size() > 1 || info.blob_crcs.size() > 1) {
                depot.likely_reset = true;
                break;
            }
        }
    }

    rebuild_depot_list(state);
    append_log(state, "Loaded " + std::to_string(rows) + " records across " +
        std::to_string(state.depots.size()) + " depots.");
    if (!state.depots.empty()) show_depot(state, state.depots.begin()->first);
}

static void browse_existing_index(State &state) {
    if (fs::exists(value(state.index_file))) import_index(state);
    else clear_selection(state);
}

static void choose_directory(Fl_Input *target, const char *title) {
    Fl_Native_File_Chooser chooser;
    chooser.title(title);
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    if (chooser.show() == 0 && chooser.filename()) target->value(chooser.filename());
}

static void choose_file(Fl_Input *target, const char *title) {
    Fl_Native_File_Chooser chooser;
    chooser.title(title);
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    if (chooser.show() == 0 && chooser.filename()) target->value(chooser.filename());
}

static bool validate_target(State &state) {
    if (value(state.depot).empty() || value(state.version).empty()) {
        fl_alert("Select a depot and a version first.");
        return false;
    }
    if (!fs::exists(value(state.index_file))) {
        fl_alert("Build or select the index first.");
        return false;
    }
    return true;
}

static void index_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    const std::string base = trim(value(state.mirror));
    if (base.empty()) {
        fl_alert("Enter the Steam2 mirror URL first.");
        return;
    }

    state.index_button->deactivate();
    append_log(state, "Building index from " + base);
    state.progress->minimum(0);
    state.progress->maximum(1);
    state.progress->value(0);

    int rc = 0;
    const std::string output = run_tool(state,
        "index --base " + quote_shell(base) + " --out " + quote_shell(value(state.index_file)), rc);
    append_log(state, output);
    if (rc == 0) {
        state.progress->value(1);
        import_index(state);
        if (state.tabs && state.browser_tab) state.tabs->value(state.browser_tab);
    } else {
        fl_alert("Indexing failed. See Activity for details.");
    }
    state.index_button->activate();
}

static void download_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    if (!validate_target(state)) return;

    state.download_button->deactivate();
    append_log(state, "Downloading depot " + value(state.depot) + " version " + value(state.version));

    int rc = 0;
    const std::string args = "download --base " + quote_shell(value(state.mirror)) +
        " --index " + quote_shell(value(state.index_file)) +
        " --depot " + quote_shell(value(state.depot)) +
        " --version " + quote_shell(value(state.version)) +
        " --dir " + quote_shell(value(state.data_dir));
    append_log(state, run_tool(state, args, rc));
    state.download_button->activate();
    if (rc == 0) fl_message("Download finished.");
    else fl_alert("Download failed. See Activity for details.");
}

static void extract_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    if (!validate_target(state)) return;

    state.extract_button->deactivate();
    append_log(state, "Extracting depot " + value(state.depot) + " version " + value(state.version));

    int rc = 0;
    std::string args = "extract --dir " + quote_shell(value(state.data_dir)) +
        " --depot " + quote_shell(value(state.depot)) +
        " --version " + quote_shell(value(state.version));
    if (!value(state.blob_crc).empty()) args += " --blobcrc " + quote_shell(value(state.blob_crc));
    if (!value(state.output_dir).empty()) args += " --out " + quote_shell(value(state.output_dir));
    append_log(state, run_tool(state, args, rc));
    state.extract_button->activate();
    if (rc == 0) fl_message("Extraction finished.");
    else fl_alert("Extraction failed. See Activity for details.");
}

static void download_extract_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    if (!validate_target(state)) return;
    download_cb(nullptr, data);
    if (fs::exists(fs::path(value(state.data_dir)) / "dats") &&
        fs::exists(fs::path(value(state.data_dir)) / "blobs")) {
        extract_cb(nullptr, data);
    }
}

static void search_cb(Fl_Widget *, void *data) {
    rebuild_depot_list(*static_cast<State *>(data));
}

static void depot_select_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    const std::string id = browser_depot_id(state);
    if (!id.empty()) show_depot(state, id);
}

static void version_select_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    const std::string id = value(state.depot);
    const std::string version = browser_version_value(state);
    if (!id.empty() && !version.empty()) show_version(state, id, version);
}

static void latest_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    const std::string id = value(state.depot);
    const auto it = state.depots.find(id);
    if (it == state.depots.end() || it->second.versions.empty()) return;
    const std::string latest = it->second.versions.rbegin()->first;
    show_version(state, id, latest);
}

static void settings_mirror_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    state.mirror->value("https://de.steam2.download/");
}

static void jump_browser_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    if (state.tabs && state.browser_tab) state.tabs->value(state.browser_tab);
}

static void jump_actions_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    if (state.tabs && state.actions_tab) state.tabs->value(state.actions_tab);
}

static void choose_index_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    choose_file(state.index_file, "Choose index file");
    browse_existing_index(state);
}

static void choose_index_output_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    choose_file(state.index_file, "Choose index output");
}

static void choose_data_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    choose_directory(state.data_dir, "Choose working directory");
}

static void choose_output_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    choose_directory(state.output_dir, "Choose extraction output");
}

static void open_data_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    const std::string dir = value(state.data_dir);
    if (dir.empty()) return;
#ifdef _WIN32
    const std::string command = "explorer " + quote_shell(dir);
#elif __APPLE__
    const std::string command = "open " + quote_shell(dir);
#else
    const std::string command = "xdg-open " + quote_shell(dir);
#endif
    (void)std::system(command.c_str());
}

static void record_crc_cb(Fl_Widget *, void *data) {
    auto &state = *static_cast<State *>(data);
    if (!value(state.blob_crc).empty()) append_log(state, "Selected blob CRC: " + value(state.blob_crc));
}

int main(int argc, char **argv) {
    State state;
    const fs::path self = argc > 0 ? fs::absolute(argv[0]) : fs::current_path() / "steam2tool-gui";
    state.tool = self.parent_path() / "steam2tool";
#ifdef _WIN32
    state.tool += ".exe";
#endif

    Fl::scheme("gtk+");

    Fl_Double_Window window(1180, 780, "Steam2 Dump Tool");
    window.size_range(980, 680);

    auto *header = new Fl_Group(0, 0, 1180, 88);
    auto *title = new Fl_Box(24, 14, 660, 32, "Steam2 Dump Tool");
    title->labelfont(FL_BOLD);
    title->labelsize(24);
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto *subtitle = new Fl_Box(24, 47, 900, 24,
        "Browse the dump, choose a version, download it, then extract it.");
    subtitle->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    subtitle->labelsize(14);
    auto *default_btn = new Fl_Button(930, 24, 210, 36, "Use German Mirror");
    header->end();

    state.tabs = new Fl_Tabs(16, 96, 1148, 650);

    auto *setup = new Fl_Group(16, 120, 1148, 620, "1. Setup & Index");
    auto *setup_box = new Fl_Group(32, 144, 1116, 238);
    auto *mirror_label = new Fl_Box(48, 162, 160, 28, "Mirror URL");
    mirror_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); mirror_label->labelfont(FL_BOLD);
    state.mirror = new Fl_Input(48, 190, 930, 34);
    state.mirror->value("https://de.steam2.download/");
    auto *browse_index = new Fl_Button(990, 190, 140, 34, "Use file...");

    auto *index_label = new Fl_Box(48, 238, 160, 28, "Index file");
    index_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); index_label->labelfont(FL_BOLD);
    state.index_file = new Fl_Input(48, 266, 930, 34);
    state.index_file->value("export.tsv");
    auto *browse_index_out = new Fl_Button(990, 266, 140, 34, "Choose...");

    auto *data_label = new Fl_Box(48, 314, 180, 28, "Working directory");
    data_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); data_label->labelfont(FL_BOLD);
    state.data_dir = new Fl_Input(48, 342, 930, 34);
    state.data_dir->value("steam2data");
    auto *browse_data = new Fl_Button(990, 342, 140, 34, "Choose...");
    setup_box->end();

    auto *index_help = new Fl_Box(48, 400, 1060, 40,
        "This uses the compact DAT/blob date indexes, not the enormous directory listing.");
    index_help->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP); index_help->labelsize(13);
    state.index_button = new Fl_Return_Button(48, 450, 300, 48, "Build / Refresh Index");
    state.index_button->labelsize(17);
    state.progress = new Fl_Progress(48, 514, 1050, 22);
    state.progress->minimum(0); state.progress->maximum(1); state.progress->value(0);
    auto *jump = new Fl_Button(48, 566, 300, 44, "Open Depot Browser");
    setup->end();

    state.browser_tab = new Fl_Group(16, 120, 1148, 620, "2. Depot Browser");
    auto *search_label = new Fl_Box(34, 145, 100, 28, "Search");
    search_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); search_label->labelfont(FL_BOLD);
    state.search = new Fl_Input(134, 145, 320, 32);
    auto *refresh = new Fl_Button(464, 145, 110, 32, "Refresh");

    state.depots_browser = new Fl_Browser(34, 190, 540, 490);
    state.depots_browser->type(FL_HOLD_BROWSER);
    state.depots_browser->textfont(FL_COURIER);
    auto *depot_caption = new Fl_Box(34, 680, 540, 28,
        "RESET means more than one CRC was found for a version.");
    depot_caption->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); depot_caption->labelsize(12);

    auto *right = new Fl_Group(594, 190, 552, 490);
    auto *selected = new Fl_Box(610, 190, 520, 28, "Selected depot");
    selected->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); selected->labelfont(FL_BOLD);
    state.depot = new Fl_Input(610, 220, 330, 32);
    auto *latest = new Fl_Button(950, 220, 180, 32, "Use Latest");
    auto *versions_label = new Fl_Box(610, 265, 520, 28, "Available versions");
    versions_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); versions_label->labelfont(FL_BOLD);
    state.versions_browser = new Fl_Browser(610, 295, 520, 150);
    state.versions_browser->type(FL_HOLD_BROWSER);
    state.versions_browser->textfont(FL_COURIER);

    auto *version_label = new Fl_Box(610, 462, 80, 28, "Version");
    version_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.version = new Fl_Input(690, 462, 180, 32);
    auto *crc_label = new Fl_Box(610, 502, 80, 28, "Blob CRC");
    crc_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    state.blob_crc = new Fl_Input(690, 502, 440, 32);
    auto *record = new Fl_Button(610, 542, 250, 34, "Record CRC in Activity");
    state.details = new Fl_Multiline_Output(610, 582, 520, 98);
    state.details->textfont(FL_COURIER); state.details->textsize(12);
    right->end();
    state.browser_tab->end();

    state.actions_tab = new Fl_Group(16, 120, 1148, 620, "3. Download & Extract");
    auto *target_title = new Fl_Box(52, 158, 350, 28, "Selected content");
    target_title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); target_title->labelfont(FL_BOLD); target_title->labelsize(18);
    auto *dlabel = new Fl_Box(52, 202, 90, 28, "Depot");
    dlabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto *dview = new Fl_Box(142, 202, 270, 28, "Choose in Depot Browser");
    dview->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto *vlabel = new Fl_Box(52, 242, 90, 28, "Version");
    vlabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto *vview = new Fl_Box(142, 242, 270, 28, "Choose a version");
    vview->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto *action_help = new Fl_Box(430, 198, 650, 70,
        "Download stores the DAT/blob chain in the working directory.\nExtract reconstructs the selected depot into the output directory.");
    action_help->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP); action_help->labelsize(13);

    auto *out_label = new Fl_Box(52, 314, 180, 28, "Extraction output");
    out_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE); out_label->labelfont(FL_BOLD);
    state.output_dir = new Fl_Input(52, 340, 900, 34);
    state.output_dir->value("steam2data/extracted");
    auto *out_browse = new Fl_Button(970, 340, 150, 34, "Choose...");

    state.download_button = new Fl_Button(52, 410, 240, 54, "Download Version");
    state.download_button->labelsize(17);
    state.extract_button = new Fl_Button(304, 410, 240, 54, "Extract Version");
    state.extract_button->labelsize(17);
    state.download_extract_button = new Fl_Button(556, 410, 300, 54, "Download + Extract");
    state.download_extract_button->labelsize(17);
    auto *open_data = new Fl_Button(868, 410, 252, 54, "Open Data Folder");
    auto *action_hint = new Fl_Box(52, 490, 1068, 60,
        "Select the depot and version in the Depot Browser first. For reset depots, inspect the blob CRC before extracting.");
    action_hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP); action_hint->labelsize(13);
    state.actions_tab->end();

    auto *activity = new Fl_Group(16, 120, 1148, 620, "4. Activity");
    state.log = new Fl_Multiline_Output(34, 145, 1110, 520);
    state.log->textfont(FL_COURIER); state.log->textsize(12);
    state.log->value("Ready. Set the mirror URL and build the index.\n");
    activity->end();

    state.tabs->end();

    default_btn->callback(settings_mirror_cb, &state);
    browse_index->callback(choose_index_cb, &state);
    browse_index_out->callback(choose_index_output_cb, &state);
    browse_data->callback(choose_data_cb, &state);
    out_browse->callback(choose_output_cb, &state);
    state.index_button->callback(index_cb, &state);
    jump->callback(jump_browser_cb, &state);
    state.search->callback(search_cb, &state);
    refresh->callback(search_cb, &state);
    state.depots_browser->callback(depot_select_cb, &state);
    state.versions_browser->callback(version_select_cb, &state);
    latest->callback(latest_cb, &state);
    record->callback(record_crc_cb, &state);
    state.download_button->callback(download_cb, &state);
    state.extract_button->callback(extract_cb, &state);
    state.download_extract_button->callback(download_extract_cb, &state);
    open_data->callback(open_data_cb, &state);

    browse_existing_index(state);
    window.resizable(window);
    window.end();
    window.show(argc, argv);
    return Fl::run();
}
