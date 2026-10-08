#include "link.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	SOCKET g_sock = INVALID_SOCKET;
	sockaddr_in g_peer = {};
	bool g_wsa = false;

	const char *find_value(const std::string &msg, const char *key)
	{
		char pattern[64];
		std::snprintf(pattern, sizeof(pattern), "\"%s\"", key);
		size_t at = 0;
		while ((at = msg.find(pattern, at)) != std::string::npos)
		{
			const char *p = msg.c_str() + at + std::strlen(pattern);
			while (*p == ' ')
				++p;
			if (*p == ':')
			{
				++p;
				while (*p == ' ')
					++p;
				return p;
			}
			at += 1;
		}
		return nullptr;
	}
}

namespace bng
{
	bool start(unsigned short beamngPort)
	{
		if (g_sock != INVALID_SOCKET)
			return true;
		if (!g_wsa)
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return false;
			g_wsa = true;
		}
		g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (g_sock == INVALID_SOCKET)
			return false;
		sockaddr_in local = {};
		local.sin_family = AF_INET;
		local.sin_port = 0;
		inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
		bind(g_sock, reinterpret_cast<sockaddr *>(&local), sizeof(local));
		u_long nonBlocking = 1;
		ioctlsocket(g_sock, FIONBIO, &nonBlocking);
		int buf = 1 << 20;
		setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buf), sizeof(buf));
		setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&buf), sizeof(buf));
		// BeamNG not listening yet makes Windows report ICMP port-unreachable as WSAECONNRESET on the next recv
		BOOL noReset = FALSE;
		DWORD bytes = 0;
		WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12), &noReset, sizeof(noReset), nullptr, 0, &bytes, nullptr, nullptr);
		g_peer.sin_family = AF_INET;
		g_peer.sin_port = htons(beamngPort);
		inet_pton(AF_INET, "127.0.0.1", &g_peer.sin_addr);
		return true;
	}

	void stop()
	{
		if (g_sock != INVALID_SOCKET)
		{
			closesocket(g_sock);
			g_sock = INVALID_SOCKET;
		}
	}

	void send(const char *data, int len)
	{
		if (g_sock != INVALID_SOCKET)
			sendto(g_sock, data, len, 0, reinterpret_cast<const sockaddr *>(&g_peer), sizeof(g_peer));
	}

	void sendf(const char *fmt, ...)
	{
		char buf[2048];
		va_list args;
		va_start(args, fmt);
		const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (n > 0 && n < int(sizeof(buf)))
			send(buf, n);
	}

	bool poll(std::string &out)
	{
		if (g_sock == INVALID_SOCKET)
			return false;
		static char buf[65536];
		const int n = recv(g_sock, buf, sizeof(buf), 0);
		if (n <= 0)
			return false;
		out.assign(buf, n);
		return true;
	}

	bool is_type(const std::string &msg, const char *type)
	{
		const char *p = find_value(msg, "t");
		if (!p || *p != '"')
			return false;
		const size_t len = std::strlen(type);
		return std::strncmp(p + 1, type, len) == 0 && p[1 + len] == '"';
	}

	bool num(const std::string &msg, const char *key, double &out)
	{
		const char *p = find_value(msg, key);
		if (!p)
			return false;
		char *end = nullptr;
		out = std::strtod(p, &end);
		return end != p;
	}

	bool vec3(const std::string &msg, const char *key, float out[3])
	{
		const char *p = find_value(msg, key);
		if (!p || *p != '[')
			return false;
		++p;
		for (int i = 0; i < 3; ++i)
		{
			char *end = nullptr;
			out[i] = std::strtof(p, &end);
			if (end == p)
				return false;
			p = end;
			while (*p == ',' || *p == ' ')
				++p;
			if (i < 2 && *p == ']')
				return false;
		}
		return true;
	}

	bool boolean(const std::string &msg, const char *key, bool &out)
	{
		const char *p = find_value(msg, key);
		if (!p)
			return false;
		out = std::strncmp(p, "true", 4) == 0;
		return true;
	}
}
