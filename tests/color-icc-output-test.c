/*
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <linux/limits.h>

#include <lcms2.h>

#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"
#include "color_util.h"
#include "image-iter.h"
#include "lcms_util.h"

struct lcms_pipeline {
	/**
	 * Color space name
	 */
	const char *color_space;
	/**
	 * Chromaticities for output profile
	 */
	cmsCIExyYTRIPLE prim_output;
	/**
	 * tone curve enum
	 */
	enum transfer_fn pre_fn;
	/**
	 * Transform matrix from sRGB to target chromaticities in prim_output
	 */
	struct lcmsMAT3 mat;
	/**
	 * matrix from prim_output to XYZ, for example matrix conversion
	 * sRGB->XYZ, adobeRGB->XYZ, bt2020->XYZ
	 */
	struct lcmsMAT3 mat2XYZ;
	/**
	 * tone curve enum
	 */
	enum transfer_fn post_fn;
};

static const int WINDOW_WIDTH  = 256;
static const int WINDOW_HEIGHT = 24;

static cmsCIExyY wp_d65 = { 0.31271, 0.32902, 1.0 };

enum profile_type {
	PTYPE_MATRIX_SHAPER,
	PTYPE_CLUT,
};

/*
 * Using currently destination gamut bigger than source.
 * Using https://www.colour-science.org/ we can extract conversion matrix:
 * import colour
 * colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['sRGB'], colour.RGB_COLOURSPACES['Adobe RGB (1998)'], None)
 * colour.matrix_RGB_to_RGB(colour.RGB_COLOURSPACES['sRGB'], colour.RGB_COLOURSPACES['ITU-R BT.2020'], None)
 */

const struct lcms_pipeline pipeline_sRGB = {
	.color_space = "sRGB",
	.prim_output = {
		.Red =   { 0.640, 0.330, 1.0 },
		.Green = { 0.300, 0.600, 1.0 },
		.Blue =  { 0.150, 0.060, 1.0 }
	},
	.pre_fn = TRANSFER_FN_SRGB_EOTF,
	.mat = LCMSMAT3(1.0, 0.0, 0.0,
			0.0, 1.0, 0.0,
			0.0, 0.0, 1.0),
	.mat2XYZ = LCMSMAT3(0.436037, 0.385124, 0.143039,
			    0.222482, 0.716913, 0.060605,
			    0.013922, 0.097078, 0.713899),
	.post_fn = TRANSFER_FN_SRGB_EOTF_INVERSE
};

const struct lcms_pipeline pipeline_adobeRGB = {
	.color_space = "adobeRGB",
	.prim_output = {
		.Red =   { 0.640, 0.330, 1.0 },
		.Green = { 0.210, 0.710, 1.0 },
		.Blue =  { 0.150, 0.060, 1.0 }
	},
	.pre_fn = TRANSFER_FN_SRGB_EOTF,
	.mat = LCMSMAT3( 0.715127, 0.284868, 0.000005,
			 0.000001, 0.999995, 0.000004,
			-0.000003, 0.041155, 0.958848),
	.mat2XYZ = LCMSMAT3(0.609740, 0.205279, 0.149181,
			    0.311111, 0.625681, 0.063208,
			    0.019469, 0.060879, 0.744552),
	.post_fn = TRANSFER_FN_ADOBE_RGB_EOTF_INVERSE
};

const struct lcms_pipeline pipeline_BT2020 = {
	.color_space = "bt2020",
	.prim_output = {
		.Red =   { 0.708, 0.292, 1.0 },
		.Green = { 0.170, 0.797, 1.0 },
		.Blue =  { 0.131, 0.046, 1.0 }
	},
	.pre_fn = TRANSFER_FN_SRGB_EOTF,
	.mat = LCMSMAT3(0.627402, 0.329292, 0.043306,
			0.069095, 0.919544, 0.011360,
			0.016394, 0.088028, 0.895578),
	/* this is equivalent to BT.1886 with zero black level */
	.post_fn = TRANSFER_FN_POWER2_4_EOTF_INVERSE,
};

struct setup_args {
	struct fixture_metadata meta;
	int ref_image_index;
	const struct lcms_pipeline *pipeline;

	/**
	 * Two-norm color error tolerance in units of 1.0/255, computed in
	 * output electrical space.
	 *
	 * Tolerance depends more on the 1D LUT used for the
	 * inv EOTF than the tested 3D LUT size:
	 * 9x9x9, 17x17x17, 33x33x33, 127x127x127
	 *
	 * TODO: when we add power-law in the curve enumeration
	 * in GL-renderer, then we should fix the tolerance
	 * as the error should reduce a lot.
	 */
	float tolerance;

