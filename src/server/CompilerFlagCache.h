#ifndef COMPILERFLAGCACHE_H_R6QHBXFU
#define COMPILERFLAGCACHE_H_R6QHBXFU

#include "TypeAlias.h"
#include <string>
#include <list>

namespace symdb {

class Project;

class CompilerFlagCache
{
    using ModuleCompileFlagsMap = std::map<std::string, StringVecPtr>;
    using RelativeDirModuleMap = std::map<std::string, std::string>;

public:
    explicit CompilerFlagCache(Project *project)
        : project_ { project } {
    }

    StringVecPtr GetModuleCompilerFlags(const std::string &module_name);
    StringVecPtr GetFileCompilerFlags(const fspath &path);

    void Rebuild(const fspath &cmake_file_path,
                 const fspath &build_path,
                 FsPathSet &abs_src_paths);

    std::string GetModuleName(const fspath &path) const;

private:
    void LoadCompileCommandsJsonFile(const fspath &build_path,
                                     FsPathSet &abs_src_paths);

    void LoadClangCompilationDatabase(const fspath &build_path,
                                      FsPathSet &abs_src_paths);

    template <class CommandParserType>
    void ParseFileCommand(const CommandParserType &parser,
                          const fspath &build_path);

private:
    Project *project_;
    ModuleCompileFlagsMap module_flags_;
    RelativeDirModuleMap rel_dir_module_map_;
};


} /* symdb  */

#endif /* end of include guard: COMPILERFLAGCACHE_H_R6QHBXFU */
