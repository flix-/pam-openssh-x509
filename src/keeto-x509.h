/*
 * Copyright (C) 2014-2018 Sebastian Roland <seroland86@gmail.com>
 *
 * This file is part of Keeto.
 *
 * Keeto is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Keeto is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Keeto.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KEETO_X509_H
#define KEETO_X509_H

#include <stdbool.h>

#include <openssl/x509.h>

#include "keeto-util.h"

enum keeto_digests {
    KEETO_DIGEST_MD5,
    KEETO_DIGEST_SHA256
};

#define PUT_32BIT(cp, value) do { \
    (cp)[0] = (unsigned char) ((value) >> 24); \
    (cp)[1] = (unsigned char) ((value) >> 16); \
    (cp)[2] = (unsigned char) ((value) >> 8); \
    (cp)[3] = (unsigned char) (value); \
} while (0)

int init_cert_store(char *cert_store_dir, bool check_crl);
void free_cert_store();
int add_key_data_from_x509(X509 *x509, struct keeto_key *key);
int validate_x509(X509 *x509, bool *valid);
char *get_serial_from_x509(X509 *x509);
int get_issuer_from_x509(X509 *x509, char **ret);
int get_subject_from_x509(X509 *x509, char **ret);
void free_x509(X509 *x509);

#endif /* KEETO_X509_H */

