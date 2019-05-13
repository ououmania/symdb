#include "Project.h"
#include "Config.h"
#include "Server.h"
#include "TranslationUnit.h"
#include "util/Logger.h"
#include "util/Functions.h"
#include "util/Exceptions.h"
#include "util/MD5.h"
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
    cx_index_ = SmartCXIndex { clang_createIndex(0, 1), clang_disposeIndex };
    if (!cx_index_) {
        THROW_AT_FILE_LINE("project<%s> failed to create index", name.c_str());
    }

    StartSmartSyncTimer();
    StartForceSyncTimer();
}

ProjectPtr Project::CreateFromDatabase(const std::string &name) {
    if (name.empty()) {
        THROW_AT_FILE_LINE("empty project name");
    }

    ProjectPtr project = std::make_shared<Project>(name);
    project->InitializeLevelDB(false, false);
    if (!project->LoadProjectInfo()) {
        THROW_AT_FILE_LINE("project<%s> load failed", name.c_str());
    }

    return project;
}

ProjectPtr Project::CreateFromConfigFile(const std::string &name, const fspath &home) {
    if (name.empty()) {
        THROW_AT_FILE_LINE("empty project name");
    }

    if (!filesystem::is_directory(home)) {
        THROW_AT_FILE_LINE("home_path<%s> is not directory", home.c_str());
    }

    ProjectPtr project = std::make_shared<Project>(name);
    project->InitializeLevelDB(true, true);
    project->ChangeHome(home);

    return project;
}

ProjectPtr Project::CreateFromConfig(std::shared_ptr<ProjectConfig> config) {
    if (!filesystem::is_directory(config->home_path())) {
        THROW_AT_FILE_LINE("home_path<%s> is not directory",
                           config->home_path().c_str());
    }

    ProjectPtr project = std::make_shared<Project>(config->name());
    project->SetConfig(config);
    project->InitializeLevelDB(true, false);
    project->LoadProjectInfo();
    project->ChangeHome(config->home_path());

    return project;
}

void Project::InitializeLevelDB(bool create_if_missing, bool error_if_exists) {
    fspath db_path { ConfigInst.db_path() };

    db_path /= proj_name_ + ".ldb";

    leveldb::Options options;
    options.create_if_missing = create_if_missing;
    options.error_if_exists = error_if_exists;

    leveldb::DB *raw_ptr;
    leveldb::Status status = leveldb::DB::Open(options, db_path.string(), &raw_ptr);
    if (!status.ok()) {
        THROW_AT_FILE_LINE("open db %s: %s", db_path.c_str(),
                                     status.ToString().c_str());
    }

    symbol_db_.reset(raw_ptr);
}

FsPathSet Project::GetAllSubDirs() {
    FsPathSet sub_dirs;

    filesystem::recursive_directory_iterator rdi(home_path_);
    filesystem::recursive_directory_iterator end_rdi;
    for (; rdi != end_rdi; ++rdi) {
        fspath abs_path;
        if (rdi->path().is_absolute()) {
            abs_path = rdi->path();
        } else {
            abs_path = filesystem::absolute(rdi->path(), home_path_);
        }

        if (!filesystem::is_directory(abs_path)) {
            continue;
        }

        if (symutil::path_has_prefix(abs_path, config_->build_path())) {
            continue;
        }

        fspath relative_path = filesystem::relative(abs_path, home_path_);
        sub_dirs.insert(relative_path);
    }

    return sub_dirs;
}

void Project::AddFileWatch(const fspath &path) {
    if (!path.is_absolute()) {
        fspath abs_path = filesystem::absolute(path, home_path_);
        return AddFileWatch(abs_path);
    }

    auto module_name = GetModuleName(path);
    if (module_name.empty()) {
        return;
    }

    try {
        WatcherPtr watcher { new ProjectFileWatcher { path } };
        watchers_[watcher->fd()] = std::move(watcher);
        LOG_INFO << "project=" << proj_name_ << " watch_path=" << path;
    } catch (const std::exception &e) {
        LOG_ERROR << "excpetion: " << e.what() << " project="
                  << proj_name_ << " watch_path=" << path;
    }
}

