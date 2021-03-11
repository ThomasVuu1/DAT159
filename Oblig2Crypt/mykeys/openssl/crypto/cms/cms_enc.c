/*
 * Copyright 2008-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "internal/cryptlib.h"
#include <openssl/asn1t.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/cms.h>
#include <openssl/rand.h>
#include "crypto/evp.h"
#include "cms_local.h"

/* CMS EncryptedData Utilities */

/* Return BIO based on EncryptedContentInfo and key */

BIO *cms_EncryptedContent_init_bio(CMS_EncryptedContentInfo *ec,
                                   const CMS_CTX *cms_ctx)
{
    BIO *b;
    EVP_CIPHER_CTX *ctx;
    EVP_CIPHER *fetched_ciph = NULL;
    const EVP_CIPHER *cipher = NULL;
    X509_ALGOR *calg = ec->contentEncryptionAlgorithm;
    evp_cipher_aead_asn1_params aparams;
    unsigned char iv[EVP_MAX_IV_LENGTH], *piv = NULL;
    unsigned char *tkey = NULL;
    int len;
    int ivlen = 0;
    size_t tkeylen = 0;
    int ok = 0;
    int enc, keep_key = 0;

    enc = ec->cipher ? 1 : 0;

    b = BIO_new(BIO_f_cipher());
    if (b == NULL) {
        CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    BIO_get_cipher_ctx(b, &ctx);

    (void)ERR_set_mark();
    if (enc) {
        cipher = ec->cipher;
        /*
         * If not keeping key set cipher to NULL so subsequent calls decrypt.
         */
        if (ec->key != NULL)
            ec->cipher = NULL;
    } else {
        cipher = EVP_get_cipherbyobj(calg->algorithm);
    }
    if (cipher != NULL) {
        fetched_ciph = EVP_CIPHER_fetch(cms_ctx->libctx, EVP_CIPHER_name(cipher),
                                        cms_ctx->propq);
        if (fetched_ciph != NULL)
            cipher = fetched_ciph;
    }
    if (cipher == NULL) {
        (void)ERR_clear_last_mark();
        CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO, CMS_R_UNKNOWN_CIPHER);
        goto err;
    }
    (void)ERR_pop_to_mark();

    if (EVP_CipherInit_ex(ctx, cipher, NULL, NULL, NULL, enc) <= 0) {
        CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO,
               CMS_R_CIPHER_INITIALISATION_ERROR);
        goto err;
    }

    if (enc) {
        calg->algorithm = OBJ_nid2obj(EVP_CIPHER_CTX_type(ctx));
        /* Generate a random IV if we need one */
        ivlen = EVP_CIPHER_CTX_iv_length(ctx);
        if (ivlen > 0) {
            if (RAND_bytes_ex(cms_ctx->libctx, iv, ivlen) <= 0)
                goto err;
            piv = iv;
        }
    } else {
        if (evp_cipher_asn1_to_param_ex(ctx, calg->parameter, &aparams) <= 0) {
            CMSerr(0, CMS_R_CIPHER_PARAMETER_INITIALISATION_ERROR);
            goto err;
        }
        if ((EVP_CIPHER_flags(cipher) & EVP_CIPH_FLAG_AEAD_CIPHER)) {
            piv = aparams.iv;
            if (ec->taglen > 0
                    && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                                           ec->taglen, ec->tag) <= 0) {
                CMSerr(0, CMS_R_CIPHER_AEAD_SET_TAG_ERROR);
                goto err;
            }
        }
    }
    len = EVP_CIPHER_CTX_key_length(ctx);
    if (len <= 0)
        goto err;
    tkeylen = (size_t)len;

    /* Generate random session key */
    if (!enc || !ec->key) {
        tkey = OPENSSL_malloc(tkeylen);
        if (tkey == NULL) {
            CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        if (EVP_CIPHER_CTX_rand_key(ctx, tkey) <= 0)
            goto err;
    }

    if (!ec->key) {
        ec->key = tkey;
        ec->keylen = tkeylen;
        tkey = NULL;
        if (enc)
            keep_key = 1;
        else
            ERR_clear_error();

    }

    if (ec->keylen != tkeylen) {
        /* If necessary set key length */
        if (EVP_CIPHER_CTX_set_key_length(ctx, ec->keylen) <= 0) {
            /*
             * Only reveal failure if debugging so we don't leak information
             * which may be useful in MMA.
             */
            if (enc || ec->debug) {
                CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO,
                       CMS_R_INVALID_KEY_LENGTH);
                goto err;
            } else {
                /* Use random key */
                OPENSSL_clear_free(ec->key, ec->keylen);
                ec->key = tkey;
                ec->keylen = tkeylen;
                tkey = NULL;
                ERR_clear_error();
            }
        }
    }

    if (EVP_CipherInit_ex(ctx, NULL, NULL, ec->key, piv, enc) <= 0) {
        CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO,
               CMS_R_CIPHER_INITIALISATION_ERROR);
        goto err;
    }
    if (enc) {
        calg->parameter = ASN1_TYPE_new();
        if (calg->parameter == NULL) {
            CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        if ((EVP_CIPHER_flags(cipher) & EVP_CIPH_FLAG_AEAD_CIPHER)) {
            memcpy(aparams.iv, piv, ivlen);
            aparams.iv_len = ivlen;
            aparams.tag_len = EVP_CIPHER_CTX_tag_length(ctx);
            if (aparams.tag_len <= 0)
                goto err;
        }

        if (evp_cipher_param_to_asn1_ex(ctx, calg->parameter, &aparams) <= 0) {
            CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT_BIO,
                   CMS_R_CIPHER_PARAMETER_INITIALISATION_ERROR);
            goto err;
        }
        /* If parameter type not set omit parameter */
        if (calg->parameter->type == V_ASN1_UNDEF) {
            ASN1_TYPE_free(calg->parameter);
            calg->parameter = NULL;
        }
    }
    ok = 1;

 err:
    EVP_CIPHER_free(fetched_ciph);
    if (!keep_key || !ok) {
        OPENSSL_clear_free(ec->key, ec->keylen);
        ec->key = NULL;
    }
    OPENSSL_clear_free(tkey, tkeylen);
    if (ok)
        return b;
    BIO_free(b);
    return NULL;
}

