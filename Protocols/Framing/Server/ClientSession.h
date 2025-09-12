#pragma once

#include <thread>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "winsock2.h"

class ChatServer;

class ClientSession
{
public:
  ClientSession(SOCKET socket, ChatServer* server);
  ~ClientSession();

  void Start();
  void Stop();
  void SendMsg(const std::string& msg); 
  void SendMsg(std::string&& msg);

private:
  void RecvLoop();
  void SendLoop();
  void GracefulShutdown();

private:
  SOCKET m_socket;
  ChatServer* m_server;

  // TX backpressure
  std::thread m_txThread;
  std::mutex m_txMutex;
  std::condition_variable m_txNotEmpty;
  std::condition_variable m_txNotFull;
  std::deque<std::string> m_txQueue;
  size_t m_txBytesQueued = 0;

  // RX
  std::thread m_rxThread;

  std::atomic<bool> m_active;
};