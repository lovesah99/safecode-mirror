/*
 * RUN: test.sh -e -t %t %s
 */

/* strrchr() with an unterminated string searching for a character not
 * that is found in the string. */

#include <stdio.h>
#include <string.h>

int main()
{
  char a[1000];
  memset(a, 'a', 1000);
  strrchr(a, 'a');
  return 0;
}
