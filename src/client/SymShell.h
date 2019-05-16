# pragma once

#include <memory>
#include <utility>
#include <boost/asio.hpp>

namespace symdb {

using StringVec = std::vector<std::string>;

class Command
{
    using CmdMap = std::map<std::string, Command>;
    using ArgVector = std::vector<std::string>;
    using CliHandler = std::function<void(ArgVector&)>;

public:
    Command& SetHandler(CliHandler handler) {
        handler_ = handler;
        return *this;
    }

    Command& operator[](const std::string& name);

    void Process(const std::string& line);

    CmdMap& children() { return children_; }
    const std::string& name() { return name_; }

private:
    static std::string GetCmdKey(const std::string &name);

private:
    CmdMap children_;
    std::string name_;
    CliHandler handler_;
};

class SymShell
{
public:
    void Init(boost::asio::io_service &io_context, const std::string &history_file);

    void WaitInput();

    void ProcessCommad(const char *cmd);

    static SymShell& Instance() {
        static SymShell symshell;
        return symshell;
    }

private:
    void HandleNewInput(boost::system::error_code ec);
    void HandleNewSignal(boost::system::error_code ec);

    static void ReadLineHandler(char *line);
    static char** ReadlineCompletion(const char*, int, int);
    static char* ReadlineCompletionGenerator(const char*, int);

private:
    void SetupSignal(boost::asio::io_service &io_context);
    void SetupReadline(boost::asio::io_service &io_context);

private:
    SymShell() = default;
    SymShell(const SymShell &) = delete;

    using AsioStream = boost::asio::posix::stream_descriptor;

    Command root_cmd_;
    std::unique_ptr<AsioStream> rl_stream_;
    std::unique_ptr<AsioStream> signal_stream_;
    std::string history_file_;
    StringVec completion_cmds_;
    boost::asio::io_service *io_service_;
    bool is_running_;
};

} // symdb

