#include <dirent.h>
#include <ev.h>
#include <linux/limits.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "message.h"

const uint32_t INOTIFY_EVENTS =
    IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY | IN_MOVE | IN_MOVE_SELF;
const uint32_t INOTIFY_FLAGS = IN_DONT_FOLLOW | IN_ONLYDIR | IN_MASK_CREATE | IN_EXCL_UNLINK;

struct ev_loop *loop = EV_DEFAULT;

PullableMessageStream in_stream;

struct Directory;
using PDirectory = std::shared_ptr<Directory>;
using WDirectory = std::weak_ptr<Directory>;

struct Watcher;
using PWatcher = std::shared_ptr<Watcher>;
using WWatcher = std::weak_ptr<Watcher>;

struct Watcher {
  ev_io ev_watcher;
  int fd;
  std::string path;
  void *directory;
  uint32_t filter;
  bool recursive;
  bool is_failed = false;

  std::map<int, WDirectory> by_wd;
  std::deque<WDirectory> unprocessed;
  PDirectory root;

  Watcher(int fd_, std::string_view path_, void *directory_, uint32_t filter_, bool recursive_)
      : fd(fd_), path(path_), directory(directory_), filter(filter_), recursive(recursive_) {}

  ~Watcher() {
    root.reset();
    if (fd != -1) {
      close(fd);
      ev_io_stop(loop, &ev_watcher);
    }
  }

  void send_event(FileAction action, std::string_view filename = "") {
    if (is_failed) {
      return;
    }
    if (action == FILE_ACTION_FAILED) {
      is_failed = true;
    }
    Message::from(
        Event{
            .directory = directory,
            .action = action,
        },
        filename.data(), filename.size())
        ->write_to(STDOUT_FILENO);
  }

  void fail() {
    send_event(FILE_ACTION_FAILED);
  }

  void add_to_queue(PDirectory dir);
  void process_queue();
  void process_events(int move_cookie);
};

struct Directory {
  int wd;
  std::string name;
  WDirectory parent;
  WWatcher watcher;
  std::vector<PDirectory> subdirs;
  int fail_cnt = 0, move_cookie = 0;
  bool tree_deleted = false, already_added = false, in_queue = false;

  ~Directory() {
    if (!watcher.expired()) {
      destruct_tree(watcher.lock().get());
    }
  }

  void destruct_tree(Watcher *w) {
    if (wd == -1) {
      return;
    }
    mark_as_deleted();
    w->by_wd.erase(wd);
    inotify_rm_watch(w->fd, wd);
    for (auto subdir : subdirs) {
      subdir->destruct_tree(w);
    }
    wd = -1;
  }

  void mark_as_deleted() {
    if (!tree_deleted) {
      tree_deleted = true;
      for (auto subdir : subdirs) {
        subdir->mark_as_deleted();
      }
    }
  }

  std::string get_path() {
    if (parent.expired()) {
      return watcher.lock()->path + "/";
    } else {
      return parent.lock()->get_path() + name + "/";
    }
  }

  std::string get_rel_path() {
    if (parent.expired()) {
      return "";
    } else {
      return parent.lock()->get_rel_path() + name + "/";
    }
  }
};

void Watcher::add_to_queue(PDirectory dir) {
  if (!dir->in_queue) {
    dir->in_queue = true;
    unprocessed.push_back(dir);
  }
}

