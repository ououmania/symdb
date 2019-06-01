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
#include <ctime>
#include <sys/inotify.h>
#include <leveldb/write_batch.h>
#include <boost/date_time.hpp>

namespace symdb {

const char* kSymdbKeyDelimeter { ":" };
const std::string kSymdbProjectHomeKey = "home";

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

    void Delete(const std::string &key) {
        batch_.Delete(key);
    }

    template <typename PBType>
    void PutSymbol(const std::string &symbol, const PBType &pb) {
        batch_.Put(project_->MakeSymbolKey(symbol), pb.SerializeAsString());
    }

    template <typename PBType>
    void PutFile(const fspath &path, const PBType &pb) {
        LOG_DEBUG << "project=" << project_->name() << ", path=" << path;
        batch_.Put(project_->MakeFileInfoKey(path), pb.SerializeAsString());
    }

    void DeleteFile(const fspath &path) {
        LOG_DEBUG << "project=" << project_->name() << ", path=" << path;
        batch_.Delete(project_->MakeFileInfoKey(path));
    }

    void WriteSrcPath() {
        DB_ProjectInfo pt;
        pt.mutable_rel_paths()->Reserve(project_->abs_src_paths_.size());
        for (const auto &abs_path : project_->abs_src_paths_) {
            fspath rel_path = filesystem::relative(abs_path, project_->home_path());
            pt.add_rel_paths(rel_path.string());
        }
        Put(project_->name_, pt);
    }

    ~BatchWriter() {
        leveldb::WriteOptions write_options;
        write_options.sync = false;
        leveldb::Status s = project_->symbol_db_->Write(write_options, &batch_);
        if (!s.ok()) {
            LOG_ERROR << "failed to write, error=" << s.ToString() << " project="
                      << project_->name_;
        }
    }

private:
    Project *project_;
    leveldb::WriteBatch batch_;
};

