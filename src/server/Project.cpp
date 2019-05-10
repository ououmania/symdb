#include "Project.h"
#include "Config.h"
#include "Server.h"
#include "TranslationUnit.h"
#include "util/Logger.h"
#include "util/Functions.h"
#include "util/Exceptions.h"
#include "proto/DBInfo.pb.h"
#include <istream>
#include <sys/inotify.h>
#include <leveldb/write_batch.h>
#include <boost/date_time.hpp>
#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <clang-c/CXCompilationDatabase.h>

namespace symdb {

namespace {

const char* kSymdbKeyDelimeter { ":" };
const std::string kSymdbProjectHomeKey = "home";

const std::initializer_list<std::string> kDefaultCompilerFlags {
    "g++", "-x", "c++", "-std=c++17",
};

StringVecPtr GetCompilerFlagsFromCmakeCommand(const std::string &command) {
    std::istringstream buffer(command);
    StringVec options;
    std::copy(std::istream_iterator<std::string>(buffer),
              std::istream_iterator<std::string>(),
              std::back_inserter(options));

    StringVec compile_options = kDefaultCompilerFlags;
    for (const auto &option : options) {
        if (option.length() > 2 && option[0] == '-' && (option[1] == 'I' || option[1] == 'D')) {
            compile_options.push_back(option);
        }
    }

    const auto &default_sys_dirs = ConfigInst.default_inc_dirs();
    std::copy(default_sys_dirs.begin(), default_sys_dirs.end(),
              std::back_inserter(compile_options));
    return std::make_shared<StringVec>(std::move(compile_options));
}

} // anonymous namespace

class BatchWriter {
public:
    explicit BatchWriter(Project *proj)
        : project_ {proj} {
    }

    template <typename PBType>
    void Put(const std::string &key, const PBType &pb) {
        batch_.Put(key, pb.SerializeAsString());
    }

    void Put(const std::string &key, const std::string &value) {
        batch_.Put(key, value);
    }

    template <typename PBType>
    void PutSymbol(const std::string &symbol, const PBType &pb) {
        batch_.Put(project_->MakeSymbolKey(symbol), pb.SerializeAsString());
    }

    void Delete(const std::string &key) {
        batch_.Delete(key);
    }

    void WriteSrcPath() {
        DB_ProjectInfo pt;
        pt.mutable_rel_paths()->Reserve(project_->abs_src_paths_.size());
        for (const auto &abs_path : project_->abs_src_paths_) {
            fspath rel_path = filesystem::relative(abs_path, project_->home_path());
            pt.add_rel_paths(rel_path.string());
        }
        Put(project_->proj_name_, pt);
    }

