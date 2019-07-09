#ifndef LISTENER_H_HDGFEQHT
#define LISTENER_H_HDGFEQHT

#include <boost/asio.hpp>

namespace symdb {

class Listener {
public:
  Listener(boost::asio::io_context& io_context, const std::string& file);

private:
  void do_accept();

private:
  boost::asio::local::stream_protocol::acceptor acceptor_;
  boost::asio::local::stream_protocol::socket socket_;
};

}  // namespace symdb

#endif /* end of include guard: LISTENER_H_HDGFEQHT */
