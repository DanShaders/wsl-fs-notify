#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

struct Message;

struct MessageDeleter {
	void operator()(Message *msg) {
#ifdef WIN32
		_aligned_free(msg);
#else
		free(msg);
#endif
	}
};

using PMessage = std::unique_ptr<Message, MessageDeleter>;

struct Message {
	uint64_t length;
	char data[0];

	template <typename T>
	T *as() {
		return (T *) data;
	}

	template <typename T>
	std::string_view get_trailer() {
		return {data + sizeof(T), data + length};
	}

	template <typename T>
	static PMessage from(const T &obj, const char *trailing = nullptr, uint64_t tlen = 0) {
		return from((const char *) &obj, sizeof(T), trailing, tlen);
	}

	static PMessage from(const char *body, uint64_t body_size, const char *trailing, uint64_t tlen);
};

class MessageStream {
private:
	const int64_t NO_MESSAGE = -1, LENGTH_MASK = 0x7fffffffffffffff;

	mutable std::stringbuf stream;
	mutable int64_t avail = 0;
	mutable int64_t next_length = NO_MESSAGE;

protected:
	virtual bool pull([[maybe_unused]] size_t length) {
		return false;
	}

public:
	virtual ~MessageStream() {}

	void feed(const char *buff, size_t length);

	bool has_message() const;
	std::optional<PMessage> get_message();
	std::optional<PMessage> pull_message();
};
