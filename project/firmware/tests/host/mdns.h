#pragma once
#include "esp_err.h"
#include <stddef.h>
esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *);
esp_err_t mdns_instance_name_set(const char *);
esp_err_t mdns_service_add(const char *, const char *, const char *, unsigned,
                           const void *, size_t);
void mdns_free(void);
