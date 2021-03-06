#pragma once
#include <obs-module.h>
#include <stdio.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("rematrix-filter", "en-US")

#define MT_ obs_module_text

#ifndef MAX_AUDIO_SIZE
#ifndef AUDIO_OUTPUT_FRAMES
#define	AUDIO_OUTPUT_FRAMES 1024
#endif
#define	MAX_AUDIO_SIZE (AUDIO_OUTPUT_FRAMES * sizeof(float))
#endif // !MAX_AUDIO_SIZE

/*****************************************************************************/
long long get_obs_output_channels() {
	// get channel number from output speaker layout set by obs
	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	long long recorded_channels = get_audio_channels(aoi.speakers);
	return recorded_channels;
}

/*****************************************************************************/
struct rematrix_data {
	obs_source_t *context;
	size_t channels;
	//store the routing information
	long route[MAX_AUDIO_CHANNELS];
	//store a temporary buffer
	uint8_t *tmpbuffer[MAX_AUDIO_CHANNELS];
};

/*****************************************************************************/
static const char *rematrix_name(void *unused) {
	UNUSED_PARAMETER(unused);
	return MT_("Rematrix");
}

/*****************************************************************************/
static void rematrix_destroy(void *data) {
	struct rematrix_data *rematrix = data;

	for (size_t i = 0; i < rematrix->channels; i++) {
		if (rematrix->tmpbuffer[i])
			bfree(rematrix->tmpbuffer[i]);
	}

	bfree(rematrix);
}

/*****************************************************************************/
static void rematrix_update(void *data, obs_data_t *settings) {
	struct rematrix_data *rematrix = data;

	rematrix->channels = audio_output_get_channels(obs_get_audio());

	bool route_changed = false;
	long route[MAX_AUDIO_CHANNELS];

	//make enough space for c strings
	int pad_digits = (int)floor(log10(abs(MAX_AUDIO_CHANNELS))) + 1;

	//template out the route format
	const char* route_name_format = "route %i";
	size_t route_len = strlen(route_name_format) + pad_digits;
	char* route_name = (char *)calloc(route_len, sizeof(char));

	//copy the routing over from the settings
	for (long long i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		sprintf(route_name, route_name_format, i);
		route[i] = (int)obs_data_get_int(settings, route_name);
		if (rematrix->route[i] != route[i]) {
			rematrix->route[i] = route[i];
			route_changed = true;
		}
	}

	//don't memory leak
	free(route_name);
}

/*****************************************************************************/
static void *rematrix_create(obs_data_t *settings, obs_source_t *filter) {
	struct rematrix_data *rematrix = bzalloc(sizeof(*rematrix));
	rematrix->context = filter;
	rematrix_update(rematrix, settings);

	for (size_t i = 0; i < rematrix->channels; i++) {
		rematrix->tmpbuffer[i] = bzalloc(MAX_AUDIO_SIZE);
	}

	return rematrix;
}

/*****************************************************************************/
static struct obs_audio_data *rematrix_filter_audio(void *data,
	struct obs_audio_data *audio) {

	//initialize once, optimize for fast use
	static volatile long long route[MAX_AUDIO_CHANNELS];

	struct rematrix_data *rematrix = data;
	const size_t channels = rematrix->channels;
	uint8_t **rematrixed_data = (uint8_t**)rematrix->tmpbuffer;
	uint8_t **adata = (uint8_t**)audio->data;
	size_t ch_buffer = (audio->frames * sizeof(float));

	//prevent race condition
	for (size_t c = 0; c < channels; c++)
		route[c] = rematrix->route[c];

	uint32_t frames = audio->frames;
	size_t copy_size = 0;
	size_t copy_index = 0;
	//consume AUDIO_OUTPUT_FRAMES or less # of frames
	for (size_t chunk = 0; chunk < frames; chunk+=AUDIO_OUTPUT_FRAMES) {
		//calculate the byte address we're copying to / from 
		//relative to the original data
		copy_index = chunk * sizeof(float);

		//calculate the size of the data we're about to try to copy
		if (frames - chunk < AUDIO_OUTPUT_FRAMES)
			copy_size = frames - chunk;
		else
			copy_size = AUDIO_OUTPUT_FRAMES;
		copy_size *= sizeof(float);

		//copy data to temporary buffer
		for (size_t c = 0; c < channels; c++) {
			//valid route copy data to temporary buffer
			if (route[c] >= 0 && route[c] < channels)
				memcpy(rematrixed_data[c], 
					&adata[route[c]][copy_index],
					copy_size);
			//not a valid route, mute
			else
				memset(rematrixed_data[c], 0, MAX_AUDIO_SIZE);
		}

		//memcpy data back into place
		for (size_t c = 0; c < channels; c++) {
			memcpy(&adata[c][copy_index], rematrixed_data[c],
				copy_size);
		}
		//move to next chunk of unprocessed data
	}
	return audio;
}

