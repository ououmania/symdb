#ifndef SESSION_H_W5DAS4RL
#define SESSION_H_W5DAS4RL

#include <memory>
#include <boost/asio.hpp>

namespace google {

namespace protobuf {
    class Message;
} // protobuf

} // google

namespace symdb {

static constexpr size_t kMaxRequestSize = 65536;

class Session
{
    using Socket = boost::asio::local::stream_protocol::socket;

public:
    Session(boost::asio::io_service &io_context, const std::string &path);
    void create_project(const std::string &proj_name, const std::string &home_dir);
    void delete_project(const std::string &proj_name);
    void update_project(const std::string &proj_name);
    void list_projects();

    void get_symbol_definition(const std::string &proj_name, const std::string &symbol, const std::string &abs_path);
    void get_symbol_references(const std::string &proj_name, const std::string &symbol);

    void list_file_symbols(const std::string &proj_name, const std::string &symbol);

private:
    bool send(int msg_id, const google::protobuf::Message &body);

    template <class RspType>
    bool send_and_recv(int msg_id, const google::protobuf::Message &req,
        RspType &rsp);

private:
    Socket socket_;
};

} // symdb

#endif /* end of include guard: SESSION_H_W5DAS4RL */
