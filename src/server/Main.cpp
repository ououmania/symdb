#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include "Config.h"
#include "Server.h"
#include "util/Functions.h"
#include "util/Logger.h"

int main(int argc, char *argv[]) {
  int daemon_flag = 0;
  std::string config_file = "Symdb.xml";

  static struct option long_options[] = {
      {"daemon", no_argument, &daemon_flag, 0},
      {"config", required_argument, 0, 'c'},
      {"help", required_argument, 0, 'h'},
      {0, 0, 0, 0}};

  while (1) {
    int option_index = 0;
    int c = getopt_long(argc, argv, "dhc:", long_options, &option_index);
    if (c == -1) {
      break;
    }

    switch (c) {
      case 'd':
        daemon_flag = 1;
        break;

      case 'c':
        config_file = optarg;
        break;

      case 'h':
        std::cout << "symdb - start the symbol database server" << std::endl;
        std::cout << "\t-c --config specify the config file" << std::endl;
        std::cout << "\t-d --daemon start as daemon" << std::endl;
        std::cout << "\t-h --help   print this help message" << std::endl;
        exit(EXIT_SUCCESS);
        break;

      case '?':
        break;

      default:
        std::cerr << "unknown option " << c << std::endl;
        ::exit(EXIT_FAILURE);
        break;
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

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGALRM);
  sigaddset(&mask, SIGHUP);
  sigaddset(&mask, SIGPOLL);
  sigaddset(&mask, SIGUSR1);
  sigaddset(&mask, SIGUSR2);
  sigaddset(&mask, SIGPIPE);
  if (daemon_flag) {
    sigaddset(&mask, SIGINT);
  }
  sigprocmask(SIG_BLOCK, &mask, NULL);

  LOG_DEBUG << "server boots up";
  if (!daemon_flag) {
    ServerInst.Run(ConfigInst.listen_path());
  } else {
    try {
      ServerInst.Run(ConfigInst.listen_path());
    } catch (const std::exception &e) {
      LOG_ERROR << "exception: " << e.what() << "\n"
                << symutil::get_backtrace();
    } catch (...) {
      LOG_ERROR << "unknown exception";
    }
  }

  return 0;
}
