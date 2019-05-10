#include "Session.h"
#include "SymShell.h"
#include "util/Logger.h"
#include "util/NetDefine.h"
#include <iostream>
#include <cstdlib>
#include <boost/program_options.hpp>

using std::string;

boost::program_options::variables_map ParseOption(int argc, char *argv[])
{
    using namespace boost::program_options;
    options_description desc{"program options"};
    desc.add_options()
      ("help,h", "Help screen")
      ("address,a", value<std::string>()->default_value(symdb::kDefaultSockPath),
                 "the address of the server")
      ("log", value<std::string>()->default_value("symcli.log"), "log file")
      ("project,p", value<string>(), "project path")
      ("symbol,s", value<string>(), "symbol name")
      ("reference,r", value<string>(), "reference name");

    variables_map vm;
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        ::exit(EXIT_SUCCESS);
    }

    return vm;
}

int main(int argc, char *argv[])
{
    auto var_map = ParseOption(argc, argv);
    auto log_file = var_map["log"].as<string>();

    LoggerInst.Init(symdb::LogLevel::DEBUG, log_file);

    auto path = var_map["address"].as<string>();
    LOG_DEBUG << "server: " << path;

    boost::asio::io_service io_context;
    symdb::SymShell::Instance().Init(io_context, "/home/richy/.symdb/.symcli_history");
    io_context.run();

    return 0;
}
