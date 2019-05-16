#include "Server.h"
#include "Config.h"
#include "util/Logger.h"
#include <cstdlib>

int main(int argc, char *argv[])
{
    ConfigInst.Init("Config.xml");

    if (symdb::Server::IsServerRunning(ConfigInst.listen_path())) {
        LOG_ERROR << "Server already running, listen=" << ConfigInst.listen_path();
        ::exit(EXIT_FAILURE);
    }

    ::unlink(ConfigInst.listen_path().c_str());

    LOG_DEBUG << "program boots up";

    ServerInst.Run(ConfigInst.listen_path());

    return 0;
}
