#include "Session.h"
#include <google/protobuf/message.h>
#include <algorithm>
#include <boost/bind.hpp>
#include "Project.h"
#include "Server.h"
#include "proto/Message.pb.h"
#include "util/Logger.h"
#include "util/NetDefine.h"
#include "util/TypeAlias.h"

namespace symdb {

template <class ResponseType>
class ResponseGuard {
public:
  ResponseGuard(Session *session, int msg_id)
      : session_(session), msg_id_(msg_id) {}

  ~ResponseGuard() {
    MessageHead head;
    head.set_msg_id(msg_id_);
    head.set_body_size(resp_.ByteSizeLong());
    session_->do_write(head, resp_);
  }

  ResponseType *operator->() { return &resp_; }

private:
  Session *session_;
  ResponseType resp_;
  int msg_id_;
};

inline bool IsValidProjectName(const std::string &proj_name) {
  if (proj_name.empty()) {
    return false;
  }

  return std::all_of(proj_name.begin(), proj_name.end(),
                     [](char c) { return ::isalnum(c) || c == '_'; });
}

void Session::start() { read_header(); }

void Session::read_header() {
  boost::asio::async_read(
      socket_, boost::asio::buffer((char *)&req_header_, sizeof(req_header_)),
      boost::bind(&Session::handle_read_header, shared_from_this(),
                  boost::asio::placeholders::error));
}

void Session::handle_read_header(const boost::system::error_code &err) {
  if (err) {
    return;
  }

  req_body_.resize(req_header_.msg_size);
  boost::asio::async_read(
      socket_, boost::asio::buffer(req_body_),
      boost::asio::transfer_exactly(req_body_.size()),
      boost::bind(&Session::handle_read_body, shared_from_this(),
                  boost::asio::placeholders::error));
}

void Session::handle_read_body(const boost::system::error_code &err) {
  if (err) {
    LOG_ERROR << "read body error: " << err;
    return;
  }

  handle_message(&*req_body_.begin(), req_body_.size());

  read_header();
}

void Session::do_write(const google::protobuf::Message &head_pb,
                       const google::protobuf::Message &msg_pb) {
  FixedHeader fh;
  fh.pb_head_size = head_pb.ByteSizeLong();
  fh.msg_size = fh.pb_head_size + msg_pb.ByteSizeLong();

  std::ostream os(&reply_);
  os.write(reinterpret_cast<char *>(&fh), sizeof(fh));
  head_pb.SerializeToOstream(&os);
  msg_pb.SerializeToOstream(&os);

  uint32_t total_size = sizeof(fh) + fh.msg_size;
  auto self(shared_from_this());
  boost::asio::async_write(socket_, reply_,
                           boost::asio::transfer_at_least(total_size),
                           [total_size, self](boost::system::error_code ec,
                                              std::size_t /* length */) {
                             if (!ec) {
                               self->read_header();
                               self->reply_.consume(total_size);
                             }
                           });
}

void Session::handle_message(const uint8_t *buffer, size_t length) {
  MessageHead head;
  if (!head.ParseFromArray(buffer, req_header_.pb_head_size)) {
    LOG_ERROR << "parse failed, length: " << length;
    return;
  }

  auto *body_buffer = buffer + req_header_.pb_head_size;
  int body_length = head.body_size();

  switch (head.msg_id()) {
    case MessageID::CREATE_PROJECT_REQ:
      create_project(body_buffer, body_length);
      break;

    case MessageID::UPDATE_PROJECT_REQ:
      update_project(body_buffer, body_length);
      break;

    case MessageID::DELETE_PROJECT_REQ:
      delete_project(body_buffer, body_length);
      break;

    case MessageID::LIST_PROJECT_REQ:
      list_project(body_buffer, body_length);
      break;

    case MessageID::LIST_PROJECT_FILES_REQ:
      list_project_files(body_buffer, body_length);
      break;

    case MessageID::GET_SYMBOL_DEFINITION_REQ:
      get_symbol_definition(body_buffer, body_length);
      break;

    case MessageID::GET_SYMBOL_REFERENCES_REQ:
      get_symbol_references(body_buffer, body_length);
      break;

    case MessageID::LIST_FILE_SYMBOLS_REQ:
      list_file_symbols(body_buffer, body_length);
      break;

    case MessageID::LIST_FILE_REFERENCES_REQ:
      list_file_references(body_buffer, body_length);
      break;

    case MessageID::REBUILD_FILE_REQ:
      rebuild_file(body_buffer, body_length);
      break;

    default:
      LOG_ERROR << "unknown message " << head.msg_id();
      break;
  }
}

void Session::create_project(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(CreateProjectReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name() << ", home_dir=" << msg.home_dir();

  const auto &proj_name = msg.proj_name();
  ResponseGuard<CreateProjectRsp> rsp(this, MessageID::CREATE_PROJECT_RSP);
  if (!IsValidProjectName(proj_name)) {
    rsp->set_error(kErrorInvalidProjName);
    LOG_ERROR << kErrorInvalidProjName << ", project=" << proj_name;
    return;
  }

  fspath proj_home(msg.home_dir());
  if (!filesystem::exists(proj_home)) {
    rsp->set_error(kErrorProjHomeNotExist);
    LOG_ERROR << kErrorProjHomeNotExist << ", project=" << proj_name
              << " home_dir=" << msg.home_dir();
    return;
  }

  try {
    ServerInst.CreateProject(proj_name, msg.home_dir());
  } catch (const std::exception &e) {
    LOG_ERROR << "exception " << e.what() << ", project=" << proj_name
              << " home_dir=" << msg.home_dir();
    rsp->set_error(e.what());
  }
}

void Session::update_project(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(UpdateProjectReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name();

  ResponseGuard<UpdateProjectRsp> rsp(this, MessageID::UPDATE_PROJECT_RSP);

  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  try {
    project->Build();
  } catch (const std::exception &e) {
    LOG_ERROR << "build error " << e.what() << ", project=" << msg.proj_name();
    rsp->set_error(e.what());
  }
}

void Session::delete_project(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(DeleteProjectReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name();

  ResponseGuard<DeleteProjectRsp> rsp(this, MessageID::DELETE_PROJECT_RSP);
  rsp->set_error("not implemented");
}

void Session::list_project(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(ListProjectReq, buffer, length);

  ResponseGuard<ListProjectRsp> rsp(this, MessageID::LIST_PROJECT_RSP);
  rsp->set_error("not implemented");
}

void Session::list_project_files(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(ListProjectFilesReq, buffer, length);

  ResponseGuard<ListProjectFilesRsp> rsp(this,
                                         MessageID::LIST_PROJECT_FILES_RSP);
  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  rsp->set_home_path(project->home_path().string());
  const auto &abs_src_paths = project->abs_src_paths();

  auto *files = rsp->mutable_files();
  files->Reserve(abs_src_paths.size());
  for (const auto &path : abs_src_paths) {
    fspath rel_path = filesystem::relative(path, project->home_path());
    auto *file = rsp->add_files();
    *file = rel_path.string();
  }
}

void Session::get_symbol_definition(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(GetSymbolDefinitionReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name() << ", symbol=" << msg.symbol();

  ResponseGuard<GetSymbolDefinitionRsp> rsp(
      this, MessageID::GET_SYMBOL_DEFINITION_RSP);
  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  if (!msg.abs_path().empty()) {
    Location location =
        project->QuerySymbolDefinition(msg.symbol(), msg.abs_path());
    if (location.IsValid()) {
      location.Serialize(*rsp->add_locations());
      LOG_DEBUG << "project=" << msg.proj_name() << ", symbol=" << msg.symbol()
                << ", abs_path=" << msg.abs_path()
                << ", path=" << location.filename();
    } else {
      LOG_ERROR << kErrorSymbolNotFound << ", project=" << msg.proj_name()
                << " symbol=" << msg.symbol();
      rsp->set_error(kErrorSymbolNotFound);
    }
  } else {
    auto locations = project->QuerySymbolDefinition(msg.symbol());
    rsp->mutable_locations()->Reserve(locations.size());
    for (const auto &loc : locations) {
      loc.Serialize(*rsp->add_locations());
    }
  }
}

void Session::get_symbol_references(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(GetSymbolReferencesReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name() << ", symbol=" << msg.symbol();

  ResponseGuard<GetSymbolReferencesRsp> rsp(
      this, MessageID::GET_SYMBOL_REFERENCES_RSP);

  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  SymbolReferenceLocationMap sym_locs;
  project->LoadSymbolReferenceInfo(msg.symbol(), sym_locs);

  auto pack_locations = [&](const PathLocPairSetMap &path_locs) {
    for (const auto &kvp : path_locs) {
      for (const auto &loc : kvp.second) {
        try {
          fspath abs_path = project->home_path() / kvp.first;
          if (filesystem::exists(abs_path)) {
            auto *item = rsp->add_locations();
            item->set_path(abs_path.string());
            item->set_line(loc.first);
            item->set_column(loc.second);
          }
        } catch (const std::exception &e) {
          LOG_ERROR << "exception=" << e.what()
                    << ", project=" << project->name()
                    << ", home=" << project->home_path()
                    << ", path=" << kvp.first;
        }
      }
    }
  };

  if (!msg.path().empty()) {
    auto module_name = project->GetModuleName(msg.path());
    auto it = sym_locs.find(module_name);
    if (it != sym_locs.end()) {
      pack_locations(it->second);
      return;
    }
  }

  for (const auto &kvp : sym_locs) {
    pack_locations(kvp.second);
  }
}

void Session::list_file_symbols(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(ListFileSymbolsReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name()
            << ", rel_path=" << msg.relative_path();

  ResponseGuard<ListFileSymbolsRsp> rsp(this, MessageID::LIST_FILE_SYMBOLS_RSP);
  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  SymbolDefinitionMap symbols;
  if (!project->LoadFileDefinedSymbolInfo(msg.relative_path(), symbols)) {
    LOG_ERROR << kErrorFileNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorFileNotFound);
  } else if (!symbols.empty()) {
    auto *pb_symbols = rsp->mutable_symbols();
    pb_symbols->Reserve(symbols.size());
    for (const auto &kv : symbols) {
      auto *pb_symbol = pb_symbols->Add();
      pb_symbol->set_name(kv.first);
      pb_symbol->set_column(kv.second.column_number());
      pb_symbol->set_line(kv.second.line_number());
    }
  }
}

void Session::list_file_references(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(ListFileReferencesReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name()
            << ", rel_path=" << msg.relative_path();

  ResponseGuard<ListFileReferencesRsp> rsp(this,
                                           MessageID::LIST_FILE_REFERENCES_RSP);
  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  FileSymbolReferenceMap symbols;
  if (!project->LoadFileReferredSymbolInfo(msg.relative_path(), symbols)) {
    LOG_ERROR << kErrorFileNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorFileNotFound);
  } else if (!symbols.empty()) {
    auto *pb_symbols = rsp->mutable_symbols();
    pb_symbols->Reserve(symbols.size());
    for (const auto &kv : symbols) {
      auto *pb_symbol = pb_symbols->Add();
      pb_symbol->set_name(kv.first.first);
      for (const auto &lcp : kv.second) {
        pb_symbol->set_line(lcp.first);
        pb_symbol->set_column(lcp.second);
      }
    }
  }
}

void Session::rebuild_file(const uint8_t *buffer, size_t length) {
  CHECK_PARSE_MESSAGE(RebuildFileReq, buffer, length);

  LOG_DEBUG << "project=" << msg.proj_name()
            << ", rel_path=" << msg.relative_path();

  ResponseGuard<RebuildFileRsp> rsp(this, MessageID::REBUILD_FILE_RSP);
  ProjectPtr project = ServerInst.GetProject(msg.proj_name());
  if (!project) {
    LOG_ERROR << kErrorProjectNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorProjectNotFound);
    return;
  }

  fspath abs_path =
      symutil::absolute_path(msg.relative_path(), project->home_path());
  if (!filesystem::exists(abs_path)) {
    LOG_ERROR << kErrorFileNotFound << ", project=" << msg.proj_name();
    rsp->set_error(kErrorFileNotFound);
    return;
  }

  project->RebuildFile(abs_path);
}

}  // namespace symdb
