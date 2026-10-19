// SPDX-License-Identifier: GPL-2.0
/*
 * GalaxyCore GC5024 5MP MIPI CSI-2 sensor driver
 *
 * Copyright (C) 2026
 *
 * Register initialisation data derived from the Rockchip BSP driver
 * (drivers/media/i2c/gc5024.c, Copyright (C) 2020 Rockchip Electronics).
 *
 * This driver is written for the Huawei MateBook E (Kaby Lake / Intel IPU3)
 * whose ACPI firmware describes the rear camera as:
 *
 *     Device (CAM1)
 *     {
 *         Name (_HID, "GCTI5024")     // method returning "GCTI5024"
 *         Name (_DDN, "BEGC5024")
 *         Method (_CRS) -> I2cSerialBusV2 (0x0037, "\\_SB.PCI0.I2C2")
 *         Method (SSDB) -> 2 lanes, 19.2 MHz MCLK
 *     }
 *
 * so the ACPI HID is GCTI5024 and the I2C address is 0x37.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "gc5024_regs.h"

#define GC5024_CHIP_ID			0x5024

#define GC5024_REG_CHIP_ID_H		CCI_REG8(0xf0)
#define GC5024_REG_CHIP_ID_L		CCI_REG8(0xf1)
#define GC5024_REG_PAGE_SELECT		CCI_REG8(0xfe)
#define GC5024_REG_MODE_SELECT		CCI_REG8(0x10)
#define GC5024_MODE_SW_STANDBY		0x00
#define GC5024_MODE_STREAMING		0x91

#define GC5024_REG_EXPOSURE_H		CCI_REG8(0x03)
#define GC5024_REG_EXPOSURE_L		CCI_REG8(0x04)
#define GC5024_EXPOSURE_MIN		4
#define GC5024_EXPOSURE_STEP		1

#define GC5024_REG_VTS_H		CCI_REG8(0x07)
#define GC5024_REG_VTS_L		CCI_REG8(0x08)
#define GC5024_VTS_MAX			0x1fff

#define GC5024_REG_ANALOG_GAIN		CCI_REG8(0xb6)
#define GC5024_REG_PREGAIN_H		CCI_REG8(0xb1)
#define GC5024_REG_PREGAIN_L		CCI_REG8(0xb2)

#define GC5024_GAIN_MIN			0x40
#define GC5024_GAIN_MAX			0x200
#define GC5024_GAIN_STEP		1
#define GC5024_GAIN_DEFAULT		0x80

/* Analog gain break points, in units of 1/64x */
#define GC5024_AGAIN_1			64	/* 1.000x */
#define GC5024_AGAIN_2			88	/* 1.375x */
#define GC5024_AGAIN_3			122	/* 1.906x */
#define GC5024_AGAIN_4			168	/* 2.625x */
#define GC5024_AGAIN_5			239	/* 3.734x */
#define GC5024_AGAIN_6			330	/* 5.156x */
#define GC5024_AGAIN_7			470	/* 7.344x */

#define GC5024_NUM_SUPPLIES		3
static const char *const gc5024_supply_names[GC5024_NUM_SUPPLIES] = {
	"avdd",		/* Analog 2.8V */
	"dovdd",	/* Digital I/O 1.8V */
	"dvdd",		/* Digital core 1.2V */
};

/*
 * Link frequency: the vendor register table configures the sensor's MIPI PLL
 * for 420 MHz (840 Mbps/lane DDR, 2 lanes) when fed a 24 MHz MCLK.
 *
 * This board's ACPI SSDB declares a 19.2 MHz MCLK, and the sensor's PLL is
 * fed from that clock, so the real link frequency may end up 19.2/24 of the
 * nominal value.  Both are overridable at load time; see the module
 * parameters at the bottom of this file.  Keep the value in sync with the
 * ipu_supported_sensors[] entry added by the ipu-bridge patch.
 */
#define GC5024_DEFAULT_LINK_FREQ	420000000ULL
#define GC5024_LANES			2
#define GC5024_BITS_PER_SAMPLE		10

#define GC5024_DEFAULT_MCLK		19200000
#define GC5024_MCLK_MAX			24000000

