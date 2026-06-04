#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#include <stdbool.h>

void wifi_manager_init(void);
void wifi_manager_wait_connected(void);
bool wifi_manager_is_connected(void);

#endif /* WIFI_MANAGER_H_ */
