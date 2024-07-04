#pragma once

#include <boost/asio.hpp>

#if USE_BOOST_FILESYSTEM
#include <boost/filesystem.hpp>
#else // USE_BOOST_FILESYSTEM
#if __cplusplus < 201703L
#error "std::filesystem requires c++17 or later"
#endif // __cplusplus < 201703L
#include <filesystem>
#endif // USE_BOOST_FILESYSTEM

#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace symdb {

using AsioStream = boost::asio::posix::stream_descriptor;
using AsioStreamPtr = std::unique_ptr<boost::asio::posix::stream_descriptor>;
using StringVec = std::vector<std::string>;
using StringVecPtr = std::shared_ptr<StringVec>;

template <class T>
using RawPointerWrap = std::shared_ptr<typename std::remove_pointer<T>::type>;

template <class T, class Deleter>
using UniqueRawPointerWrap =
    std::unique_ptr<typename std::remove_pointer<T>::type, Deleter>;

#if USE_BOOST_FILESYSTEM
namespace filesystem = boost::filesystem;
inline time_t last_wtime(const filesystem::path &path) {
  return filesystem::last_write_time(path);
}

#else // USE_BOOST_FILESYSTEM
namespace filesystem = std::filesystem;
inline time_t last_wtime(const filesystem::path &path) {
  using std::chrono;
  const auto ftime = filesystem::last_write_time(path);
  const auto system_time = clock_cast<system_clock>(ftime);
  return system_clock::to_time_t(system_time);
}

inline std::ostream &operator<<(std::ostream &osm, const filesystem::path &path) {
  return osm << path.string();
}

#endif // USE_BOOST_FILESYSTEM

using fspath = filesystem::path;
using FsPathVec = std::vector<fspath>;
using FsPathSet = std::set<fspath>;

}  // namespace symdb