/*
 * Bring-up overrides.  The vendor register table is tuned for a 24 MHz MCLK
 * while this board's SSDB declares 19.2 MHz, and the resulting MIPI link
 * frequency depends on the sensor's internal PLL.  Until the real values are
 * confirmed on hardware these let the driver be tested without a rebuild.
 *
 * Note: link_freq_mhz must match the value used in the ipu-bridge patch, or
 * the IPU3 receiver will be programmed for the wrong bit rate.
 */
static int gc5024_mclk_hz = -1;
module_param_named(mclk_hz, gc5024_mclk_hz, int, 0444);
MODULE_PARM_DESC(mclk_hz,
		 "Override MCLK in Hz (default: use ACPI clock-frequency)");

static uint gc5024_link_freq_mhz = 420;
module_param_named(link_freq_mhz, gc5024_link_freq_mhz, uint, 0444);
MODULE_PARM_DESC(link_freq_mhz,
		 "Advertised MIPI link frequency in MHz (default: 420)");

static bool gc5024_mode_720p;
module_param_named(mode_720p, gc5024_mode_720p, bool, 0444);
MODULE_PARM_DESC(mode_720p,
		 "Use the 1280x720 windowed readout mode");

static bool gc5024_mode_1080p;
module_param_named(mode_1080p, gc5024_mode_1080p, bool, 0444);
MODULE_PARM_DESC(mode_1080p,
		 "Use the 1920x1080 windowed readout mode");

struct gc5024_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	u32 exp_def;
	struct v4l2_fract max_fps;
};

static const struct gc5024_mode gc5024_supported_modes[] = {
	{
		.width = 2592,
		.height = 1944,
		.hts = 0x12c0,
		.vts = 0x07d0,
		.exp_def = 0x07c0,
		.max_fps = { .numerator = 10000, .denominator = 200000 },
	},
	{
		/* Readout 1312x736, ISP crops to 1280x720. */
		.width = 1280,
		.height = 720,
		.hts = 0x0714,		/* 1812 = 1312 + 500 HB */
		.vts = 0x02f0,		/* 752 = 736 + 16 VB */
		.exp_def = 0x02b0,
		.max_fps = { .numerator = 1, .denominator = 30 },
	},
	{
		/* Readout 1952x1096, ISP crops to 1920x1080. */
		.width = 1920,
		.height = 1080,
		.hts = 0x0994,		/* 2452 = 1952 + 500 HB */
		.vts = 0x0458,		/* 1112 = 1080 + 32 VB */
		.exp_def = 0x03e8,
		.max_fps = { .numerator = 1, .denominator = 30 },
	},
};

struct gc5024 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;

	s64 link_freq_hz;
	u64 pixel_rate_hz;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;
	struct regulator_bulk_data supplies[GC5024_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *anal_gain;

	const struct gc5024_mode *mode;
	u32 mclk;
};

static inline struct gc5024 *to_gc5024(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc5024, sd);
}

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

static int gc5024_write_regs(struct gc5024 *gc5024,
			     const struct cci_reg_sequence *regs,
			     unsigned int num_regs)
{
	int ret = 0;

	cci_multi_reg_write(gc5024->regmap, regs, num_regs, &ret);
	if (ret)
		dev_err(gc5024->sd.dev, "failed to write register table: %d\n",
			ret);

	return ret;
}

static int gc5024_set_stream(struct gc5024 *gc5024, bool on)
{
	int ret = 0;

	cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x03, &ret);
	cci_write(gc5024->regmap, GC5024_REG_MODE_SELECT,
		  on ? GC5024_MODE_STREAMING : GC5024_MODE_SW_STANDBY, &ret);
	cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x00, &ret);

	return ret;
}

/* ------------------------------------------------------------------ */
/* Power management                                                    */
/* ------------------------------------------------------------------ */

