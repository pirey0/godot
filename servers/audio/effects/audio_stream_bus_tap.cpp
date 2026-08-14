/**************************************************************************/
/*  audio_stream_bus_tap.cpp                                              */
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

#include "audio_stream_bus_tap.h"

#include "core/math/math_funcs.h"
#include "servers/audio/audio_server.h"

void AudioStreamBusTap::set_tap(const Ref<AudioEffectBusTap> &p_tap) {
	tap = p_tap;
}

Ref<AudioEffectBusTap> AudioStreamBusTap::get_tap() const {
	return tap;
}

void AudioStreamBusTap::set_ring_mod_hz(float p_hz) {
	ring_mod_hz = p_hz;
}

float AudioStreamBusTap::get_ring_mod_hz() const {
	return ring_mod_hz;
}

void AudioStreamBusTap::set_bitcrush(int p_bitcrush) {
	bitcrush = MAX(p_bitcrush, 1);
}

int AudioStreamBusTap::get_bitcrush() const {
	return bitcrush;
}

void AudioStreamBusTap::set_extra_latency_frames(int p_frames) {
	extra_latency_frames = MAX(p_frames, 0);
}

int AudioStreamBusTap::get_extra_latency_frames() const {
	return extra_latency_frames;
}

Ref<AudioStreamPlayback> AudioStreamBusTap::instantiate_playback() {
	Ref<AudioStreamBusTapPlayback> playback;
	playback.instantiate();
	playback->stream = Ref<AudioStreamBusTap>(this);
	playback->tap = tap;
	return playback;
}

String AudioStreamBusTap::get_stream_name() const {
	return "BusTap";
}

double AudioStreamBusTap::get_length() const {
	return 0.0;
}

bool AudioStreamBusTap::is_monophonic() const {
	return true;
}

void AudioStreamBusTap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tap", "tap"), &AudioStreamBusTap::set_tap);
	ClassDB::bind_method(D_METHOD("get_tap"), &AudioStreamBusTap::get_tap);
	ClassDB::bind_method(D_METHOD("set_ring_mod_hz", "hz"), &AudioStreamBusTap::set_ring_mod_hz);
	ClassDB::bind_method(D_METHOD("get_ring_mod_hz"), &AudioStreamBusTap::get_ring_mod_hz);
	ClassDB::bind_method(D_METHOD("set_bitcrush", "bitcrush"), &AudioStreamBusTap::set_bitcrush);
	ClassDB::bind_method(D_METHOD("get_bitcrush"), &AudioStreamBusTap::get_bitcrush);
	ClassDB::bind_method(D_METHOD("set_extra_latency_frames", "frames"), &AudioStreamBusTap::set_extra_latency_frames);
	ClassDB::bind_method(D_METHOD("get_extra_latency_frames"), &AudioStreamBusTap::get_extra_latency_frames);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tap", PROPERTY_HINT_RESOURCE_TYPE, "AudioEffectBusTap"), "set_tap", "get_tap");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ring_mod_hz", PROPERTY_HINT_RANGE, "-4000,4000,0.1,suffix:Hz"), "set_ring_mod_hz", "get_ring_mod_hz");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bitcrush", PROPERTY_HINT_RANGE, "1,32,1"), "set_bitcrush", "get_bitcrush");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "extra_latency_frames", PROPERTY_HINT_RANGE, "0,8192,1"), "set_extra_latency_frames", "get_extra_latency_frames");
}

////////////////

void AudioStreamBusTapPlayback::_resync(uint64_t p_write_pos) {
	const uint64_t target = stream.is_valid() ? (uint64_t)stream->get_extra_latency_frames() : 0;
	read_pos = p_write_pos > target ? p_write_pos - target : 0;
}

