#ifndef PROJECT_H_LCSDWOGL
#define PROJECT_H_LCSDWOGL

#include "TranslationUnit.h"
#include "TypeAlias.h"
#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <vector>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <boost/filesystem.hpp>
#include <leveldb/db.h>
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>

namespace symdb {

namespace stdfs = boost::filesystem;
using FsPathVec = std::vector<fspath>;
using FsPathSet = std::set<fspath>;

using SmartLevelDBPtr = std::unique_ptr< leveldb::DB >;
using SmartCXIndex = RawPointerWrap<CXIndex>;

class DB_SymbolInfo;
class BatchWriter;

struct ProjectFileInfo {
    time_t last_mtime;
    std::string content_md5;
};

class ProjectFileWatcher {
public:
    explicit ProjectFileWatcher(const fspath &path);
    ProjectFileWatcher(const ProjectFileWatcher&) = delete;

    ~ProjectFileWatcher();

    const fspath& abs_path() const { return abs_path_; }
    int fd() const { return fd_; }

private:
    fspath abs_path_;
    int fd_;
};

using WatcherPtr = std::unique_ptr<ProjectFileWatcher>;

class Project;
using ProjectPtr = std::shared_ptr<Project>;

class Project : public std::enable_shared_from_this<Project>
{
    using ModuleCompileFlagsMap = std::map<std::string, StringVecPtr>;
    using RelativeDirModuleMap = std::map<std::string, std::string>;
    friend class BatchWriter;

public:
    static ProjectPtr CreateFromDatabase(const std::string &name);
    static ProjectPtr CreateFromConfig(const std::string &name, const std::string &home_dir);

    explicit Project(const std::string &name);
    ~Project() = default;

    void Build();

    void ChangeHome(const std::string &new_home);

    void HandleFileModified(int wd, const std::string &path);
    void HandleFileDeleted(int wd, const std::string &path);

    bool LoadFileSymbolInfo(const fspath &path, SymbolMap &symbols) const;

    std::vector<Location> QuerySymbolDefinition(const std::string &symbol) const;

    Location QuerySymbolDefinition(const std::string &symbol, const fspath &abs_path) const;

    const fspath& home_path() const { return home_path_; }

    bool IsWatchFdInList(int file_wd) const;

private:
    void ChangeHomeNoCheck(fspath &&new_home);

    StringVecPtr GetModuleCompilationFlag(const std::string &module_name);

    void Initialize(bool is_new);

    void ClangParseFile(const fspath &home_path,
                        const fspath &abs_path,
                        StringVecPtr compile_flags);

    void OnParseCompleted(const fspath &relative_path,
                          const SymbolMap &new_symbols);

    std::string MakeFileInfoKey(const fspath &file_path) const;
    std::string MakeFileSymbolKey(const fspath &file_rel_path) const;
    std::string MakeSymbolKey(const std::string &symbol_name) const;

    bool LoadKey(const std::string &key, std::string &value) const;

    template <typename PBType>
    bool LoadKeyPBValue(const std::string &key, PBType &pb) const;

    void LoadProjectInfo();

    bool PutSingleKey(const std::string &key, const std::string &value);

    bool GetSymbolDBInfo(const std::string &symbol, DB_SymbolInfo &st) const;

    bool DoesFileContentChange(const fspath &abs_path) const;

    std::string GetModuleName(const fspath &path) const;

    Location GetSymbolLocation(const DB_SymbolInfo &st, const fspath &rel_path) const;

    void BuildFile(const fspath &abs_path);

    void UpdateFileWatch();

    void StartForceSyncTimer();
    void StartSmartSyncTimer();

    void ForceSync();

    void SmartSync();

    void DeleteUnexistFile(const fspath &deleted_path);
    void RebuildFiles(FsPathVec &paths);

    void LoadCmakeCompilationInfo(const fspath &build_path);

    void RebuildProject();

    void BuildModuleFlags();

    FsPathSet GetAllSubDirs();

    void AddFileWatch(const fspath &path);
    void RemoveFileWatch(const fspath &path);

private:
    std::string proj_name_;
    fspath home_path_; // it's absolute, dito
    fspath cmake_file_path_;
    SmartCXIndex cx_index_;
    SmartLevelDBPtr symbol_db_;
    FsPathSet all_sub_dirs_;
    FsPathSet abs_src_paths_;
    FsPathSet in_parsing_files_; // relative path
    FsPathVec modified_files_;
    boost::asio::deadline_timer smart_sync_timer_;
    boost::asio::deadline_timer force_sync_timer_;
    std::map<int, WatcherPtr> watchers_;

    ModuleCompileFlagsMap module_flags_map_;
    RelativeDirModuleMap rel_dir_module_map_;
    int64_t cmake_file_last_mtime_;
    mutable std::shared_mutex module_mutex_;
};

} /* symdb  */

#endif /* end of include guard: PROJECT_H_LCSDWOGL */