    ~BatchWriter() {
        leveldb::WriteOptions write_options;
        write_options.sync = false;
        leveldb::Status s = project_->symbol_db_->Write(write_options, &batch_);
        if (!s.ok()) {
            LOG_ERROR << "failed to write, error=" << s.ToString() << " project="
                      << project_->proj_name_;
        }
    }

private:
    Project *project_;
    leveldb::WriteBatch batch_;
};

ProjectFileWatcher::ProjectFileWatcher(const fspath &abs_path)
    : abs_path_ { abs_path },
      fd_ { -1 } {
    int mask = (IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    fd_ = inotify_add_watch(ServerInst.inotify_fd(), abs_path.c_str(), mask);
    if (fd_ < 0) {
        THROW_AT_FILE_LINE("inotify_add_watch error: %s", strerror(errno));
    }
}

ProjectFileWatcher::~ProjectFileWatcher() {
    assert (fd_ >= 0);
    int ret = inotify_rm_watch(ServerInst.inotify_fd(), fd_);
    if (ret < 0) {
        LOG_ERROR << "inotify_rm_watch error: " << strerror(errno);
    }
    ::close(fd_);
}

Project::Project(const std::string &name)
    : proj_name_ { name },
      smart_sync_timer_ { ServerInst.main_io_service() },
      force_sync_timer_ { ServerInst.main_io_service() } {
}

ProjectPtr Project::CreateFromDatabase(const std::string &name) {
    if (name.empty()) {
        THROW_AT_FILE_LINE("empty project name");
    }

    ProjectPtr project = std::make_shared<Project>(name);
    project->Initialize(true);
    project->LoadProjectInfo();

    return project;
}

ProjectPtr Project::CreateFromConfig(const std::string &name, const std::string &home_dir) {
    if (name.empty()) {
        THROW_AT_FILE_LINE("empty project name");
    }

    if (home_dir.empty()) {
        THROW_AT_FILE_LINE("empty project home");
    }

    fspath home_path(home_dir);
    if (!filesystem::is_directory(home_path)) {
        THROW_AT_FILE_LINE("home_path<%s> is not directory", home_path.c_str());
    }

    ProjectPtr project = std::make_shared<Project>(name);
    project->Initialize(true);
    project->ChangeHome(home_dir);
    project->Build();

    return project;
}

void Project::Initialize(bool is_new) {
    cx_index_ = SmartCXIndex { clang_createIndex(0, 1), clang_disposeIndex };
    if (!cx_index_) {
        throw symdb::ClangParseError("failed to create index");
    }

    fspath db_path { ConfigInst.db_path() };

    db_path /= proj_name_ + ".ldb";

    leveldb::Options options;
    options.create_if_missing = is_new;

    leveldb::DB *raw_ptr;
    leveldb::Status status = leveldb::DB::Open(options, db_path.string(), &raw_ptr);
    if (!status.ok()) {
        THROW_AT_FILE_LINE("open db %s: %s", db_path.c_str(),
                                     status.ToString().c_str());
    }

    symbol_db_.reset(raw_ptr);

    StartSmartSyncTimer();
    StartForceSyncTimer();
}

void Project::ResetFileWatch() {
    watchers_.clear();

    filesystem::recursive_directory_iterator rdi(home_path_);
    filesystem::recursive_directory_iterator end_rdi;

    fspath build_path = home_path_ / "_build";

    for (; rdi != end_rdi; ++rdi)
    {
        fspath abs_path;
        if (rdi->path().is_absolute()) {
            abs_path = rdi->path();
        } else {
            abs_path = filesystem::absolute(rdi->path(), home_path_);
        }

        if (!filesystem::is_directory(abs_path)) {
            continue;
        }

        if (symutil::path_has_prefix(abs_path, build_path)) {
            LOG_INFO << "project=" << proj_name_ << " exclude " << rdi->path();
            continue;
        }

        auto module_name = GetModuleName(abs_path);
        if (module_name.empty()) {
            continue;
        }

        try {
            WatcherPtr watcher { new ProjectFileWatcher { abs_path } };
            watchers_[watcher->fd()] = std::move(watcher);
        } catch (const std::exception &e) {
        }
        LOG_INFO << "project=" << proj_name_ << " watch_path=" << abs_path;
    }

    LOG_DEBUG << "project=" << proj_name_ << " home=" << home_path_
              << " wd_size=" << watchers_.size();
}

void Project::Build() {
    BatchWriter batch { this };
    batch.WriteSrcPath();

    for (const auto &abs_path : abs_src_paths_) {
        if (ConfigInst.IsFileExcluded(abs_path)) {
            continue;
        }

        if (!DoesFileContentChange(abs_path)) {
            continue;
        }

        BuildFile(abs_path);
    }

    LOG_DEBUG << "project=" << proj_name_ << ", files=" << abs_src_paths_.size();
}

void Project::BuildFile(const fspath &abs_path) {
    std::string module_name = GetModuleName(abs_path);
    LOG_DEBUG << "BuildFile, project=" << proj_name_ << " path=" << abs_path
              << " module=" << module_name;

    if (module_name.empty()) {
        LOG_ERROR << "empty module, project=" << proj_name_ << " file=" << abs_path;
        return;
    }

    StringVecPtr compiler_flags = GetModuleCompilationFlag(module_name);
    if (!compiler_flags) {
        LOG_DEBUG << "module miss, name=" << module_name << " file=" << abs_path;
        return;
    }

    ServerInst.PostToWorker(std::bind(&Project::ClangParseFile,
        shared_from_this(), abs_path.string(), compiler_flags, true));
}

void Project::ChangeHome(const std::string &new_home) {
    fspath refined_path = filesystem::canonical(new_home);
    if (filesystem::equivalent(home_path_, refined_path)) {
        LOG_INFO << "home not change, project=" << proj_name_;
        return;
    }

    ChangeHomeNoCheck(std::move(refined_path));
}

void Project::ChangeHomeNoCheck(fspath &&new_path) {
    if (!new_path.is_absolute()) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> is not absolute",
            proj_name_.c_str(), new_path.c_str());
    }

