#pragma once
// Whether the Wi-Fi can be used: a link to the router and an IP address. Read on its own thread, at once when the
// kernel says the network changed and every 3 s besides.
void wifi_start(void);
int wifi_up(void);   // 1 while usable, and before the first reading (or with no thread, in screenshot mode)
