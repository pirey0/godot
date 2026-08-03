/**************************************************************************/
/*  gds2cpp_tool.cpp                                                      */
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

#include "gds2cpp_tool.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/templates/hash_set.h"
#include "core/templates/pair.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"

// Recursively collect .gd file paths under p_dir (res:// relative).
static void _collect_gd(const String &p_dir, Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String n = da->get_next(); !n.is_empty(); n = da->get_next()) {
		if (n == "." || n == ".." || n.begins_with(".")) {
			continue;
		}
		String full = p_dir.path_join(n);
		if (da->current_is_dir()) {
			_collect_gd(full, r_files);
		} else if (n.ends_with(".gd")) {
			r_files.push_back(full);
		}
	}
	da->list_dir_end();
}

// Load a .gd file as a line vector (1-based via index-1) for comment interleaving.
static Vector<String> _load_source_lines(const String &p_path) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		return Vector<String>();
	}
	return f->get_as_text().replace("\r\n", "\n").split("\n");
}

// Non-alnum -> '_' (member names are already valid identifiers; this is defensive).
static String _sanitize(const String &p_s) {
	String r;
	for (int i = 0; i < p_s.length(); i++) {
		char32_t c = p_s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		r += ok ? String::chr(c) : String("_");
	}
	return r.is_empty() ? String("_") : r;
}

// Build member index -> "M_<name>" and the matching `static constexpr` block.
static HashMap<int, String> _member_names(const Ref<GDScript> &p_gds, String &r_block) {
	HashMap<int, String> out;
	HashSet<String> taken;
	// Sort by index so the emitted block reads top-to-bottom.
	Vector<Pair<int, StringName>> members;
	for (const auto &E : p_gds->debug_get_member_indices()) {
		members.push_back(Pair<int, StringName>(E.value.index, E.key));
	}
	members.sort_custom<PairSort<int, StringName>>();
	r_block = "// --- member slots ---\n";
	for (const Pair<int, StringName> &m : members) {
		String nm = "M_" + _sanitize(String(m.second));
		while (taken.has(nm)) {
			nm += "_";
		}
		taken.insert(nm);
		out[m.first] = nm;
		r_block += "static constexpr int " + nm + " = " + itos(m.first) + ";\n";
	}
	r_block += "\n";
	return out;
}

// Build member index -> declared builtin Variant::Type (for native specialization).
static HashMap<int, Variant::Type> _member_types(const Ref<GDScript> &p_gds) {
	HashMap<int, Variant::Type> out;
	for (const auto &E : p_gds->debug_get_member_indices()) {
		const GDScriptDataType &dt = E.value.data_type;
		if (dt.kind == GDScriptDataType::BUILTIN && dt.builtin_type != Variant::NIL) {
			out[E.value.index] = dt.builtin_type;
		}
	}
	return out;
}

// Build member index -> GDScript class it holds (for cross-class devirt).
static HashMap<int, const GDScript *> _member_classes(const Ref<GDScript> &p_gds) {
	HashMap<int, const GDScript *> out;
	for (const auto &E : p_gds->debug_get_member_indices()) {
		const GDScriptDataType &dt = E.value.data_type;
		if (dt.kind == GDScriptDataType::GDSCRIPT && dt.script_type) {
			out[E.value.index] = Object::cast_to<GDScript>(dt.script_type);
		}
	}
	return out;
}

String Gds2cppTool::analyze_script(const String &p_path) {
	Ref<GDScript> gds = ResourceLoader::load(p_path);
	if (gds.is_null()) {
		return "ERROR: could not load " + p_path;
	}

	String report = "=== gds2cpp analyze: " + p_path + " ===\n";
	int total = 0, ok_count = 0;
	const HashMap<StringName, GDScriptFunction *> &funcs = gds->get_member_functions();
	for (const KeyValue<StringName, GDScriptFunction *> &E : funcs) {
		total++;
		bool ok = false;
		String result = E.value->transpile_to_cpp("Data_gen", String(E.key), Vector<String>(), HashMap<int, String>(), HashMap<int, Variant::Type>(), HashMap<StringName, Pair<int, String>>(), HashMap<int, const GDScript *>(), HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>>(), ok);
		if (ok) {
			ok_count++;
			report += "  [C++ ] " + String(E.key) + "\n";
		} else {
			report += "  [interp] " + String(E.key) + "   (blocked by " + result + ")\n";
		}
	}
	report += vformat("--- transpiled %d / %d functions (rest interpreted) ---\n", ok_count, total);
	return report;
}