    auto status = filesystem::status(new_path);
    if (filesystem::is_symlink(status) || !filesystem::is_directory(status)) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> is not valid",
            proj_name_.c_str(), new_path.c_str());
    }

    fspath cmake_file_path = new_path / "CMakeLists.txt";
    if (!filesystem::exists(cmake_file_path)) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> has no CMakeLists.txt",
            proj_name_.c_str(), new_path.c_str());
    }

    if (!PutSingleKey(kSymdbProjectHomeKey, new_path.string())) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> put failed",
            proj_name_.c_str(), new_path.c_str());
    }

    home_path_.swap(new_path);
    cmake_file_path_.swap(cmake_file_path);

    // Although it may take some seconds and block the main thread, we think
    // it's acceptable. It's compilcated to post the task to the workers.
    RebuildProject();
}

void Project::ClangParseFile(const std::string &filename,
                             StringVecPtr compile_flags, bool is_build_proj) {
    assert(!ServerInst.IsInMainThread());

    TranslationUnit unit(filename, *compile_flags, cx_index_.get());
    unit.CollectSymbols();

    if (is_build_proj) {
        ServerInst.PostToMain(std::bind(&Project::OnParseCompleted,
            shared_from_this(), filename, std::move(unit.defined_symbols())));
    }
}

void Project::OnParseCompleted(const std::string &filename,
                               const SymbolMap &new_symbols) {
    assert(ServerInst.IsInMainThread());

    static int file_count = 0;

    LOG_DEBUG << "parsed_file_count=" << ++file_count;

    fspath abs_path(filename);

    fspath relative_path = filesystem::relative(abs_path, home_path_);

    SymbolMap old_symbols;
    (void) LoadFileSymbolInfo(relative_path.string(), old_symbols);

    leveldb::WriteBatch batch;

    auto put_symbol = [this, &batch](const std::string &symbol, const Location &loc) {
        auto symkey = MakeSymbolKey(symbol);

        DB_SymbolInfo st;
        if (LoadKeyPBValue(symkey, st)) {
            Location sym_loc = GetSymbolLocation(st, symbol);
            if (sym_loc.IsValid()) {
                LOG_DEBUG << "project=" << proj_name_ << " symbol=" << symbol
                          << " already exist:" << sym_loc;
                return;
            }

            LOG_DEBUG << "project=" << proj_name_ << " symbol=" << symbol
                      << " locations=" << st.locations_size();
        }

        auto *pb_loc = st.add_locations();
        loc.Seriaize(*pb_loc);
        auto loc_str = st.SerializeAsString();
        batch.Put(symkey, loc_str);
    };

    bool is_symbol_changed = false;
    for (const auto &kv : old_symbols) {
        auto it = new_symbols.find(kv.first);
        if (it == new_symbols.end()) {
            auto symkey = MakeSymbolKey(kv.first);
            batch.Delete(symkey);
            is_symbol_changed = true;
        } else {
            Location location = QuerySymbolDefinition(kv.first, filename);
            if (!(location == it->second)) {
                put_symbol(kv.first, location);
            }
        }
    }

    for (const auto &kv : new_symbols) {
        auto it = old_symbols.find(kv.first);
        if (it != old_symbols.end()) {
            continue;
        }

        is_symbol_changed = true;
        Location new_loc { relative_path.string(), kv.second.line_number(), kv.second.column_number() };
        put_symbol(kv.first, new_loc);
    }

    auto last_mtime = filesystem::last_write_time(abs_path);

    DB_FileBasicInfo file_table;
    file_table.set_last_mtime(last_mtime);
    file_table.set_content_md5("");

    auto filekey = MakeFileInfoKey(relative_path.string());
    batch.Put(filekey, file_table.SerializeAsString());

    if (is_symbol_changed) {
        auto file_symbol_key = MakeFileSymbolKey(relative_path.string());
        DB_FileSymbolInfo file_symbol_info;
        file_symbol_info.mutable_symbols()->Reserve(new_symbols.size());
        for (const auto &kv : new_symbols) {
            file_symbol_info.add_symbols(kv.first);
        }

        if (new_symbols.empty()) {
            batch.Delete(file_symbol_key);
        } else {
            batch.Put(file_symbol_key, file_symbol_info.SerializeAsString());
        }
    }

    leveldb::WriteOptions write_options;
    write_options.sync = false;
    leveldb::Status s = symbol_db_->Write(write_options, &batch);
    if (!s.ok()) {
        LOG_ERROR << "failed to write, error=" << s.ToString() << " project="
                  << proj_name_ << " file=" << filename;
    }

    LOG_DEBUG << "file parsed, proj_name_=" << proj_name_ << " file=" << relative_path;
}

