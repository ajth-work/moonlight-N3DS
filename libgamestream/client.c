/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "http.h"
#include "xml.h"
#include "mkcert.h"
#include "client.h"
#include "errors.h"
#include "limits.h"
#include "diagnostics.h"
#include "pin_store.h"
#include "public_key_pin.h"

#include <Limelight.h>

#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#ifdef __3DS__
#include "uuid.h"
#else
#include <uuid/uuid.h>
#endif
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#define UNIQUE_FILE_NAME "uniqueid.dat"
#define P12_FILE_NAME "client.p12"

#define UNIQUEID_BYTES 8
#define UNIQUEID_CHARS (UNIQUEID_BYTES*2)

static char unique_id[UNIQUEID_CHARS+1];
static X509 *cert;
static char cert_hex[8192];
static EVP_PKEY *privateKey;
static char client_certificate_path[PATH_MAX];
static char client_key_path[PATH_MAX];
static char client_cert_source[DIAG_CERT_SOURCE_MAX] = "unknown";

const char* gs_error;

#define LEN_AS_HEX_STR(x) ((x) * 2 + 1)
#define SIZEOF_AS_HEX_STR(x) LEN_AS_HEX_STR(sizeof(x))

#define UUID_STRLEN 37

static int mkdirtree(const char* directory) {
  char buffer[PATH_MAX];
  char* p = buffer;

  // The passed in string could be a string literal
  // so we must copy it first
  strncpy(p, directory, PATH_MAX - 1);
  buffer[PATH_MAX - 1] = '\0';

  while (*p != 0) {
    // Find the end of the path element
    do {
      p++;
    } while (*p != 0 && *p != '/');

    char oldChar = *p;
    *p = 0;

    // Create the directory if it doesn't exist already
    if (mkdir(buffer, 0775) == -1 && errno != EEXIST) {
        return -1;
    }

    *p = oldChar;
  }

  return 0;
}

static int load_unique_id(const char* keyDirectory) {
  char uniqueFilePath[PATH_MAX];
  snprintf(uniqueFilePath, PATH_MAX, "%s/%s", keyDirectory, UNIQUE_FILE_NAME);

  FILE *fd = fopen(uniqueFilePath, "r");
  if (fd == NULL || fread(unique_id, UNIQUEID_CHARS, 1, fd) != UNIQUEID_CHARS) {
    snprintf(unique_id,UNIQUEID_CHARS+1,"0123456789ABCDEF");

    if (fd)
      fclose(fd);
    fd = fopen(uniqueFilePath, "w");
    if (fd == NULL)
      return GS_FAILED;

    fwrite(unique_id, UNIQUEID_CHARS, 1, fd);
  }
  fclose(fd);
  unique_id[UNIQUEID_CHARS] = 0;

  return GS_OK;
}

static int load_cert(const char* keyDirectory) {
  char p12FilePath[PATH_MAX];
  bool generated = false;
  snprintf(client_certificate_path, sizeof(client_certificate_path), "%s/%s",
           keyDirectory, CERTIFICATE_FILE_NAME);

  snprintf(client_key_path, sizeof(client_key_path), "%s/%s", keyDirectory,
           KEY_FILE_NAME);

  FILE *fd = fopen(client_certificate_path, "r");
  if (fd == NULL) {
    printf("Generating certificate...");
    CERT_KEY_PAIR cert = mkcert_generate();
    printf("done\n");

    snprintf(p12FilePath, PATH_MAX, "%s/%s", keyDirectory, P12_FILE_NAME);

    mkcert_save(client_certificate_path, p12FilePath, client_key_path, cert);
    mkcert_free(cert);
    generated = true;
    fd = fopen(client_certificate_path, "r");
  }

  if (fd == NULL) {
    gs_error = "Can't open certificate file";
    return GS_FAILED;
  }

  if (!(cert = PEM_read_X509(fd, NULL, NULL, NULL))) {
    gs_error = "Error loading cert into memory";
    return GS_FAILED;
  }

  rewind(fd);

  int c;
  int length = 0;
  while ((c = fgetc(fd)) != EOF) {
    sprintf(cert_hex + length, "%02x", c);
    length += 2;
  }
  cert_hex[length] = 0;

  fclose(fd);

  fd = fopen(client_key_path, "r");
  if (fd == NULL) {
    gs_error = "Error loading key into memory";
    return GS_FAILED;
  }

  PEM_read_PrivateKey(fd, &privateKey, NULL, NULL);
  fclose(fd);

  snprintf(client_cert_source, sizeof(client_cert_source), "%s",
           generated ? "new" : "file");
  diag_cert_checkpoint(DIAG_CERT_STARTUP, cert, client_cert_source,
                       client_certificate_path, client_key_path,
                       generated ? p12FilePath : NULL);

  return GS_OK;
}

