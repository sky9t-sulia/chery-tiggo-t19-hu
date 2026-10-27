/* Android Auto: TLS over AA messages (OpenSSL via dlopen: 1.0.0 on the HU, 3.x on PC; no headers).
 * The HU is the TLS client, with the HU certificate. TLS bytes travel in AA messages (SSL_HANDSHAKE), and after the handshake
 * the frame payload is encrypted - through memory (BIO), without a socket. */
#ifndef AA_TLS_H
#define AA_TLS_H

#include <stddef.h>

int  aa_tls_init(const char *cert_pem, const char *key_pem);   /* 0 - ready; -1 - no OpenSSL/error */
/* handshake: in - what the phone sent (NULL at start); out - what to send; 1 - done, 0 - continue, -1 - error */
int  aa_tls_handshake(const void *in, size_t n, void *out, size_t cap, size_t *out_n);
int  aa_tls_encrypt(const void *in, size_t n, void *out, size_t cap);   /* ciphertext bytes or -1 */
int  aa_tls_decrypt(const void *in, size_t n, void *out, size_t cap);   /* plaintext bytes or -1 */
void aa_tls_free(void);

#endif
