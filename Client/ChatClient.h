#pragma once

#include <string>
#include <deque>

#include "winsock2.h"

class ChatClient
{
public:
  ChatClient(const char* ipadd, const char* port);
  ~ChatClient() {};

  void Start();
  void Stop();
  void EnqueueSend(std::string line);

private:
  bool CreateConnection();
  bool ConnectToServer(SOCKET socket, addrinfo* pInfo);
  void RunEventLoop();
  void BuildFdSets(fd_set& rset, fd_set& wset);
  int WaitForEvents(fd_set& rset, fd_set& wset);
  bool HandleReadyReceivers(const fd_set& readSet);
  bool HandleReadyWriters(const fd_set& writeSet);
  int ReceiveMsg();
  int SendMsg();

private:
  bool m_isActive;
  std::string m_port;
  std::string m_ipadd;
  SOCKET m_socket;
  struct Pending { std::string data; size_t ofs = 0; };
  std::deque<Pending> m_sendQ;
};