static int load_serverinfo(PSERVER_DATA server, bool https) {
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char url[4096];
  int ret = GS_INVALID;
  char *pairedText = NULL;
  char *currentGameText = NULL;
  char *stateText = NULL;
  char *serverCodecModeSupportText = NULL;
  char *httpsPortText = NULL;
  const char *phase = https ? "connect.serverinfo.https" : "connect.serverinfo.http";

  diag_set_phase(phase);

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);

  snprintf(url, sizeof(url), "%s://%s:%d/serverinfo?uniqueid=%s&uuid=%s",
    https ? "https" : "http", server->serverInfo.address, https ? server->httpsPort : server->httpPort, unique_id, uuid_str);

  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (http_request(url, data) != GS_OK) {
    ret = GS_IO_ERROR;
    goto cleanup;
  }

  if (xml_status(data->memory, data->size) == GS_ERROR) {
    ret = GS_ERROR;
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "currentgame", &currentGameText) != GS_OK) {
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "PairStatus", &pairedText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "appversion", (char**) &server->serverInfo.serverInfoAppVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "state", &stateText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "ServerCodecModeSupport", &serverCodecModeSupportText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "gputype", &server->gpuType) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "GsVersion", &server->gsVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "GfeVersion", (char**) &server->serverInfo.serverInfoGfeVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "HttpsPort", &httpsPortText) != GS_OK)
    goto cleanup;

  if (xml_modelist(data->memory, data->size, &server->modes) != GS_OK)
    goto cleanup;

  // These fields are present on all version of GFE that this client supports
  if (!strlen(currentGameText) || !strlen(pairedText) || !strlen(server->serverInfo.serverInfoAppVersion) || !strlen(stateText))
    goto cleanup;

  server->paired = pairedText != NULL && strcmp(pairedText, "1") == 0;
  server->currentGame = currentGameText == NULL ? 0 : atoi(currentGameText);
  server->serverInfo.serverCodecModeSupport = serverCodecModeSupportText == NULL ? SCM_H264 : atoi(serverCodecModeSupportText);
  server->serverMajorVersion = atoi(server->serverInfo.serverInfoAppVersion);
  server->isNvidiaSoftware = strstr(stateText, "MJOLNIR") != NULL;

  server->httpsPort = atoi(httpsPortText);
  if (!server->httpsPort)
    server->httpsPort = 47984;

  if (strstr(stateText, "_SERVER_BUSY") == NULL) {
    // After GFE 2.8, current game remains set even after streaming
    // has ended. We emulate the old behavior by forcing it to zero
    // if streaming is not active.
    server->currentGame = 0;
  }
  ret = GS_OK;

  cleanup:
  if (data != NULL)
    http_free_data(data);

  if (pairedText != NULL)
    free(pairedText);

  if (currentGameText != NULL)
    free(currentGameText);

  if (serverCodecModeSupportText != NULL)
    free(serverCodecModeSupportText);

  if (httpsPortText != NULL)
    free(httpsPortText);

  diag_phase_outcome(phase, ret == GS_OK ? "success" : "failed", ret);

  return ret;
}

static int load_server_status(PSERVER_DATA server) {
  int ret;
  int i;

  diag_set_phase("connect.server_status");

  /* Fetch the HTTPS port if we don't have one yet */
  if (!server->httpsPort) {
    ret = load_serverinfo(server, false);
    if (ret != GS_OK) {
      diag_phase_outcome("connect.server_status", "failed", ret);
      return ret;
    }
  }

  // Modern GFE versions don't allow serverinfo to be fetched over HTTPS if the client
  // is not already paired. Since we can't pair without knowing the server version, we
  // make another request over HTTP if the HTTPS request fails. We can't just use HTTP
  // for everything because it doesn't accurately tell us if we're paired.
  ret = GS_INVALID;
  for (i = 0; i < 2 && ret != GS_OK; i++) {
    ret = load_serverinfo(server, i == 0);
  }

  if (ret == GS_OK && !server->unsupported) {
    if (server->serverMajorVersion > MAX_SUPPORTED_GFE_VERSION) {
      gs_error = "Ensure you're running the latest version of Moonlight Embedded or downgrade GeForce Experience and try again";
      ret = GS_UNSUPPORTED_VERSION;
    } else if (server->serverMajorVersion < MIN_SUPPORTED_GFE_VERSION) {
      gs_error = "Moonlight Embedded requires a newer version of GeForce Experience. Please upgrade GFE on your PC and try again.";
      ret = GS_UNSUPPORTED_VERSION;
    }
  }

  diag_phase_outcome("connect.server_status", ret == GS_OK ? "success" : "failed", ret);
  return ret;
}