void Project::RemoveFileWatch(const fspath &path) {
    if (!path.is_absolute()) {
        fspath abs_path = filesystem::absolute(path, home_path_);
        return RemoveFileWatch(abs_path);
    }

    LOG_INFO << "project=" << proj_name_ << " path=" << path;

    for (auto it = watchers_.begin(); it != watchers_.end(); ++it) {
        if (it->second->abs_path() == path) {
            watchers_.erase(it);
            return;
        }
    }

    LOG_INFO << "watch not added, project=" << proj_name_ << " path=" << path;
}

void Project::UpdateSubDirs() {
    if (!config_->is_enable_file_watch()) {
        return;
    }

    FsPathSet new_sub_dirs = GetAllSubDirs();
    for (const auto &fs_path : new_sub_dirs) {
        if (all_sub_dirs_.find(fs_path) == all_sub_dirs_.end()) {
            AddFileWatch(fs_path);
        }
    }

    for (const auto &fs_path : all_sub_dirs_) {
        if (new_sub_dirs.find(fs_path) == new_sub_dirs.end()) {
            RemoveFileWatch(fs_path);
        }
    }

    all_sub_dirs_.swap(new_sub_dirs);

    LOG_DEBUG << "project=" << proj_name_ << " home=" << home_path_
              << " wd_size=" << watchers_.size();
}

void Project::Build() {
    BatchWriter batch { this };
    batch.WriteSrcPath();

    for (const auto &abs_path : abs_src_paths_) {
        if (config_->IsFileExcluded(abs_path)) {
            continue;
        }

        try {
            BuildFile(abs_path);
        } catch (const std::exception &e) {
            LOG_ERROR << "BuildFile error=" << e.what() << " project="
                      << proj_name_ << " path" << abs_path;
        }
    }
}

void Project::BuildFile(const fspath &abs_path) {
    std::string module_name = GetModuleName(abs_path);
    LOG_DEBUG << "BuildFile, project=" << proj_name_ << " path=" << abs_path
              << " module=" << module_name;

    if (module_name.empty()) {
        LOG_ERROR << "empty module, project=" << proj_name_ << " file=" << abs_path;
        return;
    }

    fspath relative_path = filesystem::relative(abs_path, home_path_);
    if (in_parsing_files_.find(relative_path) != in_parsing_files_.end()) {
        LOG_INFO << "file is in parsing, project=" << proj_name_
                 << " relative_path=" << relative_path;
        return;
    }

    StringVecPtr compiler_flags = GetModuleCompilationFlag(module_name);
    if (!compiler_flags) {
        LOG_DEBUG << "module miss, name=" << module_name << " file=" << abs_path;
        return;
    }

    in_parsing_files_.insert(relative_path);

    ServerInst.PostToWorker(std::bind(&Project::ClangParseFile,
        shared_from_this(), home_path_, abs_path, compiler_flags));
}

