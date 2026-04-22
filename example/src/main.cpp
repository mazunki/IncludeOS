#include <sys/mman.h>
#include <print>
#include <cstdlib>

int main() {
  constexpr size_t page_sz = 4096;

  volatile int* p = (volatile int*) mmap(nullptr, page_sz*16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (p == MAP_FAILED) {
    std::println("mmap failed");
    return 1;
  }

  std::printf("before='%d'\n", p[0]);
  p[0] = 42;
  std::printf("after='%d'\n", p[0]);

  if (mprotect((void*)p, 4000, PROT_READ) != 0) {
    std::println("mprotect failed");
    return 1;
  }

  std::printf("protected='%d'\n", p[0]);

  std::println("about to write to read-only page; this should fault");
  p[page_sz+2] = 69;

  std::printf("BUG: write succeeded, value='%d'\n", p[0]);
  return 0;
}
