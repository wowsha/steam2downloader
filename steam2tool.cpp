#include <s2fs/steam2_archive.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::string_view kUserAgent = "steam2tool/1.1 (archival use)";
constexpr std::string_view kDefaultBase = "https://de.steam2.download/";
const std::regex kNameRe(R"(^([0-9]+)_([0-9]+)_([0-9A-Fa-f]+)_([0-9A-Fa-f]+)(?:\.[A-Za-z0-9]+)?$)");
struct Entry { std::string filename; std::string version; std::string crc; };
struct Depot { std::string id; std::vector<Entry> dats; std::vector<Entry> blobs; };
[[noreturn]] void die(std::string s) { throw std::runtime_error(std::move(s)); }
std::string shell_quote(std::string_view s) {
#ifdef _WIN32
    std::string out = "\""; for (char c : s) { if (c == '"') out += "\\\""; else out += c; } out += '"'; return out;
#else
    std::string out = "'"; for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; } out += '\''; return out;
#endif
}
fs::path temp_file(std::string_view stem) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / (std::string(stem) + "_" + std::to_string(now) + ".tmp");
}
std::string fetch_text(const std::string& url) {
    const fs::path tmp = temp_file("steam2tool");
#ifdef _WIN32
    const std::string curl = "curl.exe";
#else
    const std::string curl = "curl";
#endif
    const std::string cmd = curl + " -L --fail --silent --show-error --user-agent " + shell_quote(kUserAgent) + " " + shell_quote(url) + " -o " + shell_quote(tmp.string());
    if (std::system(cmd.c_str()) != 0) { std::error_code ec; fs::remove(tmp, ec); die("curl failed while fetching " + url); }
    std::ifstream in(tmp, std::ios::binary); if (!in) { std::error_code ec; fs::remove(tmp, ec); die("cannot read temporary index file"); }
    std::stringstream ss; ss << in.rdbuf(); std::error_code ec; fs::remove(tmp, ec); return ss.str();
}
bool parse_name(std::string_view name, std::string& depot, std::string& version, std::string& crc) {
    std::cmatch m; const std::string s(name); if (!std::regex_match(s.c_str(), m, kNameRe)) return false;
    depot=m[1].str(); version=m[2].str(); crc=m[3].str(); return true;
}
void import_index_text(std::string_view text, std::map<std::string, Depot>& depots, bool blob) {
    std::istringstream in{std::string(text)}; std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line); std::string token;
        while (iss >> token) {
            const auto slash=token.find_last_of("/"); if(slash!=std::string::npos) token.erase(0,slash+1);
            std::string depot,version,crc; if(!parse_name(token,depot,version,crc)) continue;
            auto& d=depots.try_emplace(depot,Depot{depot,{},{}}).first->second;
            (blob?d.blobs:d.dats).push_back({token,version,crc});
            break;
        }
    }
}
void download_file(const std::string& base, std::string_view kind, std::string_view filename, const fs::path& root) {
    fs::create_directories(root); const fs::path out=root/fs::path(filename); if(fs::exists(out)&&fs::file_size(out)>0) return;
#ifdef _WIN32
    const std::string curl="curl.exe";
#else
    const std::string curl="curl";
#endif
    const std::string url=base+std::string(kind)+"/"+std::string(filename); const fs::path tmp=out.string()+".part";
    const std::string cmd=curl+" -L --fail --silent --show-error --user-agent "+shell_quote(kUserAgent)+" "+shell_quote(url)+" -o "+shell_quote(tmp.string());
    if(std::system(cmd.c_str())!=0){std::error_code ec;fs::remove(tmp,ec);die("download failed: "+url);} std::error_code ec;fs::rename(tmp,out,ec);if(ec)die("cannot finalize downloaded file: "+out.string()+": "+ec.message());
}
void extract_depot(const fs::path& blobs,const fs::path& dats,std::uint32_t id,std::uint32_t version,const fs::path& output,std::optional<std::uint32_t> crc){
    s2fs::DepotSpec spec;spec.id=id;spec.version=version;spec.blob_directory=blobs;spec.dat_directory=dats;spec.blob_crc=crc;
    const auto depot=s2fs::Steam2Depot::load(spec);fs::create_directories(output);std::vector<std::byte> buffer(1024*1024);std::uint64_t files=0,bytes=0;
    for(const auto&e:depot.entries()){if(!e.file)continue;std::string p=e.path;std::replace(p.begin(),p.end(),'\\','/');while(!p.empty()&&p.front()=='/')p.erase(p.begin());fs::path rel(p);for(const auto&part:rel)if(part==".."||part==".")die("unsafe archive path: "+e.path);const fs::path dst=output/rel;fs::create_directories(dst.parent_path());std::ofstream out(dst,std::ios::binary|std::ios::trunc);if(!out)die("cannot write: "+dst.string());const auto size=e.file->size();for(std::uint64_t off=0;off<size;){const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size-off));const auto got=e.file->read(off,std::span<std::byte>(buffer.data(),n));if(got!=n)die("short Steam2 read: "+e.path);out.write(reinterpret_cast<const char*>(buffer.data()),static_cast<std::streamsize>(got));if(!out)die("write failed: "+dst.string());off+=got;}++files;bytes+=size;}
    std::cout<<"Extracted "<<files<<" files ("<<bytes<<" bytes)\n";
}
void usage(){std::cout<<"steam2tool - native Steam2 dump indexer/downloader/extractor\n\n"<<"  steam2tool index --base URL --out export.tsv\n"<<"  steam2tool download --base URL --index export.tsv --depot ID --version N --dir DIR\n"<<"  steam2tool extract --dir DIR --depot ID --version N [--blobcrc HEX] [--out DIR]\n\n"<<"download fetches the complete history up to the selected version so blob ancestry can be reconstructed.\n";}
std::string arg_value(int&i,int argc,char**argv,std::string_view name){if(++i>=argc)die(std::string(name)+" requires a value");return argv[i];}
int run(int argc,char**argv){
    if(argc<2){usage();return 0;}const std::string mode=argv[1];
    if(mode=="index"){
        std::string base(kDefaultBase),out="export.tsv";for(int i=2;i<argc;++i){std::string a=argv[i];if(a=="--base")base=arg_value(i,argc,argv,a);else if(a=="--out")out=arg_value(i,argc,argv,a);else if(a=="--help"||a=="-h"){usage();return 0;}else die("unknown option: "+a);}if(base.back()!='/')base+='/';
        std::map<std::string,Depot> depots;import_index_text(fetch_text(base+"dats_dates.txt"),depots,false);import_index_text(fetch_text(base+"blobs_dates.txt"),depots,true);std::ofstream f(out);if(!f)die("cannot write index: "+out);f<<"depot\tkind\tfilename\n";for(const auto&[id,d]:depots){for(const auto&e:d.dats)f<<id<<"\tdat\t"<<e.filename<<"\n";for(const auto&e:d.blobs)f<<id<<"\tblob\t"<<e.filename<<"\n";}std::cout<<"Indexed "<<depots.size()<<" depots -> "<<out<<"\n";return 0;
    }
    if(mode=="download"){
        std::string base(kDefaultBase),index="export.tsv",depot,version,dir="steam2data";for(int i=2;i<argc;++i){std::string a=argv[i];if(a=="--base")base=arg_value(i,argc,argv,a);else if(a=="--index")index=arg_value(i,argc,argv,a);else if(a=="--depot")depot=arg_value(i,argc,argv,a);else if(a=="--version")version=arg_value(i,argc,argv,a);else if(a=="--dir")dir=arg_value(i,argc,argv,a);else if(a=="--help"||a=="-h"){usage();return 0;}else die("unknown option: "+a);}if(depot.empty()||version.empty())die("--depot and --version are required");if(base.back()!='/')base+='/';std::uint64_t target{};auto vr=std::from_chars(version.data(),version.data()+version.size(),target);if(vr.ec!=std::errc()||vr.ptr!=version.data()+version.size())die("invalid --version: "+version);std::ifstream in(index);if(!in)die("cannot open index: "+index);
        std::vector<Entry> dats,blobs;std::string line;while(std::getline(in,line)){std::istringstream iss(line);std::string id,kind,name;if(!(iss>>id>>kind>>name)||id!=depot||kind=="depot")continue;std::string d,v,crc;if(!parse_name(name,d,v,crc))continue;std::uint64_t vv{};auto rr=std::from_chars(v.data(),v.data()+v.size(),vv);if(rr.ec!=std::errc()||rr.ptr!=v.data()+v.size()||vv>target)continue;Entry e{name,v,crc};if(kind=="dat")dats.push_back(std::move(e));else if(kind=="blob")blobs.push_back(std::move(e));}
        std::sort(dats.begin(),dats.end(),[](const Entry&a,const Entry&b){return std::stoull(a.version)<std::stoull(b.version);});std::sort(blobs.begin(),blobs.end(),[](const Entry&a,const Entry&b){return std::stoull(a.version)<std::stoull(b.version);});
        std::size_t n=0;for(const auto&e:dats){download_file(base,"dats",e.filename,fs::path(dir)/"dats");++n;}for(const auto&e:blobs){download_file(base,"blobs",e.filename,fs::path(dir)/"blobs");++n;}std::cout<<"Downloaded "<<n<<" files for depot "<<depot<<" through v"<<version<<" (history included for blob ancestry)\n";return 0;
    }
    if(mode=="extract"){
        std::string dir="steam2data",depot,version,out;std::optional<std::uint32_t> crc;for(int i=2;i<argc;++i){std::string a=argv[i];if(a=="--dir")dir=arg_value(i,argc,argv,a);else if(a=="--depot")depot=arg_value(i,argc,argv,a);else if(a=="--version")version=arg_value(i,argc,argv,a);else if(a=="--out")out=arg_value(i,argc,argv,a);else if(a=="--blobcrc"){std::string x=arg_value(i,argc,argv,a);if(x.rfind("0x",0)==0||x.rfind("0X",0)==0)x.erase(0,2);std::uint32_t val{};auto r=std::from_chars(x.data(),x.data()+x.size(),val,16);if(r.ec!=std::errc()||r.ptr!=x.data()+x.size())die("invalid --blobcrc");crc=val;}else if(a=="--help"||a=="-h"){usage();return 0;}else die("unknown option: "+a);}if(depot.empty()||version.empty())die("--depot and --version are required");if(out.empty())out=dir+"/extracted/"+depot+"_"+version;std::uint32_t id{},v{};auto rd=std::from_chars(depot.data(),depot.data()+depot.size(),id);auto rv=std::from_chars(version.data(),version.data()+version.size(),v);if(rd.ec!=std::errc()||rv.ec!=std::errc())die("invalid depot/version");extract_depot(fs::path(dir)/"blobs",fs::path(dir)/"dats",id,v,out,crc);return 0;
    }
    usage();return 0;
}
}
int main(int argc,char**argv){try{return run(argc,argv);}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<'\n';return 1;}}