void Project::ChangeHome(const fspath &new_home) {
    fspath refined_path = filesystem::canonical(new_home);
    if (filesystem::equivalent(home_path_, refined_path)) {
        LOG_INFO << "home not change, project=" << proj_name_ << " home=" << new_home;
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
    ForceSync();
}

void Project::ClangParseFile(fspath home_path,
                             fspath abs_path,
                             StringVecPtr compile_flags) {
    assert(!ServerInst.IsInMainThread());

    auto file_info_key = MakeFileInfoKey(abs_path);
    DB_FileBasicInfo file_info;
    (void) LoadKeyPBValue(file_info_key, file_info);

    auto last_mtime = symdb::last_wtime(abs_path);

    LOG_DEBUG << "project=" << proj_name_ << ", path=" << abs_path
              << " saved_mtime=" << file_info.last_mtime() << ", last_mtime="
              << last_mtime;

    if (file_info.last_mtime() == last_mtime) {
        return;
    }

    std::string file_md5 = symutil::md5_stream_str(abs_path.c_str());
    if (file_md5 == file_info.content_md5()) {
        return;
    }

    LOG_DEBUG << "start, file=" << abs_path;

    TranslationUnit unit(abs_path.string(), *compile_flags, cx_index_.get());
    unit.CollectSymbols();

    LOG_DEBUG << "end, file=" << abs_path;

    // We just tell the main thread the relative path so we can change the home
    // easily even if the project is building.
    fspath relative_path = filesystem::relative(abs_path, home_path);
    WriteCompiledFile(relative_path, abs_path, file_md5, unit.defined_symbols());

    ServerInst.PostToMain(std::bind(&Project::OnParseCompleted,
        shared_from_this(), relative_path));
}

void Project::WriteCompiledFile(const fspath &relative_path,
                                const fspath &abs_path,
                                const std::string &md5,
                                const SymbolMap &new_symbols) {
    SymbolMap old_symbols;
    (void) LoadFileSymbolInfo(relative_path, old_symbols);

    BatchWriter batch { this };

    std::string module_name = GetModuleName(relative_path);

    auto put_symbol = [&](const std::string &symbol, const Location &loc) {
        auto symkey = MakeSymbolKey(symbol);

        DB_SymbolInfo st;
        (void) LoadKeyPBValue(symkey, st);
        AddSymbolLocation(st, module_name, loc);

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
            Location location = QuerySymbolDefinition(kv.first, relative_path);
            if (!(location == it->second)) {
                put_symbol(kv.first, it->second);
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

    auto last_mtime = symdb::last_wtime(abs_path);

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
}

void Project::OnParseCompleted(fspath relative_path) {
    assert(ServerInst.IsInMainThread());

    fspath abs_path = filesystem::absolute(relative_path, home_path_);
    if (in_parsing_files_.erase(relative_path) == 0) {
        LOG_INFO << "path is not in built, project=" << proj_name_
                 << " path=" << relative_path;
    }

    if (in_parsing_files_.size() < 5) {
        LOG_INFO << "project=" << proj_name_ << " in_parsing_files="
                 << in_parsing_files_.size();
    }

    if (abs_src_paths_.find(abs_path) == abs_src_paths_.end()) {
        LOG_INFO << "path already deleted, project=" << proj_name_
                 << " path=" << abs_path;
        return;
    }
}

bool Project::LoadProjectInfo() {
    std::string home_dir;
    if (!LoadKey(kSymdbProjectHomeKey, home_dir)) {
        return false;
    }

    DB_ProjectInfo db_info;
    if (!LoadKeyPBValue(proj_name_, db_info)) {
        THROW_AT_FILE_LINE("load project<%s> failed", proj_name_.c_str());
    }

    LOG_DEBUG << "project=" << proj_name_ << ", home=" << home_dir;

    home_path_ = fspath { home_dir };

    for (const auto &rel_path : db_info.rel_paths()) {
        LOG_DEBUG << "relative source file: " << rel_path;
        abs_src_paths_.insert(filesystem::absolute(rel_path, home_path_));
    }

    ChangeHomeNoCheck(home_dir);

    return true;
}

bool Project::LoadFileSymbolInfo(const fspath &file_path, SymbolMap &symbols) const
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
    const fspath &abs_path) const
{
    DB_SymbolInfo db_info;
    if (!GetSymbolDBInfo(symbol, db_info)) {
        return Location {};
    }

    Location location = GetSymbolLocation(db_info, abs_path);
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
    const fspath &file_path) const
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

    if (!fs_path.has_extension()) {
        return;
    }

    if (filesystem::equivalent(cmake_file_path_, fs_path)) {
        ForceSync();
    } else {
        auto ext = fs_path.extension();
        if (ext == "cc" || ext == "cpp") {
            modified_files_.push_back(fs_path);
        }
    }
}

void Project::HandleFileDeleted(int wd, const std::string &path)
{
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());

    fspath fs_path = it->second->abs_path() / path;

    if (filesystem::exists(fs_path)) {
        LOG_ERROR << "path still exists, project=" << proj_name_ << " path="
                  << fs_path;
        return;
    }

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
            if (!ec) {
                this->ForceSync();
                StartForceSyncTimer();
            }
        }
    );
}

void Project::ForceSync() {
    FsPathSet old_abs_paths(std::move(abs_src_paths_));

    try {
        BuildModuleFlags();
        UpdateSubDirs();
        for (const auto &abs_path : old_abs_paths) {
            if (abs_src_paths_.find(abs_path) == abs_src_paths_.end()) {
                DeleteUnexistFile(abs_path);
            }
        }

        Build();
    } catch (const std::exception &e) {
        LOG_ERROR << "exception: " << e.what() << " project=" << proj_name_;
    }
}

