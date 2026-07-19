/* SPDX-License-Identifier: Apache-2.0 */

/*
 * PSA transparent driver backing P-256 with the ESP32-C6 ECC accelerator.
 *
 * The engine does two things: affine point multiplication and point-on-curve
 * verification (optionally fused). That covers ECDH outright and the k*G /
 * d*G multiplications of ECDSA signing and public-key derivation; the
 * remaining scalar arithmetic (r, s mod n) runs on the builtin bignum.
 * ECDSA *verification* runs u1*G and u2*Q on the engine plus one software
 * affine addition (the engine has no point add); its measure-zero corner
 * cases — and every other curve/algorithm — fall through to the builtin
 * software path via the wrappers' fallback dispatch. The remaining verify
 * cost is dominated by two mbedtls_mpi_inv_mod calls (s^-1 mod n, the
 * point-add denominator mod p); the C6's MPI mod-exp block could do Fermat
 * inversion if that ever needs to shrink further.
 *
 * Keys arrive in PSA export representation: 32-byte big-endian private
 * scalar, 65-byte 0x04||X||Y public point. The engine wants little-endian,
 * so every parameter is byte-reversed on the way in/out (the hal's
 * ecc_alt.c does the same dance for ESP-IDF).
 *
 * Side-channel posture: the C6 engine has no constant-time point-mul mode
 * (ecc_ll_enable_constant_time_point_mul is a stub), so scalars are exposed
 * to whatever timing profile the engine has — the same posture as
 * Espressif's production ESP-IDF port on this chip. In Matter terms the
 * scalars that cross the engine are one-shot (CASE-ephemeral ECDH keys,
 * per-signature ECDSA nonces, one d*G at key birth); the long-lived NOC/DAC
 * private keys are only ever *multiplied* as fresh nonces, and the mod-n
 * inversion that touches them is blinded below, mirroring the builtin
 * ecdsa.c countermeasure.
 *
 * Validation split: the rejection-sampling and range comparisons here are
 * plain memcmp-class code (non-constant-time); they leak only whether a
 * candidate was resampled or a public input was invalid, not secret bits.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "psa/crypto.h"

#include <mbedtls/platform_util.h>
#include <mbedtls/private/bignum.h>

#include <soc/soc_caps.h>
#include "ecc_impl.h"

#include "esp_ecc_psa_driver.h"

#if !defined(MBEDTLS_PSA_ESP_ECC_DRIVER_ENABLED)
#error "esp_ecc_psa_driver.c compiled but MBEDTLS_PSA_ESP_ECC_DRIVER_ENABLED " \
       "is not defined -- check the LEDCTRL_PSA_ESP_ECC_DRIVER CMake glue"
#endif

#define PRIVKEY_LEN		32u
#define PSA_PUBKEY_LEN		65u
#define PSA_PUBKEY_HEADER_BYTE	0x04
#define SHARED_SECRET_LEN	32u
#define SIGNATURE_LEN		64u

/* secp256r1 domain parameters, big-endian (SEC 2 v2.0, section 2.4.2). */
static const uint8_t p256_p_be[PRIVKEY_LEN] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t p256_n_be[PRIVKEY_LEN] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
	0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};
static const uint8_t p256_gx_be[PRIVKEY_LEN] = {
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
	0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
	0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
	0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
};
static const uint8_t p256_gy_be[PRIVKEY_LEN] = {
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b,
	0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
	0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
	0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
};

static void reverse_bytes(uint8_t *dst, const uint8_t *src, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		dst[i] = src[len - 1 - i];
	}
}

/* Fixed-width big-endian: lexicographic order == numeric order. */
static bool be_lt(const uint8_t a[PRIVKEY_LEN], const uint8_t b[PRIVKEY_LEN])
{
	return memcmp(a, b, PRIVKEY_LEN) < 0;
}

static bool be_is_zero(const uint8_t a[PRIVKEY_LEN])
{
	uint8_t acc = 0;

	for (size_t i = 0; i < PRIVKEY_LEN; i++) {
		acc |= a[i];
	}
	return acc == 0;
}