void Project::LoadProjectInfo() {
    std::string home_dir;
    if (!LoadKey(kSymdbProjectHomeKey, home_dir)) {
        THROW_AT_FILE_LINE("project<%s> home not set", proj_name_.c_str());
    }

    DB_ProjectInfo db_info;
    if (!LoadKeyPBValue(proj_name_, db_info)) {
        THROW_AT_FILE_LINE("load project<%s> failed", proj_name_.c_str());
    }

    LOG_DEBUG << "project=" << proj_name_ << ", home=" << home_dir;

    for (const auto &rel_path : db_info.rel_paths()) {
        LOG_DEBUG << "file=" << rel_path;
        abs_src_paths_.insert(filesystem::absolute(rel_path, home_path_));
    }

    ChangeHomeNoCheck(home_dir);
}

bool Project::LoadFileSymbolInfo(const std::string &file_path, SymbolMap &symbols) const
{
    std::string file_key = MakeFileSymbolKey(file_path);

    DB_FileSymbolInfo db_info;
    if (!LoadKeyPBValue(file_key, db_info)) {
        return false;
    }

    for (const auto &symbol : db_info.symbols()) {
        Location location = QuerySymbolDefinition(symbol, file_path);
        if (!location.IsValid()) {
            LOG_ERROR << "QuerySymbolDefinition failed, project=" << proj_name_
                      << " file=" << file_path << " symbol=" << symbol;
            continue;
        }
        symbols[symbol] = location;
    }

    return true;
}

bool Project::GetSymbolDBInfo(const std::string &symbol, DB_SymbolInfo &st) const
{
    std::string symbol_key = MakeSymbolKey(symbol);

    if (!LoadKeyPBValue(symbol_key, st)) {
        LOG_ERROR << "failed to parse pb, project=" << proj_name_ << " symbol=" << symbol;
        return false;
    }

    return true;
}

