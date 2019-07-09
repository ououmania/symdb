#ifndef MESSAGEDEFINE_H_4PP1KLCY
#define MESSAGEDEFINE_H_4PP1KLCY

#include <string>

namespace symdb {

struct MessageID {
  enum {
    INVALID = 0,
    CREATE_PROJECT_REQ,
    CREATE_PROJECT_RSP,
    UPDATE_PROJECT_REQ,
    UPDATE_PROJECT_RSP,
    DELETE_PROJECT_REQ,
    DELETE_PROJECT_RSP,
    LIST_PROJECT_REQ,
    LIST_PROJECT_RSP,
    COMPILE_FILE_REQ,
    COMPILE_FILE_RSP,
    GET_SYMBOL_DEFINITION_REQ,
    GET_SYMBOL_DEFINITION_RSP,
    GET_SYMBOL_REFERENCES_REQ,
    GET_SYMBOL_REFERENCES_RSP,
    LIST_FILE_SYMBOLS_REQ,
    LIST_FILE_SYMBOLS_RSP,
    LIST_PROJECT_FILES_REQ,
    LIST_PROJECT_FILES_RSP,
    MAX_MESSAGE_ID,
  };
};

#pragma pack(push, 1)
struct FixedHeader {
  uint16_t msg_size;
  uint16_t pb_head_size;
};
#pragma pack(pop)

static const std::string kDefaultSockPath = "/tmp/symdb.sock";
static constexpr uint32_t kMaxNetErrorSize = 1024;

static const std::string kErrorProjHomeNotExist = "project home not exists";
static const std::string kErrorInvalidProjName =
    "invalid project name: only lower letters and underscore alllowed";
static const std::string kErrorProjectNotFound = "project not found";
static const std::string kErrorFileNotFound = "symbol not found";
static const std::string kErrorSymbolNotFound = "symbol not found";

}  // namespace symdb

#define CHECK_PARSE_MESSAGE(_MsgType, _buf, _len) \
  _MsgType msg;                                   \
  do {                                            \
    if (!msg.ParseFromArray(_buf, _len)) {        \
      LOG_ERROR << "Parse message failed";        \
    }                                             \
  } while (false)

#endif /* end of include guard: MESSAGEDEFINE_H_4PP1KLCY */