ProjectFileWatcher::ProjectFileWatcher(const fspath &abs_path)
    : abs_path_ { abs_path },
      fd_ { -1 } {
    int mask = (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE |
                IN_DELETE_SELF	| IN_MOVED_TO);
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
    : name_ { name },
      smart_sync_timer_ { ServerInst.main_io_service() },
      force_sync_timer_ { ServerInst.main_io_service() },
      flag_cache_ { this } {
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

    db_path /= name_ + ".ldb";

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

FsPathSet Project::GetWatchDirs() {
    FsPathSet sub_dirs;

    LOG_DEBUG << "project=" << name_ << " home=" << home_path_;

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

        if (IsFileExcluded(abs_path)) {
            continue;
        }

        fspath relative_path = filesystem::relative(abs_path, home_path_);
        auto module_name = flag_cache_.GetModuleName(relative_path);
        if (module_name.empty()) {
            continue;
        }

        LOG_DEBUG << "project=" << name_ << " sub_dir=" << relative_path;
        sub_dirs.insert(abs_path);
    }

    return sub_dirs;
}

void Project::AddFileWatch(const fspath &path) {
    assert(path.is_absolute());

    auto module_name = GetModuleName(path);
    if (module_name.empty()) {
        return;
    }

    try {
        WatcherPtr watcher { new ProjectFileWatcher { path } };
        watchers_[watcher->fd()] = std::move(watcher);
        LOG_INFO << "project=" << name_ << " watch_path=" << path;
    } catch (const std::exception &e) {
        LOG_ERROR << "excpetion: " << e.what() << " project="
                  << name_ << " watch_path=" << path;
    }
}

void Project::RemoveFileWatch(const fspath &path) {
    assert(path.is_absolute());

    LOG_INFO << "project=" << name_ << " path=" << path;

    for (auto it = watchers_.begin(); it != watchers_.end(); ++it) {
        if (it->second->abs_path() == path) {
            watchers_.erase(it);
            return;
        }
    }

    LOG_INFO << "watch not added, project=" << name_ << " path=" << path;
}

void Project::UpdateWatchDirs() {
    if (!config_->is_enable_file_watch()) {
        return;
    }

    FsPathSet old_watch_dirs;
    for (const auto &kvp : watchers_) {
        old_watch_dirs.insert(kvp.second->abs_path());
    }

    FsPathSet new_watch_dirs = GetWatchDirs();
    LOG_DEBUG << "project=" << name_ << " new_watch_dirs="
              << new_watch_dirs.size();

    for (const auto &fs_path : new_watch_dirs) {
        if (old_watch_dirs.find(fs_path) == old_watch_dirs.end()) {
            AddFileWatch(fs_path);
        }
    }

    for (const auto &fs_path : old_watch_dirs) {
        if (new_watch_dirs.find(fs_path) == new_watch_dirs.end()) {
            RemoveFileWatch(fs_path);
        }
    }

    LOG_DEBUG << "project=" << name_ << " home=" << home_path_
              << " wd_size=" << watchers_.size();
}

void Project::Build() {
    // excludeDeclsFromPCH = 1, displayDiagnostics=0
    SmartCXIndex cx_index { clang_createIndex(1, 0), clang_disposeIndex };
    if (!cx_index) {
        THROW_AT_FILE_LINE("project<%s> failed to create index", name_.c_str());
    }

    BatchWriter batch { this };
    batch.WriteSrcPath();

    for (const auto &abs_path : abs_src_paths_) {
        if (config_->IsFileExcluded(abs_path)) {
            continue;
        }

        try {
            BuildFile(cx_index, abs_path);
        } catch (const std::exception &e) {
            LOG_ERROR << "BuildFile error=" << e.what() << " project="
                      << name_ << " path" << abs_path;
        }
    }
}

void Project::BuildFile(SmartCXIndex cx_index, const fspath &abs_path) {
    fspath relative_path = filesystem::relative(abs_path, home_path_);
    if (in_parsing_files_.find(relative_path) != in_parsing_files_.end()) {
        LOG_INFO << "file is in parsing, project=" << name_
                 << " relative_path=" << relative_path;
        return;
    }

    StringVecPtr compiler_flags = flag_cache_.GetFileCompilerFlags(abs_path);
    if (!compiler_flags) {
        LOG_DEBUG << "file has no compiler flags, project=" << name_
                  << " file=" << abs_path;
        return;
    }

    in_parsing_files_.insert(relative_path);

    ServerInst.PostToWorker(std::bind(&Project::ClangParseFile,
        shared_from_this(), cx_index, home_path_, abs_path, compiler_flags));
}

void Project::ChangeHome(const fspath &new_home) {
    fspath refined_path = filesystem::canonical(new_home);
    if (filesystem::equivalent(home_path_, refined_path)) {
        LOG_INFO << "home not change, project=" << name_ << " home=" << new_home;
        return;
    }

    ChangeHomeNoCheck(std::move(refined_path));
}

void Project::ChangeHomeNoCheck(fspath &&new_path) {
    if (!new_path.is_absolute()) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> is not absolute",
            name_.c_str(), new_path.c_str());
    }

    auto status = filesystem::status(new_path);
    if (filesystem::is_symlink(status) || !filesystem::is_directory(status)) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> is not valid",
            name_.c_str(), new_path.c_str());
    }

    fspath cmake_file_path = new_path / "CMakeLists.txt";
    if (!filesystem::exists(cmake_file_path)) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> has no CMakeLists.txt",
            name_.c_str(), new_path.c_str());
    }

    if (!PutSingleKey(kSymdbProjectHomeKey, new_path.string())) {
        THROW_AT_FILE_LINE("project<%s> new_home<%s> put failed",
            name_.c_str(), new_path.c_str());
    }

    home_path_.swap(new_path);
    cmake_file_path_.swap(cmake_file_path);

    // Although it may take some seconds and block the main thread, we think
    // it's acceptable. It's compilcated to post the task to the workers.
    ForceSync();
}

void Project::ClangParseFile(SmartCXIndex cx_index,
                             fspath home_path,
                             fspath abs_path,
                             StringVecPtr compile_flags) {
    assert(!ServerInst.IsInMainThread());

    fspath relative_path = filesystem::relative(abs_path, home_path);

    auto file_info_key = MakeFileInfoKey(relative_path);
    DB_FileBasicInfo file_info;
    (void) LoadKeyPBValue(file_info_key, file_info);

    auto last_mtime = symdb::last_wtime(abs_path);

    LOG_DEBUG << "project=" << name_ << ", path=" << abs_path
              << " saved_mtime=" << file_info.last_mtime() << ", last_mtime="
              << last_mtime;

    // We just tell the main thread the relative path so we can change the home
    // easily even if the project is building.
    symutil::FunctionRunnerGuard unused_guard { [&]() {
        ServerInst.PostToMain(std::bind(&Project::RemoveParsingFile,
            shared_from_this(), relative_path));
    } };

    if (file_info.last_mtime() == last_mtime) {
        return;
    }

    std::string file_md5 = symutil::md5_file_str(abs_path.c_str());
    if (file_info.content_md5() == file_md5) {
        return;
    }

    LOG_DEBUG << "start, file=" << abs_path;

    try {
        TranslationUnitPtr clang_unit = std::make_shared<TranslationUnit>(
            abs_path.string(), *compile_flags, cx_index.get());
        clang_unit->CollectSymbols();
        ServerInst.PostToMain(std::bind(&Project::WriteCompiledFile,
            shared_from_this(), clang_unit, relative_path,
            CompiledFileInfo { file_md5, last_mtime }));
    } catch (const std::exception &e) {
        LOG_ERROR << "exception: " << e.what() << ", project=" << name_
                  << ", file=" << relative_path;
    }

    LOG_DEBUG << "end, file=" << abs_path;
}

