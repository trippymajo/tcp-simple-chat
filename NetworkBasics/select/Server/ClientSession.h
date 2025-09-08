#pragma once

#include <string>
#include <memory>

#include "winsock2.h"

class ChatServer;

class ClientSession
{
public:
  ClientSession(SOCKET socket, ChatServer* server);
  ~ClientSession();

  void Stop();
  void SendMsg(const std::string& msg);
  int ReceiveMsg();
  SOCKET GetSocket() { return m_socket; }

private:
  void GracefulShutdown();

private:
  SOCKET m_socket;
  ChatServer* m_server;
};