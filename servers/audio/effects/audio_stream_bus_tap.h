/**************************************************************************/
/*  audio_stream_bus_tap.h                                                */
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

#pragma once

#include "core/math/audio_frame.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_stream.h"
#include "servers/audio/effects/audio_effect_bus_tap.h"

// Replays an AudioEffectBusTap through an ordinary AudioStreamPlayer/2D/3D, so
// a bus can be spatialised without routing its audio through a script.
//
// AudioServer mixes every playback before it runs bus effects, so a reader is
// always exactly one mix block behind the tap regardless of framerate or audio
// device period. Assign one stream per player: the DSP parameters live here,
// while the tap itself is shared freely.
class AudioStreamBusTap : public AudioStream {
	GDCLASS(AudioStreamBusTap, AudioStream);

	friend class AudioStreamBusTapPlayback;

	Ref<AudioEffectBusTap> tap;
	float ring_mod_hz = 0.0f;
	int bitcrush = 1;

	// Extra lag beyond the unavoidable one mix block, in frames.
	int extra_latency_frames = 0;

protected:
	static void _bind_methods();

public:
	void set_tap(const Ref<AudioEffectBusTap> &p_tap);
	Ref<AudioEffectBusTap> get_tap() const;

	void set_ring_mod_hz(float p_hz);
	float get_ring_mod_hz() const;

	void set_bitcrush(int p_bitcrush);
	int get_bitcrush() const;

	void set_extra_latency_frames(int p_frames);
	int get_extra_latency_frames() const;

	virtual Ref<AudioStreamPlayback> instantiate_playback() override;
	virtual String get_stream_name() const override;
	virtual double get_length() const override;
	virtual bool is_monophonic() const override;
};

class AudioStreamBusTapPlayback : public AudioStreamPlayback {
	GDCLASS(AudioStreamBusTapPlayback, AudioStreamPlayback);
	friend class AudioStreamBusTap;

	Ref<AudioStreamBusTap> stream;
	Ref<AudioEffectBusTap> tap;

	bool active = false;
	uint64_t read_pos = 0;
	double mixed = 0.0;

	double phase = 0.0;
	AudioFrame hold = AudioFrame(0, 0);
	int crush_counter = 0;

	SafeNumeric<uint64_t> underrun_frames;
	SafeNumeric<uint64_t> resyncs;

	void _resync(uint64_t p_write_pos);

protected:
	static void _bind_methods();

public:
	virtual void start(double p_from_pos = 0.0) override;
	virtual void stop() override;
	virtual bool is_playing() const override;

	virtual int get_loop_count() const override;

	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;

	virtual int mix(AudioFrame *p_buffer, float p_rate_scale, int p_frames) override;

	int64_t get_underrun_frames() const;
	int64_t get_resync_count() const;
};
