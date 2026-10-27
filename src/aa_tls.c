/* Android Auto: TLS (see aa_tls.h). Two in-memory BIOs: rbio - what came from the phone, wbio - what to send to it. */
#include "aa_tls.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define SSL_ERROR_WANT_READ 2
#define BIO_CTRL_PENDING 10

static struct {
    int ok;
    void *ctx, *ssl, *rbio, *wbio;
    int (*lib_init)(void);                                   /* 1.0: SSL_library_init */
    int (*init_ssl)(unsigned long long, const void *);       /* 1.1+: OPENSSL_init_ssl */
    const void *(*method)(void);
    void *(*ctx_new)(const void *);
    void (*ctx_free)(void *);
    void (*sec_level)(void *, int);                           /* 1.1+: SSL_CTX_set_security_level */
    void *(*ssl_new)(void *);
    void (*ssl_free)(void *);
    void (*set_bio)(void *, void *, void *);
    void (*set_connect)(void *);
    int (*handshake)(void *);
    int (*get_error)(const void *, int);
    int (*ssl_read)(void *, void *, int);
    int (*ssl_write)(void *, const void *, int);
    int (*use_cert)(void *, void *);
    int (*use_key)(void *, void *);
    const void *(*s_mem)(void);
    void *(*bio_new)(const void *);
    void *(*bio_mem_buf)(const void *, int);
    int (*bio_free)(void *);
    int (*bio_read)(void *, void *, int);
    int (*bio_write)(void *, const void *, int);
    long (*bio_ctrl)(void *, int, long, void *);
    void *(*pem_x509)(void *, void *, void *, void *);
    void *(*pem_key)(void *, void *, void *, void *);
    unsigned long (*err_get)(void);
    void (*err_str)(unsigned long, char *, size_t);
} S;

static void *sym(void *a, void *b, const char *n) { void *p = dlsym(a, n); return p ? p : dlsym(b, n); }

static void ssl_errors(const char *what)
{
    unsigned long e;
    char b[256];
    while (S.err_get && (e = S.err_get())) { S.err_str(e, b, sizeof b); fprintf(stderr, "hu: aa: tls %s: %s\n", what, b); }
}

static int load(void)
{
    static const char *SSL_N[] = { "libssl.so.1.0.0", "libssl.so.3", "libssl.so" };
    static const char *CRY_N[] = { "libcrypto.so.1.0.0", "libcrypto.so.3", "libcrypto.so" };
    void *ls = NULL, *lc = NULL;
    unsigned i;
    for (i = 0; i < 3 && !ls; i++) if ((ls = dlopen(SSL_N[i], RTLD_NOW | RTLD_GLOBAL))) lc = dlopen(CRY_N[i], RTLD_NOW | RTLD_GLOBAL);
    if (!ls || !lc) { fprintf(stderr, "hu: aa: no OpenSSL\n"); return -1; }
    *(void **)&S.lib_init = sym(ls, lc, "SSL_library_init");
    *(void **)&S.init_ssl = sym(ls, lc, "OPENSSL_init_ssl");
    if (!(*(void **)&S.method = sym(ls, lc, "TLSv1_2_client_method"))) *(void **)&S.method = sym(ls, lc, "TLS_client_method");
    *(void **)&S.ctx_new = sym(ls, lc, "SSL_CTX_new");
    *(void **)&S.ctx_free = sym(ls, lc, "SSL_CTX_free");
    *(void **)&S.sec_level = sym(ls, lc, "SSL_CTX_set_security_level");
    *(void **)&S.ssl_new = sym(ls, lc, "SSL_new");
    *(void **)&S.ssl_free = sym(ls, lc, "SSL_free");
    *(void **)&S.set_bio = sym(ls, lc, "SSL_set_bio");
    *(void **)&S.set_connect = sym(ls, lc, "SSL_set_connect_state");
    *(void **)&S.handshake = sym(ls, lc, "SSL_do_handshake");
    *(void **)&S.get_error = sym(ls, lc, "SSL_get_error");
    *(void **)&S.ssl_read = sym(ls, lc, "SSL_read");
    *(void **)&S.ssl_write = sym(ls, lc, "SSL_write");
    *(void **)&S.use_cert = sym(ls, lc, "SSL_use_certificate");
    *(void **)&S.use_key = sym(ls, lc, "SSL_use_PrivateKey");
    *(void **)&S.s_mem = sym(ls, lc, "BIO_s_mem");
    *(void **)&S.bio_new = sym(ls, lc, "BIO_new");
    *(void **)&S.bio_mem_buf = sym(ls, lc, "BIO_new_mem_buf");
    *(void **)&S.bio_free = sym(ls, lc, "BIO_free");
    *(void **)&S.bio_read = sym(ls, lc, "BIO_read");
    *(void **)&S.bio_write = sym(ls, lc, "BIO_write");
    *(void **)&S.bio_ctrl = sym(ls, lc, "BIO_ctrl");
    *(void **)&S.pem_x509 = sym(ls, lc, "PEM_read_bio_X509");
    *(void **)&S.pem_key = sym(ls, lc, "PEM_read_bio_PrivateKey");
    *(void **)&S.err_get = sym(ls, lc, "ERR_get_error");
    *(void **)&S.err_str = sym(ls, lc, "ERR_error_string_n");
    if ((!S.lib_init && !S.init_ssl) || !S.method || !S.ctx_new || !S.ssl_new || !S.set_bio || !S.handshake || !S.ssl_read ||
        !S.ssl_write || !S.use_cert || !S.use_key || !S.s_mem || !S.bio_new || !S.bio_mem_buf || !S.bio_read || !S.bio_write ||
        !S.bio_ctrl || !S.pem_x509 || !S.pem_key || !S.get_error || !S.set_connect) { fprintf(stderr, "hu: aa: OpenSSL: missing symbols\n"); return -1; }
    if (S.lib_init) S.lib_init(); else S.init_ssl(0, NULL);
    return 0;
}