static int gc5024_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc5024 *gc5024 = to_gc5024(sd);
	int ret;

	ret = regulator_bulk_enable(GC5024_NUM_SUPPLIES, gc5024->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	/*
	 * The rails need to be stable before the sensor is taken out of
	 * reset, otherwise it never starts its internal power-on sequence.
	 * On a cold probe this is masked by the rails already being up; on a
	 * runtime-PM resume it is not.
	 */
	msleep(10);

	ret = clk_set_rate(gc5024->xclk, gc5024->mclk);
	if (ret)
		dev_warn(dev, "failed to set xclk to %u Hz: %d\n",
			 gc5024->mclk, ret);

	ret = clk_prepare_enable(gc5024->xclk);
	if (ret) {
		dev_err(dev, "failed to enable clock: %d\n", ret);
		goto err_disable_regulators;
	}

	/* MCLK must be running before reset is deasserted. */
	msleep(5);

	gpiod_set_value_cansleep(gc5024->powerdown_gpio, 0);
	gpiod_set_value_cansleep(gc5024->reset_gpio, 1);
	msleep(5);
	gpiod_set_value_cansleep(gc5024->reset_gpio, 0);

	/* Give the sensor time to boot before the first SCCB transfer. */
	msleep(20);

	return 0;

err_disable_regulators:
	regulator_bulk_disable(GC5024_NUM_SUPPLIES, gc5024->supplies);
	return ret;
}

static int gc5024_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc5024 *gc5024 = to_gc5024(sd);

	gpiod_set_value_cansleep(gc5024->reset_gpio, 1);
	gpiod_set_value_cansleep(gc5024->powerdown_gpio, 1);

	clk_disable_unprepare(gc5024->xclk);
	regulator_bulk_disable(GC5024_NUM_SUPPLIES, gc5024->supplies);

	return 0;
}

static int gc5024_get_regulators(struct gc5024 *gc5024)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc5024->sd);
	unsigned int i;

	for (i = 0; i < GC5024_NUM_SUPPLIES; i++)
		gc5024->supplies[i].supply = gc5024_supply_names[i];

	return devm_regulator_bulk_get(&client->dev, GC5024_NUM_SUPPLIES,
				       gc5024->supplies);
}

