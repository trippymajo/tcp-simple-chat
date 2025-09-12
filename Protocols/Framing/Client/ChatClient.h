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

enum class TxOverflowMode
{
  BlockProducer, DropNewest, DropOldest, DisconnectOnOverflow
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
  bool TxEnqueueMessage(std::string msg);
  void CinMessages();
  void GracefulShutDown();

private:
  std::atomic<bool> m_isActive;
  std::string m_port;
  std::string m_ipadd;
  SOCKET m_socket;

  // RX
  std::mutex m_rxMutex;
  std::condition_variable m_rxCV;
  std::queue<std::string> m_rxQueue;
  RxOverflowMode m_rxMode = RxOverflowMode::DropNewest;

  // TX
  std::mutex m_txMutex;
  std::condition_variable m_txNotEmpty;
  std::condition_variable m_txNotFull;
  size_t m_txBytesQueued = 0;
  std::queue<std::string> m_txQueue;
  TxOverflowMode m_txMode = TxOverflowMode::BlockProducer;

  std::thread m_print;
  std::thread m_recv;
  std::thread m_send;
};