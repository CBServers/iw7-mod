#pragma once

#include <string>

namespace ipc
{
	// Queue a single-line JSON message for the launcher. Thread-safe; dropped if the pipe is down.
	void send_message(std::string line);

	// Push a presence update on the next main-thread frame instead of waiting for the 2s tick.
	void flush_presence();
}