std::vector<Location> Project::QuerySymbolDefinition(const std::string &symbol) const
{
    std::vector<Location> locations;

    DB_SymbolInfo db_info;
    if (!GetSymbolDBInfo(symbol, db_info)) {
        LOG_ERROR << "GetSymbolDBInfo failed, project=" << proj_name_ << " symbol=" << symbol;
        return locations;
    }

    locations.reserve(db_info.locations_size());
    for (const auto &pb_loc : db_info.locations()) {
        fspath rel_path(pb_loc.path());
        fspath abs_path = filesystem::absolute(rel_path, home_path_);
        locations.emplace_back(abs_path.string(), pb_loc.line(), pb_loc.column());
    }

    return locations;
}

// A symbol may appear more than once. Get the one match abs_path or the first
// one if there's none.
Location Project::QuerySymbolDefinition(const std::string &symbol,
    const std::string &abs_path) const
{
    DB_SymbolInfo db_info;
    if (!GetSymbolDBInfo(symbol, db_info)) {
        return Location {};
    }

    fspath relative_path = filesystem::relative(abs_path, home_path_);

    Location location = GetSymbolLocation(db_info, relative_path.string());
    if (location.IsValid()) {
        return location;
    }

    return Location { db_info.locations(0) };
}

std::string Project::MakeFileInfoKey(const fspath &file_path) const
{
    if (file_path.is_absolute()) {
        return MakeFileInfoKey(filesystem::relative(file_path, home_path_));
    }

    return symutil::str_join(kSymdbKeyDelimeter, "file", "info", file_path);
}

std::string Project::MakeFileSymbolKey(const fspath &file_path) const
{
    if (file_path.is_absolute()) {
        return MakeFileSymbolKey(filesystem::relative(file_path, home_path_));
    }

    return symutil::str_join(kSymdbKeyDelimeter, "file", "symbol", file_path);
}

std::string Project::MakeSymbolKey(const std::string &symbol_name) const
{
    return symutil::str_join(kSymdbKeyDelimeter, "symbol", symbol_name);
}

Location Project::GetSymbolLocation(
    const DB_SymbolInfo &st,
    const std::string &file_path) const
{
    std::string module_name = GetModuleName(file_path);
    if (module_name.empty()) {
        return Location {};
    }

    for (const auto &pb_loc : st.locations()) {
        auto pb_module_name = GetModuleName(pb_loc.path());
        if (pb_module_name.empty()) {
            LOG_WARN << "pb_module_name empty, project=" << proj_name_
                     << " path=" << pb_loc.path();
            continue;
        }
        if (module_name == pb_module_name) {
            fspath rel_path(pb_loc.path());
            fspath abs_path = filesystem::absolute(rel_path, home_path_);
            return Location { abs_path.string(), pb_loc.line(), pb_loc.column() };
        }
    }

    return Location {};
}

bool Project::LoadKey(const std::string &key, std::string &value) const
{
    leveldb::Status s = symbol_db_->Get(leveldb::ReadOptions(), key, &value);
    if (!s.ok() && !s.IsNotFound()) {
        LOG_ERROR << "LevelDB::Get failed, key=" << key << ", error="
                  << s.ToString();
    }

    return s.ok();
}

template <typename PBType>
bool Project::LoadKeyPBValue(const std::string &key, PBType &pb) const
{
    std::string value;

    if (!LoadKey(key, value)) {
        return false;
    }

    bool ok = pb.ParseFromString(value);
    if (!ok) {
        LOG_ERROR << "ParseFromString failed, project=" << proj_name_
                  << " key=" << key << ", pb_type=" << pb.GetTypeName();
    }

    return ok;
}

bool Project::PutSingleKey(const std::string &key, const std::string &value)
{
    leveldb::WriteOptions write_options;
    write_options.sync = false;
    leveldb::Status s = symbol_db_->Put(write_options, key, value);
    if (!s.ok()) {
        LOG_ERROR << "LevelDB::Put failed, error=" << s.ToString() << " project="
                  << proj_name_ << " key=" << key;
    }

    return s.ok();
}

