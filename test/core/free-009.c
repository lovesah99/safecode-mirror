// RUN: test.sh -e -t %t %s
//
// TEST: free-008
// XFAIL: darwin
//
// Description:
//  Test invalid memory deallocations
//

#include <stdio.h>
#include <stdlib.h>

int
main (int argc, char ** argv) {
  free (argv[0]);
  return 0;
}

