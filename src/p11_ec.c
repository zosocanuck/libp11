/* libp11, a simple layer on to of PKCS#11 API
 * Copyright (C) 2005 Olaf Kirch <okir@lst.de>
 * Copyright (C) 2011, 2013 Douglas E. Engert <deengert@anl.gov>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

/*
 * This file implements the handling of EC keys stored on a
 * PKCS11 token
 */

#include <config.h>
#include <string.h>
#include <openssl/opensslv.h>
#include <openssl/opensslconf.h>

#define LIBP11_BUILD_WITHOUT_ECDSA
#if !defined(OPENSSL_NO_EC) && !defined(OPENSSL_NO_ECDSA)  && OPENSSL_VERSION_NUMBER >= 0x1000100f
#undef LIBP11_BUILD_WITHOUT_ECDSA

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>

/* To build this mode,
 * OpenSSL has ECDSA_METHOD defined in internal header file ecs_locl.h
 * Until this is resolved use something like:
 * CPPFLAGS="-DBUILD_WITH_ECS_LOCL_H -I/path.to.openssl-1.0.1e/src/crypto/ecdsa"
 * See OpenSSL bug report #2459 02/23/2011
 * Once OpenSSL addresses the issues this code will be changed.
 *
 * OpenSSL mods are being submitted 09/2013 that will set ECDSA_F_ECDSA_METHOD_NEW
 * and define the ECDSA_METHOD_new and friends functions
 * We will try both methods. 
 */

#if defined(ECDSA_F_ECDSA_METHOD_NEW) && defined(BUILD_WITH_ECS_LOCL_H)
     #warning "Both BUILD_WITH_ECS_LOCL_H and ECDSA_F_ECDSA_METHOD_NEW defined"
     #warning "Consider not using BUILD_WITH_ECS_LOCL_H"
#endif

#if defined(BUILD_WITH_ECS_LOCL_H)
    #include "ecs_locl.h"

    #if !defined(HEADER_ECS_LOCL_H)
        #warning "Unable to find OpenSSL src/crypto/ecs_locl.h"
        #warning "add to CPPFLAGS: -I/path/to/source/openssl-n.n.n/src/crypto/ecdsa"
        #warning "or copy ecs_locl.h  or create symlink to it"
        #if defined(ECDSA_F_ECDSA_METHOD_NEW)
            #warning "Will build instead using ECDSA_F_ECDSA_METHOD_NEW"
	#else
	    #error " Unable to build with ECDSA support"
        #endif
    #else
        #define LIBP11_BUILD_WITH_ECS_LOCL_H
    #endif
#else
    #if !defined(ECDSA_F_ECDSA_METHOD_NEW)
        #define LIBP11_BUILD_WITHOUT_ECDSA
    #endif
#endif

#endif /* OpenSSL EC tests and version */  

#if !defined(LIBP11_BUILD_WITHOUT_ECDSA)

#include "libp11-int.h"

static int pkcs11_get_ec_public(PKCS11_KEY *, EVP_PKEY *);
static int pkcs11_get_ec_private(PKCS11_KEY *, EVP_PKEY *);

static ECDSA_METHOD *ops = NULL;

/*
 * Get EC key material and stach pointer in ex_data
 * Note we get called twice, once for private key, and once for public
 * We need to get the EC_PARAMS and EC_POINT into both,
 * as lib11 dates from RSA only where all the pub key components
 * were also part of the privite key.  With EC the point
 * is not in the privite key, and the params may or may not be.
 *
 */
static int pkcs11_get_ec_private(PKCS11_KEY * key, EVP_PKEY * pk)
{
	CK_BBOOL sensitive, extractable;
	EC_KEY * ec = NULL;
	CK_RV ckrv;
	int rv;
	size_t ec_paramslen = 0;
	CK_BYTE * ec_params = NULL;
	size_t ec_pointlen = 0;
	CK_BYTE * ec_point = NULL;
	PKCS11_KEY * prkey;
	PKCS11_KEY * pubkey;
	ASN1_OCTET_STRING *os=NULL;

	if (key->isPrivate) {  /* Are we being called for the prive or pub key */
		prkey = key;
		pubkey = PKCS11_find_key_from_key(key);
	} else {
		pubkey = key;
		prkey = PKCS11_find_key_from_key(key);
	}


	if (!(ec = EVP_PKEY_get1_EC_KEY(pk))) {
		ERR_clear_error();	/* the above flags an error */
		ec = EC_KEY_new();
		EVP_PKEY_set1_EC_KEY(pk, ec);
	}

	if (prkey) {
		if (key_getattr(prkey, CKA_SENSITIVE, &sensitive, sizeof(sensitive))
		    || key_getattr(prkey, CKA_EXTRACTABLE, &extractable, sizeof(extractable))) {
			EC_KEY_free(ec);
			return -1;
		}

		/* For Openssl req we need at least the 
		 * EC_KEY_get0_group(ec_key)) to return the group. 
		 * Even if it fails will continue as a sign only does not need
		 * need this if the pkcs11 or card can figure this out.  
		 */

		if (key_getattr_var(prkey, CKA_EC_PARAMS, NULL, &ec_paramslen) == CKR_OK &&
				ec_paramslen > 0) {
			ec_params = OPENSSL_malloc(ec_paramslen);
			if (ec_params) {
			    ckrv = key_getattr_var(prkey, CKA_EC_PARAMS, ec_params, &ec_paramslen);
			    if (ckrv == CKR_OK) {
				const unsigned char * a = ec_params;
				    /* convert to OpenSSL parmas */
				    d2i_ECParameters(&ec, &a, ec_paramslen);
			    }
			}
		}
	}

	/* Now get the ec_point */

	if (pubkey) {
		if (key_getattr_var(pubkey, CKA_EC_POINT, NULL, &ec_pointlen) == CKR_OK &&
					ec_pointlen > 0) {
			ec_point = OPENSSL_malloc(ec_pointlen);
			if (ec_point) {
			    ckrv = key_getattr_var(pubkey, CKA_EC_POINT, ec_point, &ec_pointlen);
			    if (ckrv == CKR_OK) {
				/* PKCS#11 returns ASN1 octstring*/
				const unsigned char * a;
				/*  we have asn1 octet string, need to strip off 04 len */

				a = ec_point;
				os = d2i_ASN1_OCTET_STRING(NULL, &a, ec_pointlen);
				if (os) {
				    a = os->data;
				    o2i_ECPublicKey(&ec, &a, os->length);
				}
/* EC_KEY_print_fp(stderr, ec, 5); */
			    }
			}
		}
	}

	/* If the key is not extractable, create a key object
	 * that will use the card's functions to sign & decrypt
	 */
	if (os)
	    M_ASN1_OCTET_STRING_free(os);
	if (ec_point)
		OPENSSL_free(ec_point);
	if (ec_params)
		OPENSSL_free(ec_params);

	if (sensitive || !extractable) {
		ECDSA_set_ex_data(ec, 0, key);
		return 0;
	}

	return -1;
}

