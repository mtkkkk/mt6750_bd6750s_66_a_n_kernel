/* BMI160_ACC motion sensor driver
 *
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2011 Bosch Sensortec GmbH
 * All Rights Reserved
 *
 * VERSION: V1.5
 * HISTORY: V1.0 --- Driver creation
 *          V1.1 --- Add share I2C address function
 *          V1.2 --- Fix the bug that sometimes sensor is stuck after system resume.
 *          V1.3 --- Add FIFO interfaces.
 *          V1.4 --- Use basic i2c function to read fifo data instead of i2c DMA mode.
 *          V1.5 --- Add compensated value performed by MTK acceleration calibration process.
 */
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>

#include <linux/dma-mapping.h>

#include <accel.h>
#include <cust_acc.h>
#include "bmi160_acc.h"

#ifdef CONFIG_CUSTOM_KERNEL_SIGNIFICANT_MOTION_SENSOR
#define BMI160_INTERRUPT_ENABLE
#endif

#define SW_CALIBRATION
#define FIFO_READ_USE_DMA_MODE_I2C

#define BMM050_DEFAULT_DELAY	100
#define CALIBRATION_DATA_SIZE	12

#define BYTES_PER_LINE		(16)
#define WORK_DELAY_DEFAULT	(200)

static const struct i2c_device_id bmi160_acc_i2c_id[] = { {BMI160_DEV_NAME, 0}, {} };

static int bmi160_set_command_register(u8 cmd_reg);
static int bmi160_acc_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int bmi160_acc_i2c_remove(struct i2c_client *client);
static int bmi160_acc_local_init(void);
static int bmi160_acc_remove(void);

struct scale_factor {
	u8 whole;
	u8 fraction;
};

struct data_resolution {
	struct scale_factor scalefactor;
	int sensitivity;
};

struct data_filter {
	s16 raw[C_MAX_FIR_LENGTH][BMI160_ACC_AXES_NUM];
	int sum[BMI160_ACC_AXES_NUM];
	int num;
	int idx;
};

struct bmi160_acc_i2c_data {
	struct i2c_client *client;
	struct acc_hw *hw;
	struct hwmsen_convert cvt;
	struct bmi160_t device;
	/*misc */
	struct data_resolution *reso;
	atomic_t trace;
	atomic_t suspend;
	atomic_t selftest;
	atomic_t filter;
	s16 cali_sw[BMI160_ACC_AXES_NUM + 1];
	struct mutex lock;
	/* +1: for 4-byte alignment */
	s8 offset[BMI160_ACC_AXES_NUM + 1];
	s16 data[BMI160_ACC_AXES_NUM + 1];
	u8 fifo_count;

	u8 fifo_head_en;
	u8 fifo_data_sel;
	u16 fifo_bytecount;
	struct odr_t odr;
	u64 fifo_time;
	atomic_t layout;

	bool is_input_enable;
	struct delayed_work work;
	struct input_dev *input;
	struct hrtimer timer;
	int is_timer_running;
	struct work_struct report_data_work;
	ktime_t work_delay_kt;
	uint64_t time_odr;
	struct mutex mutex_ring_buf;
	atomic_t wkqueue_en;
	atomic_t delay;
	int IRQ;
	uint16_t gpio_pin;
	struct work_struct irq_work;
	/* step detector */
	u8 std;

	/*early suspend */
#if defined(CONFIG_HAS_EARLYSUSPEND)
	struct early_suspend early_drv;
#endif
};

struct bmi160_axis_data_t {
	s16 x;
	s16 y;
	s16 z;
};

struct i2c_client *bmi160_acc_i2c_client = NULL;
static struct acc_init_info bmi160_acc_init_info;
static struct bmi160_acc_i2c_data *obj_i2c_data;
static bool sensor_power = true;
static struct GSENSOR_VECTOR3D gsensor_gain;

/* signification motion flag*/
int sig_flag;
/* 0=OK, -1=fail */
static int bmi160_acc_init_flag = -1;
struct bmi160_t *p_bmi160;
struct acc_hw accel_cust;
static struct acc_hw *hw = &accel_cust;
static struct data_resolution bmi160_acc_data_resolution[] = {
	/*8 combination by {FULL_RES,RANGE} */
	{{0, 6}, 16384},	/*+/-2g  in 16-bit resolution:  0.06 mg/LSB */
	{{0, 12}, 8192},	/*+/-4g  in 16-bit resolution:  0.12 mg/LSB */
	{{0, 24}, 4096},	/*+/-8g  in 16-bit resolution:  0.24 mg/LSB */
	{{0, 5}, 2048},		/*+/-16g in 16-bit resolution:  0.49 mg/LSB */
};

static struct data_resolution bmi160_acc_offset_resolution = { {0, 12}, 8192 };

#ifdef FIFO_READ_USE_DMA_MODE_I2C
#include <linux/dma-mapping.h>

#ifndef I2C_MASK_FLAG
#define I2C_MASK_FLAG   (0x00ff)
#define I2C_DMA_FLAG    (0x2000)
#define I2C_ENEXT_FLAG  (0x0200)
#endif

#define ACCEL_DMA_MAX_TRANSACTION_LENGTH 1003

#ifdef CONFIG_MTK_I2C_EXTENSION
static u8 *gpDMABuf_va;
static dma_addr_t gpDMABuf_pa;
static int i2c_dma_read(struct i2c_client *client, uint8_t addr, uint8_t *readbuf, int32_t readlen)
{
	int err = 0;
	s32 retry = 0;
	struct i2c_msg msg[2] = {
		{
		 .addr = (client->addr & I2C_MASK_FLAG),
		 .flags = 0,
		 .buf = &addr,
		 .len = 1,
		 /* .timing = I2C_MASTER_CLOCK */
		 },
		{
		 .addr = (client->addr & I2C_MASK_FLAG),
		 .ext_flag = (client->ext_flag | I2C_ENEXT_FLAG | I2C_DMA_FLAG),
		 .flags = I2C_M_RD,
		 .buf = (u8 *) gpDMABuf_pa,
		 .len = readlen,
		 /* .timing = I2C_MASTER_CLOCK */
		 },
	};

	if (NULL == readbuf)
		return -1;

	for (retry = 0; retry < 5; ++retry) {
		err = i2c_transfer(client->adapter, &msg[0], 2);
		if (err < 0)
			continue;
		memcpy(readbuf, gpDMABuf_va, readlen);
		return 0;
	}
	GSE_ERR("DMA I2C read error: 0x%04X, %d byte(s), err-code: %d", addr, readlen, err);
	return err;
}
#else
static int i2c_dma_read(struct i2c_client *client, uint8_t addr, uint8_t *readbuf, int32_t readlen)
{
	int err = 0;
	struct i2c_msg msgs[2] = {
		{
		 .addr = client->addr,
		 .flags = 0,
		 .len = 1,
		 .buf = &addr},
		{
		 .addr = client->addr,
		 .flags = I2C_M_RD,
		 .len = readlen,
		 .buf = readbuf,
		 }
	};
	err += i2c_transfer(client->adapter, &msgs[0], 1);
	err += i2c_transfer(client->adapter, &msgs[1], 1);
	if (err != 2)
		err = -EIO;
	else
		err = 0;
	return err;
}
#endif				/* #ifdef CONFIG_MTK_I2C_EXTENSION */
#endif				/* #ifdef FIFO_READ_USE_DMA_MODE_I2C */

/* I2C operation functions */
int bma_i2c_write_block(struct i2c_client *client, u8 addr, u8 *data, u8 len)
{
	int err = 0;
	unsigned char *buffer = NULL;
	struct i2c_msg msg;

	memset(&msg, 0, sizeof(struct i2c_msg));
	buffer = kzalloc(len + 1, GFP_KERNEL);
	if (buffer == NULL)
		return -1;
	buffer[0] = addr;
	memcpy((buffer + 1), data, len);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len + 1;
	msg.buf = buffer;

	err = i2c_transfer(client->adapter, &msg, 1);
	if (err != 1)
		err = -EIO;
	else
		err = 0;
	kfree(buffer);
	return err;
}
EXPORT_SYMBOL_GPL(bma_i2c_write_block);

#ifdef CONFIG_MTK_I2C_EXTENSION
int bma_i2c_read_block(struct i2c_client *client, u8 addr, u8 *data, u8 len)
{
	int err = 0;
	struct i2c_msg msg;

	memset(&msg, 0, sizeof(struct i2c_msg));

	data[0] = addr;
	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = ((len & 0x1f) << 8) | 1;
	msg.buf = data;
	msg.ext_flag = I2C_WR_FLAG | I2C_RS_FLAG;

	err = i2c_transfer(client->adapter, &msg, 1);
	if (err != 1)
		err = -EIO;
	else
		err = 0;
	return err;
}

#else

int bma_i2c_read_block(struct i2c_client *client, u8 addr, u8 *data, u8 len)
{
	int err;
	struct i2c_msg msgs[2] = {
		{
		 .addr = client->addr,
		 .flags = 0,
		 .len = 1,
		 .buf = &addr},
		{
		 .addr = client->addr,
		 .flags = I2C_M_RD,
		 .len = len,
		 .buf = data,
		 }
	};
	err = i2c_transfer(client->adapter, msgs, 2);
	if (err != 2)
		err = -EIO;
	else
		err = 0;
	return err;
}
#endif
EXPORT_SYMBOL_GPL(bma_i2c_read_block);

s8 bmi_i2c_read_wrapper(u8 dev_addr, u8 reg_addr, u8 *data, u8 len)
{
	int err = 0;

	err = bma_i2c_read_block(bmi160_acc_i2c_client, reg_addr, data, len);
	if (err < 0)
		GSE_ERR("read bmi160 i2c failed.\n");
	return err;
}

s8 bmi_i2c_write_wrapper(u8 dev_addr, u8 reg_addr, u8 *data, u8 len)
{
	int err = 0;

	err = bma_i2c_write_block(bmi160_acc_i2c_client, reg_addr, data, len);
	if (err < 0)
		GSE_ERR("read bmi160 i2c failed.\n");
	return err;
}

static void bmi_delay(u32 msec)
{
	mdelay(msec);
}

static void BMI160_ACC_power(struct acc_hw *hw, unsigned int on)
{

}

/*!
 * @brief Set data resolution
 *
 * @param[in] client the pointer of bmi160_acc_i2c_data
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_SetDataResolution(struct bmi160_acc_i2c_data *obj)
{

	int res = 0;
	u8 reso;
	u8 databuf[2] = { 0 };

	mutex_lock(&obj->lock);
	res = bma_i2c_read_block(obj->client, BMI160_USER_ACC_RANGE__REG, databuf, 1);
	mutex_unlock(&obj->lock);
	if (res < 0) {
		GSE_ERR("read data format fail: %d!!\n", res);
		return res;
	}
	databuf[0] = databuf[0] & BMI160_USER_ACC_RANGE__MSK;

	reso = 0xFF;
	switch (databuf[0]) {
	case BMI160_ACCEL_RANGE_2G:
		reso = 0;
		break;
	case BMI160_ACCEL_RANGE_4G:
		reso = 1;
		break;
	case BMI160_ACCEL_RANGE_8G:
		reso = 2;
		break;
	case BMI160_ACCEL_RANGE_16G:
		reso = 3;
		break;
	default:
		break;
	}

	if (reso < sizeof(bmi160_acc_data_resolution) / sizeof(bmi160_acc_data_resolution[0])) {
		obj->reso = &bmi160_acc_data_resolution[reso];
		GSE_LOG("BMI160_ACC_SetDataResolution: %d\n", obj->reso->sensitivity);
		return 0;
	} else {
		return -EINVAL;
	}
}

static int BMI160_ACC_ReadData(struct i2c_client *client, s16 data[BMI160_ACC_AXES_NUM])
{
	int err = 0;
	u8 addr = BMI160_USER_DATA_14_ACC_X_LSB__REG;
	u8 buf[BMI160_ACC_DATA_LEN] = { 0 };

	err = bma_i2c_read_block(client, addr, buf, BMI160_ACC_DATA_LEN);
	if (err < 0)
		GSE_ERR("read data failed.\n");
	else {
		/* Convert sensor raw data to 16-bit integer */
		/* Data X */
		data[BMI160_ACC_AXIS_X] = (s16) ((((s32) ((s8) buf[1]))
						  << BMI160_SHIFT_8_POSITION) | (buf[0]));
		/* Data Y */
		data[BMI160_ACC_AXIS_Y] = (s16) ((((s32) ((s8) buf[3]))
						  << BMI160_SHIFT_8_POSITION) | (buf[2]));
		/* Data Z */
		data[BMI160_ACC_AXIS_Z] = (s16) ((((s32) ((s8) buf[5]))
						  << BMI160_SHIFT_8_POSITION) | (buf[4]));
	}
	return err;
}

static int BMI160_ACC_ReadOffset(struct i2c_client *client, s8 ofs[BMI160_ACC_AXES_NUM])
{
	int err = 0;
#ifdef SW_CALIBRATION
	ofs[0] = ofs[1] = ofs[2] = 0x0;
#else
	err = bma_i2c_read_block(client, BMI160_ACC_REG_OFSX, ofs, BMI160_ACC_AXES_NUM);
	if (err < 0)
		GSE_ERR("error: %d\n", err);
#endif
	return err;
}

/*!
 * @brief Reset calibration for acc
 *
 * @param[in] client the pointer of i2c_client
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_ResetCalibration(struct i2c_client *client)
{
	int err = 0;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;
#ifdef SW_CALIBRATION

#else
	u8 ofs[4] = { 0, 0, 0, 0 };

	err = bma_i2c_write_block(client, BMI160_ACC_REG_OFSX, ofs, 4);
	if (err)
		GSE_ERR("write offset failed.\n");
#endif
	memset(obj->cali_sw, 0x00, sizeof(obj->cali_sw));
	memset(obj->offset, 0x00, sizeof(obj->offset));
	return err;
}

static int BMI160_ACC_ReadCalibration(struct i2c_client *client, int dat[BMI160_ACC_AXES_NUM])
{
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;
	int err = 0;
	int mul;

#ifdef SW_CALIBRATION
	mul = 0;		/* only SW Calibration, disable HW Calibration */
#else
	err = BMI160_ACC_ReadOffset(client, obj->offset);
	if (err) {
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}
	mul = obj->reso->sensitivity / bmi160_acc_offset_resolution.sensitivity;
#endif

	dat[obj->cvt.map[BMI160_ACC_AXIS_X]] = obj->cvt.sign[BMI160_ACC_AXIS_X]
	    * (obj->offset[BMI160_ACC_AXIS_X] * mul + obj->cali_sw[BMI160_ACC_AXIS_X]);
	dat[obj->cvt.map[BMI160_ACC_AXIS_Y]] = obj->cvt.sign[BMI160_ACC_AXIS_Y]
	    * (obj->offset[BMI160_ACC_AXIS_Y] * mul + obj->cali_sw[BMI160_ACC_AXIS_Y]);
	dat[obj->cvt.map[BMI160_ACC_AXIS_Z]] = obj->cvt.sign[BMI160_ACC_AXIS_Z]
	    * (obj->offset[BMI160_ACC_AXIS_Z] * mul + obj->cali_sw[BMI160_ACC_AXIS_Z]);

	return err;
}