static void bytes_to_hex(unsigned char *in, char *out, size_t len) {
  for (int i = 0; i < len; i++) {
    sprintf(out + i * 2, "%02x", in[i]);
  }
  out[len * 2] = 0;
}

static void hex_to_bytes(const char *in, unsigned char* out, size_t len) {
  for (int count = 0; count < len; count += 2) {
    sscanf(&in[count], "%2hhx", &out[count / 2]);
  }
}

static int sign_it(const char *msg, size_t mlen, unsigned char **sig, size_t *slen, EVP_PKEY *pkey) {
  int result = GS_FAILED;

  *sig = NULL;
  *slen = 0;

  EVP_MD_CTX *ctx = EVP_MD_CTX_create();
  if (ctx == NULL)
    return GS_FAILED;

  int rc = EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey);
  if (rc != 1)
    goto cleanup;

  rc = EVP_DigestSignUpdate(ctx, msg, mlen);
  if (rc != 1)
    goto cleanup;

  size_t req = 0;
  rc = EVP_DigestSignFinal(ctx, NULL, &req);
  if (rc != 1 || !(req > 0))
    goto cleanup;

  *sig = OPENSSL_malloc(req);
  if (*sig == NULL)
    goto cleanup;

  *slen = req;
  rc = EVP_DigestSignFinal(ctx, *sig, slen);
  if (rc != 1 || req != *slen)
    goto cleanup;

  result = GS_OK;

  cleanup:
  EVP_MD_CTX_destroy(ctx);
  ctx = NULL;

  return result;
}

static bool verifySignature(const char *data, int dataLength, char *signature, int signatureLength, const char *cert) {
    X509* x509;
    BIO* bio = BIO_new(BIO_s_mem());
    BIO_puts(bio, cert);
    x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);

    BIO_free(bio);

    if (!x509) {
        return false;
    }

    EVP_PKEY* pubKey = X509_get_pubkey(x509);
    EVP_MD_CTX *mdctx = NULL;
    mdctx = EVP_MD_CTX_create();
    EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubKey);
    EVP_DigestVerifyUpdate(mdctx, data, dataLength);
    int result = EVP_DigestVerifyFinal(mdctx, signature, signatureLength);

    X509_free(x509);
    EVP_PKEY_free(pubKey);
    EVP_MD_CTX_destroy(mdctx);

    return result > 0;
}

static void encrypt(const unsigned char *plaintext, int plaintextLen, const unsigned char *key, unsigned char *ciphertext) {
  EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();

  EVP_EncryptInit(cipher, EVP_aes_128_ecb(), key, NULL);
  EVP_CIPHER_CTX_set_padding(cipher, 0);

  int ciphertextLen = 0;
  EVP_EncryptUpdate(cipher, ciphertext, &ciphertextLen, plaintext, plaintextLen);

  EVP_CIPHER_CTX_free(cipher);
}

static void decrypt(const unsigned char *ciphertext, int ciphertextLen, const unsigned char *key, unsigned char *plaintext) {
  EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();

  EVP_DecryptInit(cipher, EVP_aes_128_ecb(), key, NULL);
  EVP_CIPHER_CTX_set_padding(cipher, 0);

  int plaintextLen = 0;
  EVP_DecryptUpdate(cipher, plaintext, &plaintextLen, ciphertext, ciphertextLen);

  EVP_CIPHER_CTX_free(cipher);
}

static int gs_unpair_internal(PSERVER_DATA server, bool clear_pin) {
  int ret = GS_OK;
  char url[4096];
  diag_set_phase("pair.unpair_request");
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    diag_phase_outcome("pair.unpair_request", "failed", GS_OUT_OF_MEMORY);
    return GS_OUT_OF_MEMORY;
  }

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "http://%s:%u/unpair?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str);
  ret = http_request(url, data);

  http_free_data(data);
  if (ret == GS_OK) server->paired = false;
  if (ret == GS_OK && clear_pin)
    host_pin_store_remove(server->serverInfo.address, server->httpPort);
  diag_phase_outcome("pair.unpair_request", ret == GS_OK ? "success" : "failed", ret);
  return ret;
}

