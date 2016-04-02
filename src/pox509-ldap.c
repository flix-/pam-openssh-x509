/*
 * Copyright (C) 2014-2016 Sebastian Roland <seroland86@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pox509-ldap.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <confuse.h>
#include <lber.h>
#include <ldap.h>
#include <openssl/x509.h>

#include "pox509-log.h"

#define LDAP_SEARCH_FILTER_BUFFER_SIZE 1024

static void
set_ldap_options(LDAP *ldap_handle, cfg_t *cfg)
{
    if (ldap_handle == NULL || cfg == NULL) {
        fatal("ldap_handle or cfg == NULL");
    }

    /* set protocol version */
    int ldap_version = LDAP_VERSION3;
    int rc = ldap_set_option(ldap_handle, LDAP_OPT_PROTOCOL_VERSION,
        &ldap_version);
    if (rc != LDAP_OPT_SUCCESS) {
        fatal("ldap_set_option(): key: LDAP_OPT_PROTOCOL_VERSION, value: %d",
            ldap_version);
    }

    /* force validation of certificates when using ldaps */
    int req_cert = LDAP_OPT_X_TLS_HARD;
    rc = ldap_set_option(ldap_handle, LDAP_OPT_X_TLS_REQUIRE_CERT, &req_cert);
    if (rc != LDAP_OPT_SUCCESS) {
        fatal("ldap_set_option(): key: LDAP_OPT_X_TLS_REQUIRE_CERT, value: %d",
            req_cert);
    }

    /* set trusted ca path */
    char *cacerts_dir = cfg_getstr(cfg, "cacerts_dir");
    rc = ldap_set_option(ldap_handle, LDAP_OPT_X_TLS_CACERTDIR, cacerts_dir);
    if (rc != LDAP_OPT_SUCCESS) {
        fatal("ldap_set_option(): key: LDAP_OPT_X_TLS_CACERTDIR, value: %s",
            cacerts_dir);
    }

    /*
     * new context has to be set in order to apply options set above regarding
     * tls.
     */
    int new_ctx = 0x56;
    rc = ldap_set_option(ldap_handle, LDAP_OPT_X_TLS_NEWCTX, &new_ctx);
    if (rc != LDAP_OPT_SUCCESS) {
        fatal("ldap_set_option(): key: LDAP_OPT_X_TLS_NEWCTX, value: %d",
            new_ctx);
    }
}

static void
init_starttls(LDAP *ldap_handle)
{
    if (ldap_handle == NULL) {
        fatal("ldap_handle == NULL");
    }

    int rc = ldap_start_tls_s(ldap_handle, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
        char *msg = NULL;
        rc = ldap_get_option(ldap_handle, LDAP_OPT_DIAGNOSTIC_MESSAGE, &msg);
        if (rc == LDAP_OPT_SUCCESS) {
            fatal("ldap_start_tls_s(): '%s'", msg);
        }
        fatal("ldap_start_tls_s()");
    }
}

static int
bind_to_ldap(LDAP *ldap_handle, cfg_t *cfg)
{
    if (ldap_handle == NULL || cfg == NULL) {
        fatal("ldap_handle or cfg == NULL");
    }

    char *ldap_bind_dn = cfg_getstr(cfg, "ldap_bind_dn");
    char *ldap_bind_pwd = cfg_getstr(cfg, "ldap_bind_pwd");
    size_t ldap_bind_pwd_length = strlen(ldap_bind_pwd);
    struct berval cred = {
        .bv_len = ldap_bind_pwd_length,
        .bv_val = ldap_bind_pwd
    };
    int rc = ldap_sasl_bind_s(ldap_handle, ldap_bind_dn, LDAP_SASL_SIMPLE,
        &cred, NULL, NULL, NULL);
    memset(ldap_bind_pwd, 0, ldap_bind_pwd_length);
    return rc;
}

static void
init_ldap_handle(LDAP **ldap_handle, cfg_t *cfg)
{
    if (ldap_handle == NULL || cfg == NULL) {
        fatal("ldap_handle or cfg == NULL");
    }

    char *ldap_uri = cfg_getstr(cfg, "ldap_uri");
    int rc = ldap_initialize(ldap_handle, ldap_uri);
    if (rc != LDAP_SUCCESS) {
        fatal("ldap_initialize(): '%s' (%d)", ldap_err2string(rc), rc);
    }

    set_ldap_options(*ldap_handle, cfg);
    int ldap_starttls = cfg_getint(cfg, "ldap_starttls");
    if (ldap_starttls) {
        init_starttls(*ldap_handle);
    }
}

