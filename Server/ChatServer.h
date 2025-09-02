#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <sys/epoll.h>   // epoll()

class ClientSession;

class ChatServer
{
public:
  ChatServer(const std::vector<std::string>& ips, const std::string& port);
  ~ChatServer();

  void Start();
  void Stop();

  void BroadcastMsg(const std::string& msg, ClientSession* pSender);

private:
  int CreateListenSocket(const std::string& ip);
  void RunLoop();

  void HandleListeners(int& sfd, uint32_t& event);
  void HandleClients(int& sfd, uint32_t& event);

  void AddListenToEpoll(const int& lsocket);
  void AddClientToEpoll(const int& clsocket);

  void AcceptAll(int& fd);
  void CloseClient(int& sfd);
  void ModClientWritable(int fd, const bool& enable);
  bool ReadClient(int& sfd);
  bool WriteClient(int& sfd);


private:
  bool m_running = false;
  int m_epoll = -1;
  std::string m_port;
  std::vector<std::string> m_ips;

  std::unordered_set<int> m_listenSockets;
  std::unordered_map<int, std::unique_ptr<ClientSession>> m_clients;
};
