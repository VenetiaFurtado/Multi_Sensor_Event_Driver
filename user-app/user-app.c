#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#define NUM_FDS 2

int main(void)
{
   struct pollfd fds[NUM_FDS];
   int high_temp;
   int low_temp;

   /* Open device nodes */
   fds[0].fd = open("/dev/high_temperature_event", O_RDONLY | O_NONBLOCK);
   if (fds[0].fd < 0)
   {
      perror("open /dev/high_temperature_event");
      return 1;
   }

   fds[1].fd = open("/dev/low_temperature_event", O_RDONLY | O_NONBLOCK);
   if (fds[1].fd < 0)
   {
      perror("open /dev/low_temperature_event");
      close(fds[0].fd);
      return 1;
   }

   /* Tell poll() what events we care about */
   fds[0].events = POLLIN;
   fds[1].events = POLLIN;

   printf("Monitoring device FDs...\n");

   while (1)
   {
      int ret = poll(fds, NUM_FDS, -1); /* wait forever */

      if (ret < 0)
      {
         perror("poll");
         break;
      }

      /* High temperature event */
      if (fds[0].revents & POLLIN)
      {
         ssize_t n = read(fds[0].fd, &high_temp, sizeof(high_temp));
         if (n > 0)
         {
            printf("HIGH TEMP EVENT: %d\n", high_temp);
         }
         else if (n < 0 && errno != EAGAIN)
         {
            perror("read high_temperature_event");
         }
      }

      /* Low temperature event */
      if (fds[1].revents & POLLIN)
      {
         ssize_t n = read(fds[1].fd, &low_temp, sizeof(low_temp));
         if (n > 0)
         {
            printf("LOW TEMP EVENT: %d\n", low_temp);
         }
         else if (n < 0 && errno != EAGAIN)
         {
            perror("read low_temperature_event");
         }
      }

      /* Handle errors/hangups */
      if (fds[0].revents & (POLLERR | POLLHUP))
         printf("high_temperature_event error/hangup\n");

      if (fds[1].revents & (POLLERR | POLLHUP))
         printf("low_temperature_event error/hangup\n");
   }

   close(fds[0].fd);
   close(fds[1].fd);

   return 0;
}