/* 1 <= d < n */
static bool scalar_be_in_range(const uint8_t d_be[PRIVKEY_LEN])
{
	return !be_is_zero(d_be) && be_lt(d_be, p256_n_be);
}

static bool is_p256_keypair(const psa_key_attributes_t *attributes)
{
	return psa_get_key_type(attributes) ==
		       PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) &&
	       psa_get_key_bits(attributes) == 256;
}

/* Uniform scalar in [1, n-1] by rejection sampling (~2^-32 reject rate,
 * so the retry cap only ever fires on a broken RNG). */
static psa_status_t random_scalar_be(uint8_t out_be[PRIVKEY_LEN])
{
	for (int i = 0; i < 32; i++) {
		psa_status_t status = psa_generate_random(out_be, PRIVKEY_LEN);

		if (status != PSA_SUCCESS) {
			return status;
		}
		if (scalar_be_in_range(out_be)) {
			return PSA_SUCCESS;
		}
	}
	return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

/* R = scalar * (Px, Py) on the engine; big-endian in/out. With
 * verify_on_curve the engine first checks the point is on the curve and
 * refuses the multiplication if not (VERIFY_THEN_POINT_MUL mode). */
static psa_status_t hw_point_mul(const uint8_t scalar_be[PRIVKEY_LEN],
				 const uint8_t px_be[PRIVKEY_LEN],
				 const uint8_t py_be[PRIVKEY_LEN],
				 bool verify_on_curve,
				 uint8_t rx_be[PRIVKEY_LEN],
				 uint8_t ry_be[PRIVKEY_LEN])
{
	ecc_point_t point;
	ecc_point_t result;
	uint8_t scalar_le[PRIVKEY_LEN];
	int ret;

	memset(&point, 0, sizeof(point));
	point.len = PRIVKEY_LEN;
	reverse_bytes(point.x, px_be, PRIVKEY_LEN);
	reverse_bytes(point.y, py_be, PRIVKEY_LEN);
	reverse_bytes(scalar_le, scalar_be, PRIVKEY_LEN);

	ret = esp_ecc_point_multiply(&point, scalar_le, &result,
				     verify_on_curve);

	mbedtls_platform_zeroize(scalar_le, sizeof(scalar_le));
	if (ret != 0) {
		mbedtls_platform_zeroize(&result, sizeof(result));
		return PSA_ERROR_INVALID_ARGUMENT;
	}

	reverse_bytes(rx_be, result.x, PRIVKEY_LEN);
	reverse_bytes(ry_be, result.y, PRIVKEY_LEN);
	mbedtls_platform_zeroize(&result, sizeof(result));
	return PSA_SUCCESS;
}

static psa_status_t mpi_to_psa_error(int ret)
{
	switch (ret) {
	case 0:
		return PSA_SUCCESS;
	case MBEDTLS_ERR_MPI_ALLOC_FAILED:
		return PSA_ERROR_INSUFFICIENT_MEMORY;
	default:
		return PSA_ERROR_GENERIC_ERROR;
	}
}

psa_status_t esp_ecc_transparent_generate_key(
	const psa_key_attributes_t *attributes,
	uint8_t *key_buffer, size_t key_buffer_size,
	size_t *key_buffer_length)
{
	/* Dispatch already filtered for P-256 key pairs. */
	(void) attributes;

	if (key_buffer_size != PRIVKEY_LEN) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}

	/* PSA's key-pair export representation is the private scalar alone;
	 * the public point is derived on demand through export_public_key
	 * (one engine multiplication there). So generation is pure RNG —
	 * this skips even the d*G the builtin implementation performs.
	 */
	psa_status_t status = random_scalar_be(key_buffer);

	if (status == PSA_SUCCESS) {
		*key_buffer_length = PRIVKEY_LEN;
	}
	return status;
}

