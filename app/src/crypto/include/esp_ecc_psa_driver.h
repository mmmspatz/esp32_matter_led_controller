/* SPDX-License-Identifier: Apache-2.0 */

/*
 * PSA transparent-driver entry points backing P-256 (secp256r1) with the
 * ESP32-C6 ECC point-multiplication accelerator. Dispatch into these is
 * wired by tf-psa-crypto-patches/0001; signatures follow the PSA driver
 * entry-point contract (keys arrive in PSA export representation: 32-byte
 * big-endian private scalar / 65-byte 0x04||X||Y public point).
 *
 * This header is included by tf-psa-crypto's driver wrappers, so it must
 * stay C and self-contained.
 */

#ifndef ESP_ECC_PSA_DRIVER_H
#define ESP_ECC_PSA_DRIVER_H

#if defined(MBEDTLS_PSA_ESP_ECC_DRIVER_ENABLED)
#ifndef PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT
#define PSA_CRYPTO_ACCELERATOR_DRIVER_PRESENT
#endif
#endif

#include "psa/crypto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

psa_status_t esp_ecc_transparent_export_public_key(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	uint8_t *data, size_t data_size, size_t *data_length);

psa_status_t esp_ecc_transparent_generate_key(
	const psa_key_attributes_t *attributes,
	uint8_t *key_buffer, size_t key_buffer_size,
	size_t *key_buffer_length);

psa_status_t esp_ecc_transparent_key_agreement(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	psa_algorithm_t alg,
	const uint8_t *peer_key, size_t peer_key_length,
	uint8_t *shared_secret, size_t shared_secret_size,
	size_t *shared_secret_length);

psa_status_t esp_ecc_transparent_sign_hash(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	psa_algorithm_t alg,
	const uint8_t *hash, size_t hash_length,
	uint8_t *signature, size_t signature_size, size_t *signature_length);

psa_status_t esp_ecc_transparent_verify_hash(
	const psa_key_attributes_t *attributes,
	const uint8_t *key_buffer, size_t key_buffer_size,
	psa_algorithm_t alg,
	const uint8_t *hash, size_t hash_length,
	const uint8_t *signature, size_t signature_length);

#ifdef __cplusplus
}
#endif

#endif /* ESP_ECC_PSA_DRIVER_H */