static int BMI160_ACC_ReadCalibrationEx(struct i2c_client *client,
					int act[BMI160_ACC_AXES_NUM], int raw[BMI160_ACC_AXES_NUM])
{
	/* raw: the raw calibration data; act: the actual calibration data */
	int mul;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;
#ifdef SW_CALIBRATION
	/* only SW Calibration, disable  Calibration */
	mul = 0;
#else
	int err;

	err = BMI160_ACC_ReadOffset(client, obj->offset);
	if (err) {
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}
	mul = obj->reso->sensitivity / bmi160_acc_offset_resolution.sensitivity;
#endif

	raw[BMI160_ACC_AXIS_X] =
	    obj->offset[BMI160_ACC_AXIS_X] * mul + obj->cali_sw[BMI160_ACC_AXIS_X];
	raw[BMI160_ACC_AXIS_Y] =
	    obj->offset[BMI160_ACC_AXIS_Y] * mul + obj->cali_sw[BMI160_ACC_AXIS_Y];
	raw[BMI160_ACC_AXIS_Z] =
	    obj->offset[BMI160_ACC_AXIS_Z] * mul + obj->cali_sw[BMI160_ACC_AXIS_Z];

	act[obj->cvt.map[BMI160_ACC_AXIS_X]] =
	    obj->cvt.sign[BMI160_ACC_AXIS_X] * raw[BMI160_ACC_AXIS_X];
	act[obj->cvt.map[BMI160_ACC_AXIS_Y]] =
	    obj->cvt.sign[BMI160_ACC_AXIS_Y] * raw[BMI160_ACC_AXIS_Y];
	act[obj->cvt.map[BMI160_ACC_AXIS_Z]] =
	    obj->cvt.sign[BMI160_ACC_AXIS_Z] * raw[BMI160_ACC_AXIS_Z];

	return 0;
}

static int BMI160_ACC_WriteCalibration(struct i2c_client *client, int dat[BMI160_ACC_AXES_NUM])
{
	int err = 0;
	int cali[BMI160_ACC_AXES_NUM] = { 0 };
	int raw[BMI160_ACC_AXES_NUM] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;
#ifndef SW_CALIBRATION
	int lsb = bmi160_acc_offset_resolution.sensitivity;
	int divisor = obj->reso->sensitivity / lsb;
#endif
	err = BMI160_ACC_ReadCalibrationEx(client, cali, raw);
	/* offset will be updated in obj->offset */
	if (err) {
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}
	GSE_LOG("OLDOFF: (%+3d %+3d %+3d): (%+3d %+3d %+3d) / (%+3d %+3d %+3d)\n",
		raw[BMI160_ACC_AXIS_X], raw[BMI160_ACC_AXIS_Y], raw[BMI160_ACC_AXIS_Z],
		obj->offset[BMI160_ACC_AXIS_X], obj->offset[BMI160_ACC_AXIS_Y],
		obj->offset[BMI160_ACC_AXIS_Z], obj->cali_sw[BMI160_ACC_AXIS_X],
		obj->cali_sw[BMI160_ACC_AXIS_Y], obj->cali_sw[BMI160_ACC_AXIS_Z]);
	/* calculate the real offset expected by caller */
	cali[BMI160_ACC_AXIS_X] += dat[BMI160_ACC_AXIS_X];
	cali[BMI160_ACC_AXIS_Y] += dat[BMI160_ACC_AXIS_Y];
	cali[BMI160_ACC_AXIS_Z] += dat[BMI160_ACC_AXIS_Z];
	GSE_LOG("UPDATE: (%+3d %+3d %+3d)\n",
		dat[BMI160_ACC_AXIS_X], dat[BMI160_ACC_AXIS_Y], dat[BMI160_ACC_AXIS_Z]);
#ifdef SW_CALIBRATION
	obj->cali_sw[BMI160_ACC_AXIS_X] =
	    obj->cvt.sign[BMI160_ACC_AXIS_X] * (cali[obj->cvt.map[BMI160_ACC_AXIS_X]]);
	obj->cali_sw[BMI160_ACC_AXIS_Y] =
	    obj->cvt.sign[BMI160_ACC_AXIS_Y] * (cali[obj->cvt.map[BMI160_ACC_AXIS_Y]]);
	obj->cali_sw[BMI160_ACC_AXIS_Z] =
	    obj->cvt.sign[BMI160_ACC_AXIS_Z] * (cali[obj->cvt.map[BMI160_ACC_AXIS_Z]]);
#else
	obj->offset[BMI160_ACC_AXIS_X] =
	    (s8) (obj->cvt.sign[BMI160_ACC_AXIS_X] * (cali[obj->cvt.map[BMI160_ACC_AXIS_X]]) /
		  (divisor));
	obj->offset[BMI160_ACC_AXIS_Y] =
	    (s8) (obj->cvt.sign[BMI160_ACC_AXIS_Y] * (cali[obj->cvt.map[BMI160_ACC_AXIS_Y]]) /
		  (divisor));
	obj->offset[BMI160_ACC_AXIS_Z] =
	    (s8) (obj->cvt.sign[BMI160_ACC_AXIS_Z] * (cali[obj->cvt.map[BMI160_ACC_AXIS_Z]]) /
		  (divisor));

	/*convert software calibration using standard calibration */
	obj->cali_sw[BMI160_ACC_AXIS_X] =
	    obj->cvt.sign[BMI160_ACC_AXIS_X] * (cali[obj->cvt.map[BMI160_ACC_AXIS_X]]) % (divisor);
	obj->cali_sw[BMI160_ACC_AXIS_Y] =
	    obj->cvt.sign[BMI160_ACC_AXIS_Y] * (cali[obj->cvt.map[BMI160_ACC_AXIS_Y]]) % (divisor);
	obj->cali_sw[BMI160_ACC_AXIS_Z] =
	    obj->cvt.sign[BMI160_ACC_AXIS_Z] * (cali[obj->cvt.map[BMI160_ACC_AXIS_Z]]) % (divisor);

	GSE_LOG("NEWOFF: (%+3d %+3d %+3d): (%+3d %+3d %+3d) / (%+3d %+3d %+3d)\n",
		obj->offset[BMI160_ACC_AXIS_X] * divisor + obj->cali_sw[BMI160_ACC_AXIS_X],
		obj->offset[BMI160_ACC_AXIS_Y] * divisor + obj->cali_sw[BMI160_ACC_AXIS_Y],
		obj->offset[BMI160_ACC_AXIS_Z] * divisor + obj->cali_sw[BMI160_ACC_AXIS_Z],
		obj->offset[BMI160_ACC_AXIS_X], obj->offset[BMI160_ACC_AXIS_Y],
		obj->offset[BMI160_ACC_AXIS_Z], obj->cali_sw[BMI160_ACC_AXIS_X],
		obj->cali_sw[BMI160_ACC_AXIS_Y], obj->cali_sw[BMI160_ACC_AXIS_Z]);

	err =
	    bma_i2c_write_block(obj->client, BMI160_ACC_REG_OFSX, obj->offset, BMI160_ACC_AXES_NUM);
	if (err) {
		GSE_ERR("write offset fail: %d\n", err);
		return err;
	}
#endif

	return err;
}

/*!
 * @brief Input event initialization for device
 *
 * @param[in] client the pointer of i2c_client
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_CheckDeviceID(struct i2c_client *client)
{
	int err = 0;
	u8 databuf[2] = { 0 };

	err = bma_i2c_read_block(client, BMI160_USER_CHIP_ID__REG, databuf, 1);
	err = bma_i2c_read_block(client, BMI160_USER_CHIP_ID__REG, databuf, 1);
	if (err < 0) {
		GSE_ERR("read chip id failed.\n");
		return BMI160_ACC_ERR_I2C;
	}
	switch (databuf[0]) {
	case SENSOR_CHIP_ID_BMI:
	case SENSOR_CHIP_ID_BMI_C2:
	case SENSOR_CHIP_ID_BMI_C3:
		GSE_LOG("check chip id %d successfully.\n", databuf[0]);
		break;
	default:
		GSE_LOG("check chip id %d failed.\n", databuf[0]);
		break;
	}
	return err;
}

/*!
 * @brief Set power mode for acc
 *
 * @param[in] client the pointer of i2c_client
 * @param[in] enable
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_SetPowerMode(struct i2c_client *client, bool enable)
{
	int err = 0;
	u8 databuf[2] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (enable == sensor_power) {
		GSE_LOG("power status is newest!\n");
		return 0;
	}
	mutex_lock(&obj->lock);
	if (enable == true)
		databuf[0] = CMD_PMU_ACC_NORMAL;
	else
		databuf[0] = CMD_PMU_ACC_SUSPEND;
	err = bma_i2c_write_block(client, BMI160_CMD_COMMANDS__REG, &databuf[0], 1);
	if (err < 0) {
		GSE_ERR("write power mode value to register failed.\n");
		mutex_unlock(&obj->lock);
		return BMI160_ACC_ERR_I2C;
	}
	sensor_power = enable;
	mdelay(1);
	mutex_unlock(&obj->lock);
	GSE_LOG("set power mode enable = %d ok!\n", enable);
	return 0;
}

/*!
 * @brief Set range value for acc
 *
 * @param[in] client the pointer of i2c_client
 * @param[in] range value
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_SetDataFormat(struct i2c_client *client, u8 dataformat)
{
	int res = 0;
	u8 databuf[2] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	mutex_lock(&obj->lock);
	res = bma_i2c_read_block(client, BMI160_USER_ACC_RANGE__REG, &databuf[0], 1);
	databuf[0] = BMI160_SET_BITSLICE(databuf[0], BMI160_USER_ACC_RANGE, dataformat);
	res += bma_i2c_write_block(client, BMI160_USER_ACC_RANGE__REG, &databuf[0], 1);
	mdelay(1);
	if (res < 0) {
		GSE_ERR("set range failed.\n");
		mutex_unlock(&obj->lock);
		return BMI160_ACC_ERR_I2C;
	}
	mutex_unlock(&obj->lock);
	GSE_LOG("BMI160_ACC_SetDataFormat(0x%x)\n", dataformat);
	return BMI160_ACC_SetDataResolution(obj);
}

/*!
 * @brief Set bandwidth for acc
 *
 * @param[in] client the pointer of i2c_client
 * @param[in] bandwidth value
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_SetBWRate(struct i2c_client *client, u8 bwrate)
{
	int err = 0;
	u8 databuf[2] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	mutex_lock(&obj->lock);
	err = bma_i2c_read_block(client, BMI160_USER_ACC_CONF_ODR__REG, &databuf[0], 1);
	databuf[0] = BMI160_SET_BITSLICE(databuf[0], BMI160_USER_ACC_CONF_ODR, bwrate);
	err += bma_i2c_write_block(client, BMI160_USER_ACC_CONF_ODR__REG, &databuf[0], 1);
	mdelay(20);
	if (err < 0) {
		GSE_ERR("set bandwidth failed, res = %d\n", err);
		mutex_unlock(&obj->lock);
		return BMI160_ACC_ERR_I2C;
	}
	GSE_LOG("set bandwidth = %d ok.\n", bwrate);
	mutex_unlock(&obj->lock);
	return err;
}

/*!
 * @brief Set OSR for acc
 *
 * @param[in] client the pointer of i2c_client
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_SetOSR4(struct i2c_client *client)
{
	int err = 0;
	uint8_t databuf[2] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;
	uint8_t bandwidth = BMI160_ACCEL_OSR4_AVG1;
	uint8_t accel_undersampling_parameter = 0;

	mutex_lock(&obj->lock);
	err = bma_i2c_read_block(client, BMI160_USER_ACC_CONF_ODR__REG, &databuf[0], 1);
	databuf[0] = BMI160_SET_BITSLICE(databuf[0], BMI160_USER_ACC_CONF_ACC_BWP, bandwidth);
	databuf[0] = BMI160_SET_BITSLICE(databuf[0],
					 BMI160_USER_ACC_CONF_ACC_UNDER_SAMPLING,
					 accel_undersampling_parameter);
	err += bma_i2c_write_block(client, BMI160_USER_ACC_CONF_ODR__REG, &databuf[0], 1);
	mdelay(10);
	if (err < 0) {
		GSE_ERR("set OSR failed.\n");
		mutex_unlock(&obj->lock);
		return BMI160_ACC_ERR_I2C;
	}
	GSE_LOG("[%s] acc_bmp = %d, acc_us = %d ok.\n", __func__,
		bandwidth, accel_undersampling_parameter);
	mutex_unlock(&obj->lock);
	return err;
}

/*!
 * @brief Set interrupt enable
 *
 * @param[in] client the pointer of i2c_client
 * @param[in] enable for interrupt
 *
 * @return zero success, non-zero failed
 */
static int BMI160_ACC_SetIntEnable(struct i2c_client *client, u8 enable)
{
	int err = 0;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	mutex_lock(&obj->lock);
	err = bma_i2c_write_block(client, BMI160_USER_INT_EN_0_ADDR, &enable, 0x01);
	mdelay(1);
	if (err < 0) {
		mutex_unlock(&obj->lock);
		return err;
	}
	err = bma_i2c_write_block(client, BMI160_USER_INT_EN_1_ADDR, &enable, 0x01);
	mdelay(1);
	if (err < 0) {
		mutex_unlock(&obj->lock);
		return err;
	}
	err = bma_i2c_write_block(client, BMI160_USER_INT_EN_2_ADDR, &enable, 0x01);
	mdelay(1);
	if (err < 0) {
		mutex_unlock(&obj->lock);
		return err;
	}
	mutex_unlock(&obj->lock);
	GSE_LOG("bmi160 set interrupt enable = %d ok.\n", enable);
	return 0;
}

/*!
 * @brief bmi160 initialization
 *
 * @param[in] client the pointer of i2c_client
 * @param[in] int reset calibration value
 *
 * @return zero success, non-zero failed
 */
static int bmi160_acc_init_client(struct i2c_client *client, int reset_cali)
{
	int err = 0;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	err = BMI160_ACC_CheckDeviceID(client);
	if (err < 0)
		return err;

	/* soft reset */
	err = bmi160_set_command_register(0xB6);
	if (err < 0)
		return err;
	mdelay(5);

	err = BMI160_ACC_SetBWRate(client, BMI160_ACCEL_ODR_200HZ);
	if (err < 0)
		return err;
	err = BMI160_ACC_SetOSR4(client);
	if (err < 0)
		return err;
	err = BMI160_ACC_SetDataFormat(client, BMI160_ACCEL_RANGE_4G);
	if (err < 0)
		return err;
	gsensor_gain.x = gsensor_gain.y = gsensor_gain.z = obj->reso->sensitivity;
	err = BMI160_ACC_SetIntEnable(client, 0);
	if (err < 0)
		return err;
	err = BMI160_ACC_SetPowerMode(client, false);
	if (err < 0)
		return err;
	if (0 != reset_cali) {
		/* reset calibration only in power on */
		err = BMI160_ACC_ResetCalibration(client);
		if (err < 0)
			return err;
	}
	GSE_LOG("bmi160 acc init OK.\n");
	return 0;
}

static int BMI160_ACC_ReadChipInfo(struct i2c_client *client, char *buf, int bufsize)
{
	sprintf(buf, "bmi160_acc");
	return 0;
}

