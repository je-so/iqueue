#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST(COND) \
         if (!(COND)) { \
            fprintf(stderr, "%s:%d: TEST failed\n", __FILE__, __LINE__); \
            exit(1); \
         }

#ifdef __linux
/* 
 * Uses GNU malloc_stats extension.
 * This function returns internal collected statistics
 * about memory usage so implementing a thin wrapper
 * for malloc is not necessary.
 *
 * Currently it is only tested on Linux platforms.
 *
 * What malloc_stats does:
 * The GNU C lib function malloc_stats writes textual information
 * to standard err in the following form:
 * > Arena 0:
 * > system bytes     =     135168
 * > in use bytes     =      15000
 * > Total (incl. mmap):
 * > system bytes     =     135168
 * > in use bytes     =      15000
 * > max mmap regions =          0
 * > max mmap bytes   =          0
 *
 * How it is implemented:
 * This function redirects standard error file descriptor
 * to a pipe and reads the content of the pipe into a buffer.
 * It scans backwards until the third last line is
 * reached ("in use bytes") and then returns the converted
 * number at the end of the line as result.
 * */
int allocated_bytes(/*out*/size_t * nrofbytes)
{
   int err;
   int fd     = -1;
   int pfd[2] = { -1, -1 };

   if (pipe(pfd)) {
      err = errno;
      perror("pipe");
      goto ONERR;
   }
   
   int flags = fcntl(pfd[0], F_GETFL);
   fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

   fd = dup(STDERR_FILENO);
   if (fd == -1) {
      err = errno;
      perror("dup");
      goto ONERR;
   }

   if (-1 == dup2(pfd[1], STDERR_FILENO)) {
      err = errno;
      perror("dup2");
      goto ONERR;
   }

   malloc_stats();

   char     buffer[256/*must be even*/];
   ssize_t  len    = 0;

   len = read(pfd[0], buffer, sizeof(buffer));
   if (len < 0) {
      err = errno;
      perror("read");
      goto ONERR;
   }

   while (sizeof(buffer) == len) {
      memcpy(buffer, &buffer[sizeof(buffer)/2], sizeof(buffer)/2);
      len = read(pfd[0], &buffer[sizeof(buffer)/2], sizeof(buffer)/2);
      if (len < 0) {
         err = errno;
         if (err == EWOULDBLOCK || err == EAGAIN) {
            len = sizeof(buffer)/2;
            break;
         }
         perror("read");
         goto ONERR;
      }
      len += (int)sizeof(buffer)/2;
   }

   // remove last two lines
   for (unsigned i = 3; len > 0; --len) {
      i -= (buffer[len] == '\n');
      if (!i) break;
   }

   while (len > 0
          && buffer[len-1] >= '0'
          && buffer[len-1] <= '9') {
      -- len;
   }

   size_t used_bytes = 0;
   if (  len > 0
         && buffer[len] >= '0'
         && buffer[len] <= '9'  ) {
      sscanf(&buffer[len], "%zu", &used_bytes);
   }

   if (-1 == dup2(fd, STDERR_FILENO)) {
      err = errno;
      perror("dup2");
      goto ONERR;
   }

   (void) close(fd);
   (void) close(pfd[0]);
   (void) close(pfd[1]);

   *nrofbytes = used_bytes;

   return 0;
ONERR:
   err = errno;
   if (pfd[0] != -1) close(pfd[0]);
   if (pfd[1] != -1) close(pfd[1]);
   if (fd != -1) {
      dup2(fd, STDERR_FILENO);
      close(fd);
   }
   return err;
}
#else

// Implement allocated_bytes for your operating system here !
int allocated_bytes(size_t * nrofbytes)
{
   *nrofbytes = 0;
   return 0;
}

#endif

int main(void)
{
   size_t nrofbytes;
   size_t nrofbytes2;
   
   printf("Running iqueue test\n");

   TEST(0 == allocated_bytes(&nrofbytes));


   TEST(0 == allocated_bytes(&nrofbytes2));

   if (nrofbytes != nrofbytes2) {
      printf("Memory leak of '%ld' bytes!\n", (long) (nrofbytes2 - nrofbytes));
      return 1;
   }

   return 0;
}
