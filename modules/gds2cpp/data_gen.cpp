/**************************************************************************/
/*  data_gen.cpp                                                          */
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

#include "data_gen.h"

#include "core/os/os.h"
#include "core/string/ustring.h"

// ---- systems/data/Data.gd ----
// func ofOr(property, default):
//     # Fast path: keys are stored (and passed) lowercase, so skip the per-access to_lower alloc.
//     if values.has(property):
//         return values[property]
//     return values.get(property.to_lower(), default)
Variant Gds2cppData::of_or(const Variant &property, const Variant &p_default) {
	// if values.has(property):
	if (values.has(property)) {
		// return values[property]
		return values[property];
	}
	// return values.get(property.to_lower(), default)
	return values.get(String(property).to_lower(), p_default);
}

// ---- PoC benchmark harness (not part of Data.gd) ----
uint64_t Gds2cppData::bench_native(int n, const Variant &property, const Variant &p_default) {
	Variant sink;
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	for (int i = 0; i < n; i++) {
		// Direct C++ call to the transpiled function — no dispatch, no name lookup.
		sink = of_or(property, p_default);
	}
	const uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - t0;
	_bench_sink = sink; // prevent the loop from being optimized away
	return elapsed;
}

void Gds2cppData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("of_or", "property", "default"), &Gds2cppData::of_or);
	ClassDB::bind_method(D_METHOD("bench_native", "n", "property", "default"), &Gds2cppData::bench_native);
	ClassDB::bind_method(D_METHOD("set_values", "values"), &Gds2cppData::set_values);
	ClassDB::bind_method(D_METHOD("get_values"), &Gds2cppData::get_values);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "values"), "set_values", "get_values");
}