static int BMI160_ACC_CompassReadData(struct i2c_client *client, char *buf, int bufsize)
{
	int res = 0;
	int acc[BMI160_ACC_AXES_NUM] = { 0 };
	s16 databuf[BMI160_ACC_AXES_NUM] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (sensor_power == false) {
		res = BMI160_ACC_SetPowerMode(client, true);
		if (res)
			GSE_ERR("Power on bmi160_acc error %d!\n", res);
	}
	res = BMI160_ACC_ReadData(client, databuf);
	if (res) {
		GSE_ERR("read acc data failed.\n");
		res = -3;
	} else {
		/* Add compensated value performed by MTK calibration process */
		databuf[BMI160_ACC_AXIS_X] += obj->cali_sw[BMI160_ACC_AXIS_X];
		databuf[BMI160_ACC_AXIS_Y] += obj->cali_sw[BMI160_ACC_AXIS_Y];
		databuf[BMI160_ACC_AXIS_Z] += obj->cali_sw[BMI160_ACC_AXIS_Z];
		/*remap coordinate */
		acc[obj->cvt.map[BMI160_ACC_AXIS_X]] =
		    obj->cvt.sign[BMI160_ACC_AXIS_X] * databuf[BMI160_ACC_AXIS_X];
		acc[obj->cvt.map[BMI160_ACC_AXIS_Y]] =
		    obj->cvt.sign[BMI160_ACC_AXIS_Y] * databuf[BMI160_ACC_AXIS_Y];
		acc[obj->cvt.map[BMI160_ACC_AXIS_Z]] =
		    obj->cvt.sign[BMI160_ACC_AXIS_Z] * databuf[BMI160_ACC_AXIS_Z];

		sprintf(buf, "%d %d %d",
			(s16) acc[BMI160_ACC_AXIS_X],
			(s16) acc[BMI160_ACC_AXIS_Y], (s16) acc[BMI160_ACC_AXIS_Z]);
	}
	return res;
}

static int BMI160_ACC_ReadSensorData(struct i2c_client *client, char *buf, int bufsize)
{
	int err = 0;
	int acc[BMI160_ACC_AXES_NUM] = { 0 };
	s16 databuf[BMI160_ACC_AXES_NUM] = { 0 };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (sensor_power == false) {
		err = BMI160_ACC_SetPowerMode(client, true);
		if (err) {
			GSE_ERR("set power on acc failed.\n");
			return err;
		}
	}
	err = BMI160_ACC_ReadData(client, databuf);
	if (err) {
		GSE_ERR("read acc data failed.\n");
	} else {
		databuf[BMI160_ACC_AXIS_X] += obj->cali_sw[BMI160_ACC_AXIS_X];
		databuf[BMI160_ACC_AXIS_Y] += obj->cali_sw[BMI160_ACC_AXIS_Y];
		databuf[BMI160_ACC_AXIS_Z] += obj->cali_sw[BMI160_ACC_AXIS_Z];
		/* remap coordinate */
		acc[obj->cvt.map[BMI160_ACC_AXIS_X]] =
		    obj->cvt.sign[BMI160_ACC_AXIS_X] * databuf[BMI160_ACC_AXIS_X];
		acc[obj->cvt.map[BMI160_ACC_AXIS_Y]] =
		    obj->cvt.sign[BMI160_ACC_AXIS_Y] * databuf[BMI160_ACC_AXIS_Y];
		acc[obj->cvt.map[BMI160_ACC_AXIS_Z]] =
		    obj->cvt.sign[BMI160_ACC_AXIS_Z] * databuf[BMI160_ACC_AXIS_Z];
		/* Output the mg */
		acc[BMI160_ACC_AXIS_X] = acc[BMI160_ACC_AXIS_X]
		    * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		acc[BMI160_ACC_AXIS_Y] = acc[BMI160_ACC_AXIS_Y]
		    * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		acc[BMI160_ACC_AXIS_Z] = acc[BMI160_ACC_AXIS_Z]
		    * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		GSE_LOG("acc final xyz data: %d,%d,%d, sens:%d\n",
			acc[0], acc[1], acc[2], obj->reso->sensitivity);

		sprintf(buf, "%04x %04x %04x",
			acc[BMI160_ACC_AXIS_X], acc[BMI160_ACC_AXIS_Y], acc[BMI160_ACC_AXIS_Z]);
	}
	return err;
}

static int BMI160_ACC_ReadRawData(struct i2c_client *client, char *buf)
{
	int err = 0;
	s16 databuf[BMI160_ACC_AXES_NUM] = { 0 };

	err = BMI160_ACC_ReadData(client, databuf);
	if (err) {
		GSE_ERR("read acc raw data failed.\n");
	} else {
		sprintf(buf, "BMI160_ACC_ReadRawData %04x %04x %04x",
			databuf[BMI160_ACC_AXIS_X],
			databuf[BMI160_ACC_AXIS_Y], databuf[BMI160_ACC_AXIS_Z]);
	}
	return err;
}

int bmi160_acc_get_mode(struct i2c_client *client, unsigned char *mode)
{
	int comres = 0;
	u8 v_data_u8r = C_BMI160_ZERO_U8X;

	comres = bma_i2c_read_block(client, BMI160_USER_ACC_PMU_STATUS__REG, &v_data_u8r, 1);
	*mode = BMI160_GET_BITSLICE(v_data_u8r, BMI160_USER_ACC_PMU_STATUS);
	return comres;
}

static int bmi160_acc_set_range(struct i2c_client *client, unsigned char range)
{
	return BMI160_ACC_SetDataFormat(client, (u8) range);
/*
	int comres = 0;
	unsigned char data[2] = {BMI160_USER_ACC_RANGE__REG};
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (client == NULL)
	{
		return -1;
	}
	mutex_lock(&obj->lock);
	comres = bma_i2c_read_block(client,
			BMI160_USER_ACC_RANGE__REG, data+1, 1);

	data[1]  = BMI160_SET_BITSLICE(data[1],
			BMI160_USER_ACC_RANGE, range);

	comres = i2c_master_send(client, data, 2);
	mutex_unlock(&obj->lock);
	if(comres <= 0)
	{
		return BMI160_ACC_ERR_I2C;
	}
	else
	{
		return comres;
	}
*/
}

static int bmi160_acc_get_range(struct i2c_client *client, u8 *range)
{
	int comres = 0;
	u8 data;

	comres = bma_i2c_read_block(client, BMI160_USER_ACC_RANGE__REG, &data, 1);
	*range = BMI160_GET_BITSLICE(data, BMI160_USER_ACC_RANGE);
	return comres;
}

static int bmi160_acc_set_bandwidth(struct i2c_client *client, unsigned char bandwidth)
{
	int comres = 0;
	unsigned char data[2] = { BMI160_USER_ACC_CONF_ODR__REG };
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	GSE_LOG("[%s] bandwidth = %d\n", __func__, bandwidth);
	mutex_lock(&obj->lock);
	comres = bma_i2c_read_block(client, BMI160_USER_ACC_CONF_ODR__REG, data + 1, 1);
	data[1] = BMI160_SET_BITSLICE(data[1], BMI160_USER_ACC_CONF_ODR, bandwidth);
	comres = i2c_master_send(client, data, 2);
	mdelay(1);
	mutex_unlock(&obj->lock);
	if (comres <= 0)
		return BMI160_ACC_ERR_I2C;
	else
		return comres;
}

static int bmi160_acc_get_bandwidth(struct i2c_client *client, unsigned char *bandwidth)
{
	int comres = 0;

	comres = bma_i2c_read_block(client, BMI160_USER_ACC_CONF_ODR__REG, bandwidth, 1);
	*bandwidth = BMI160_GET_BITSLICE(*bandwidth, BMI160_USER_ACC_CONF_ODR);
	return comres;
}

static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	char strbuf[BMI160_BUFSIZE];
	struct i2c_client *client = bmi160_acc_i2c_client;

	BMI160_ACC_ReadChipInfo(client, strbuf, BMI160_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);
}

static ssize_t show_acc_op_mode_value(struct device_driver *ddri, char *buf)
{
	int err = 0;
	u8 data = 0;

	err = bmi160_acc_get_mode(bmi160_acc_i2c_client, &data);
	if (err < 0) {
		GSE_ERR("get acc op mode failed.\n");
		return err;
	}
	return sprintf(buf, "%d\n", data);
}

static ssize_t store_acc_op_mode_value(struct device_driver *ddri, const char *buf, size_t count)
{
	int err;
	unsigned long data;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	GSE_ERR("store_acc_op_mode_value = %d .\n", (int)data);
	if (data == BMI160_ACC_MODE_NORMAL)
		err = BMI160_ACC_SetPowerMode(bmi160_acc_i2c_client, true);
	else
		err = BMI160_ACC_SetPowerMode(bmi160_acc_i2c_client, false);
	if (err < 0) {
		GSE_ERR("set acc op mode = %d failed.\n", (int)data);
		return err;
	}
	return count;
}

static ssize_t show_acc_range_value(struct device_driver *ddri, char *buf)
{
	int err = 0;
	u8 data;

	err = bmi160_acc_get_range(bmi160_acc_i2c_client, &data);
	if (err < 0) {
		GSE_ERR("get acc range failed.\n");
		return err;
	}
	return sprintf(buf, "%d\n", data);
}

static ssize_t store_acc_range_value(struct device_driver *ddri, const char *buf, size_t count)
{
	unsigned long data;
	int err;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	err = bmi160_acc_set_range(bmi160_acc_i2c_client, (u8) data);
	if (err < 0) {
		GSE_ERR("set acc range = %d failed.\n", (int)data);
		return err;
	}
	return count;
}

static ssize_t show_acc_odr_value(struct device_driver *ddri, char *buf)
{
	int err;
	u8 data;

	err = bmi160_acc_get_bandwidth(bmi160_acc_i2c_client, &data);
	if (err < 0) {
		GSE_ERR("get acc odr failed.\n");
		return err;
	}
	return sprintf(buf, "%d\n", data);
}

static ssize_t store_acc_odr_value(struct device_driver *ddri, const char *buf, size_t count)
{
	unsigned long data;
	int err;
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	err = bmi160_acc_set_bandwidth(bmi160_acc_i2c_client, (u8) data);
	if (err < 0) {
		GSE_ERR("set acc bandwidth failed.\n");
		return err;
	}
	client_data->odr.acc_odr = data;
	return count;
}

static ssize_t show_cpsdata_value(struct device_driver *ddri, char *buf)
{
	char strbuf[BMI160_BUFSIZE] = { 0 };
	struct i2c_client *client = bmi160_acc_i2c_client;

	BMI160_ACC_CompassReadData(client, strbuf, BMI160_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);
}

static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{
	char strbuf[BMI160_BUFSIZE] = { 0 };
	struct i2c_client *client = bmi160_acc_i2c_client;

	BMI160_ACC_ReadSensorData(client, strbuf, BMI160_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);
}

static ssize_t show_cali_value(struct device_driver *ddri, char *buf)
{
	int err = 0;
	int len = 0;
	int mul;
	int tmp[BMI160_ACC_AXES_NUM] = { 0 };
	struct bmi160_acc_i2c_data *obj;
	struct i2c_client *client = bmi160_acc_i2c_client;

	obj = obj_i2c_data;
	err = BMI160_ACC_ReadOffset(client, obj->offset);
	if (err)
		return -EINVAL;
	err = BMI160_ACC_ReadCalibration(client, tmp);
	if (err)
		return -EINVAL;

	mul = obj->reso->sensitivity / bmi160_acc_offset_resolution.sensitivity;
	len +=
	    snprintf(buf + len, PAGE_SIZE - len,
		     "[HW ][%d] (%+3d, %+3d, %+3d) : (0x%02X, 0x%02X, 0x%02X)\n", mul,
		     obj->offset[BMI160_ACC_AXIS_X], obj->offset[BMI160_ACC_AXIS_Y],
		     obj->offset[BMI160_ACC_AXIS_Z], obj->offset[BMI160_ACC_AXIS_X],
		     obj->offset[BMI160_ACC_AXIS_Y], obj->offset[BMI160_ACC_AXIS_Z]);
	len +=
	    snprintf(buf + len, PAGE_SIZE - len, "[SW ][%d] (%+3d, %+3d, %+3d)\n", 1,
		     obj->cali_sw[BMI160_ACC_AXIS_X], obj->cali_sw[BMI160_ACC_AXIS_Y],
		     obj->cali_sw[BMI160_ACC_AXIS_Z]);

	len +=
	    snprintf(buf + len, PAGE_SIZE - len,
		     "[ALL]    (%+3d, %+3d, %+3d) : (%+3d, %+3d, %+3d)\n",
		     obj->offset[BMI160_ACC_AXIS_X] * mul + obj->cali_sw[BMI160_ACC_AXIS_X],
		     obj->offset[BMI160_ACC_AXIS_Y] * mul + obj->cali_sw[BMI160_ACC_AXIS_Y],
		     obj->offset[BMI160_ACC_AXIS_Z] * mul + obj->cali_sw[BMI160_ACC_AXIS_Z],
		     tmp[BMI160_ACC_AXIS_X], tmp[BMI160_ACC_AXIS_Y],
		     tmp[BMI160_ACC_AXIS_Z]);

	return len;
}

static ssize_t store_cali_value(struct device_driver *ddri, const char *buf, size_t count)
{
	int err, x, y, z;
	int dat[BMI160_ACC_AXES_NUM] = { 0 };
	struct i2c_client *client = bmi160_acc_i2c_client;

	if (!strncmp(buf, "rst", 3)) {
		err = BMI160_ACC_ResetCalibration(client);
		if (err)
			GSE_ERR("reset offset err = %d\n", err);
	} else if (3 == sscanf(buf, "0x%02X 0x%02X 0x%02X", &x, &y, &z)) {
		dat[BMI160_ACC_AXIS_X] = x;
		dat[BMI160_ACC_AXIS_Y] = y;
		dat[BMI160_ACC_AXIS_Z] = z;
		err = BMI160_ACC_WriteCalibration(client, dat);
		if (err)
			GSE_ERR("write calibration err = %d\n", err);
	} else
		GSE_ERR("set calibration value by invalid format.\n");
	return count;
}

static ssize_t show_firlen_value(struct device_driver *ddri, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "not support\n");
}

static ssize_t store_firlen_value(struct device_driver *ddri, const char *buf, size_t count)
{
	return count;
}

static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
	int err;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	err = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));
	return err;
}

static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	int trace;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (1 == sscanf(buf, "0x%x", &trace))
		atomic_set(&obj->trace, trace);
	else
		GSE_ERR("invalid content: '%s'\n", buf);
	return count;
}

static ssize_t show_status_value(struct device_driver *ddri, char *buf)
{
	ssize_t len = 0;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (obj->hw) {
		len += snprintf(buf + len, PAGE_SIZE - len, "CUST: %d %d (%d %d)\n",
				obj->hw->i2c_num, obj->hw->direction, obj->hw->power_id,
				obj->hw->power_vol);
	} else
		len += snprintf(buf + len, PAGE_SIZE - len, "CUST: NULL\n");
	return len;
}

static ssize_t show_power_status_value(struct device_driver *ddri, char *buf)
{
	if (sensor_power)
		GSE_LOG("G sensor is in work mode, sensor_power = %d\n", sensor_power);
	else
		GSE_LOG("G sensor is in standby mode, sensor_power = %d\n", sensor_power);

	return 0;
}

