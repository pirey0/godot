/**************************************************************************/
/*  audio_effect_bus_tap.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "audio_effect_bus_tap.h"

#include "servers/audio/audio_server.h"

void AudioEffectBusTap::set_buffer_length(float p_buffer_length_seconds) {
	buffer_length_seconds = MAX(p_buffer_length_seconds, 0.05f);
}

float AudioEffectBusTap::get_buffer_length() const {
	return buffer_length_seconds;
}

void AudioEffectBusTap::_ensure_ring() {
	const int rate = (int)AudioServer::get_singleton()->get_mix_rate();

	// Resize when the output device changes rate mid-session; AudioEffectCapture
	// latches its buffer on first use and never does this.
	if (!ring.is_empty() && rate == mix_rate_at_init) {
		return;
	}

	const uint32_t frames = next_power_of_2((uint32_t)(rate * buffer_length_seconds));
	ERR_FAIL_COND(frames == 0);

	ring.resize(frames);
	for (uint32_t i = 0; i < frames; i++) {
		ring[i] = AudioFrame(0, 0);
	}
	mask = frames - 1;
	mix_rate_at_init = rate;
	write_pos.set(0);
	instance_counter.set(0);
}

Ref<AudioEffectInstance> AudioEffectBusTap::instantiate() {
	_ensure_ring();

	Ref<AudioEffectBusTapInstance> ins;
	ins.instantiate();
	ins->base = Ref<AudioEffectBusTap>(this);

	const uint32_t channels = MAX(AudioServer::get_singleton()->get_channel_count(), 1);
	ins->channel_index = (instance_counter.postincrement()) % channels;

	return ins;
}

bool AudioEffectBusTap::read_span(uint64_t p_start, AudioFrame *p_dst, uint32_t p_frames) const {
	if (ring.is_empty() || p_frames == 0) {
		return false;
	}

	const uint64_t w = write_pos.get();
	const uint32_t capacity = mask + 1;

	if (p_start + p_frames > w || w - p_start > capacity) {
		return false;
	}

	for (uint32_t i = 0; i < p_frames; i++) {
		p_dst[i] = ring[(uint32_t)((p_start + i) & mask)];
	}
	return true;
}

int64_t AudioEffectBusTap::get_written_frames() const {
	return (int64_t)write_pos.get();
}

int AudioEffectBusTap::get_capacity_frames() const {
	return (int)get_capacity();
}

void AudioEffectBusTap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_buffer_length", "buffer_length_seconds"), &AudioEffectBusTap::set_buffer_length);
	ClassDB::bind_method(D_METHOD("get_buffer_length"), &AudioEffectBusTap::get_buffer_length);
	ClassDB::bind_method(D_METHOD("get_written_frames"), &AudioEffectBusTap::get_written_frames);
	ClassDB::bind_method(D_METHOD("get_capacity_frames"), &AudioEffectBusTap::get_capacity_frames);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "buffer_length", PROPERTY_HINT_RANGE, "0.05,2.0,0.01,suffix:s"), "set_buffer_length", "get_buffer_length");
}

void AudioEffectBusTapInstance::process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) {
	// The tap is transparent: the bus keeps flowing to its send as usual.
	for (int i = 0; i < p_frame_count; i++) {
		p_dst_frames[i] = p_src_frames[i];
	}

	if (base.is_null() || channel_index != 0 || base->ring.is_empty()) {
		return;
	}

	const uint64_t w = base->write_pos.get();
	for (int i = 0; i < p_frame_count; i++) {
		base->ring[(uint32_t)((w + (uint64_t)i) & base->mask)] = p_src_frames[i];
	}
	base->write_pos.set(w + (uint64_t)p_frame_count);
}

bool AudioEffectBusTapInstance::process_silence() const {
	return true;
}