int gs_unpair(PSERVER_DATA server) {
  return gs_unpair_internal(server, true);
}

int gs_request_pinned(PSERVER_DATA server, char *url, PHTTP_DATA data,
                      const char *request_body, size_t body_size) {
  char expected_prefix[1024];
  char pin[53];
  int prefix_length;
  if (server == NULL || !server->paired || server->serverInfo.address == NULL ||
      url == NULL || data == NULL ||
      (request_body == NULL && body_size != 0) || body_size > 256U * 1024U) return GS_FAILED;
  prefix_length = snprintf(expected_prefix, sizeof(expected_prefix),
                           "https://%s:%u/", server->serverInfo.address,
                           server->httpsPort);
  if (prefix_length < 0 || (size_t)prefix_length >= sizeof(expected_prefix) ||
      strncmp(url, expected_prefix, (size_t)prefix_length) != 0 ||
      !host_pin_store_load(server->serverInfo.address, server->httpPort, pin,
                           sizeof(pin))) {
    gs_error = "No valid paired host public-key pin for this endpoint";
    return GS_FAILED;
  }
  return request_body != NULL
      ? http_request_post_pinned(url, data, pin, request_body, body_size)
      : http_request_pinned(url, data, pin);
}

int gs_pair(PSERVER_DATA server, char* pin) {
  int ret = GS_OK;
  char* result = NULL;
  size_t url_max_len = 16384;
  char* url = malloc(url_max_len);
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char* plaincert = NULL;
  char public_key_pin[53];
  char* challenge_response = NULL;
  char* pairing_secret = NULL;
  char* client_pairing_secret = NULL;
  char* client_pairing_secret_hex = NULL;
  PHTTP_DATA data = NULL;
  const char *current_phase = "pair.precheck";
#define SET_PAIR_PHASE(name) do { current_phase = (name); diag_set_phase(current_phase); } while (0)

  SET_PAIR_PHASE("pair.precheck");

  if (server->paired) {
    gs_error = "Already paired";
    ret = GS_WRONG_STATE;
    goto cleanup;
  }
  diag_phase_outcome(current_phase, "success", GS_OK);
  unsigned char salt_data[16];
  char salt_hex[SIZEOF_AS_HEX_STR(salt_data)];
  RAND_bytes(salt_data, sizeof(salt_data));
  bytes_to_hex(salt_data, salt_hex, sizeof(salt_data));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  SET_PAIR_PHASE("pair.request_server_certificate");
  /* cert_hex is the PEM file bytes hex-encoded for this clientcert payload. */
  diag_cert_checkpoint(DIAG_CERT_PAIRING, cert, client_cert_source,
                       client_certificate_path, client_key_path, NULL);
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&phrase=getservercert&salt=%s&clientcert=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, salt_hex, cert_hex);
  data = http_create_data();
  if (data == NULL) {
    ret = GS_OUT_OF_MEMORY;
    diag_phase_outcome(current_phase, "failed", ret);
    diag_phase_outcome("pair", "failed", ret);
    free(url);
    return ret;
  }
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;
  diag_phase_outcome(current_phase, "success", ret);

  SET_PAIR_PHASE("pair.parse_server_certificate");
  if ((ret = xml_status(data->memory, data->size) != GS_OK))
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if ((ret = xml_search(data->memory, data->size, "plaincert", &result)) != GS_OK)
    goto cleanup;


  size_t plaincertlen = strlen(result)/2;
  plaincert = malloc(plaincertlen + 1);
  hex_to_bytes(result, plaincert, plaincertlen*2);
  plaincert[plaincertlen] = 0;
  diag_phase_outcome(current_phase, "success", GS_OK);

  SET_PAIR_PHASE("pair.create_client_challenge");
  unsigned char salt_pin[sizeof(salt_data) + 4];
  unsigned char aes_key[32]; // Must fit SHA256
  memcpy(salt_pin, salt_data, sizeof(salt_data));
  memcpy(salt_pin+sizeof(salt_data), pin, 4);

  int hash_length = server->serverMajorVersion >= 7 ? 32 : 20;
  if (server->serverMajorVersion >= 7)
    SHA256(salt_pin, sizeof(salt_pin), aes_key);
  else
    SHA1(salt_pin, sizeof(salt_pin), aes_key);

  unsigned char challenge_data[16];
  unsigned char challenge_enc[sizeof(challenge_data)];
  char challenge_hex[SIZEOF_AS_HEX_STR(challenge_enc)];
  RAND_bytes(challenge_data, sizeof(challenge_data));
  encrypt(challenge_data, sizeof(challenge_data), aes_key, challenge_enc);
  bytes_to_hex(challenge_enc, challenge_hex, sizeof(challenge_enc));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  diag_phase_outcome(current_phase, "success", GS_OK);
  SET_PAIR_PHASE("pair.request_server_challenge");
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&clientchallenge=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, challenge_hex);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;
  diag_phase_outcome(current_phase, "success", ret);

  SET_PAIR_PHASE("pair.parse_server_challenge");
  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size) != GS_OK))
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "challengeresponse", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  char challenge_response_data_enc[64];
  char challenge_response_data[sizeof(challenge_response_data_enc)];

  if (strlen(result) / 2 > sizeof(challenge_response_data_enc)) {
    gs_error = "Server challenge response too big";
    ret = GS_FAILED;
    goto cleanup;
  }

  hex_to_bytes(result, challenge_response_data_enc, strlen(result));
  decrypt(challenge_response_data_enc, sizeof(challenge_response_data_enc), aes_key, challenge_response_data);
  diag_phase_outcome(current_phase, "success", GS_OK);

  SET_PAIR_PHASE("pair.create_client_challenge_response");
  char client_secret_data[16];
  RAND_bytes(client_secret_data, sizeof(client_secret_data));

  const ASN1_BIT_STRING *asnSignature;
  X509_get0_signature(&asnSignature, NULL, cert);

  challenge_response = malloc(16 + asnSignature->length + sizeof(client_secret_data));
  char challenge_response_hash[32];
  char challenge_response_hash_enc[sizeof(challenge_response_hash)];
  char challenge_response_hex[SIZEOF_AS_HEX_STR(challenge_response_hash_enc)];
  memcpy(challenge_response, challenge_response_data + hash_length, 16);
  memcpy(challenge_response + 16, asnSignature->data, asnSignature->length);
  memcpy(challenge_response + 16 + asnSignature->length, client_secret_data, sizeof(client_secret_data));
  if (server->serverMajorVersion >= 7)
    SHA256(challenge_response, 16 + asnSignature->length + sizeof(client_secret_data), challenge_response_hash);
  else
    SHA1(challenge_response, 16 + asnSignature->length + sizeof(client_secret_data), challenge_response_hash);

  encrypt(challenge_response_hash, sizeof(challenge_response_hash), aes_key, challenge_response_hash_enc);
  bytes_to_hex(challenge_response_hash_enc, challenge_response_hex, sizeof(challenge_response_hash_enc));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  diag_phase_outcome(current_phase, "success", GS_OK);
  SET_PAIR_PHASE("pair.request_pairing_secret");
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&serverchallengeresp=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, challenge_response_hex);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;
  diag_phase_outcome(current_phase, "success", ret);

  SET_PAIR_PHASE("pair.verify_server_pairing_secret");
  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size) != GS_OK))
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "pairingsecret", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  size_t pairing_secret_len = strlen(result) / 2;
  if (pairing_secret_len <= 16) {
    ret = GS_INVALID;
    goto cleanup;
  }

  pairing_secret = malloc(pairing_secret_len);
  hex_to_bytes(result, pairing_secret, pairing_secret_len*2);
  if (!verifySignature(pairing_secret, 16, pairing_secret+16, pairing_secret_len-16, plaincert)) {
    gs_error = "MITM attack detected";
    ret = GS_FAILED;
    goto cleanup;
  }
  if (!derive_public_key_pin(plaincert, public_key_pin)) {
    gs_error = "Failed to derive paired host public-key pin";
    ret = GS_FAILED;
    goto cleanup;
  }
  diag_phase_outcome(current_phase, "success", GS_OK);

  SET_PAIR_PHASE("pair.create_client_pairing_secret");
  unsigned char *signature = NULL;
  size_t s_len;
  if (sign_it(client_secret_data, sizeof(client_secret_data), &signature, &s_len, privateKey) != GS_OK) {
      gs_error = "Failed to sign data";
      ret = GS_FAILED;
      goto cleanup;
  }

  client_pairing_secret = malloc(sizeof(client_secret_data) + s_len);
  client_pairing_secret_hex = malloc(LEN_AS_HEX_STR(sizeof(client_secret_data) + s_len));
  memcpy(client_pairing_secret, client_secret_data, sizeof(client_secret_data));
  memcpy(client_pairing_secret + sizeof(client_secret_data), signature, s_len);
  bytes_to_hex(client_pairing_secret, client_pairing_secret_hex, sizeof(client_secret_data) + s_len);

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  diag_phase_outcome(current_phase, "success", GS_OK);
  SET_PAIR_PHASE("pair.submit_client_pairing_secret");
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&clientpairingsecret=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, client_pairing_secret_hex);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;
  diag_phase_outcome(current_phase, "success", ret);

  SET_PAIR_PHASE("pair.parse_client_pairing_secret_response");
  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size) != GS_OK))
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  diag_phase_outcome(current_phase, "success", GS_OK);
  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  SET_PAIR_PHASE("pair.request_pair_challenge");
  snprintf(url, url_max_len, "https://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&phrase=pairchallenge", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;
  diag_phase_outcome(current_phase, "success", ret);

  SET_PAIR_PHASE("pair.verify_pair_challenge");
  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size) != GS_OK))
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  diag_log_sha256("PAIR_CHALLENGE_VERIFY", "client_expected_paired",
                  (const unsigned char *)"1", 1);
  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  if (!host_pin_store_save_after_pairing(server->serverInfo.address,
                                         server->httpPort, public_key_pin,
                                         true)) {
    gs_error = "Failed to persist paired host public-key pin";
    ret = GS_FAILED;
    goto cleanup;
  }
  diag_phase_outcome(current_phase, "success", GS_OK);
  SET_PAIR_PHASE("pair.complete");
  server->paired = true;

  cleanup:
  if (ret != GS_OK && !diag_cancel_requested()) {
    const char *failed_phase = current_phase;
    diag_phase_outcome(failed_phase, "failed", ret);
    diag_set_phase("pair.cleanup_unpair");
    int cleanup_ret = gs_unpair_internal(server, false);
    diag_phase_outcome("pair.cleanup_unpair",
                       cleanup_ret == GS_OK ? "success" : "failed",
                       cleanup_ret);
  } else if (ret != GS_OK) {
    diag_phase_outcome(current_phase, "cancelled", ret);
    diag_log("PAIR_CLEANUP", "skipped unpair because watchdog cancellation was requested");
  } else {
    diag_phase_outcome(current_phase, "success", ret);
  }

  free(url);
  free(plaincert);
  free(challenge_response);
  free(pairing_secret);
  free(client_pairing_secret);
  free(client_pairing_secret_hex);
  free(result);

  http_free_data(data);

  // If we failed when attempting to pair with a game running, that's likely the issue.
  // Sunshine supports pairing with an active session, but GFE does not.
  if (ret != GS_OK && server->currentGame != 0) {
    gs_error = "The computer is currently in a game. You must close the game before pairing.";
    ret = GS_WRONG_STATE;
  }

  diag_phase_outcome("pair", ret == GS_OK ? "success" : "failed", ret);
