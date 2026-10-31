#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
bool derive_public_key_pin(const char *certificate_pem, char pin[53]);
bool valid_public_key_pin(const char *pin);
#ifdef __cplusplus
}
#endif
