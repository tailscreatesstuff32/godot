/*************************************************************************/
/*  audio_stream_ogg_vorbis.cpp                                          */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2017 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2017 Godot Engine contributors (cf. AUTHORS.md)    */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/
#include "audio_stream_ogg_vorbis.h"

#include "os/file_access.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "thirdparty/misc/stb_vorbis.c"
#pragma GCC diagnostic pop

#ifndef CLAMP
#define CLAMP(m_a, m_min, m_max) (((m_a) < (m_min)) ? (m_min) : (((m_a) > (m_max)) ? m_max : m_a))
#endif

void AudioStreamPlaybackOGGVorbis::_mix_internal(AudioFrame *p_buffer, int p_frames) {

	ERR_FAIL_COND(!active);

	int todo = p_frames;

	int start_buffer = 0;

	while (todo > 0 && active) {
		float *buffer = (float *)p_buffer;
		if (start_buffer > 0) {
			buffer = (buffer + start_buffer * 2);
		}
		int mixed = stb_vorbis_get_samples_float_interleaved(ogg_stream, 2, buffer, todo * 2);
		if (vorbis_stream->channels == 1 && mixed > 0) {
			//mix mono to stereo
			for (int i = start_buffer; i < mixed; i++) {
				p_buffer[i].r = p_buffer[i].l;
			}
		}
		todo -= mixed;
		frames_mixed += mixed;

		if (todo > 0) {
			//end of file!
			if (vorbis_stream->loop) {
				//loop to the loop_beginning
				seek(vorbis_stream->loop_begin);
				loops++;
				// we still have buffer to fill, start from this element in the next iteration.
				start_buffer = p_frames - todo;
			} else {
				for (int i = p_frames - todo; i < p_frames; i++) {
					p_buffer[i] = AudioFrame(0, 0);
				}
				active = false;
				todo = 0;
			}
		} else if (vorbis_stream->loop && frames_mixed >= vorbis_stream->loop_end_frames) {
			// We reached loop_end. Loop to loop_begin plus whatever extra length we already mixed
			uint32_t frames_to_advance = uint32_t(frames_mixed - vorbis_stream->loop_end_frames);
			float start_loop = vorbis_stream->loop_begin + (float(frames_to_advance) / vorbis_stream->sample_rate);
			seek(start_loop);
			loops++;
		}
	}
}

float AudioStreamPlaybackOGGVorbis::get_stream_sampling_rate() {

	return vorbis_stream->sample_rate;
}

void AudioStreamPlaybackOGGVorbis::start(float p_from_pos) {

	active = true;
	seek(p_from_pos);
	loops = 0;
	_begin_resample();
}

void AudioStreamPlaybackOGGVorbis::stop() {

	active = false;
}
bool AudioStreamPlaybackOGGVorbis::is_playing() const {

	return active;
}

int AudioStreamPlaybackOGGVorbis::get_loop_count() const {

	return loops;
}

float AudioStreamPlaybackOGGVorbis::get_playback_position() const {

	return float(frames_mixed) / vorbis_stream->sample_rate;
}
void AudioStreamPlaybackOGGVorbis::seek(float p_time) {

	if (!active)
		return;

	if (p_time >= get_length()) {
		p_time = 0;
	}
	frames_mixed = uint32_t(vorbis_stream->sample_rate * p_time);

	stb_vorbis_seek(ogg_stream, frames_mixed);
}

float AudioStreamPlaybackOGGVorbis::get_length() const {

	return vorbis_stream->length;
}

AudioStreamPlaybackOGGVorbis::~AudioStreamPlaybackOGGVorbis() {
	if (ogg_alloc.alloc_buffer) {
		stb_vorbis_close(ogg_stream);
		AudioServer::get_singleton()->audio_data_free(ogg_alloc.alloc_buffer);
	}
}

Ref<AudioStreamPlayback> AudioStreamOGGVorbis::instance_playback() {

	Ref<AudioStreamPlaybackOGGVorbis> ovs;

	ERR_FAIL_COND_V(data == NULL, ovs);

	ovs.instance();
	ovs->vorbis_stream = Ref<AudioStreamOGGVorbis>(this);
	ovs->ogg_alloc.alloc_buffer = (char *)AudioServer::get_singleton()->audio_data_alloc(decode_mem_size);
	ovs->ogg_alloc.alloc_buffer_length_in_bytes = decode_mem_size;
	ovs->frames_mixed = 0;
	ovs->active = false;
	ovs->loops = 0;
	int error;
	ovs->ogg_stream = stb_vorbis_open_memory((const unsigned char *)data, data_len, &error, &ovs->ogg_alloc);
	if (!ovs->ogg_stream) {

		AudioServer::get_singleton()->audio_data_free(ovs->ogg_alloc.alloc_buffer);
		ovs->ogg_alloc.alloc_buffer = NULL;
		ERR_FAIL_COND_V(!ovs->ogg_stream, Ref<AudioStreamPlaybackOGGVorbis>());
	}

	return ovs;
}

String AudioStreamOGGVorbis::get_stream_name() const {

	return ""; //return stream_name;
}