	/**
	 * 3DLUT dimension size
	 */
	int dim_size;
	enum profile_type type;

	/** Two-norm error limit for cLUT DToB->BToD roundtrip */
	float clut_roundtrip_tolerance;
};

static const struct setup_args my_setup_args[] = {
	/* name,          ref img, pipeline,     tolerance, dim, profile type, clut tolerance */
	{ { "sRGB->sRGB" },     0, &pipeline_sRGB,     0.0,  0, PTYPE_MATRIX_SHAPER },
	{ { "sRGB->adobeRGB" }, 1, &pipeline_adobeRGB, 1.4,  0, PTYPE_MATRIX_SHAPER },
	{ { "sRGB->BT2020" },   2, &pipeline_BT2020,   4.5,  0, PTYPE_MATRIX_SHAPER },
	{ { "sRGB->sRGB" },     0, &pipeline_sRGB,     0.0, 17, PTYPE_CLUT,         0.0005 },
	{ { "sRGB->adobeRGB" }, 1, &pipeline_adobeRGB, 1.8, 17, PTYPE_CLUT,         0.0065 },
};

static void
test_roundtrip(uint8_t r, uint8_t g, uint8_t b, cmsPipeline *pip,
	       struct rgb_diff_stat *stat)
{
	struct color_float in = { .rgb = { r / 255.0, g / 255.0, b / 255.0 } };
	struct color_float out = {};

	cmsPipelineEvalFloat(in.rgb, out.rgb, pip);
	rgb_diff_stat_update(stat, &in, &out, &in);
}

/*
 * Roundtrip verification tests that converting device -> PCS -> device
 * results in the original color values close enough.
 *
 * This ensures that the two pipelines are probably built correctly, and we
 * do not have problems with unexpected value clamping or with representing
 * (inverse) EOTF curves.
 */
static void
roundtrip_verification(cmsPipeline *DToB, cmsPipeline *BToD, float tolerance)
{
	unsigned r, g, b;
	struct rgb_diff_stat stat = {};
	cmsPipeline *pip;

	pip = cmsPipelineDup(DToB);
	cmsPipelineCat(pip, BToD);

	/*
	 * Inverse-EOTF is known to have precision problems near zero, so
	 * sample near zero densely, the rest can be more sparse to run faster.
	 */
	for (r = 0; r < 256; r += (r < 15) ? 1 : 8) {
		for (g = 0; g < 256; g += (g < 15) ? 1 : 8) {
			for (b = 0; b < 256; b += (b < 15) ? 1 : 8)
				test_roundtrip(r, g, b, pip, &stat);
		}
	}

	cmsPipelineFree(pip);

	rgb_diff_stat_print(&stat, "DToB->BToD roundtrip", 8);
	assert(stat.two_norm.max < tolerance);
}

static cmsInt32Number
sampler_matrix(const float src[], float dst[], void *cargo)
{
	const struct lcmsMAT3 *mat = cargo;
	struct color_float in = { .r = src[0], .g = src[1], .b = src[2] };
	struct color_float cf;
	unsigned i;

	cf = color_float_apply_matrix(mat, in);

	for (i = 0; i < COLOR_CHAN_NUM; i++)
		dst[i] = cf.rgb[i];

	return 1;
}

static cmsStage *
create_cLUT_from_matrix(cmsContext context_id, const struct lcmsMAT3 *mat, int dim_size)
{
	cmsStage *cLUT_stage;

	cLUT_stage = cmsStageAllocCLutFloat(context_id, dim_size, 3, 3, NULL);
	cmsStageSampleCLutFloat(cLUT_stage, sampler_matrix, (void *)mat, 0);

	return cLUT_stage;
}

