// UDP link to BeamNG's gtaxbeam_bridge on 127.0.0.1:47801 (PROTOCOL.md). Non-blocking; call from the script thread.
#pragma once
#include <string>

namespace bng
{
	bool start(unsigned short beamngPort);
	void stop();
	void send(const char *data, int len);
	void sendf(const char *fmt, ...);
	/// Next datagram from BeamNG, if any.
	bool poll(std::string &out);

	/// Tiny readers for BeamNG's flat JSON messages.
	bool is_type(const std::string &msg, const char *type);
	bool num(const std::string &msg, const char *key, double &out);
	bool vec3(const std::string &msg, const char *key, float out[3]);
	bool boolean(const std::string &msg, const char *key, bool &out);
}
