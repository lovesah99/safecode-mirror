/*
 * RUN: sh test.sh %s
 * XFAIL: *
 */

/* Call strchr() on an unterminated string, and the character
 * to find is inside the string. */

#include <string.h>
#include <stdio.h>

int main()
{
  char a[1000];
  memset(a, 'a', 1000);
  printf("%p\n", strchr(a, 'a'));
  return 0;
}