static int bmi160_fifo_length(uint32_t *fifo_length)
{
	int comres = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;
	uint8_t a_data_u8r[2] = { 0, 0 };

	comres += bma_i2c_read_block(client, BMI160_USER_FIFO_BYTE_COUNTER_LSB__REG, a_data_u8r, 2);
	a_data_u8r[1] = BMI160_GET_BITSLICE(a_data_u8r[1], BMI160_USER_FIFO_BYTE_COUNTER_MSB);
	*fifo_length =
	    (uint32_t) (((uint32_t) ((uint8_t) (a_data_u8r[1]) << BMI160_SHIFT_8_POSITION)) |
			a_data_u8r[0]);

	return comres;
}

int bmi160_set_fifo_time_enable(u8 v_fifo_time_enable_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	u8 v_data_u8 = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	if (v_fifo_time_enable_u8 <= 1) {
		/* write the fifo sensor time */
		com_rslt = bma_i2c_read_block(client,
					      BMI160_USER_FIFO_TIME_ENABLE__REG, &v_data_u8, 1);
		if (com_rslt == 0) {
			v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
							BMI160_USER_FIFO_TIME_ENABLE,
							v_fifo_time_enable_u8);
			com_rslt += bma_i2c_write_block(client,
							BMI160_USER_FIFO_TIME_ENABLE__REG,
							&v_data_u8, 1);
		}
	} else {
		com_rslt = -2;
	}
	return com_rslt;
}

int bmi160_set_fifo_header_enable(u8 v_fifo_header_enable_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	u8 v_data_u8 = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	if (v_fifo_header_enable_u8 <= 1) {
		/* read the fifo sensor header enable */
		com_rslt = bma_i2c_read_block(client,
					      BMI160_USER_FIFO_HEADER_EN__REG, &v_data_u8, 1);
		if (com_rslt == 0) {
			v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
							BMI160_USER_FIFO_HEADER_EN,
							v_fifo_header_enable_u8);
			com_rslt += bma_i2c_write_block(client,
							BMI160_USER_FIFO_HEADER_EN__REG, &v_data_u8,
							1);
		}
	} else {
		com_rslt = -2;
	}
	return com_rslt;
}

int bmi160_get_fifo_time_enable(u8 *v_fifo_time_enable_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	u8 v_data_u8 = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	com_rslt = bma_i2c_read_block(client, BMI160_USER_FIFO_TIME_ENABLE__REG, &v_data_u8, 1);
	*v_fifo_time_enable_u8 = BMI160_GET_BITSLICE(v_data_u8, BMI160_USER_FIFO_TIME_ENABLE);
	return com_rslt;
}

static int bmi160_get_fifo_header_enable(u8 *v_fifo_header_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	u8 v_data_u8 = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	com_rslt = bma_i2c_read_block(client, BMI160_USER_FIFO_HEADER_EN__REG, &v_data_u8, 1);
	*v_fifo_header_u8 = BMI160_GET_BITSLICE(v_data_u8, BMI160_USER_FIFO_HEADER_EN);
	return com_rslt;
}

/*!
 *	@brief This API is used to read
 *	interrupt enable step detector interrupt from
 *	the register bit 0x52 bit 3
 *
 *	@param v_step_intr_u8 : The value of step detector interrupt enable
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 */
static BMI160_RETURN_FUNCTION_TYPE bmi160_get_std_enable(u8 *v_step_intr_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		/* read the step detector interrupt */
		com_rslt = p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							  BMI160_USER_INTR_ENABLE_2_STEP_DETECTOR_ENABLE__REG,
							  &v_data_u8,
							  BMI160_GEN_READ_WRITE_DATA_LENGTH);
		*v_step_intr_u8 =
		    BMI160_GET_BITSLICE(v_data_u8, BMI160_USER_INTR_ENABLE_2_STEP_DETECTOR_ENABLE);
	}
	return com_rslt;
}

BMI160_RETURN_FUNCTION_TYPE bmi160_write_reg(u8 v_addr_u8, u8 *v_data_u8, u8 v_len_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		/* write data from register */
		com_rslt =
		    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
						    v_addr_u8, v_data_u8, v_len_u8);
	}
	return com_rslt;
}

BMI160_RETURN_FUNCTION_TYPE bmi160_init(struct bmi160_t *bmi160)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	u8 v_pmu_data_u8 = BMI160_INIT_VALUE;
	/* assign bmi160 ptr */
	p_bmi160 = bmi160;
	com_rslt =
	    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
					   BMI160_USER_CHIP_ID__REG,
					   &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
	/* read Chip Id */
	p_bmi160->chip_id = v_data_u8;
	/* To avoid gyro wakeup it is required to write 0x00 to 0x6C */
	com_rslt += bmi160_write_reg(BMI160_USER_PMU_TRIGGER_ADDR,
				     &v_pmu_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
	return com_rslt;
}

/*!
 *	@brief  Configure trigger condition of interrupt1
 *	and interrupt2 pin from the register 0x53
 *	@brief interrupt1 - bit 0
 *	@brief interrupt2 - bit 4
 *
 *  @param v_channel_u8: The value of edge trigger selection
 *   v_channel_u8  |   Edge trigger
 *  ---------------|---------------
 *       0         | BMI160_INTR1_EDGE_CTRL
 *       1         | BMI160_INTR2_EDGE_CTRL
 *
 *	@param v_intr_edge_ctrl_u8 : The value of edge trigger enable
 *	value    | interrupt enable
 * ----------|-------------------
 *  0x01     |  BMI160_EDGE
 *  0x00     |  BMI160_LEVEL
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
*/
BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_edge_ctrl(u8 v_channel_u8, u8 v_intr_edge_ctrl_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		switch (v_channel_u8) {
		case BMI160_INTR1_EDGE_CTRL:
			/* write the edge trigger interrupt1 */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR1_EDGE_CTRL__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR1_EDGE_CTRL,
								v_intr_edge_ctrl_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR1_EDGE_CTRL__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_INTR2_EDGE_CTRL:
			/* write the edge trigger interrupt2 */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR2_EDGE_CTRL__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR2_EDGE_CTRL,
								v_intr_edge_ctrl_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR2_EDGE_CTRL__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		default:
			com_rslt = E_BMI160_OUT_OF_RANGE;
			break;
		}
	}
	return com_rslt;
}

/*
static void bmi_fifo_watermark_interrupt_handle(struct bmi160_acc_i2c_data *client_data)
{
	return;
}
*/

static int bmi160_set_command_register(u8 cmd_reg)
{
	int comres = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	comres += bma_i2c_write_block(client, BMI160_CMD_COMMANDS__REG, &cmd_reg, 1);
	return comres;
}

static ssize_t bmi160_fifo_bytecount_show(struct device_driver *ddri, char *buf)
{
	int comres = 0;
	uint32_t fifo_bytecount = 0;
	uint8_t a_data_u8r[2] = { 0, 0 };
	struct i2c_client *client = bmi160_acc_i2c_client;

	comres += bma_i2c_read_block(client, BMI160_USER_FIFO_BYTE_COUNTER_LSB__REG, a_data_u8r, 2);
	a_data_u8r[1] = BMI160_GET_BITSLICE(a_data_u8r[1], BMI160_USER_FIFO_BYTE_COUNTER_MSB);
	fifo_bytecount =
	    (uint32_t) (((uint32_t) ((uint8_t) (a_data_u8r[1]) << BMI160_SHIFT_8_POSITION)) |
			a_data_u8r[0]);
	comres = sprintf(buf, "%u\n", fifo_bytecount);
	return comres;
}

static ssize_t bmi160_fifo_bytecount_store(struct device_driver *ddri, const char *buf,
					   size_t count)
{
	int err;
	unsigned long data;
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	client_data->fifo_bytecount = (u16) data;
	return count;
}

static int bmi160_fifo_data_sel_get(struct bmi160_acc_i2c_data *client_data)
{
	int err = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;
	unsigned char data;
	unsigned char fifo_acc_en, fifo_gyro_en, fifo_mag_en;
	unsigned char fifo_datasel;


	err = bma_i2c_read_block(client, BMI160_USER_FIFO_ACC_EN__REG, &data, 1);
	fifo_acc_en = BMI160_GET_BITSLICE(data, BMI160_USER_FIFO_ACC_EN);

	err += bma_i2c_read_block(client, BMI160_USER_FIFO_GYRO_EN__REG, &data, 1);
	fifo_gyro_en = BMI160_GET_BITSLICE(data, BMI160_USER_FIFO_GYRO_EN);

	err += bma_i2c_read_block(client, BMI160_USER_FIFO_MAG_EN__REG, &data, 1);
	fifo_mag_en = BMI160_GET_BITSLICE(data, BMI160_USER_FIFO_MAG_EN);

	if (err)
		return err;

	fifo_datasel = (fifo_acc_en << BMI_ACC_SENSOR) |
	    (fifo_gyro_en << BMI_GYRO_SENSOR) | (fifo_mag_en << BMI_MAG_SENSOR);

	client_data->fifo_data_sel = fifo_datasel;

	return err;
}

static ssize_t bmi160_fifo_data_sel_show(struct device_driver *ddri, char *buf)
{
	int err = 0;
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	err = bmi160_fifo_data_sel_get(client_data);
	if (err)
		return -EINVAL;
	return sprintf(buf, "%d\n", client_data->fifo_data_sel);
}

static ssize_t bmi160_fifo_data_sel_store(struct device_driver *ddri, const char *buf, size_t count)
{
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;
	struct i2c_client *client = bmi160_acc_i2c_client;
	int err;
	unsigned long data;
	unsigned char fifo_datasel;
	unsigned char fifo_acc_en, fifo_gyro_en, fifo_mag_en;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	/* data format: aimed 0b0000 0x(m)x(g)x(a), x:1 enable, 0:disable */
	if (data > 7)
		return -EINVAL;

	fifo_datasel = (unsigned char)data;
	fifo_acc_en = fifo_datasel & (1 << BMI_ACC_SENSOR) ? 1 : 0;
	fifo_gyro_en = fifo_datasel & (1 << BMI_GYRO_SENSOR) ? 1 : 0;
	fifo_mag_en = fifo_datasel & (1 << BMI_MAG_SENSOR) ? 1 : 0;

	err += bma_i2c_read_block(client, BMI160_USER_FIFO_ACC_EN__REG, &fifo_datasel, 1);
	fifo_datasel = BMI160_SET_BITSLICE(fifo_datasel, BMI160_USER_FIFO_ACC_EN, fifo_acc_en);
	err += bma_i2c_write_block(client, BMI160_USER_FIFO_ACC_EN__REG, &fifo_datasel, 1);
	udelay(500);
	err += bma_i2c_read_block(client, BMI160_USER_FIFO_GYRO_EN__REG, &fifo_datasel, 1);
	fifo_datasel = BMI160_SET_BITSLICE(fifo_datasel, BMI160_USER_FIFO_GYRO_EN, fifo_gyro_en);
	err += bma_i2c_write_block(client, BMI160_USER_FIFO_GYRO_EN__REG, &fifo_datasel, 1);
	udelay(500);
	err += bma_i2c_read_block(client, BMI160_USER_FIFO_MAG_EN__REG, &fifo_datasel, 1);
	fifo_datasel = BMI160_SET_BITSLICE(fifo_datasel, BMI160_USER_FIFO_MAG_EN, fifo_mag_en);
	err += bma_i2c_write_block(client, BMI160_USER_FIFO_MAG_EN__REG, &fifo_datasel, 1);
	udelay(500);
	if (err)
		return -EIO;

	client_data->fifo_data_sel = (u8) data;
	GSE_LOG("FIFO fifo_data_sel %d, A_en:%d, G_en:%d, M_en:%d\n",
		client_data->fifo_data_sel, fifo_acc_en, fifo_gyro_en, fifo_mag_en);
	return count;
}

static ssize_t bmi160_fifo_data_out_frame_show(struct device_driver *ddri, char *buf)
{
	int err = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;
	uint32_t fifo_bytecount = 0;

	static char tmp_buf[1003] = { 0 };
	char *pBuf = NULL;

	pBuf = &tmp_buf[0];
	err = bmi160_fifo_length(&fifo_bytecount);
	if (err < 0) {
		GSE_ERR("read fifo length error.\n");
		return -EINVAL;
	}
	if (0 == fifo_bytecount)
		return 0;
	client_data->fifo_bytecount = fifo_bytecount;
#ifdef FIFO_READ_USE_DMA_MODE_I2C
	err = i2c_dma_read(client, BMI160_USER_FIFO_DATA__REG, buf, client_data->fifo_bytecount);
#else
	err = bma_i2c_read_block(client, BMI160_USER_FIFO_DATA__REG, buf, fifo_bytecount);
#endif
	if (err < 0) {
		GSE_ERR("read fifo data error.\n");
		return sprintf(buf, "Read byte block error.");
	}

	return client_data->fifo_bytecount;
}

static ssize_t show_layout_value(struct device_driver *ddri, char *buf)
{
	struct bmi160_acc_i2c_data *data = obj_i2c_data;

	return sprintf(buf, "(%d, %d)\n[%+2d %+2d %+2d]\n[%+2d %+2d %+2d]\n",
		       data->hw->direction, atomic_read(&data->layout), data->cvt.sign[0],
		       data->cvt.sign[1], data->cvt.sign[2], data->cvt.map[0], data->cvt.map[1],
		       data->cvt.map[2]);
}

static ssize_t store_layout_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct bmi160_acc_i2c_data *data = obj_i2c_data;
	int layout = 0;
	int rc;

	rc = kstrtoint(buf, 0, &layout);

	if (rc == 0) {
		atomic_set(&data->layout, layout);
		if (!hwmsen_get_convert(layout, &data->cvt)) {
			GSE_ERR("HWMSEN_GET_CONVERT function error!\r\n");
		} else if (!hwmsen_get_convert(data->hw->direction, &data->cvt)) {
			GSE_ERR("invalid layout: %d, restore to %d\n", layout, data->hw->direction);
		} else {
			GSE_ERR("invalid layout: (%d, %d)\n", layout, data->hw->direction);
			hwmsen_get_convert(0, &data->cvt);
		}
	} else {
		GSE_ERR("invalid format = '%s'\n", buf);
	}

	return count;
}

static void bmi_dump_reg(struct bmi160_acc_i2c_data *client_data)
{
	int i;
	u8 dbg_buf0[REG_MAX0];
	u8 dbg_buf1[REG_MAX1];
	u8 dbg_buf_str0[REG_MAX0 * 3 + 1] = "";
	u8 dbg_buf_str1[REG_MAX1 * 3 + 1] = "";
	struct i2c_client *client = bmi160_acc_i2c_client;

	GSE_LOG("\nFrom 0x00:\n");
	bma_i2c_read_block(client, BMI160_USER_CHIP_ID__REG, dbg_buf0, REG_MAX0);
	for (i = 0; i < REG_MAX0; i++) {
		sprintf(dbg_buf_str0 + i * 3, "%02x%c", dbg_buf0[i],
			(((i + 1) % BYTES_PER_LINE == 0) ? '\n' : ' '));
	}
	GSE_LOG("%s\n", dbg_buf_str0);

	bma_i2c_read_block(client, BMI160_USER_ACCEL_CONFIG_ADDR, dbg_buf1, REG_MAX1);
	GSE_LOG("\nFrom 0x40:\n");
	for (i = 0; i < REG_MAX1; i++) {
		sprintf(dbg_buf_str1 + i * 3, "%02x%c", dbg_buf1[i],
			(((i + 1) % BYTES_PER_LINE == 0) ? '\n' : ' '));
	}
	GSE_LOG("\n%s\n", dbg_buf_str1);
}

