#pragma once


#include <string>
#include <deque>
#include <cstdint>
#include <mutex>
#include <thread>
#include <condition_variable>


#include <winsock2.h>
#include <ws2tcpip.h>
#include "../include/Shared.h"


struct SendItem
{
  std::string data;
  size_t off = 0;
  OvEx ov{ {}, OvEx::Kind::Send };
};


class ChatClient
{
public:
  ChatClient(const char* ipadd, const char* port);
  ~ChatClient();


  void Start();
  void Stop();


  bool Send(const std::string& msg);


private:
  // init
  bool InitIocp();
  bool CreateConnection();
  bool ConnectToServer(SOCKET socket, addrinfo* pInfo);
  bool AssociateHandle();


  // IOCP loop + handlers
  void RunEventLoop();
  void OnRecvComplete(DWORD bytes, bool ok);
  void OnSendComplete(DWORD bytes, bool ok);


  // overlapped I/O
  bool PostRecv();
  bool PostSend(const std::string& msg);
  bool KickSend();


  // sender thread
  void SendWorker();


private:
  std::string m_ipadd;
  std::string m_port;


  bool m_isActive = false;
  HANDLE m_iocp = nullptr;
  SOCKET m_socket = INVALID_SOCKET;


  // recv
  OvEx m_ovRecv{ {}, OvEx::Kind::Recv };
  WSABUF m_recvBufWsa{};
  static constexpr size_t RECV_BUF = 64 * 1024;
  char m_recvStorage[RECV_BUF]{};


  // send
  std::deque<SendItem> m_sendQueue;
  bool m_sendInFlight = false;
  std::mutex m_sendMx;


  // sender thread infra
  std::thread m_sendThread;
  std::condition_variable m_sendCv;
  bool m_runSend = false;
};