String Gds2cppTool::transpile_script(const String &p_path, const String &p_cpp_class) {
	Ref<GDScript> gds = ResourceLoader::load(p_path);
	if (gds.is_null()) {
		return "// ERROR: could not load " + p_path;
	}
	Vector<String> src_lines = _load_source_lines(p_path);
	String member_block;
	HashMap<int, String> member_names = _member_names(gds, member_block);
	HashMap<int, Variant::Type> member_types = _member_types(gds);
	String out = member_block;
	const HashMap<StringName, GDScriptFunction *> &funcs = gds->get_member_functions();
	for (const KeyValue<StringName, GDScriptFunction *> &E : funcs) {
		bool ok = false;
		String result = E.value->transpile_to_cpp(p_cpp_class, String(E.key), src_lines, member_names, member_types, HashMap<StringName, Pair<int, String>>(), HashMap<int, const GDScript *>(), HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>>(), ok);
		if (ok) {
			out += result + "\n";
		} else {
			out += "// (interpreted, blocked by " + result + "): " + String(E.key) + "\n\n";
		}
	}
	return out;
}

// Emit a compilable <file_base>.{h,cpp} (struct <cls>) for one script. Self-calls to
// methods in p_eligible are devirtualized to direct C++ calls; nullptr means all own
// methods are eligible (safe only when the class has no overriding subclass).
static const HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> _no_resolver;

struct OkFn {
	String orig, cpp;
	GDScriptFunction *fn;
	int implicit = 0; // 0 = member function; 1 = @implicit_new; 2 = @implicit_ready
};

// Pass A: which functions transpile + assign deterministic unique C++ names.
static Vector<OkFn> _pass_a(const Ref<GDScript> &gds, const String &cls) {
	Vector<String> src_lines = _load_source_lines(gds->get_path());
	String mb;
	HashMap<int, String> member_names = _member_names(gds, mb);
	HashMap<int, Variant::Type> member_types = _member_types(gds);
	Vector<OkFn> ok;
	HashSet<String> taken_fn;
	for (const KeyValue<StringName, GDScriptFunction *> &E : gds->get_member_functions()) {
		String cpp_name = "fn_" + _sanitize(String(E.key));
		while (taken_fn.has(cpp_name)) {
			cpp_name += "_";
		}
		bool okv = false;
		E.value->transpile_to_cpp(cls, cpp_name, src_lines, member_names, member_types, HashMap<StringName, Pair<int, String>>(), HashMap<int, const GDScript *>(), HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>>(), okv);
		if (okv) {
			taken_fn.insert(cpp_name);
			ok.push_back(OkFn{ String(E.key), cpp_name, E.value });
		}
	}
	// @implicit_new (the auto-generated member initializer, run on every .new()) is stored
	// outside member_functions -- transpile + bind it too (value-correctness verified).
	// @implicit_ready (onready-var init on tree entry) is deliberately left interpreted
	// until its $-path / get_node handling is verified.
	const GDScriptFunction *ctor = gds->get_implicit_initializer();
	if (ctor != nullptr) {
		bool okv = false;
		ctor->transpile_to_cpp(cls, "fn__implicit_new", src_lines, member_names, member_types, HashMap<StringName, Pair<int, String>>(), HashMap<int, const GDScript *>(), HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>>(), okv);
		if (okv) {
			ok.push_back(OkFn{ "@implicit_new", "fn__implicit_new", const_cast<GDScriptFunction *>(ctor), 1 });
		}
	}
	return ok;
}

