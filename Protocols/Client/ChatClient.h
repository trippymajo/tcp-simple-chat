#pragma once

#include <thread>
#include <atomic>
#include <string>
#include <queue>
#include <mutex>

#include "winsock2.h"

enum class RxOverflowMode
{
  DropNewest, DropOldest, DisconnectOnOverflow
};

class ChatClient
{
public:
  ChatClient(const char* ipadd, const char* port);
  ~ChatClient() {};

  void Run();

private:
  void PrintMessages();
  void ReceiveMessages();
  void Disconnect();
  void SendMessages();
  void GracefulShutDown();

private:
  std::atomic<bool> m_isActive;
  std::string m_port;
  std::string m_ipadd;
  SOCKET m_socket;

  // RX
  std::mutex m_mtxQueue;
  std::condition_variable m_cvQueue;
  std::queue<std::string> m_rxQueue;
  RxOverflowMode m_rxMode = RxOverflowMode::DropNewest;

  std::thread m_print;
  std::thread m_recv;
};