#undef SET_PAIR_PHASE

  return ret;
}

int gs_applist(PSERVER_DATA server, PAPP_LIST *list) {
  int ret = GS_OK;
  char url[4096];
  diag_set_phase("connect.applist");
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    diag_phase_outcome("connect.applist", "failed", GS_OUT_OF_MEMORY);
    return GS_OUT_OF_MEMORY;
  }

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/applist?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if (http_request(url, data) != GS_OK)
    ret = GS_IO_ERROR;
  else if (xml_status(data->memory, data->size) == GS_ERROR)
    ret = GS_ERROR;
  else if (xml_applist(data->memory, data->size, list) != GS_OK)
    ret = GS_INVALID;

  http_free_data(data);
  diag_phase_outcome("connect.applist", ret == GS_OK ? "success" : "failed", ret);
  return ret;
}

int gs_display_list(PSERVER_DATA server, PDISPLAY_INFO *list) {
  char url[4096];
  char uuid_str[UUID_STRLEN];
  uuid_t uuid;
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  *list = NULL;
  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/displayinfo?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  int ret = http_request(url, data);
  if (ret == GS_OK) {
    int status_code = 0;
    int has_status_code = 0;
    char status_message[96] = {0};
    ret = xml_status_details(data->memory, data->size, &status_code,
                             &has_status_code, status_message, sizeof(status_message));
    if (ret == GS_OK) {
      ret = xml_display_list(data->memory, data->size, list);
    } else if (ret == GS_ERROR && !has_status_code) {
      // Some GameStream-compatible hosts omit status_code on this optional endpoint.
      // Accept only a well-formed display list; explicit non-200 statuses still fail.
      ret = xml_display_list(data->memory, data->size, list);
      if (ret == GS_OK) {
        int display_count = 0;
        for (PDISPLAY_INFO display = *list; display != NULL; display = display->next)
          display_count++;
        diag_log("DISPLAY_ENUM_COMPAT", "status_code=missing response=valid_xml display_count=%d", display_count);
      }
    } else if (ret == GS_ERROR) {
      static char display_status_error[160];
      snprintf(display_status_error, sizeof(display_status_error),
               "GameStream status_code=%d message=%s", status_code,
               status_message[0] ? status_message : "(missing)");
      gs_error = display_status_error;
    }
  }

  http_free_data(data);
  return ret;
}

