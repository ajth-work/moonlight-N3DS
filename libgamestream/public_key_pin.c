#include "public_key_pin.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <string.h>

bool derive_public_key_pin(const char *pem, char pin[53]) {
  BIO *bio = NULL;
  X509 *x509 = NULL;
  EVP_PKEY *key = NULL;
  unsigned char *der = NULL;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  int der_length;
  bool ok = false;
  if (pem == NULL || pin == NULL) return false;
  bio = BIO_new_mem_buf(pem, -1);
  if (bio == NULL) goto cleanup;
  x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  if (x509 == NULL) goto cleanup;
  key = X509_get_pubkey(x509);
  if (key == NULL) goto cleanup;
  der_length = i2d_PUBKEY(key, &der);
  if (der_length <= 0 || der == NULL) goto cleanup;
  if (SHA256(der, (size_t)der_length, digest) == NULL) goto cleanup;
  memcpy(pin, "sha256//", 8);
  if (EVP_EncodeBlock((unsigned char *)pin + 8, digest, sizeof(digest)) != 44)
    goto cleanup;
  pin[52] = '\0';
  ok = true;
cleanup:
  OPENSSL_free(der);
  EVP_PKEY_free(key);
  X509_free(x509);
  BIO_free(bio);
  return ok;
}

bool valid_public_key_pin(const char *pin) {
  size_t i;
  if (pin == NULL || strlen(pin) != 52 || strncmp(pin, "sha256//", 8) != 0 ||
      pin[51] != '=') return false;
  for (i = 8; i < 52; ++i) {
    const unsigned char ch = (unsigned char)pin[i];
    const bool alpha_numeric = (ch >= 'A' && ch <= 'Z') ||
                               (ch >= 'a' && ch <= 'z') ||
                               (ch >= '0' && ch <= '9');
    if (!(alpha_numeric || ch == '+' || ch == '/' ||
          (ch == '=' && i == 51)) ||
        (i == 50 && ch == '=')) return false;
  }
  return true;
}