bool Project::DoesFileContentChange(const fspath &abs_path) const {
    auto file_info_key = MakeFileInfoKey(abs_path);

    DB_FileBasicInfo file_info;
    if (!LoadKeyPBValue(file_info_key, file_info)) {
        return true;
    }

    auto last_mtime = filesystem::last_write_time(abs_path);
    LOG_DEBUG << "filename=" << abs_path << ", last_mtime=" << last_mtime;

    return (file_info.last_mtime() != last_mtime);
}

std::string Project::GetModuleName(const fspath &path) const {
    if (!path.is_absolute()) {
        fspath abs_path = filesystem::absolute(path, home_path_);
        return GetModuleName(abs_path);
    }

    fspath relative_dir;
    if (filesystem::is_directory(path)) {
        relative_dir = filesystem::relative(path, home_path_);
    } else {
        relative_dir = filesystem::relative(path.parent_path(), home_path_);
    }
    std::shared_lock lock_guard { module_mutex_ };
    auto it = rel_dir_module_map_.find(relative_dir.string());
    if (it != rel_dir_module_map_.end()) {
        return it->second;
    }
    return std::string {};
}

void Project::HandleFileModified(int wd, const std::string &path)
{
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());
    assert (path.front() != '/');

    // inotify gives us only the file name.
    fspath fs_path = it->second->abs_path() / path;

    LOG_DEBUG << "project=" << proj_name_ << " wd=" << wd << " path=" << fs_path;

    if (filesystem::equivalent(cmake_file_path_, fs_path)) {
        RebuildProject();
    } else {
        modified_files_.push_back(fs_path);
    }
}

void Project::HandleFileDeleted(int wd, const std::string &path)
{
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());

    fspath fs_path = it->second->abs_path() / path;

    LOG_DEBUG << "project=" << proj_name_ << ", path=" << fs_path;

    DeleteUnexistFile(fs_path);
}

void Project::StartForceSyncTimer() {
    namespace posix_time = boost::posix_time;
    using posix_time::time_duration;

    static std::initializer_list<time_duration> durations = {
        time_duration { 3, 30, 0 },
        time_duration { 8,  30, 0 },
        time_duration { 12, 30, 0 },
        time_duration { 18, 15, 0 },
        time_duration { 23, 30, 0 },
    };

    posix_time::ptime day = posix_time::second_clock::local_time();
    time_duration now = day.time_of_day();

    time_duration duration;
    auto it = std::upper_bound(durations.begin(), durations.end(), now);
    if (it == durations.end()) {
        duration = *durations.begin() + posix_time::minutes(60 * 24) - now;
    } else {
        duration = *it - now;
    }

    force_sync_timer_.expires_from_now(duration);
    force_sync_timer_.async_wait(
        [this](const auto &ec) {
            if(!ec) this->ForceSync();
        }
    );

    LOG_INFO <<  "project=" << proj_name_ << " next force sync at "
             << to_iso_extended_string(day + duration);
}

void Project::ForceSync() {
    StartForceSyncTimer();
}

void Project::StartSmartSyncTimer() {
    smart_sync_timer_.expires_from_now(boost::posix_time::seconds(60));
    smart_sync_timer_.async_wait(
        [this](const auto &ec) {
            if(!ec) this->SmartSync();
        }
    );
}

void Project::SmartSync() {
    std::sort(modified_files_.begin(), modified_files_.end());
    auto uq_it = std::unique(modified_files_.begin(), modified_files_.end());
    modified_files_.erase(uq_it, modified_files_.end());

    LOG_DEBUG << "unique files=" << modified_files_.size();

    for (const auto &path : modified_files_) {
        try {
            BuildFile(path);
        } catch (const std::exception &e) {
            LOG_ERROR << "BuildFile error=" << e.what() << " project="
                      << proj_name_ << " path" << path;
        }
    }

    StartSmartSyncTimer();
}

void Project::RebuildFiles(FsPathVec &paths) {
    std::sort(paths.begin(), paths.end());
    auto uq_it = std::unique(paths.begin(), paths.end());
    paths.erase(uq_it, paths.end());

    for (const auto &path : paths) {
        try {
            BuildFile(path);
        } catch (const std::exception &e) {
            LOG_ERROR << "BuildFile exception=" << e.what() << ", path" << path;
        }
    }
}

