#include "CompilerFlagCache.h"
#include "Project.h"
#include "Config.h"
#include "util/Exceptions.h"
#include "util/Functions.h"
#include "util/Logger.h"
#include <list>
#include <algorithm>
#include <cstdlib>
#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <clang-c/CXCompilationDatabase.h>

namespace symdb {

// Use a regex to correctly detect c++/c language for both versioned and
// non-versioned compiler executable names suffixes
// (e.g., c++, g++, clang++, g++-4.9, clang++-3.7, c++-10.2 etc).
std::regex CPP_COMPILER_REGEX { R"--(\+\+(-\d+(\.\d+){0,2})?$)--" };

using CompilerFlagArgPair = std::pair<std::string, int>;

// An easy way is only caring about the '-I', '-D', '-W' flags. Otherwise, we
// have to exclude many other flags except those listed below.
std::initializer_list<CompilerFlagArgPair> kCompilerFlagsToSkip= {
    { "-c"                      , 0 } ,
    { "-MD"                     , 0 } ,
    { "-MMD"                    , 0 } ,
    { "-MP"                     , 0 } ,
    { "-rdynamic"               , 0 } ,
    { "--fcolor-diagnostics"    , 0 } ,
    { "-MF"                     , 1 } ,
    { "-MQ"                     , 1 } ,
    { "-MT"                     , 1 } ,
    { "-o"                      , 1 } ,
    { "--serialize-diagnostics" , 1 } ,
};

void PruneCompilerFlags(std::list<std::string> &flags, const std::string &filename) {
    for (auto it = flags.begin(); it != flags.end() && it->front() == '-'; ) {
        it = flags.erase(it);
    }

    if (flags.empty()) {
        return;
    }

    const auto &compiler = flags.front();
    auto it = std::next(flags.begin());
    if (std::regex_search(compiler, CPP_COMPILER_REGEX)) {
        it = std::next(flags.emplace(it, "-x"));
        it = std::next(flags.emplace(it, "c++"));
    }

    // If we don't remove the compiler, libclang complains like:
    //        /usr/bin/c++: 'linker' input unused.
    // libclang uses clang if available, but what if not?
    flags.erase(flags.begin());

    while (it != flags.end()) {
        const auto &flag = *it;

        auto it_skipped = std::find_if(kCompilerFlagsToSkip.begin(),
                                       kCompilerFlagsToSkip.end(),
                [&flag](const CompilerFlagArgPair &desc) {
                    return desc.first == flag;
                }
        );

        if (it_skipped != kCompilerFlagsToSkip.end()) {
            it = flags.erase(it);
            for (int i = 0; i < it_skipped->second; i++) {
                it = flags.erase(it);
            }
            continue;
        }

        if (flag.front() == '/' && flag == filename) {
            it = flags.erase(it);
        } else {
            ++it;
        }
    }
}

class JsonCommandParser {
    using JsonNodeType = boost::property_tree::ptree;
public:
    explicit JsonCommandParser(JsonNodeType &node)
        : node_ { node },
          filepath_ { node.get<std::string>("file") }
    {
    }

    const fspath& GetFileAbsPath() const { return filepath_; }

    std::string GetWorkDirectory() const {
        return node_.get<std::string>("directory");
    }

    std::list<std::string> GetFlags() const {
        std::string command = node_.get<std::string>("command");
        std::istringstream buffer(command);
        std::list<std::string> flags;
        std::copy(std::istream_iterator<std::string>(buffer),
                std::istream_iterator<std::string>(),
                std::back_inserter(flags));

        return flags;
    }

private:
    JsonNodeType &node_;
    fspath filepath_;
};

class ClangCommandParser {
public:
    explicit ClangCommandParser(CXCompileCommand command)
        : command_ { command },
          filepath_ { CXStringToString(clang_CompileCommand_getFilename(command_)) }
    {
    }

    const fspath& GetFileAbsPath() const { return filepath_; }

