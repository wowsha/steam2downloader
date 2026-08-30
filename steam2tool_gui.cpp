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

static std::string q(const std::string &s) {
#ifdef _WIN32
    std::string r="\""; for(char c:s){ if(c=='\"') r+="\\\""; else r+=c; } return r+"\"";
#else
    std::string r="'"; for(char c:s){ if(c=='\'') r+="'\\''"; else r+=c; } return r+"'";
#endif
}
static void log(State& s,const std::string& x){std::string t=s.log->value()?s.log->value():"";t+=x;if(t.empty()||t.back()!='\n')t+='\n';s.log->value(t.c_str());}
static fs::path tool(){
    fs::path exe="steam2tool";
#ifdef _WIN32
    exe+=".exe";
#endif
    return fs::absolute(exe);
}
static std::string run(State& s,const std::string& args){
    const fs::path tmp=fs::temp_directory_path()/"steam2tool_gui.log";
    std::string cmd=q(tool().string())+" "+args+" > "+q(tmp.string())+" 2>&1";
    const int rc=std::system(cmd.c_str());
    std::ifstream in(tmp);std::ostringstream out;out<<in.rdbuf();std::error_code ec;fs::remove(tmp,ec);
    if(rc!=0)out<<"\nProcess exited with code "<<rc<<".";return out.str();
}
static std::string v(Fl_Input* i){return i&&i->value()?i->value():"";}
static void index_cb(Fl_Widget*,void* d){auto&s=*static_cast<State*>(d);log(s,"Indexing dump...");log(s,run(s,"index --base "+q(v(s.base))+" --out "+q(v(s.index))));s.browser->clear();std::ifstream in(v(s.index));std::string line,last;while(std::getline(in,line)){std::istringstream iss(line);std::string id,kind,name;if(!(iss>>id>>kind>>name)||id=="depot"||id==last)continue;s.browser->add(id.c_str());last=id;}s.browser->redraw();}
static void select_cb(Fl_Widget*,void*d){auto&s=*static_cast<State*>(d);if(s.browser->value()){if(const char*t=s.browser->text(s.browser->value()))s.depot->value(t);}}
static void download_cb(Fl_Widget*,void*d){auto&s=*static_cast<State*>(d);if(v(s.depot).empty()||v(s.version).empty()){fl_alert("Enter a depot ID and version.");return;}std::string a="download --base "+q(v(s.base))+" --index "+q(v(s.index))+" --depot "+q(v(s.depot))+" --version "+q(v(s.version))+" --dir "+q(v(s.workdir));log(s,"Downloading...");log(s,run(s,a));}
static void extract_cb(Fl_Widget*,void*d){auto&s=*static_cast<State*>(d);if(v(s.depot).empty()||v(s.version).empty()){fl_alert("Enter a depot ID and version.");return;}std::string a="extract --dir "+q(v(s.workdir))+" --depot "+q(v(s.depot))+" --version "+q(v(s.version));if(!v(s.blobcrc).empty())a+=" --blobcrc "+q(v(s.blobcrc));if(!v(s.output).empty())a+=" --out "+q(v(s.output));log(s,"Extracting...");log(s,run(s,a));}

int main(){
    Fl_Double_Window w(1080,700,"Steam2 Dump Tool"); State s; int y=15;
    new Fl_Box(15,y,110,28,"Mirror URL:");s.base=new Fl_Input(130,y,700,28);s.base->value("https://de.steam2.download/");auto* ib=new Fl_Button(850,y,190,28,"Index dump");ib->callback(index_cb,&s);y+=38;
    new Fl_Box(15,y,110,28,"Index file:");s.index=new Fl_Input(130,y,700,28);s.index->value("export.tsv");y+=38;
    new Fl_Box(15,y,110,28,"Working dir:");s.workdir=new Fl_Input(130,y,700,28);s.workdir->value("steam2data");y+=45;
    new Fl_Box(15,y,70,28,"Depot:");s.depot=new Fl_Input(85,y,145,28);new Fl_Box(245,y,80,28,"Version:");s.version=new Fl_Input(320,y,145,28);new Fl_Box(480,y,90,28,"Blob CRC:");s.blobcrc=new Fl_Input(565,y,180,28);auto* db=new Fl_Button(850,y,190,28,"Download version");db->callback(download_cb,&s);y+=38;
    new Fl_Box(15,y,110,28,"Output:");s.output=new Fl_Input(130,y,615,28);auto* eb=new Fl_Button(850,y,190,28,"Extract");eb->callback(extract_cb,&s);y+=45;
    new Fl_Box(15,y,250,28,"Indexed depots:");new Fl_Box(350,y,250,28,"Log:");y+=30;
    s.browser=new Fl_Browser(15,y,300,500);s.browser->callback(select_cb,&s);s.log=new Fl_Multiline_Output(350,y,690,500);s.log->value("Ready. Set a mirror URL and click Index dump.\n");
    w.end();w.resizable(w);w.show();return Fl::run();
}
