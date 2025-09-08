#pragma once

#include <WinSock2.h>
#include <string>

static bool send_frame(SOCKET s, const std::string& payload);
static bool recv_frame(SOCKET s, std::string& out);