void Project::StartSmartSyncTimer() {
    smart_sync_timer_.expires_from_now(boost::posix_time::seconds(30));
    smart_sync_timer_.async_wait(
        [this](const auto &ec) {
            if (!ec) {
                this->SmartSync();
                StartSmartSyncTimer();
            }
        }
    );
}

void Project::SmartSync() {
    std::sort(modified_files_.begin(), modified_files_.end());
    auto uq_it = std::unique(modified_files_.begin(), modified_files_.end());
    modified_files_.erase(uq_it, modified_files_.end());

    if (modified_files_.empty()) return;

    LOG_DEBUG << "unique files=" << modified_files_.size();

    for (const auto &path : modified_files_) {
        try {
            BuildFile(path);
        } catch (const std::exception &e) {
            LOG_ERROR << "BuildFile error=" << e.what() << " project="
                      << proj_name_ << " path" << path;
        }
    }
}

void Project::DeleteUnexistFile(const fspath &deleted_path)
{
    LOG_INFO << "project=" << proj_name_ << " deleted_path=" << deleted_path;

    if (abs_src_paths_.erase(deleted_path) == 0) {
        LOG_INFO << "path is not added, project=" << proj_name_
                 << " path=" << deleted_path;
        return;
    }

    fspath relative_path = filesystem::relative(deleted_path, home_path_);
    in_parsing_files_.erase(relative_path);

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
            LOG_ERROR << "GetSymbolDBInfo failed, project=" << proj_name_
                      << " symbol=" << symbol;
            continue;
        }

        RemoveSymbolLocation(db_info, module_name);
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

    const char *build_dir = config_->build_path().c_str();

    const char *cmake_default_cmd = "cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1";

    fspath cmake_file_dir = cmake_file_path_.parent_path();

    char command[BUFSIZ];
    snprintf(command, sizeof(command), "%s -S %s -B %s &> /%s/error.txt",
             cmake_default_cmd, cmake_file_dir.c_str(), build_dir, build_dir);

    int ret = system(command);
    if (ret != 0) {
        THROW_AT_FILE_LINE("command<%s> build_dir<%s> system ret<%d>: %s",
            command, build_dir, ret, strerror(errno));
    }

    LoadCmakeCompilationInfo(config_->build_path());

    /*CXCompilationDatabase_Error status;
    auto database = clang_CompilationDatabase_fromDirectory(
                            build_path.c_str(),
                            &status );
    if (status == CXCompilationDatabase_NoError) {
        RawPointerWrap<CXCompilationDatabase> guard { database, clang_CompilationDatabase_dispose };
        LoadCmakeCompilationInfo(build_path, database);
    }*/
}

void Project::LoadCmakeCompilationInfo(const fspath &build_path)
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
            if (config_->IsFileExcluded(abs_file_path)) {
                continue;
            }

            if (symutil::path_has_prefix(abs_file_path, build_path)) {
                continue;
            }

            assert (symutil::path_has_prefix(abs_file_path, home_path_));

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

    LOG_DEBUG << "project=" << proj_name_ << ", files=" << abs_src_paths_.size();

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

void Project::AddSymbolLocation(DB_SymbolInfo &db_info,
                                const std::string &module_name,
                                const Location &location)
{
    assert(location.IsValid());

    auto locations = db_info.mutable_locations();
    for (auto it = locations->begin(); it != locations->end(); ++it) {
        auto pb_module_name = GetModuleName(it->path());
        if (module_name == pb_module_name) {
            location.Seriaize(*it);
            return;
        }
    }

    auto *pb_loc = db_info.add_locations();
    location.Seriaize(*pb_loc);
}

bool Project::RemoveSymbolLocation(DB_SymbolInfo &db_info,
                                   const std::string &module_name)
{
    auto locations = db_info.mutable_locations();
    for (auto it = locations->begin(); it != locations->end(); ++it) {
        auto pb_module_name = GetModuleName(it->path());
        if (module_name == pb_module_name) {
            locations->erase(it);
            return true;
        }
    }

    return false;
}

} /* symdb */