void Watcher::process_events(int move_cookie) {
  struct hl_inotify_event {
    int wd;
    decltype(by_wd)::iterator dir_it;
    PDirectory dir;
    uint32_t mask;
    std::string path, filename;
  };

  std::map<uint32_t, hl_inotify_event> tinder;

  auto process_add = [&](const hl_inotify_event &e) {
    if (!(e.mask & IN_ISDIR)) {
      return;
    }
    std::string abs_path = e.path + e.filename;
    int wd = inotify_add_watch(fd, abs_path.data(), INOTIFY_EVENTS | INOTIFY_FLAGS);
    if (wd == -1) {
      if (errno == EEXIST || errno == ENOTDIR || errno == ENOENT) {
        add_to_queue(e.dir);
      } else {
        fail();
      }
    } else {
      auto curr = std::make_shared<Directory>(wd, e.filename, e.dir, e.dir->watcher);
      e.dir->subdirs.push_back(curr);
      add_to_queue(curr);
    }
  };

  auto process_delete = [&](const hl_inotify_event &e) {
    if (!(e.mask & IN_ISDIR)) {
      return;
    }
    auto curr =
        std::ranges::find(e.dir->subdirs, e.filename, [](const auto &x) { return x->name; });
    if (curr != e.dir->subdirs.end()) {
      (*curr)->mark_as_deleted();
      e.dir->subdirs.erase(curr);
    }
  };

  auto process_move = [&](const hl_inotify_event &from, const hl_inotify_event &to) {
    if (!(from.mask & IN_ISDIR)) {
      return;
    }
    auto curr =
        std::ranges::find(from.dir->subdirs, from.filename, [](const auto &x) { return x->name; });
    if (curr != from.dir->subdirs.end()) {
      to.dir->subdirs.push_back(*curr);
      (*curr)->parent = to.dir;
      from.dir->subdirs.erase(curr);
    }
  };

  auto process_event = [&](const inotify_event &raw) {
    auto dir_it = by_wd.find(raw.wd);
    if (dir_it == by_wd.end()) {
      return;
    }
    if (dir_it->second.expired()) {
      by_wd.erase(dir_it);
      return;
    }
    auto dir = dir_it->second.lock();

    hl_inotify_event e{
        .wd = raw.wd,
        .dir_it = dir_it,
        .dir = dir,
        .mask = raw.mask,
        .path = dir->get_path(),
        .filename = std::string{raw.name},
    };

    auto rel_path = dir->get_rel_path();

    if ((e.mask & IN_MOVE_SELF) || (e.mask & IN_DELETE_SELF)) {
      if (e.wd == root->wd) {
        fail();
      } else {
        e.dir->move_cookie = move_cookie;
      }
    }

    if ((e.mask & IN_IGNORED) || (e.mask & IN_UNMOUNT)) {
      if (e.wd == root->wd) {
        fail();
      } else {
        process_delete(e);
      }
      return;
    }
    if ((e.mask & IN_MODIFY) || (e.mask & IN_ATTRIB)) {
      send_event(FILE_ACTION_MODIFIED, rel_path + e.filename);
    } else if (e.mask & IN_MOVED_FROM) {
      send_event(FILE_ACTION_REMOVED, rel_path + e.filename);
      tinder[raw.cookie] = e;
    } else if (e.mask & IN_MOVED_TO) {
      send_event(FILE_ACTION_ADDED, rel_path + e.filename);
      auto match = tinder.find(raw.cookie);
      if (match == tinder.end()) {
        process_add(e);
      } else {
        process_move(match->second, e);
        tinder.erase(match);
      }
    } else if (e.mask & IN_CREATE) {
      send_event(FILE_ACTION_ADDED, rel_path + e.filename);
      process_add(e);
    } else if (e.mask & IN_DELETE) {
      send_event(FILE_ACTION_REMOVED, rel_path + e.filename);
      process_delete(e);
    }
  };

  static char buf[sizeof(inotify_event) + PATH_MAX + 1]
      __attribute__((aligned(alignof(inotify_event))));

  while (true) {
    ssize_t len = read(fd, buf, sizeof(buf));
    if (len <= 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        fail();
      }
      break;
    }

    const inotify_event *event = nullptr;

    for (char *ptr = buf; ptr < buf + len; ptr += sizeof(inotify_event) + event->len) {
      event = (const inotify_event *) ptr;
      process_event(*event);
    }
  }

  for (auto [cookie, event] : tinder) {
    process_delete(event);
  }
}

