#include "Session.h"
#include "util/Logger.h"
#include "util/NetDefine.h"
#include "proto/Message.pb.h"
#include <google/protobuf/message.h>
#include <boost/filesystem.hpp>

namespace symdb {

#define CHECK_PARSE_RESPONSE(_RspType, _buf, _len)         \
    CHECK_PARSE_MESSAGE(_RspType, _buf, _len);             \
                                                           \
    if (!msg.error().empty()) {                            \
        LOG_ERROR << "failed to compile: " << msg.error(); \
        return;                                            \
    }

Session::Session(boost::asio::io_service &io_context, const std::string &path)
    : socket_(io_context)
{
    socket_.connect(boost::asio::local::stream_protocol::endpoint(path));

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
            &tv, sizeof(tv)) != 0) {
        throw std::runtime_error(strerror(errno));
    }
}

void Session::create_project(const std::string &proj_name, const std::string &home_dir)
{
    CreateProjectReq req;
    req.set_proj_name(proj_name);
    req.set_home_dir(home_dir);

    CreateProjectRsp rsp;
    send_and_recv(MessageID::CREATE_PROJECT_REQ, req, rsp);
}

void Session::update_project(const std::string &proj_name)
{
    UpdateProjectReq req;
    req.set_proj_name(proj_name);

    UpdateProjectRsp rsp;
    send_and_recv(MessageID::UPDATE_PROJECT_REQ, req, rsp);
}

void Session::delete_project(const std::string &proj_name)
{
    DeleteProjectReq req;
    req.set_proj_name(proj_name);

    DeleteProjectRsp rsp;
    send_and_recv(MessageID::DELETE_PROJECT_REQ, req, rsp);
}

void Session::list_projects()
{
    ListProjectReq req;
    req.set_unused(0);

    ListProjectRsp rsp;
    send_and_recv(MessageID::LIST_PROJECT_REQ, req, rsp);
}

void Session::list_project_files(const std::string &proj_name)
{
    ListProjectFilesReq req;
    req.set_proj_name(proj_name);

    ListProjectFilesRsp rsp;
    send_and_recv(MessageID::LIST_PROJECT_FILES_REQ, req, rsp);
}

void Session::get_symbol_definition(const std::string &proj_name,
    const std::string &symbol, const std::string &abs_path)
{
    GetSymbolDefinitionReq req;
    req.set_proj_name(proj_name);
    req.set_symbol(symbol);
    req.set_abs_path(abs_path);

    GetSymbolDefinitionRsp rsp;
    send_and_recv(MessageID::GET_SYMBOL_DEFINITION_REQ, req, rsp);
}

void Session::get_symbol_references(const std::string &proj_name, const std::string &symbol)
{
    GetSymbolReferencesReq req;
    req.set_proj_name(proj_name);
    req.set_symbol(symbol);

    GetSymbolReferencesRsp rsp;
    send_and_recv(MessageID::GET_SYMBOL_REFERENCES_REQ, req, rsp);
}

void Session::list_file_symbols(const std::string &proj_name, const std::string &rel_path)
{
    ListFileSymbolsReq req;
    req.set_proj_name(proj_name);
    req.set_relative_path(rel_path);

    ListFileSymbolsRsp rsp;
    send_and_recv(MessageID::LIST_FILE_SYMBOLS_REQ, req, rsp);
}

bool Session::send(int msg_id, const google::protobuf::Message &body)
{
    MessageHead head;
    head.set_msg_id(msg_id);
    head.set_body_size(body.ByteSize());

    LOG_DEBUG << "send " << body.GetTypeName() << ": "
              << body.ShortDebugString();

    FixedHeader fh;
    fh.pb_head_size = head.ByteSize();
    fh.msg_size = fh.pb_head_size + body.GetCachedSize();

    boost::asio::streambuf buf;
    std::ostream os(&buf);
    os.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    head.SerializeToOstream(&os);
    body.SerializeToOstream(&os);

    size_t total_size = fh.msg_size + sizeof(fh);

    boost::system::error_code error;
    boost::asio::write(socket_, buf,
        boost::asio::transfer_at_least(total_size), error);
    if (error) {
        LOG_ERROR << "send error ";
    }

    return !bool(error);
}

template <class RspType>
bool Session::send_and_recv(int msg_id, const google::protobuf::Message &body,
    RspType &rsp)
{
    if (!send(msg_id, body)) {
        LOG_ERROR << "Send message " << msg_id << " failed";
        return false;
    }

    FixedHeader fh;

    boost::system::error_code error;
    boost::asio::read(socket_, boost::asio::buffer(&fh, sizeof(fh)),
        boost::asio::transfer_exactly(sizeof(fh)), error);
    if (error) {
        LOG_ERROR << "read fixed header error: " << error;
        return false;
    }

    std::vector<uint8_t> reply_body(fh.msg_size);
    boost::asio::read(socket_, boost::asio::buffer(reply_body),
        boost::asio::transfer_exactly(fh.msg_size), error);
    if (error) {
        LOG_ERROR << "read pb error: " << error;
        return false;
    }

    MessageHead head_pb;
    if (!head_pb.ParseFromArray(&*reply_body.begin(), 0)) {
        LOG_ERROR << "parse pb header error";
        return true;
    }

    if (!rsp.ParseFromArray(&reply_body[fh.pb_head_size],
                             fh.msg_size - fh.pb_head_size)) {
        LOG_ERROR << "parse pb body error";
        return false;
    }

    std::string rsp_str = rsp.ShortDebugString();

    LOG_STATUS << "" << RspType::default_instance().GetTypeName()
               << ": " << (rsp_str.empty() ? "ok" : rsp_str);

    return true;
}

} // symdb
