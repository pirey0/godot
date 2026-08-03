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

#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "core/templates/pair.h"
#include "data_gen_auto.h"
#include "data_spec.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"
#include "optest_gen.h"

// Whole-program registry, present only once transpile_program has generated wp/.
#if defined(__has_include) && __has_include("wp/gds2cpp_all.h")
#include "wp/gds2cpp_all.h"
#define GDS2CPP_HAS_WP 1
#endif

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
	// Populate the generated classes' devirt tables (g_gf) from this script.
	Data_gen::bind(gds.ptr());
	OpTest_gen::bind(gds.ptr());
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

int Gds2cppHarness::install(Object *p_obj) {
	if (!p_obj) {
		return 0;
	}
	Ref<GDScript> gds = p_obj->get_script();
	if (gds.is_null()) {
		return 0;
	}
	// bind() only touches functions whose names match, so binding every compiled-in
	// class against the script is safe -- non-matching classes are no-ops.
	Data_gen::bind(gds.ptr());
	OpTest_gen::bind(gds.ptr());
	return 1;
}

void Gds2cppHarness::set_enabled(bool p_on) {
	GDScriptFunction::gds2cpp_enabled = p_on;
}

bool Gds2cppHarness::is_enabled() const {
	return GDScriptFunction::gds2cpp_enabled;
}

extern bool g_gds2cpp_verify;
extern uint64_t g_gds2cpp_verify_crashes;
extern HashMap<StringName, uint64_t> g_gds2cpp_verify_crash_names;
extern const StringName *g_gds2cpp_last_source;
extern const StringName *g_gds2cpp_last_name;

String Gds2cppHarness::last_cpp_fn() const {
	String s = g_gds2cpp_last_source ? String(*g_gds2cpp_last_source) : String("<none>");
	String n = g_gds2cpp_last_name ? String(*g_gds2cpp_last_name) : String("<none>");
	return s + " :: " + n;
}

void Gds2cppHarness::set_verify(bool p_on) {
	g_gds2cpp_verify = p_on;
}

bool Gds2cppHarness::is_verify() const {
	return g_gds2cpp_verify;
}

Dictionary Gds2cppHarness::verify_report() const {
	Dictionary d;
	d["crashes"] = (int64_t)g_gds2cpp_verify_crashes;
	Vector<Pair<StringName, uint64_t>> v;
	for (const KeyValue<StringName, uint64_t> &e : g_gds2cpp_verify_crash_names) {
		v.push_back(Pair<StringName, uint64_t>(e.key, e.value));
	}
	struct ByCount {
		bool operator()(const Pair<StringName, uint64_t> &a, const Pair<StringName, uint64_t> &b) const { return a.second > b.second; }
	};
	v.sort_custom<ByCount>();
	Array fns;
	for (int i = 0; i < v.size(); i++) {
		Array row;
		row.push_back(String(v[i].first));
		row.push_back((int64_t)v[i].second);
		fns.push_back(row);
	}
	d["functions"] = fns;
	return d;
}

int Gds2cppHarness::bind_all() {
#ifdef GDS2CPP_HAS_WP
	gds2cpp_bind_all();
	return 1;
#else
	return 0;
#endif
}

int Gds2cppHarness::uninstall_path(const String &p_path) {
	Ref<GDScript> gds = ResourceLoader::load(p_path);
	if (gds.is_null()) {
		return 0;
	}
	const HashMap<StringName, GDScriptFunction *> &fns = gds->get_member_functions();
	for (const KeyValue<StringName, GDScriptFunction *> &e : fns) {
		e.value->gds2cpp_set_fn(nullptr);
	}
	return fns.size();
}

Dictionary Gds2cppHarness::dispatch_stats() const {
	Dictionary d;
	const uint64_t total = GDScriptFunction::gds2cpp_calls_total;
	d["total"] = (int64_t)total;
	d["cpp"] = (int64_t)GDScriptFunction::gds2cpp_calls_cpp;
	d["interpreted"] = (int64_t)(total - GDScriptFunction::gds2cpp_calls_cpp);
	d["no_fn"] = (int64_t)GDScriptFunction::gds2cpp_calls_no_fn;
	d["defarg"] = (int64_t)GDScriptFunction::gds2cpp_calls_defarg;
	d["coroutine"] = (int64_t)GDScriptFunction::gds2cpp_calls_coroutine;
	d["cpp_pct"] = total ? (100.0 * (double)GDScriptFunction::gds2cpp_calls_cpp / (double)total) : 0.0;
	return d;
}

void Gds2cppHarness::reset_dispatch_stats() {
	GDScriptFunction::gds2cpp_calls_total = 0;
	GDScriptFunction::gds2cpp_calls_cpp = 0;
	GDScriptFunction::gds2cpp_calls_no_fn = 0;
	GDScriptFunction::gds2cpp_calls_defarg = 0;
	GDScriptFunction::gds2cpp_calls_coroutine = 0;
	GDScriptFunction::gds2cpp_no_fn_names.clear();
}

Array Gds2cppHarness::top_uncovered(int p_n) const {
	Vector<Pair<StringName, uint64_t>> v;
	for (const KeyValue<StringName, uint64_t> &e : GDScriptFunction::gds2cpp_no_fn_names) {
		v.push_back(Pair<StringName, uint64_t>(e.key, e.value));
	}
	struct ByCount {
		bool operator()(const Pair<StringName, uint64_t> &a, const Pair<StringName, uint64_t> &b) const { return a.second > b.second; }
	};
	v.sort_custom<ByCount>();
	Array out;
	for (int i = 0; i < MIN(p_n, v.size()); i++) {
		Array row;
		row.push_back(String(v[i].first));
		row.push_back((int64_t)v[i].second);
		out.push_back(row);
	}
	return out;
}

void Gds2cppHarness::_bind_methods() {
	ClassDB::bind_method(D_METHOD("run", "data", "func", "args"), &Gds2cppHarness::run);
	ClassDB::bind_method(D_METHOD("bench", "data", "func", "args", "n"), &Gds2cppHarness::bench);
	ClassDB::bind_method(D_METHOD("bench3", "data", "func", "args", "n"), &Gds2cppHarness::bench3);
	ClassDB::bind_method(D_METHOD("probe_bm", "data", "func", "idx", "base", "args"), &Gds2cppHarness::probe_bm);
	ClassDB::bind_method(D_METHOD("install", "obj"), &Gds2cppHarness::install);
	ClassDB::bind_method(D_METHOD("set_enabled", "on"), &Gds2cppHarness::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &Gds2cppHarness::is_enabled);
	ClassDB::bind_method(D_METHOD("set_verify", "on"), &Gds2cppHarness::set_verify);
	ClassDB::bind_method(D_METHOD("is_verify"), &Gds2cppHarness::is_verify);
	ClassDB::bind_method(D_METHOD("verify_report"), &Gds2cppHarness::verify_report);
	ClassDB::bind_method(D_METHOD("last_cpp_fn"), &Gds2cppHarness::last_cpp_fn);
	ClassDB::bind_method(D_METHOD("bind_all"), &Gds2cppHarness::bind_all);
	ClassDB::bind_method(D_METHOD("uninstall_path", "path"), &Gds2cppHarness::uninstall_path);
	ClassDB::bind_method(D_METHOD("dispatch_stats"), &Gds2cppHarness::dispatch_stats);
	ClassDB::bind_method(D_METHOD("reset_dispatch_stats"), &Gds2cppHarness::reset_dispatch_stats);
	ClassDB::bind_method(D_METHOD("top_uncovered", "n"), &Gds2cppHarness::top_uncovered);
}
