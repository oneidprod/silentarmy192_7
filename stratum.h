#ifndef STRATUM_H
#define STRATUM_H

#include <stdint.h>
#include <pthread.h>

/* Zero coin Stratum protocol constants */
#define STRATUM_HEADER_LEN   108   /* version(4)+prevhash(32)+merkle(32)+reserved(32)+ntime(4)+nbits(4) */
#define STRATUM_JOB_ID_LEN   64
#define STRATUM_NTIME_LEN    9    /* 8 hex chars + NUL */
#define STRATUM_NONCE1_MAXLEN 16  /* up to 8 bytes = 16 hex chars */
#define STRATUM_RECV_BUF     8192

typedef struct {
    char     job_id[STRATUM_JOB_ID_LEN];
    uint8_t  header[STRATUM_HEADER_LEN];   /* binary header, ready for blake */
    char     ntime[STRATUM_NTIME_LEN];     /* ntime as 8-char hex (for submit) */
    int      clean;
    int      valid;
} stratum_job_t;

typedef struct {
    /* connection */
    int      sockfd;
    char     host[256];
    char     port[16];
    char     user[256];
    char     pass[256];

    /* server-assigned nonce1 prefix (binary) */
    uint8_t  nonce1[8];
    int      nonce1_len;    /* bytes (typically 2-4) */
    int      nonce2_size;   /* bytes the pool expects for nonce2 field */

    /* current job — protected by job_mutex */
    stratum_job_t  job;
    pthread_mutex_t job_mutex;

    /* mining cancel flag — set by recv thread on clean_jobs=true notify */
    volatile int   cancel;

    /* state */
    int      authorized;
    int      connected;
    int      submit_id;   /* next share id (starts at 4) */

    /* line receive buffer */
    char     recv_buf[STRATUM_RECV_BUF];
    int      recv_len;
} stratum_ctx_t;

/* Initialize context with connection parameters. Does NOT connect. */
void stratum_init(stratum_ctx_t *ctx, const char *host, const char *port,
                  const char *user, const char *pass);

/*
 * Connect TCP socket, send subscribe + authorize.
 * Blocks until authorized or an error occurs.
 * Returns 0 on success, -1 on error.
 */
int stratum_connect(stratum_ctx_t *ctx);

/*
 * Receive and process one newline-terminated JSON message (blocking).
 * Updates ctx->job and ctx->cancel as appropriate.
 * Returns 0 on success, -1 on disconnect/error.
 */
int stratum_recv_line(stratum_ctx_t *ctx);

/*
 * Copy current job into *dst under mutex.
 * Returns 1 if a valid job exists, 0 otherwise.
 */
int stratum_get_job(stratum_ctx_t *ctx, stratum_job_t *dst);

/*
 * Submit a found solution to the pool.
 * nonce2_val: the miner-iterated 4-byte nonce2 (little-endian in headernonce)
 * sol_hex:    800-char hex string of compressed solution (no fd9001 prefix)
 * Returns 0 on success, -1 on send error.
 */
int stratum_submit(stratum_ctx_t *ctx, const char *job_id, const char *ntime,
                   uint32_t nonce2_val, const char *sol_hex);

/* Close socket and free resources. */
void stratum_disconnect(stratum_ctx_t *ctx);

#endif /* STRATUM_H */
