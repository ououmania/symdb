CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

PROJECT(proto)

set(TARGET proto)

FILE(GLOB PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.proto)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

set(PROTO_SRC_DIR ${CMAKE_CURRENT_BINARY_DIR} PARENT_SCOPE)

add_library(proto STATIC ${PROTO_SRCS})

