/**************************************************************************/
/*  audio_effect_bus_tap.h                                                */
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
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_effect.h"

class AudioEffectBusTap;

class AudioEffectBusTapInstance : public AudioEffectInstance {
	GDCLASS(AudioEffectBusTapInstance, AudioEffectInstance);
	friend class AudioEffectBusTap;

	Ref<AudioEffectBusTap> base;
	uint32_t channel_index = 0;

public:
	virtual void process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) override;
	virtual bool process_silence() const override;
};

// Mirrors a bus into a ring so AudioStreamBusTap playbacks can replay it from
// the audio thread. Unlike AudioEffectCapture the ring has only a monotonic
// write cursor, so any number of readers consume it independently and none of
// them can starve the writer.
class AudioEffectBusTap : public AudioEffect {
	GDCLASS(AudioEffectBusTap, AudioEffect);
	friend class AudioEffectBusTapInstance;

	LocalVector<AudioFrame> ring;
	uint32_t mask = 0;
	SafeNumeric<uint64_t> write_pos;

	// Bus effects are instantiated once per bus channel. Only the first channel
	// of each round feeds the ring, otherwise channels interleave.
	SafeNumeric<uint32_t> instance_counter;

	float buffer_length_seconds = 0.25f;
	int mix_rate_at_init = 0;

	void _ensure_ring();

protected:
	static void _bind_methods();

public:
	virtual Ref<AudioEffectInstance> instantiate() override;

	void set_buffer_length(float p_buffer_length_seconds);
	float get_buffer_length() const;

	// Frames written since the tap was created; readers use this as their clock.
	uint64_t get_write_position() const { return write_pos.get(); }
	uint32_t get_capacity() const { return ring.is_empty() ? 0 : mask + 1; }

	// Copies frames [p_start, p_start + p_frames) into p_dst. Fails if that span
	// has already been overwritten.
	bool read_span(uint64_t p_start, AudioFrame *p_dst, uint32_t p_frames) const;

	int64_t get_written_frames() const;
	int get_capacity_frames() const;
};
