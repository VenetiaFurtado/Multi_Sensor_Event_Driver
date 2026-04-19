/**
 * @file user-app.c
 * @brief User application that tests out poll() operation
 *
 * @author Venetia Furtado
 * @date 2026-04-19
 *
 * References:
 * 1. https://chatgpt.com/share/69e51403-a0bc-83e8-9e37-528524efaf44
 * 2. https://chatgpt.com/share/69e3bede-534c-83e8-99f7-7d7e060bdeaf
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#define NUM_FDS 2
#define NUM_DEVICES 2

int main(void)
{
   struct pollfd fds[NUM_FDS];
   int high_temp;
   int low_temp;
   char *device_nodes[NUM_DEVICES] = {"/dev/high_temperature_event",
                                      "/dev/low_temperature_event"};

   // Open device nodes
   for (int i = 0; i < NUM_DEVICES; i++)
   {
      char buffer[100];
      fds[i].fd = open(device_nodes[i], O_RDONLY | O_NONBLOCK);
      if (fds[i].fd < 0)
      {
         snprintf(buffer, sizeof(buffer), "Error opening %s\n\r", device_nodes[i]);
         perror(buffer);
         return 1;
      }
      fds[i].events = POLLIN; // Tell poll() what events we care about
   }

   printf("Monitoring device FDs...\n");

   while (1)
   {
      int ret = poll(fds, NUM_FDS, -1);

      if (ret < 0)
      {
         perror("poll");
         break;
      }

      if (ret == 0)
      {
         printf("Event generation disabled\n");
         continue;
      }

      // High temperature event
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

      // Low temperature event
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

      // Handle errors/hangups
      for (int i = 0; i < NUM_DEVICES; i++)
      {
         if (fds[i].revents & (POLLERR | POLLHUP))
         {
            printf("Device %s error/hangup\n", device_nodes[i]);
         }
      }
   }

   // close fds
   for (int i = 0; i < NUM_DEVICES; i++)
   {
      close(fds[i].fd);
   }

   return 0;
}