static void
get_attr_values_as_string(LDAP *ldap_handle, LDAPMessage *result, char *attr,
    char ***ret)
{
    if (ldap_handle == NULL || result == NULL || attr == NULL || ret == NULL) {
        fatal("ldap_handle, result, attr or ret == NULL");
    }

    int rc = ldap_count_entries(ldap_handle, result);
    if (rc == 0) {
        log_fail("ldap_count_entries() == 0");
        goto error;
    }

    /* get attribute values */
    result = ldap_first_entry(ldap_handle, result);
    if (result == NULL) {
        fatal("ldap_first_entry() == NULL");
    }
    struct berval **values = ldap_get_values_len(ldap_handle, result, attr);
    if (values == NULL) {
        log_fail("ldap_get_values_len() == NULL");
        goto error;
    }
    int count = ldap_count_values_len(values);
    if (count == 0) {
        log_fail("ldap_count_values_len() == 0");
        goto error;
    }

    *ret = malloc(sizeof(char *) * (count + 1));
    if (*ret == NULL) {
        fatal("malloc");
    }

    for (int i = 0; values[i] != NULL; i++) {
        char *value = values[i]->bv_val;
        ber_len_t len = values[i]->bv_len;
        (*ret)[i] = strndup(value, len + 1);
        /* FIXME: Check strndup() function --> possibly writing to buffer not
        owned */
        (*ret)[i][len] = '\0';
    }
    (*ret)[count] = NULL;
    ldap_value_free_len(values);

    return;

error:
    *ret = NULL;
    return;
}

static void
get_attr_values_as_binary(LDAP *ldap_handle, LDAPMessage *result, char *attr,
    struct berval ***ret)
{
    if (ldap_handle == NULL || result == NULL || attr == NULL || ret == NULL) {
        fatal("ldap_handle, result, attr or ret == NULL");
    }

    int rc = ldap_count_entries(ldap_handle, result);
    if (rc == 0) {
        log_fail("ldap_count_entries() == 0");
        return;
    }

    /* get attribute values */
    result = ldap_first_entry(ldap_handle, result);
    if (result == NULL) {
        fatal("ldap_first_entry() == NULL");
    }
    *ret = ldap_get_values_len(ldap_handle, result, attr);
    if (*ret == NULL) {
        log_fail("ldap_get_values_len() == NULL");
        return;
    }
}

static void
free_attr_values_as_string_array(char **values)
{
    if (values == NULL) {
        fatal("values == NULL");
    }

    for (int i = 0; values[i] != NULL; i++) {
        free(values[i]);
    }
    free(values);
}

static void
free_attr_values_as_binary_array(struct berval **values)
{
    if (values == NULL) {
        fatal("values == NULL");
    }

    ldap_value_free_len(values);
}

static bool
is_relevant_dap(struct pox509_info *pox509_info,
    struct pox509_key_provider *provider)
{
    if (pox509_info == NULL || provider == NULL) {
        fatal("pox509_info or provider == NULL");
    }

    if (strcmp(pox509_info->uid, provider->uid) == 0) {
        return true;
    }
    return false;
}

static bool
is_relevant_aobp(struct pox509_info *pox509_info, char *target)
{
    if (pox509_info == NULL || target == NULL) {
        fatal("pox509_info or target == NULL");
    }

    /*
    if (strcmp(pox509_info->uid, target->uid) == 0) {
        return true;
    }
    return false;
    */
    return true;
}

