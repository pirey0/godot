/**************************************************************************/
/*  harness.h                                                             */
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
#include "core/object/ref_counted.h"

// Drives the auto-generated transpiled Data functions against a live Data object
// and compares/times them versus the interpreted versions.
class Gds2cppHarness : public RefCounted {
	GDCLASS(Gds2cppHarness, RefCounted);

protected:
	static void _bind_methods();

public:
	// Run a transpiled function by name on p_data (a live GDScript object), returning
	// its result. Mutating functions affect p_data's real members, same as interpreted.
	Variant run(Object *p_data, const String &p_func, const Array &p_args);

	// Time interpreted vs transpiled for n iterations; returns a report dictionary.
	Dictionary bench(Object *p_data, const String &p_func, const Array &p_args, int n);

	// Diagnostic: invoke validated builtin method #idx of p_func on base with args.
	Variant probe_bm(Object *p_data, const String &p_func, int idx, const Variant &base, const Array &args);

	// Three-way: interpreted vs faithful-transpiled vs type-specialized; returns report.
	Dictionary bench3(Object *p_data, const String &p_func, const Array &p_args, int n);

	// --- Live dispatch (run the real game through the transpiled bodies) ---
	// Install the compiled-in transpiled bodies onto p_obj's GDScript so the engine
	// dispatches to them. Only methods whose names match are installed; safe to call on
	// any script. Returns the number of scripts whose functions were bound (0 or 1).
	int install(Object *p_obj);
	// Global on/off: when true, GDScriptFunction::call() routes any function with an
	// installed transpiled body to the C++ version instead of the interpreter.
	void set_enabled(bool p_on);
	bool is_enabled() const;

	// Whole-program: install transpiled bodies for EVERY script in the compiled-in
	// wp/ set (gds2cpp_bind_all loads each by path and binds). Returns 1 if the
	// whole-program output was compiled in, 0 otherwise. Call once after scripts load.
	int bind_all();
};