/*****************************************************************************/
static void rematrix_defaults(obs_data_t *settings)
{
	//make enough space for c strings
	int pad_digits = (int)floor(log10(abs(MAX_AUDIO_CHANNELS))) + 1;

	//template out the route format
	const char* route_name_format = "route %i";
	size_t route_len = strlen(route_name_format) + pad_digits;
	char* route_name = (char *)calloc(route_len, sizeof(char));

	//default is no routing (ordered) -1 or any out of bounds is mute*
	for (long long i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		sprintf(route_name, route_name_format, i);
		obs_data_set_default_int(settings, route_name, i);
	}

	//don't memory leak
	free(route_name);
}

/*****************************************************************************/
static bool fill_out_channels(obs_properties_t *props, obs_property_t *list,
	obs_data_t *settings) {
	
	obs_property_list_clear(list);
	obs_property_list_add_int(list, MT_("mute"), -1);
	long long channels = get_obs_output_channels();

	//make enough space for c strings
	int pad_digits = (int)floor(log10(abs(MAX_AUDIO_CHANNELS))) + 1;

	//template out the format for the json
	const char* route_obs_format = "in.ch.%i";
	size_t route_obs_len = strlen(route_obs_format) + pad_digits;
	char* route_obs = (char *)calloc(route_obs_len, sizeof(char));

	for (long long c = 0; c < channels; c++) {
		sprintf(route_obs, route_obs_format, c);
		obs_property_list_add_int(list, MT_(route_obs) , c);
	}

	//don't memory leak
	free(route_obs);

	return true;
}

/*****************************************************************************/
static obs_properties_t *rematrix_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	//make a list long enough for the maximum # of chs
	obs_property_t *route[MAX_AUDIO_CHANNELS];
	
	size_t channels = audio_output_get_channels(obs_get_audio());

	//make enough space for c strings
	int pad_digits = (int)floor(log10(abs(MAX_AUDIO_CHANNELS))) + 1;

	//template out the route format
	const char* route_name_format = "route %i";
	size_t route_len = strlen(route_name_format) + pad_digits;
	char* route_name = (char *)calloc(route_len, sizeof(char));

	//template out the format for the json
	const char* route_obs_format = "out.ch.%i";
	size_t route_obs_len = strlen(route_obs_format) + pad_digits;
	char* route_obs = (char *)calloc(route_obs_len, sizeof(char));

	//add an appropriate # of options to mix from
	for (size_t i = 0; i < channels; i++) {
		sprintf(route_name, route_name_format, i);
		sprintf(route_obs, route_obs_format, i);
		route[i] = obs_properties_add_list(props, route_name,
		    MT_(route_obs), OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);

		obs_property_set_long_description(route[i],
		    MT_("tooltip"));

		obs_property_set_modified_callback(route[i],
		    fill_out_channels);
	}

	//don't memory leak
	free(route_name);
	free(route_obs);

	return props;
}

/*****************************************************************************/
bool obs_module_load(void)
{
	struct obs_source_info rematrixer_filter = {
		.id = "rematrix_filter",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_AUDIO,
		.get_name = rematrix_name,
		.create = rematrix_create,
		.destroy = rematrix_destroy,
		.update = rematrix_update,
		.filter_audio = rematrix_filter_audio,
		.get_defaults = rematrix_defaults,
		.get_properties = rematrix_properties,
	};

	obs_register_source(&rematrixer_filter);
	return true;
}
