#include <getopt.h>
#include <unistd.h>
#include <cstdlib>
#include "Config.h"
#include "Server.h"
#include "util/Logger.h"

int main(int argc, char *argv[]) {
  int daemon_flag = 0;
  std::string config_file = "Symdb.xml";

  static struct option long_options[] = {
      {"daemon", no_argument, &daemon_flag, 1},
      {"config", required_argument, 0, 'c'},
      {0, 0, 0, 0}
  };

  while (1) {
    int option_index = 0;
    int c = getopt_long(argc, argv, "dc:", long_options, &option_index);
    if (c == -1) {
      break;
    }

    switch (c) {
      case 0:
        break;

      case 'c':
        config_file = optarg;
        break;

      case '?':
        break;

      default:
        abort();
    }
  }

  ConfigInst.Init(config_file);
  if (symdb::Server::IsServerRunning(ConfigInst.listen_path())) {
    LOG_ERROR << "Server already running, listen=" << ConfigInst.listen_path();
    ::exit(EXIT_FAILURE);
  }

  if (daemon_flag) {
    daemon(0, 0);
  }

  ::unlink(ConfigInst.listen_path().c_str());

  try {
    LOG_DEBUG << "program boots up";
    ServerInst.Run(ConfigInst.listen_path());
  } catch (const std::exception &e) {
    LOG_ERROR << "exception: " << e.what();
  } catch (...) {
    LOG_ERROR << "unknown exception";
  }

  return 0;
}
