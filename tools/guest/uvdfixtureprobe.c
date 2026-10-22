/* Run only after the kernel's UVD ring diagnostic is ready.
 * gcc -Ihw -Itests tools/guest/uvdfixtureprobe.c tests/uvd_decode_fixture.c \
 *     -framework IOKit -framework CoreFoundation -o uvdfixtureprobe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#include "rdn_user.h"
#include "uvd_decode_fixture.h"

static void sha256(const void *bytes, size_t size, char output[65])
{
	unsigned char digest[CC_SHA256_DIGEST_LENGTH];
	unsigned i;
	CC_SHA256(bytes, (CC_LONG)size, digest);
	for (i = 0; i < sizeof(digest); ++i)
		sprintf(output + 2 * i, "%02x", digest[i]);
	output[64] = 0;
}

static int save(const char *prefix, const char *suffix, const void *bytes, size_t size)
{
	char path[PATH_MAX];
	FILE *stream;
	int fd, n = snprintf(path, sizeof(path), "%s%s", prefix, suffix);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		perror(path);
		return -1;
	}
	stream = fdopen(fd, "wb");
	if (!stream) {
		close(fd);
		return -1;
	}
	if (fwrite(bytes, 1, size, stream) != size) {
		fclose(stream);
		return -1;
	}
	return fclose(stream);
}

int main(int argc, char **argv)
{
	io_service_t service;
	io_connect_t connection = IO_OBJECT_NULL;
	CFTypeRef property;
	struct rdn_user_uvd_fixture result;
	struct rdn_user_uvd_diagnostic diagnostic;
	uint32_t version = 0, chunk;
	IOByteCount size;
	kern_return_t kr;
	uint8_t block[RDN_UVD_FIXTURE_CHUNK_BYTES], *snapshot = 0, *nv12 = 0;
	char rawHash[65], imageHash[65], feedbackHash[65];
	int matched = 0, code = 1;
	if (argc != 3 || strcmp(argv[1], "run") || geteuid() != 0) {
		fprintf(stderr, "usage (root): %s run NEW_OUTPUT_PREFIX\n", argv[0]);
		return 2;
	}
	service = IOServiceGetMatchingService(kIOMasterPortDefault,
		IOServiceMatching(RDN_UC_SERVICE_CLASS));
	if (!service)
		return 3;
	property = IORegistryEntryCreateCFProperty(service,
		CFSTR("RadeonNIUVDFixtureVersion"), kCFAllocatorDefault, 0);
	if (property && CFGetTypeID(property) == CFNumberGetTypeID())
		CFNumberGetValue((CFNumberRef)property, kCFNumberSInt32Type, &version);
	if (property)
		CFRelease(property);
	if (version != RDN_UVD_FIXTURE_VERSION) {
		fprintf(stderr, "fixed fixture ABI unavailable; nothing submitted\n");
		IOObjectRelease(service);
		return 3;
	}
	kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &connection);
	IOObjectRelease(service);
	if (kr)
		return 3;
	memset(&diagnostic, 0, sizeof(diagnostic));
	size = sizeof(diagnostic);
	kr = IOConnectMethodScalarIStructureO(connection, RDN_UC_UVD_DIAGNOSTIC_INFO,
		0, &size, &diagnostic);
	if (kr || size != sizeof(diagnostic) || diagnostic.version != RDN_UVD_DIAGNOSTIC_VERSION ||
	    !(diagnostic.flags & RDN_UVD_DIAGNOSTIC_ALLOWED)) {
		fprintf(stderr, "UVD diagnostic query/opt-in failed; nothing submitted\n");
		goto out;
	}
	/* RUN is idempotent after its first attempt, including a failed attempt. */
	memset(&result, 0, sizeof(result));
	size = sizeof(result);
	kr = IOConnectMethodScalarIStructureO(connection, RDN_UC_UVD_FIXTURE_RUN,
		1, &size, 0, &result);
	if (kr || size != sizeof(result) || result.version != version) {
		fprintf(stderr, "fixture RUN: 0x%x size%lu (ring state%u)\n",
			kr, (unsigned long)size, diagnostic.state);
		goto out;
	}
	if (result.fixture_id != RDN_UVD_FIXTURE_ID || result.width != RDN_UVD_FIXTURE_WIDTH ||
	    result.height != RDN_UVD_FIXTURE_HEIGHT || result.target_pitch != RDN_UVD_FIXTURE_PITCH ||
	    result.target_bytes != RDN_UVD_FIXTURE_TARGET_BYTES || result.feedback_bytes != 2048 ||
	    result.feedback_offset != RDN_UVD_FIXTURE_TARGET_BYTES ||
	    result.snapshot_bytes != RDN_UVD_FIXTURE_TARGET_BYTES + 2048 || !result.generation)
		goto out;
	if (!(result.flags & RDN_UVD_FIXTURE_OUTPUT_READY)) {
		printf("{\"state\":%u,\"stage\":%u,\"error\":%d,\"create_fence\":%u,"
			"\"decode_fence\":%u,\"destroy_fence\":%u,\"snapshot_available\":false}\n",
			result.state, result.stage, result.error, result.create_fence,
			result.decode_fence, result.destroy_fence);
		goto out;
	}
	snapshot = (uint8_t *)malloc(result.snapshot_bytes);
	nv12 = (uint8_t *)malloc(RDN_UVD_FIXTURE_NV12_BYTES);
	if (!snapshot || !nv12)
		goto out;
	for (chunk = 0; chunk < (result.snapshot_bytes + sizeof(block) - 1) / sizeof(block); ++chunk) {
		size_t offset = chunk * sizeof(block), count = result.snapshot_bytes - offset, i;
		if (count > sizeof(block)) count = sizeof(block);
		size = sizeof(block);
		kr = IOConnectMethodScalarIStructureO(connection, RDN_UC_UVD_FIXTURE_READ,
			2, &size, result.generation, chunk, block);
		if (kr || size != sizeof(block))
			goto out;
		for (i = count; i < sizeof(block); ++i)
			if (block[i]) goto out;
		memcpy(snapshot + offset, block, count);
	}
	if (rdn_uvd_fixture_unpack_nv12(snapshot, result.target_bytes,
	    nv12, RDN_UVD_FIXTURE_NV12_BYTES))
		goto out;
	sha256(snapshot, result.target_bytes, rawHash);
	sha256(nv12, RDN_UVD_FIXTURE_NV12_BYTES, imageHash);
	sha256(snapshot + result.feedback_offset, result.feedback_bytes, feedbackHash);
	matched = !strcmp(imageHash, rdn_uvd_fixture_reference_sha256);
	if (save(argv[2], ".target", snapshot, result.target_bytes) ||
	    save(argv[2], ".nv12", nv12, RDN_UVD_FIXTURE_NV12_BYTES) ||
	    save(argv[2], ".feedback", snapshot + result.feedback_offset, result.feedback_bytes))
		goto out;
	printf("{\"version\":%u,\"state\":%u,\"stage\":%u,\"error\":%d,\"flags\":%u,\"generation\":%u,"
		"\"width\":%u,\"height\":%u,\"pitch\":%u,\"target_bytes\":%u,\"nv12_bytes\":%u,"
		"\"message_variant\":%u,\"db_pitch\":864,\"create_fence\":%u,\"decode_fence\":%u,\"destroy_fence\":%u,"
		"\"idle_error\":%d,\"target_sha256\":\"%s\",\"feedback_sha256\":\"%s\","
		"\"nv12_sha256\":\"%s\",\"expected_nv12_sha256\":\"%s\",\"pixel_match\":%s}\n",
		result.version, result.state, result.stage, result.error, result.flags,
		result.generation, result.width, result.height,
		result.target_pitch, result.target_bytes, RDN_UVD_FIXTURE_NV12_BYTES,
		result.message_variant, result.create_fence, result.decode_fence, result.destroy_fence,
		result.idle_error, rawHash, feedbackHash, imageHash,
		rdn_uvd_fixture_reference_sha256, matched ? "true" : "false");
	code = matched && result.state == RDN_UVD_FIXTURE_COMPLETE && !result.error &&
		(result.flags & RDN_UVD_FIXTURE_GUARDS_OK) && !result.idle_error ? 0 : 1;
out:
	free(nv12);
	free(snapshot);
	IOServiceClose(connection);
	return code;
}
