/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NVMe end-to-end data protection test
 */

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

static uint32_t swap32(uint32_t value)
{
	uint32_t result = 0;
	result |= (value & 0x000000FF) << 24;
	result |= (value & 0x0000FF00) << 8;
	result |= (value & 0x00FF0000) >> 8;
	result |= (value & 0xFF000000) >> 24;
	return result;
}


static uint16_t swap16(uint16_t value)
{
	uint16_t result = 0;

	result |= (value & 0x00FF) << 8;
	result |= (value & 0xFF00) >> 8;

	return result;
}

struct rte_mempool *request_mempool;

#define MAX_DEVS 64

#define DATA_PATTERN 0x5A

struct dev {
	struct spdk_nvme_ctrlr			*ctrlr;
	char 					name[100];
};

static struct dev devs[MAX_DEVS];
static int num_devs = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static int io_complete_flag = 0;

struct io_request {
	void *contig;
	void *metadata;
	bool use_extended_lba;
	uint64_t lba;
	uint32_t lba_count;
	uint16_t apptag_mask;
	uint16_t apptag;
};

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl))
		io_complete_flag = 2;
	else
		io_complete_flag = 1;
}

/*
 * No protection information with PRACT setting to 1,
 *  both extended LBA format and separate metadata can
 *  run the test case.
 */
static uint32_t dp_with_pract_test(struct spdk_nvme_ns *ns, struct io_request *req,
				   uint32_t *io_flags)
{
	uint32_t sector_size;

	req->lba_count = 8;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	/* No additional metadata buffer provided */
	req->contig = rte_zmalloc(NULL, sector_size * req->lba_count, 0x1000);
	if (!req->contig)
		return 0;

	switch (spdk_nvme_ns_get_pi_type(ns)) {
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE3:
		*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD | SPDK_NVME_IO_FLAGS_PRACT;
		break;
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE1:
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE2:
		*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_GUARD | SPDK_NVME_IO_FLAGS_PRCHK_REFTAG |
			    SPDK_NVME_IO_FLAGS_PRACT;
		break;
	default:
		*io_flags = 0;
		break;
	}
	req->lba = 0x100000;
	req->use_extended_lba = false;
	req->metadata = NULL;

	return req->lba_count;
}

/* Block Reference Tag checked for TYPE1 and TYPE2 with PRACT setting to 0 */
static uint32_t dp_without_pract_extended_lba_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 2;

	switch (spdk_nvme_ns_get_pi_type(ns)) {
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE3:
		return 0;
	default:
		break;
	}

	/* extended LBA only for the test case */
	if (!(spdk_nvme_ns_supports_extended_lba(ns)))
		return 0;

	sector_size = spdk_nvme_ns_get_sector_size(ns);;
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = rte_zmalloc(NULL, (sector_size + md_size) * req->lba_count, 0x1000);
	if (!req->contig)
		return 0;

	req->lba = 0x200000;
	req->use_extended_lba = true;
	req->metadata = NULL;
	pi = (struct spdk_nvme_protection_info *)(req->contig + sector_size + md_size - 8);
	/* big-endian for reference tag */
	pi->ref_tag = swap32((uint32_t)req->lba);

	pi = (struct spdk_nvme_protection_info *)(req->contig + (sector_size + md_size) * 2 - 8);
	/* is incremented for each subsequent logical block */
	pi->ref_tag = swap32((uint32_t)req->lba + 1);

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;

	return req->lba_count;
}

/* LBA + Metadata without data protection bits setting */
static uint32_t dp_without_flags_extended_lba_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	uint32_t md_size, sector_size;

	req->lba_count = 16;

	/* extended LBA only for the test case */
	if (!(spdk_nvme_ns_supports_extended_lba(ns)))
		return 0;

	sector_size = spdk_nvme_ns_get_sector_size(ns);;
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = rte_zmalloc(NULL, (sector_size + md_size) * req->lba_count, 0x1000);
	if (!req->contig)
		return 0;

	req->lba = 0x400000;
	req->use_extended_lba = true;
	req->metadata = NULL;
	*io_flags = 0;

	return req->lba_count;
}

/* Block Reference Tag checked for TYPE1 and TYPE2 with PRACT setting to 0 */
static uint32_t dp_without_pract_separate_meta_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 2;

	switch (spdk_nvme_ns_get_pi_type(ns)) {
	case SPDK_NVME_FMT_NVM_PROTECTION_TYPE3:
		return 0;
	default:
		break;
	}

	/* separate metadata payload for the test case */
	if (spdk_nvme_ns_supports_extended_lba(ns))
		return 0;

	sector_size = spdk_nvme_ns_get_sector_size(ns);;
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = rte_zmalloc(NULL, sector_size * req->lba_count, 0x1000);
	if (!req->contig)
		return 0;

	req->metadata = rte_zmalloc(NULL, md_size * req->lba_count, 0x1000);
	if (!req->metadata) {
		rte_free(req->contig);
		return 0;
	}

	req->lba = 0x400000;
	req->use_extended_lba = false;

	/* last 8 bytes if the metadata size bigger than 8 */
	pi = (struct spdk_nvme_protection_info *)(req->metadata + md_size - 8);
	/* big-endian for reference tag */
	pi->ref_tag = swap32((uint32_t)req->lba);

	pi = (struct spdk_nvme_protection_info *)(req->metadata + md_size * 2 - 8);
	/* is incremented for each subsequent logical block */
	pi->ref_tag = swap32((uint32_t)req->lba + 1);

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_REFTAG;

	return req->lba_count;
}

