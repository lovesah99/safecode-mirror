// RUN: test.sh -c -e -t %t %s

// strpbrk() searching on an unterminated string.

#include <string.h>

int main()
{
  char a[100];
  memset(a, 'a', 100);
  strpbrk(a, "ab");
  return 0;
}
