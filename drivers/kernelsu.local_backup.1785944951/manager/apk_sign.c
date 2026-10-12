#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/version.h>
#ifdef CONFIG_KSU_DEBUG
#include <linux/moduleparam.h>
#endif
#include <crypto/hash.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#include <crypto/sha2.h>
#else
#include <crypto/sha.h>
#endif

#include "apk_sign.h"
#include "policy/app_profile.h"
#include "klog.h" // IWYU pragma: keep
#include "compat/kernel_compat.h"

#define KSHA256_BLOCK_SIZE 64
#define KSHA256_DIGEST_SIZE 32

struct ksha256_ctx {
	u32 state[8];
	u64 bitlen;
	u8 data[KSHA256_BLOCK_SIZE];
	u32 datalen;
};

static const u32 ksha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
	0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline u32 ksha_rotr(u32 x, u32 n)
{
	return (x >> n) | (x << (32 - n));
}

static inline u32 ksha_ep0(u32 x)
{
	return ksha_rotr(x, 2) ^ ksha_rotr(x, 13) ^ ksha_rotr(x, 22);
}

static inline u32 ksha_ep1(u32 x)
{
	return ksha_rotr(x, 6) ^ ksha_rotr(x, 11) ^ ksha_rotr(x, 25);
}

static inline u32 ksha_sig0(u32 x)
{
	return ksha_rotr(x, 7) ^ ksha_rotr(x, 18) ^ (x >> 3);
}

static inline u32 ksha_sig1(u32 x)
{
	return ksha_rotr(x, 17) ^ ksha_rotr(x, 19) ^ (x >> 10);
}

static inline u32 ksha_ch(u32 x, u32 y, u32 z)
{
	return (x & y) ^ (~x & z);
}

static inline u32 ksha_maj(u32 x, u32 y, u32 z)
{
	return (x & y) ^ (x & z) ^ (y & z);
}

