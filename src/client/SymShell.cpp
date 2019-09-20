#include "SymShell.h"
#include <readline/history.h>
#include <readline/readline.h>
#include <sys/signalfd.h>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <iostream>
#include "Session.h"
#include "util/Exceptions.h"
#include "util/Logger.h"
#include "util/NetDefine.h"

namespace symdb {

template <int... Is>
struct seq {};

template <int N, int... Is>
struct gen_seq : gen_seq<N - 1, N - 1, Is...> {};

template <int... Is>
struct gen_seq<0, Is...> : seq<Is...> {};

template <size_t Argc>
struct SessionMemberFunc;

template <>
struct SessionMemberFunc<0> {
  using type = void (Session::*)();
};

template <>
struct SessionMemberFunc<1> {
  using type = void (Session::*)(const std::string &);
};

template <>
struct SessionMemberFunc<2> {
  using type = void (Session::*)(const std::string &, const std::string &);
};

template <>
struct SessionMemberFunc<3> {
  using type = void (Session::*)(const std::string &, const std::string &,
                                 const std::string &);
};

template <>
struct SessionMemberFunc<4> {
  using type = void (Session::*)(const std::string &, const std::string &,
                                 const std::string &, const std::string &);
};

template <size_t MinArgc, size_t MaxArgc = MinArgc>
class CommandDelegator {
  using FuncType = typename SessionMemberFunc<MaxArgc>::type;

public:
  CommandDelegator(const std::string &usage, FuncType func)
      : usage_{usage}, func_{func} {}

  template <int... Is>
  void helper(StringVec &args, seq<Is...>) {
    boost::asio::io_service io_service;
    symdb::Session session(io_service, kDefaultSockPath);
    (session.*func_)(args[Is]...);
  }

  void operator()(StringVec &args) {
    if (args.size() < MinArgc || args.size() > MaxArgc) {
      std::cerr << "usage: " << usage_ << std::endl;
      return;
    }

    args.resize(MaxArgc);
    helper(args, gen_seq<MaxArgc>{});
  }

  const std::string& usage() const { return usage_; }

private:
  std::string usage_;
  FuncType func_;
};

std::string Command::GetCmdKey(const std::string &name) {
  std::string key;
  std::transform(name.begin(), name.end(), std::back_inserter(key), ::tolower);
  return key;
}

Command &Command::operator[](const std::string &name) {
  std::string key = Command::GetCmdKey(name);

  if (children_.find(key) != children_.end()) {
    return children_[key];
  } else {
    Command &child = children_[key];
    child.name_ = key;
    child.handler_ = NULL;
    return child;
  }
}

void Command::Process(const std::string &command) {
  StringVec args;
  Command *sub_cmd = this;

  std::stringstream ss;
  ss.str(command);

  for (std::string arg; ss >> arg;) {
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
                    const std::string &history_file) {
  auto &project_cmd = root_cmd_["project"];
  project_cmd["create"].SetHandler(CommandDelegator<2>{
      "project create <name> <home>", &Session::create_project});

  project_cmd["update"].SetHandler(
      CommandDelegator<1>{"project update <name>", &Session::update_project});

  project_cmd["delete"].SetHandler(
      CommandDelegator<1>{"project delete <name>", &Session::delete_project});

  project_cmd["list"].SetHandler(
      CommandDelegator<0>{"project list project", &Session::list_projects});

  project_cmd["files"].SetHandler(CommandDelegator<1>{
      "project files <proj_name>", &Session::list_project_files});

  auto &sym_cmd = root_cmd_["symbol"];
  sym_cmd["definition"].SetHandler(
      CommandDelegator<2, 3>{"symbol definition <proj_name> <symbol> [path]",
                             &Session::get_symbol_definition});

  sym_cmd["reference"].SetHandler(
      CommandDelegator<2, 3>{"symbol reference <proj_name> <symbol> [path]",
                             &Session::get_symbol_references});

  root_cmd_["file"]["symbols"].SetHandler(CommandDelegator<2>{
      "file symbols <proj_name> <path>", &Session::list_file_symbols});

  root_cmd_["file"]["refer"].SetHandler(CommandDelegator<2>{
      "file refer <proj_name> <path>", &Session::list_file_references});

  history_file_ = history_file;

  is_running_ = true;

  io_service_ = &io_context;
  SetupSignal(io_context);
  SetupReadline(io_context);
}

void SymShell::SetupSignal(boost::asio::io_service &io_context) {
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
                             [this](const boost::system::error_code &ec) {
                               this->HandleNewSignal(ec);
                             });
}

void SymShell::SetupReadline(boost::asio::io_service &io_context) {
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

void SymShell::WaitInput() {
  if (is_running_) {
    rl_stream_->async_wait(AsioStream::wait_read,
                           [this](const boost::system::error_code &ec) {
                             this->HandleNewInput(ec);
                           });
  }
}

void SymShell::ReadLineHandler(char *line) {
  if (line == nullptr || strcmp(line, "exit") == 0 ||
      strcmp(line, "quit") == 0) {
    if (line == nullptr) {
      printf("\n");
    }
    printf("bye...\n");

    /* This function needs to be called to reset the terminal settings,
     and calling it from the line handler keeps one extra prompt from
     being displayed. */
    rl_callback_handler_remove();

    SymShell::Instance().is_running_ = false;
    SymShell::Instance().signal_stream_->close();
  } else {
    if (*line) {
      add_history(line);
      SymShell::Instance().ProcessCommad(line);
    }
    free(line);
  }
}

void SymShell::ProcessCommad(const char *cmd) {
  LOG_DEBUG << "cmd: " << cmd;
  try {
    root_cmd_.Process(cmd);
    write_history(history_file_.c_str());
  } catch (const std::exception &e) {
    return;
  }
}

void SymShell::HandleNewInput(boost::system::error_code ec) {
  if (!ec) {
    rl_callback_read_char();
    WaitInput();
  } else {
    LOG_ERROR << "readline error: " << ec;
    rl_callback_handler_remove();
  }
}

void SymShell::HandleNewSignal(boost::system::error_code ec) {
  if (ec) {
    return;
  }

  signalfd_siginfo fdsi;
  boost::asio::read(*signal_stream_,
                    boost::asio::buffer((char *)&fdsi, sizeof(fdsi)),
                    boost::asio::transfer_exactly(sizeof(fdsi)));

  printf("\n");  // Move to a new line
  rl_on_new_line();
  rl_replace_line("", 0);
  rl_redisplay();

  signal_stream_->async_wait(AsioStream::wait_read,
                             [this](const boost::system::error_code &ec) {
                               this->HandleNewSignal(ec);
                             });
}

char **SymShell::ReadlineCompletion(const char *text, int start, int end) {
  (void)(start);
  (void)(end);

  SymShell &sym_shell = SymShell::Instance();
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

  auto it = (cur.empty() ? parent->children().begin()
                         : parent->children().lower_bound(cur));
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

char *SymShell::ReadlineCompletionGenerator(const char *text, int state) {
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

}  // namespace symdb
