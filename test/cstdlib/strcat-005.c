/*
 * RUN: sh test.sh %s
 * XFAIL: *
 */

/* Concatenation destination and source overlap. */

#include <string.h>

int main()
{
  char buf[10] = "afg";
  strcat(&buf[2], buf);
  printf("%s\n", buf);
  return 0;
}