static void
get_access_profile_dns(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_info *pox509_info, char ***access_profile_dns)
{
    if (ldap_handle == NULL || cfg == NULL || access_profile_dns == NULL) {
        fatal("ldap_handle, cfg or access_profile_dns == NULL");
    }

    char *server_dn = cfg_getstr(cfg, "ldap_server_base_dn");
    int server_search_scope = cfg_getint(cfg, "ldap_server_search_scope");
    /* construct search filter */
    char filter[LDAP_SEARCH_FILTER_BUFFER_SIZE];
    char *server_uid_attr = cfg_getstr(cfg, "ldap_server_uid_attr");
    char *server_uid = cfg_getstr(cfg, "server_uid");
    create_ldap_search_filter(server_uid_attr, server_uid, filter,
        sizeof filter);
    char *access_profile_attr = cfg_getstr(cfg,
        "ldap_server_access_profile_attr");
    char *attrs[] = {
        access_profile_attr,
        NULL
    };
    int ldap_search_timeout = cfg_getint(cfg, "ldap_search_timeout");
    struct timeval search_timeout = {
        .tv_sec = ldap_search_timeout,
        .tv_usec = 0
    };
    int sizelimit = 1;
    LDAPMessage *result = NULL;
    int rc = ldap_search_ext_s(ldap_handle, server_dn, server_search_scope,
        filter, attrs, 0, NULL, NULL, &search_timeout, sizelimit, &result);
    if (rc != LDAP_SUCCESS) {
        fatal("ldap_search_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
    }
    /* set dn in dto */
    pox509_info->dn = ldap_get_dn(ldap_handle, result);
    if (pox509_info->dn == NULL) {
        log_fail("ldap_get_dn() failed");
    }
    /* get dn's as strings */
    get_attr_values_as_string(ldap_handle, result, access_profile_attr,
        access_profile_dns);
}

static void
get_access_profiles(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_info *pox509_info)
{
    if (ldap_handle == NULL || cfg == NULL || pox509_info == NULL) {
        fatal("ldap_handle, cfg or pox509_info == NULL");
    }

    char **access_profile_dns = NULL;
    get_access_profile_dns(ldap_handle, cfg, pox509_info, &access_profile_dns);
    if (access_profile_dns == NULL) {
        fatal("access_profile_dns == NULL");
    }

    /* iterate access profiles */
    for (int i = 0; access_profile_dns[i] != NULL; i++) {
        char *dn = access_profile_dns[i];
        int ldap_search_timeout = cfg_getint(cfg, "ldap_search_timeout");
        struct timeval search_timeout = {
            .tv_sec = ldap_search_timeout,
            .tv_usec = 0
        };
        int sizelimit = 1;
        LDAPMessage *result = NULL;

        int rc = ldap_search_ext_s(ldap_handle, dn, LDAP_SCOPE_BASE, NULL, NULL,
            0, NULL, NULL, &search_timeout, sizelimit, &result);
        if (rc != LDAP_SUCCESS) {
            fatal("ldap_search_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
        }

        /* get objectclass attribute in order to decide whether we have
         * an access profile of type direct or onbehalf
         */
        char **access_profile_objectclass = NULL;
        get_attr_values_as_string(ldap_handle, result, "objectClass",
            &access_profile_objectclass);
        if (access_profile_objectclass == NULL) {
            fatal("access_profile_objectclass == NULL");
        }

        enum pox509_access_profile_type profile_type = UNKNOWN;
        for (int j = 0; access_profile_objectclass[j] != NULL; j++) {
            char *objectclass = access_profile_objectclass[j];
            if (strcmp(objectclass, POX509_DAP_OBJCLASS) == 0) {
                profile_type = DIRECT_ACCESS;
                break;
            } else if (strcmp(objectclass, POX509_AOBP_OBJCLASS) == 0) {
                profile_type = ACCESS_ON_BEHALF;
                break;
            }
        }
        free_attr_values_as_string_array(access_profile_objectclass);

        switch(profile_type) {
        case DIRECT_ACCESS:
        {
            log_msg("got direct access profile");
            /* create new direct access profile */
            struct pox509_direct_access_profile *profile =
                malloc(sizeof *profile);
            if (profile == NULL) {
                fatal("malloc()");
            }
            init_direct_access_profile(profile);

            profile->dn = strdup(dn);
            set_rdn_value_from_dn(dn, &profile->name);

            /* set key provider dn */
            char **key_provider_dn = NULL;
            get_attr_values_as_string(ldap_handle, result,
                POX509_DAP_KEY_PROVIDER_ATTR, &key_provider_dn);
            if (key_provider_dn == NULL) {
                fatal("key_provider_dn == NULL");
            }
            profile->key_provider_dn = strdup(key_provider_dn[0]);
            free_attr_values_as_string_array(key_provider_dn);

            /* set keystore options dn */
            char **keystore_options_dn = NULL;
            get_attr_values_as_string(ldap_handle, result,
                POX509_DAP_KEYSTORE_OPTIONS_ATTR, &keystore_options_dn);
            if (keystore_options_dn == NULL) {
                fatal("keystore_options_dn == NULL");
            }
            profile->keystore_options_dn = strdup(keystore_options_dn[0]);
            free_attr_values_as_string_array(keystore_options_dn);

            /* add to direct access profile list */
            STAILQ_INSERT_TAIL(&pox509_info->direct_access_profiles, profile,
                profiles);
            break;
        }
        case ACCESS_ON_BEHALF:
        {
            log_msg("got access on behalf profile");
            struct pox509_access_on_behalf_profile *profile = NULL;
            profile = malloc(sizeof *profile);
            if (profile == NULL) {
                fatal("malloc()");
            }
            init_access_on_behalf_profile(profile);

            profile->dn = strdup(dn);
            set_rdn_value_from_dn(dn, &profile->name);

            /* set target keystore group dn */
            char **target_keystore_group_dn = NULL;
            get_attr_values_as_string(ldap_handle, result,
                POX509_AOBP_TARGET_KEYSTORE_ATTR, &target_keystore_group_dn);
            if (target_keystore_group_dn == NULL) {
                fatal("target_keystore_group_dn == NULL");
            }
            profile->target_keystore_group_dn =
                strdup(target_keystore_group_dn[0]);
            free_attr_values_as_string_array(target_keystore_group_dn);

            /* set key provider group dn*/
            char **key_provider_group_dn = NULL;
            get_attr_values_as_string(ldap_handle, result,
                POX509_AOBP_KEY_PROVIDER_ATTR, &key_provider_group_dn);
            if (key_provider_group_dn == NULL) {
                fatal("key_provider_group_dn == NULL");
            }
            profile->key_provider_group_dn = strdup(key_provider_group_dn[0]);
            free_attr_values_as_string_array(key_provider_group_dn);

            /* set keystore options */
            char **keystore_options_dn = NULL;
            get_attr_values_as_string(ldap_handle, result,
                POX509_AOBP_KEYSTORE_OPTIONS_ATTR, &keystore_options_dn);
            if (keystore_options_dn == NULL) {
                fatal("keystore_options_dn == NULL");
            }
            profile->keystore_options_dn = strdup(keystore_options_dn[0]);
            free_attr_values_as_string_array(keystore_options_dn);

            /* add to access on behalf profile list */
            STAILQ_INSERT_TAIL(&pox509_info->access_on_behalf_profiles, profile,
                profiles);
            break;
        }
        case UNKNOWN:
        {
            break;
        }
        default:
            fatal("profile_type undefined: %d", profile_type);
        }
        ldap_msgfree(result);
    }
    free_attr_values_as_string_array(access_profile_dns);
}

/*
static void
get_keystore_options(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_keystore_options *keystore_options)
{
    if (ldap_handle == NULL || cfg == NULL || keystore_options == NULL) {
        fatal("ldap_handle, cfg or keystore_options == NULL");
    }

    char *attrs[] = {
        POX509_KEYSTORE_OPTIONS_FROM_ATTR,
        POX509_KEYSTORE_OPTIONS_CMD_ATTR,
        NULL
    };
    int ldap_search_timeout = cfg_getint(cfg, "ldap_search_timeout");
    struct timeval search_timeout = {
        .tv_sec = ldap_search_timeout,
        .tv_usec = 0
    };
    int sizelimit = 1;
    LDAPMessage *result = NULL;
    int rc = ldap_search_ext_s(ldap_handle, keystore_options->dn,
        LDAP_SCOPE_BASE, NULL, attrs, 0, NULL, NULL, &search_timeout, sizelimit,
        &result);
    if (rc != LDAP_SUCCESS) {
        fatal("ldap_search_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
    }
*/
    /* optional attributes --> can be NULL */
/*
    char **keystore_options_from = NULL;
    get_attr_values_as_string(ldap_handle, result,
        POX509_KEYSTORE_OPTIONS_FROM_ATTR, &keystore_options_from);
    if (keystore_options_from == NULL) {
        log_msg("keystore_options_from_attr == NULL");
    } else {
        keystore_options->from_option = strdup(keystore_options_from[0]);
        free_attr_values_as_string_array(keystore_options_from);
    }

    char **keystore_options_cmd = NULL;
    get_attr_values_as_string(ldap_handle, result,
        POX509_KEYSTORE_OPTIONS_CMD_ATTR, &keystore_options_cmd);
    if (keystore_options_cmd == NULL) {
        log_msg("keystore_options_cmd_attr == NULL");
    } else {
        keystore_options->command_option = strdup(keystore_options_cmd[0]);
        free_attr_values_as_string_array(keystore_options_cmd);
    }
    ldap_msgfree(result);
}

static void
get_key_provider(LDAP *ldap_handle, cfg_t *cfg, char *key_provider_dn,
    struct pox509_key_provider **provider)
{
    if (ldap_handle == NULL || cfg == NULL || provider == NULL) {
        fatal("ldap_handle, cfg or provider == NULL");
    }

    char *provider_uid_attr = cfg_getstr(cfg, "ldap_provider_uid_attr");
    char *provider_cert_attr = cfg_getstr(cfg, "ldap_provider_cert_attr");
    char *attrs[] = {
        provider_uid_attr,
        provider_cert_attr,
        NULL
    };
    int ldap_search_timeout = cfg_getint(cfg, "ldap_search_timeout");
    struct timeval search_timeout = {
        .tv_sec = ldap_search_timeout,
        .tv_usec = 0
    };
    int sizelimit = 1;
    LDAPMessage *result = NULL;
    int rc = ldap_search_ext_s(ldap_handle, provider->dn, LDAP_SCOPE_BASE, NULL,
        attrs, 0, NULL, NULL, &search_timeout, sizelimit, &result);
    if (rc != LDAP_SUCCESS) {
        fatal("ldap_search_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
    }

    char **provider_uid = NULL;
    get_attr_values_as_string(ldap_handle, result, provider_uid_attr,
        &provider_uid);
    if (provider_uid == NULL) {
        fatal("provider_uid == NULL");
    }
    provider->uid = strdup(provider_uid[0]);
    free_attr_values_as_string_array(provider_uid);

    struct berval **provider_cert = NULL;
    get_attr_values_as_binary(ldap_handle, result, provider_cert_attr,
        &provider_cert);
    if (provider_cert == NULL) {
        fatal("provider_cert == NULL");
    }
    for (int i = 0; provider_cert[i] != NULL; i++) {
        char *value = provider_cert[i]->bv_val;
        ber_len_t len = provider_cert[i]->bv_len;
        provider->x509 = d2i_X509(NULL, (const unsigned char **) &value, len);
        if (provider->x509 == NULL) {
            log_fail("d2i_X509(): cannot decode certificate");
            */
            /* try next certificate if existing */
            /*
            continue;
        }
        break;
    }
    free_attr_values_as_binary_array(provider_cert);
    ldap_msgfree(result);
}
*/

static void // TODO: move this code to is_relevant_aobp_profile
strip_access_on_behalf_profiles(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_info *pox509_info)
{
    if (ldap_handle == NULL || cfg == NULL || pox509_info == NULL) {
        fatal("ldap_handle, cfg or pox509_info == NULL");
    }

    if (STAILQ_EMPTY(&pox509_info->access_on_behalf_profiles)) {
        log_msg("access on behalf profile list EMPTY");
        return;
    }

    /* iterate access on behalf profiles */
    struct pox509_access_on_behalf_profile *profile = NULL;
    STAILQ_FOREACH(profile, &pox509_info->access_on_behalf_profiles, profiles) {
        char *target_group_attr = cfg_getstr(cfg, "ldap_target_group_attr");
        char *target_group_attrs[] = {
            target_group_attr,
            NULL
        };
        int ldap_search_timeout = cfg_getint(cfg, "ldap_search_timeout");
        struct timeval search_timeout = {
            .tv_sec = ldap_search_timeout,
            .tv_usec = 0
        };
        int sizelimit = 1;
        LDAPMessage *result = NULL;

        int rc = ldap_search_ext_s(ldap_handle,
            profile->target_keystore_group_dn, LDAP_SCOPE_BASE, NULL,
            target_group_attrs, 0, NULL, NULL, &search_timeout, sizelimit,
            &result);
        if (rc != LDAP_SUCCESS) {
            fatal("ldap_search_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
        }

        /* get dn's of target keystore ee's */
        char **target_ee_dns = NULL;
        get_attr_values_as_string(ldap_handle, result, target_group_attr,
            &target_ee_dns);
        if (target_ee_dns == NULL) {
            fatal("target_ee_dns == NULL");
        }
        ldap_msgfree(result);

        bool strip_profile = true;
        for (int i = 0; target_ee_dns[i] != NULL && strip_profile; i++) {
            /* get ee's and compare uid to uid logging in */
            char *target_uid_attr = cfg_getstr(cfg, "ldap_target_uid_attr");
            char *target_ee_attrs[] = {
                target_uid_attr,
                NULL
            };
            result = NULL;
            rc = ldap_search_ext_s(ldap_handle, target_ee_dns[i],
                LDAP_SCOPE_BASE, NULL, target_ee_attrs, 0, NULL, NULL,
                &search_timeout, sizelimit, &result);
            if (rc != LDAP_SUCCESS) {
                fatal("ldap_search_ext_s(): '%s' (%d)", ldap_err2string(rc),
                    rc);
            }

            char **target_ee_uid = NULL;
            get_attr_values_as_string(ldap_handle, result, target_uid_attr,
                &target_ee_uid);
            if (target_ee_uid == NULL) {
                fatal("target_ee_uid == NULL");
            }

            if (strcmp(target_ee_uid[0], pox509_info->uid) == 0) {
                strip_profile = false;
            }
            free_attr_values_as_string_array(target_ee_uid);
        }

        if (strip_profile) {
            STAILQ_REMOVE(&pox509_info->access_on_behalf_profiles, profile,
            pox509_access_on_behalf_profile, profiles);
            free_access_on_behalf_profile(profile);
        }
        free_attr_values_as_string_array(target_ee_dns);
    }
}

static void
process_direct_access_profiles(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_info *pox509_info)
{
    if (ldap_handle == NULL || cfg == NULL || pox509_info == NULL) {
        fatal("ldap_handle, cfg or pox509_info == NULL");
    }

    if (STAILQ_EMPTY(&pox509_info->direct_access_profiles)) {
        log_msg("access profile list EMPTY");
        return;
    }

    /* iterate direct access profiles */
    struct pox509_direct_access_profile *profile = NULL;
    STAILQ_FOREACH(profile, &pox509_info->direct_access_profiles, profiles) {
        /*
        get_key_provider(profile->key_provider_dn);
        if (!is_relevant_dap(pox509_info, profile->key_provider)) {
            free and remove
        }
        get_keystore_options(profile->keystore_options_dn);
        */
    }
}

static void
process_access_on_behalf_profiles(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_info *pox509_info)
{
    if (ldap_handle == NULL || cfg == NULL || pox509_info == NULL) {
        fatal("ldap_handle, cfg or pox509_info == NULL");
    }

    if (STAILQ_EMPTY(&pox509_info->access_on_behalf_profiles)) {
        log_msg("access on behalf profile list EMPTY");
        return;
    }

    /* iterate access on behalf profiles */
    struct pox509_access_on_behalf_profile *profile = NULL;
    STAILQ_FOREACH(profile, &pox509_info->access_on_behalf_profiles, profiles) {
        /*
        if (!is_relevant_aobp(profile->target_keystore_group_dn)) {
            free and remove;
        }
        for each dn in key_provider_group_dn {
            get_key_provider();
        }
        get_keystore_options(profile->keystore_options_dn);
        */
    }
}

void
get_keystore_data_from_ldap(cfg_t *cfg, struct pox509_info *pox509_info)
{
    if (cfg == NULL || pox509_info == NULL) {
        fatal("cfg or pox509_info == NULL");
    }

    /* init ldap handle */
    LDAP *ldap_handle = NULL;
    init_ldap_handle(&ldap_handle, cfg);
    /* bind to ldap server */
    int rc = bind_to_ldap(ldap_handle, cfg);
    if (rc != LDAP_SUCCESS) {
        pox509_info->ldap_online = 0;
        log_fail("bind_to_ldap(): '%s' (%d)", ldap_err2string(rc), rc);
        goto unbind_and_free_handle;
    }
    log_success("bind_to_ldap()");
    pox509_info->ldap_online = 1;

    /* retrieve data */
    get_access_profiles(ldap_handle, cfg, pox509_info);
    process_direct_access_profiles(ldap_handle, cfg, pox509_info);
    process_access_on_behalf_profiles(ldap_handle, cfg, pox509_info);

unbind_and_free_handle:
    rc = ldap_unbind_ext_s(ldap_handle, NULL, NULL);
    if (rc == LDAP_SUCCESS) {
        log_success("ldap_unbind_ext_s()");
    } else {
        log_fail("ldap_unbind_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
    }
}

