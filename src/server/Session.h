#ifndef SESSION_H_W5DAS4RL
#define SESSION_H_W5DAS4RL

#include <boost/asio.hpp>
#include <memory>
#include "util/NetDefine.h"

namespace google {

namespace protobuf {
class Message;
}  // namespace protobuf

}  // namespace google

namespace symdb {

static constexpr size_t kMaxRequestSize = 8192;

class Session : public std::enable_shared_from_this<Session> {
  using Socket = boost::asio::local::stream_protocol::socket;

public:
  explicit Session(Socket socket) : socket_(std::move(socket)) {}

  void start();

  void do_write(const google::protobuf::Message &head_pb,
                const google::protobuf::Message &msg_pb);

private:
  void read_header();
  void read_body();

  void handle_read_header(const boost::system::error_code &err);
  void handle_read_body(const boost::system::error_code &err);

  void handle_message(const uint8_t *buffer, size_t length);
  void create_project(const uint8_t *buffer, size_t length);
  void delete_project(const uint8_t *buffer, size_t length);
  void update_project(const uint8_t *buffer, size_t length);
  void list_project(const uint8_t *buffer, size_t length);
  void list_project_files(const uint8_t *buffer, size_t length);
  void get_symbol_definition(const uint8_t *buffer, size_t length);
  void get_symbol_references(const uint8_t *buffer, size_t length);
  void list_file_symbols(const uint8_t *buffer, size_t length);

private:
  Socket socket_;
  FixedHeader req_header_;
  std::vector<uint8_t> req_body_;
  boost::asio::streambuf reply_;
};

}  // namespace symdb

#endif /* end of include guard: SESSION_H_W5DAS4RL */
