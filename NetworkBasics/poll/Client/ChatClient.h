#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>

#include <sys/poll.h>   // poll()

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

  void CreatePoll();
  void SetPollEvents();
  bool HandleConnection();

  bool Read();
  bool Write();

private:
  const char* m_ip;
  const char* m_port;

  std::atomic<bool> m_running {false};
  int m_socket = -1;
  pollfd m_poll;

  std::mutex m_sendMutex;
  std::deque<std::string> m_sendQueue;
};