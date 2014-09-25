#include <stdio.h>

#define TEST(COND) \
         if (!(COND)) { \
            fprintf(stderr, "%s:%d: TEST failed\n", __FILE__, __LINE__); \
            exit(1); \
         }

int main(void)
{
   printf("Running iqueue test\n");

   return 0;
}