    std::string GetWorkDirectory() const {
        return CXStringToString(clang_CompileCommand_getDirectory(command_));
    }

    std::list<std::string> GetFlags() const {
        std::list<std::string> flags;

        size_t num_flags = clang_CompileCommand_getNumArgs(command_);
        for ( size_t j = 0; j < num_flags; ++j ) {
            std::string flag = CXStringToString(clang_CompileCommand_getArg(command_, j)) ;
            flags.push_back(flag);
        }
        return flags;
    }

private:
    CXCompileCommand command_;
    fspath filepath_;
};


void CompilerFlagCache::Rebuild(const fspath &cmake_file_path,
                                const fspath &build_path,
                                FsPathSet &abs_src_paths) {
    if (!filesystem::exists(cmake_file_path)) {
        THROW_AT_FILE_LINE("project<%s> cmake_file_path<%s> not exists",
            project_->name().c_str(), cmake_file_path.c_str());
    }

    const char *build_dir = build_path.c_str();

    const char *cmake_default_cmd = "cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1";

    fspath cmake_file_dir = cmake_file_path.parent_path();

    char command[BUFSIZ];
    snprintf(command, sizeof(command), "%s -S %s -B %s &> /%s/error.txt",
             cmake_default_cmd, cmake_file_dir.c_str(), build_dir, build_dir);

    int ret = system(command);
    if (ret != 0) {
        THROW_AT_FILE_LINE("command<%s> build_dir<%s> system ret<%d>: %s",
            command, build_dir, ret, strerror(errno));
    }

    module_flags_.clear();
    rel_dir_module_map_.clear();

    LoadClangCompilationDatabase(build_path, abs_src_paths);
}

void CompilerFlagCache::LoadCompileCommandsJsonFile(
    const fspath &build_path, FsPathSet &abs_src_paths)
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
            JsonCommandParser parser { v.second };
            const auto &abs_file_path = parser.GetFileAbsPath();
            if (project_->IsFileExcluded(abs_file_path)) {
                continue;
            }
            abs_src_paths.insert(abs_file_path);
            ParseFileCommand(parser, build_path);
        }
    } catch (const std::exception &e) {
        std::cerr << "Exception " << e.what() << std::endl;
    }
}

void CompilerFlagCache::LoadClangCompilationDatabase(
    const fspath &build_path, FsPathSet &abs_src_paths)
{
    CXCompilationDatabase_Error status;
    auto database = clang_CompilationDatabase_fromDirectory(
                            build_path.c_str(),
                            &status );
    if (status != CXCompilationDatabase_NoError) {
        LOG_WARN << "project=" << project_->name() << " failed to create "
                 << " compilation database: " << status;

        LoadCompileCommandsJsonFile(build_path, abs_src_paths);

        return;
    }

    RawPointerWrap<CXCompilationDatabase> guard { database, clang_CompilationDatabase_dispose };

    RawPointerWrap<CXCompileCommands> commands(
      clang_CompilationDatabase_getAllCompileCommands(
        database), clang_CompileCommands_dispose );

    size_t num_commands = clang_CompileCommands_getSize( commands.get() );
    if (num_commands < 1) {
        return;
    }

    for (size_t i = 0; i < num_commands; i++) {
        auto command = clang_CompileCommands_getCommand(commands.get(), i);

        ClangCommandParser parser { command };
        const auto &abs_file_path = parser.GetFileAbsPath();
        if (project_->IsFileExcluded(abs_file_path)) {
            continue;
        }
        abs_src_paths.insert(abs_file_path);
        ParseFileCommand(parser, build_path);
    }
}

StringVecPtr CompilerFlagCache::GetModuleCompilerFlags(const std::string &module_name) {
    auto it = module_flags_.find(module_name);
    if (it != module_flags_.end()) {
        return it->second;
    }
    return StringVecPtr {};
}

StringVecPtr CompilerFlagCache::GetFileCompilerFlags(const fspath &path) {
    std::string module = GetModuleName(path);
    if (module.empty()) {
        return StringVecPtr {};
    }

    return GetModuleCompilerFlags(module);
}

