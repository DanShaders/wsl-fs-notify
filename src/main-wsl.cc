#include <ev.h>
#include <linux/limits.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "message.h"

bool write_exactly(int fd, const char *buff, size_t len) {
	while (len) {
		ssize_t written = write(fd, buff, len);
		if (written <= 0) {
			return false;
		}
		len -= written;
		buff += written;
	}
	return true;
}

namespace {
int notify_fd;
char buf[sizeof(struct inotify_event) + PATH_MAX + 1]
	__attribute__((aligned(__alignof__(struct inotify_event))));

struct Directory {
	std::string path;
	std::vector<void *> handles;
};

std::map<int, Directory> listeners;

void stdin_cb(EV_P_ ev_io *w, int) {
	char type;
	if (read(STDIN_FILENO, &type, 1) == 0) {
		ev_io_stop(EV_A_ w);
		ev_break(EV_A_ EVBREAK_ALL);
	}

	if (type == 'D') {
		DirectoryWatchRequest req;
		assert(read(STDIN_FILENO, ((char *) &req) + 1, sizeof(req) - 1) == sizeof(req) - 1);

		char path[req.path_length];
		assert(read(STDIN_FILENO, path, req.path_length) == (ssize_t) req.path_length);

		std::string real_path{path, req.path_length};
		auto pos = real_path.find('\\', 13);
		assert(pos != real_path.npos);
		real_path = real_path.substr(pos);
		for (char &c : real_path) {
			if (c == '\\') {
				c = '/';
			}
		}

		int wd = inotify_add_watch(notify_fd, real_path.c_str(),
								   IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO);

		auto &dir = listeners[wd];
		dir.handles.push_back(req.directory);
	}
}

void notify_cb(EV_P_ ev_io *w, int) {
	const struct inotify_event *event;
	ssize_t len = read(notify_fd, buf, sizeof(buf));

	for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
		event = (const struct inotify_event *) ptr;

		const auto &dir = listeners[event->wd];
		for (auto handle : dir.handles) {
			auto filename = dir.path + std::string{event->name, event->len};

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

			// std::cerr << "sending " << filename << std::endl;

			auto to_send = Message::from(msg, filename.c_str(), filename.size());
			assert(write_exactly(STDOUT_FILENO, (const char *) to_send.get(),
								 to_send->length + sizeof(Message)));
		}
	}
}
}  // namespace

int main() {
	char hello_buff[HELLO_LENGTH];
	assert(read(STDIN_FILENO, hello_buff, HELLO_LENGTH) == HELLO_LENGTH);
	assert(!strncmp(hello_buff, CLIENT_HELLO, HELLO_LENGTH));
	assert(write_exactly(STDOUT_FILENO, SERVER_HELLO, HELLO_LENGTH));

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
