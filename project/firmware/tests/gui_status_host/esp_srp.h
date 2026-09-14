#pragma once
#include "esp_err.h"
esp_err_t esp_srp_gen_salt_verifier(const char *, int, const char *, int,
                                  char **, int, char **, int *);
