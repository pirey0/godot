/**************************************************************************/
/*  harness.cpp                                                           */
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

#include "harness.h"

#include "core/os/os.h"
#include "data_gen_auto.h"
#include "data_spec.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"
#include "optest_gen.h"

// Try each generated translation unit's lookup (same Fn signature).
static Data_gen::Fn _find_fn(const StringName &n) {
	if (Data_gen::Fn f = Data_gen::lookup(n)) {
		return f;
	}
	return OpTest_gen::lookup(n);
}

// Resolve the GDScriptInstance + GDScriptFunction for a live GDScript object.
static bool _resolve(Object *p_data, const StringName &p_func, GDScriptInstance *&r_inst, GDScriptFunction *&r_gf) {
	r_inst = static_cast<GDScriptInstance *>(p_data->get_script_instance());
	if (!r_inst) {
		return false;
	}
	Ref<GDScript> gds = p_data->get_script();
	if (gds.is_null()) {
		return false;
	}
	const HashMap<StringName, GDScriptFunction *> &fns = gds->get_member_functions();
	if (!fns.has(p_func)) {
		return false;
	}
	r_gf = fns[p_func];
	return true;
}

Variant Gds2cppHarness::run(Object *p_data, const String &p_func, const Array &p_args) {
	StringName fname(p_func);
	Data_gen::Fn f = _find_fn(fname);
	GDScriptInstance *inst = nullptr;
	GDScriptFunction *gf = nullptr;
	if (!f || !_resolve(p_data, fname, inst, gf)) {
		return Variant();
	}
	Variant valbuf[8];
	const Variant *argp[8];
	int argc = MIN(p_args.size(), 8);
	for (int i = 0; i < argc; i++) {
		valbuf[i] = p_args[i];
		argp[i] = &valbuf[i];
	}
	return f(inst, gf, argp, argc);
}

Dictionary Gds2cppHarness::bench(Object *p_data, const String &p_func, const Array &p_args, int n) {
	Dictionary out;
	StringName fname(p_func);
	Data_gen::Fn f = _find_fn(fname);
	GDScriptInstance *inst = nullptr;
	GDScriptFunction *gf = nullptr;
	if (!f || !_resolve(p_data, fname, inst, gf)) {
		out["error"] = "no transpiled function or bad object for " + p_func;
		return out;
	}
	Variant valbuf[8];
	const Variant *argp[8];
	int argc = MIN(p_args.size(), 8);
	for (int i = 0; i < argc; i++) {
		valbuf[i] = p_args[i];
		argp[i] = &valbuf[i];
	}

	// Correctness (single call each).
	Callable::CallError ce;
	Variant interp_res = p_data->callp(fname, argp, argc, ce);
	Variant trans_res = f(inst, gf, argp, argc);

	// Timing.
	uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	for (int i = 0; i < n; i++) {
		Callable::CallError e;
		p_data->callp(fname, argp, argc, e);
	}
	uint64_t interp_us = OS::get_singleton()->get_ticks_usec() - t0;

	t0 = OS::get_singleton()->get_ticks_usec();
	Variant sink;
	for (int i = 0; i < n; i++) {
		sink = f(inst, gf, argp, argc);
	}
	uint64_t trans_us = OS::get_singleton()->get_ticks_usec() - t0;
	(void)sink;

	out["interp_result"] = interp_res;
	out["trans_result"] = trans_res;
	out["match"] = interp_res == trans_res;
	out["interp_us"] = (int64_t)interp_us;
	out["trans_us"] = (int64_t)trans_us;
	out["speedup"] = trans_us > 0 ? (double)interp_us / (double)trans_us : 0.0;
	return out;
}

Variant Gds2cppHarness::probe_bm(Object *p_data, const String &p_func, int idx, const Variant &base, const Array &args) {
	GDScriptInstance *inst = nullptr;
	GDScriptFunction *gf = nullptr;
	if (!_resolve(p_data, StringName(p_func), inst, gf)) {
		return "no-resolve";
	}
	Variant b = base;
	Variant valbuf[8];
	const Variant *argp[8];
	int argc = MIN(args.size(), 8);
	for (int i = 0; i < argc; i++) {
		valbuf[i] = args[i];
		argp[i] = &valbuf[i];
	}
	Variant ret;
	Variant::ValidatedBuiltInMethod m = gf->gds2cpp_builtin_method(idx);
	m(&b, argc > 0 ? argp : nullptr, argc, &ret);
	return ret;
}

Dictionary Gds2cppHarness::bench3(Object *p_data, const String &p_func, const Array &p_args, int n) {
	Dictionary out;
	StringName fname(p_func);
	Data_gen::Fn ff = _find_fn(fname);
	Data_spec::Fn fs = Data_spec::lookup(fname);
	GDScriptInstance *inst = nullptr;
	GDScriptFunction *gf = nullptr;
	if (!ff || !fs || !_resolve(p_data, fname, inst, gf)) {
		out["error"] = "need faithful+specialized+object for " + p_func;
		return out;
	}
	Variant valbuf[8];
	const Variant *argp[8];
	int argc = MIN(p_args.size(), 8);
	for (int i = 0; i < argc; i++) {
		valbuf[i] = p_args[i];
		argp[i] = &valbuf[i];
	}

	// Correctness: all three must agree.
	Callable::CallError ce;
	Variant ri = p_data->callp(fname, argp, argc, ce);
	Variant rf = ff(inst, gf, argp, argc);
	Variant rs = fs(inst, gf, argp, argc);

	uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	for (int i = 0; i < n; i++) {
		Callable::CallError e;
		p_data->callp(fname, argp, argc, e);
	}
	uint64_t interp_us = OS::get_singleton()->get_ticks_usec() - t0;

	Variant sink;
	t0 = OS::get_singleton()->get_ticks_usec();
	for (int i = 0; i < n; i++) {
		sink = ff(inst, gf, argp, argc);
	}
	uint64_t faithful_us = OS::get_singleton()->get_ticks_usec() - t0;

	t0 = OS::get_singleton()->get_ticks_usec();
	for (int i = 0; i < n; i++) {
		sink = fs(inst, gf, argp, argc);
	}
	uint64_t spec_us = OS::get_singleton()->get_ticks_usec() - t0;
	(void)sink;

	out["match"] = (ri == rf) && (rf == rs);
	out["interp_us"] = (int64_t)interp_us;
	out["faithful_us"] = (int64_t)faithful_us;
	out["spec_us"] = (int64_t)spec_us;
	out["faithful_x"] = faithful_us > 0 ? (double)interp_us / (double)faithful_us : 0.0;
	out["spec_x"] = spec_us > 0 ? (double)interp_us / (double)spec_us : 0.0;
	return out;
}

void Gds2cppHarness::_bind_methods() {
	ClassDB::bind_method(D_METHOD("run", "data", "func", "args"), &Gds2cppHarness::run);
	ClassDB::bind_method(D_METHOD("bench", "data", "func", "args", "n"), &Gds2cppHarness::bench);
	ClassDB::bind_method(D_METHOD("bench3", "data", "func", "args", "n"), &Gds2cppHarness::bench3);
	ClassDB::bind_method(D_METHOD("probe_bm", "data", "func", "idx", "base", "args"), &Gds2cppHarness::probe_bm);
}
