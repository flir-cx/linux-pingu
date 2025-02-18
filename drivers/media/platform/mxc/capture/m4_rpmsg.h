/* SPDX-License-Identifier: GPL-2.0 */
/*
 * m4_rpmsg.h
 *
 *  Created on: Jun 30, 2020
 *      Author: eaxelsso
 */

#ifndef SRC_M4_RPMSG_H_
#define SRC_M4_RPMSG_H_

enum ovRpmsg_mode {
	ovRpmsg_mode_MIN = 0,
	ovRpmsg_mode_QQVGA_160_120 = 0,
	ovRpmsg_mode_QQVGA_128_96 = 1,
	ovRpmsg_mode_QQVGA_160_122 = 2, //inc telemetry rows
	ovRpmsg_mode_QQVGA_164_122 = 3, //raw vospi frame
	ovRpmsg_mode_HQVGA = 4,
	ovRpmsg_mode_QVGA = 5,
	ovRpmsg_mode_256x192 = 6,
	ovRpmsg_mode_MAX = 6
};

enum ovRpmsg_format {
	ovRpmsg_format_MIN = 0,
	ovRpmsg_format_YUYV = 0,
	ovRpmsg_format_Y16 = 1,
	ovRpmsg_format_MAX = 1
};

#define IR_RESOLUTION_QVGA_WIDTH  320
#define IR_RESOLUTION_QVGA_HEIGHT  240
#define IR_RESOLUTION_HQVGA_WIDTH  240
#define IR_RESOLUTION_HQVGA_HEIGHT  160
#define IR_RESOLUTION_256x192_HEIGHT  192
#define IR_RESOLUTION_256x192_WIDTH  256
#define IR_RESOLUTION_FULL_WIDTH  160
#define IR_RESOLUTION_FULL_WIDTH_VOSPI  164
#define IR_RESOLUTION_FULL_HEIGHT 120
#define IR_RESOLUTION_FULL_TELEMETRY_HEIGHT 122
#define IR_RESOLUTION_REDUCED_WIDTH  128
#define IR_RESOLUTION_REDUCED_HEIGHT 96
#define IR_RESOLUTION_DEFAULT_WIDTH  IR_RESOLUTION_FULL_WIDTH
#define IR_RESOLUTION_DEFAULT_HEIGHT IR_RESOLUTION_FULL_HEIGHT

extern int rpmsg_send_buffer(dma_addr_t eba);
extern int rpmsg_drop_buffers(void);
extern int rpmsg_set_resolution(uint32_t res_mode);
extern int rpmsg_set_pixelformat(uint32_t res_format);
extern int rpmsg_setup_callback(
	void (*func)(uint32_t addr, uint32_t buf_num, void *ptr),
	void *ptr);

#endif /* SRC_M4_RPMSG_H_ */