/* Application Tag checked with PRACT setting to 0 */
static uint32_t dp_without_pract_separate_meta_apptag_test(struct spdk_nvme_ns *ns,
		struct io_request *req,
		uint32_t *io_flags)
{
	struct spdk_nvme_protection_info *pi;
	uint32_t md_size, sector_size;

	req->lba_count = 1;

	/* separate metadata payload for the test case */
	if (spdk_nvme_ns_supports_extended_lba(ns))
		return 0;

	sector_size = spdk_nvme_ns_get_sector_size(ns);;
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = rte_zmalloc(NULL, sector_size * req->lba_count, 0x1000);
	if (!req->contig)
		return 0;

	req->metadata = rte_zmalloc(NULL, md_size * req->lba_count, 0x1000);
	if (!req->metadata) {
		rte_free(req->contig);
		return 0;
	}

	req->lba = 0x500000;
	req->use_extended_lba = false;
	req->apptag_mask = 0xFFFF;
	req->apptag = req->lba_count;

	/* last 8 bytes if the metadata size bigger than 8 */
	pi = (struct spdk_nvme_protection_info *)(req->metadata + md_size - 8);
	pi->app_tag = swap16(req->lba_count);

	*io_flags = SPDK_NVME_IO_FLAGS_PRCHK_APPTAG;

	return req->lba_count;
}

/*
 * LBA + Metadata without data protection bits setting,
 *  separate metadata payload for the test case.
 */
static uint32_t dp_without_flags_separate_meta_test(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *io_flags)
{
	uint32_t md_size, sector_size;

	req->lba_count = 16;

	/* separate metadata payload for the test case */
	if (spdk_nvme_ns_supports_extended_lba(ns))
		return 0;

	sector_size = spdk_nvme_ns_get_sector_size(ns);;
	md_size = spdk_nvme_ns_get_md_size(ns);
	req->contig = rte_zmalloc(NULL, sector_size * req->lba_count, 0x1000);
	if (!req->contig)
		return 0;

	req->metadata = rte_zmalloc(NULL, md_size * req->lba_count, 0x1000);
	if (!req->metadata) {
		rte_free(req->contig);
		return 0;
	}

	req->lba = 0x600000;
	req->use_extended_lba = false;
	*io_flags = 0;

	return req->lba_count;
}

typedef uint32_t (*nvme_build_io_req_fn_t)(struct spdk_nvme_ns *ns, struct io_request *req,
		uint32_t *lba_count);

static void
free_req(struct io_request *req)
{
	if (req == NULL) {
		return;
	}

	if (req->contig)
		rte_free(req->contig);

	if (req->metadata)
		rte_free(req->metadata);

	rte_free(req);
}

static void
ns_data_buffer_reset(struct spdk_nvme_ns *ns, struct io_request *req, uint8_t data_pattern)
{
	uint32_t md_size, sector_size;
	uint32_t i, offset = 0;
	uint8_t *buf;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);

	for (i = 0; i < req->lba_count; i++) {
		if (req->use_extended_lba)
			offset = (sector_size + md_size) * i;
		else
			offset = sector_size * i;

		buf = (uint8_t *)req->contig + offset;
		memset(buf, data_pattern, sector_size);
	}
}

static int
ns_data_buffer_compare(struct spdk_nvme_ns *ns, struct io_request *req, uint8_t data_pattern)
{
	uint32_t md_size, sector_size;
	uint32_t i, j, offset = 0;
	uint8_t *buf;

	sector_size = spdk_nvme_ns_get_sector_size(ns);
	md_size = spdk_nvme_ns_get_md_size(ns);

	for (i = 0; i < req->lba_count; i++) {
		if (req->use_extended_lba)
			offset = (sector_size + md_size) * i;
		else
			offset = sector_size * i;

		buf = (uint8_t *)req->contig + offset;
		for (j = 0; j < sector_size; j++) {
			if (buf[j] != data_pattern) {
				return -1;
			}
		}
	}

	return 0;
}