void Watcher::process_queue() {
  if (!recursive || is_failed) {
    unprocessed.clear();
    return;
  }
  while (unprocessed.size()) {
    auto curr = unprocessed.front();
    unprocessed.pop_front();

    if (curr.expired()) {
      continue;
    }
    auto dir = curr.lock();
    auto dir_abs_path = dir->get_path();

    by_wd[dir->wd] = curr;

    std::vector<PDirectory> subdirs;
    bool trustworthy = true;

    for (auto entry : std::filesystem::directory_iterator{dir_abs_path}) {
      if (!entry.is_directory()) {
        continue;
      }
      std::string curr_path = entry.path().string();
      int wd = inotify_add_watch(fd, curr_path.data(), INOTIFY_EVENTS | INOTIFY_FLAGS);
      if (wd == -1) {
        if (errno == EEXIST || errno == ENOTDIR || errno == ENOENT) {
          trustworthy = (dir->already_added && errno == EEXIST);
          if (!trustworthy) {
            break;
          }
        } else {
          fail();
          return;
        }
      } else {
        auto subdir =
            std::make_shared<Directory>(wd, entry.path().filename().string(), dir, dir->watcher);
        subdirs.push_back(subdir);
      }
    }
    dir->subdirs = std::move(subdirs);

    static int move_cookie = 1;

    process_events(move_cookie);
    if (dir->tree_deleted) {
      continue;
    }

    auto ptr = curr;
    while (!ptr.expired()) {
      auto cdir = ptr.lock();
      if (cdir->move_cookie == move_cookie) {
        trustworthy = false;
      }
      ptr = cdir->parent;
    }
    ++move_cookie;

    if (trustworthy) {
      dir->in_queue = false;
      dir->already_added = true;
      for (auto subdir : dir->subdirs) {
        add_to_queue(subdir);
      }
    } else {
      if (++dir->fail_cnt == DIR_FAIL_CNT) {
        fail();
        return;
      }
      unprocessed.push_back(curr);
    }
  }
}

std::map<void *, PWatcher> watchers;

void notify_cb(EV_P_ ev_io *w, int) {
  auto watcher = reinterpret_cast<Watcher *>(w);
  watcher->process_events(0);
  watcher->process_queue();
}

void do_directory_watch(DirectoryWatchRequest *req, std::string_view path) {
  int notify_fd = inotify_init1(IN_NONBLOCK);
  auto watcher =
      std::make_shared<Watcher>(notify_fd, path, req->directory, req->filter, req->recursive);

  if (notify_fd != -1) {
    ev_io_init(&watcher->ev_watcher, notify_cb, notify_fd, EV_READ);
    ev_io_start(loop, &watcher->ev_watcher);
  }

  int wd = inotify_add_watch(notify_fd, watcher->path.data(), INOTIFY_EVENTS | INOTIFY_FLAGS);
  if (wd == -1) {
    watcher->fail();
    return;
  }

  auto root = std::make_shared<Directory>(wd, "", WDirectory{}, watcher);
  watcher->root = root;
  watcher->unprocessed.push_back(root);

  watchers[req->directory] = watcher;
  watcher->process_queue();
}

void do_directory_unwatch(DirectoryUnwatchRequest *req) {
  watchers.erase(req->directory);
}

void stdin_cb(EV_P_ ev_io *w, int) {
  const int BUFF = 4096;
  static char buff[BUFF];

  ssize_t buff_len = read(STDIN_FILENO, buff, BUFF);
  if (buff_len <= 0) {
    ev_io_stop(EV_A_ w);
    ev_break(EV_A_ EVBREAK_ALL);
  }
  in_stream.feed(buff, buff_len);

  while (in_stream.has_message()) {
    auto msg = *in_stream.get_message();

    if (msg->data[0] == 'D') {
      do_directory_watch(msg->as<DirectoryWatchRequest>(),
                         msg->get_trailer<DirectoryWatchRequest>());
    } else if (msg->data[0] == 'S') {
      do_directory_unwatch(msg->as<DirectoryUnwatchRequest>());
    }
  }
}

int main() {
  in_stream.set_fd(STDIN_FILENO);

  auto client_hello = in_stream.pull_message();
  assert(client_hello);
  assert((*client_hello)->as<HelloRequest>()->is_eq(CLIENT_HELLO));

  HelloRequest req;
  memcpy(req.data, SERVER_HELLO, HELLO_LENGTH);
  Message::from(req)->write_to(STDOUT_FILENO);

  ev_io stdin_watcher;
  ev_io_init(&stdin_watcher, stdin_cb, STDIN_FILENO, EV_READ);
  ev_io_start(loop, &stdin_watcher);

  ev_run(loop, 0);
}