void gs_free_display_list(PDISPLAY_INFO display_list) {
  while (display_list != NULL) {
    PDISPLAY_INFO next = display_list->next;
    free(display_list->id);
    free(display_list->name);
    free(display_list);
    display_list = next;
  }
}

static void url_encode_component(const char *input, char *output, size_t output_size) {
  static const char hex[] = "0123456789ABCDEF";
  size_t used = 0;
  for (const unsigned char *p = (const unsigned char *) input; *p != 0; ++p) {
    unsigned char ch = *p;
    int safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
    size_t needed = safe ? 1 : 3;
    if (used + needed >= output_size) {
      output[0] = 0;
      return;
    }
    if (safe) {
      output[used++] = (char) ch;
    } else {
      output[used++] = '%';
      output[used++] = hex[ch >> 4];
      output[used++] = hex[ch & 0x0F];
    }
  }
  output[used] = 0;
}

int gs_start_app(PSERVER_DATA server, STREAM_CONFIGURATION *config, int appId, bool sops, bool localaudio, int gamepad_mask, const char *display_id) {
  int ret = GS_OK;
  uuid_t uuid;
  char* result = NULL;
  char uuid_str[UUID_STRLEN];
  const char *current_phase = "stream.validate_mode";

  PDISPLAY_MODE mode = server->modes;
  bool correct_mode = false;
  diag_set_phase("stream.validate_mode");
  while (mode != NULL) {
    if (mode->width == config->width && mode->height == config->height) {
      if (mode->refresh == config->fps)
        correct_mode = true;
    }

    mode = mode->next;
  }

  if (!correct_mode && !server->unsupported) {
    diag_phase_outcome("stream.validate_mode", "failed", GS_NOT_SUPPORTED_MODE);
    return GS_NOT_SUPPORTED_MODE;
  }
  diag_phase_outcome("stream.validate_mode", "success", GS_OK);

  diag_set_phase("stream.start_app");
  RAND_bytes(config->remoteInputAesKey, sizeof(config->remoteInputAesKey));
  memset(config->remoteInputAesIv, 0, sizeof(config->remoteInputAesIv));

  char url[4096];
  char display_id_encoded[1024] = {0};
  char display_id_arg[1040] = {0};
  if (display_id != NULL && display_id[0] != 0) {
    url_encode_component(display_id, display_id_encoded, sizeof(display_id_encoded));
    if (display_id_encoded[0] != 0)
      snprintf(display_id_arg, sizeof(display_id_arg), "&display_id=%s", display_id_encoded);
  }
  u_int32_t rikeyid = 0;
  RAND_bytes(config->remoteInputAesIv, sizeof(rikeyid));
  memcpy(&rikeyid, config->remoteInputAesIv, sizeof(rikeyid));
  rikeyid = htonl(rikeyid);
  char rikey_hex[SIZEOF_AS_HEX_STR(config->remoteInputAesKey)];
  bytes_to_hex(config->remoteInputAesKey, rikey_hex, sizeof(config->remoteInputAesKey));

  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    diag_phase_outcome("stream.start_app", "failed", GS_OUT_OF_MEMORY);
    return GS_OUT_OF_MEMORY;
  }

  // Using an FPS value over 60 causes SOPS to default to 720p60,
  // so force it to 0 to ensure the correct resolution is set. We
  // used to use 60 here but that locked the frame rate to 60 FPS
  // on GFE 3.20.3.
  int fps = (server->isNvidiaSoftware && config->fps > 60) ? 0 : config->fps;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  int surround_info = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config->audioConfiguration);
  snprintf(url, sizeof(url), "https://%s:%u/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d&localAudioPlayMode=%d&surroundAudioInfo=%d&remoteControllersBitmap=%d&gcmap=%d%s%s%s",
           server->serverInfo.address, server->httpsPort, server->currentGame ? "resume" : "launch", unique_id, uuid_str, appId, config->width, config->height, fps, sops, rikey_hex, rikeyid, localaudio, surround_info, gamepad_mask, gamepad_mask,
           display_id_arg,
           (config->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ? "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" : "",
           LiGetLaunchUrlQueryParameters());
  current_phase = "stream.start_app_request";
  diag_set_phase(current_phase);
  if ((ret = http_request(url, data)) == GS_OK)
    server->currentGame = appId;
  else
    goto cleanup;
  diag_phase_outcome("stream.start_app_request", "success", ret);

  current_phase = "stream.parse_start_app_response";
  diag_set_phase(current_phase);
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "gamesession", &result)) != GS_OK &&
           (ret = xml_search(data->memory, data->size, "resume", &result)) != GS_OK)
    goto cleanup;

  if (!strcmp(result, "0")) {
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;

  if (xml_search(data->memory, data->size, "sessionUrl0", &result) == GS_OK) {
    server->serverInfo.rtspSessionUrl = result;
    result = NULL;
  }

  cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  diag_phase_outcome(current_phase, ret == GS_OK ? "success" : "failed", ret);
  return ret;
}

