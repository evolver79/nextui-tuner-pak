// Whether the Wi-Fi can be used, read on its own thread (iw takes a few ms, too long to wait for on the drawing thread):
// at once when the kernel says the network changed (netlink: wlan0 up or down, an IP address got or lost), and every
// 3 s besides. Usable means a link to the router and an IPv4 address: joined but still waiting on DHCP counts as off,
// as no channel can be reached yet. Every channel is streamed, so without it Tuner waits (see app.c).
#include "wifi.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static SDL_atomic_t down;   // 0 until the first reading says otherwise

static int has_ip(void) {   // wlan0 has an IPv4 address (DHCP done)
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return 1;   // can't tell: don't hold Tuner back over it
  struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
  snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "wlan0");
  ifr.ifr_addr.sa_family = AF_INET;
  int ok = ioctl(fd, SIOCGIFADDR, &ifr) == 0;
  close(fd);
  return ok;
}

static void read_once(void) {
  FILE *f = popen("/usr/sbin/iw dev wlan0 link 2>/dev/null", "r");
  if (!f) return;
  char line[160]; int linked = 0;
  while (fgets(line, sizeof line, f)) if (!strncmp(line, "Connected to", 12)) linked = 1;
  pclose(f);
  SDL_AtomicSet(&down, !(linked && has_ip()));
}

static int open_netlink(void) {   // link and IPv4 address changes; -1: no events, polling only
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) return -1;
  struct sockaddr_nl sa; memset(&sa, 0, sizeof sa);
  sa.nl_family = AF_NETLINK;
  sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
  return fd;
}

static int run(void *ud) {
  (void)ud;
  int nl = open_netlink();
  for (;;) {
    read_once();
    if (nl < 0) { SDL_Delay(3000); continue; }
    struct pollfd p = { nl, POLLIN, 0 };
    if (poll(&p, 1, 3000) > 0) {
      SDL_Delay(200);   // changes come in bursts (link up, then the address): read once they've settled
      char buf[4096];
      while (recv(nl, buf, sizeof buf, MSG_DONTWAIT) > 0) {}   // what changed doesn't matter: iw is asked again
    }
  }
  return 0;
}

void wifi_start(void) {
  read_once();   // the first reading before Tuner decides whether to load (a few ms)
  SDL_Thread *t = SDL_CreateThread(run, "wifi-up", NULL);
  if (t) SDL_DetachThread(t);
}

int wifi_up(void) { return !SDL_AtomicGet(&down); }