void Project::WriteCompiledFile(TranslationUnitPtr tu,
                                fspath relative_path,
                                CompiledFileInfo info) {
    const auto &new_symbols = tu->defined_symbols();
    if (new_symbols.empty()) {
        LOG_ERROR << "empty symbols, project=" << name_ << " file=" << relative_path;
        return;
    }

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
                is_symbol_changed = true;
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

    DB_FileBasicInfo file_table;
    file_table.set_last_mtime(info.last_mtime);
    file_table.set_content_md5(info.md5);

    batch.PutFile(relative_path, file_table);

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

void Project::RemoveParsingFile(fspath relative_path) {
    assert(ServerInst.IsInMainThread());

    fspath abs_path = filesystem::absolute(relative_path, home_path_);
    if (in_parsing_files_.erase(relative_path) == 0) {
        LOG_INFO << "path is not in built, project=" << name_
                 << " path=" << relative_path;
    }

    if (in_parsing_files_.size() < 5) {
        LOG_INFO << "project=" << name_ << " in_parsing_files="
                 << in_parsing_files_.size();
    }

    if (abs_src_paths_.find(abs_path) == abs_src_paths_.end()) {
        LOG_INFO << "path already deleted, project=" << name_
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
    if (!LoadKeyPBValue(name_, db_info)) {
        THROW_AT_FILE_LINE("load project<%s> failed", name_.c_str());
    }

    LOG_DEBUG << "project=" << name_ << ", home=" << home_dir;

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
            LOG_ERROR << "QuerySymbolDefinition failed, project=" << name_
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
        LOG_ERROR << "GetSymbolDBInfo failed, project=" << name_ << " symbol=" << symbol;
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
            LOG_WARN << "pb_module_name empty, project=" << name_
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
    leveldb::ReadOptions options;
    options.fill_cache = false;
    leveldb::Status s = symbol_db_->Get(options, key, &value);
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
        LOG_ERROR << "ParseFromString failed, project=" << name_
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
                  << name_ << " key=" << key;
    }

    return s.ok();
}

std::string Project::GetModuleName(const fspath &path) const {
    if (!path.is_absolute()) {
        fspath abs_path = filesystem::absolute(path, home_path_);
        return GetModuleName(abs_path);
    }

    return flag_cache_.GetModuleName(path);
}

void Project::HandleEntryCreate(int wd, bool is_dir, const std::string &path) {
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());
    assert (path.front() != '/');

    // inotify gives us only the file name.
    fspath fs_path = it->second->abs_path() / path;

    LOG_DEBUG << "project=" << name_ << " wd=" << wd << " path=" << fs_path;

    if (is_dir) {
        auto module_name = flag_cache_.GetModuleName(fs_path.parent_path());
        assert(!module_name.empty());
        flag_cache_.AddDirToModule(fs_path, module_name);
        return;
    }

    if (!fs_path.has_extension()) {
        return;
    }

    auto ext = fs_path.extension().string();
    if (ext == ".cc" || ext == ".cpp") {
        abs_src_paths_.insert(fs_path);
        modified_files_.push_back(fs_path);
    }
}

void Project::HandleFileModified(int wd, const std::string &path) {
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());
    assert (path.front() != '/');

    // inotify gives us only the file name.
    fspath fs_path = it->second->abs_path() / path;

    LOG_DEBUG << "project=" << name_ << " wd=" << wd << " path=" << fs_path;

    if (!fs_path.has_extension()) {
        return;
    }

    if (filesystem::equivalent(cmake_file_path_, fs_path)) {
        ForceSync();
    } else {
        auto ext = fs_path.extension().string();
        if (ext == ".cc" || ext == ".cpp") {
            modified_files_.push_back(fs_path);
        }
    }
}

