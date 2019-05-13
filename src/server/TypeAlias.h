# pragma once

#include <vector>
#include <string>
#include <memory>
#include <type_traits>
#include <boost/filesystem.hpp>
#include <boost/asio.hpp>

namespace symdb {

// std::experimental::filesystem has no relative()
// std::filesystem has no canonical() with a bas path
// boost::filesystem is the most proper one at present.
namespace filesystem = boost::filesystem;

using AsioStream = boost::asio::posix::stream_descriptor;
using AsioStreamPtr = std::unique_ptr<boost::asio::posix::stream_descriptor>;
using StringVec = std::vector<std::string>;
using StringVecPtr = std::shared_ptr<StringVec>;
using fspath = filesystem::path;

template <class T>
using RawPointerWrap = std::shared_ptr<typename std::remove_pointer<T>::type >;

#if USE_STD_FILESYSTEM
inline time_t last_wtime(const fspath &path) {
    auto ftime = filesystem::last_write_time(path);
    return decltype(ftime)::clock::to_time_t(ftime);;
}

inline std::ostream&
operator<<(std::ostream &osm, const fspath &path) {
    return osm << path.string();
}

#else
inline time_t last_wtime(const fspath &path) {
    return filesystem::last_write_time(path);
}

#endif

} /* symdb */
