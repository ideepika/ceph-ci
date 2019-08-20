#ifndef _SRC_MOUNT_MOUNT_CEPH_H
#define _SRC_MOUNT_MOUNT_CEPH_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * See class CryptoKey
 *
 * 2 (for the type of secret) +
 * 8 (for the timestamp) +
 * 2 (for the length of secret) +
 * 16 (for an AES-128 key)
 */
#define MAX_RAW_SECRET_LEN (2 + 8 + 2 + 16)

/* Max length of base64 encoded secret. 4/3 original size (rounded up) */
#define MAX_SECRET_LEN ((MAX_RAW_SECRET_LEN + (3 - 1)) * 4 / 3)

/* Max Including null terminator */
#define SECRET_BUFSIZE (MAX_SECRET_LEN + 1)

/* Buffer size for secret= option */
#define SECRET_OPTION_BUFSIZE (sizeof("secret=") + MAX_SECRET_LEN + 1)

void mount_ceph_debug(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SRC_MOUNT_MOUNT_CEPH_H */