/*
 * Originally the cLUT profile test attempted to use the AToB/BToA tags. Those
 * come with serious limitations though: at most uint16 representation for
 * values in a LUT which means LUT entry precision is limited and range is
 * [0.0, 1.0]. This poses difficulties such as:
 * - for AToB, the resulting PCS XYZ values may need to be > 1.0
 * - for BToA, it is easy to fall outside of device color volume meaning that
 *   out-of-range values are needed in the 3D LUT
 * Working around these could require offsetting and scaling of values
 * before and after the 3D LUT, and even that may not always be possible.
 *
 * DToB/BToD tags do not have most of these problems, because there pipelines
 * use float32 representation throughout. We have much more precision, and
 * we can mostly use negative and greater than 1.0 values. LUT elements
 * still clamp their input to [0.0, 1.0] before applying the LUT. This type of
 * pipeline is called multiProcessElement (MPE).
 *
 * MPE also allows us to represent curves in a few analytical forms. These are
 * just enough to represent the EOTF curves we have and their inverses, but
 * they do not allow encoding extended EOTF curves or their inverses
 * (defined for all real numbers by extrapolation, and mirroring for negative
 * inputs). Using MPE curves we avoid the precision problems that arise from
 * attempting to represent an inverse-EOTF as a LUT. For the precision issue,
 * see: https://gitlab.freedesktop.org/pq/color-and-hdr/-/merge_requests/9
 *
 * MPE is not a complete remedy, because 3D LUT inputs are still always clamped
 * to [0.0, 1.0]. Therefore a 3D LUT cannot represent the inverse of a matrix
 * that can produce negative or greater than 1.0 values without further tricks
 * (scaling and offsetting) in the pipeline. Rather than implementing that
 * complication, we decided to just not test with such matrices. Therefore
 * BT.2020 color space is not used in the cLUT test. AdobeRGB is enough.
 */
static cmsHPROFILE
build_lcms_clut_profile_output(cmsContext context_id,
			       const struct setup_args *arg)
{
	enum transfer_fn inv_eotf_fn = arg->pipeline->post_fn;
	enum transfer_fn eotf_fn = transfer_fn_invert(inv_eotf_fn);
	cmsHPROFILE hRGB;
	cmsPipeline *DToB0, *BToD0;
	cmsStage *stage;
	cmsStage *stage_inv_eotf;
	cmsStage *stage_eotf;
	struct lcmsMAT3 mat2XYZ_inv;

	lcmsMAT3_invert(&mat2XYZ_inv, &arg->pipeline->mat2XYZ);

	hRGB = cmsCreateProfilePlaceholder(context_id);
	cmsSetProfileVersion(hRGB, 4.3);
	cmsSetDeviceClass(hRGB, cmsSigDisplayClass);
	cmsSetColorSpace(hRGB, cmsSigRgbData);
	cmsSetPCS(hRGB, cmsSigXYZData);
	SetTextTags(hRGB, L"cLut profile");

	stage_eotf = build_MPE_curve_stage(context_id, eotf_fn);
	stage_inv_eotf = build_MPE_curve_stage(context_id, inv_eotf_fn);

	/*
	 * Pipeline from PCS (optical) to device (electrical)
	 */
	BToD0 = cmsPipelineAlloc(context_id, 3, 3);

	stage = create_cLUT_from_matrix(context_id, &mat2XYZ_inv, arg->dim_size);
	cmsPipelineInsertStage(BToD0, cmsAT_END, stage);
	cmsPipelineInsertStage(BToD0, cmsAT_END, cmsStageDup(stage_inv_eotf));

	cmsWriteTag(hRGB, cmsSigBToD0Tag, BToD0);
	cmsLinkTag(hRGB, cmsSigBToD1Tag, cmsSigBToD0Tag);
	cmsLinkTag(hRGB, cmsSigBToD2Tag, cmsSigBToD0Tag);
	cmsLinkTag(hRGB, cmsSigBToD3Tag, cmsSigBToD0Tag);

	/*
	 * Pipeline from device (electrical) to PCS (optical)
	 */
	DToB0 = cmsPipelineAlloc(context_id, 3, 3);

	cmsPipelineInsertStage(DToB0, cmsAT_END, cmsStageDup(stage_eotf));
	stage = create_cLUT_from_matrix(context_id, &arg->pipeline->mat2XYZ, arg->dim_size);
	cmsPipelineInsertStage(DToB0, cmsAT_END, stage);

	cmsWriteTag(hRGB, cmsSigDToB0Tag, DToB0);
	cmsLinkTag(hRGB, cmsSigDToB1Tag, cmsSigDToB0Tag);
	cmsLinkTag(hRGB, cmsSigDToB2Tag, cmsSigDToB0Tag);
	cmsLinkTag(hRGB, cmsSigDToB3Tag, cmsSigDToB0Tag);

	roundtrip_verification(DToB0, BToD0, arg->clut_roundtrip_tolerance);

	cmsPipelineFree(BToD0);
	cmsPipelineFree(DToB0);
	cmsStageFree(stage_eotf);
	cmsStageFree(stage_inv_eotf);

	return hRGB;
}

