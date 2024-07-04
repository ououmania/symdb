#include <cstdlib>
#include <iostream>
#include "Session.h"
#include "SymShell.h"
#include "util/Functions.h"
#include "util/Logger.h"
#include "util/NetDefine.h"

using std::string;

int main(int argc, char *argv[]) {
  std::string symdb_dir = symutil::expand_env("${HOME}/.symdb");
  symdb::InitLogger(symdb::LogLevel::DEBUG, symdb_dir + "/log/symcli.log");

  LOG_INFO << "starting";

  boost::asio::io_service io_context;
  symdb::SymShell::Instance().Init(io_context,
                                   symdb_dir + "/.symcli_history");
  io_context.run();

  LOG_INFO << "stopping";

  return 0;
}
