#include "message.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#ifdef WIN32
namespace std {
void *aligned_alloc(size_t alignment, size_t size) {
	return _aligned_malloc(size, alignment);
}
}  // namespace std
#endif

PMessage Message::from(const char *body, uint64_t body_size, const char *trailing, uint64_t tlen) {
	auto raw = (Message *) std::aligned_alloc(alignof(Message), sizeof(Message) + body_size + tlen);
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
	auto raw = (Message *) std::aligned_alloc(alignof(Message), sizeof(Message) + next_length);
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