static ssize_t bmi_register_show(struct device_driver *ddri, char *buf)
{
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	bmi_dump_reg(client_data);
	return sprintf(buf, "Dump OK\n");
}

static ssize_t bmi_register_store(struct device_driver *ddri, const char *buf, size_t count)
{
	int err;
	int reg_addr = 0;
	int data;
	u8 write_reg_add = 0;
	u8 write_data = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	err = sscanf(buf, "%3d %3d", &reg_addr, &data);
	if (err < 2)
		return err;

	if (data > 0xff)
		return -EINVAL;

	write_reg_add = (u8) reg_addr;
	write_data = (u8) data;
	err += bma_i2c_write_block(client, write_reg_add, &write_data, 1);

	if (!err) {
		GSE_ERR("write reg 0x%2x, value= 0x%2x\n", reg_addr, data);
	} else {
		GSE_ERR("write reg fail\n");
		return err;
	}
	return count;
}

static ssize_t bmi160_bmi_value_show(struct device_driver *ddri, char *buf)
{
	int err;
	u8 raw_data[12] = { 0 };
	struct i2c_client *client = bmi160_acc_i2c_client;

	memset(raw_data, 0, sizeof(raw_data));
	err = bma_i2c_read_block(client, BMI160_USER_DATA_8_GYRO_X_LSB__REG, raw_data, 12);
	if (err)
		return err;
	/* output:gyro x y z acc x y z */
	return sprintf(buf, "%hd %d %hd %hd %hd %hd\n",
		       (s16) (raw_data[1] << 8 | raw_data[0]),
		       (s16) (raw_data[3] << 8 | raw_data[2]),
		       (s16) (raw_data[5] << 8 | raw_data[4]),
		       (s16) (raw_data[7] << 8 | raw_data[6]),
		       (s16) (raw_data[9] << 8 | raw_data[8]),
		       (s16) (raw_data[11] << 8 | raw_data[10]));
}

static int bmi160_get_fifo_wm(u8 *v_fifo_wm_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	u8 v_data_u8 = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;
	/* check the p_bmi160 structure as NULL */
	/* read the fifo water mark level */
	com_rslt = bma_i2c_read_block(client, BMI160_USER_FIFO_WM__REG, &v_data_u8, 1);
	*v_fifo_wm_u8 = BMI160_GET_BITSLICE(v_data_u8, BMI160_USER_FIFO_WM);
	return com_rslt;
}

static int bmi160_set_fifo_wm(u8 v_fifo_wm_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	struct i2c_client *client = bmi160_acc_i2c_client;
	/* write the fifo water mark level */
	com_rslt = bma_i2c_write_block(client, BMI160_USER_FIFO_WM__REG, &v_fifo_wm_u8, 1);
	return com_rslt;
}

static ssize_t bmi160_fifo_watermark_show(struct device_driver *ddri, char *buf)
{
	int err;
	u8 data = 0xff;

	err = bmi160_get_fifo_wm(&data);
	if (err)
		return err;
	return sprintf(buf, "%d\n", data);
}

static ssize_t bmi160_fifo_watermark_store(struct device_driver *ddri,
					   const char *buf, size_t count)
{
	int err;
	unsigned long data;
	u8 fifo_watermark;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;

	fifo_watermark = (u8) data;
	err = bmi160_set_fifo_wm(fifo_watermark);
	if (err)
		return -EIO;

	GSE_LOG("set fifo watermark = %d ok.", (int)fifo_watermark);
	return count;
}

static ssize_t bmi160_delay_show(struct device_driver *ddri, char *buf)
{
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	return sprintf(buf, "%d\n", atomic_read(&client_data->delay));
}

static ssize_t bmi160_delay_store(struct device_driver *ddri, const char *buf, size_t count)
{
	int err;
	unsigned long data;
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	if (data < BMI_DELAY_MIN)
		data = BMI_DELAY_MIN;

	atomic_set(&client_data->delay, (unsigned int)data);
	return count;
}

static ssize_t bmi160_fifo_flush_store(struct device_driver *ddri, const char *buf, size_t count)
{
	int err;
	unsigned long enable;

	err = kstrtoul(buf, 10, &enable);
	if (err)
		return err;
	if (enable)
		err = bmi160_set_command_register(CMD_CLR_FIFO_DATA);
	if (err)
		GSE_ERR("fifo flush failed!\n");
	return count;
}

static ssize_t bmi160_fifo_header_en_show(struct device_driver *ddri, char *buf)
{
	int err;
	u8 data = 0xff;

	err = bmi160_get_fifo_header_enable(&data);
	if (err)
		return err;
	return sprintf(buf, "%d\n", data);
}

static ssize_t bmi160_fifo_header_en_store(struct device_driver *ddri,
					   const char *buf, size_t count)
{
	int err;
	unsigned long data;
	u8 fifo_header_en;
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	err = kstrtoul(buf, 10, &data);
	if (err)
		return err;
	if (data > 1)
		return -ENOENT;
	fifo_header_en = (u8) data;
	err = bmi160_set_fifo_header_enable(fifo_header_en);
	if (err)
		return -EIO;
	client_data->fifo_head_en = fifo_header_en;
	return count;
}

static DRIVER_ATTR(chipinfo, S_IWUSR | S_IRUGO, show_chipinfo_value, NULL);
static DRIVER_ATTR(cpsdata, S_IWUSR | S_IRUGO, show_cpsdata_value, NULL);
static DRIVER_ATTR(acc_op_mode, S_IWUSR | S_IRUGO, show_acc_op_mode_value, store_acc_op_mode_value);
static DRIVER_ATTR(acc_range, S_IWUSR | S_IRUGO, show_acc_range_value, store_acc_range_value);
static DRIVER_ATTR(acc_odr, S_IWUSR | S_IRUGO, show_acc_odr_value, store_acc_odr_value);
static DRIVER_ATTR(sensordata, S_IWUSR | S_IRUGO, show_sensordata_value, NULL);
static DRIVER_ATTR(cali, S_IWUSR | S_IRUGO, show_cali_value, store_cali_value);
static DRIVER_ATTR(firlen, S_IWUSR | S_IRUGO, show_firlen_value, store_firlen_value);
static DRIVER_ATTR(trace, S_IWUSR | S_IRUGO, show_trace_value, store_trace_value);
static DRIVER_ATTR(status, S_IRUGO, show_status_value, NULL);
static DRIVER_ATTR(powerstatus, S_IRUGO, show_power_status_value, NULL);

static DRIVER_ATTR(fifo_bytecount, S_IRUGO | S_IWUSR, bmi160_fifo_bytecount_show,
		   bmi160_fifo_bytecount_store);
static DRIVER_ATTR(fifo_data_sel, S_IRUGO | S_IWUSR, bmi160_fifo_data_sel_show,
		   bmi160_fifo_data_sel_store);
static DRIVER_ATTR(fifo_data_frame, S_IRUGO, bmi160_fifo_data_out_frame_show, NULL);
static DRIVER_ATTR(layout, S_IRUGO | S_IWUSR, show_layout_value, store_layout_value);
static DRIVER_ATTR(register, S_IRUGO | S_IWUSR, bmi_register_show, bmi_register_store);
static DRIVER_ATTR(bmi_value, S_IRUGO, bmi160_bmi_value_show, NULL);
static DRIVER_ATTR(fifo_watermark, S_IRUGO | S_IWUSR, bmi160_fifo_watermark_show,
		   bmi160_fifo_watermark_store);
static DRIVER_ATTR(delay, S_IRUGO | S_IWUSR, bmi160_delay_show, bmi160_delay_store);
static DRIVER_ATTR(fifo_flush, S_IWUSR | S_IRUGO, NULL, bmi160_fifo_flush_store);
static DRIVER_ATTR(fifo_header_en, S_IWUSR | S_IRUGO, bmi160_fifo_header_en_show,
		   bmi160_fifo_header_en_store);

static struct driver_attribute *bmi160_acc_attr_list[] = {
	&driver_attr_chipinfo,	/*chip information */
	&driver_attr_sensordata,	/*dump sensor data */
	&driver_attr_cali,	/*show calibration data */
	&driver_attr_firlen,	/*filter length: 0: disable, others: enable */
	&driver_attr_trace,	/*trace log */
	&driver_attr_status,
	&driver_attr_powerstatus,
	&driver_attr_cpsdata,	/*g sensor data for compass tilt compensation */
	&driver_attr_acc_op_mode,	/*g sensor opmode for compass tilt compensation */
	&driver_attr_acc_range,	/*g sensor range for compass tilt compensation */
	&driver_attr_acc_odr,	/*g sensor bandwidth for compass tilt compensation */

	&driver_attr_fifo_bytecount,
	&driver_attr_fifo_data_sel,
	&driver_attr_fifo_data_frame,
	&driver_attr_layout,
	&driver_attr_register,
	&driver_attr_bmi_value,
	&driver_attr_fifo_watermark,
	&driver_attr_delay,
	&driver_attr_fifo_flush,
	&driver_attr_fifo_header_en,
};


static int bmi160_acc_create_attr(struct device_driver *driver)
{
	int err = 0;
	int idx = 0;
	int num = (int)(sizeof(bmi160_acc_attr_list) / sizeof(bmi160_acc_attr_list[0]));

	for (idx = 0; idx < num; idx++) {
		err = driver_create_file(driver, bmi160_acc_attr_list[idx]);
		if (err) {
			GSE_ERR("create driver file (%s) failed.\n",
				bmi160_acc_attr_list[idx]->attr.name);
			break;
		}
	}
	return err;
}

static int bmi160_acc_delete_attr(struct device_driver *driver)
{
	int idx = 0;
	int err = 0;
	int num = (int)(sizeof(bmi160_acc_attr_list) / sizeof(bmi160_acc_attr_list[0]));

	for (idx = 0; idx < num; idx++)
		driver_remove_file(driver, bmi160_acc_attr_list[idx]);
	return err;
}

static int bmi160_acc_open(struct inode *inode, struct file *file)
{
	file->private_data = bmi160_acc_i2c_client;
	if (file->private_data == NULL) {
		GSE_ERR("file->private_data is null pointer.\n");
		return -EINVAL;
	}
	return nonseekable_open(inode, file);
}

static int bmi160_acc_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static long bmi160_acc_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	char strbuf[BMI160_BUFSIZE] = { 0 };
	void __user *data;
	struct SENSOR_DATA sensor_data;
	int err = 0;
	int cali[3] = { 0 };
	struct i2c_client *client = (struct i2c_client *)file->private_data;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (obj == NULL)
		return -EFAULT;
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (err) {
		GSE_ERR("access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
		return -EFAULT;
	}

	GSE_LOG("bmi160_acc_unlocked_ioctl, cmd = 0x%x\n", cmd);

	switch (cmd) {
	case GSENSOR_IOCTL_INIT:
		bmi160_acc_init_client(client, 0);
		break;
	case GSENSOR_IOCTL_READ_CHIPINFO:
		data = (void __user *)arg;
		if (data == NULL) {
			err = -EINVAL;
			break;
		}
		BMI160_ACC_ReadChipInfo(client, strbuf, BMI160_BUFSIZE);
		if (copy_to_user(data, strbuf, strlen(strbuf) + 1))
			err = -EFAULT;
		break;
	case GSENSOR_IOCTL_READ_SENSORDATA:
		data = (void __user *)arg;
		if (data == NULL) {
			err = -EINVAL;
			break;
		}
		BMI160_ACC_ReadSensorData(client, strbuf, BMI160_BUFSIZE);
		if (copy_to_user(data, strbuf, strlen(strbuf) + 1))
			err = -EFAULT;
		break;
	case GSENSOR_IOCTL_READ_GAIN:
		data = (void __user *)arg;
		if (data == NULL) {
			err = -EINVAL;
			break;
		}
		if (copy_to_user(data, &gsensor_gain, sizeof(struct GSENSOR_VECTOR3D)))
			err = -EFAULT;
		break;
	case GSENSOR_IOCTL_READ_RAW_DATA:
		data = (void __user *)arg;
		if (data == NULL) {
			err = -EINVAL;
			break;
		}
		BMI160_ACC_ReadRawData(client, strbuf);
		if (copy_to_user(data, &strbuf, strlen(strbuf) + 1))
			err = -EFAULT;
		break;
	case GSENSOR_IOCTL_SET_CALI:
		data = (void __user *)arg;
		if (data == NULL) {
			err = -EINVAL;
			break;
		}
		if (copy_from_user(&sensor_data, data, sizeof(sensor_data))) {
			err = -EFAULT;
			break;
		}
		if (atomic_read(&obj->suspend)) {
			GSE_ERR("can't perform calibration in suspend state.\n");
			err = -EINVAL;
		} else {
			cali[BMI160_ACC_AXIS_X] = sensor_data.x
			    * obj->reso->sensitivity / GRAVITY_EARTH_1000;
			cali[BMI160_ACC_AXIS_Y] = sensor_data.y
			    * obj->reso->sensitivity / GRAVITY_EARTH_1000;
			cali[BMI160_ACC_AXIS_Z] = sensor_data.z
			    * obj->reso->sensitivity / GRAVITY_EARTH_1000;
			err = BMI160_ACC_WriteCalibration(client, cali);
		}
		break;
	case GSENSOR_IOCTL_CLR_CALI:
		err = BMI160_ACC_ResetCalibration(client);
		break;
	case GSENSOR_IOCTL_GET_CALI:
		data = (void __user *)arg;
		if (data == NULL) {
			err = -EINVAL;
			break;
		}
		err = BMI160_ACC_ReadCalibration(client, cali);
		if (err) {
			GSE_ERR("read calibration failed.\n");
			break;
		}
		sensor_data.x = cali[BMI160_ACC_AXIS_X]
		    * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		sensor_data.y = cali[BMI160_ACC_AXIS_Y]
		    * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		sensor_data.z = cali[BMI160_ACC_AXIS_Z]
		    * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		if (copy_to_user(data, &sensor_data, sizeof(sensor_data)))
			err = -EFAULT;
		break;
	default:
		GSE_ERR("unknown IOCTL: 0x%08x\n", cmd);
		err = -ENOIOCTLCMD;
		break;
	}
	return err;
}