// A transpilable lambda body + how bind() reaches its live GDScriptFunction: from the enclosing
// member function (by name, or the implicit initializer), walk gds2cpp_lambda() down `chain`.
struct LamFn {
	String cpp;
	GDScriptFunction *fn = nullptr;
	String root_key; // enclosing member function's original name
	int root_implicit = 0; // 0 = member function, 1 = @implicit_new
	Vector<int> chain; // gds2cpp_lambda() index path from the enclosing function
};

// Recursively gather a function's nested lambda bodies (lambdas can contain lambdas).
static void _collect_lambdas(const String &p_root_key, int p_root_implicit, GDScriptFunction *p_fn, const Vector<int> &p_prefix, HashSet<String> &r_taken, Vector<LamFn> &r_out) {
	const int n = p_fn->gds2cpp_lambda_count();
	for (int i = 0; i < n; i++) {
		GDScriptFunction *lam = p_fn->gds2cpp_lambda(i);
		if (lam == nullptr) {
			continue;
		}
		Vector<int> chain = p_prefix;
		chain.push_back(i);
		String cpp = "fn_lambda_" + itos(r_out.size());
		while (r_taken.has(cpp)) {
			cpp += "_";
		}
		r_taken.insert(cpp);
		r_out.push_back(LamFn{ cpp, lam, p_root_key, p_root_implicit, chain });
		_collect_lambdas(p_root_key, p_root_implicit, lam, chain, r_taken, r_out);
	}
}