static int pkcs11_get_ec_public(PKCS11_KEY * key, EVP_PKEY * pk)
{
	/* TBD */
	return pkcs11_get_ec_private(key, pk);
}

/* TODO Looks like this is never called */
static int pkcs11_ecdsa_sign_setup(EC_KEY *ec, BN_CTX *ctx_in,
	BIGNUM **kinvp, BIGNUM **rp) {


	if (*kinvp != NULL)
		BN_clear_free(*kinvp);
	*kinvp = BN_new();

	if (*rp != NULL)
		BN_clear_free(*rp);
	*rp = BN_new();
	return 1;
}

static ECDSA_SIG * pkcs11_ecdsa_do_sign(const unsigned char *dgst, int dlen,
			const BIGNUM *inv, const BIGNUM *r, EC_KEY * ec)
{
	int type;
	unsigned char sigret[512]; /* HACK for now */
	ECDSA_SIG * sig = NULL;
	PKCS11_KEY * key = NULL;
	int siglen;
	int nLen = 48; /* HACK */;
	int rv;

	key = (PKCS11_KEY *) ECDSA_get_ex_data(ec, 0);
	if  (key == NULL)
		return NULL;

	siglen = sizeof(sigret);

	rv = PKCS11_ecdsa_sign(dgst,dlen,sigret,&siglen, key);
	nLen = siglen / 2;
	if (rv > 0) {
		sig = ECDSA_SIG_new();
		if (sig) {
			BN_bin2bn(&sigret[0], nLen, sig->r);
			BN_bin2bn(&sigret[nLen], nLen, sig->s);
		}
	}
	return sig;
}

/*
 * Overload the default OpenSSL methods for ECDSA
 * If OpenSSL supports  ECDSA_METHOD_new we will use it.
 * Otherwise we expect the ecs_locl.h to be present.
 * Mods have been submitted to to define ECDSA_F_ECDSA_METHOD_NEW
 * If present use it.
 */
#if !defined(LIBP11_BUILD_WITH_ECS_LOCL_H)
#warning "DEE - building  new "

/* New  way to allocate  a ECDSA_METOD... */
ECDSA_METHOD *PKCS11_get_ecdsa_method(void)
{

    if (ops == NULL) {
	ops = ECDSA_METHOD_new(NULL);
	ECDSA_METHOD_set_sign(ops, pkcs11_ecdsa_do_sign);
	ECDSA_METHOD_set_sign_setup(ops, pkcs11_ecdsa_sign_setup);
    }
    return ops;
}

void PKCS11_ecdsa_method_free(void)
{
	if (ops) {
	ECDSA_METHOD_free(ops);
	ops = NULL;
	}
}

#else

#warning "DEE - building old way with ecs_locl.h"
/* old way using ecs_locl.h */
ECDSA_METHOD *PKCS11_get_ecdsa_method(void)
{
	static struct ecdsa_method sops;

	if (!sops.ecdsa_do_sign) {
/* question if compiler is copying each  member of struct or not */
		sops = *ECDSA_get_default_method();
		sops.ecdsa_do_sign = pkcs11_ecdsa_do_sign;
		sops.ecdsa_sign_setup = pkcs11_ecdsa_sign_setup;
	}
	return &sops;
}

void PKCS11_ecdsa_method_free(void)
{
    /* in old method it is static */
}
#endif

PKCS11_KEY_ops pkcs11_ec_ops = {
	EVP_PKEY_EC,
	pkcs11_get_ec_public,
	pkcs11_get_ec_private
};

#else /* LIBP11_BUILD_WITHOUT_ECDSA */
/* if not built with EC or OpenSSL does not support ECDSA
 * add these so engine_pkcs11 can be built now and not
 * require further changes */
#warning  "ECDSA support not built with libp11"
void * PKCS11_get_ecdsa_method(void)
{
	return NULL;
}
void PKCS11_ecdsa_method_free(void)
{
 /* no op, as it is static in old code. */
}
#endif /* OPENSSL_NO_EC */