psa_status_t esp_ecc_transparent_export_public_key(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	uint8_t *data, size_t data_size, size_t *data_length)
{
	if (!is_p256_keypair(attributes)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size != PRIVKEY_LEN) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (data_size < PSA_PUBKEY_LEN) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	if (!scalar_be_in_range(key_buffer)) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}

	psa_status_t status = hw_point_mul(key_buffer, p256_gx_be, p256_gy_be,
					   false, &data[1],
					   &data[1 + PRIVKEY_LEN]);

	if (status != PSA_SUCCESS) {
		return status;
	}
	data[0] = PSA_PUBKEY_HEADER_BYTE;
	*data_length = PSA_PUBKEY_LEN;
	return PSA_SUCCESS;
}

psa_status_t esp_ecc_transparent_key_agreement(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	psa_algorithm_t alg,
	const uint8_t *peer_key, size_t peer_key_length,
	uint8_t *shared_secret, size_t shared_secret_size,
	size_t *shared_secret_length)
{
	(void) attributes;

	if (!PSA_ALG_IS_ECDH(alg)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size != PRIVKEY_LEN ||
	    peer_key_length != PSA_PUBKEY_LEN ||
	    peer_key[0] != PSA_PUBKEY_HEADER_BYTE) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (shared_secret_size < SHARED_SECRET_LEN) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	if (!scalar_be_in_range(key_buffer)) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}

	/* Peer coordinates must be field elements; the engine's fused
	 * on-curve check below rejects everything else off the curve.
	 * (Prime-order curve: every on-curve affine point generates the
	 * full group, so no cofactor/low-order concerns.)
	 */
	const uint8_t *peer_x = &peer_key[1];
	const uint8_t *peer_y = &peer_key[1 + PRIVKEY_LEN];

	if (!be_lt(peer_x, p256_p_be) || !be_lt(peer_y, p256_p_be)) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}

	uint8_t shared_y[PRIVKEY_LEN];
	psa_status_t status = hw_point_mul(key_buffer, peer_x, peer_y, true,
					   shared_secret, shared_y);

	mbedtls_platform_zeroize(shared_y, sizeof(shared_y));
	if (status != PSA_SUCCESS) {
		return status;
	}
	/* Raw ECDH shared secret is the x-coordinate, already in place. */
	*shared_secret_length = SHARED_SECRET_LEN;
	return PSA_SUCCESS;
}

psa_status_t esp_ecc_transparent_sign_hash(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	psa_algorithm_t alg,
	const uint8_t *hash, size_t hash_length,
	uint8_t *signature, size_t signature_size, size_t *signature_length)
{
	if (!is_p256_keypair(attributes) || !PSA_ALG_IS_RANDOMIZED_ECDSA(alg)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (key_buffer_size != PRIVKEY_LEN) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (signature_size < SIGNATURE_LEN) {
		return PSA_ERROR_BUFFER_TOO_SMALL;
	}
	if (!scalar_be_in_range(key_buffer)) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}

	/* e = leftmost 256 bits of the hash (SEC 1 v2.0, 4.1.3 step 5);
	 * shorter hashes are used whole. */
	size_t e_len = hash_length < PRIVKEY_LEN ? hash_length : PRIVKEY_LEN;

	psa_status_t status;
	uint8_t k_be[PRIVKEY_LEN];
	uint8_t t_be[PRIVKEY_LEN];
	uint8_t rx_be[PRIVKEY_LEN];
	uint8_t ry_be[PRIVKEY_LEN];
	mbedtls_mpi n, d, e, k, t, r, s, u;
	int ret;

	mbedtls_mpi_init(&n);
	mbedtls_mpi_init(&d);
	mbedtls_mpi_init(&e);
	mbedtls_mpi_init(&k);
	mbedtls_mpi_init(&t);
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);
	mbedtls_mpi_init(&u);

