# pragma once

#include <vector>
#include <string>
#include <memory>
#include <type_traits>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

namespace symdb {

namespace filesystem = boost::filesystem;

using AsioStream = boost::asio::posix::stream_descriptor;
using AsioStreamPtr = std::unique_ptr<boost::asio::posix::stream_descriptor>;
using StringVec = std::vector<std::string>;
using StringVecPtr = std::shared_ptr<StringVec>;
using fspath = boost::filesystem::path;

template <class T>
using RawPointerWrap = std::shared_ptr<typename std::remove_pointer<T>::type >;


} /* symdb */
