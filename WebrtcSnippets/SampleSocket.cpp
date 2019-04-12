#include "rtc_base/physical_socket_server.h"
#include "rtc_base/thread.h"

#if defined(WEBRTC_WIN)
#include "rtc_base/win32_socket_init.h"
#endif

#include <iostream>
#include <memory>
#include <cstring>

void SampleSocket()
{
#if defined(WEBRTC_WIN)
	rtc::WinsockInitializer winsock_init;
#endif

	// udp: client -> server
	rtc::IPAddress src(AF_INET);
	auto pss = std::make_unique<rtc::PhysicalSocketServer>();
	std::unique_ptr<rtc::AsyncSocket> client(
		pss->CreateAsyncSocket(src.family(), SOCK_DGRAM));
	std::unique_ptr<rtc::AsyncSocket> server(
		pss->CreateAsyncSocket(src.family(), SOCK_DGRAM));
	if (client->Bind(rtc::SocketAddress("127.0.0.1", 0)) != 0 ||
		server->Bind(rtc::SocketAddress("127.0.0.1", 0)) != 0) {
		std::cerr << "cannot bind\n";
		return;
	}
	const char* buf = "hello other socket";
	size_t len = strlen(buf);
	int sent = client->SendTo(buf, len, server->GetLocalAddress());
	rtc::SocketAddress addr;
	const size_t kRecvBufSize = 64;
	char recvbuf[kRecvBufSize];
	rtc::Thread::Current()->SleepMs(100);
	int received = server->RecvFrom(recvbuf, kRecvBufSize, &addr, nullptr);
	if (received != sent || ::memcmp(buf, recvbuf, len) != 0) {
		std::cerr << "received something different\n";
		return;
	}

	std::string msg(recvbuf, received);
	std::cout << "received '" << msg << "'\n";
}