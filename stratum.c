/*
 * stratum.c — Pure POSIX C Stratum client for Zero coin (Equihash 192,7)
 *
 * Protocol reference: nheqminer/libstratum/StratumClient.cpp
 * Submit format:  mining.submit(user, job_id, ntime, nonce2_hex, solution_hex)
 * Notify format:  params = [job_id, version(8h), prevhash(64h), merkle(64h),
 *                           reserved(64h), ntime(8h), nbits(8h), clean_jobs]
 */

#define _POSIX_C_SOURCE 200809L
#include "stratum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <endian.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Decode two hex chars at s[0..1] into one byte. Returns -1 on bad input. */
static int hex2byte(const char *s)
{
    int hi, lo;
    if (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else return -1;
    if (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else return -1;
    return (hi << 4) | lo;
}

/* Decode len hex chars from hex into out (len/2 bytes). Returns 0 on success. */
static int hex_decode(const char *hex, int len, uint8_t *out)
{
    for (int i = 0; i < len / 2; i++) {
        int b = hex2byte(hex + 2 * i);
        if (b < 0) return -1;
        out[i] = (uint8_t)b;
    }
    return 0;
}

/*
 * Extract the Nth (0-based) string element from a JSON array string like
 *   ["a","b","c",...]
 * Returns 1 on success.
 */
static int json_array_get_str(const char *array_start, int n,
                               char *out, size_t maxlen)
{
    const char *p = array_start;
    int elem = 0;
    while (*p) {
        /* find next '"' */
        p = strchr(p, '"');
        if (!p) return 0;
        p++; /* skip opening quote */
        if (elem == n) {
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < maxlen)
                out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        /* skip to closing quote */
        p = strchr(p, '"');
        if (!p) return 0;
        p++; /* skip closing quote */
        elem++;
    }
    return 0;
}

/*
 * Check if the Nth element of a JSON array is the boolean true.
 */
static int json_array_get_bool(const char *array_start, int n)
{
    const char *p = array_start;
    int elem = 0;
    while (*p) {
        /* skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') p++;
        if (*p == '[' || *p == ']') { p++; continue; }
        if (elem == n) {
            return (strncmp(p, "true", 4) == 0);
        }
        /* skip element: string or non-string */
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p) p++;
        } else {
            while (*p && *p != ',' && *p != ']') p++;
        }
        elem++;
    }
    return 0;
}