static String _emit_class(const Ref<GDScript> &gds, const String &cls, const String &file_base, const String &p_out_dir, const HashSet<StringName> *p_eligible, Gds2cppStats *r_stats = nullptr, const HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> *p_resolver = nullptr, const HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> *p_super_targets = nullptr, const HashMap<StringName, Vector<Gds2cppTarget>> *p_by_name = nullptr) {
	const String p_path = gds->get_path();
	const HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> &resolver = p_resolver ? *p_resolver : _no_resolver;
	Vector<String> src_lines = _load_source_lines(p_path);
	String member_block;
	HashMap<int, String> member_names = _member_names(gds, member_block);
	HashMap<int, Variant::Type> member_types = _member_types(gds);
	HashMap<int, const GDScript *> member_classes = _member_classes(gds);
	const HashMap<StringName, GDScriptFunction *> &funcs = gds->get_member_functions();

	Vector<OkFn> ok = _pass_a(gds, cls);
	int total = funcs.size();

	// Devirt map: name -> (g_gf index, C++ name), gated by the never-overridden oracle.
	HashMap<StringName, Pair<int, String>> self_methods;
	for (int i = 0; i < ok.size(); i++) {
		if (p_eligible == nullptr || p_eligible->has(StringName(ok[i].orig))) {
			self_methods[StringName(ok[i].orig)] = Pair<int, String>(i, ok[i].cpp);
		}
	}

	// Pass B: transpile with devirtualized self-calls (accumulate call-outcome stats).
	String bodies;
	for (int i = 0; i < ok.size(); i++) {
		bool okv = false;
		bodies += ok[i].fn->transpile_to_cpp(cls, ok[i].cpp, src_lines, member_names, member_types, self_methods, member_classes, resolver, okv, r_stats, p_super_targets, p_by_name) + "\n";
	}

	// Pass B (lambdas): transpile each transpilable nested lambda body with the SAME class
	// context (members / self-methods / resolver) as its enclosing function. A lambda body that
	// does not transpile is simply left interpreted -- the CREATE_LAMBDA site still builds a
	// working Callable, so no enclosing function is blocked by it.
	HashSet<String> taken_names;
	for (const OkFn &o : ok) {
		taken_names.insert(o.cpp);
	}
	Vector<LamFn> lams;
	for (const OkFn &o : ok) {
		_collect_lambdas(o.orig, o.implicit, o.fn, Vector<int>(), taken_names, lams);
	}
	Vector<LamFn> ok_lams;
	for (const LamFn &L : lams) {
		bool okv = false;
		String body = L.fn->transpile_to_cpp(cls, L.cpp, src_lines, member_names, member_types, self_methods, member_classes, resolver, okv, r_stats, p_super_targets, p_by_name);
		if (okv) {
			bodies += body + "\n";
			ok_lams.push_back(L);
		}
	}

	const int N = MAX(ok.size(), 1);

	// --- header ---
	String h;
	h += "#pragma once\n";
	h += "// GENERATED by gds2cpp from " + p_path + " -- do not edit; regenerate.\n";
	h += "#include \"core/string/string_name.h\"\n";
	h += "#include \"core/variant/variant.h\"\n";
	h += "class GDScript;\nclass GDScriptInstance;\nclass GDScriptFunction;\n\n";
	h += "struct " + cls + " {\n";
	h += "\ttypedef Variant (*Fn)(GDScriptInstance *, GDScriptFunction *, const Variant **, int);\n";
	h += "\tstatic Fn lookup(const StringName &p_name);\n";
	h += "\tstatic GDScriptFunction *g_gf[" + itos(N) + "]; // per-method GDScriptFunction, for devirt calls\n";
	h += "\tstatic GDScript *g_script; // owning script, for the runtime guard of speculative devirt\n";
	h += "\tstatic void bind(GDScript *p_script); // populate g_gf from the owning script\n";
	for (const OkFn &n : ok) {
		h += "\tstatic Variant " + n.cpp + "(GDScriptInstance *, GDScriptFunction *, const Variant **, int);\n";
	}
	for (const LamFn &L : ok_lams) {
		h += "\tstatic Variant " + L.cpp + "(GDScriptInstance *, GDScriptFunction *, const Variant **, int); // lambda\n";
	}
	h += "};\n";

	// --- source ---
	String c;
	c += "// GENERATED by gds2cpp from " + p_path + " -- do not edit; regenerate.\n";
	c += "#include \"" + file_base + ".h\"\n\n";
	c += "#include \"core/object/class_db.h\"\n";
	c += "#include \"core/variant/variant_internal.h\"\n";
	c += "#include \"modules/gdscript/gdscript.h\"\n";
	c += "#include \"modules/gdscript/gdscript_function.h\"\n";
	c += "#include \"modules/gdscript/gdscript_lambda_callable.h\"\n";
	if (!resolver.is_empty()) {
		c += "#include \"gds2cpp_all.h\" // cross-class devirt targets\n"; // NOLINT
	}
	c += "\n";
	// Named slot indices into g_gf, referenced via the GF(<method>) macro so devirt
	// calls read GF(ofOr) instead of g_gf[8].
	if (ok.size() > 0) {
		c += "enum { // g_gf slots\n";
		for (int i = 0; i < ok.size(); i++) {
			c += "\tGF_" + ok[i].cpp.substr(3) + " = " + itos(i) + ",\n";
		}
		c += "};\n";
	}
	c += "#define GF(m) g_gf[GF_##m]\n";
	c += "GDScriptFunction *" + cls + "::g_gf[" + itos(N) + "] = {};\n";
	c += "GDScript *" + cls + "::g_script = nullptr;\n";
	c += "void " + cls + "::bind(GDScript *p_script) {\n";
	c += "\tg_script = p_script;\n";
	c += "\tconst HashMap<StringName, GDScriptFunction *> &fns = p_script->get_member_functions();\n";
	for (int i = 0; i < ok.size(); i++) {
		// Fill the g_gf devirt table AND install the transpiled body on the live
		// GDScriptFunction so GDScriptFunction::call() can dispatch to it directly.
		const String slot = ok[i].cpp.substr(3);
		if (ok[i].implicit != 0) {
			// @implicit_new / @implicit_ready live outside member_functions.
			const String getter = ok[i].implicit == 1 ? "get_implicit_initializer()" : "get_implicit_ready()";
			c += "\tif (p_script->" + getter + ") { GDScriptFunction *gf = const_cast<GDScriptFunction *>(p_script->" + getter + "); GF(" + slot + ") = gf; gf->gds2cpp_set_fn(&" + cls + "::" + ok[i].cpp + "); }\n";
		} else {
			const String key = "StringName(\"" + ok[i].orig + "\")";
			c += "\tif (fns.has(" + key + ")) { GDScriptFunction *gf = fns[" + key + "]; GF(" + slot + ") = gf; gf->gds2cpp_set_fn(&" + cls + "::" + ok[i].cpp + "); }\n";
		}
	}
	// Install lambda bodies: reach each lambda's live GDScriptFunction from its enclosing
	// member function, then walk gds2cpp_lambda() down the recorded index chain.
	for (const LamFn &L : ok_lams) {
		const String root = L.root_implicit == 1
				? "const_cast<GDScriptFunction *>(p_script->get_implicit_initializer())"
				: ("fns.has(StringName(\"" + L.root_key + "\")) ? fns[StringName(\"" + L.root_key + "\")] : nullptr");
		c += "\t{ GDScriptFunction *_r = " + root + ";\n";
		for (int idx : L.chain) {
			c += "\t\t_r = _r ? _r->gds2cpp_lambda(" + itos(idx) + ") : nullptr;\n";
		}
		c += "\t\tif (_r) _r->gds2cpp_set_fn(&" + cls + "::" + L.cpp + "); }\n";
	}
	c += "}\n\n";
	c += member_block;
	c += bodies;
	c += cls + "::Fn " + cls + "::lookup(const StringName &p_name) {\n";
	for (const OkFn &n : ok) {
		c += "\tif (p_name == StringName(\"" + n.orig + "\")) return &" + cls + "::" + n.cpp + ";\n";
	}
	c += "\treturn nullptr;\n}\n";
	c += "#undef GF\n";

	Ref<FileAccess> fh = FileAccess::open(p_out_dir.path_join(file_base + ".h"), FileAccess::WRITE);
	if (fh.is_null()) {
		return "ERROR: cannot write header to " + p_out_dir;
	}
	fh->store_string(h);
	fh->close();
	Ref<FileAccess> fc = FileAccess::open(p_out_dir.path_join(file_base + ".cpp"), FileAccess::WRITE);
	fc->store_string(c);
	fc->close();
	return vformat("wrote %s.{h,cpp}: %d/%d functions + %d/%d lambda bodies", file_base, ok.size(), total, ok_lams.size(), lams.size());
}

