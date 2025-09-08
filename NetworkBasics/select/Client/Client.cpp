#include <iostream>
#include <thread>
#include <atomic>

#include "ChatClient.h"

#pragma comment(lib, "Ws2_32.lib")

int main(int argc, char* argv[])
{
  // Loopback localhost by default
  const char* ipadd = "127.0.0.1";
  const char* port = "27015";

  if (argc > 1) port = argv[1];
  if (argc > 2) ipadd = argv[2];

  try 
  {
    ChatClient cli(ipadd, port);

    // Start a tiny input thread: read lines and queue them for sending.
    std::atomic<bool> inputRun{ true };
    std::thread input([&] 
      {
      std::string line;
      while (inputRun && std::getline(std::cin, line)) 
      {
        if (line == "/quit") 
        {
          cli.Stop();
          break;
        }
        cli.EnqueueSend(line);
      }
      });

    cli.Start();

    // Ensure input thread finishes
    inputRun = false;
    if (input.joinable()) 
      input.join();
  }
  catch (const std::exception& ex) {
    std::cout << "Exception occurred: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
