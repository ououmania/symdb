#include "Project.h"
#include <assert.h>
#include <leveldb/write_batch.h>
#include <sys/inotify.h>
#include <boost/date_time.hpp>
#include <ctime>
#include <istream>
#include "Config.h"
#include "Server.h"
#include "TranslationUnit.h"
#include "proto/DBInfo.pb.h"
#include "util/Exceptions.h"
#include "util/Functions.h"
#include "util/Logger.h"
#include "util/MD5.h"

namespace symdb {

const char *kSymdbKeyDelimiter{":"};
const std::string kSymdbProjectHomeKey = "home";

class BatchWriter {
public:
  explicit BatchWriter(Project *proj) : project_{proj} {}

  template <typename PBType>
  std::enable_if_t<std::is_base_of_v<google::protobuf::Message, PBType>, void>
  Put(const std::string &key, const PBType &pb) {
    Put(key, pb.SerializeAsString());
  }

  void Put(const std::string &key, const std::string &value) {
    batch_.Put(key, value);
    ++batch_count_;
  }

  void Delete(const std::string &key) {
    batch_.Delete(key);
    ++batch_count_;
  }

  template <typename PBType>
  void PutSymbol(const std::string &symbol, const PBType &pb) {
    Put(project_->MakeSymbolDefineKey(symbol), pb.SerializeAsString());
  }

  template <typename PBType>
  void PutFile(const fspath &path, const PBType &pb) {
    LOG_DEBUG << "project=" << project_->name() << ", path=" << path;
    Put(project_->MakeFileInfoKey(path), pb.SerializeAsString());
  }