String Gds2cppTool::transpile_module_files(const String &p_path, const String &p_out_dir, const String &p_cpp_class, const String &p_file_base) {
	Ref<GDScript> gds = ResourceLoader::load(p_path);
	if (gds.is_null()) {
		return "ERROR: could not load " + p_path;
	}
	return _emit_class(gds, p_cpp_class, p_file_base, p_out_dir, nullptr); // nullptr = all eligible
}

// Whole-program: emit every .gd in p_root as its own <G_path>.{h,cpp} with devirt gated
// by the never-overridden oracle, plus gds2cpp_all.{h,cpp} (bind_all() registry).
String Gds2cppTool::transpile_program(const String &p_root, const String &p_out_dir) {
	Vector<String> files;
	_collect_gd(p_root, files);

	struct Cls {
		String path, cpp;
		Ref<GDScript> gds;
	};
	Vector<Cls> classes;
	HashSet<String> taken_cls;
	for (const String &f : files) {
		Ref<GDScript> g = ResourceLoader::load(f);
		if (g.is_null()) {
			continue;
		}
		String base = f.trim_prefix("res://").trim_suffix(".gd");
		String cpp = "G_" + _sanitize(base);
		while (taken_cls.has(cpp)) {
			cpp += "_";
		}
		taken_cls.insert(cpp);
		classes.push_back(Cls{ f, cpp, g });
	}

	// Inheritance children map for the override oracle.
	HashMap<GDScript *, Vector<GDScript *>> children;
	for (const Cls &c : classes) {
		GDScript *b = c.gds->get_base().ptr();
		if (b) {
			children[b].push_back(c.gds.ptr());
		}
	}

	int total_methods = 0, total_eligible = 0;
	Gds2cppStats stats;

	// PRE-PASS: per-class devirt-eligibility + the program-wide call resolver
	// (class -> method -> direct C++ target), needed before any cross-class emit.
	HashMap<const GDScript *, HashSet<StringName>> eligible_of;
	HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> resolver;
	// Unfiltered variant (includes overridden methods) for resolving super.method() targets.
	HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>> super_targets;
	// method name -> every class's target for it (guarded-devirt candidates for dynamic calls).
	HashMap<StringName, Vector<Gds2cppTarget>> by_name;
	for (const Cls &c : classes) {
		HashSet<StringName> eligible;
		for (const KeyValue<StringName, GDScriptFunction *> &E : c.gds->get_member_functions()) {
			total_methods++;
			bool over = false;
			Vector<GDScript *> stack;
			if (children.has(c.gds.ptr())) {
				stack = children[c.gds.ptr()];
			}
			while (!stack.is_empty() && !over) {
				GDScript *d = stack[stack.size() - 1];
				stack.remove_at(stack.size() - 1);
				if (d->get_member_functions().has(E.key)) {
					over = true;
					break;
				}
				if (children.has(d)) {
					for (GDScript *x : children[d]) {
						stack.push_back(x);
					}
				}
			}
			if (!over) {
				eligible.insert(E.key);
				total_eligible++;
			}
		}
		eligible_of[c.gds.ptr()] = eligible;
		Vector<OkFn> ok = _pass_a(c.gds, c.cpp);
		for (int i = 0; i < ok.size(); i++) {
			const StringName key = StringName(ok[i].orig);
			const Gds2cppTarget tgt{ c.cpp, ok[i].cpp, i };
			super_targets[c.gds.ptr()][key] = tgt;
			if (ok[i].implicit == 0) {
				by_name[key].push_back(tgt); // real member methods only (not @implicit_new)
			}
			if (eligible.has(key)) {
				resolver[c.gds.ptr()][key] = tgt;
			}
		}
	}

	// EMIT with cross-class direct calls resolved against the whole program.
	String includes, bind_body;
	for (const Cls &c : classes) {
		_emit_class(c.gds, c.cpp, c.cpp, p_out_dir, &eligible_of[c.gds.ptr()], &stats, &resolver, &super_targets, &by_name);
		includes += "#include \"" + c.cpp + ".h\"\n";
		bind_body += "\t{ Ref<GDScript> g = ResourceLoader::load(\"" + c.path + "\"); if (g.is_valid()) " + c.cpp + "::bind(g.ptr()); }\n";
	}

	// Whole-program index: bind_all() populates every class's g_gf.
	String h = "#pragma once\n// GENERATED whole-program index -- do not edit; regenerate.\n" + includes + "\nvoid gds2cpp_bind_all();\n";
	String cc = "#include \"gds2cpp_all.h\"\n\n#include \"core/io/resource_loader.h\"\n#include \"modules/gdscript/gdscript.h\"\n\nvoid gds2cpp_bind_all() {\n" + bind_body + "}\n";
	Ref<FileAccess> fh = FileAccess::open(p_out_dir.path_join("gds2cpp_all.h"), FileAccess::WRITE);
	if (fh.is_valid()) {
		fh->store_string(h);
		fh->close();
	}
	Ref<FileAccess> fc = FileAccess::open(p_out_dir.path_join("gds2cpp_all.cpp"), FileAccess::WRITE);
	if (fc.is_valid()) {
		fc->store_string(cc);
		fc->close();
	}

	// By-name method calls: how many became native / direct vs stayed dynamic.
	const int byname = stats.native_calls + stats.devirt_calls + stats.cross_calls + stats.dynamic_calls;
	const int all_calls = byname + stats.validated_calls;
	String r = vformat("classes: %d, methods: %d, devirt-eligible: %d (%.1f%%)\n",
			classes.size(), total_methods, total_eligible, total_methods ? 100.0 * total_eligible / total_methods : 0.0);
	r += vformat("call sites: %d total (%d by-name + %d already-validated-builtin)\n", all_calls, byname, stats.validated_calls);
	r += vformat("  native (by-name -> native builtin):  %d (%.1f%% of by-name)\n", stats.native_calls, byname ? 100.0 * stats.native_calls / byname : 0.0);
	r += vformat("  devirt (self-call -> direct C++):    %d (%.1f%%)\n", stats.devirt_calls, byname ? 100.0 * stats.devirt_calls / byname : 0.0);
	r += vformat("  cross-class (typed recv -> direct):  %d (%.1f%%)\n", stats.cross_calls, byname ? 100.0 * stats.cross_calls / byname : 0.0);
	r += vformat("  dynamic (callp fallback):            %d (%.1f%%)\n", stats.dynamic_calls, byname ? 100.0 * stats.dynamic_calls / byname : 0.0);
	const int super_total = stats.super_calls + stats.super_dynamic_calls;
	if (super_total > 0) {
		r += vformat("  super (direct parent C++):           %d of %d super-calls (%.1f%%); %d runtime-walk (native/untranspiled parent)\n",
				stats.super_calls, super_total, super_total ? 100.0 * stats.super_calls / super_total : 0.0, stats.super_dynamic_calls);
	}
	const int specialized = stats.native_calls + stats.devirt_calls + stats.cross_calls + stats.validated_calls;
	r += vformat("  => %.1f%% of all call sites go through pure C++ (native+devirt+cross+validated)\n",
			all_calls ? 100.0 * specialized / all_calls : 0.0);
	if (stats.lambda_creates > 0) {
		r += vformat("  lambda sites emitted: %d\n", stats.lambda_creates);
	}
	if (stats.native_ops > 0) {
		r += vformat("  native scalar ops (inlined, no dispatch): %d\n", stats.native_ops);
	}
	const int dyn_total = stats.dyn_uniq + stats.dyn_few + stats.dyn_many + stats.dyn_none;
	if (dyn_total > 0) {
		r += vformat("  dynamic-call candidates (guarded-devirt opportunity): %d unique class, %d few(2-3), %d many(4+), %d native-only(0)\n",
				stats.dyn_uniq, stats.dyn_few, stats.dyn_many, stats.dyn_none);
		r += vformat("    => %.1f%% of dynamic calls have <=3 candidate classes (guardable)\n",
				dyn_total ? 100.0 * (stats.dyn_uniq + stats.dyn_few) / dyn_total : 0.0);
		r += vformat("  guarded speculative devirt emitted: %d sites\n", stats.pic_calls);
	}
	return r;
}