/* Send a string over the socket. Returns 0 on success, -1 on error. */
static int sock_send(int fd, const char *msg)
{
    size_t len = strlen(msg);
    while (len > 0) {
        ssize_t n = write(fd, msg, len);
        if (n <= 0) return -1;
        msg += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * Read one '\n'-terminated line from the socket into line_buf.
 * Uses ctx->recv_buf as a lookahead buffer.
 * Returns length of line (excluding '\n') on success, -1 on disconnect.
 */
static int recv_line(stratum_ctx_t *ctx, char *line_buf, int maxlen)
{
    while (1) {
        /* scan existing buffer for '\n' */
        for (int i = 0; i < ctx->recv_len; i++) {
            if (ctx->recv_buf[i] == '\n') {
                int len = i < maxlen - 1 ? i : maxlen - 1;
                memcpy(line_buf, ctx->recv_buf, (size_t)len);
                line_buf[len] = '\0';
                /* shift buffer */
                ctx->recv_len -= i + 1;
                memmove(ctx->recv_buf, ctx->recv_buf + i + 1, (size_t)ctx->recv_len);
                return len;
            }
        }
        /* need more data */
        int space = STRATUM_RECV_BUF - ctx->recv_len - 1;
        if (space <= 0) return -1; /* buffer full without newline */
        ssize_t n = read(ctx->sockfd, ctx->recv_buf + ctx->recv_len, (size_t)space);
        if (n <= 0) return -1;
        ctx->recv_len += (int)n;
    }
}

/* -------------------------------------------------------------------------
 * Handle incoming messages
 * ---------------------------------------------------------------------- */

/*
 * Parse and apply a mining.notify message.
 * Updates ctx->job under mutex and sets ctx->cancel if clean_jobs.
 */
static void handle_notify(stratum_ctx_t *ctx, const char *line)
{
    /* Find the params array: "params":[...] */
    const char *params = strstr(line, "\"params\":[");
    if (!params) { fprintf(stderr, "[stratum] notify: no params\n"); return; }
    params += strlen("\"params\":[");

    char job_id[STRATUM_JOB_ID_LEN] = {0};
    char version[16]  = {0};
    char prevhash[68] = {0};
    char merkle[68]   = {0};
    char reserved[68] = {0};
    char ntime[12]    = {0};
    char nbits[12]    = {0};

    /* params[0..6] are strings; params[7] is bool */
    json_array_get_str(params, 0, job_id,   sizeof(job_id));
    json_array_get_str(params, 1, version,  sizeof(version));
    json_array_get_str(params, 2, prevhash, sizeof(prevhash));
    json_array_get_str(params, 3, merkle,   sizeof(merkle));
    json_array_get_str(params, 4, reserved, sizeof(reserved));
    json_array_get_str(params, 5, ntime,    sizeof(ntime));
    json_array_get_str(params, 6, nbits,    sizeof(nbits));
    int clean = json_array_get_bool(params, 7);

    /* Build 108-byte binary header */
    uint8_t header[STRATUM_HEADER_LEN];
    int ok = 1;
    ok &= (hex_decode(version,  8,  header + 0)   == 0);
    ok &= (hex_decode(prevhash, 64, header + 4)   == 0);
    ok &= (hex_decode(merkle,   64, header + 36)  == 0);
    ok &= (hex_decode(reserved, 64, header + 68)  == 0);
    ok &= (hex_decode(ntime,    8,  header + 100) == 0);
    ok &= (hex_decode(nbits,    8,  header + 104) == 0);

    if (!ok) { fprintf(stderr, "[stratum] notify: hex decode error\n"); return; }

    fprintf(stderr, "[stratum] New job: %s  ntime=%s  clean=%d\n",
            job_id, ntime, clean);

    pthread_mutex_lock(&ctx->job_mutex);
    if (clean) ctx->cancel = 1;
    stratum_job_t *j = &ctx->job;
    snprintf(j->job_id, sizeof(j->job_id), "%s", job_id);
    memcpy(j->header, header, STRATUM_HEADER_LEN);
    snprintf(j->ntime, sizeof(j->ntime), "%s", ntime);
    j->clean = clean;
    j->valid = 1;
    pthread_mutex_unlock(&ctx->job_mutex);
}

/*
 * Parse and apply a mining.set_target message (informational only for now).
 */
static void handle_set_target(const char *line)
{
    const char *params = strstr(line, "\"params\":[\"");
    if (!params) return;
    char target[68] = {0};
    json_array_get_str(params + strlen("\"params\":["), 0, target, sizeof(target));
    fprintf(stderr, "[stratum] Target: %s\n", target);
}

/*
 * Parse a share result message (id >= 4).
 */
static void handle_share_result(const char *line, int id)
{
    if (strstr(line, "\"result\":true"))
        fprintf(stderr, "[stratum] Share #%d ACCEPTED\n", id);
    else
        fprintf(stderr, "[stratum] Share #%d REJECTED: %s\n", id, line);
}

/*
 * Dispatch one received line.
 * Returns 0 on success, -1 on disconnect/fatal error.
 */
static int dispatch_line(stratum_ctx_t *ctx, const char *line)
{
    if (!*line) return 0;  /* empty */

    /* Extract id field */
    int msg_id = -1;
    {
        const char *p = strstr(line, "\"id\":");
        if (p) sscanf(p + 5, "%d", &msg_id);
    }

    /* Check method */
    if (strstr(line, "\"method\":\"mining.notify\"")) {
        handle_notify(ctx, line);
        return 0;
    }
    if (strstr(line, "\"method\":\"mining.set_target\"")) {
        handle_set_target(line);
        return 0;
    }
    if (strstr(line, "\"method\":\"mining.set_extranonce\"")) {
        fprintf(stderr, "[stratum] set_extranonce received (ignored)\n");
        return 0;
    }

    /* Response to subscribe (id=1) */
    if (msg_id == 1) {
        /* Extract nonce1: result[1] in the params array */
        const char *result = strstr(line, "\"result\":");
        if (!result) { fprintf(stderr, "[stratum] subscribe: no result\n"); return 0; }
        /* nonce1 is nested: [[...], "nonce1_hex", nonce2_size] */
        /* Find nonce1 by looking for the second top-level string after result */
        const char *p = result + strlen("\"result\":");
        /* skip first array element (subscription array [[...,...],]) */
        /* result = [[subs_array], "nonce1", nonce2_size] — skip outer [ then subs [ */
        int depth = 0;
        while (*p) {
            if (*p == '[') depth++;
            else if (*p == ']') { depth--; if (depth == 1) { p++; break; } }
            p++;
        }
        /* now at nonce1 string */
        char nonce1_hex[32] = {0};
        /* skip comma and whitespace */
        while (*p == ',' || *p == ' ') p++;
        if (*p == '"') {
            p++;
            int i = 0;
            while (*p && *p != '"' && i < 31) nonce1_hex[i++] = *p++;
            nonce1_hex[i] = '\0';
        }
        int nlen = (int)strlen(nonce1_hex);
        ctx->nonce1_len = nlen / 2;
        hex_decode(nonce1_hex, nlen, ctx->nonce1);
        fprintf(stderr, "[stratum] Subscribed. nonce1=%s (%d bytes)\n",
                nonce1_hex, ctx->nonce1_len);
        return 0;
    }

    /* Response to authorize (id=2) */
    if (msg_id == 2) {
        if (strstr(line, "\"result\":true")) {
            fprintf(stderr, "[stratum] Authorized.\n");
            ctx->authorized = 1;
        } else {
            fprintf(stderr, "[stratum] Authorization FAILED: %s\n", line);
        }
        return 0;
    }

    /* Share results (id >= 4) */
    if (msg_id >= 4)
        handle_share_result(line, msg_id);

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void stratum_init(stratum_ctx_t *ctx, const char *host, const char *port,
                  const char *user, const char *pass)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->sockfd     = -1;
    ctx->submit_id  = 4;
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    strncpy(ctx->port, port, sizeof(ctx->port) - 1);
    strncpy(ctx->user, user, sizeof(ctx->user) - 1);
    strncpy(ctx->pass, pass, sizeof(ctx->pass) - 1);
    pthread_mutex_init(&ctx->job_mutex, NULL);
}

int stratum_connect(stratum_ctx_t *ctx)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(ctx->host, ctx->port, &hints, &res);
    if (err) {
        fprintf(stderr, "[stratum] getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "[stratum] connect failed: %s\n", strerror(errno));
        return -1;
    }
    ctx->sockfd    = fd;
    ctx->connected = 1;
    ctx->recv_len  = 0;

    fprintf(stderr, "[stratum] Connected to %s:%s\n", ctx->host, ctx->port);

    /* Subscribe */
    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"id\":1,\"method\":\"mining.subscribe\","
             "\"params\":[\"sa-tromp/1.0\",null,\"%s\",\"%s\"]}\n",
             ctx->host, ctx->port);
    if (sock_send(fd, msg) < 0) { close(fd); return -1; }

    /* Wait for subscribe response */
    char line[STRATUM_RECV_BUF];
    int got_subscribe = 0;
    while (!got_subscribe) {
        if (recv_line(ctx, line, sizeof(line)) < 0) return -1;
        int id = -1;
        const char *p = strstr(line, "\"id\":");
        if (p) sscanf(p + 5, "%d", &id);
        if (id == 1) {
            dispatch_line(ctx, line);
            got_subscribe = 1;
        } else {
            dispatch_line(ctx, line);  /* e.g. early notify */
        }
    }

    /* Authorize */
    snprintf(msg, sizeof(msg),
             "{\"id\":2,\"method\":\"mining.authorize\","
             "\"params\":[\"%s\",\"%s\"]}\n",
             ctx->user, ctx->pass);
    if (sock_send(fd, msg) < 0) return -1;

    /* extranonce subscribe */
    snprintf(msg, sizeof(msg),
             "{\"id\":3,\"method\":\"mining.extranonce.subscribe\","
             "\"params\":[]}\n");
    sock_send(fd, msg);  /* best-effort */

    /* Wait for authorize response */
    while (!ctx->authorized) {
        if (recv_line(ctx, line, sizeof(line)) < 0) return -1;
        dispatch_line(ctx, line);
    }

    return 0;
}

int stratum_recv_line(stratum_ctx_t *ctx)
{
    char line[STRATUM_RECV_BUF];
    int r = recv_line(ctx, line, sizeof(line));
    if (r < 0) return -1;
    return dispatch_line(ctx, line);
}

int stratum_get_job(stratum_ctx_t *ctx, stratum_job_t *dst)
{
    pthread_mutex_lock(&ctx->job_mutex);
    int valid = ctx->job.valid;
    if (valid) *dst = ctx->job;
    pthread_mutex_unlock(&ctx->job_mutex);
    return valid;
}

int stratum_submit(stratum_ctx_t *ctx, const char *job_id, const char *ntime,
                   uint32_t nonce2_val, const char *sol_hex)
{
    int id = ctx->submit_id++;

    /*
     * Build the full 32-byte nonce (64 hex chars):
     *   nonce1_hex (nonce1_len*2 chars) + nonce2_le_hex (8 chars) + zeros
     * nonce2 is stored LE in headernonce, submitted LE to pool.
     */
    char nonce_hex[65];
    memset(nonce_hex, '0', 64);
    nonce_hex[64] = '\0';

    /* Write nonce1 */
    for (int i = 0; i < ctx->nonce1_len; i++)
        snprintf(nonce_hex + i * 2, 3, "%02x", ctx->nonce1[i]);

    /* Write nonce2 (4 bytes, LE → hex) immediately after nonce1 */
    uint8_t n2[4];
    n2[0] = (nonce2_val >>  0) & 0xFF;
    n2[1] = (nonce2_val >>  8) & 0xFF;
    n2[2] = (nonce2_val >> 16) & 0xFF;
    n2[3] = (nonce2_val >> 24) & 0xFF;
    int off = ctx->nonce1_len * 2;
    snprintf(nonce_hex + off,     3, "%02x", n2[0]);
    snprintf(nonce_hex + off + 2, 3, "%02x", n2[1]);
    snprintf(nonce_hex + off + 4, 3, "%02x", n2[2]);
    snprintf(nonce_hex + off + 6, 3, "%02x", n2[3]);
    /* remaining chars stay as '0' */

    /* The nonce2 field submitted to pool: chars after nonce1 */
    const char *nonce2_submit = nonce_hex + ctx->nonce1_len * 2;

    char msg[2048];
    snprintf(msg, sizeof(msg),
             "{\"id\":%d,\"method\":\"mining.submit\",\"params\":"
             "[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
             id, ctx->user, job_id, ntime, nonce2_submit, sol_hex);

    fprintf(stderr, "[stratum] Submitting share #%d nonce2=%s\n", id, nonce2_submit);
    return sock_send(ctx->sockfd, msg);
}

void stratum_disconnect(stratum_ctx_t *ctx)
{
    if (ctx->sockfd >= 0) {
        close(ctx->sockfd);
        ctx->sockfd = -1;
    }
    ctx->connected  = 0;
    ctx->authorized = 0;
    pthread_mutex_destroy(&ctx->job_mutex);
}
