#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

/* clang-format off */

#define SETTING_OPACITY                "opacity"
#define SETTING_CONTRAST               "contrast"
#define SETTING_BRIGHTNESS             "brightness"
#define SETTING_GAMMA                  "gamma"
#define SETTING_COLOR_TYPE             "key_color_type"
#define SETTING_KEY_COLOR              "key_color"
#define SETTING_HUE_ANGLE              "hue_angle"
#define SETTING_CULL_RADIUS            "cull_radius"

#define TEXT_OPACITY                   obs_module_text("Opacity")
#define TEXT_CONTRAST                  obs_module_text("Contrast")
#define TEXT_BRIGHTNESS                obs_module_text("Brightness")
#define TEXT_GAMMA                     obs_module_text("Gamma")
#define TEXT_COLOR_TYPE                obs_module_text("KeyColorType")
#define TEXT_KEY_COLOR                 obs_module_text("KeyColor")
#define TEXT_HUE_ANGLE                 obs_module_text("HueAngle")
#define TEXT_CULL_RADIUS               obs_module_text("CullRadius")

/* clang-format on */

struct chroma_key_filter_data {
	obs_source_t *context;

	gs_effect_t *effect;

	gs_eparam_t *color_param;
	gs_eparam_t *contrast_param;
	gs_eparam_t *brightness_param;
	gs_eparam_t *gamma_param;

	gs_eparam_t *chroma_param;
	gs_eparam_t *hue_angle_param;
	gs_eparam_t *cull_radius_param;

	struct vec4 color;
	float contrast;
	float brightness;
	float gamma;

	struct vec3 chroma;
	float hue_angle;
	float cull_radius;
};

static const char *chroma_key_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NewChromaKeyFilter");
}

static const float yuv_mat[16] = {0.182586f, -0.100644f, 0.439216f,  0.0f,
				  0.614231f, -0.338572f, -0.398942f, 0.0f,
				  0.062007f, 0.439216f,  -0.040274f, 0.0f,
				  0.062745f, 0.501961f,  0.501961f,  1.0f};

static inline void color_settings_update(struct chroma_key_filter_data *filter,
					 obs_data_t *settings)
{
	uint32_t opacity =
		(uint32_t)obs_data_get_int(settings, SETTING_OPACITY);
	uint32_t color = 0xFFFFFF | (((opacity * 255) / 100) << 24);
	double contrast = obs_data_get_double(settings, SETTING_CONTRAST);
	double brightness = obs_data_get_double(settings, SETTING_BRIGHTNESS);
	double gamma = obs_data_get_double(settings, SETTING_GAMMA);

	contrast = (contrast < 0.0) ? (1.0 / (-contrast + 1.0))
				    : (contrast + 1.0);

	brightness *= 0.5;

	gamma = (gamma < 0.0) ? (-gamma + 1.0) : (1.0 / (gamma + 1.0));

	filter->contrast = (float)contrast;
	filter->brightness = (float)brightness;
	filter->gamma = (float)gamma;

	vec4_from_rgba(&filter->color, color);
}

static inline void chroma_settings_update(struct chroma_key_filter_data *filter,
					  obs_data_t *settings)
{
	int64_t hue_angle = obs_data_get_int(settings, SETTING_HUE_ANGLE);
	float cull_radius =
		(float)obs_data_get_double(settings, SETTING_CULL_RADIUS);
	uint32_t key_color =
		(uint32_t)obs_data_get_int(settings, SETTING_KEY_COLOR);
	const char *key_type =
		obs_data_get_string(settings, SETTING_COLOR_TYPE);
	struct vec4 key_rgb;
	struct vec4 key_color_v4;
	struct matrix4 yuv_mat_m4;

	if (strcmp(key_type, "green") == 0)
		key_color = 0x00FF00;
	else if (strcmp(key_type, "blue") == 0)
		key_color = 0xFF9900;
	else if (strcmp(key_type, "magenta") == 0)
		key_color = 0xFF00FF;

	vec4_from_rgba(&key_rgb, key_color | 0xFF000000);

	memcpy(&yuv_mat_m4, yuv_mat, sizeof(yuv_mat));
	vec4_transform(&key_color_v4, &key_rgb, &yuv_mat_m4);
	vec3_set(&filter->chroma, key_color_v4.x, key_color_v4.y, key_color_v4.z);

	filter->hue_angle = (float)hue_angle * M_PI / 180.0f;
	filter->cull_radius = cull_radius;
}

static void chroma_key_update(void *data, obs_data_t *settings)
{
	struct chroma_key_filter_data *filter = data;

	color_settings_update(filter, settings);
	chroma_settings_update(filter, settings);
}

static void chroma_key_destroy(void *data)
{
	struct chroma_key_filter_data *filter = data;

	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
	}

	bfree(data);
}

