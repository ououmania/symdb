#include "Server.h"

#include <errno.h>
#include <sys/inotify.h>
#include <string>

#include "Config.h"
#include "Project.h"
#include "Session.h"
#include "util/Exceptions.h"
#include "util/Functions.h"
#include "util/Logger.h"
#include "util/TypeAlias.h"

namespace symdb {

Server::~Server() {
  projects_.clear();
  idle_work_.reset();
  worker_io_service_.stop();
  main_io_service_.stop();
#if __cplusplus < 202002L
  for (auto &t : worker_threads_) {
    t.join();
  }
#endif  // __cplusplus < 202002L
}

void Server::Run(const std::string &listen_path) {
  main_thread_id_ = std::this_thread::get_id();

  listener_.reset(new Listener{main_io_service_, listen_path});

  // To keep the main io_service alive
  idle_work_.reset(new AsioWorkPtr::element_type{worker_io_service_});

  for (size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
    worker_threads_.emplace_back([this]() { worker_io_service_.run(); });
  }

  int inotify_fd = inotify_init1(IN_NONBLOCK);
  if (inotify_fd < 0) {
    THROW_AT_FILE_LINE("initofy_init1 error: %s", strerror(errno));
  }

  LOG_DEBUG << "inotify_fd=" << inotify_fd;

  inotify_stream_.reset(new AsioStream{main_io_service_, inotify_fd});
  inotify_stream_->non_blocking();

  inotify_stream_->async_wait(AsioStream::wait_read,
                              [this](const boost::system::error_code &ec) {
                                if (!ec) {
                                  this->HandleInotifyReadable();
                                } else {
                                  LOG_ERROR << "inotify wait error: " << ec;
                                }
                              });

  LoadConfiguredProject();

  main_io_service_.run();
}

ProjectPtr Server::GetProject(const std::string &name) {
  auto it = projects_.find(name);
  if (it != projects_.end()) {
    return it->second;
  }

  try {
    ProjectPtr project = Project::CreateFromDatabase(name);
    AddProject(name, project);
    return project;
  } catch (const std::exception &e) {
    LOG_ERROR << "load project failed, name=" << name << ", error=" << e.what();
  }

  return ProjectPtr{};
}

ProjectPtr Server::CreateProject(const std::string &proj_name,
                                 const std::string &home_dir) {
  auto it = projects_.find(proj_name);
  if (it != projects_.end()) {
    filesystem::path home_path{home_dir};
    if (filesystem::equivalent(it->second->home_path(), home_path)) {
      return it->second;
    }
    THROW_AT_FILE_LINE("project<%s> with home<%s> already exists",
                       proj_name.c_str(), it->second->home_path().c_str());
  }

  ProjectPtr project;
  try {
    project = Project::CreateFromConfigFile(proj_name, home_dir);
  } catch (const std::exception &e) {
    LOG_ERROR << "CreateFromConfigFile failed, proj_name=" << proj_name
              << ", error=" << e.what();
  }

  if (project) {
    AddProject(proj_name, project);
  }

  return project;
}

void Server::AddProject(const std::string &proj_name, ProjectPtr ptr) {
  projects_[proj_name] = ptr;
}

ProjectPtr Server::GetProjectByWatcher(int watch_fd) {
  for (auto &kvp : projects_) {
    if (kvp.second->IsWatchFdInList(watch_fd)) {
      return kvp.second;
    }
  }

  return ProjectPtr{};
}

void Server::HandleInotifyReadable() {
  int inotify_fd = inotify_stream_->native_handle();

  unsigned int avail;
  (void)ioctl(inotify_fd, FIONREAD, &avail);

  char buffer[avail];
  read(inotify_fd, buffer, avail);

  for (decltype(avail) offset = 0; offset < avail;) {
    inotify_event *event = (inotify_event *)(buffer + offset);

    offset += sizeof(inotify_event) + event->len;

    if (event->len == 0) {
      LOG_WARN << "event=" << event->mask << ", watch_fd=" << event->wd;
      continue;
    }

    // VIM creates the weried file 4913...
    if (!strncmp(event->name, "4913", event->len)) {
      continue;
    }

    try {
      auto ext = fspath(event->name).extension().c_str();
      if (!symutil::is_cpp_ext(ext)) {
        continue;
      }
    } catch (const std::exception &e) {
      LOG_ERROR << "file=" << event->name << " extension error: " << e.what();
      return;
    }

    try {
      HandleInotifyEvent(event);
    } catch (const std::exception &e) {
      LOG_ERROR << "exception: " << e.what();
    }
  }

  inotify_stream_->async_wait(AsioStream::wait_read,
                              [this](const boost::system::error_code &ec) {
                                if (!ec) {
                                  this->HandleInotifyReadable();
                                }
                              });
}

void Server::HandleInotifyEvent(const inotify_event *event) {
  LOG_DEBUG << "event=" << event->mask << ", watch_fd=" << event->wd
            << ", file=" << event->name;

  if (ConfigInst.IsFileExcluded(event->name)) {
    LOG_INFO << "file ignored, path=" << event->name;
    return;
  }

  ProjectPtr project = GetProjectByWatcher(event->wd);
  if (!project) {
    LOG_ERROR << "GetProjectByWatcher failed, watch_fd=" << event->wd;
    return;
  }

  bool is_dir = !!(event->mask & IN_ISDIR);
  if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
    project->HandleEntryCreate(event->wd, is_dir, event->name);
  }

  if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
    project->HandleFileModified(event->wd, event->name);
  }

  if (event->mask & IN_DELETE) {
    project->HandleEntryDeleted(event->wd, is_dir, event->name);
  }

  if (event->mask & IN_DELETE_SELF) {
    project->HandleWatchedDirDeleted(event->wd, event->name);
  }
}

void Server::LoadConfiguredProject() {
  for (auto cfg_ptr : ConfigInst.projects()) {
    LOG_DEBUG << "project=" << cfg_ptr->name()
              << ", home=" << cfg_ptr->home_path();
    auto it = projects_.find(cfg_ptr->name());
    if (it != projects_.end()) {
      if (!filesystem::equivalent(it->second->home_path(),
                                  cfg_ptr->home_path())) {
        THROW_AT_FILE_LINE("project<%s> with home<%s> already exists",
                           cfg_ptr->name().c_str(),
                           it->second->home_path().c_str());
      }
    }

    ProjectPtr project;
    try {
      project = Project::CreateFromConfig(cfg_ptr);
    } catch (const std::exception &e) {
      LOG_ERROR << "CreateFromConfig failed, proj_name=" << cfg_ptr->name()
                << ", error=" << e.what();
    }

    if (project) {
      AddProject(cfg_ptr->name(), project);
    }
  }
}

bool Server::IsServerRunning(const std::string &listen_path) {
  boost::asio::io_service io_service;
  boost::asio::local::stream_protocol::socket socket{io_service};
  boost::system::error_code error;
  socket.connect(listen_path, error);
  io_service.run_one();

  return !error && socket.is_open();
}

}  // namespace symdb