static cmsHPROFILE
build_lcms_matrix_shaper_profile_output(cmsContext context_id,
					const struct lcms_pipeline *pipeline)
{
	cmsToneCurve *arr_curves[3];
	cmsHPROFILE hRGB;
	int type_inverse_tone_curve;
	double inverse_tone_curve_param[5];

	assert(find_tone_curve_type(pipeline->post_fn, &type_inverse_tone_curve,
				    inverse_tone_curve_param));

	/*
	 * We are creating output profile and therefore we can use the following:
	 * calling semantics:
	 * cmsBuildParametricToneCurve(type_inverse_tone_curve, inverse_tone_curve_param)
	 * The function find_tone_curve_type sets the type of curve positive if it
	 * is tone curve and negative if it is inverse. When we create an ICC
	 * profile we should use a tone curve, the inversion is done by LCMS
	 * when the profile is used for output.
	 */

	arr_curves[0] = arr_curves[1] = arr_curves[2] =
		cmsBuildParametricToneCurve(context_id,
					    (-1) * type_inverse_tone_curve,
					    inverse_tone_curve_param);

	assert(arr_curves[0]);
	hRGB = cmsCreateRGBProfileTHR(context_id, &wp_d65,
				      &pipeline->prim_output, arr_curves);
	assert(hRGB);

	cmsFreeToneCurve(arr_curves[0]);
	return hRGB;
}

static cmsHPROFILE
build_lcms_profile_output(cmsContext context_id, const struct setup_args *arg)
{
	switch (arg->type) {
	case PTYPE_MATRIX_SHAPER:
		return build_lcms_matrix_shaper_profile_output(context_id,
							       arg->pipeline);
	case PTYPE_CLUT:
		return build_lcms_clut_profile_output(context_id, arg);
	}

	return NULL;
}

static char *
build_output_icc_profile(const struct setup_args *arg)
{
	char *profile_name = NULL;
	cmsHPROFILE profile = NULL;
	char *wd;
	int ret;
	bool saved;

	wd = realpath(".", NULL);
	assert(wd);
	if (arg->type == PTYPE_MATRIX_SHAPER)
		ret = asprintf(&profile_name, "%s/matrix-shaper-test-%s.icm", wd,
			       arg->pipeline->color_space);
	else
		ret = asprintf(&profile_name, "%s/cLUT-test-%s.icm", wd,
			       arg->pipeline->color_space);
	assert(ret > 0);

	profile = build_lcms_profile_output(NULL, arg);
	assert(profile);

	saved = cmsSaveProfileToFile(profile, profile_name);
	assert(saved);

	cmsCloseProfile(profile);
	free(wd);

	return profile_name;
}

static void
test_lcms_error_logger(cmsContext context_id,
		       cmsUInt32Number error_code,
		       const char *text)
{
	testlog("LittleCMS error: %s\n", text);
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;
	char *file_name;

	cmsSetLogErrorHandler(test_lcms_error_logger);

	compositor_setup_defaults(&setup);
	setup.renderer = RENDERER_GL;
	setup.backend = WESTON_BACKEND_HEADLESS;
	setup.width = WINDOW_WIDTH;
	setup.height = WINDOW_HEIGHT;
	setup.shell = SHELL_TEST_DESKTOP;

	file_name = build_output_icc_profile(arg);
	if (!file_name)
		return RESULT_HARD_ERROR;

	weston_ini_setup(&setup,
		cfgln("[core]"),
		cfgln("color-management=true"),
		cfgln("[output]"),
		cfgln("name=headless"),
		cfgln("icc_profile=%s", file_name));

	free(file_name);

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args, meta);

