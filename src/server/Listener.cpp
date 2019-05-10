#include "Listener.h"
#include "Session.h"
#include <memory>

namespace symdb {

Listener::Listener(boost::asio::io_context& io_context, const std::string& file)
  : acceptor_(io_context, boost::asio::local::stream_protocol::endpoint(file)),
    socket_(io_context)
{
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    do_accept();
}

void Listener::do_accept()
{
    acceptor_.async_accept(socket_,
        [this](boost::system::error_code ec)
        {
          if (!ec) {
              auto session = std::make_shared<Session>(std::move(socket_));
              session->start();
          }

          do_accept();
        });
}

} // symdb
