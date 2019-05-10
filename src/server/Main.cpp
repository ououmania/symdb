#include "Server.h"
#include "Config.h"
#include "Project.h"
#include "util/Logger.h"
#include "util/NetDefine.h"
#include "util/Exceptions.h"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <boost/foreach.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string.hpp>

boost::program_options::variables_map ParseOption(int argc, char *argv[])
{
    using namespace boost::program_options;
    options_description desc{"program options"};
    desc.add_options()
      ("help,h", "Help screen")
      ("listen", value<std::string>()->default_value(symdb::kDefaultSockPath),
                 "The unix domain socket path")
      ("daemon", value<bool>()->default_value(false), "run as daemon")
      ("log", value<std::string>()->default_value("symdb.log"), "Config file");

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

    bool is_daemon = var_map["daemon"].as<bool>();
    if (is_daemon) {
        LOG_DEBUG << "run as daemon";
        daemon(1, 0);
    }

    ConfigInst.Init("Config.xml");

    LOG_DEBUG << "program boots up";

    auto path = var_map["listen"].as<std::string>();
    LOG_DEBUG << "listen: " << path;

    ServerInst.Run(path);

    return 0;
}