static int gc5024_identify_module(struct gc5024 *gc5024)
{
	struct i2c_client *client = v4l2_get_subdevdata(&gc5024->sd);
	u64 id_h, id_l;
	u32 id;
	int ret;

	ret = cci_read(gc5024->regmap, GC5024_REG_CHIP_ID_H, &id_h,
		       NULL);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read chip ID high byte\n");

	ret = cci_read(gc5024->regmap, GC5024_REG_CHIP_ID_L, &id_l,
		       NULL);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read chip ID low byte\n");

	id = (id_h << 8) | id_l;
	if (id != GC5024_CHIP_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "chip ID mismatch: expected 0x%04x, got 0x%04x\n",
				     GC5024_CHIP_ID, id);

	dev_info(&client->dev, "detected GC5024 sensor (0x%04x)\n", id);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

/*
 * The GC5024 gain block is piecewise linear: a 3-bit coarse "analog gain"
 * selector plus a 10-bit pre-gain value that covers the ratio inside each
 * band.  This mirrors the vendor implementation.
 */
static int gc5024_set_gain(struct gc5024 *gc5024, u32 again)
{
	static const u32 band_min[] = {
		GC5024_AGAIN_1, GC5024_AGAIN_2, GC5024_AGAIN_3, GC5024_AGAIN_4,
		GC5024_AGAIN_5, GC5024_AGAIN_6, GC5024_AGAIN_7,
	};
	u32 coarse, base, pre;
	int ret = 0;
	int i;

	for (i = ARRAY_SIZE(band_min) - 1; i > 0; i--)
		if (again >= band_min[i])
			break;

	coarse = i;
	base = band_min[i];

	if (i == 0)
		pre = again;
	else
		pre = 64 * again / base;

	cci_write(gc5024->regmap, GC5024_REG_ANALOG_GAIN, coarse, &ret);
	cci_write(gc5024->regmap, GC5024_REG_PREGAIN_H, (pre >> 6) & 0xff,
		  &ret);
	cci_write(gc5024->regmap, GC5024_REG_PREGAIN_L,
		  (pre << 2) & 0xfc, &ret);

	return ret;
}

static int gc5024_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc5024 *gc5024 =
		container_of(ctrl->handler, struct gc5024, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&gc5024->sd);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Adjust the exposure limit as vblank changes. */
		__v4l2_ctrl_modify_range(gc5024->exposure,
					 gc5024->exposure->minimum,
					 gc5024->mode->height + ctrl->val - 4,
					 GC5024_EXPOSURE_STEP,
					 gc5024->exposure->default_value);
		break;
	default:
		break;
	}

	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x00, &ret);
		cci_write(gc5024->regmap, GC5024_REG_EXPOSURE_H,
			  (ctrl->val >> 8) & 0x3f, &ret);
		cci_write(gc5024->regmap, GC5024_REG_EXPOSURE_L,
			  ctrl->val & 0xff, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x00, &ret);
		ret = gc5024_set_gain(gc5024, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		/*
		 * P0:0x07/0x08 is the vertical blanking itself, not the total
		 * frame length: the frame is VB + window height lines.  The
		 * old code wrote height + vblank here, adding a whole extra
		 * window of blanking to every frame.
		 */
		cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x00, &ret);
		cci_write(gc5024->regmap, GC5024_REG_VTS_H,
			  (ctrl->val >> 8) & 0x1f, &ret);
		cci_write(gc5024->regmap, GC5024_REG_VTS_L, ctrl->val & 0xff,
			  &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops gc5024_ctrl_ops = {
	.s_ctrl = gc5024_s_ctrl,
};

static int gc5024_init_controls(struct gc5024 *gc5024)
{
	const struct gc5024_mode *mode = gc5024->mode;
	struct v4l2_ctrl_handler *handler = &gc5024->ctrl_handler;
	struct v4l2_ctrl *ctrl;
	u32 hblank = mode->hts - mode->width;
	u32 vblank = mode->vts - mode->height;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;

	gc5024->link_freq =
		v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				       &gc5024->link_freq_hz);
	if (gc5024->link_freq)
		gc5024->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5024->pixel_rate =
		v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE, 0,
				  gc5024->pixel_rate_hz, 1,
				  gc5024->pixel_rate_hz);
	if (gc5024->pixel_rate)
		gc5024->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5024->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
					   hblank, hblank, 1, hblank);
	if (gc5024->hblank)
		gc5024->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5024->vblank = v4l2_ctrl_new_std(handler, &gc5024_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   GC5024_VTS_MAX - mode->height, 1,
					   vblank);

	gc5024->exposure = v4l2_ctrl_new_std(handler, &gc5024_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     GC5024_EXPOSURE_MIN,
					     mode->vts - 4,
					     GC5024_EXPOSURE_STEP,
					     mode->exp_def);

	gc5024->anal_gain = v4l2_ctrl_new_std(handler, &gc5024_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN,
					      GC5024_GAIN_MIN, GC5024_GAIN_MAX,
					      GC5024_GAIN_STEP,
					      GC5024_GAIN_DEFAULT);

	ctrl = v4l2_ctrl_new_std_menu(handler, NULL, V4L2_CID_CAMERA_ORIENTATION,
				      V4L2_CAMERA_ORIENTATION_FRONT, 0,
				      V4L2_CAMERA_ORIENTATION_FRONT);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_CAMERA_SENSOR_ROTATION,
				 0, 0, 1, 0);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (handler->error) {
		ret = handler->error;
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	gc5024->sd.ctrl_handler = handler;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Subdev pad operations                                               */
/* ------------------------------------------------------------------ */

static void gc5024_fill_format(const struct gc5024_mode *mode,
			       struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int gc5024_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct gc5024 *gc5024 = to_gc5024(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, 0);
	gc5024_fill_format(gc5024->mode, fmt);

	return 0;
}

static int gc5024_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad != 0 || code->index != 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int gc5024_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0 || fse->index != 0)
		return -EINVAL;

	if (fse->code && fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = to_gc5024(sd)->mode->width;
	fse->max_width = to_gc5024(sd)->mode->width;
	fse->min_height = to_gc5024(sd)->mode->height;
	fse->max_height = to_gc5024(sd)->mode->height;

	return 0;
}

static int gc5024_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct gc5024 *gc5024 = to_gc5024(sd);
	const struct gc5024_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;

	mode = gc5024->mode;
	gc5024_fill_format(mode, &fmt->format);

	framefmt = v4l2_subdev_state_get_format(state, 0);
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		gc5024->mode = mode;

	*framefmt = fmt->format;

	return 0;
}

static int gc5024_get_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_interval *fi)
{
	fi->interval = to_gc5024(sd)->mode->max_fps;

	return 0;
}

