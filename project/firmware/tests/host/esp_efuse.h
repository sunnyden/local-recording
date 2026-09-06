#pragma once
#include <stdbool.h>
typedef int esp_efuse_block_t;
typedef int esp_efuse_purpose_t;
#define EFUSE_BLK_KEY0 4
#define ESP_EFUSE_KEY_PURPOSE_HMAC_UP 8
esp_efuse_purpose_t esp_efuse_get_key_purpose(esp_efuse_block_t);
bool esp_efuse_get_key_dis_read(esp_efuse_block_t);
bool esp_efuse_get_key_dis_write(esp_efuse_block_t);
bool esp_efuse_get_keypurpose_dis_write(esp_efuse_block_t);
