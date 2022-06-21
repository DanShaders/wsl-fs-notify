#include <dirent.h>
#include <ev.h>
#include <linux/limits.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "message.h"

const uint32_t INOTIFY_EVENTS = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO;

int notify_fd;

struct Directory {
	int wd;
	std::string path;
	std::set<void *> handles;
};

PullableMessageStream in_stream;
std::map<int, std::shared_ptr<Directory>> listeners;
std::multimap<void *, std::shared_ptr<Directory>> by_handle;

void install_watchers(std::string root, std::string path, void *handle) {
	auto abspath = root + path;

	int wd = inotify_add_watch(notify_fd, abspath.c_str(), INOTIFY_EVENTS);
	if (wd == -1) {
		return;
	}

	auto dir_it = listeners.find(wd);
	if (dir_it == listeners.end()) {
		dir_it = listeners.insert({wd, std::make_shared<Directory>()}).first;
		dir_it->second->path = path;
	}
	auto dir = dir_it->second;
	if (dir->handles.count(handle)) {
		return;
	}
	dir->handles.insert(handle);
	by_handle.insert({handle, dir});

	DIR *d = opendir(abspath.c_str());
	if (d) {
		dirent *entry;
		while ((entry = readdir(d))) {
			std::string_view filename{entry->d_name};
			if (entry->d_type == DT_DIR && filename != "." && filename != "..") {
				install_watchers(root, path + entry->d_name + "/", handle);
			}
		}
		closedir(d);
	}
}

void do_directory_watch(DirectoryWatchRequest *req, std::string_view path) {
	install_watchers(std::string(path) + "/", "", req->directory);
}

void do_directory_unwatch(DirectoryUnwatchRequest *req) {
	auto [start, end] = by_handle.equal_range(req->directory);

	auto cnext = [](auto it) {
		if (it == by_handle.end()) {
			return it;
		}
		return next(it);
	};
	for (auto nxt = cnext(start); start != end; start = nxt, nxt = cnext(nxt)) {
		auto dir = start->second;
		dir->handles.erase(req->directory);
		if (dir->handles.size() == 0) {
			listeners.erase(dir->wd);
			inotify_rm_watch(notify_fd, dir->wd);
		}
		by_handle.erase(start);
	}
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

void notify_cb(EV_P_ ev_io *, int) {
	static char buf[sizeof(struct inotify_event) + PATH_MAX + 1]
		__attribute__((aligned(__alignof__(struct inotify_event))));

	const struct inotify_event *event;
	ssize_t len = read(notify_fd, buf, sizeof(buf));

	for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
		event = (const struct inotify_event *) ptr;

		const auto &dir = listeners[event->wd];
		for (auto handle : dir->handles) {
			auto filename = dir->path + std::string{event->name, event->len};

			Event msg;
			msg.msg_type = 'U';
			msg.directory = handle;

			if (event->mask & IN_CREATE) {
				msg.action = FILE_ACTION_ADDED;
			} else if (event->mask & IN_MODIFY) {
				msg.action = FILE_ACTION_MODIFIED;
			} else if (event->mask & IN_MOVED_FROM) {
				msg.action = FILE_ACTION_REMOVED;
			} else if (event->mask & IN_MOVED_TO) {
				msg.action = FILE_ACTION_ADDED;
			} else if (event->mask & IN_DELETE) {
				msg.action = FILE_ACTION_REMOVED;
			}

			Message::from(msg, filename.c_str(), filename.size())->write_to(STDOUT_FILENO);
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

	struct ev_loop *loop = EV_DEFAULT;

	ev_io stdin_watcher;
	ev_io_init(&stdin_watcher, stdin_cb, STDIN_FILENO, EV_READ);
	ev_io_start(loop, &stdin_watcher);

	notify_fd = inotify_init1(IN_NONBLOCK);
	ev_io notify_watcher;
	ev_io_init(&notify_watcher, notify_cb, notify_fd, EV_READ);
	ev_io_start(loop, &notify_watcher);

	ev_run(loop, 0);

	close(notify_fd);
}