void Project::DeleteUnexistFile(const fspath &deleted_path)
{
    if (filesystem::exists(deleted_path)) {
        LOG_ERROR << "path still exists, project=" << proj_name_ << " paht="
                  << deleted_path;
        return;
    }

    if (abs_src_paths_.erase(deleted_path) == 0) {
        LOG_INFO << "path not added, project=" << proj_name_
                 << " path=" << deleted_path;
        return;
    }

    BatchWriter batch { this };

    std::string file_info_key = MakeFileInfoKey(deleted_path);
    batch.Delete(file_info_key);

    std::string module_name = GetModuleName(deleted_path);

    SymbolMap old_symbols;
    std::string file_symbol_key = MakeFileSymbolKey(deleted_path);

    DB_FileSymbolInfo db_fs_info;
    if (!LoadKeyPBValue(file_symbol_key, db_fs_info)) {
        LOG_ERROR << "file symbol info not exist, proj=" << proj_name_
                  << ", path=" << deleted_path;
        return;
    }

    for (const auto &symbol : db_fs_info.symbols()) {
        DB_SymbolInfo db_info;
        if (!GetSymbolDBInfo(symbol, db_info)) {
            LOG_ERROR << "GetSymbolDBInfo failed, project=" << proj_name_ << " symbol=" << symbol;
            continue;
        }

        for (auto it = db_info.locations().begin(); it != db_info.locations().end(); ++it) {
            auto pb_module_name = GetModuleName(it->path());
            if (module_name == pb_module_name) {
                db_info.mutable_locations()->erase(it);
            }
        }
        batch.PutSymbol(symbol, db_info);
    }

    batch.Delete(file_symbol_key);

    batch.WriteSrcPath();
}

bool Project::IsWatchFdInList(int file_wd) const {
    return watchers_.find(file_wd) != watchers_.end();
}

void Project::BuildModuleFlags() {
    if (!filesystem::exists(cmake_file_path_)) {
        THROW_AT_FILE_LINE("project<%s> cmake_file_path<%s> not exists",
            proj_name_.c_str(), cmake_file_path_.c_str());
    }

    char file_templ[] = "/tmp/symdb_buildXXXXXX";
    const char *tmp_dir = mkdtemp(file_templ);

    const char *cmake_default_cmd = "cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1";

    fspath cmake_file_dir = cmake_file_path_.parent_path();

    char command[BUFSIZ];
    snprintf(command, sizeof(command), "%s -S %s -B %s &> /dev/null",
             cmake_default_cmd, cmake_file_dir.c_str(), tmp_dir);

    int ret = system(command);
    if (ret != 0) {
        THROW_AT_FILE_LINE("system ret<%d>: %s", ret, strerror(errno));
    }

    fspath build_path { tmp_dir };
    fspath cmake_json_path = build_path / "compile_commands.json";
    if (!filesystem::exists(cmake_json_path)) {
        THROW_AT_FILE_LINE("compile_commands.json not exist");
    }

    LoadModuleCompilationFlag(build_path);
    /*CXCompilationDatabase_Error status;
    auto database = clang_CompilationDatabase_fromDirectory(
                            build_path.c_str(),
                            &status );
    if (status == CXCompilationDatabase_NoError) {
        RawPointerWrap<CXCompilationDatabase> guard { database, clang_CompilationDatabase_dispose };
        LoadModuleCompilationFlag(build_path, database);
        is_loaded_.store(true);
    }*/
}

void Project::RebuildProject() {
    try {
        BuildModuleFlags();
        ResetFileWatch();
        Build();
    } catch (const std::exception &e) {
        LOG_ERROR << "BuildModuleFlags exception: " << e.what();
    }
}