static int gc5024_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct gc5024 *gc5024 = to_gc5024(sd);

	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = gc5024->mode->width;
		sel->r.height = gc5024->mode->height;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = 2592;
		sel->r.height = 1944;
		return 0;
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

static int gc5024_apply_mode(struct gc5024 *gc5024)
{
	const struct gc5024_mode *mode = gc5024->mode;
	const u32 out_w = mode->width, out_h = mode->height;
	u32 win_w, win_h, col_start, row_start, lwc;
	int ret = 0;

	/* The global table already programs the full 2592x1944 window. */
	if (out_w == 2592 && out_h == 1944)
		return 0;

	/* Readout window is the output plus the ISP crop margins (32x16). */
	win_w = out_w + 32;
	win_h = out_h + 16;
	col_start = (2624 - win_w) / 2;
	row_start = (1960 - win_h) / 2;
	lwc = out_w * 5 / 4;		/* RAW10 line word count */

	cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x00, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x09), (row_start >> 8) & 0x07, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x0a), row_start & 0xff, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x0b), (col_start >> 8) & 0x0f, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x0c), col_start & 0xfe, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x0d), (win_h >> 8) & 0x07, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x0e), win_h & 0xff, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x0f), (win_w >> 8) & 0x0f, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x10), win_w & 0xfe, &ret);
	/* ISP output window */
	cci_write(gc5024->regmap, CCI_REG8(0x95), (mode->height >> 8) & 0x07, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x96), mode->height & 0xff, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x97), (mode->width >> 8) & 0x0f, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x98), mode->width & 0xfe, &ret);
	/* MIPI line word count and buffer window width */
	cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x03, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x12), lwc & 0xff, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x13), (lwc >> 8) & 0xff, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x42), mode->width & 0xff, &ret);
	cci_write(gc5024->regmap, CCI_REG8(0x43), (mode->width >> 8) & 0x0f, &ret);
	cci_write(gc5024->regmap, GC5024_REG_PAGE_SELECT, 0x00, &ret);

	return ret;
}

