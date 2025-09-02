#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>

#include <sys/epoll.h>   // poll()

class ChatClient
{
public:
  ChatClient(const char* ipadd, const char* port);
  ~ChatClient();


  void Start();
  void Stop();

  void Send(const std::string& msg);

private:
  bool CreateConnection();
  void RunLoop();

  void CreateEpoll();
  void AddSockToEpoll();
  bool HandleConnection(uint32_t& ev);

  void ModWritable(const bool& enable);
  bool Read();
  bool Write();

private:
  const char* m_ip;
  const char* m_port;

  std::atomic<bool> m_running {false};
  int m_socket = -1;
  int m_epoll = -1;

  std::mutex m_sendMutex;
  std::deque<std::string> m_sendQueue;
};