void Project::LoadModuleCompilationFlag(const fspath &build_path)
{
    fspath cmake_json_path = build_path / "compile_commands.json";
    if (!filesystem::exists(cmake_json_path)) {
        THROW_AT_FILE_LINE("compile_commands.json not exist");
    }

    try {
        std::ifstream ifs(cmake_json_path.c_str());
        boost::property_tree::ptree pt;
        read_json(ifs, pt);
        BOOST_FOREACH(boost::property_tree::ptree::value_type &v, pt) {
            std::string file_name = v.second.get<std::string>("file");
            fspath abs_file_path = filesystem::path(file_name);
            if (ConfigInst.IsFileExcluded(abs_file_path)) {
                LOG_DEBUG << "excluded " << abs_file_path;
                continue;
            }

            abs_src_paths_.insert(abs_file_path);
            fspath work_dir_path = fspath { v.second.get<std::string>("directory") };

            auto module_name = filesystem::relative(work_dir_path, build_path).string();
            fspath relative_dir = filesystem::relative(abs_file_path.parent_path(), home_path_);

            LOG_DEBUG << "file=" << abs_file_path << ", module=" << module_name
                      << " relative_dir=" << relative_dir;

            rel_dir_module_map_[relative_dir.string()] = module_name;

            if (module_flags_map_.find(module_name) != module_flags_map_.end()) {
                continue;
            }

            std::string command = v.second.get<std::string>("command");
            StringVecPtr compiler_flags = GetCompilerFlagsFromCmakeCommand(command);
            rel_dir_module_map_[module_name] = module_name;
            module_flags_map_[module_name] = compiler_flags;
        }
    } catch (const std::exception &e) {
        std::cerr << "Exception " << e.what() << std::endl;
    }

    /*RawPointerWrap<CXCompileCommands> commands(
      clang_CompilationDatabase_getAllCompileCommands(
        database), clang_CompileCommands_dispose );

    size_t num_commands = clang_CompileCommands_getSize( commands.get() );
    if (num_commands < 1) {
        return;
    }

    std::unique_lock lock_guard { module_mutex_ };

    for (size_t i = 0; i < num_commands; i++) {
        auto command = clang_CompileCommands_getCommand(commands.get(), i);

        std::string file_name = CXStringToString(clang_CompileCommand_getFilename(command));
        fspath abs_file_path = filesystem::path(file_name);

        // Ignore the files which are generated out of source.
        if (symutil::path_has_prefix(abs_file_path, build_path)) {
            continue;
        }

        abs_src_paths_.insert(abs_file_path);

        fspath work_dir_path { CXStringToString(
                               clang_CompileCommand_getDirectory( command ) ) };

        auto module_name = filesystem::relative(work_dir_path, build_path).string();
        fspath relative_dir = filesystem::relative(abs_file_path.parent_path(), home_path_);

        LOG_DEBUG << "file=" << file_name << ", module=" << module_name
                  << " relative_dir=" << relative_dir;

        rel_dir_module_map_[relative_dir.string()] = module_name;

        if (module_flags_map_.find(module_name) != module_flags_map_.end()) {
            continue;
        }

        rel_dir_module_map_[module_name] = module_name;

        size_t num_flags = clang_CompileCommand_getNumArgs( command );
        StringVecPtr compiler_flags = std::make_shared<StringVec>();
        compiler_flags->reserve( num_flags );

        for ( size_t j = 0; j < num_flags; ++j ) {
            compiler_flags->push_back(
                CXStringToString( clang_CompileCommand_getArg( command, j ) ) );
        }
        module_flags_map_[module_name] = compiler_flags;
    }*/
}

StringVecPtr Project::GetModuleCompilationFlag(const std::string &module_name) {
    std::shared_lock lock_guard { module_mutex_ };
    auto it = module_flags_map_.find(module_name);
    if (it != module_flags_map_.end()) {
        return it->second;
    }
    return StringVecPtr {};
}

} /* symdb */