#define ECC_MPI_CHK(f)							\
	do {								\
		ret = (f);						\
		if (ret != 0) {						\
			status = mpi_to_psa_error(ret);			\
			goto cleanup;					\
		}							\
	} while (0)

	ECC_MPI_CHK(mbedtls_mpi_read_binary(&n, p256_n_be, PRIVKEY_LEN));
	ECC_MPI_CHK(mbedtls_mpi_read_binary(&d, key_buffer, PRIVKEY_LEN));
	ECC_MPI_CHK(mbedtls_mpi_read_binary(&e, hash, e_len));

	/* SEC 1 v2.0, 4.1.3: retry on the (cryptographically unreachable)
	 * r == 0 / s == 0 outcomes; a cap turns a broken RNG or engine into
	 * an error instead of a hang. */
	for (int attempt = 0; attempt < 8; attempt++) {
		status = random_scalar_be(k_be);
		if (status != PSA_SUCCESS) {
			goto cleanup;
		}
		status = hw_point_mul(k_be, p256_gx_be, p256_gy_be, false,
				      rx_be, ry_be);
		if (status != PSA_SUCCESS) {
			goto cleanup;
		}

		ECC_MPI_CHK(mbedtls_mpi_read_binary(&r, rx_be, PRIVKEY_LEN));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&r, &r, &n));
		if (mbedtls_mpi_cmp_int(&r, 0) == 0) {
			continue;
		}

		/* s = (e + r*d) / k, computed as t*(e + r*d) * (k*t)^-1 so
		 * the non-constant-time inversion only ever sees the blinded
		 * k*t (the builtin ecdsa.c countermeasure). */
		status = random_scalar_be(t_be);
		if (status != PSA_SUCCESS) {
			goto cleanup;
		}
		ECC_MPI_CHK(mbedtls_mpi_read_binary(&k, k_be, PRIVKEY_LEN));
		ECC_MPI_CHK(mbedtls_mpi_read_binary(&t, t_be, PRIVKEY_LEN));

		ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&s, &r, &d));
		ECC_MPI_CHK(mbedtls_mpi_add_mpi(&s, &s, &e));
		ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&s, &s, &t));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&s, &s, &n));
		ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&u, &k, &t));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&u, &u, &n));
		ECC_MPI_CHK(mbedtls_mpi_inv_mod(&u, &u, &n));
		ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&s, &s, &u));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&s, &s, &n));
		if (mbedtls_mpi_cmp_int(&s, 0) == 0) {
			continue;
		}

		ECC_MPI_CHK(mbedtls_mpi_write_binary(&r, signature,
						     PRIVKEY_LEN));
		ECC_MPI_CHK(mbedtls_mpi_write_binary(&s,
						     signature + PRIVKEY_LEN,
						     PRIVKEY_LEN));
		*signature_length = SIGNATURE_LEN;
		status = PSA_SUCCESS;
		goto cleanup;
	}
	status = PSA_ERROR_INSUFFICIENT_ENTROPY;

cleanup:
#undef ECC_MPI_CHK
	/* mbedtls_mpi_free zeroizes limbs before releasing them. */
	mbedtls_mpi_free(&n);
	mbedtls_mpi_free(&d);
	mbedtls_mpi_free(&e);
	mbedtls_mpi_free(&k);
	mbedtls_mpi_free(&t);
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&s);
	mbedtls_mpi_free(&u);
	mbedtls_platform_zeroize(k_be, sizeof(k_be));
	mbedtls_platform_zeroize(t_be, sizeof(t_be));
	return status;
}