#ifdef CONFIG_COMPAT
static long bmi160_acc_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err = 0;
	void __user *arg32 = compat_ptr(arg);

	if (!file->f_op || !file->f_op->unlocked_ioctl) {
		GSE_ERR("error cmd: %d\n", cmd);
		return -ENOTTY;
	}

	/* GSE_LOG("bmi160_acc_compat_ioctl, cmd = 0x%x\n", cmd); */

	switch (cmd) {
	case COMPAT_GSENSOR_IOCTL_INIT:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err = file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_INIT, (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_INIT unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_READ_CHIPINFO:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_READ_CHIPINFO,
					       (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_READ_CHIPINFO unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_READ_SENSORDATA:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_READ_SENSORDATA,
					       (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_READ_SENSORDATA unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_READ_GAIN:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_READ_GAIN, (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_READ_GAIN unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_READ_RAW_DATA:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_READ_RAW_DATA,
					       (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_READ_RAW_DATA unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_SET_CALI:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}

		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_SET_CALI, (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_SET_CALI unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_CLR_CALI:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_CLR_CALI, (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_CLR_CALI unlocked_ioctl failed.");
			return err;
		}
		break;
	case COMPAT_GSENSOR_IOCTL_GET_CALI:
		if (arg32 == NULL) {
			err = -EINVAL;
			break;
		}
		err =
		    file->f_op->unlocked_ioctl(file, GSENSOR_IOCTL_GET_CALI, (unsigned long)arg32);
		if (err) {
			GSE_ERR("GSENSOR_IOCTL_GET_CALI unlocked_ioctl failed.");
			return err;
		}
		break;
	default:
		GSE_ERR("unknown IOCTL: 0x%08x\n", cmd);
		err = -ENOIOCTLCMD;
		break;
	}
	return err;
}
#endif

static const struct file_operations bmi160_acc_fops = {
	/* .owner = THIS_MODULE, */
	.open = bmi160_acc_open,
	.release = bmi160_acc_release,
	.unlocked_ioctl = bmi160_acc_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = bmi160_acc_compat_ioctl,
#endif
};

static struct miscdevice bmi160_acc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gsensor",
	.fops = &bmi160_acc_fops,
};

#ifndef CONFIG_HAS_EARLYSUSPEND

static int bmi160_acc_suspend(struct i2c_client *client, pm_message_t msg)
{
	int err = 0;
	u8 op_mode = 0;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	if (atomic_read(&obj->wkqueue_en) == 1) {
		BMI160_ACC_SetPowerMode(obj->client, false);
		cancel_delayed_work_sync(&obj->work);
	}
	err = bmi160_acc_get_mode(client, &op_mode);
	if (msg.event == PM_EVENT_SUSPEND) {
		atomic_set(&obj->suspend, 1);
		if (op_mode != BMI160_ACC_MODE_SUSPEND && sig_flag != 1)
			err = BMI160_ACC_SetPowerMode(obj->client, false);
		if (err) {
			GSE_ERR("write power control failed.\n");
			return err;
		}
		BMI160_ACC_power(obj->hw, 0);
	}
	return err;
}

static int bmi160_acc_resume(struct i2c_client *client)
{
	int err;
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;

	BMI160_ACC_power(obj->hw, 1);
	err = bmi160_acc_init_client(client, 0);
	if (err) {
		GSE_ERR("initialize client fail!!\n");
		return err;
	}
	atomic_set(&obj->suspend, 0);
	return 0;
}

#else				/*CONFIG_HAS_EARLY_SUSPEND is defined */

static void bmi160_acc_early_suspend(struct early_suspend *h)
{
	struct bmi160_acc_i2c_data *obj = container_of(h, struct bmi160_acc_i2c_data, early_drv);
	int err;

	atomic_set(&obj->suspend, 1);
	err = BMI160_ACC_SetPowerMode(obj->client, false);
	if (err) {
		GSE_ERR("write power control fail!!\n");
		return;
	}
	BMI160_ACC_power(obj->hw, 0);
}

static void bmi160_acc_late_resume(struct early_suspend *h)
{
	struct bmi160_acc_i2c_data *obj = container_of(h, struct bmi160_acc_i2c_data, early_drv);
	int err;

	BMI160_ACC_power(obj->hw, 1);
	err = bmi160_acc_init_client(obj->client, 0);
	if (err) {
		GSE_ERR("initialize client fail!!\n");
		return;
	}
	atomic_set(&obj->suspend, 0);
}

#endif				/*CONFIG_HAS_EARLYSUSPEND */

#ifdef CONFIG_OF
static const struct of_device_id gse_int_of_match[] = {
	{.compatible = "mediatek, gse_1-eint",},
	{},
};
#endif

#ifdef CONFIG_OF
static const struct of_device_id accel_of_match[] = {
	{.compatible = "mediatek,gsensor"},
	{},
};
#endif

static struct i2c_driver bmi160_acc_i2c_driver = {
	.driver = {
		   .name = BMI160_DEV_NAME,
#ifdef CONFIG_OF
		   .of_match_table = accel_of_match,
#endif
		   },
	.probe = bmi160_acc_i2c_probe,
	.remove = bmi160_acc_i2c_remove,
#if !defined(CONFIG_HAS_EARLYSUSPEND)
	.suspend = bmi160_acc_suspend,
	.resume = bmi160_acc_resume,
#endif
	.id_table = bmi160_acc_i2c_id,
};

/*!
 * @brief if use this type of enable,
 * Gsensor should report inputEvent(x, y, z, status, div) to HAL
 *
 * @param[in] int open true or false
 *
 * @return zero success, non-zero failed
 */
static int bmi160_acc_open_report_data(int open)
{
	/* should queue work to report event if is_report_input_direct=true
	   struct bmi160_acc_i2c_data *obj = obj_i2c_data;
	   if(true == obj->is_input_enable){
	   if(0 == open) {
	   atomic_set(&obj->wkqueue_en, 0);
	   cancel_delayed_work_sync(&obj->work);
	   }else {
	   atomic_set(&obj->wkqueue_en, 1);
	   schedule_delayed_work(&obj->work,
	   msecs_to_jiffies(atomic_read(&obj->delay)));
	   }
	   }
	 */
	return 0;
}

/*!
 * @brief If use this type of enable, Gsensor only enabled but not report inputEvent to HAL
 *
 * @param[in] int enable true or false
 *
 * @return zero success, non-zero failed
 */
static int bmi160_acc_enable_nodata(int en)
{
	int err = 0;
	bool power = false;

	if (1 == en)
		power = true;
	else
		power = false;
	err = BMI160_ACC_SetPowerMode(obj_i2c_data->client, power);
	if (err < 0) {
		GSE_ERR("BMI160_ACC_SetPowerMode failed.\n");
		return -1;
	}
	GSE_LOG("bmi160_acc_enable_nodata ok!\n");
	return err;
}

/*!
 * @brief set the delay value for acc
 *
 * @param[in] u64 ns for dealy
 *
 * @return zero success, non-zero failed
 */
static int bmi160_acc_set_delay(u64 ns)
{
	int value = 0;
	int sample_delay = 0;
	int err = 0;

	value = (int)ns / 1000 / 1000;
	if (value <= 5)
		sample_delay = BMI160_ACCEL_ODR_400HZ;
	else if (value <= 10)
		sample_delay = BMI160_ACCEL_ODR_200HZ;
	else
		sample_delay = BMI160_ACCEL_ODR_100HZ;
	err = BMI160_ACC_SetBWRate(obj_i2c_data->client, sample_delay);
	if (err < 0) {
		GSE_ERR("set delay parameter error!\n");
		return -1;
	}
	GSE_LOG("bmi160 acc set delay = (%d) ok.\n", value);
	return 0;
}

/*!
 * @brief get the raw data for gsensor
 *
 * @param[in] int x axis value
 * @param[in] int y axis value
 * @param[in] int z axis value
 * @param[in] int status
 *
 * @return zero success, non-zero failed
 */
static int bmi160_acc_get_data(int *x, int *y, int *z, int *status)
{
	int err = 0;
	char buff[BMI160_BUFSIZE];

	err = BMI160_ACC_ReadSensorData(obj_i2c_data->client, buff, BMI160_BUFSIZE);
	if (err < 0) {
		GSE_ERR("bmi160_acc_get_data failed.\n");
		return err;
	}
	err = sscanf(buff, "%x %x %x", x, y, z);
	if (err != -1)
		*status = SENSOR_STATUS_ACCURACY_MEDIUM;
	return 0;
}

int bmi160_set_intr_fifo_wm(u8 v_channel_u8, u8 v_intr_fifo_wm_u8)
{
	/* variable used for return the status of communication result */
	int com_rslt = -1;
	u8 v_data_u8 = 0;
	struct i2c_client *client = bmi160_acc_i2c_client;

	switch (v_channel_u8) {
		/* write the fifo water mark interrupt */
	case BMI160_INTR1_MAP_FIFO_WM:
		com_rslt = bma_i2c_read_block(client,
					      BMI160_USER_INTR_MAP_1_INTR1_FIFO_WM__REG, &v_data_u8,
					      1);
		if (com_rslt == 0) {
			v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
							BMI160_USER_INTR_MAP_1_INTR1_FIFO_WM,
							v_intr_fifo_wm_u8);
			com_rslt += bma_i2c_write_block(client,
							BMI160_USER_INTR_MAP_1_INTR1_FIFO_WM__REG,
							&v_data_u8, 1);
		}
		break;
	case BMI160_INTR2_MAP_FIFO_WM:
		com_rslt = bma_i2c_read_block(client,
					      BMI160_USER_INTR_MAP_1_INTR2_FIFO_WM__REG, &v_data_u8,
					      1);
		if (com_rslt == 0) {
			v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
							BMI160_USER_INTR_MAP_1_INTR2_FIFO_WM,
							v_intr_fifo_wm_u8);
			com_rslt += bma_i2c_write_block(client,
							BMI160_USER_INTR_MAP_1_INTR2_FIFO_WM__REG,
							&v_data_u8, 1);
		}
		break;
	default:
		com_rslt = -2;
		break;
	}
	return com_rslt;
}

/*!
 *	@brief  API used for set the Configure level condition of interrupt1
 *	and interrupt2 pin form the register 0x53
 *	@brief interrupt1 - bit 1
 *	@brief interrupt2 - bit 5
 *
 *  @param v_channel_u8: The value of level condition selection
 *   v_channel_u8  |   level selection
 *  ---------------|---------------
 *       0         | BMI160_INTR1_LEVEL
 *       1         | BMI160_INTR2_LEVEL
 *
 *	@param v_intr_level_u8 : The value of level of interrupt enable
 *	value    | Behaviour
 * ----------|-------------------
 *  0x01     |  BMI160_LEVEL_HIGH
 *  0x00     |  BMI160_LEVEL_LOW
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
 *
*/
BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_level(u8 v_channel_u8, u8 v_intr_level_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		switch (v_channel_u8) {
		case BMI160_INTR1_LEVEL:
			/* write the interrupt1 level */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR1_LEVEL__REG, &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR1_LEVEL,
								v_intr_level_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR1_LEVEL__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_INTR2_LEVEL:
			/* write the interrupt2 level */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR2_LEVEL__REG, &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR2_LEVEL,
								v_intr_level_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR2_LEVEL__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		default:
			com_rslt = E_BMI160_OUT_OF_RANGE;
			break;
		}
	}
	return com_rslt;
}

/*!
*	@brief API used to set the Output enable for interrupt1
*	and interrupt1 pin from the register 0x53
*	@brief interrupt1 - bit 3
*	@brief interrupt2 - bit 7
*
*  @param v_channel_u8: The value of output enable selection
*   v_channel_u8  |   level selection
*  ---------------|---------------
*       0         | BMI160_INTR1_OUTPUT_TYPE
*       1         | BMI160_INTR2_OUTPUT_TYPE
*
*	@param v_output_enable_u8 :
*	The value of output enable of interrupt enable
*	value    | Behaviour
* ----------|-------------------
*  0x01     |  BMI160_INPUT
*  0x00     |  BMI160_OUTPUT
*
*	@return results of bus communication function
*	@retval 0 -> Success
*	@retval -1 -> Error
*
*/
BMI160_RETURN_FUNCTION_TYPE bmi160_set_output_enable(u8 v_channel_u8, u8 v_output_enable_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		switch (v_channel_u8) {
		case BMI160_INTR1_OUTPUT_ENABLE:
			/* write the output enable of interrupt1 */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR1_OUTPUT_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR1_OUTPUT_ENABLE,
								v_output_enable_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR1_OUTPUT_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_INTR2_OUTPUT_ENABLE:
			/* write the output enable of interrupt2 */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR2_OUTPUT_EN__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR2_OUTPUT_EN,
								v_output_enable_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR2_OUTPUT_EN__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		default:
			com_rslt = E_BMI160_OUT_OF_RANGE;
			break;
		}
	}
	return com_rslt;
}

 /*!
  *     @brief This API is used to set
  *     interrupt enable step detector interrupt from
  *     the register bit 0x52 bit 3
  *
  *     @param v_step_intr_u8 : The value of step detector interrupt enable
  *
  *     @return results of bus communication function
  *     @retval 0 -> Success
  *     @retval -1 -> Error
  *
  */
static BMI160_RETURN_FUNCTION_TYPE bmi160_set_std_enable(u8 v_step_intr_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		com_rslt = p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							  BMI160_USER_INTR_ENABLE_2_STEP_DETECTOR_ENABLE__REG,
							  &v_data_u8,
							  BMI160_GEN_READ_WRITE_DATA_LENGTH);
		if (com_rslt == SUCCESS) {
			v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
							BMI160_USER_INTR_ENABLE_2_STEP_DETECTOR_ENABLE,
							v_step_intr_u8);
			com_rslt +=
			    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
							    BMI160_USER_INTR_ENABLE_2_STEP_DETECTOR_ENABLE__REG,
							    &v_data_u8,
							    BMI160_GEN_READ_WRITE_DATA_LENGTH);
		}
	}
	return com_rslt;
}

/*!
 * @brief
 *	This API reads the data from
 *	the given register
 *
 *	@param v_addr_u8 -> Address of the register
 *	@param v_data_u8 -> The data from the register
 *	@param v_len_u8 -> no of bytes to read
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
 */
BMI160_RETURN_FUNCTION_TYPE bmi160_read_reg(u8 v_addr_u8, u8 *v_data_u8, u8 v_len_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		/* Read data from register */
		com_rslt =
		    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
						   v_addr_u8, v_data_u8, v_len_u8);
	}
	return com_rslt;
}

