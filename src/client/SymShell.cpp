#include "SymShell.h"
#include "Session.h"
#include "util/Exceptions.h"
#include "util/NetDefine.h"
#include <iostream>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/signalfd.h>

#ifndef ERROR
    #define ERROR(fmt) std::cerr << fmt
    #define ERROR_LN(fmt) ERROR(fmt) << std::endl
#endif

namespace symdb {

std::string Command::GetCmdKey(const std::string &name)
{
    std::string key;
    std::transform(name.begin(), name.end(),
        std::back_inserter(key), ::tolower);
    return key;
}

Command& Command::operator[](const std::string& name)
{
    std::string key = Command::GetCmdKey(name);

    if (children_.find(key) != children_.end()) {
        return children_[key];
    } else {
        Command& child = children_[key];
        child.name_ = key;
        child.handler_ = NULL;
        return child;
    }
}

void Command::Process(const std::string& command)
{
    ArgVector args;
    Command* sub_cmd = this;

    std::stringstream ss;
    ss.str(command);

    for (std::string arg; ss >> arg; ) {
        std::string key = Command::GetCmdKey(arg);

        auto it = sub_cmd->children_.find(key);
        if (args.empty() && it != sub_cmd->children_.end()) {
            sub_cmd = &it->second;
        } else {
            args.push_back(arg);
        }
    }

    if (sub_cmd && sub_cmd->handler_) {
        sub_cmd->handler_(args);
    } else {
    }
}

void SymShell::Init(boost::asio::io_service &io_context,
    const std::string &history_file)
{
    root_cmd_["project"]["create"].SetHandler(std::bind(&SymShell::CreateProject, this, std::placeholders::_1));
    root_cmd_["project"]["update"].SetHandler(std::bind(&SymShell::UpdateProject, this, std::placeholders::_1));
    root_cmd_["project"]["delete"].SetHandler(std::bind(&SymShell::DeleteProject, this, std::placeholders::_1));
    root_cmd_["project"]["list"].SetHandler(std::bind(&SymShell::ListProjects, this, std::placeholders::_1));
    root_cmd_["project"]["files"].SetHandler(std::bind(&SymShell::ListProjectFiles, this, std::placeholders::_1));
    root_cmd_["symbol"]["definition"].SetHandler(std::bind(&SymShell::GetSymbolDefinition, this, std::placeholders::_1));
    root_cmd_["symbol"]["reference"].SetHandler(std::bind(&SymShell::GetSymbolReference, this, std::placeholders::_1));
    root_cmd_["file"]["symbols"].SetHandler(std::bind(&SymShell::GetFileSymbols, this, std::placeholders::_1));

    history_file_ = history_file;

    is_running_ = true;

    io_service_ = &io_context;
    SetupSignal(io_context);
    SetupReadline(io_context);
}

void SymShell::SetupSignal(boost::asio::io_service &io_context)
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGQUIT);
    sigaddset(&mask, SIGTTIN);
    sigaddset(&mask, SIGTTOU);

    int error = sigprocmask(SIG_BLOCK, &mask, NULL);
    if (error < 0) {
        THROW_AT_FILE_LINE("sigprocmask error: %s", strerror(errno));
    }

    int fd = signalfd(-1, &mask, 0);
    if (fd < 0) {
        THROW_AT_FILE_LINE("signalfd error: %s", strerror(errno));
    }

    signal_stream_.reset(new AsioStream(io_context, fd));
    signal_stream_->non_blocking();

    signal_stream_->async_wait(AsioStream::wait_read,
        [this] (const auto &ec) {
            this->HandleNewSignal(ec);
        }
    );
}

void SymShell::SetupReadline(boost::asio::io_service &io_context)
{
    rl_initialize();

    rl_readline_name = "symshell";
    rl_catch_signals = 0;

    using_history();
    read_history(history_file_.c_str());

    rl_attempted_completion_function = SymShell::ReadlineCompletion;
    rl_callback_handler_install("symdb>", SymShell::ReadLineHandler);

    rl_stream_.reset(new AsioStream(io_context, ::fileno(rl_instream)));
    rl_stream_->non_blocking();

    WaitInput();
}

void SymShell::WaitInput()
{
    if (is_running_) {
        rl_stream_->async_wait(AsioStream::wait_read,
            [this] (const auto &ec) {
                this->HandleNewInput(ec);
            }
        );
    }
}

