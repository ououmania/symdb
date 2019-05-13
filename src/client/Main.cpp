#include "Session.h"
#include "SymShell.h"
#include "util/Logger.h"
#include "util/NetDefine.h"
#include <iostream>
#include <cstdlib>

using std::string;

int main(int argc, char *argv[])
{
    LoggerInst.Init(symdb::LogLevel::DEBUG, "/home/richy/.symdb/log/symcli.log");

    boost::asio::io_service io_context;
    symdb::SymShell::Instance().Init(io_context, "/home/richy/.symdb/.symcli_history");
    io_context.run();

    return 0;
}
