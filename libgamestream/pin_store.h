#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
bool host_pin_store_save(const char *address, uint16_t http_port,
                         const char *pin);
bool host_pin_store_save_after_pairing(const char *address, uint16_t http_port,
                                       const char *pin, bool pairing_succeeded);
bool host_pin_store_load(const char *address, uint16_t http_port, char *pin,
                         size_t pin_size);
void host_pin_store_remove(const char *address, uint16_t http_port);
bool host_upload_consent_get(const char *address, uint16_t http_port,
                             bool *known, bool *enabled);
bool host_upload_consent_set(const char *address, uint16_t http_port, bool enabled);
#ifdef __cplusplus
}
#endif