static int
write_read_e2e_dp_tests(struct dev *dev, nvme_build_io_req_fn_t build_io_fn, const char *test_name)
{
	int rc = 0;
	uint32_t lba_count;
	uint32_t io_flags = 0;

	struct io_request *req;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	const struct spdk_nvme_ns_data *nsdata;

	ns = spdk_nvme_ctrlr_get_ns(dev->ctrlr, 1);
	if (!ns) {
		fprintf(stderr, "Null namespace\n");
		return 0;
	}

	if (!(spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED))
		return 0;

	nsdata = spdk_nvme_ns_get_data(ns);
	if (!nsdata || !spdk_nvme_ns_get_sector_size(ns)) {
		fprintf(stderr, "Empty nsdata or wrong sector size\n");
		return 0;
	}

	req = rte_zmalloc(NULL, sizeof(*req), 0);
	if (!req) {
		fprintf(stderr, "Allocate request failed\n");
		return 0;
	}

	/* IO parameters setting */
	lba_count = build_io_fn(ns, req, &io_flags);

	if (!lba_count) {
		fprintf(stderr, "%s: %s bypass the test case\n", dev->name, test_name);
		free_req(req);
		return 0;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->ctrlr, 0);
	if (!qpair) {
		free_req(req);
		return -1;
	}

	ns_data_buffer_reset(ns, req, DATA_PATTERN);
	if (req->use_extended_lba)
		rc = spdk_nvme_ns_cmd_write(ns, qpair, req->contig, req->lba, lba_count,
					    io_complete, req, io_flags);
	else
		rc = spdk_nvme_ns_cmd_write_with_md(ns, qpair, req->contig, req->metadata, req->lba, lba_count,
						    io_complete, req, io_flags, req->apptag_mask, req->apptag);

	if (rc != 0) {
		fprintf(stderr, "%s: %s write submit failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	io_complete_flag = 0;

	while (!io_complete_flag)
		spdk_nvme_qpair_process_completions(qpair, 1);

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s write exec failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	/* reset completion flag */
	io_complete_flag = 0;

	ns_data_buffer_reset(ns, req, 0);
	if (req->use_extended_lba)
		rc = spdk_nvme_ns_cmd_read(ns, qpair, req->contig, req->lba, lba_count,
					   io_complete, req, io_flags);
	else
		rc = spdk_nvme_ns_cmd_read_with_md(ns, qpair, req->contig, req->metadata, req->lba, lba_count,
						   io_complete, req, io_flags, req->apptag_mask, req->apptag);

	if (rc != 0) {
		fprintf(stderr, "%s: %s read failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	while (!io_complete_flag)
		spdk_nvme_qpair_process_completions(qpair, 1);

	if (io_complete_flag != 1) {
		fprintf(stderr, "%s: %s read failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	rc = ns_data_buffer_compare(ns, req, DATA_PATTERN);
	if (rc < 0) {
		fprintf(stderr, "%s: %s write/read success, but memcmp Failed\n", dev->name, test_name);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		free_req(req);
		return -1;
	}

	fprintf(stdout, "%s: %s test passed\n", dev->name, test_name);
	spdk_nvme_ctrlr_free_io_qpair(qpair);
	free_req(req);
	return rc;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr_opts *opts)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to NVMe\n");
		fprintf(stderr, " controller at PCI address %04x:%02x:%02x.%02x\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		fprintf(stderr, " skipping...\n");
		return false;
	}

	printf("Attaching to %04x:%02x:%02x.%02x\n",
	       spdk_pci_device_get_domain(dev),
	       spdk_pci_device_get_bus(dev),
	       spdk_pci_device_get_dev(dev),
	       spdk_pci_device_get_func(dev));

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;

	/* add to dev list */
	dev = &devs[num_devs++];

	dev->ctrlr = ctrlr;

	snprintf(dev->name, sizeof(dev->name), "%04X:%02X:%02X.%02X",
		 spdk_pci_device_get_domain(pci_dev),
		 spdk_pci_device_get_bus(pci_dev),
		 spdk_pci_device_get_dev(pci_dev),
		 spdk_pci_device_get_func(pci_dev));

	printf("Attached to %s\n", dev->name);
}


static const char *ealargs[] = {
	"nvme_dp",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	struct dev			*iter;
	int				rc, i;

	printf("NVMe Write/Read with End-to-End data protection test\n");

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		exit(1);
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     spdk_nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		exit(1);
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "nvme_probe() failed\n");
		exit(1);
	}

	rc = 0;
	foreach_dev(iter) {
#define TEST(x) write_read_e2e_dp_tests(iter, x, #x)
		if (TEST(dp_with_pract_test)
		    || TEST(dp_without_pract_extended_lba_test)
		    || TEST(dp_without_flags_extended_lba_test)
		    || TEST(dp_without_pract_separate_meta_test)
		    || TEST(dp_without_pract_separate_meta_apptag_test)
		    || TEST(dp_without_flags_separate_meta_test)) {
#undef TEST
			rc = 1;
			printf("%s: failed End-to-End data protection tests\n", iter->name);
		}
	}

	printf("Cleaning up...\n");

	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];

		spdk_nvme_detach(dev->ctrlr);
	}

	return rc;
}
