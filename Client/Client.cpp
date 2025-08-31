#include "ChatClient.h"

#include <thread>
#include <iostream>
#include <string>
#include <memory>

int main(int argc, char* argv[])
{
  const char* port = "27015";
  const char* ip = "127.0.0.1";
  if (argc > 1) port = argv[1];
  if (argc > 2) ip = argv[2];


  try
  {
    auto cli = std::make_shared<ChatClient>(ip, port);

    std::thread netThread([&] 
      {
        cli->Start();
      });

    std::string line;
    while (std::getline(std::cin, line))
    {
      if (line == "/quit")
      {
        cli->Stop();
        break;
      }
      if (!line.empty())
        line.push_back('\n');
      cli->Send(line);
    }


    if (netThread.joinable())
      netThread.join();
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Exception: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}