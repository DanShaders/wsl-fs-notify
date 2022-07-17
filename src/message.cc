#include "message.h"

#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#ifdef WIN32
#	include <Windows.h>
#endif

bool Message::write_to(fd_t handle) const {
	return write_exactly(handle, {(char *) this, sizeof(Message) + length});
}

void Message::write_to(std::string &s) const {
	s += std::string_view{(char *) this, sizeof(Message) + length};
}

PMessage Message::from(const char *body, uint64_t body_size, const char *trailing, uint64_t tlen) {
	auto raw = (Message *) std::malloc(sizeof(Message) + body_size + tlen);
	assert(raw != nullptr);
	PMessage msg{raw};
	msg->length = body_size + tlen;
	memcpy(msg->data, body, body_size);
	if (trailing != nullptr) {
		memcpy(msg->data + body_size, trailing, tlen);
	}
	return msg;
}

void MessageStream::feed(const char *buff, size_t length) {
	avail += length;
	stream.sputn(buff, length);
}

bool MessageStream::has_message() const {
	if (next_length == NO_MESSAGE) {
		if (avail < 8) {
			return false;
		}
		stream.sgetn((char *) &next_length, 8);
		avail -= 8;
		next_length &= LENGTH_MASK;
	}
	return avail >= next_length;
}

std::optional<PMessage> MessageStream::get_message() {
	if (!has_message()) {
		return {};
	}
	auto raw = (Message *) std::malloc(sizeof(Message) + next_length);
	assert(raw != nullptr);
	PMessage msg{raw};
	msg->length = next_length;
	stream.sgetn(msg->data, next_length);
	avail -= next_length;
	next_length = NO_MESSAGE;
	return msg;
}

std::optional<PMessage> MessageStream::pull_message() {
	if (has_message()) {
		return get_message();
	}
	if (next_length == NO_MESSAGE) {
		if (!pull(8 - avail)) {
			return {};
		}
		assert(avail >= 8);
		stream.sgetn((char *) &next_length, 8);
		avail -= 8;
		next_length &= LENGTH_MASK;
	}
	if (avail < next_length) {
		if (!pull(next_length - avail)) {
			return {};
		}
	}
	return get_message();
}

bool PullableMessageStream::pull(size_t length) {
	const int BUFF = 4096;
	static char buff[BUFF];

	if (fd == INVALID_FD) {
		return false;
	}

	while (length) {
#ifdef WIN32
		DWORD res;
		if (!ReadFile(fd, buff, BUFF, &res, nullptr)) {
			return false;
		}
#else
		ssize_t res = read(fd, buff, BUFF);
		if (res <= 0) {
			return false;
		}
#endif
		feed(buff, res);
		if ((size_t) res >= length) {
			break;
		}
		length -= res;
	}
	return true;
}

void PullableMessageStream::set_fd(fd_t fd_) {
	assert(fd == INVALID_FD);
	fd = fd_;
}