void Project::HandleEntryDeleted(int wd, bool is_dir, const std::string &path) {
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());

    fspath fs_path = it->second->abs_path() / path;

    if (filesystem::exists(fs_path)) {
        LOG_ERROR << "path still exists, project=" << name_ << " path="
                  << fs_path;
        return;
    }

    if (!is_dir) {
        DeleteUnexistFile(fs_path);
        return;
    }

    if (!flag_cache_.TryRemoveDir(fs_path)) {
        return;
    }

    LOG_WARN << "delete-self is not handled, project=" << name_
             << " deleted_path=" << fs_path;
    for (auto wit = watchers_.begin(); wit != watchers_.end(); ) {
        if (symutil::path_has_prefix(wit->second->abs_path(), fs_path)) {
            auto tmp_it = wit++;
            watchers_.erase(tmp_it);
        } else {
            ++wit;
        }
    }
}

void Project::HandleWatchedDirDeleted(int wd, const std::string &path) {
    auto it = watchers_.find(wd);
    assert (it != watchers_.end());
    assert (path.front() != '/');

    const auto &fs_path = it->second->abs_path();
    LOG_DEBUG << "project=" << name_ << " wd=" << wd << " path=" << fs_path;

    // inotify emits file-delete event before the directory is deleted. So
    // we know the files under this directory is alreday removed from both
    // abs_src_paths_ and the database.
    if (!flag_cache_.TryRemoveDir(fs_path)) {
        LOG_ERROR << "delete from flag cache failed, project=" << name_
                  << " path=" <<  fs_path;
    }
    watchers_.erase(it);
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

    auto pt_tm = posix_time::to_tm(day + duration);
    char time_buf[BUFSIZ];
    (void) strftime(time_buf, sizeof(time_buf), "%F %H:%M", &pt_tm);
    LOG_DEBUG << "project=" << name_ << " next force sync at: " << time_buf;

    force_sync_timer_.expires_from_now(duration);
    force_sync_timer_.async_wait(
        [this](const boost::system::error_code &ec) {
            if (!ec) {
                LOG_DEBUG << "start to sync forcefully, project=" << name_;
                this->ForceSync();
                StartForceSyncTimer();
            }
        }
    );
}

void Project::ForceSync() {
    FsPathSet old_abs_paths(std::move(abs_src_paths_));

    try {
        flag_cache_.Rebuild(cmake_file_path_, config_->build_path(), abs_src_paths_);
        UpdateWatchDirs();
        for (const auto &abs_path : old_abs_paths) {
            if (abs_src_paths_.find(abs_path) == abs_src_paths_.end()) {
                DeleteUnexistFile(abs_path);
            }
        }

        Build();
    } catch (const std::exception &e) {
        LOG_ERROR << "exception: " << e.what() << " project=" << name_;
    }
}

void Project::StartSmartSyncTimer() {
    smart_sync_timer_.expires_from_now(boost::posix_time::seconds(30));
    smart_sync_timer_.async_wait(
        [this](const boost::system::error_code &ec) {
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

    // excludeDeclsFromPCH = 1, displayDiagnostics=0
    SmartCXIndex cx_index { clang_createIndex(1, 0), clang_disposeIndex };

    for (const auto &path : modified_files_) {
        try {
            BuildFile(cx_index, path);
        } catch (const std::exception &e) {
            LOG_ERROR << "BuildFile error=" << e.what() << " project="
                      << name_ << " path" << path;
        }
    }

    modified_files_.clear();
}

void Project::DeleteUnexistFile(const fspath &deleted_path)
{
    LOG_INFO << "project=" << name_ << " deleted_path=" << deleted_path;

    if (abs_src_paths_.erase(deleted_path) == 0) {
        LOG_INFO << "path is not added, project=" << name_
                 << " path=" << deleted_path;
        return;
    }

    fspath relative_path = filesystem::relative(deleted_path, home_path_);
    in_parsing_files_.erase(relative_path);

    BatchWriter batch { this };

    batch.DeleteFile(deleted_path);

    std::string module_name = GetModuleName(deleted_path);

    SymbolMap old_symbols;
    std::string file_symbol_key = MakeFileSymbolKey(deleted_path);

    DB_FileSymbolInfo db_fs_info;
    if (!LoadKeyPBValue(file_symbol_key, db_fs_info)) {
        LOG_ERROR << "file symbol info not exist, proj=" << name_
                  << ", path=" << deleted_path;
        return;
    }

    for (const auto &symbol : db_fs_info.symbols()) {
        DB_SymbolInfo db_info;
        if (!GetSymbolDBInfo(symbol, db_info)) {
            LOG_ERROR << "GetSymbolDBInfo failed, project=" << name_
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

bool Project::IsFileExcluded(const fspath &path) const {

    if (symutil::path_has_prefix(path, config_->build_path())) {
        return true;
    }
    return config_->IsFileExcluded(path);
}

} /* symdb */