std::string CompilerFlagCache::GetModuleName(const fspath &path) const {
    if (!path.is_absolute()) {
        fspath abs_path = filesystem::absolute(path, project_->home_path());
        return GetModuleName(abs_path);
    }

    fspath relative_dir;
    if (filesystem::is_directory(path)) {
        relative_dir = filesystem::relative(path, project_->home_path());
    } else {
        relative_dir = filesystem::relative(path.parent_path(), project_->home_path());
    }
    auto it = rel_dir_module_map_.find(relative_dir);
    if (it != rel_dir_module_map_.end()) {
        return it->second;
    }
    return std::string {};
}

template <class CommandParserType>
void CompilerFlagCache::ParseFileCommand(const CommandParserType &parser,
                                         const fspath &build_path)
{
    fspath abs_file_path = parser.GetFileAbsPath();
    if (project_->IsFileExcluded(abs_file_path)) {
        return;
    }

    // Ignore the files which are generated out of source.
    if (symutil::path_has_prefix(abs_file_path, build_path)) {
        return;
    }

    assert (symutil::path_has_prefix(abs_file_path, project_->home_path()));

    fspath work_dir_path { parser.GetWorkDirectory() };

    auto module_home = filesystem::relative(work_dir_path, build_path);
    const auto &module_name = module_home.string();
    fspath relative_dir = filesystem::relative(
        abs_file_path.parent_path(), project_->home_path());

    LOG_DEBUG << "file=" << abs_file_path << ", module=" << module_name
              << " relative_dir=" << relative_dir;

    rel_dir_module_map_[relative_dir] = module_name;

    if (module_flags_.find(module_name) != module_flags_.end()) {
        return;
    }

    rel_dir_module_map_[module_home] = module_name;

    std::list<std::string> flags = parser.GetFlags();

    PruneCompilerFlags(flags, abs_file_path.string());

    const auto &default_sys_dirs = ConfigInst.default_inc_dirs();
    StringVecPtr final_flags = std::make_shared<StringVec>();
    final_flags->reserve(flags.size() + default_sys_dirs.size());

    std::copy(flags.begin(), flags.end(), std::back_inserter(*final_flags));

    std::copy(default_sys_dirs.begin(), default_sys_dirs.end(),
              std::back_inserter(*final_flags));

    module_flags_[module_name] = final_flags;
}

void CompilerFlagCache::AddDirToModule(const fspath &path,
    const std::string &module_name) {
    assert(symutil::path_has_prefix(path, project_->home_path()));
    assert(filesystem::is_directory(path));
    assert(rel_dir_module_map_.find(path) == rel_dir_module_map_.end());

    auto relative_dir = filesystem::relative(path, project_->home_path());
    rel_dir_module_map_[relative_dir] = module_name;
}

bool CompilerFlagCache::TryRemoveDir(const fspath &path) {
    assert(symutil::path_has_prefix(path, project_->home_path()));
    assert(filesystem::is_directory(path));
    auto relative_dir = filesystem::relative(path, project_->home_path());
    auto it = rel_dir_module_map_.find(relative_dir);
    if (it == rel_dir_module_map_.end()) {
        LOG_WARN << "path module not found, project=" << project_->name()
                  << " path=" << path;
        return false;
    }

    rel_dir_module_map_.erase(it);

    auto module_name = it->second;

    LOG_STATUS << "project=" << project_->name() << " module=" << module_name
               << " remove dir " << path;

    if (relative_dir.string() != module_name) {
        return true;
    }

    for (auto lit = rel_dir_module_map_.begin(); lit != rel_dir_module_map_.end(); ) {
        if (lit->second == module_name) {
            auto tmp_it = lit++;
            rel_dir_module_map_.erase(tmp_it);
        } else {
            ++lit;
        }
    }

    module_flags_.erase(module_name);
    return true;
}

} /* symdb  */