/*!
 *	@brief This API used to trigger the step detector
 *	interrupt
 *
 *  @param  v_step_detector_u8 : The value of interrupt selection
 *  value    |  interrupt
 * ----------|-----------
 *   0       |  BMI160_MAP_INTR1
 *   1       |  BMI160_MAP_INTR2
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
*/
static BMI160_RETURN_FUNCTION_TYPE bmi160_map_std_intr(u8 v_step_detector_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_step_det_u8 = BMI160_INIT_VALUE;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	u8 v_low_g_intr_u81_stat_u8 = BMI160_LOW_G_INTR_STAT;
	u8 v_low_g_intr_u82_stat_u8 = BMI160_LOW_G_INTR_STAT;
	u8 v_low_g_enable_u8 = BMI160_ENABLE_LOW_G;
	/* read the v_status_s8 of step detector interrupt */
	com_rslt = bmi160_get_std_enable(&v_step_det_u8);
	if (v_step_det_u8 != BMI160_STEP_DET_STAT_HIGH)
		com_rslt += bmi160_set_std_enable(BMI160_STEP_DETECT_INTR_ENABLE);
	switch (v_step_detector_u8) {
	case BMI160_MAP_INTR1:
		com_rslt += bmi160_read_reg(BMI160_USER_INTR_MAP_0_INTR1_LOW_G__REG,
					    &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_low_g_intr_u81_stat_u8;
		/* map the step detector interrupt
		   to Low-g interrupt 1 */
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_MAP_0_INTR1_LOW_G__REG,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		/* Enable the Low-g interrupt */
		com_rslt = bmi160_read_reg(BMI160_USER_INTR_ENABLE_1_LOW_G_ENABLE__REG,
					   &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_low_g_enable_u8;
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_ENABLE_1_LOW_G_ENABLE__REG,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);

		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		break;
	case BMI160_MAP_INTR2:
		/* map the step detector interrupt
		   to Low-g interrupt 1 */
		com_rslt += bmi160_read_reg(BMI160_USER_INTR_MAP_2_INTR2_LOW_G__REG,
					    &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_low_g_intr_u82_stat_u8;

		com_rslt += bmi160_write_reg(BMI160_USER_INTR_MAP_2_INTR2_LOW_G__REG,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		/* Enable the Low-g interrupt */
		com_rslt = bmi160_read_reg(BMI160_USER_INTR_ENABLE_1_LOW_G_ENABLE__REG,
					   &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_low_g_enable_u8;
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_ENABLE_1_LOW_G_ENABLE__REG,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		break;
	default:
		com_rslt = E_BMI160_OUT_OF_RANGE;
		break;
	}
	return com_rslt;
}

/*!
*	@brief This API is used to select
*	the significant or any motion interrupt from the register 0x62 bit 1
*
*  @param  v_intr_significant_motion_select_u8 :
*	the value of significant or any motion interrupt selection
*	value    | Behaviour
* ----------|-------------------
*  0x00     |  ANY_MOTION
*  0x01     |  SIGNIFICANT_MOTION
*
*	@return results of bus communication function
*	@retval 0 -> Success
*	@retval -1 -> Error
*
*/
BMI160_RETURN_FUNCTION_TYPE bmi160_get_intr_sm_select(u8 *v_intr_significant_motion_select_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		/* read the significant or any motion interrupt */
		com_rslt = p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							  BMI160_USER_INTR_SIGNIFICATION_MOTION_SELECT__REG,
							  &v_data_u8,
							  BMI160_GEN_READ_WRITE_DATA_LENGTH);
		*v_intr_significant_motion_select_u8 =
		    BMI160_GET_BITSLICE(v_data_u8, BMI160_USER_INTR_SIGNIFICATION_MOTION_SELECT);
	}
	return com_rslt;
}

/*!
 *	@brief This API is used to write threshold
 *	definition for the any-motion interrupt
 *	from the register 0x60 bit 0 to 7
 *
 *  @param  v_any_motion_thres_u8 : The value of any motion threshold
 *
 *	@note any motion threshold changes according to accel g range
 *	accel g range can be set by the function ""
 *   accel_range    | any motion threshold
 *  ----------------|---------------------
 *      2g          |  v_any_motion_thres_u8*3.91 mg
 *      4g          |  v_any_motion_thres_u8*7.81 mg
 *      8g          |  v_any_motion_thres_u8*15.63 mg
 *      16g         |  v_any_motion_thres_u8*31.25 mg
 *	@note when v_any_motion_thres_u8 = 0
 *   accel_range    | any motion threshold
 *  ----------------|---------------------
 *      2g          |  1.95 mg
 *      4g          |  3.91 mg
 *      8g          |  7.81 mg
 *      16g         |  15.63 mg
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
 *
*/
BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_any_motion_thres(u8 v_any_motion_thres_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		/* write any motion threshold */
		com_rslt = p_bmi160->BMI160_BUS_WRITE_FUNC
		    (p_bmi160->dev_addr,
		     BMI160_USER_INTR_MOTION_1_INTR_ANY_MOTION_THRES__REG,
		     &v_any_motion_thres_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
	}
	return com_rslt;
}

/*!
 *	@brief This API is used to write
 *	the significant skip time from the register 0x62 bit  2 and 3
 *
 *  @param  v_int_sig_mot_skip_u8 : the value of significant skip time
 *	value    | Behaviour
 * ----------|-------------------
 *  0x00     |  skip time 1.5 seconds
 *  0x01     |  skip time 3 seconds
 *  0x02     |  skip time 6 seconds
 *  0x03     |  skip time 12 seconds
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
*/
static BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_sm_skip(u8 v_int_sig_mot_skip_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		if (v_int_sig_mot_skip_u8 <= BMI160_MAX_UNDER_SIG_MOTION) {
			/* write significant skip time */
			com_rslt = p_bmi160->BMI160_BUS_READ_FUNC
			    (p_bmi160->dev_addr,
			     BMI160_USER_INTR_SIGNIFICANT_MOTION_SKIP__REG,
			     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_SIGNIFICANT_MOTION_SKIP,
								v_int_sig_mot_skip_u8);
				com_rslt += p_bmi160->BMI160_BUS_WRITE_FUNC
				    (p_bmi160->dev_addr,
				     BMI160_USER_INTR_SIGNIFICANT_MOTION_SKIP__REG,
				     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
		} else {
			com_rslt = E_BMI160_OUT_OF_RANGE;
		}
	}
	return com_rslt;
}

/*!
 *	@brief This API is used to write
 *	the significant proof time from the register 0x62 bit  4 and 5
 *
 *  @param  v_significant_motion_proof_u8 :
 *	the value of significant proof time
 *	value    | Behaviour
 * ----------|-------------------
 *  0x00     |  proof time 0.25 seconds
 *  0x01     |  proof time 0.5 seconds
 *  0x02     |  proof time 1 seconds
 *  0x03     |  proof time 2 seconds
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
*/
static BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_sm_proof(u8 v_significant_motion_proof_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		if (v_significant_motion_proof_u8 <= BMI160_MAX_UNDER_SIG_MOTION) {
			/* write significant proof time */
			com_rslt = p_bmi160->BMI160_BUS_READ_FUNC
			    (p_bmi160->dev_addr,
			     BMI160_USER_INTR_SIGNIFICANT_MOTION_PROOF__REG,
			     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_SIGNIFICANT_MOTION_PROOF,
								v_significant_motion_proof_u8);
				com_rslt += p_bmi160->BMI160_BUS_WRITE_FUNC
				    (p_bmi160->dev_addr,
				     BMI160_USER_INTR_SIGNIFICANT_MOTION_PROOF__REG,
				     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
		} else {
			com_rslt = E_BMI160_OUT_OF_RANGE;
		}
	}
	return com_rslt;
}

/*!
*	@brief This API is used to write, select
*	the significant or any motion interrupt from the register 0x62 bit 1
*
*  @param  v_intr_significant_motion_select_u8 :
*	the value of significant or any motion interrupt selection
*	value    | Behaviour
* ----------|-------------------
*  0x00     |  ANY_MOTION
*  0x01     |  SIGNIFICANT_MOTION
*
*	@return results of bus communication function
*	@retval 0 -> Success
*	@retval -1 -> Error
*
*/
static BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_sm_select(u8 v_intr_significant_motion_select_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		if (v_intr_significant_motion_select_u8 <= BMI160_MAX_VALUE_SIGNIFICANT_MOTION) {
			/* write the significant or any motion interrupt */
			com_rslt = p_bmi160->BMI160_BUS_READ_FUNC
			    (p_bmi160->dev_addr,
			     BMI160_USER_INTR_SIGNIFICATION_MOTION_SELECT__REG,
			     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_SIGNIFICATION_MOTION_SELECT,
								v_intr_significant_motion_select_u8);
				com_rslt += p_bmi160->BMI160_BUS_WRITE_FUNC
				    (p_bmi160->dev_addr,
				     BMI160_USER_INTR_SIGNIFICATION_MOTION_SELECT__REG,
				     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
		} else {
			com_rslt = E_BMI160_OUT_OF_RANGE;
		}
	}
	return com_rslt;
}

/*!
 *	@brief This API used to trigger the  signification motion
 *	interrupt
 *
 *  @param  v_significant_u8 : The value of interrupt selection
 *  value    |  interrupt
 * ----------|-----------
 *   0       |  BMI160_MAP_INTR1
 *   1       |  BMI160_MAP_INTR2
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
*/
static BMI160_RETURN_FUNCTION_TYPE bmi160_map_sm_intr(u8 v_significant_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_sig_motion_u8 = BMI160_INIT_VALUE;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	u8 v_any_motion_intr1_stat_u8 = BMI160_ENABLE_ANY_MOTION_INTR1;
	u8 v_any_motion_intr2_stat_u8 = BMI160_ENABLE_ANY_MOTION_INTR2;
	u8 v_any_motion_axis_stat_u8 = BMI160_ENABLE_ANY_MOTION_AXIS;
	/* enable the significant motion interrupt */
	com_rslt = bmi160_get_intr_sm_select(&v_sig_motion_u8);
	if (v_sig_motion_u8 != BMI160_SIG_MOTION_STAT_HIGH)
		com_rslt += bmi160_set_intr_sm_select(BMI160_SIG_MOTION_INTR_ENABLE);
	switch (v_significant_u8) {
	case BMI160_MAP_INTR1:
		/* interrupt */
		com_rslt += bmi160_read_reg(BMI160_USER_INTR_MAP_0_INTR1_ANY_MOTION__REG,
					    &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_any_motion_intr1_stat_u8;
		/* map the signification interrupt to any-motion interrupt1 */
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_MAP_0_INTR1_ANY_MOTION__REG,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		/* axis */
		com_rslt = bmi160_read_reg(BMI160_USER_INTR_ENABLE_0_ADDR,
					   &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_any_motion_axis_stat_u8;
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_ENABLE_0_ADDR,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		break;

	case BMI160_MAP_INTR2:
		/* map the signification interrupt to any-motion interrupt2 */
		com_rslt += bmi160_read_reg(BMI160_USER_INTR_MAP_2_INTR2_ANY_MOTION__REG,
					    &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_any_motion_intr2_stat_u8;
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_MAP_2_INTR2_ANY_MOTION__REG,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		/* axis */
		com_rslt = bmi160_read_reg(BMI160_USER_INTR_ENABLE_0_ADDR,
					   &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		v_data_u8 |= v_any_motion_axis_stat_u8;
		com_rslt += bmi160_write_reg(BMI160_USER_INTR_ENABLE_0_ADDR,
					     &v_data_u8, BMI160_GEN_READ_WRITE_DATA_LENGTH);
		p_bmi160->delay_msec(BMI160_GEN_READ_WRITE_DELAY);
		break;

	default:
		com_rslt = E_BMI160_OUT_OF_RANGE;
		break;

	}
	return com_rslt;
}


/*!
 *	@brief  This API is used to set
 *	interrupt enable from the register 0x50 bit 0 to 7
 *
 *	@param v_enable_u8 : Value to decided to select interrupt
 *   v_enable_u8   |   interrupt
 *  ---------------|---------------
 *       0         | BMI160_ANY_MOTION_X_ENABLE
 *       1         | BMI160_ANY_MOTION_Y_ENABLE
 *       2         | BMI160_ANY_MOTION_Z_ENABLE
 *       3         | BMI160_DOUBLE_TAP_ENABLE
 *       4         | BMI160_SINGLE_TAP_ENABLE
 *       5         | BMI160_ORIENT_ENABLE
 *       6         | BMI160_FLAT_ENABLE
 *
 *	@param v_intr_enable_zero_u8 : The interrupt enable value
 *	value    | interrupt enable
 * ----------|-------------------
 *  0x01     |  BMI160_ENABLE
 *  0x00     |  BMI160_DISABLE
 *
 *	@return results of bus communication function
 *	@retval 0 -> Success
 *	@retval -1 -> Error
 *
 */
static BMI160_RETURN_FUNCTION_TYPE bmi160_set_intr_enable_0(u8 v_enable_u8,
							    u8 v_intr_enable_zero_u8)
{
	/* variable used for return the status of communication result */
	BMI160_RETURN_FUNCTION_TYPE com_rslt = E_BMI160_COMM_RES;
	u8 v_data_u8 = BMI160_INIT_VALUE;
	/* check the p_bmi160 structure as NULL */
	if (p_bmi160 == BMI160_NULL)
		com_rslt = E_BMI160_NULL_PTR;
	else {
		switch (v_enable_u8) {
		case BMI160_ANY_MOTION_X_ENABLE:
			/* write any motion x */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_ANY_MOTION_X_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_ANY_MOTION_X_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_ANY_MOTION_X_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_ANY_MOTION_Y_ENABLE:
			/* write any motion y */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_ANY_MOTION_Y_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_ANY_MOTION_Y_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_ANY_MOTION_Y_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_ANY_MOTION_Z_ENABLE:
			/* write any motion z */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_ANY_MOTION_Z_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_ANY_MOTION_Z_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_ANY_MOTION_Z_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_DOUBLE_TAP_ENABLE:
			/* write double tap */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_DOUBLE_TAP_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_DOUBLE_TAP_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_DOUBLE_TAP_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_SINGLE_TAP_ENABLE:
			/* write single tap */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_SINGLE_TAP_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_SINGLE_TAP_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_SINGLE_TAP_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_ORIENT_ENABLE:
			/* write orient interrupt */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_ORIENT_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_ORIENT_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_ORIENT_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		case BMI160_FLAT_ENABLE:
			/* write flat interrupt */
			com_rslt =
			    p_bmi160->BMI160_BUS_READ_FUNC(p_bmi160->dev_addr,
							   BMI160_USER_INTR_ENABLE_0_FLAT_ENABLE__REG,
							   &v_data_u8,
							   BMI160_GEN_READ_WRITE_DATA_LENGTH);
			if (com_rslt == SUCCESS) {
				v_data_u8 = BMI160_SET_BITSLICE(v_data_u8,
								BMI160_USER_INTR_ENABLE_0_FLAT_ENABLE,
								v_intr_enable_zero_u8);
				com_rslt +=
				    p_bmi160->BMI160_BUS_WRITE_FUNC(p_bmi160->dev_addr,
								    BMI160_USER_INTR_ENABLE_0_FLAT_ENABLE__REG,
								    &v_data_u8,
								    BMI160_GEN_READ_WRITE_DATA_LENGTH);
			}
			break;
		default:
			com_rslt = E_BMI160_OUT_OF_RANGE;
			break;
		}
	}
	return com_rslt;
}

static int sm_init_interrupts(u8 sig_map_int_pin)
{
	int ret = 0;
	/*0x60  */
	ret += bmi160_set_intr_any_motion_thres(0x1e);
	/* 0x62(bit 3~2)        0=1.5s */
	ret += bmi160_set_intr_sm_skip(0);
	/*0x62(bit 5~4) 1=0.5s */
	ret += bmi160_set_intr_sm_proof(1);
	/*0x50 (bit 0, 1, 2)  INT_EN_0 anymo x y z */
	ret += bmi160_map_sm_intr(sig_map_int_pin);
	/*0x62 (bit 1) INT_MOTION_3     int_sig_mot_sel
	   close the signification_motion */
	ret += bmi160_set_intr_sm_select(0);
	/*close the anymotion interrupt */
	ret += bmi160_set_intr_enable_0(BMI160_ANY_MOTION_X_ENABLE, 0);
	ret += bmi160_set_intr_enable_0(BMI160_ANY_MOTION_Y_ENABLE, 0);
	ret += bmi160_set_intr_enable_0(BMI160_ANY_MOTION_Z_ENABLE, 0);
	if (ret)
		GSE_ERR("bmi160 sig motion setting failed.\n");
	return ret;
}

/*!
 * @brief Input event initialization for device
 *
 * @param[in] client the pointer of bmi160_acc_i2c_data
 *
 * @return zero success, non-zero failed
 */
#if 0
static int bmi160_input_init(struct bmi160_acc_i2c_data *client_data)
{
	int ret = 0;
	struct input_dev *dev;

	dev = input_allocate_device();
	if (!dev) {
		input_free_device(dev);
		return -ENOMEM;
	}
	dev->name = BMI160_DEV_NAME;
	dev->id.bustype = BUS_I2C;
	input_set_capability(dev, EV_ABS, ABS_MISC);
	input_set_abs_params(dev, ABS_X, ABSMIN, ABSMAX, 0, 0);
	input_set_abs_params(dev, ABS_Y, ABSMIN, ABSMAX, 0, 0);
	input_set_abs_params(dev, ABS_Z, ABSMIN, ABSMAX, 0, 0);
	input_set_drvdata(dev, client_data);
	/*register device */
	ret = input_register_device(dev);
	if (ret < 0) {
		input_free_device(dev);
		GSE_ERR("bmi160 acc input register failed.\n");
		return ret;
	}
	client_data->input = dev;
	GSE_LOG("bmi160 acc input register ok.\n");
	return ret;
}
#endif

#ifdef CONFIG_CUSTOM_KERNEL_SIGNIFICANT_MOTION_SENSOR
static void bmi_std_interrupt_handle(struct bmi160_acc_i2c_data *client_data)
{
	u8 step_detect_enable = 0;
	int err = 0;

	err = bmi160_get_std_enable(&step_detect_enable);
	if (err < 0) {
		GSE_ERR("get step detect enable failed.\n");
		return;
	}
	if (ENABLE == step_detect_enable)
		err = bmi160_step_notify(TYPE_STEP_DETECTOR);
	if (err < 0)
		GSE_ERR("notify step detect failed.\n");
	GSE_LOG("step detect enable = %d.\n", step_detect_enable);
}

static void bmi_sm_interrupt_handle(struct bmi160_acc_i2c_data *client_data)
{
	u8 sig_sel = 0;
	int err = 0;

	err = bmi160_get_intr_sm_select(&sig_sel);
	if (err < 0) {
		GSE_ERR("get significant motion failed.\n");
		return;
	}
	if (ENABLE == sig_sel) {
		sig_flag = 1;
		err = bmi160_step_notify(TYPE_SIGNIFICANT);
	}
	if (err < 0)
		GSE_ERR("notify significant motion failed.\n");
	GSE_LOG("signification motion = %d.\n", (int)sig_sel);
}

#endif

static void bmi_irq_work_func(struct work_struct *work)
{
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;
	u8 int_status[4] = { 0, 0, 0, 0 };

	bma_i2c_read_block(client_data->client, BMI160_USER_INTR_STAT_0_ADDR, int_status, 4);
#if 0
	if (BMI160_GET_BITSLICE(int_status[0], BMI160_USER_INTR_STAT_0_ANY_MOTION))
		bmi_slope_interrupt_handle(client_data);
#endif

#ifdef CONFIG_CUSTOM_KERNEL_SIGNIFICANT_MOTION_SENSOR
	if (BMI160_GET_BITSLICE(int_status[0], BMI160_USER_INTR_STAT_0_STEP_INTR))
		bmi_std_interrupt_handle(client_data);
#endif

#if 0
	if (BMI160_GET_BITSLICE(int_status[1], BMI160_USER_INTR_STAT_1_FIFO_WM_INTR))
		bmi_fifo_watermark_interrupt_handle(client_data);
#endif

#ifdef CONFIG_CUSTOM_KERNEL_SIGNIFICANT_MOTION_SENSOR
	if (BMI160_GET_BITSLICE(int_status[0], BMI160_USER_INTR_STAT_0_SIGNIFICANT_INTR))
		bmi_sm_interrupt_handle(client_data);
#endif
}

#ifdef BMI160_INTERRUPT_ENABLE
static irqreturn_t bmi_irq_handler(int irq, void *handle)
{
	struct bmi160_acc_i2c_data *client_data = obj_i2c_data;

	if (client_data == NULL)
		return IRQ_HANDLED;
	schedule_work(&client_data->irq_work);
	return IRQ_HANDLED;
}
#endif

static int bmi160acc_setup_eint(void)
{
#ifdef BMI160_INTERRUPT_ENABLE
	int ret;
	struct device_node *node = NULL;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_eint;
	/* u32 ints[2] = {0, 0}; */
	struct bmi160_acc_i2c_data *obj = obj_i2c_data;
	/* gpio setting */
	pinctrl = devm_pinctrl_get(&(accel_Platform_Dev->dev));
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		GSE_ERR("Cannot find step pinctrl!\n");
		return ret;
	}
	pins_default = pinctrl_lookup_state(pinctrl, "default");
	if (IS_ERR(pins_default)) {
		ret = PTR_ERR(pins_default);
		GSE_ERR("Cannot find step pinctrl default!\n");
		/* return ret; */
	}
	pins_eint = pinctrl_lookup_state(pinctrl, "state_eint_as_int");
	if (IS_ERR(pins_eint)) {
		ret = PTR_ERR(pins_eint);
		GSE_ERR("Cannot find step pinctrl pins_eint!\n");
		return ret;
	}
	pinctrl_select_state(pinctrl, pins_eint);
	node = of_find_matching_node(NULL, gse_int_of_match);
	/* eint request */
	if (node) {
		GSE_LOG("irq node is ok!");
		/* of_property_read_u32_array(node, "debounce", ints, ARRAY_SIZE(ints)); */
		/* gpio_set_debounce(ints[0], ints[1]); */
		/* GSE_LOG("ints[0] = %d, ints[1] = %d!!\n", ints[0], ints[1]); */
		obj->IRQ = irq_of_parse_and_map(node, 0);
		GSE_LOG("obj->IRQ = %d\n", obj->IRQ);
		if (!obj->IRQ) {
			GSE_ERR("irq_of_parse_and_map fail!!\n");
			return -EINVAL;
		}
		if (request_irq
		    (obj->IRQ, bmi_irq_handler, IRQF_TRIGGER_RISING, "Gsensor-eint", NULL)) {
			GSE_ERR("IRQ LINE NOT AVAILABLE!!\n");
			return -EINVAL;
		}
		enable_irq(obj->IRQ);
	} else {
		GSE_ERR("null irq node!!\n");
		return -EINVAL;
	}
	return 0;
#else
	return 0;
#endif
}

/*!
 * @brief Delayed work queue call function
 *
 * @param[in] client the pointer of work_struct
 *
 */
/*
static void delay_work_func(struct work_struct *work)
{
	int err = 0;
	unsigned long delay;
	s16 data[BMI160_ACC_AXES_NUM] = {0};
	int acc[BMI160_ACC_AXES_NUM] = {0};
	struct bmi160_acc_i2c_data *obj = container_of((struct delayed_work *)work,
				struct bmi160_acc_i2c_data, work);
	struct i2c_client *client = bmi160_acc_i2c_client;
	delay = msecs_to_jiffies(atomic_read(&obj->delay));
	atomic_set(&obj->wkqueue_en, 1);
	err = BMI160_ACC_ReadData(client, data);
	if(err) {
		GSE_ERR("read acc raw data failed.\n");
		return;
	}
	acc[obj->cvt.map[BMI160_ACC_AXIS_X]] = obj->cvt.sign[BMI160_ACC_AXIS_X]*data[BMI160_ACC_AXIS_X];
	acc[obj->cvt.map[BMI160_ACC_AXIS_Y]] = obj->cvt.sign[BMI160_ACC_AXIS_Y]*data[BMI160_ACC_AXIS_Y];
	acc[obj->cvt.map[BMI160_ACC_AXIS_Z]] = obj->cvt.sign[BMI160_ACC_AXIS_Z]*data[BMI160_ACC_AXIS_Z];

	input_report_abs(obj->input, ABS_X, acc[BMI160_ACC_AXIS_X]);
	input_report_abs(obj->input, ABS_Y, acc[BMI160_ACC_AXIS_Y]);
	input_report_abs(obj->input, ABS_Z, acc[BMI160_ACC_AXIS_Z]);
	input_sync(obj->input);
	schedule_delayed_work(&obj->work, delay);
}
*/

static int bmi160_acc_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err = 0;
	struct i2c_client *new_client;
	struct bmi160_acc_i2c_data *obj;
	struct acc_control_path ctl = { 0 };
	struct acc_data_path data = { 0 };

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		err = -ENOMEM;
		goto exit;
	}
	memset(obj, 0, sizeof(struct bmi160_acc_i2c_data));
	obj->hw = hw;
	err = hwmsen_get_convert(obj->hw->direction, &obj->cvt);
	if (err) {
		GSE_ERR("invalid direction: %d\n", obj->hw->direction);
		goto exit;
	}
	obj_i2c_data = obj;
	client->addr = *hw->i2c_addr;
	obj->client = client;
	bmi160_acc_i2c_client = new_client = obj->client;
	i2c_set_clientdata(new_client, obj);
	atomic_set(&obj->trace, 0);
	atomic_set(&obj->suspend, 0);
	mutex_init(&obj->lock);
	/* input event register
	   err = bmi160_input_init(obj);
	   if(err) {
	   goto exit_init_failed;
	   }
	 */
	/* delayed work
	   INIT_DELAYED_WORK(&obj->work, delay_work_func);
	   atomic_set(&obj->delay, WORK_DELAY_DEFAULT);
	   atomic_set(&obj->wkqueue_en, 0);
	 */
	err = bmi160_acc_init_client(new_client, 1);
	if (err)
		goto exit_init_failed;
#ifdef FIFO_READ_USE_DMA_MODE_I2C
#ifdef CONFIG_MTK_I2C_EXTENSION
	client->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	client->dev.dma_mask = &client->dev.coherent_dma_mask;
	/* DMA size for customer */
	gpDMABuf_va =
	    (u8 *) dma_alloc_coherent(&client->dev, ACCEL_DMA_MAX_TRANSACTION_LENGTH, &gpDMABuf_pa,
				      GFP_KERNEL | GFP_DMA);
	if (!gpDMABuf_va)
		GSE_ERR("[DMA][Error] Allocate DMA I2C Buffer failed!\n");

	memset(gpDMABuf_va, 0, ACCEL_DMA_MAX_TRANSACTION_LENGTH);
#endif
#endif

	err = bmi160acc_setup_eint();
	if (err)
		GSE_ERR("could not request irq\n");

	err = misc_register(&bmi160_acc_device);
	if (err) {
		GSE_ERR("bmi160_acc_device register failed.\n");
		goto exit_misc_device_register_failed;
	}

	err = bmi160_acc_create_attr(&(bmi160_acc_init_info.platform_diver_addr->driver));
	if (err) {
		GSE_ERR("create attribute failed.\n");
		goto exit_create_attr_failed;
	}
	ctl.open_report_data = bmi160_acc_open_report_data;
	ctl.enable_nodata = bmi160_acc_enable_nodata;
	ctl.set_delay = bmi160_acc_set_delay;
	ctl.is_report_input_direct = false;
	/* obj->is_input_enable = ctl.is_report_input_direct; */

	err = acc_register_control_path(&ctl);
	if (err) {
		GSE_ERR("register acc control path error.\n");
		goto exit_kfree;
	}
	data.get_data = bmi160_acc_get_data;
	data.vender_div = 1000;
	err = acc_register_data_path(&data);
	if (err) {
		GSE_ERR("register acc data path error.\n");
		goto exit_kfree;
	}
#ifdef CONFIG_HAS_EARLYSUSPEND
	obj->early_drv.level = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1,
	    obj->early_drv.suspend = bmi160_acc_early_suspend,
	    obj->early_drv.resume = bmi160_acc_late_resume, register_early_suspend(&obj->early_drv);
#endif
	/* h/w init */
	obj->device.bus_read = bmi_i2c_read_wrapper;
	obj->device.bus_write = bmi_i2c_write_wrapper;
	obj->device.delay_msec = bmi_delay;
	bmi160_init(&obj->device);

	/* maps interrupt to INT1 pin, set interrupt trigger level way */
	bmi160_set_intr_edge_ctrl(BMI_INT0, BMI_INT_LEVEL);
	udelay(500);
	bmi160_set_intr_fifo_wm(BMI_INT0, ENABLE);
	udelay(500);
	bmi160_set_intr_level(BMI_INT0, ENABLE);
	udelay(500);
#ifdef BMI160_INTERRUPT_ENABLE
	bmi160_set_output_enable(BMI160_INTR1_OUTPUT_ENABLE, ENABLE);
#else
	bmi160_set_output_enable(BMI160_INTR1_OUTPUT_ENABLE, DISABLE);
#endif
	udelay(500);
	sm_init_interrupts(BMI160_MAP_INTR1);
	mdelay(3);
	bmi160_map_std_intr(BMI160_MAP_INTR1);
	mdelay(3);
	bmi160_set_std_enable(DISABLE);
	mdelay(3);
	/* fifo setting */
	bmi160_set_fifo_header_enable(ENABLE);
	udelay(500);
	bmi160_set_fifo_time_enable(DISABLE);
	udelay(500);
	INIT_WORK(&obj->irq_work, bmi_irq_work_func);
	bmi160_acc_init_flag = 0;
	GSE_LOG("%s: is ok.\n", __func__);
	return 0;

exit_create_attr_failed:
	misc_deregister(&bmi160_acc_device);
exit_misc_device_register_failed:
exit_init_failed:
exit_kfree:
	kfree(obj);
exit:
	GSE_ERR("%s: err = %d\n", __func__, err);
	bmi160_acc_init_flag = -1;
	return err;
}

static int bmi160_acc_i2c_remove(struct i2c_client *client)
{
	int err = 0;

	err = bmi160_acc_delete_attr(&(bmi160_acc_init_info.platform_diver_addr->driver));
	if (err)
		GSE_ERR("delete device attribute failed.\n");

	err = misc_deregister(&bmi160_acc_device);
	if (err)
		GSE_ERR("misc_deregister fail: %d\n", err);

	bmi160_acc_i2c_client = NULL;
	i2c_unregister_device(client);
	kfree(obj_i2c_data);
	return 0;
}

static int bmi160_acc_local_init(void)
{
	/* compulsory registration for testing
	   struct i2c_adapter *adapter;
	   struct i2c_client *client;
	   struct i2c_board_info info;
	   adapter = i2c_get_adapter(3);
	   if(adapter == NULL){
	   printk(KERN_ERR"gsensor_local_init error");
	   }
	   memset(&info,0,sizeof(struct i2c_board_info));
	   info.addr = 0x68;
	   //info.type = "mediatek,gsensor";
	   strlcpy(info.type,"bmi160_acc",I2C_NAME_SIZE);
	   client = i2c_new_device(adapter,&info);
	   strlcpy(client->name,"bmi160_acc",I2C_NAME_SIZE);
	 */
	BMI160_ACC_power(hw, 1);
	if (i2c_add_driver(&bmi160_acc_i2c_driver)) {
		GSE_ERR("add driver error\n");
		return -1;
	}
	if (-1 == bmi160_acc_init_flag)
		return -1;
	GSE_LOG("bmi160 acc local init.\n");
	return 0;
}

static int bmi160_acc_remove(void)
{
	GSE_FUN();
	BMI160_ACC_power(hw, 0);
	i2c_del_driver(&bmi160_acc_i2c_driver);
	return 0;
}

static struct acc_init_info bmi160_acc_init_info = {
	.name = BMI160_DEV_NAME,
	.init = bmi160_acc_local_init,
	.uninit = bmi160_acc_remove,
};

static int __init bmi160_acc_init(void)
{
	const char *name = "mediatek,bmi160_acc";

	hw = get_accel_dts_func(name, hw);
	if (!hw) {
		GSE_ERR("device tree configuration error!\n");
		return 0;
	}
	GSE_FUN();
	acc_driver_add(&bmi160_acc_init_info);
	return 0;
}

static void __exit bmi160_acc_exit(void)
{
	GSE_FUN();
}

module_init(bmi160_acc_init);
module_exit(bmi160_acc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BMI160_ACC I2C driver");
MODULE_AUTHOR("xiaogang.fan@cn.bosch.com");
