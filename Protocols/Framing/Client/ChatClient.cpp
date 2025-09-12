#include <iostream>

#include "ChatClient.h"
#include "ws2tcpip.h"

#include "../Shared/Framing.h"

using std::cin;
using std::cout;
using std::thread;
using std::mutex;
using std::string;

constexpr size_t TX_HWM = 1024u * 1024u; // 1 MB
constexpr size_t TX_LWM = 512u * 1024u; // 0.5 MB
constexpr unsigned int MAX_RX_QUEUE_SIZE = 500;
constexpr int RECV_TIMEOUT = 3000; // ms

ChatClient::ChatClient(const char* ipadd, const char* port)
{
  m_isActive.store(false, std::memory_order_release);
  m_port = port;
  m_ipadd = ipadd;
  m_socket = INVALID_SOCKET;
}

void ChatClient::PrintMessages()
{
  string msg;
  while (true)
  {
    std::unique_lock<mutex>lock(m_rxMutex);
    m_rxCV.wait(lock, [&]
    {
      return !m_isActive.load(std::memory_order_acquire) || !m_rxQueue.empty();
    });

    if (!m_isActive.load(std::memory_order_acquire) && m_rxQueue.empty())
      break;

    msg = std::move(m_rxQueue.front());
    m_rxQueue.pop();
    lock.unlock();

    cout << msg << "\n";
  }
}

void ChatClient::ReceiveMessages()
{
  std::string msg;

  while (m_isActive.load(std::memory_order_acquire))
  {
    if (!recv_frame(m_socket, msg))
    {
      cout << "Server disconnected. Press Enter to exit.\n";
      m_isActive.store(false, std::memory_order_release);
      m_rxCV.notify_all(); // unsleep printer
      break;
    }

    {
      std::unique_lock<std::mutex>lock(m_rxMutex);

      bool wouldOverflowMsgs = (static_cast<unsigned int>(m_rxQueue.size()) >= MAX_RX_QUEUE_SIZE);
      if (wouldOverflowMsgs)
      {
        switch (m_rxMode)
        {
          case RxOverflowMode::DropNewest:
            break;

          case RxOverflowMode::DropOldest:
            if (!m_rxQueue.empty())
            {
              m_rxQueue.pop();
            }
            m_rxQueue.push(std::move(msg));
            m_rxCV.notify_all();
            break;

          case RxOverflowMode::DisconnectOnOverflow:
            m_isActive.store(false, std::memory_order_release);
            lock.unlock();
            m_rxCV.notify_all();
            return;
        }
      }
      else
      {
        m_rxQueue.push(std::move(msg));
        m_rxCV.notify_all();
      }
    }
  }
}

void ChatClient::SendMessages()
{
  while (true)
  {
    std::string msg;

    {
      std::unique_lock<std::mutex> lock(m_txMutex);
      m_txNotEmpty.wait(lock, [&] 
      {
        return !m_isActive.load(std::memory_order_acquire) || !m_txQueue.empty();
      });

      if (!m_isActive.load(std::memory_order_acquire) && m_txQueue.empty())
        break;

      msg = std::move(m_txQueue.front());
      m_txBytesQueued -= msg.size();
      m_txQueue.pop();

      // if we have size to recieve new messages in queue for sending
      if (m_txBytesQueued <= TX_LWM)
        m_txNotFull.notify_all();
    }

    if (!send_frame(m_socket, msg)) 
    {
      m_isActive.store(false, std::memory_order_release);
      m_txNotEmpty.notify_all();
      m_txNotFull.notify_all();
      return;
    }
  }
}

bool ChatClient::TxEnqueueMessage(std::string msg)
{
  std::unique_lock<std::mutex>lock(m_txMutex);

  size_t msgSize = msg.size();

  auto wouldOverflowMsgs = [&]()
    {
      return (m_txBytesQueued + msgSize) > TX_HWM;
    };

  if (m_txMode == TxOverflowMode::BlockProducer)
  {
    m_txNotFull.wait(lock, [&]()
      {
        return !m_isActive.load(std::memory_order_acquire) ||
          (m_txBytesQueued <= TX_LWM && !wouldOverflowMsgs());
      });

    if (!m_isActive.load(std::memory_order_acquire))
      return false;
  }
  else if (wouldOverflowMsgs())
  {
    switch (m_txMode)
    {
      case TxOverflowMode::DropNewest:
        return false;

      case TxOverflowMode::DropOldest:
        while (!m_txQueue.empty() && wouldOverflowMsgs()) 
        {
          m_txBytesQueued -= m_txQueue.front().size();
          m_txQueue.pop();
        }
        break;

      case TxOverflowMode::DisconnectOnOverflow:
        m_isActive.store(false, std::memory_order_release);
        m_txNotEmpty.notify_all();
        return false;

      default: break;
    }
  }

  const bool was_empty = m_txQueue.empty();
  m_txQueue.push(std::move(msg));
  m_txBytesQueued += msgSize;

  if (was_empty)
    m_txNotEmpty.notify_all();

  return true;
}

void ChatClient::CinMessages()
{
  string msg;
  while (std::getline(cin, msg))
  {
    if (!m_isActive.load(std::memory_order_acquire))
      break;

    if (msg.size() > TX_HWM)
    {
      cout << "Message is too heavy. Skipped.\n";
      break;
    }

    if (!TxEnqueueMessage(std::move(msg)))
      break;
  }
}

void ChatClient::Run()
{
  WSADATA wsaData;
  int operationResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (operationResult != 0)
  {
    cout << "WSAStartup filed:" << operationResult << "\n";
    return;
  }

  struct addrinfo* result = nullptr, *ptr = nullptr, hints;
  memset(&hints, 0, sizeof(hints)); // Make all members of the struct = 0
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = 0;

  operationResult = getaddrinfo(m_ipadd.c_str(), m_port.c_str(), &hints, &result);
  if (operationResult != 0)
  {
    cout << "getaddrinfo failed:" << operationResult << "\n";
    WSACleanup();
    return;
  }

  for (ptr = result; ptr != nullptr; ptr = ptr->ai_next)
  {
    // Try create socket
    m_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
    if (m_socket == INVALID_SOCKET)
      continue;

    // Try connect socket
    if (connect(m_socket, ptr->ai_addr, (int)ptr->ai_addrlen) == 0)
        break;

    closesocket(m_socket);
  }

  freeaddrinfo(result);

  if (m_socket == INVALID_SOCKET)
  {
    cout << "Could not connect server" << WSAGetLastError() << "\n";
    WSACleanup();
    return;
  }

  m_isActive = true;

  m_recv = thread(&ChatClient::ReceiveMessages, this);
  m_print = thread(&ChatClient::PrintMessages, this);
  m_send = thread(&ChatClient::SendMessages, this);

  CinMessages();

  Disconnect();
  m_recv.join();
  m_send.join();
  m_print.join();

  WSACleanup();
}

void ChatClient::GracefulShutDown()
{
  shutdown(m_socket, SD_SEND);

  int recvTimeOut = RECV_TIMEOUT;
  setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, 
    reinterpret_cast<const char*>(&recvTimeOut), sizeof(recvTimeOut));

  string msg;

  while (recv_frame(m_socket, msg)) {}
}

void ChatClient::Disconnect()
{
  m_isActive.store(false, std::memory_order_release);
  m_rxCV.notify_all();

  m_txNotEmpty.notify_all();
  m_txNotFull.notify_all();

  if (m_socket != INVALID_SOCKET)
  {
    GracefulShutDown();
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
  }
}