int gs_quit_app(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char* result = NULL;
  diag_set_phase("stream.quit_app_request");
  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    diag_phase_outcome("stream.quit_app_request", "failed", GS_OUT_OF_MEMORY);
    return GS_OUT_OF_MEMORY;
  }

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/cancel?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "cancel", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "0") == 0) {
    ret = GS_FAILED;
    goto cleanup;
  }

  cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  diag_phase_outcome("stream.quit_app_request",
                     ret == GS_OK ? "success" : "failed", ret);
  return ret;
}

int gs_init(PSERVER_DATA server, char *address, unsigned short httpPort, const char *keyDirectory, int log_level, bool unsupported) {
  int ret;
  diag_set_phase("connect.gs_init");
  diag_set_phase("connect.load_identity");
  mkdirtree(keyDirectory);
  if (load_unique_id(keyDirectory) != GS_OK) {
    diag_phase_outcome("connect.load_identity", "failed", GS_FAILED);
    diag_phase_outcome("connect.gs_init", "failed", GS_FAILED);
    return GS_FAILED;
  }
  diag_phase_outcome("connect.load_identity", "success", GS_OK);

  diag_set_phase("connect.load_certificate");
  if (load_cert(keyDirectory)) {
    diag_phase_outcome("connect.load_certificate", "failed", GS_FAILED);
    diag_phase_outcome("connect.gs_init", "failed", GS_FAILED);
    return GS_FAILED;
  }
  diag_phase_outcome("connect.load_certificate", "success", GS_OK);

  uintptr_t stack_pointer;
  __asm__ volatile("mov %0, sp" : "=r"(stack_pointer) : : "memory");
  diag_stack_checkpoint("connect.http_init.before_phase", stack_pointer);
  diag_set_phase("connect.http_init");
  ret = http_init(keyDirectory, log_level);
  diag_phase_outcome("connect.http_init", ret == GS_OK ? "success" : "failed", ret);

  LiInitializeServerInformation(&server->serverInfo);
  server->serverInfo.address = address;
  server->unsupported = unsupported;
  server->httpPort = httpPort ? httpPort : 47989;
  server->httpsPort = 0; /* Populated by load_server_status() */
  ret = load_server_status(server);
  diag_phase_outcome("connect.gs_init", ret == GS_OK ? "success" : "failed", ret);
  return ret;
}

void gs_cleanup() {
  if (cert != NULL)
    X509_free(cert);
  if (privateKey != NULL)
  {
    EVP_PKEY_free(privateKey);
    // Set to NULL to avoid reconnection issues
    privateKey = NULL;
  }
  http_cleanup();
}