void AudioStreamOGGVorbis::set_data(const PoolVector<uint8_t> &p_data) {

	int src_data_len = p_data.size();
#define MAX_TEST_MEM (1 << 20)

	uint32_t alloc_try = 1024;
	PoolVector<char> alloc_mem;
	PoolVector<char>::Write w;
	stb_vorbis *ogg_stream = NULL;
	stb_vorbis_alloc ogg_alloc;

	while (alloc_try < MAX_TEST_MEM) {

		alloc_mem.resize(alloc_try);
		w = alloc_mem.write();

		ogg_alloc.alloc_buffer = w.ptr();
		ogg_alloc.alloc_buffer_length_in_bytes = alloc_try;

		PoolVector<uint8_t>::Read src_datar = p_data.read();

		int error;
		ogg_stream = stb_vorbis_open_memory((const unsigned char *)src_datar.ptr(), src_data_len, &error, &ogg_alloc);

		if (!ogg_stream && error == VORBIS_outofmem) {
			w = PoolVector<char>::Write();
			alloc_try *= 2;
		} else {

			ERR_FAIL_COND(alloc_try == MAX_TEST_MEM);
			ERR_FAIL_COND(ogg_stream == NULL);

			stb_vorbis_info info = stb_vorbis_get_info(ogg_stream);

			channels = info.channels;
			sample_rate = info.sample_rate;
			decode_mem_size = alloc_try;
			//does this work? (it's less mem..)
			//decode_mem_size = ogg_alloc.alloc_buffer_length_in_bytes + info.setup_memory_required + info.temp_memory_required + info.max_frame_size;

			//print_line("succeeded "+itos(ogg_alloc.alloc_buffer_length_in_bytes)+" setup "+itos(info.setup_memory_required)+" setup temp "+itos(info.setup_temp_memory_required)+" temp "+itos(info.temp_memory_required)+" maxframe"+itos(info.max_frame_size));

			length = stb_vorbis_stream_length_in_seconds(ogg_stream);
			if (loop_end == 0) {
				set_loop_end(length);
			}
			stb_vorbis_close(ogg_stream);

			data = AudioServer::get_singleton()->audio_data_alloc(src_data_len, src_datar.ptr());
			data_len = src_data_len;

			break;
		}
	}
}

PoolVector<uint8_t> AudioStreamOGGVorbis::get_data() const {

	PoolVector<uint8_t> vdata;

	if (data_len && data) {
		vdata.resize(data_len);
		{
			PoolVector<uint8_t>::Write w = vdata.write();
			copymem(w.ptr(), data, data_len);
		}
	}

	return vdata;
}

void AudioStreamOGGVorbis::set_loop(bool p_enable) {
	loop = p_enable;
}

bool AudioStreamOGGVorbis::has_loop() const {

	return loop;
}

void AudioStreamOGGVorbis::set_loop_begin(float p_seconds) {
	p_seconds = CLAMP(p_seconds, 0, length);
	loop_begin = p_seconds;
	loop_begin_frames = uint32_t(sample_rate * p_seconds);
}

float AudioStreamOGGVorbis::get_loop_begin() const {
	return loop_begin;
}

void AudioStreamOGGVorbis::set_loop_end(float p_seconds) {
	p_seconds = CLAMP(p_seconds, 0, length);
	loop_end = p_seconds;
	loop_end_frames = uint32_t(sample_rate * p_seconds);
}

float AudioStreamOGGVorbis::get_loop_end() const {
	return loop_end;
}

void AudioStreamOGGVorbis::_bind_methods() {

	ClassDB::bind_method(D_METHOD("set_data", "data"), &AudioStreamOGGVorbis::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &AudioStreamOGGVorbis::get_data);

	ClassDB::bind_method(D_METHOD("set_loop", "enable"), &AudioStreamOGGVorbis::set_loop);
	ClassDB::bind_method(D_METHOD("has_loop"), &AudioStreamOGGVorbis::has_loop);

	ClassDB::bind_method(D_METHOD("set_loop_begin", "seconds"), &AudioStreamOGGVorbis::set_loop_begin);
	ClassDB::bind_method(D_METHOD("get_loop_begin"), &AudioStreamOGGVorbis::get_loop_begin);

	ClassDB::bind_method(D_METHOD("set_loop_end", "seconds"), &AudioStreamOGGVorbis::set_loop_end);
	ClassDB::bind_method(D_METHOD("get_loop_end"), &AudioStreamOGGVorbis::get_loop_end);

	ADD_PROPERTY(PropertyInfo(Variant::POOL_BYTE_ARRAY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_loop", "has_loop");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "loop_begin", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_loop_begin", "get_loop_begin");
	ADD_PROPERTY(PropertyInfo(Variant::REAL, "loop_end", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NOEDITOR), "set_loop_end", "get_loop_end");
}

AudioStreamOGGVorbis::AudioStreamOGGVorbis() {

	data = NULL;
	length = 0;
	sample_rate = 1;
	channels = 1;
	loop_begin = 0;
	loop_end = 0;
	loop_begin_frames = 0;
	loop_end_frames = 0;
	decode_mem_size = 0;
	loop = true;
}