int aa_tls_init(const char *cert_pem, const char *key_pem)
{
    void *b, *x, *k;
    if (!S.ok) { if (load() < 0) return -1; S.ok = 1; }
    aa_tls_free();
    if (!(S.ctx = S.ctx_new(S.method()))) { ssl_errors("ctx"); return -1; }
    if (S.sec_level) S.sec_level(S.ctx, 0);                   /* OpenSSL 3: the HU certificate is old - otherwise it is rejected */
    if (!(S.ssl = S.ssl_new(S.ctx))) { ssl_errors("new"); return -1; }
    b = S.bio_mem_buf(cert_pem, -1); x = S.pem_x509(b, NULL, NULL, NULL); S.bio_free(b);
    b = S.bio_mem_buf(key_pem, -1); k = S.pem_key(b, NULL, NULL, NULL); S.bio_free(b);
    if (!x || !k || S.use_cert(S.ssl, x) != 1 || S.use_key(S.ssl, k) != 1) { ssl_errors("cert"); return -1; }
    S.rbio = S.bio_new(S.s_mem()); S.wbio = S.bio_new(S.s_mem());
    S.set_bio(S.ssl, S.rbio, S.wbio);                         /* the BIOs now belong to SSL */
    S.set_connect(S.ssl);
    return 0;
}

static size_t drain(void *out, size_t cap)
{
    long p = S.bio_ctrl(S.wbio, BIO_CTRL_PENDING, 0, NULL);
    int n;
    if (p <= 0) return 0;
    if ((size_t)p > cap) p = (long)cap;
    n = S.bio_read(S.wbio, out, (int)p);
    return n > 0 ? (size_t)n : 0;
}

int aa_tls_handshake(const void *in, size_t n, void *out, size_t cap, size_t *out_n)
{
    int r;
    if (!S.ssl) return -1;
    if (in && n) S.bio_write(S.rbio, in, (int)n);
    r = S.handshake(S.ssl);
    *out_n = drain(out, cap);
    if (r == 1) return 1;
    if (S.get_error(S.ssl, r) == SSL_ERROR_WANT_READ) return 0;
    ssl_errors("handshake");
    return -1;
}

int aa_tls_encrypt(const void *in, size_t n, void *out, size_t cap)
{
    if (!S.ssl || S.ssl_write(S.ssl, in, (int)n) != (int)n) { ssl_errors("write"); return -1; }
    return (int)drain(out, cap);
}

int aa_tls_decrypt(const void *in, size_t n, void *out, size_t cap)
{
    int r, got = 0;
    if (!S.ssl) return -1;
    S.bio_write(S.rbio, in, (int)n);
    while ((size_t)got < cap && (r = S.ssl_read(S.ssl, (char *)out + got, (int)(cap - got))) > 0) got += r;
    return got;
}

void aa_tls_free(void)
{
    if (S.ssl) S.ssl_free(S.ssl);
    if (S.ctx && S.ctx_free) S.ctx_free(S.ctx);
    S.ssl = S.ctx = S.rbio = S.wbio = NULL;
}