psa_status_t esp_ecc_transparent_verify_hash(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	psa_algorithm_t alg,
	const uint8_t *hash, size_t hash_length,
	const uint8_t *signature, size_t signature_length)
{
	psa_key_type_t type = psa_get_key_type(attributes);
	bool is_pair = type == PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1);
	bool is_pub =
		type == PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1);

	if ((!is_pair && !is_pub) || psa_get_key_bits(attributes) != 256 ||
	    !PSA_ALG_IS_ECDSA(alg)) {
		return PSA_ERROR_NOT_SUPPORTED;
	}
	if (signature_length != SIGNATURE_LEN) {
		return PSA_ERROR_INVALID_SIGNATURE;
	}

	const uint8_t *r_be = signature;
	const uint8_t *s_be = signature + PRIVKEY_LEN;

	if (!scalar_be_in_range(r_be) || !scalar_be_in_range(s_be)) {
		return PSA_ERROR_INVALID_SIGNATURE;
	}

	/* Q in big-endian affine coordinates. CHIP verifies against imported
	 * public keys; the key-pair form (bare scalar, Q = d*G on the engine)
	 * is the rare path. */
	uint8_t qx_be[PRIVKEY_LEN];
	uint8_t qy_be[PRIVKEY_LEN];
	psa_status_t status;

	if (is_pub) {
		if (key_buffer_size != PSA_PUBKEY_LEN ||
		    key_buffer[0] != PSA_PUBKEY_HEADER_BYTE) {
			return PSA_ERROR_INVALID_ARGUMENT;
		}
		memcpy(qx_be, &key_buffer[1], PRIVKEY_LEN);
		memcpy(qy_be, &key_buffer[1 + PRIVKEY_LEN], PRIVKEY_LEN);
	} else {
		if (key_buffer_size != PRIVKEY_LEN ||
		    !scalar_be_in_range(key_buffer)) {
			return PSA_ERROR_INVALID_ARGUMENT;
		}
		status = hw_point_mul(key_buffer, p256_gx_be, p256_gy_be,
				      false, qx_be, qy_be);
		if (status != PSA_SUCCESS) {
			return status;
		}
	}

	if (!be_lt(qx_be, p256_p_be) || !be_lt(qy_be, p256_p_be)) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}

	/* e as in sign_hash: leftmost 256 bits of the hash. */
	size_t e_len = hash_length < PRIVKEY_LEN ? hash_length : PRIVKEY_LEN;

	/* SEC 1 v2.0, 4.1.4: w = s^-1, u1 = e*w, u2 = r*w (mod n);
	 * R = u1*G + u2*Q; valid iff R.x mod n == r. The engine does the two
	 * multiplications; the single affine addition runs on mbedtls_mpi.
	 * Everything here is public, so no blinding or zeroizing. */
	uint8_t u1_be[PRIVKEY_LEN];
	uint8_t u2_be[PRIVKEY_LEN];
	uint8_t r1x_be[PRIVKEY_LEN];
	uint8_t r1y_be[PRIVKEY_LEN];
	uint8_t r2x_be[PRIVKEY_LEN];
	uint8_t r2y_be[PRIVKEY_LEN];
	mbedtls_mpi n, p, e, r, s, u1, u2, x1, y1, x2, y2, lam, v;
	int ret;

	mbedtls_mpi_init(&n);
	mbedtls_mpi_init(&p);
	mbedtls_mpi_init(&e);
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);
	mbedtls_mpi_init(&u1);
	mbedtls_mpi_init(&u2);
	mbedtls_mpi_init(&x1);
	mbedtls_mpi_init(&y1);
	mbedtls_mpi_init(&x2);
	mbedtls_mpi_init(&y2);
	mbedtls_mpi_init(&lam);
	mbedtls_mpi_init(&v);