static void
gen_ramp_rgb(pixman_image_t *image, int bitwidth, int width_bar)
{
	static const int hue[][COLOR_CHAN_NUM] = {
		{ 1, 1, 1 },	/* White	*/
		{ 1, 1, 0 },	/* Yellow 	*/
		{ 0, 1, 1 },	/* Cyan 	*/
		{ 0, 1, 0 },	/* Green 	*/
		{ 1, 0, 1 },	/* Magenta 	*/
		{ 1, 0, 0 },	/* Red 		*/
		{ 0, 0, 1 },	/* Blue 	*/
	};
	const int num_hues = ARRAY_LENGTH(hue);

	struct image_header ih = image_header_from(image);
	float val_max;
	int x, y;
	int hue_index;
	int chan;
	float value;
	unsigned char r, g, b;
	uint32_t *pixel;

	float n_steps = width_bar - 1;

	val_max = (1 << bitwidth) - 1;

	for (y = 0; y < ih.height; y++) {
		hue_index = (y * num_hues) / (ih.height - 1);
		hue_index = MIN(hue_index, num_hues - 1);

		pixel = image_header_get_row_u32(&ih, y);
		for (x = 0; x < ih.width; x++, pixel++) {
			struct color_float rgb = { .rgb = { 0, 0, 0 } };

			value = (float)x / (float)(ih.width - 1);

			if (width_bar > 1)
				value = floor(value * n_steps) / n_steps;

			for (chan = 0; chan < COLOR_CHAN_NUM; chan++) {
				if (hue[hue_index][chan])
					rgb.rgb[chan] = value;
			}

			sRGB_delinearize(&rgb);

			r = round(rgb.r * val_max);
			g = round(rgb.g * val_max);
			b = round(rgb.b * val_max);

			*pixel = (255U << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

static bool
process_pipeline_comparison(const struct buffer *src_buf,
			    const struct buffer *shot_buf,
			    const struct setup_args * arg)
{
	FILE *dump = NULL;
#if 0
	/*
	 * This file can be loaded in Octave for visualization. Find the script
	 * in tests/visualization/weston_plot_rgb_diff_stat.m and call it with
	 *
	 * weston_plot_rgb_diff_stat('opaque_pixel_conversion-f05-dump.txt')
	 */
	dump = fopen_dump_file("dump");
#endif

	struct image_header ih_src = image_header_from(src_buf->image);
	struct image_header ih_shot = image_header_from(shot_buf->image);
	int y, x;
	struct color_float pix_src;
	struct color_float pix_src_pipeline;
	struct color_float pix_shot;
	struct rgb_diff_stat diffstat = { .dump = dump };
	bool ok;

	/* no point to compare different images */
	assert(ih_src.width == ih_shot.width);
	assert(ih_src.height == ih_shot.height);

	for (y = 0; y < ih_src.height; y++) {
		uint32_t *row_ptr = image_header_get_row_u32(&ih_src, y);
		uint32_t *row_ptr_shot = image_header_get_row_u32(&ih_shot, y);

		for (x = 0; x < ih_src.width; x++) {
			pix_src = a8r8g8b8_to_float(row_ptr[x]);
			pix_shot = a8r8g8b8_to_float(row_ptr_shot[x]);

			process_pixel_using_pipeline(arg->pipeline->pre_fn,
						     &arg->pipeline->mat,
						     arg->pipeline->post_fn,
						     &pix_src, &pix_src_pipeline);

			rgb_diff_stat_update(&diffstat,
					     &pix_src_pipeline, &pix_shot,
					     &pix_src);
		}
	}

	ok = diffstat.two_norm.max <= arg->tolerance / 255.0f;

	testlog("%s %s %s tolerance %f %s\n", __func__,
		ok ? "SUCCESS" : "FAILURE",
		arg->meta.name, arg->tolerance,
		arg->type == PTYPE_MATRIX_SHAPER ? "matrix-shaper" : "cLUT");

	rgb_diff_stat_print(&diffstat, __func__, 8);

	if (dump)
		fclose(dump);

	return ok;
}

/*
 * Test that opaque client pixels produce the expected output when converted
 * from the implicit sRGB input to ICC profile described output.
 *
 * The groundtruth conversion comes from the struct lcms_pipeline definitions.
 * The first error source is converting those to ICC files. The second error
 * source is Weston.
 */
TEST(opaque_pixel_conversion)
{
	int seq_no = get_test_fixture_index();
	const struct setup_args *arg = &my_setup_args[seq_no];
	const int width = WINDOW_WIDTH;
	const int height = WINDOW_HEIGHT;
	const int bitwidth = 8;
	const int width_bar = 32;

	struct client *client;
	struct buffer *buf;
	struct buffer *shot;
	struct wl_surface *surface;
	bool match;

	client = create_client_and_test_surface(0, 0, width, height);
	assert(client);
	surface = client->surface->wl_surface;

	buf = create_shm_buffer_a8r8g8b8(client, width, height);
	gen_ramp_rgb(buf->image, bitwidth, width_bar);

	wl_surface_attach(surface, buf->proxy, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	shot = capture_screenshot_of_output(client);
	assert(shot);

	match = verify_image(shot, "shaper_matrix", arg->ref_image_index,
			     NULL, seq_no);
	assert(process_pipeline_comparison(buf, shot, arg));
	assert(match);
	buffer_destroy(shot);
	buffer_destroy(buf);
	client_destroy(client);
}