static void *chroma_key_create(obs_data_t *settings, obs_source_t *context)
{
	struct chroma_key_filter_data *filter =
		bzalloc(sizeof(struct chroma_key_filter_data));
	char *effect_path = obs_module_file("new_chroma_key_filter.effect");

	filter->context = context;

	obs_enter_graphics();

	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	if (filter->effect) {
		filter->color_param =
			gs_effect_get_param_by_name(filter->effect, "color");
		filter->contrast_param =
			gs_effect_get_param_by_name(filter->effect, "contrast");
		filter->brightness_param = gs_effect_get_param_by_name(
			filter->effect, "brightness");
		filter->gamma_param =
			gs_effect_get_param_by_name(filter->effect, "gamma");
		filter->chroma_param = gs_effect_get_param_by_name(
			filter->effect, "chroma_key");
		filter->hue_angle_param = gs_effect_get_param_by_name(
			filter->effect, "hue_angle");
		filter->cull_radius_param = gs_effect_get_param_by_name(
			filter->effect, "cull_radius");
	}

	obs_leave_graphics();

	bfree(effect_path);

	if (!filter->effect) {
		chroma_key_destroy(filter);
		return NULL;
	}

	chroma_key_update(filter, settings);
	return filter;
}

static void chroma_key_render(void *data, gs_effect_t *effect)
{
	struct chroma_key_filter_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	if (!obs_source_process_filter_begin(filter->context, GS_RGBA,
					     OBS_ALLOW_DIRECT_RENDERING))
		return;

	gs_effect_set_vec4(filter->color_param, &filter->color);
	gs_effect_set_float(filter->contrast_param, filter->contrast);
	gs_effect_set_float(filter->brightness_param, filter->brightness);
	gs_effect_set_float(filter->gamma_param, filter->gamma);
	gs_effect_set_vec3(filter->chroma_param, &filter->chroma);
	gs_effect_set_float(filter->hue_angle_param, filter->hue_angle);
	gs_effect_set_float(filter->cull_radius_param, filter->cull_radius);

	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);

	UNUSED_PARAMETER(effect);
}

static bool key_type_changed(obs_properties_t *props, obs_property_t *p,
			     obs_data_t *settings)
{
	const char *type = obs_data_get_string(settings, SETTING_COLOR_TYPE);
	bool custom = strcmp(type, "custom") == 0;

	obs_property_set_visible(obs_properties_get(props, SETTING_KEY_COLOR),
				 custom);

	UNUSED_PARAMETER(p);
	return true;
}

static obs_properties_t *chroma_key_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, SETTING_COLOR_TYPE,
						    TEXT_COLOR_TYPE,
						    OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Green"), "green");
	obs_property_list_add_string(p, obs_module_text("Blue"), "blue");
	obs_property_list_add_string(p, obs_module_text("Magenta"), "magenta");
	obs_property_list_add_string(p, obs_module_text("Custom"), "custom");

	obs_property_set_modified_callback(p, key_type_changed);

	obs_properties_add_color(props, SETTING_KEY_COLOR, TEXT_KEY_COLOR);
	obs_properties_add_int_slider(props, SETTING_HUE_ANGLE, TEXT_HUE_ANGLE,
				      10, 80, 1);
	obs_properties_add_float_slider(props, SETTING_CULL_RADIUS,
					TEXT_CULL_RADIUS, 0.0, 1.0, 0.01);

	obs_properties_add_int_slider(props, SETTING_OPACITY, TEXT_OPACITY, 0,
				      100, 1);
	obs_properties_add_float_slider(props, SETTING_CONTRAST, TEXT_CONTRAST,
					-1.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, SETTING_BRIGHTNESS,
					TEXT_BRIGHTNESS, -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, SETTING_GAMMA, TEXT_GAMMA, -1.0,
					1.0, 0.01);

	UNUSED_PARAMETER(data);
	return props;
}

static void chroma_key_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_OPACITY, 100);
	obs_data_set_default_double(settings, SETTING_CONTRAST, 0.0);
	obs_data_set_default_double(settings, SETTING_BRIGHTNESS, 0.0);
	obs_data_set_default_double(settings, SETTING_GAMMA, 0.0);
	obs_data_set_default_int(settings, SETTING_KEY_COLOR, 0x00FF00);
	obs_data_set_default_string(settings, SETTING_COLOR_TYPE, "green");
	obs_data_set_default_int(settings, SETTING_HUE_ANGLE, 30);
	obs_data_set_default_double(settings, SETTING_CULL_RADIUS, 0.2);
}

struct obs_source_info new_chroma_key_filter = {
	.id = "new_chroma_key_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = chroma_key_name,
	.create = chroma_key_create,
	.destroy = chroma_key_destroy,
	.video_render = chroma_key_render,
	.update = chroma_key_update,
	.get_properties = chroma_key_properties,
	.get_defaults = chroma_key_defaults,
};