  void DeleteFile(const fspath &path) {
    LOG_DEBUG << "project=" << project_->name() << ", path=" << path;
    Delete(project_->MakeFileInfoKey(path));
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

  void Clear() {
    batch_.Clear();
    batch_count_ = 0;
  }

  ~BatchWriter() {
    if (batch_count_) {
      leveldb::WriteOptions write_options;
      write_options.sync = false;
      leveldb::Status s = project_->symbol_db_->Write(write_options, &batch_);
      if (!s.ok()) {
        LOG_ERROR << "failed to write, error=" << s.ToString()
                  << " project=" << project_->name_;
      }
    }
  }

private:
  Project *project_;
  leveldb::WriteBatch batch_;
  int batch_count_ = 0;
};

ProjectFileWatcher::ProjectFileWatcher(const fspath &abs_path)
    : abs_path_{abs_path}, fd_{-1} {
  int mask = (IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE |
              IN_DELETE_SELF | IN_MOVED_TO);
  fd_ = inotify_add_watch(ServerInst.inotify_fd(), abs_path.c_str(), mask);
  if (fd_ < 0) {
    THROW_AT_FILE_LINE("inotify_add_watch error: %s", strerror(errno));
  }
}

ProjectFileWatcher::~ProjectFileWatcher() {
  assert(fd_ >= 0);
  int inotify_fd = ServerInst.inotify_fd();
  if (inotify_fd >= 0) {
    int ret = inotify_rm_watch(inotify_fd, fd_);
    if (ret < 0) {
      LOG_ERROR << "inotify_rm_watch error: " << strerror(errno);
    }
  }
  ::close(fd_);
}

Project::Project(const std::string &name)
    : name_{name},
      smart_sync_timer_{ServerInst.main_io_service()},
      force_sync_timer_{ServerInst.main_io_service()},
      flag_cache_{this} {
  StartSmartSyncTimer();
  StartForceSyncTimer();
}

ProjectPtr Project::CreateFromDatabase(const std::string &name,
                                       ProjectConfigPtr config) {
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

ProjectPtr Project::CreateFromConfigFile(const std::string &name,
                                         const fspath &home) {
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
  project->config_ = config;
  fspath db_path{ConfigInst.db_path()};
  db_path /= project->name() + ".ldb";
  if (filesystem::exists(db_path)) {
    project->InitializeLevelDB(false, false);
    if (!project->LoadProjectInfo()) {
      LOG_WARN << "rmdir " << db_path << " after loading failed";
      filesystem::remove_all(db_path);
    }
  }
  if (!filesystem::exists(db_path)) {
    project->InitializeLevelDB(true, true);
  }

  project->ChangeHome(config->home_path());

  return project;
}

void Project::InitializeLevelDB(bool create_if_missing, bool error_if_exists) {
  fspath db_path{ConfigInst.db_path()};

  db_path /= name_ + ".ldb";

  leveldb::Options options;
  options.create_if_missing = create_if_missing;
  options.error_if_exists = error_if_exists;

  leveldb::DB *raw_ptr = nullptr;
  leveldb::Status status =
      leveldb::DB::Open(options, db_path.string(), &raw_ptr);
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
      abs_path = symutil::absolute_path(rdi->path(), home_path_);
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
    WatcherPtr watcher{new ProjectFileWatcher{path}};
    watchers_[watcher->fd()] = std::move(watcher);
    LOG_INFO << "project=" << name_ << " watch_path=" << path;
  } catch (const std::exception &e) {
    LOG_ERROR << "exception: " << e.what() << " project=" << name_
              << " watch_path=" << path;
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
  LOG_DEBUG << "project=" << name_
            << " new_watch_dirs=" << new_watch_dirs.size();

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
  SmartCXIndex cx_index{clang_createIndex(1, 0), clang_disposeIndex};
  if (!cx_index) {
    THROW_AT_FILE_LINE("project<%s> failed to create index", name_.c_str());
  }

  BatchWriter batch{this};
  batch.WriteSrcPath();

  for (const auto &abs_path : abs_src_paths_) {
    if (config_->IsFileExcluded(abs_path)) {
      continue;
    }

    try {
      BuildFile(cx_index, abs_path);
    } catch (const std::exception &e) {
      LOG_ERROR << "BuildFile error=" << e.what() << " project=" << name_
                << " path" << abs_path;
    }
  }
}

void Project::RebuildFile(const fspath &abs_path) {
  assert(filesystem::exists(abs_path));

  fspath relative_path = filesystem::relative(abs_path, home_path_);
  if (in_parsing_files_.find(relative_path) != in_parsing_files_.end()) {
    return;
  }

  {
    BatchWriter batch{this};
    batch.DeleteFile(relative_path);
    DeleteFileDefinedSymbolInfo(relative_path, batch);
    DeleteFileReferredSymbolInfo(relative_path, batch);
  }

  SmartCXIndex cx_index{clang_createIndex(1, 0), clang_disposeIndex};
  try {
    BuildFile(cx_index, abs_path);
  } catch (const std::exception &e) {
    LOG_ERROR << "BuildFile error=" << e.what() << " project=" << name_
              << " path=" << abs_path;
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
                                    shared_from_this(), cx_index, home_path_,
                                    abs_path, compiler_flags));
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
    THROW_AT_FILE_LINE("project<%s> new_home<%s> is not valid", name_.c_str(),
                       new_path.c_str());
  }

  if (!filesystem::exists(config_->cmake_file())) {
    THROW_AT_FILE_LINE("project<%s> new_home<%s> has no CMakeLists.txt",
                       name_.c_str(), new_path.c_str());
  }

  if (!PutSingleKey(kSymdbProjectHomeKey, new_path.string())) {
    THROW_AT_FILE_LINE("project<%s> new_home<%s> put failed", name_.c_str(),
                       new_path.c_str());
  }

  home_path_.swap(new_path);

  // Although it may take some seconds and block the main thread, we think
  // it's acceptable. It's complicated to post the task to the workers.
  ForceSync();
}

void Project::ClangParseFile(SmartCXIndex cx_index, fspath home_path,
                             fspath abs_path, StringVecPtr compile_flags) {
  assert(!ServerInst.IsInMainThread());

  int64_t last_mtime = 0;
  try {
    last_mtime = symutil::last_wtime(abs_path);
  } catch (const std::exception &e) {
    LOG_ERROR << "exception=" << e.what() << ", project=" << name_
              << ", path=" << abs_path;
    return;
  }

  fspath relative_path = filesystem::relative(abs_path, home_path);

  auto file_info_key = MakeFileInfoKey(relative_path);
  DB_FileBasicInfo file_info;
  (void)LoadKeyPBValue(file_info_key, file_info);

  LOG_DEBUG << "project=" << name_ << ", path=" << abs_path
            << " saved_mtime=" << file_info.last_mtime()
            << ", last_mtime=" << last_mtime;

  // We just tell the main thread the relative path so we can change the home
  // easily even if the project is building.
  symutil::FunctionRunnerGuard unused_guard{[&]() {
    ServerInst.PostToMain(std::bind(&Project::RemoveParsingFile,
                                    shared_from_this(), relative_path));
  }};

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
    ServerInst.PostToMain(
        std::bind(&Project::WriteCompiledFile, shared_from_this(), clang_unit,
                  relative_path, CompiledFileInfo{file_md5, last_mtime}));
  } catch (const std::exception &e) {
    LOG_ERROR << "exception: " << e.what() << ", project=" << name_
              << ", file=" << relative_path;
  }