int cms_EncryptedContent_init(CMS_EncryptedContentInfo *ec,
                              const EVP_CIPHER *cipher,
                              const unsigned char *key, size_t keylen,
                              const CMS_CTX *cms_ctx)
{
    ec->cipher = cipher;
    if (key) {
        if ((ec->key = OPENSSL_malloc(keylen)) == NULL) {
            CMSerr(CMS_F_CMS_ENCRYPTEDCONTENT_INIT, ERR_R_MALLOC_FAILURE);
            return 0;
        }
        memcpy(ec->key, key, keylen);
    }
    ec->keylen = keylen;
    if (cipher != NULL)
        ec->contentType = OBJ_nid2obj(NID_pkcs7_data);
    return 1;
}

int CMS_EncryptedData_set1_key(CMS_ContentInfo *cms, const EVP_CIPHER *ciph,
                               const unsigned char *key, size_t keylen)
{
    CMS_EncryptedContentInfo *ec;

    if (!key || !keylen) {
        CMSerr(CMS_F_CMS_ENCRYPTEDDATA_SET1_KEY, CMS_R_NO_KEY);
        return 0;
    }
    if (ciph) {
        cms->d.encryptedData = M_ASN1_new_of(CMS_EncryptedData);
        if (!cms->d.encryptedData) {
            CMSerr(CMS_F_CMS_ENCRYPTEDDATA_SET1_KEY, ERR_R_MALLOC_FAILURE);
            return 0;
        }
        cms->contentType = OBJ_nid2obj(NID_pkcs7_encrypted);
        cms->d.encryptedData->version = 0;
    } else if (OBJ_obj2nid(cms->contentType) != NID_pkcs7_encrypted) {
        CMSerr(CMS_F_CMS_ENCRYPTEDDATA_SET1_KEY, CMS_R_NOT_ENCRYPTED_DATA);
        return 0;
    }
    ec = cms->d.encryptedData->encryptedContentInfo;
    return cms_EncryptedContent_init(ec, ciph, key, keylen, cms_get0_cmsctx(cms));
}

BIO *cms_EncryptedData_init_bio(const CMS_ContentInfo *cms)
{
    CMS_EncryptedData *enc = cms->d.encryptedData;
    if (enc->encryptedContentInfo->cipher && enc->unprotectedAttrs)
        enc->version = 2;
    return cms_EncryptedContent_init_bio(enc->encryptedContentInfo,
                                         cms_get0_cmsctx(cms));
}
