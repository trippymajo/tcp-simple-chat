#pragma once

#include <WinSock2.h>
#include <string>

bool send_frame(SOCKET s, const std::string& payload);
bool recv_frame(SOCKET s, std::string& out);