void SymShell::ReadLineHandler(char *line)
{
    if (line == nullptr || strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        if (line == nullptr) {
            printf("\n");
        }
        printf("bye...\n");

        /* This function needs to be called to reset the terminal settings,
         and calling it from the line handler keeps one extra prompt from
         being displayed. */
        rl_callback_handler_remove ();

        SymShell::Instance().is_running_ = false;
        SymShell::Instance().signal_stream_->close();
    } else {
        if (*line) {
            add_history(line);
        }
        SymShell::Instance().ProcessCommad(line);
        free(line);
    }
}

void SymShell::ProcessCommad(const char *cmd) {
    try {
        root_cmd_.Process(cmd);
        write_history(history_file_.c_str());
    } catch(const std::exception &e) {
        return;
    }
}

void SymShell::HandleNewInput(boost::system::error_code ec)
{
    if (!ec) {
        rl_callback_read_char();
        WaitInput();
    } else {
        rl_callback_handler_remove();
    }
}

void SymShell::HandleNewSignal(boost::system::error_code ec)
{
    if (ec) {
        return;
    }

    signalfd_siginfo fdsi;
    boost::asio::read(*signal_stream_,
        boost::asio::buffer((char*) &fdsi, sizeof(fdsi)),
        boost::asio::transfer_exactly(sizeof(fdsi)));

    printf("\n"); // Move to a new line
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();

    signal_stream_->async_wait(AsioStream::wait_read,
        [this] (const auto &ec) {
            this->HandleNewSignal(ec);
        }
    );
}

void SymShell::CreateProject(const StringVec &args)
{
    if (args.size() != 2) {
        ERROR_LN("usage: project create <name> <home>");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.create_project(args[0], args[1]);
}

void SymShell::UpdateProject(const StringVec &args)
{
    if (args.size() != 1) {
        ERROR_LN("usage: project create <name>");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.update_project(args[0]);
}

void SymShell::DeleteProject(const StringVec &args)
{
    if (args.empty()) {
        ERROR_LN("usage: project delete <name...>");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.delete_project(args[0]);
}

void SymShell::ListProjects(const StringVec &args)
{
    (void) args;

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.list_projects();
}

void SymShell::ListProjectFiles(const StringVec &args)
{
    if (args.size() != 1) {
        ERROR_LN("usage: project files <proj_name>");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.list_project_files(args.front());
}

void SymShell::GetSymbolDefinition(const StringVec &args)
{
    if (args.size() != 2 && args.size() != 3) {
        ERROR_LN("usage: symbol definition <proj_name> <symbol> [path]");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    if (args.size() == 3) {
        session.get_symbol_definition(args[0], args[1], args[2]);
    } else {
        session.get_symbol_definition(args[0], args[1], std::string {});
    }
}

void SymShell::GetSymbolReference(const StringVec &args)
{
    if (args.size() != 2) {
        ERROR_LN("usage: symbol reference <proj_name> <symbol>");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.get_symbol_references(args[0], args[1]);
}

void SymShell::GetFileSymbols(const StringVec &args)
{
    if (args.size() != 2) {
        ERROR_LN("usage: file symbols <proj_name> <relative_path>");
        return;
    }

    symdb::Session session(*io_service_, kDefaultSockPath);
    session.list_file_symbols(args[0], args[1]);
}

char** SymShell::ReadlineCompletion(const char* text, int start, int end)
{
    (void)(start);
    (void)(end);

    SymShell& sym_shell = SymShell::Instance();
    Command *parent = &sym_shell.root_cmd_;

    std::stringstream ss(rl_line_buffer);

    std::string cur;
    while (ss >> cur) {
        std::transform(cur.begin(), cur.end(), cur.begin(), ::tolower);
        auto it = parent->children().find(cur);
        if (it == parent->children().end()) {
            break;
        }

        parent = &it->second;
        cur.clear();
    }

    auto it = (cur.empty() ? parent->children().begin() :parent->children().lower_bound(cur));
    for (; it != parent->children().end(); ++it) {
        if (cur == it->first.substr(0, cur.size())) {
            sym_shell.completion_cmds_.push_back(it->second.name());
        } else {
            break;
        }
    }

    if (sym_shell.completion_cmds_.empty()) {
        return rl_completion_matches(text, rl_filename_completion_function);
    }

    return rl_completion_matches(text, SymShell::ReadlineCompletionGenerator);
}

char* SymShell::ReadlineCompletionGenerator(const char* text, int state)
{
    (void)(text);
    (void)(state);

    SymShell &sym_shell = SymShell::Instance();

    if (sym_shell.completion_cmds_.empty()) {
        return nullptr;
    }

    auto it = sym_shell.completion_cmds_.begin();
    char *res = strdup(it->c_str());
    sym_shell.completion_cmds_.erase(it);

    return res;
}

} /* symdb */
