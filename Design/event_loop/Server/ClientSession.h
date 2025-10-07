#pragma once

#include <string>
#include <deque>

class ChatServer;

/// <summary>
/// Client session with overlapped recv/send and a send queue.
/// </summary>
class ClientSession
{
public:
  ClientSession(int& socket, ChatServer* server);
  ~ClientSession();

  void Stop();

  bool IsWantSend() { return !m_sendQueue.empty(); }

  bool Read();
  bool Write();

  void PostSend(const std::string& msg);

private:
  void GracefulShutdown();

private:
  int m_socket;
  ChatServer* m_server;

  std::deque<std::string> m_sendQueue;
};