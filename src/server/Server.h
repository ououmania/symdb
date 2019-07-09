#pragma once

#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <map>
#include <memory>
#include <string>
#include "Listener.h"
#include "TypeAlias.h"
#include "util/Singleton.h"

struct inotify_event;

namespace symdb {

class Project;
class Listener;

using ProjectPtr = std::shared_ptr<Project>;
namespace asio = boost::asio;

class Server : public symutil::Singleton<Server> {
  friend class Singleton<Server>;

public:
  ~Server();

  void Run(const std::string &listen_path);

  ProjectPtr GetProject(const std::string &proj_name);

  ProjectPtr CreateProject(const std::string &proj_name,
                           const std::string &home_dir);

  asio::io_service &main_io_service() { return main_io_service_; }

  int inotify_fd() const { return inotify_stream_->native_handle(); }

  template <class F>
  void PostToWorker(F f) {
    worker_io_service_.post(f);
  }

  template <class F>
  void PostToMain(F f) {
    main_io_service_.post(f);
  }

  bool IsInMainThread() const {
    return boost::this_thread::get_id() == main_thread_id_;
  }

  static bool IsServerRunning(const std::string &listen_path);

private:
  void AddProject(const std::string &proj_name, ProjectPtr ptr);

  void HandleInotifyReadable();
  void HandleInotifyEvent(const inotify_event *event);

  ProjectPtr GetProjectByWatcher(int watch_fd);

  void LoadConfiguredProject();

private:
  Server() = default;
  Server(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(const Server &) = delete;

  using ProjectMap = std::map<std::string, ProjectPtr>;
  using AsioWorkPtr = std::unique_ptr<asio::io_service::work>;

  asio::io_service main_io_service_;
  asio::io_service worker_io_service_;
  boost::thread_group worker_threads_;
  boost::thread::id main_thread_id_;
  AsioWorkPtr idle_work_;
  std::unique_ptr<Listener> listener_;
  ProjectMap projects_;
  AsioStreamPtr inotify_stream_;
};

}  // namespace symdb

#define ServerInst symdb::Server::Instance()
