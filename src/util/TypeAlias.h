#pragma once

#include <boost/asio.hpp>

#if USE_BOOST_FILESYSTEM
#include <boost/filesystem.hpp>
#else  // USE_BOOST_FILESYSTEM
#if __cplusplus < 201703L
#error "std::filesystem requires c++17 or later"
#endif  // __cplusplus < 201703L
#include <filesystem>
#endif  // USE_BOOST_FILESYSTEM

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

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
using fspath = filesystem::path;
#else  // USE_BOOST_FILESYSTEM
namespace filesystem = std::filesystem;
using fspath = filesystem::path;
inline std::ostream &operator<<(std::ostream &osm,
                                const filesystem::path &path) {
  return osm << path.string();
}
#endif  // USE_BOOST_FILESYSTEM

using FsPathVec = std::vector<fspath>;
using FsPathSet = std::set<fspath>;

namespace symutil {
#if USE_BOOST_FILESYSTEM
inline fspath absolute_path(const fspath &p, const fspath &base) {
  if (p.is_absolute()) {
    return p;
  }
  return filesystem::absolute(p, base);
}

inline time_t last_wtime(const filesystem::path &path) {
  return filesystem::last_write_time(path);
}
#else  // USE_BOOST_FILESYSTEM
inline fspath absolute_path(const fspath &p, const fspath &base) {
  if (p.is_absolute()) {
    return p;
  }
  return filesystem::canonical(base / p);
}

inline time_t last_wtime(const filesystem::path &path) {
  const auto ftime = filesystem::last_write_time(path);
#if __cplusplus > 201703L
  const auto system_time = std::chrono::file_clock::to_sys(ftime);
  return std::chrono::system_clock::to_time_t(system_time);
#else
  return std::chrono::duration_cast<std::chrono::seconds>(
             ftime.time_since_epoch())
      .count();
#endif
}

#endif  // USE_BOOST_FILESYSTEM

}  // namespace symutil