static void ksha256_transform(struct ksha256_ctx *ctx, const u8 *data)
{
	u32 m[64];
	u32 a, b, c, d, e, f, g, h;
	u32 t1, t2;
	int i, j;

	for (i = 0, j = 0; i < 16; ++i, j += 4)
		m[i] = ((u32)data[j] << 24) | ((u32)data[j + 1] << 16) |
		       ((u32)data[j + 2] << 8) | ((u32)data[j + 3]);

	for (; i < 64; ++i)
		m[i] = ksha_sig1(m[i - 2]) + m[i - 7] + ksha_sig0(m[i - 15]) +
		       m[i - 16];

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; ++i) {
		t1 = h + ksha_ep1(e) + ksha_ch(e, f, g) + ksha256_k[i] + m[i];
		t2 = ksha_ep0(a) + ksha_maj(a, b, c);
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

static void ksha256_init(struct ksha256_ctx *ctx)
{
	ctx->datalen = 0;
	ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

static void ksha256_update(struct ksha256_ctx *ctx, const u8 *data,
			  u32 len)
{
	u32 i;

	for (i = 0; i < len; ++i) {
		ctx->data[ctx->datalen] = data[i];
		ctx->datalen++;
		if (ctx->datalen == KSHA256_BLOCK_SIZE) {
			ksha256_transform(ctx, ctx->data);
			ctx->bitlen += 512;
			ctx->datalen = 0;
		}
	}
}

static void ksha256_final(struct ksha256_ctx *ctx, u8 *hash)
{
	u32 i = ctx->datalen;

	ctx->data[i++] = 0x80;
	if (i > 56) {
		while (i < KSHA256_BLOCK_SIZE)
			ctx->data[i++] = 0x00;
		ksha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56)
		ctx->data[i++] = 0x00;

	ctx->bitlen += ctx->datalen * 8;
	ctx->data[63] = (u8)ctx->bitlen;
	ctx->data[62] = (u8)(ctx->bitlen >> 8);
	ctx->data[61] = (u8)(ctx->bitlen >> 16);
	ctx->data[60] = (u8)(ctx->bitlen >> 24);
	ctx->data[59] = (u8)(ctx->bitlen >> 32);
	ctx->data[58] = (u8)(ctx->bitlen >> 40);
	ctx->data[57] = (u8)(ctx->bitlen >> 48);
	ctx->data[56] = (u8)(ctx->bitlen >> 56);

	ksha256_transform(ctx, ctx->data);

	for (i = 0; i < 4; ++i) {
		hash[i] = (u8)(ctx->state[0] >> (24 - i * 8));
		hash[i + 4] = (u8)(ctx->state[1] >> (24 - i * 8));
		hash[i + 8] = (u8)(ctx->state[2] >> (24 - i * 8));
		hash[i + 12] = (u8)(ctx->state[3] >> (24 - i * 8));
		hash[i + 16] = (u8)(ctx->state[4] >> (24 - i * 8));
		hash[i + 20] = (u8)(ctx->state[5] >> (24 - i * 8));
		hash[i + 24] = (u8)(ctx->state[6] >> (24 - i * 8));
		hash[i + 28] = (u8)(ctx->state[7] >> (24 - i * 8));
	}
}

static int ksu_sha256(const unsigned char *data, unsigned int datalen,
			unsigned char *digest)
{
	struct ksha256_ctx ctx;

	ksha256_init(&ctx);
	ksha256_update(&ctx, data, datalen);
	ksha256_final(&ctx, digest);
	return 0;
}

static bool check_block(struct file *fp, u32 *size4, loff_t *pos, u32 *offset,
			unsigned expected_size, const char *expected_sha256)
{
	ksu_kernel_read_compat(fp, size4, 0x4, pos); // signer-sequence length
	ksu_kernel_read_compat(fp, size4, 0x4, pos); // signer length
	ksu_kernel_read_compat(fp, size4, 0x4, pos); // signed data length

	*offset += 0x4 * 3;

	ksu_kernel_read_compat(fp, size4, 0x4, pos); // digests-sequence length

	*pos += *size4;
	*offset += 0x4 + *size4;

	ksu_kernel_read_compat(fp, size4, 0x4, pos); // certificates length
	ksu_kernel_read_compat(fp, size4, 0x4, pos); // certificate length
	*offset += 0x4 * 2;

	if (*size4 == expected_size) {
		*offset += *size4;

#define CERT_MAX_LENGTH 1024
		char cert[CERT_MAX_LENGTH];
		if (*size4 > CERT_MAX_LENGTH) {
			pr_info("cert length overlimit\n");
			return false;
		}
		ksu_kernel_read_compat(fp, cert, *size4, pos);
		unsigned char digest[SHA256_DIGEST_SIZE];
		if (ksu_sha256(cert, *size4, digest) < 0 ) {
			pr_info("sha256 error\n");
			return false;
		}

		char hash_str[SHA256_DIGEST_SIZE * 2 + 1];
		hash_str[SHA256_DIGEST_SIZE * 2] = '\0';

		bin2hex(hash_str, digest, SHA256_DIGEST_SIZE);
		pr_info("sha256: %s, expected: %s\n", hash_str,
			expected_sha256);
		if (strcmp(expected_sha256, hash_str) == 0) {
			return true;
		}
	}
	return false;
}

struct zip_entry_header {
	uint32_t signature;
	uint16_t version;
	uint16_t flags;
	uint16_t compression;
	uint16_t mod_time;
	uint16_t mod_date;
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint16_t file_name_length;
	uint16_t extra_field_length;
} __attribute__((packed));

struct ksu_buf_reader {
	struct file *fp;
	loff_t file_pos;
	char buf[4096];
	size_t buf_len;
};

static inline ssize_t ksu_bread(struct ksu_buf_reader *br, void *dst,
				size_t count, loff_t *pos)
{
	if (*pos >= br->file_pos &&
	    *pos + count <= br->file_pos + br->buf_len) {
		memcpy(dst, br->buf + (*pos - br->file_pos), count);
		*pos += count;
		return count;
	}

	br->file_pos = *pos;
	loff_t read_pos = br->file_pos;
	ssize_t res = ksu_kernel_read_compat(br->fp, br->buf, sizeof(br->buf),
					     &read_pos);
	if (res <= 0) {
		br->buf_len = 0;
		return res;
	}
	br->buf_len = res;

	if (count <= br->buf_len) {
		memcpy(dst, br->buf, count);
		*pos += count;
		return count;
	}

	return 0;
}

// This is a necessary but not sufficient condition, but it is enough for us
static bool has_v1_signature_file(struct file *fp)
{
	struct zip_entry_header header;
	const char MANIFEST[] = "META-INF/MANIFEST.MF";
	bool found = false;
	loff_t pos = 0;

	struct ksu_buf_reader *br =
		kzalloc(sizeof(struct ksu_buf_reader), GFP_KERNEL);
	if (!br) {
		pr_err("ksu_buf_reader alloc failed\n");
		return false;
	}

	br->fp = fp;
	br->file_pos = 0;
	br->buf_len = 0;
	while (ksu_bread(br, &header,
					sizeof(struct zip_entry_header), &pos) ==
		sizeof(struct zip_entry_header)) {
		if (header.signature != 0x04034b50) {
			// ZIP magic: 'PK'
			break;
		}
		// Read the entry file name
		if (header.file_name_length == sizeof(MANIFEST) - 1) {
			char fileName[sizeof(MANIFEST)];
			if (ksu_bread(br, fileName, header.file_name_length,
				      &pos) == header.file_name_length) {
				fileName[header.file_name_length] = '\0';

				// Check if the entry matches META-INF/MANIFEST.MF
				if (strncmp(MANIFEST, fileName, sizeof(MANIFEST) - 1) == 0) {
					found = true;
					break;
				}
			} else {
				break;
			}
		} else {
			// Skip the entry file name
			pos += header.file_name_length;
		}

		// Skip to the next entry
		pos += header.extra_field_length + header.compressed_size;
	}

	kfree(br);
	return found;
}

static __always_inline bool check_v2_signature(char *path,
						unsigned expected_size,
						const char *expected_sha256)
{
	unsigned char buffer[0x11] = { 0 };
	u32 size4;
	u64 size8, size_of_block;

	loff_t pos;

	bool v2_signing_valid = false;
	int v2_signing_blocks = 0;
	bool v3_signing_exist = false;
	bool v3_1_signing_exist = false;

	int i;
	struct file *fp = ksu_filp_open_compat(path, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		pr_err("open %s error.\n", path);
		return false;
	}

	// disable inotify for this file
	fp->f_mode |= FMODE_NONOTIFY;

	// https://en.wikipedia.org/wiki/Zip_(file_format)#End_of_central_directory_record_(EOCD)
	{
		unsigned char *eocd_buffer;
		loff_t file_size;
		long search_size;
		long max_comment_size = 0xffff;
		long eocd_min_size = 22;
		long eocd_found = 0;

		file_size = generic_file_llseek(fp, 0, SEEK_END);
		search_size = max_comment_size + eocd_min_size;
		if (search_size > file_size) {
			search_size = file_size;
		}

		eocd_buffer = kvmalloc(search_size, GFP_KERNEL);
		if (!eocd_buffer) {
			pr_err("error: cannot allocate memory for eocd\n");
			goto clean;
		}

		pos = file_size - search_size;
		ksu_kernel_read_compat(fp, eocd_buffer, search_size, &pos);

		if (search_size >= eocd_min_size) {
			long j;
			for (j = search_size - eocd_min_size; j >= 0; j--) {
				if (eocd_buffer[j] == 0x50 &&
				    eocd_buffer[j + 1] == 0x4b &&
				    eocd_buffer[j + 2] == 0x05 &&
				    eocd_buffer[j + 3] == 0x06) {
					unsigned short comment_len = 
						eocd_buffer[j + 20] | (eocd_buffer[j + 21] << 8);
					if (comment_len == search_size - j - eocd_min_size) {
						pos = file_size - search_size + j;
						eocd_found = 1;
						break;
					}
				}
			}
		}

		kvfree(eocd_buffer);

		if (!eocd_found) {
			pr_info("error: cannot find eocd\n");
			goto clean;
		}
	}

	pos += 16; // skip 4 bytes signature + 12 bytes
	// offset
	ksu_kernel_read_compat(fp, &size4, 0x4, &pos);
	pos = size4 - 0x18;

	ksu_kernel_read_compat(fp, &size8, 0x8, &pos);
	ksu_kernel_read_compat(fp, buffer, 0x10, &pos);
	if (strcmp((char *)buffer, "APK Sig Block 42")) {
		goto clean;
	}

	pos = size4 - (size8 + 0x8);
	ksu_kernel_read_compat(fp, &size_of_block, 0x8, &pos);
	if (size_of_block != size8) {
		goto clean;
	}

	int loop_count = 0;
	while (loop_count++ < 10) {
		uint32_t id;
		uint32_t offset;
		ksu_kernel_read_compat(fp, &size8, 0x8,
					&pos); // sequence length
		if (size8 == size_of_block) {
			break;
		}
		ksu_kernel_read_compat(fp, &id, 0x4, &pos); // id
		offset = 4;
		if (id == 0x7109871au) {
			v2_signing_blocks++;
			v2_signing_valid =
				check_block(fp, &size4, &pos, &offset,
						expected_size, expected_sha256);
		} else if (id == 0xf05368c0u) {
			// http://aospxref.com/android-14.0.0_r2/xref/frameworks/base/core/java/android/util/apk/ApkSignatureSchemeV3Verifier.java#73
			v3_signing_exist = true;
		} else if (id == 0x1b93ad61u) {
			// http://aospxref.com/android-14.0.0_r2/xref/frameworks/base/core/java/android/util/apk/ApkSignatureSchemeV3Verifier.java#74
			v3_1_signing_exist = true;
		} else {
#ifdef CONFIG_KSU_DEBUG
			pr_info("Unknown id: 0x%08x\n", id);
#endif
		}
		pos += (size8 - offset);
	}

	if (v2_signing_blocks != 1) {
#ifdef CONFIG_KSU_DEBUG
		pr_err("Unexpected v2 signature count: %d\n",
			v2_signing_blocks);
#endif
		v2_signing_valid = false;
	}

	if (v2_signing_valid) {
		int has_v1_signing = has_v1_signature_file(fp);
		if (has_v1_signing) {
			pr_err("Unexpected v1 signature scheme found!\n");
			filp_close(fp, 0);
			return false;
		}
	}
clean:
	filp_close(fp, 0);

	if (v3_signing_exist || v3_1_signing_exist) {
#ifdef CONFIG_KSU_DEBUG
		pr_err("Unexpected v3 signature scheme found!\n");
#endif
		return false;
	}

	return v2_signing_valid;
}

#ifdef CONFIG_KSU_DEBUG

int ksu_debug_manager_appid = -1;

#include "manager/manager_identity.h"

static int set_expected_size(const char *val, const struct kernel_param *kp)
{
	int rv = param_set_uint(val, kp);
	ksu_set_manager_appid(ksu_debug_manager_appid);
	pr_info("ksu_manager_appid set to %d\n", ksu_debug_manager_appid);
	return rv;
}

static struct kernel_param_ops expected_size_ops = {
	.set = set_expected_size,
	.get = param_get_uint,
};

module_param_cb(ksu_debug_manager_appid, &expected_size_ops,
		&ksu_debug_manager_appid, S_IRUSR | S_IWUSR);

#endif

int get_pkg_from_apk_path(char *pkg, const char *path)
{
	int len = strlen(path);
	if (len >= KSU_MAX_PACKAGE_NAME || len < 1)
		return -1;

	const char *last_slash = NULL;
	const char *second_last_slash = NULL;

	int i;
	for (i = len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			if (!last_slash) {
				last_slash = &path[i];
			} else {
				second_last_slash = &path[i];
				break;
			}
		}
	}

	if (!last_slash || !second_last_slash)
		return -1;

	const char *last_hyphen = strchr(second_last_slash, '-');
	if (!last_hyphen || last_hyphen > last_slash)
		return -1;

	int pkg_len = last_hyphen - second_last_slash - 1;
	if (pkg_len >= KSU_MAX_PACKAGE_NAME || pkg_len <= 0)
		return -1;

	// Copying the package name
	memcpy(pkg, second_last_slash + 1, pkg_len);
	pkg[pkg_len] = '\0';

	return 0;
}

bool is_manager_apk(char *path)
{
#ifdef KSU_MANAGER_PACKAGE
	char pkg[KSU_MAX_PACKAGE_NAME];
	if (get_pkg_from_apk_path(pkg, path) < 0) {
		pr_err("Failed to get package name from apk path: %s\n", path);
		return false;
	}

	// pkg is `<real package>`
	if (strncmp(pkg, KSU_MANAGER_PACKAGE, sizeof(KSU_MANAGER_PACKAGE))) {
		return false;
	}
#endif
	return check_v2_signature(path, EXPECTED_MANAGER_SIZE, EXPECTED_MANAGER_HASH);
}