// Whole-program collect + override analysis: how much devirtualization is safe?
// A method is devirt-eligible if no descendant class overrides it (so a self-call
// or exact-typed-receiver call always resolves to that one C++ function).
String Gds2cppTool::analyze_program(const String &p_root) {
	Vector<String> files;
	_collect_gd(p_root, files);

	Vector<Ref<GDScript>> scripts;
	for (const String &f : files) {
		Ref<GDScript> g = ResourceLoader::load(f);
		if (g.is_valid()) {
			scripts.push_back(g);
		}
	}
	// base -> children
	HashMap<GDScript *, Vector<GDScript *>> children;
	for (const Ref<GDScript> &g : scripts) {
		GDScript *b = g->get_base().ptr();
		if (b) {
			children[b].push_back(g.ptr());
		}
	}

	int total_classes = scripts.size();
	int total_methods = 0, devirt_eligible = 0, overridden = 0;
	for (const Ref<GDScript> &g : scripts) {
		for (const KeyValue<StringName, GDScriptFunction *> &E : g->get_member_functions()) {
			total_methods++;
			// DFS descendants of g; is E.key redefined below?
			bool is_over = false;
			Vector<GDScript *> stack;
			if (children.has(g.ptr())) {
				stack = children[g.ptr()];
			}
			while (!stack.is_empty() && !is_over) {
				GDScript *d = stack[stack.size() - 1];
				stack.remove_at(stack.size() - 1);
				if (d->get_member_functions().has(E.key)) {
					is_over = true;
					break;
				}
				if (children.has(d)) {
					for (GDScript *c : children[d]) {
						stack.push_back(c);
					}
				}
			}
			if (is_over) {
				overridden++;
			} else {
				devirt_eligible++;
			}
		}
	}

	String r = "=== gds2cpp analyze_program: " + p_root + " ===\n";
	r += vformat("classes: %d\n", total_classes);
	r += vformat("methods: %d total\n", total_methods);
	r += vformat("  devirt-eligible (never overridden below): %d (%.1f%%)\n",
			devirt_eligible, total_methods ? 100.0 * devirt_eligible / total_methods : 0.0);
	r += vformat("  overridden somewhere: %d\n", overridden);
	return r;
}