#define ECC_MPI_CHK(f)							\
	do {								\
		ret = (f);						\
		if (ret != 0) {						\
			status = mpi_to_psa_error(ret);			\
			goto cleanup;					\
		}							\
	} while (0)

	ECC_MPI_CHK(mbedtls_mpi_read_binary(&n, p256_n_be, PRIVKEY_LEN));
	ECC_MPI_CHK(mbedtls_mpi_read_binary(&p, p256_p_be, PRIVKEY_LEN));
	ECC_MPI_CHK(mbedtls_mpi_read_binary(&e, hash, e_len));
	ECC_MPI_CHK(mbedtls_mpi_read_binary(&r, r_be, PRIVKEY_LEN));
	ECC_MPI_CHK(mbedtls_mpi_read_binary(&s, s_be, PRIVKEY_LEN));

	ECC_MPI_CHK(mbedtls_mpi_inv_mod(&s, &s, &n));
	ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&u1, &e, &s));
	ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&u1, &u1, &n));
	ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&u2, &r, &s));
	ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&u2, &u2, &n));
	ECC_MPI_CHK(mbedtls_mpi_write_binary(&u2, u2_be, PRIVKEY_LEN));

	/* u2 = r/s can't be 0 for r in [1, n-1]; the engine's fused check
	 * also validates Q is on the curve. */
	status = hw_point_mul(u2_be, qx_be, qy_be, true, r2x_be, r2y_be);
	if (status != PSA_SUCCESS) {
		goto cleanup;
	}

	if (mbedtls_mpi_cmp_int(&u1, 0) == 0) {
		/* e == 0 mod n: R = u2*Q outright. */
		ECC_MPI_CHK(mbedtls_mpi_read_binary(&v, r2x_be, PRIVKEY_LEN));
	} else {
		ECC_MPI_CHK(mbedtls_mpi_write_binary(&u1, u1_be, PRIVKEY_LEN));
		status = hw_point_mul(u1_be, p256_gx_be, p256_gy_be, false,
				      r1x_be, r1y_be);
		if (status != PSA_SUCCESS) {
			goto cleanup;
		}

		/* Equal x-coordinates means doubling or point-at-infinity —
		 * cryptographically unreachable for honest inputs. Punt to
		 * the builtin implementation, which handles every case. */
		if (memcmp(r1x_be, r2x_be, PRIVKEY_LEN) == 0) {
			status = PSA_ERROR_NOT_SUPPORTED;
			goto cleanup;
		}

		ECC_MPI_CHK(mbedtls_mpi_read_binary(&x1, r1x_be, PRIVKEY_LEN));
		ECC_MPI_CHK(mbedtls_mpi_read_binary(&y1, r1y_be, PRIVKEY_LEN));
		ECC_MPI_CHK(mbedtls_mpi_read_binary(&x2, r2x_be, PRIVKEY_LEN));
		ECC_MPI_CHK(mbedtls_mpi_read_binary(&y2, r2y_be, PRIVKEY_LEN));

		/* lam = (y2-y1)/(x2-x1); we only need R.x, so y3 is never
		 * computed. */
		ECC_MPI_CHK(mbedtls_mpi_sub_mpi(&lam, &x2, &x1));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&lam, &lam, &p));
		ECC_MPI_CHK(mbedtls_mpi_inv_mod(&lam, &lam, &p));
		ECC_MPI_CHK(mbedtls_mpi_sub_mpi(&v, &y2, &y1));
		ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&lam, &lam, &v));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&lam, &lam, &p));

		ECC_MPI_CHK(mbedtls_mpi_mul_mpi(&v, &lam, &lam));
		ECC_MPI_CHK(mbedtls_mpi_sub_mpi(&v, &v, &x1));
		ECC_MPI_CHK(mbedtls_mpi_sub_mpi(&v, &v, &x2));
		ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&v, &v, &p));
	}

	ECC_MPI_CHK(mbedtls_mpi_mod_mpi(&v, &v, &n));
	status = mbedtls_mpi_cmp_mpi(&v, &r) == 0 ? PSA_SUCCESS
						  : PSA_ERROR_INVALID_SIGNATURE;

cleanup:
#undef ECC_MPI_CHK
	mbedtls_mpi_free(&n);
	mbedtls_mpi_free(&p);
	mbedtls_mpi_free(&e);
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&s);
	mbedtls_mpi_free(&u1);
	mbedtls_mpi_free(&u2);
	mbedtls_mpi_free(&x1);
	mbedtls_mpi_free(&y1);
	mbedtls_mpi_free(&x2);
	mbedtls_mpi_free(&y2);
	mbedtls_mpi_free(&lam);
	mbedtls_mpi_free(&v);
	return status;
}
