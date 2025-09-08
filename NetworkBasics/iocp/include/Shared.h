#pragma once

#include "winsock2.h"
#include <string>

/// <summary>
/// Discriminated OVERLAPPED to know which operation completed.
/// </summary>
struct OvEx
{
  OVERLAPPED ov{};
  enum class Kind : uint8_t { Accept, Recv, Send } kind{};
};