String Gds2cppTool::analyze_dir(const String &p_root) {
	Vector<String> files;
	_collect_gd(p_root, files);

	int total_files = 0, load_fail = 0, total_fns = 0, ok_fns = 0;
	HashMap<String, int> block_by_op; // opcode token -> count of functions it blocks

	for (const String &f : files) {
		Ref<GDScript> gds = ResourceLoader::load(f);
		if (gds.is_null()) {
			load_fail++;
			continue;
		}
		total_files++;
		for (const KeyValue<StringName, GDScriptFunction *> &E : gds->get_member_functions()) {
			total_fns++;
			bool ok = false;
			String res = E.value->transpile_to_cpp("X", String(E.key), Vector<String>(), HashMap<int, String>(), HashMap<int, Variant::Type>(), HashMap<StringName, Pair<int, String>>(), HashMap<int, const GDScript *>(), HashMap<const GDScript *, HashMap<StringName, Gds2cppTarget>>(), ok);
			if (ok) {
				ok_fns++;
			} else {
				String key = res.get_slice(" ", 0); // "OPCODE_<n>"
				block_by_op[key] = block_by_op.has(key) ? block_by_op[key] + 1 : 1;
			}
		}
	}

	Vector<Pair<int, String>> sorted; // (count, opcode token)
	for (const KeyValue<String, int> &E : block_by_op) {
		sorted.push_back(Pair<int, String>(E.value, E.key));
	}
	sorted.sort_custom<PairSort<int, String>>();

	String r = "=== gds2cpp analyze_dir: " + p_root + " ===\n";
	r += vformat("files: %d loaded, %d failed to load\n", total_files, load_fail);
	r += vformat("functions: %d total, %d transpiled (%.1f%%), %d interpreted\n",
			total_fns, ok_fns, total_fns ? 100.0 * ok_fns / total_fns : 0.0, total_fns - ok_fns);
	r += "--- blocking opcodes (functions blocked) ---\n";
	for (int i = sorted.size() - 1; i >= 0; i--) {
		r += vformat("  %s: %d\n", sorted[i].second, sorted[i].first);
	}
	return r;
}

void Gds2cppTool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("analyze_script", "path"), &Gds2cppTool::analyze_script);
	ClassDB::bind_method(D_METHOD("analyze_dir", "root"), &Gds2cppTool::analyze_dir);
	ClassDB::bind_method(D_METHOD("analyze_program", "root"), &Gds2cppTool::analyze_program);
	ClassDB::bind_method(D_METHOD("transpile_script", "path", "cpp_class"), &Gds2cppTool::transpile_script);
	ClassDB::bind_method(D_METHOD("transpile_module_files", "path", "out_dir", "cpp_class", "file_base"), &Gds2cppTool::transpile_module_files);
	ClassDB::bind_method(D_METHOD("transpile_program", "root", "out_dir"), &Gds2cppTool::transpile_program);
}