static int gc5024_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct gc5024 *gc5024 = to_gc5024(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	ret = gc5024_write_regs(gc5024, gc5024_global_regs,
				ARRAY_SIZE(gc5024_global_regs));
	if (ret)
		goto err_rpm_put;

	ret = gc5024_apply_mode(gc5024);
	if (ret)
		goto err_rpm_put;

	ret = __v4l2_ctrl_handler_setup(&gc5024->ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = gc5024_set_stream(gc5024, true);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put_autosuspend(&client->dev);
	return ret;
}

static int gc5024_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct gc5024 *gc5024 = to_gc5024(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = gc5024_set_stream(gc5024, false);

	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops gc5024_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops gc5024_pad_ops = {
	.enum_mbus_code = gc5024_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = gc5024_set_pad_format,
	.get_selection = gc5024_get_selection,
	.enum_frame_size = gc5024_enum_frame_size,
	.get_frame_interval = gc5024_get_frame_interval,
	.enable_streams = gc5024_enable_streams,
	.disable_streams = gc5024_disable_streams,
};

static const struct v4l2_subdev_ops gc5024_subdev_ops = {
	.video = &gc5024_video_ops,
	.pad = &gc5024_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc5024_internal_ops = {
	.init_state = gc5024_init_state,
};

/* ------------------------------------------------------------------ */
/* Probe / remove                                                      */
/* ------------------------------------------------------------------ */

static int gc5024_parse_mclk(struct gc5024 *gc5024)
{
	struct device *dev = gc5024->sd.dev;
	u32 freq = GC5024_DEFAULT_MCLK;
	int ret;

	if (gc5024_mclk_hz > 0) {
		freq = gc5024_mclk_hz;
		dev_info(dev, "MCLK overridden to %u Hz by module parameter\n",
			 freq);
	} else {
		ret = device_property_read_u32(dev, "clock-frequency", &freq);
		if (ret)
			dev_info(dev,
				 "no clock-frequency property, assuming %u Hz\n",
				 GC5024_DEFAULT_MCLK);
	}

	if (freq < 6000000 || freq > GC5024_MCLK_MAX) {
		dev_err(dev, "unsupported MCLK frequency %u Hz\n", freq);
		return -EINVAL;
	}

	gc5024->mclk = freq;

	return 0;
}

static int gc5024_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc5024 *gc5024;
	int ret;

	gc5024 = devm_kzalloc(dev, sizeof(*gc5024), GFP_KERNEL);
	if (!gc5024)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&gc5024->sd, client, &gc5024_subdev_ops);
	gc5024->sd.internal_ops = &gc5024_internal_ops;
	gc5024->sd.dev = dev;
	if (gc5024_mode_1080p)
		gc5024->mode = &gc5024_supported_modes[2];
	else if (gc5024_mode_720p)
		gc5024->mode = &gc5024_supported_modes[1];
	else
		gc5024->mode = &gc5024_supported_modes[0];
	gc5024->link_freq_hz = (s64)gc5024_link_freq_mhz * 1000000;
	gc5024->pixel_rate_hz = gc5024->link_freq_hz * 2 * GC5024_LANES /
				GC5024_BITS_PER_SAMPLE;

	ret = gc5024_parse_mclk(gc5024);
	if (ret)
		return ret;

	if (gc5024_link_freq_mhz < 80 || gc5024_link_freq_mhz > 3000)
		return dev_err_probe(dev, -EINVAL,
				     "invalid link frequency %u MHz\n",
				     gc5024_link_freq_mhz);

	gc5024->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(gc5024->xclk))
		return dev_err_probe(dev, PTR_ERR(gc5024->xclk),
				     "failed to get xclk\n");

	gc5024->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(gc5024->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(gc5024->reset_gpio),
				     "failed to get reset GPIO\n");

	gc5024->powerdown_gpio = devm_gpiod_get_optional(dev, "powerdown",
							 GPIOD_OUT_HIGH);
	if (IS_ERR(gc5024->powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(gc5024->powerdown_gpio),
				     "failed to get powerdown GPIO\n");

	ret = gc5024_get_regulators(gc5024);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	gc5024->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(gc5024->regmap))
		return dev_err_probe(dev, PTR_ERR(gc5024->regmap),
				     "failed to init CCI regmap\n");

	/* The sensor must be powered to read the chip ID. */
	ret = gc5024_power_on(dev);
	if (ret)
		return ret;

	ret = gc5024_identify_module(gc5024);
	if (ret)
		goto err_power_off;

	ret = gc5024_init_controls(gc5024);
	if (ret)
		goto err_power_off;

	gc5024->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	gc5024->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	gc5024->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&gc5024->sd.entity, 1, &gc5024->pad);
	if (ret)
		goto err_free_handler;

	gc5024->sd.state_lock = gc5024->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&gc5024->sd);
	if (ret)
		goto err_media_entity;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&gc5024->sd);
	if (ret)
		goto err_subdev_cleanup;

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(&gc5024->sd);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
err_media_entity:
	media_entity_cleanup(&gc5024->sd.entity);
err_free_handler:
	v4l2_ctrl_handler_free(&gc5024->ctrl_handler);
err_power_off:
	gc5024_power_off(dev);

	return ret;
}

static void gc5024_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc5024 *gc5024 = to_gc5024(sd);

	v4l2_subdev_cleanup(sd);
	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&gc5024->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc5024_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct dev_pm_ops gc5024_pm_ops = {
	RUNTIME_PM_OPS(gc5024_power_off, gc5024_power_on, NULL)
};

static const struct acpi_device_id gc5024_acpi_ids[] = {
	{ "GCTI5024" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(acpi, gc5024_acpi_ids);

static const struct of_device_id gc5024_of_ids[] = {
	{ .compatible = "galaxycore,gc5024" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gc5024_of_ids);

static struct i2c_driver gc5024_i2c_driver = {
	.driver = {
		.name = "gc5024",
		.acpi_match_table = gc5024_acpi_ids,
		.of_match_table = gc5024_of_ids,
		.pm = pm_ptr(&gc5024_pm_ops),
	},
	.probe = gc5024_probe,
	.remove = gc5024_remove,
};

module_i2c_driver(gc5024_i2c_driver);

MODULE_DESCRIPTION("GalaxyCore GC5024 5MP MIPI CSI-2 sensor driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("MateBook E camera project");
