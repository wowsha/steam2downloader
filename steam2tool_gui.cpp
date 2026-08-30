#include <FL/Fl.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Box.H>
#include <FL/fl_ask.H>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

struct State {
    Fl_Input *base{}, *index{}, *workdir{}, *depot{}, *version{}, *blobcrc{}, *output{};
    Fl_Browser *browser{};
    Fl_Multiline_Output *log{};
};

static fs::path g_tool;

static std::string q(const std::string &s) {
#ifdef _WIN32
    std::string r = "\"";
    for (char c : s) { if (c == '\"') r += "\\\""; else r += c; }
    return r + "\"";
#else
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
#endif
}

static std::string v(Fl_Input *i) { return i && i->value() ? i->value() : ""; }

static void log_line(State &s, const std::string &text) {
    std::string current = s.log->value() ? s.log->value() : "";
    current += text;
    if (current.empty() || current.back() != '\n') current += '\n';
    s.log->value(current.c_str());
    s.log->redraw();
    Fl::check();
}

static std::string run_tool(const std::string &args) {
    const fs::path tmp = fs::temp_directory_path() / "steam2tool_gui.log";
    const std::string command = q(g_tool.string()) + " " + args + " > " + q(tmp.string()) + " 2>&1";
    const int rc = std::system(command.c_str());
    std::ifstream in(tmp);
    std::ostringstream output;
    output << in.rdbuf();
    std::error_code ec;
    fs::remove(tmp, ec);
    if (rc != 0) output << "\nProcess exited with code " << rc << ".";
    return output.str();
}

static void index_cb(Fl_Widget *, void *data) {
    auto &s = *static_cast<State *>(data);
    log_line(s, "Indexing " + v(s.base) + "...");
    log_line(s, run_tool("index --base " + q(v(s.base)) + " --out " + q(v(s.index))));
    s.browser->clear();
    std::ifstream in(v(s.index));
    std::string line, last;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string id, kind, name;
        if (!(iss >> id >> kind >> name) || id == "depot" || id == last) continue;
        s.browser->add(id.c_str());
        last = id;
    }
}

static void select_cb(Fl_Widget *, void *data) {
    auto &s = *static_cast<State *>(data);
    if (s.browser->value()) s.depot->value(s.browser->text(s.browser->value()));
}

static void download_cb(Fl_Widget *, void *data) {
    auto &s = *static_cast<State *>(data);
    if (v(s.depot).empty() || v(s.version).empty()) { fl_alert("Enter a depot ID and version."); return; }
    const std::string args = "download --base " + q(v(s.base)) + " --index " + q(v(s.index)) +
        " --depot " + q(v(s.depot)) + " --version " + q(v(s.version)) + " --dir " + q(v(s.workdir));
    log_line(s, "Downloading...");
    log_line(s, run_tool(args));
}

static void extract_cb(Fl_Widget *, void *data) {
    auto &s = *static_cast<State *>(data);
    if (v(s.depot).empty() || v(s.version).empty()) { fl_alert("Enter a depot ID and version."); return; }
    std::string args = "extract --dir " + q(v(s.workdir)) + " --depot " + q(v(s.depot)) +
        " --version " + q(v(s.version));
    if (!v(s.blobcrc).empty()) args += " --blobcrc " + q(v(s.blobcrc));
    if (!v(s.output).empty()) args += " --out " + q(v(s.output));
    log_line(s, "Extracting...");
    log_line(s, run_tool(args));
}

int main(int argc, char **argv) {
    fs::path self = argc > 0 ? fs::absolute(argv[0]) : fs::current_path() / "steam2tool-gui";
    g_tool = self.parent_path() / "steam2tool";
#ifdef _WIN32
    g_tool += ".exe";
#endif

    Fl_Double_Window window(1080, 700, "Steam2 Dump Tool");
    State s;
    int y = 15;
    new Fl_Box(15, y, 110, 28, "Mirror URL:");
    s.base = new Fl_Input(130, y, 700, 28); s.base->value("https://de.steam2.download/");
    auto *ib = new Fl_Button(850, y, 190, 28, "Index dump"); ib->callback(index_cb, &s); y += 38;
    new Fl_Box(15, y, 110, 28, "Index file:"); s.index = new Fl_Input(130, y, 700, 28); s.index->value("export.tsv"); y += 38;
    new Fl_Box(15, y, 110, 28, "Working dir:"); s.workdir = new Fl_Input(130, y, 700, 28); s.workdir->value("steam2data"); y += 45;
    new Fl_Box(15, y, 70, 28, "Depot:"); s.depot = new Fl_Input(85, y, 145, 28);
    new Fl_Box(245, y, 80, 28, "Version:"); s.version = new Fl_Input(320, y, 145, 28);
    new Fl_Box(480, y, 90, 28, "Blob CRC:"); s.blobcrc = new Fl_Input(565, y, 180, 28);
    auto *db = new Fl_Button(850, y, 190, 28, "Download version"); db->callback(download_cb, &s); y += 38;
    new Fl_Box(15, y, 110, 28, "Output:"); s.output = new Fl_Input(130, y, 615, 28);
    auto *eb = new Fl_Button(850, y, 190, 28, "Extract"); eb->callback(extract_cb, &s); y += 45;
    new Fl_Box(15, y, 250, 28, "Indexed depots:"); new Fl_Box(350, y, 250, 28, "Log:"); y += 30;
    s.browser = new Fl_Browser(15, y, 300, 500); s.browser->callback(select_cb, &s);
    s.log = new Fl_Multiline_Output(350, y, 690, 500); s.log->value("Ready. Set a mirror URL and click Index dump.\n");
    window.end(); window.resizable(window); window.show();
    return Fl::run();
}
