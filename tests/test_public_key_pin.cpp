#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <assert.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>

#include "public_key_pin.h"
#include "pin_store.h"
#include "pair_record.hpp"

static std::string make_certificate(EVP_PKEY *key, long serial) {
    X509 *cert = X509_new();
    assert(cert != NULL);
    assert(X509_set_version(cert, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), serial) == 1);
    assert(X509_gmtime_adj(X509_get_notBefore(cert), 0) != NULL);
    assert(X509_gmtime_adj(X509_get_notAfter(cert), 3600) != NULL);
    assert(X509_set_pubkey(cert, key) == 1);
    X509_NAME *name = X509_get_subject_name(cert);
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      (const unsigned char *)"synthetic", -1,
                                      -1, 0) == 1);
    assert(X509_set_issuer_name(cert, name) == 1);
    assert(X509_sign(cert, key, EVP_sha256()) > 0);
    BIO *bio = BIO_new(BIO_s_mem());
    assert(bio != NULL && PEM_write_bio_X509(bio, cert) == 1);
    char *data = NULL;
    const long length = BIO_get_mem_data(bio, &data);
    assert(length > 0 && data != NULL);
    std::string pem(data, (size_t)length);
    BIO_free(bio);
    X509_free(cert);
    return pem;
}

static EVP_PKEY *make_key() {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    assert(ctx != NULL);
    assert(EVP_PKEY_keygen_init(ctx) == 1);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
    EVP_PKEY *key = NULL;
    assert(EVP_PKEY_keygen(ctx, &key) == 1);
    EVP_PKEY_CTX_free(ctx);
    return key;
}

int main() {
    assert(mkdir(MOONLIGHT_3DS_PATH, 0700) == 0);
    EVP_PKEY *key_a = make_key();
    EVP_PKEY *key_b = make_key();
    const std::string cert_a = make_certificate(key_a, 1);
    const std::string cert_a_renewed = make_certificate(key_a, 2);
    const std::string cert_b = make_certificate(key_b, 3);
    char pin_a[53], pin_a_renewed[53], pin_b[53];
    assert(derive_public_key_pin(cert_a.c_str(), pin_a));
    assert(derive_public_key_pin(cert_a_renewed.c_str(), pin_a_renewed));
    assert(derive_public_key_pin(cert_b.c_str(), pin_b));
    assert(valid_public_key_pin(pin_a));
    assert(strcmp(pin_a, pin_a_renewed) == 0);
    assert(strcmp(pin_a, pin_b) != 0);
    assert(valid_public_key_pin(pin_a_renewed)); // same SPKI is a pin match
    assert(valid_public_key_pin(pin_b) && strcmp(pin_a, pin_b) != 0); // key mismatch
    assert(!valid_public_key_pin("sha256//bad"));
    assert(!derive_public_key_pin("not a certificate", pin_b));

    assert(host_pin_store_save_after_pairing("192.0.2.10", 47989, pin_a, true));
    char loaded[53];
    assert(host_pin_store_load("192.0.2.10", 47989, loaded, sizeof(loaded)));
    assert(strcmp(loaded, pin_a) == 0);
    assert(!host_pin_store_load("192.0.2.10", 47990, loaded, sizeof(loaded)));
    assert(!host_pin_store_load("192.0.2.11", 47989, loaded, sizeof(loaded)));
    assert(!host_pin_store_load("192.0.2.12", 47989, loaded, sizeof(loaded))); // absent pin

    assert(!host_pin_store_save_after_pairing("192.0.2.10", 47989, pin_b, false));
    assert(host_pin_store_load("192.0.2.10", 47989, loaded, sizeof(loaded)));
    assert(strcmp(loaded, pin_a) == 0);
    assert(host_pin_store_save_after_pairing("192.0.2.10", 47989, pin_b, true));
    assert(host_pin_store_load("192.0.2.10", 47989, loaded, sizeof(loaded)));
    assert(strcmp(loaded, pin_b) == 0);
    remove_host_public_key_pin("192.0.2.10", 47989);
    assert(!host_pin_store_load("192.0.2.10", 47989, loaded, sizeof(loaded)));

    EVP_PKEY_free(key_a);
    EVP_PKEY_free(key_b);
    puts("public-key pin derivation and persistence tests passed");
    return 0;
}
