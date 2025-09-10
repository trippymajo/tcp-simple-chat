#include <iostream>

#include "ChatClient.h"
#include "ws2tcpip.h"

#include "../Shared/Framing.h"

using std::cin;
using std::cout;
using std::thread;
using std::mutex;
using std::string;

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
    std::unique_lock<mutex>lock(m_mtxQueue);
    m_cvQueue.wait(lock, [&]
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
      m_cvQueue.notify_all(); // unsleep printer
      break;
    }

    {
      std::unique_lock<std::mutex>lock(m_mtxQueue);

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
            m_cvQueue.notify_all();
            break;

          case RxOverflowMode::DisconnectOnOverflow:
            m_isActive.store(false, std::memory_order_release);
            lock.unlock();
            m_cvQueue.notify_all();
            return;
        }
      }
      else
      {
        m_rxQueue.push(std::move(msg));
        m_cvQueue.notify_all();
      }
    }
  }
}

void ChatClient::SendMessages()
{
  string msg;
  while (std::getline(cin, msg))
  {
    if (!m_isActive.load(std::memory_order_acquire))
      break;

    // read num of bytes sent, and do send all untill the full message sent
    send_frame(m_socket, msg);
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

  SendMessages();

  Disconnect();
  m_recv.join();
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
  m_cvQueue.notify_all();


  if (m_socket != INVALID_SOCKET)
  {
    GracefulShutDown();
    closesocket(m_socket);
    m_socket = INVALID_SOCKET;
  }
}