void AudioStreamBusTapPlayback::start(double p_from_pos) {
	active = true;
	mixed = 0.0;
	phase = 0.0;
	crush_counter = 0;
	hold = AudioFrame(0, 0);
	underrun_frames.set(0);
	resyncs.set(0);

	// Start level with the writer. The fixed one-block lag that makes this
	// glitch-free comes for free from the mix order: every playback is mixed
	// before bus effects run, so this reader is always exactly one block behind
	// the tap no matter the framerate or device period.
	_resync(tap.is_valid() ? tap->get_write_position() : 0);
}

void AudioStreamBusTapPlayback::stop() {
	active = false;
}

bool AudioStreamBusTapPlayback::is_playing() const {
	return active;
}

int AudioStreamBusTapPlayback::get_loop_count() const {
	return 0;
}

double AudioStreamBusTapPlayback::get_playback_position() const {
	return mixed;
}

void AudioStreamBusTapPlayback::seek(double p_time) {
	// A live bus tap has nothing to seek to.
}

int AudioStreamBusTapPlayback::mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) {
	// p_rate_scale (the player's pitch_scale) is deliberately ignored. Honoring
	// it would mean resampling, which decouples this reader from the tap's
	// writer and brings back the drift this class exists to remove. Pitch the
	// sources feeding the tapped bus instead.
	if (p_frames <= 0) {
		return p_frames;
	}

	const uint32_t frames = (uint32_t)p_frames;
	uint32_t n = 0;

	if (active && tap.is_valid()) {
		const uint64_t w = tap->get_write_position();
		const uint32_t capacity = tap->get_capacity();

		if (capacity > 0) {
			// Reader fell outside the window (player was paused, device changed,
			// tap re-created). Jump level with the writer rather than replaying
			// stale audio.
			if (read_pos > w || (w - read_pos) > capacity) {
				_resync(w);
				resyncs.increment();
			}

			const uint64_t avail = w > read_pos ? w - read_pos : 0;
			n = avail < (uint64_t)frames ? (uint32_t)avail : frames;

			if (n > 0 && !tap->read_span(read_pos, p_buffer, n)) {
				n = 0;
			}
			read_pos += n;
		}
	}

	for (uint32_t i = n; i < frames; i++) {
		p_buffer[i] = AudioFrame(0, 0);
	}
	if (n < frames) {
		underrun_frames.add((uint64_t)(frames - n));
	}

	const double rate = AudioServer::get_singleton()->get_mix_rate();
	const double safe_rate = rate > 0.0 ? rate : 48000.0;

	if (stream.is_valid()) {
		const float hz = stream->get_ring_mod_hz();
		if (hz != 0.0f) {
			const double increment = Math::TAU * (double)hz / safe_rate;
			for (uint32_t i = 0; i < frames; i++) {
				const float mono = (p_buffer[i].left + p_buffer[i].right) * 0.5f;
				const float shifted = mono * (float)Math::cos(phase);
				p_buffer[i] = AudioFrame(shifted, shifted);
				phase += increment;
			}
			phase = Math::fmod(phase, Math::TAU);
		}

		const int crush = stream->get_bitcrush();
		if (crush > 1) {
			for (uint32_t i = 0; i < frames; i++) {
				if (crush_counter == 0) {
					hold = p_buffer[i];
				}
				p_buffer[i] = hold;
				crush_counter = (crush_counter + 1) % crush;
			}
		} else {
			crush_counter = 0;
		}
	}

	mixed += (double)frames / safe_rate;

	// Always report a full buffer: a short return makes AudioServer fade the
	// playback out and delete it.
	return p_frames;
}

int64_t AudioStreamBusTapPlayback::get_underrun_frames() const {
	return (int64_t)underrun_frames.get();
}

int64_t AudioStreamBusTapPlayback::get_resync_count() const {
	return (int64_t)resyncs.get();
}

void AudioStreamBusTapPlayback::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_underrun_frames"), &AudioStreamBusTapPlayback::get_underrun_frames);
	ClassDB::bind_method(D_METHOD("get_resync_count"), &AudioStreamBusTapPlayback::get_resync_count);
}