  LOG_DEBUG << "end, file=" << abs_path;
}

void Project::WriteCompiledFile(TranslationUnitPtr tu, fspath relative_path,
                                CompiledFileInfo info) {
  if (relative_path.is_absolute()) {
    relative_path = symutil::absolute_path(relative_path, home_path_);
  }

  BatchWriter writer{this};

  DB_FileBasicInfo file_table;
  file_table.set_last_mtime(info.last_mtime);
  file_table.set_content_md5(info.md5);

  writer.PutFile(relative_path, file_table);

  try {
    WriteFileDefinitions(tu, relative_path, writer);
    WriteFileReferences(tu, relative_path, writer);
  } catch (const std::exception &e) {
    LOG_ERROR << "project=" << name_ << " file=" << relative_path
              << " error=" << e.what();
    writer.Clear();
  }
}

void Project::WriteFileDefinitions(TranslationUnitPtr tu, fspath relative_path,
                                   BatchWriter &writer) {
  const auto &new_symbols = tu->defined_symbols();
  LOG_INFO << "project=" << name_ << " file=" << relative_path
           << " symbols=" << new_symbols.size();

  SymbolDefinitionMap old_symbols;
  (void)LoadFileDefinedSymbolInfo(relative_path, old_symbols);

  std::string module_name = GetModuleName(relative_path);

  auto put_symbol = [&](const std::string &symbol, const Location &loc) {
    auto symkey = MakeSymbolDefineKey(symbol);

    DB_SymbolDefinitionInfo st;
    (void)LoadKeyPBValue(symkey, st);
    AddSymbolLocation(st, module_name, loc);

    auto loc_str = st.SerializeAsString();
    writer.Put(symkey, loc_str);
  };

  bool is_symbol_changed = false;
  for (const auto &kv : old_symbols) {
    auto it = new_symbols.find(kv.first);
    if (it == new_symbols.end()) {
      LOG_INFO << "project=" << name_ << " file=" << relative_path
               << " deleted_symbol=" << kv.first;
      auto symkey = MakeSymbolDefineKey(kv.first);
      writer.Delete(symkey);
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
    Location new_loc{relative_path.string(), kv.second.line_number(),
                     kv.second.column_number()};
    put_symbol(kv.first, new_loc);
  }

  if (is_symbol_changed) {
    auto file_symbol_key = MakeFileSymbolDefineKey(relative_path.string());
    DB_FileSymbolInfo file_symbol_info;
    file_symbol_info.mutable_symbols()->Reserve(new_symbols.size());
    for (const auto &kv : new_symbols) {
      file_symbol_info.add_symbols(kv.first);
    }

    if (new_symbols.empty()) {
      writer.Delete(file_symbol_key);
    } else {
      writer.Put(file_symbol_key, file_symbol_info.SerializeAsString());
    }
  }
}

void Project::WriteFileReferences(TranslationUnitPtr tu, fspath relative_path,
                                  BatchWriter &writer) {
  FileSymbolReferenceMap new_symbols;
  int nr_referred = 0;
  for (const auto &kvp : tu->reference_symbols()) {
    const auto &symbol = kvp.first.first;
    const auto &path = kvp.first.second;
    std::string module_name = GetModuleName(path);
    SymbolModulePair sym_mod{symbol, module_name};
    auto &loc_set = new_symbols[sym_mod];
    for (const auto &loc : kvp.second) {
      loc_set.insert({loc.first, loc.second});
      ++nr_referred;
    }
  }

  LOG_INFO << "project=" << name_ << " file=" << relative_path
           << " referred_symbols=" << nr_referred;

  FileSymbolReferenceMap old_symbols;
  (void)LoadFileReferredSymbolInfo(relative_path, old_symbols);

  auto put_symbol_reference = [&](const std::string &symbol_name,
                                  const SymbolReferenceLocationMap &sym_locs) {
    auto symbol_key = MakeSymbolReferKey(symbol_name);
    if (sym_locs.empty()) {
      writer.Delete(symbol_key);
      return;
    }
    DB_SymbolReferenceInfo db_info;
    db_info.mutable_items()->Reserve(sym_locs.size());
    for (const auto &kvp : sym_locs) {
      auto *item = db_info.add_items();
      item->set_module_name(kvp.first);
      item->mutable_path_locs()->Reserve(kvp.second.size());
      for (const auto &kvp2 : kvp.second) {
        auto *path_loc = item->add_path_locs();
        path_loc->set_path(kvp2.first.string());
        path_loc->mutable_locations()->Reserve(kvp2.second.size());
        for (const auto &loc : kvp2.second) {
          auto *pb_loc = path_loc->add_locations();
          pb_loc->set_line(loc.first);
          pb_loc->set_column(loc.second);
        }
      }
    }
    writer.Put(symbol_key, db_info);
  };

  bool is_symbol_changed = false;
  for (const auto &kv : old_symbols) {
    auto it = new_symbols.find(kv.first);
    if (it != new_symbols.end()) {
      continue;
    }

    const auto &sym_name = kv.first.first;
    const auto &mod_name = kv.first.second;

    SymbolReferenceLocationMap sym_locs;
    if (!LoadSymbolReferenceInfo(sym_name, sym_locs)) {
      LOG_DEBUG << "symref=" << sym_name << " not in db";
      continue;
    }

    auto ref_it = sym_locs.find(mod_name);
    if (ref_it == sym_locs.end()) {
      continue;
    }

    is_symbol_changed = true;
    ref_it->second.erase(relative_path);
    if (ref_it->second.empty()) {
      sym_locs.erase(ref_it);
    }
    put_symbol_reference(sym_name, sym_locs);
  }

  for (const auto &kv : new_symbols) {
    const auto &sym_name = kv.first.first;
    const auto &mod_name = kv.first.second;
    auto it = old_symbols.find(kv.first);
    if (it == old_symbols.end() || it->second != kv.second) {
      SymbolReferenceLocationMap sym_locs;
      (void)LoadSymbolReferenceInfo(sym_name, sym_locs);
      sym_locs[mod_name][relative_path] = kv.second;
      is_symbol_changed = true;
      put_symbol_reference(sym_name, sym_locs);
    }
  }

  if (is_symbol_changed) {
    auto file_symbol_key = MakeFileSymbolReferKey(relative_path.string());
    DB_FileReferenceInfo file_symbol_info;
    file_symbol_info.mutable_symbols()->Reserve(new_symbols.size());
    for (const auto &kv : new_symbols) {
      auto item = file_symbol_info.add_symbols();
      item->mutable_locations()->Reserve(kv.second.size());
      item->set_symbol_name(kv.first.first);
      item->set_module_name(kv.first.second);
      for (const auto &loc : kv.second) {
        auto *pb_loc = item->add_locations();
        pb_loc->set_line(loc.first);
        pb_loc->set_column(loc.second);
      }
    }
    writer.Put(file_symbol_key, file_symbol_info);
  }
}

void Project::RemoveParsingFile(fspath relative_path) {
  assert(ServerInst.IsInMainThread());

  fspath abs_path = symutil::absolute_path(relative_path, home_path_);
  if (in_parsing_files_.erase(relative_path) == 0) {
    LOG_INFO << "path is not in built, project=" << name_
             << " path=" << relative_path;
  }

  if (in_parsing_files_.size() < 5) {
    LOG_INFO << "project=" << name_
             << " in_parsing_files=" << in_parsing_files_.size();
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
    return false;
  }

  LOG_DEBUG << "project=" << name_ << ", home=" << home_dir;

  home_path_ = fspath{home_dir};

  // This project may not exist in config.
  if (!config_) {
    RestoreConfig();
  }

  for (const auto &rel_path : db_info.rel_paths()) {
    LOG_DEBUG << "relative source file: " << rel_path;
    fspath p = home_path_ / rel_path;
    if (!filesystem::exists(p)) {
      LOG_DEBUG << "file doesn't exist on disk: " << rel_path;
      continue;
    }
    abs_src_paths_.insert(symutil::absolute_path(rel_path, home_path_));
  }

  ChangeHomeNoCheck(home_dir);

  return true;
}

bool Project::LoadFileDefinedSymbolInfo(const fspath &file_path,
                                        SymbolDefinitionMap &symbols) const {
  std::string file_key = MakeFileSymbolDefineKey(file_path);

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

bool Project::LoadFileReferredSymbolInfo(
    const fspath &file_path, FileSymbolReferenceMap &symbols) const {
  std::string file_key = MakeFileSymbolReferKey(file_path);

  DB_FileReferenceInfo db_info;
  if (!LoadKeyPBValue(file_key, db_info)) {
    return false;
  }

  for (const auto &symbol : db_info.symbols()) {
    SymbolModulePair smp{symbol.symbol_name(), symbol.module_name()};
    auto &lcs = symbols[smp];
    for (const auto &item : symbol.locations()) {
      lcs.insert({item.line(), item.column()});
    }
  }

  return true;
}

bool Project::LoadSymbolReferenceInfo(
    const std::string &symbol_name,
    SymbolReferenceLocationMap &sym_locs) const {
  auto symbol_key = MakeSymbolReferKey(symbol_name);

  DB_SymbolReferenceInfo db_info;
  if (!LoadKeyPBValue(symbol_key, db_info)) {
    LOG_DEBUG << "symbol=" << symbol_name << " no references";
    return false;
  }

  for (const auto &item : db_info.items()) {
    auto &sym_refs = sym_locs[item.module_name()];
    for (const auto &path_loc : item.path_locs()) {
      auto &file_info = sym_refs[path_loc.path()];
      for (const auto &loc : path_loc.locations()) {
        file_info.insert({loc.line(), loc.column()});
      }
    }
  }

  return true;
}

bool Project::GetSymbolDefinitionInfo(const std::string &symbol,
                                      DB_SymbolDefinitionInfo &st) const {
  std::string symbol_key = MakeSymbolDefineKey(symbol);

  if (!LoadKeyPBValue(symbol_key, st)) {
    return false;
  }

  return true;
}

std::vector<Location> Project::QuerySymbolDefinition(
    const std::string &symbol) const {
  std::vector<Location> locations;

  DB_SymbolDefinitionInfo db_info;
  if (!GetSymbolDefinitionInfo(symbol, db_info)) {
    LOG_ERROR << "GetSymbolDefinitionInfo failed, project=" << name_
              << " symbol=" << symbol;
    return locations;
  }

  locations.reserve(db_info.locations_size());
  for (const auto &pb_loc : db_info.locations()) {
    fspath rel_path(pb_loc.path());
    fspath abs_path = symutil::absolute_path(rel_path, home_path_);
    locations.emplace_back(abs_path.string(), pb_loc.line(), pb_loc.column());
  }

  return locations;
}

// A symbol may appear more than once. Get the one match abs_path or the first
// one if there's none.
Location Project::QuerySymbolDefinition(const std::string &symbol,
                                        const fspath &abs_path) const {
  DB_SymbolDefinitionInfo db_info;
  if (!GetSymbolDefinitionInfo(symbol, db_info)) {
    return Location{};
  }

  Location location = GetSymbolLocation(db_info, abs_path);
  if (location.IsValid()) {
    return location;
  }

  if (db_info.locations().empty()) {
    return Location{};
  }

  return Location{db_info.locations(0)};
}

std::string Project::MakeFileInfoKey(const fspath &file_path) const {
  if (file_path.is_absolute()) {
    return MakeFileInfoKey(filesystem::relative(file_path, home_path_));
  }

  return symutil::str_join(kSymdbKeyDelimiter, "file", "info", file_path);
}

std::string Project::MakeFileSymbolDefineKey(const fspath &file_path) const {
  if (file_path.is_absolute()) {
    return MakeFileSymbolDefineKey(filesystem::relative(file_path, home_path_));
  }

  return symutil::str_join(kSymdbKeyDelimiter, "file", "symdef", file_path);
}

std::string Project::MakeFileSymbolReferKey(const fspath &file_path) const {
  if (file_path.is_absolute()) {
    return MakeFileSymbolReferKey(filesystem::relative(file_path, home_path_));
  }

  return symutil::str_join(kSymdbKeyDelimiter, "file", "symref", file_path);
}

std::string Project::MakeSymbolDefineKey(const std::string &symbol_name) const {
  return symutil::str_join(kSymdbKeyDelimiter, "symdef", symbol_name);
}

std::string Project::MakeSymbolReferKey(const std::string &symbol_name) const {
  return symutil::str_join(kSymdbKeyDelimiter, "symref", symbol_name);
}

Location Project::GetSymbolLocation(const DB_SymbolDefinitionInfo &st,
                                    const fspath &file_path) const {
  std::string module_name = GetModuleName(file_path);
  if (module_name.empty()) {
    return Location{};
  }

  for (const auto &pb_loc : st.locations()) {
    auto abs_path = symutil::absolute_path(home_path_, pb_loc.path());
    if (!filesystem::exists(abs_path)) {
      LOG_WARN << "file may be deleted! project=" << name_
               << " path=" << pb_loc.path() << " abs_path=" << abs_path;
      continue;
    }
    auto pb_module_name = GetModuleName(pb_loc.path());
    if (pb_module_name.empty()) {
      LOG_WARN << "pb_module_name empty, project=" << name_
               << " path=" << pb_loc.path();
      continue;
    }
    if (module_name == pb_module_name) {
      fspath rel_path(pb_loc.path());
      fspath abs_path = symutil::absolute_path(rel_path, home_path_);
      return Location{abs_path.string(), pb_loc.line(), pb_loc.column()};
    }
  }

  return Location{};
}

bool Project::LoadKey(const std::string &key, std::string &value) const {
  leveldb::ReadOptions options;
  options.fill_cache = false;
  leveldb::Status s = symbol_db_->Get(options, key, &value);
  if (!s.ok() && !s.IsNotFound()) {
    LOG_ERROR << "LevelDB::Get failed, key=" << key
              << ", error=" << s.ToString();
  }

  return s.ok();
}

template <typename PBType>
bool Project::LoadKeyPBValue(const std::string &key, PBType &pb) const {
  std::string value;

  if (!LoadKey(key, value)) {
    LOG_ERROR << "key " << key << " doesn't exist.";
    return false;
  }

  bool ok = pb.ParseFromString(value);
  if (!ok) {
    LOG_ERROR << "ParseFromString failed, project=" << name_ << " key=" << key
              << ", pb_type=" << pb.GetTypeName();
  }

  return ok;
}

bool Project::PutSingleKey(const std::string &key, const std::string &value) {
  leveldb::WriteOptions write_options;
  write_options.sync = false;
  leveldb::Status s = symbol_db_->Put(write_options, key, value);
  if (!s.ok()) {
    LOG_ERROR << "LevelDB::Put failed, error=" << s.ToString()
              << " project=" << name_ << " key=" << key;
  }

  return s.ok();
}

std::string Project::GetModuleName(const fspath &path) const {
  if (!path.is_absolute()) {
    fspath abs_path = symutil::absolute_path(path, home_path_);
    return GetModuleName(abs_path);
  }

  return flag_cache_.GetModuleName(path);
}

void Project::HandleEntryCreate(int wd, bool is_dir, const std::string &path) {
  auto it = watchers_.find(wd);
  assert(it != watchers_.end());
  assert(path.front() != '/');

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
  if (symutil::is_cpp_source_ext(ext)) {
    abs_src_paths_.insert(fs_path);
    modified_files_.push_back(fs_path);
  }
}

void Project::HandleFileModified(int wd, const std::string &path) {
  auto it = watchers_.find(wd);
  assert(it != watchers_.end());
  assert(path.front() != '/');

  // inotify gives us only the file name.
  fspath fs_path = it->second->abs_path() / path;

  LOG_DEBUG << "project=" << name_ << " wd=" << wd << " path=" << fs_path;

  if (!fs_path.has_extension()) {
    return;
  }

  if (filesystem::equivalent(config_->cmake_file(), fs_path)) {
    ForceSync();
  } else {
    auto ext = fs_path.extension().string();
    if (symutil::is_cpp_source_ext(ext)) {
      modified_files_.push_back(fs_path);
    }
  }
}

void Project::HandleEntryDeleted(int wd, bool is_dir, const std::string &path) {
  auto it = watchers_.find(wd);
  assert(it != watchers_.end());

  fspath fs_path = it->second->abs_path() / path;

  if (filesystem::exists(fs_path)) {
    LOG_ERROR << "path still exists, project=" << name_ << " path=" << fs_path;
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
  for (auto wit = watchers_.begin(); wit != watchers_.end();) {
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
  assert(it != watchers_.end());
  assert(path.front() != '/');

  const auto &fs_path = it->second->abs_path();
  LOG_DEBUG << "project=" << name_ << " wd=" << wd << " path=" << fs_path;

  // inotify emits file-delete event before the directory is deleted. So
  // we know the files under this directory is already removed from both
  // abs_src_paths_ and the database.
  if (!flag_cache_.TryRemoveDir(fs_path)) {
    LOG_ERROR << "delete from flag cache failed, project=" << name_
              << " path=" << fs_path;
  }
  watchers_.erase(it);
}

void Project::StartForceSyncTimer() {
  namespace posix_time = boost::posix_time;
  using posix_time::time_duration;

  static std::initializer_list<time_duration> durations = {
      time_duration{3, 30, 0},  time_duration{8, 30, 0},
      time_duration{12, 30, 0}, time_duration{18, 15, 0},
      time_duration{23, 30, 0},
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
  (void)strftime(time_buf, sizeof(time_buf), "%F %H:%M", &pt_tm);
  LOG_DEBUG << "project=" << name_ << " next force sync at: " << time_buf;

  force_sync_timer_.expires_from_now(duration);
  force_sync_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {
      LOG_DEBUG << "start to sync forcefully, project=" << name_;
      this->ForceSync();
      StartForceSyncTimer();
    }
  });
}

void Project::ForceSync() {
  FsPathSet old_abs_paths(std::move(abs_src_paths_));

  try {
    flag_cache_.Rebuild(config_->cmake_file(), config_->build_path(),
                        abs_src_paths_);
    UpdateWatchDirs();
    for (const auto &abs_path : old_abs_paths) {
      if (abs_src_paths_.find(abs_path) == abs_src_paths_.end()) {
        DeleteUnexistFile(abs_path);
      }
    }

    Build();
    modified_files_.clear();
  } catch (const std::exception &e) {
    LOG_ERROR << "exception: " << e.what() << " project=" << name_;
  }
}

void Project::StartSmartSyncTimer() {
  smart_sync_timer_.expires_from_now(boost::posix_time::seconds(30));
  smart_sync_timer_.async_wait([this](const boost::system::error_code &ec) {
    if (!ec) {
      this->SmartSync();
      StartSmartSyncTimer();
    }
  });
}

void Project::SmartSync() {
  std::sort(modified_files_.begin(), modified_files_.end());
  auto uq_it = std::unique(modified_files_.begin(), modified_files_.end());
  modified_files_.erase(uq_it, modified_files_.end());

  if (modified_files_.empty()) return;

  LOG_DEBUG << "unique files=" << modified_files_.size();

  // excludeDeclsFromPCH = 1, displayDiagnostics=0
  SmartCXIndex cx_index{clang_createIndex(1, 0), clang_disposeIndex};

  for (const auto &path : modified_files_) {
    try {
      BuildFile(cx_index, path);
    } catch (const std::exception &e) {
      LOG_ERROR << "BuildFile error=" << e.what() << " project=" << name_
                << " path=" << path;
    }
  }

  modified_files_.clear();
}

void Project::DeleteUnexistFile(const fspath &deleted_path) {
  LOG_INFO << "project=" << name_ << " deleted_path=" << deleted_path;

  if (abs_src_paths_.erase(deleted_path) == 0) {
    LOG_INFO << "path is not added, project=" << name_
             << " path=" << deleted_path;
    return;
  }

  fspath relative_path = filesystem::relative(deleted_path, home_path_);
  in_parsing_files_.erase(relative_path);

  BatchWriter batch{this};

  batch.DeleteFile(deleted_path);

  DeleteFileDefinedSymbolInfo(relative_path, batch);
  DeleteFileReferredSymbolInfo(relative_path, batch);

  batch.WriteSrcPath();
}

void Project::DeleteFileDefinedSymbolInfo(const fspath &relative_path,
                                          BatchWriter &writer) const {
  std::string file_symbol_key = MakeFileSymbolDefineKey(relative_path);

  DB_FileSymbolInfo db_fs_info;
  if (!LoadKeyPBValue(file_symbol_key, db_fs_info)) {
    LOG_ERROR << "file symbol info not exist, proj=" << name_
              << ", path=" << relative_path;
    return;
  }

  std::string module_name = GetModuleName(relative_path);
  for (const auto &symbol : db_fs_info.symbols()) {
    DB_SymbolDefinitionInfo db_info;
    if (!GetSymbolDefinitionInfo(symbol, db_info)) {
      LOG_ERROR << "GetSymbolDefinitionInfo failed, project=" << name_
                << " symbol=" << symbol;
      continue;
    }

    RemoveSymbolLocation(db_info, module_name);
    writer.PutSymbol(symbol, db_info);
  }
}

void Project::DeleteFileReferredSymbolInfo(const fspath &relative_path,
                                           BatchWriter &writer) const {
  std::string file_symbol_key = MakeFileSymbolReferKey(relative_path);

  FileSymbolReferenceMap old_symbols;
  (void)LoadFileReferredSymbolInfo(relative_path, old_symbols);

  for (const auto &kvp : old_symbols) {
    SymbolReferenceLocationMap sym_locs;
    const auto &sym_name = kvp.first.first;
    const auto &mod_name = kvp.first.second;
    if (!LoadSymbolReferenceInfo(sym_name, sym_locs)) {
      continue;
    }
    auto it = sym_locs.find(mod_name);
    if (it == sym_locs.end()) {
      continue;
    }

    if (!it->second.erase(relative_path)) {
      continue;
    }

    auto symbol_key = MakeSymbolReferKey(sym_name);
    if (it->second.empty()) {
      sym_locs.erase(it);
      if (sym_locs.empty()) {
        writer.Delete(symbol_key);
        continue;
      }
    }

    DB_SymbolReferenceInfo db_info;
    db_info.mutable_items()->Reserve(sym_locs.size());
    for (const auto &kvp : sym_locs) {
      auto *item = db_info.add_items();
      item->set_module_name(kvp.first);
      item->mutable_path_locs()->Reserve(kvp.second.size());
      for (const auto &kvp2 : kvp.second) {
        auto *path_loc = item->add_path_locs();
        path_loc->set_path(kvp2.first.string());
        path_loc->mutable_locations()->Reserve(kvp2.second.size());
        for (const auto &loc : kvp2.second) {
          auto *pb_loc = path_loc->add_locations();
          pb_loc->set_line(loc.first);
          pb_loc->set_column(loc.second);
        }
      }
    }
    writer.Put(symbol_key, db_info);
  }
}

bool Project::IsWatchFdInList(int file_wd) const {
  return watchers_.find(file_wd) != watchers_.end();
}

void Project::AddSymbolLocation(DB_SymbolDefinitionInfo &db_info,
                                const std::string &module_name,
                                const Location &location) {
  assert(location.IsValid());

  auto locations = db_info.mutable_locations();
  for (auto it = locations->begin(); it != locations->end(); ++it) {
    auto pb_module_name = GetModuleName(it->path());
    if (module_name == pb_module_name) {
      location.Serialize(*it);
      return;
    }
  }

  auto *pb_loc = db_info.add_locations();
  location.Serialize(*pb_loc);
}

bool Project::RemoveSymbolLocation(DB_SymbolDefinitionInfo &db_info,
                                   const std::string &module_name) const {
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

void Project::RestoreConfig() {
  if (config_) {
    THROW_AT_FILE_LINE("project<%s> config is already set", name_.c_str());
  }
  config_ = std::make_shared<ProjectConfig>(name_.c_str(), home_path_.string());
  config_->is_enable_file_watch(true);
  config_->UseDefaultBuildPath();
}

}  // namespace symdb
