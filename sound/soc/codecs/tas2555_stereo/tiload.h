/*
** =============================================================================
** Copyright (c) 2016  Texas Instruments Inc.
**
** This program is free software; you can redistribute it and/or modify it under
** the terms of the GNU General Public License as published by the Free Software
** Foundation; version 2.
**
** This program is distributed in the hope that it will be useful, but WITHOUT
** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
** FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
**
** File:
**     tiload.h
**
** Description:
**     header file for tiload.c
**
** =============================================================================
*/

#ifndef _TILOAD_H
#define _TILOAD_H

#include "tas2555.h"

#define MAX_LENGTH 128

#define BPR_REG(book, page, reg)		(((book * 256 * 128) + \
						 (page * 128)) + reg)

/* typedefs required for the included header files */
typedef char *string;

typedef struct {
	unsigned char nBook;
	unsigned char nPage;
	unsigned char nRegister;
} BPR;

struct tiload_data {
	unsigned char mnBook;
	unsigned char mnPage;
	unsigned char mnMagicNum;
	int mnTiload_Opened;
	char mpRd_data[MAX_LENGTH + 1];
	char mpWr_data[MAX_LENGTH + 1];
};

/* defines */
#define CHL_DEVICE_NAME     "tiload_chl_node"
#define CHR_DEVICE_NAME     "tiload_chr_node"

#define TILOAD_IOC_MAGIC   0xE0
#define TILOAD_IOMAGICNUM_GET           _IOR(TILOAD_IOC_MAGIC, 1, int)
#define TILOAD_IOMAGICNUM_SET           _IOW(TILOAD_IOC_MAGIC, 2, int)
#define TILOAD_BPR_READ                 _IOR(TILOAD_IOC_MAGIC, 3, BPR)
#define TILOAD_BPR_WRITE                _IOW(TILOAD_IOC_MAGIC, 4, BPR)
#define TILOAD_IOCHLNUM_SET             _IOW(TILOAD_IOC_MAGIC, 5, int)
#define TILOAD_IOCTL_SET_CONFIG         _IOW(TILOAD_IOC_MAGIC, 6, int)
#define TILOAD_IOCTL_SET_CALIBRATION    _IOW(TILOAD_IOC_MAGIC, 7, int)

int tiload_driver_init(struct tas2555_priv *pTAS2555, unsigned char channel);

#endif
