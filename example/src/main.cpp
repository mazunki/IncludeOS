#include "kernel/memory.hpp"
#include <os>
#include <service>

void Service::start(const std::string& args){
  std::println("Virtual mappings:");
  for (const auto& entry : os::mem::vmmap())
      std::println(" {}", entry.second.to_string());

  printf("Service done. Shutting down...\n");

  os::shutdown();
}
