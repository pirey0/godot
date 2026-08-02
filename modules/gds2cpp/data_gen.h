/**************************************************************************/
/*  data_gen.h                                                            */
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

// ============================================================================
//  GENERATED (faithful mode) from: systems/data/Data.gd
//  gds2cpp: faithful GDScript->C++ proof-of-concept. Do not edit by hand — regenerate.
//  Faithful mode: values are Variant; calls between generated functions are
//  direct C++ calls (no interpreter dispatch, no name lookup).
// ============================================================================

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class Gds2cppData : public RefCounted {
	GDCLASS(Gds2cppData, RefCounted);

	// GDScript: var values:Dictionary
	Dictionary values;

	// Sink to defeat dead-code elimination in the benchmark loop.
	Variant _bench_sink;

protected:
	static void _bind_methods();

public:
	void set_values(const Dictionary &p_values) { values = p_values; }
	Dictionary get_values() const { return values; }

	// Transpiled GDScript function.
	Variant of_or(const Variant &property, const Variant &p_default);

	// PoC harness: run `of_or` `n` times natively (direct C++ calls), return elapsed usec.
	uint64_t bench_native(int n, const Variant &property, const Variant &p_default);
};
