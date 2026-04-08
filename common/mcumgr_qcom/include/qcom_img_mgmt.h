/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QCOM_IMG_MGMT_H_
#define QCOM_IMG_MGMT_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Image slot definitions */
#define QCOM_IMG_SLOT_ACTIVE    0  /* Currently running image */
#define QCOM_IMG_SLOT_UPDATE    1  /* Update/staging slot */

/* Image states */
#define QCOM_IMG_STATE_NONE     0  /* No image */
#define QCOM_IMG_STATE_PENDING  1  /* Image uploaded, pending activation */
#define QCOM_IMG_STATE_ACTIVE   2  /* Currently active image */
#define QCOM_IMG_STATE_TESTING  3  /* Image being tested */

/* Image magic number "FWDT"*/
#define QCOM_IMG_MAGIC          0x54445746

/* Image header (simplified, no MCUboot dependency) */
struct qcom_image_header {
	uint32_t magic;           /* Magic number: 0x54445746 */
	uint32_t load_addr;       /* Load address */
	uint16_t hdr_size;        /* Header size */
	uint16_t pad1;
	uint32_t img_size;        /* Image size */
	uint32_t flags;           /* Image flags */
	struct {
		uint8_t major;
		uint8_t minor;
		uint16_t revision;
		uint32_t build_num;
	} version;
	uint32_t pad2;
} __packed;

/* Image info structure */
struct qcom_image_info {
	uint8_t slot;
	uint8_t state;
	struct qcom_image_header header;
	uint8_t hash[32];         /* SHA256 hash */
	bool valid;
};

/**
 * @brief Write image data chunk
 *
 * @param offset Offset in image
 * @param data Data buffer
 * @param len Data length
 * @return 0 on success, negative errno on failure
 */
int fw_upgrade_session_process(uint32_t offset, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* QCOM_IMG_MGMT_H_ */
