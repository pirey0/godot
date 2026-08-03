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
		String result = E.value->transpile_to_cpp("Data_gen", String(E.key), Vector<String>(), HashMap<int, String>(), ok);
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
	String out = member_block;
	const HashMap<StringName, GDScriptFunction *> &funcs = gds->get_member_functions();
	for (const KeyValue<StringName, GDScriptFunction *> &E : funcs) {
		bool ok = false;
		String result = E.value->transpile_to_cpp(p_cpp_class, String(E.key), src_lines, member_names, ok);
		if (ok) {
			out += result + "\n";
		} else {
			out += "// (interpreted, blocked by " + result + "): " + String(E.key) + "\n\n";
		}
	}
	return out;
}

// Emit a compilable header + source pair into p_out_dir so the generated code can
// be built into the engine and driven by name via Data_gen::lookup().
String Gds2cppTool::transpile_module_files(const String &p_path, const String &p_out_dir, const String &p_cpp_class, const String &p_file_base) {
	Ref<GDScript> gds = ResourceLoader::load(p_path);
	if (gds.is_null()) {
		return "ERROR: could not load " + p_path;
	}
	const String cls = p_cpp_class;
	Vector<String> src_lines = _load_source_lines(p_path);
	String member_block;
	HashMap<int, String> member_names = _member_names(gds, member_block);

	String bodies;
	Vector<Pair<String, String>> ok_names; // (original name, sanitized C++ identifier)
	HashSet<String> taken_fn;
	int total = 0;
	const HashMap<StringName, GDScriptFunction *> &funcs = gds->get_member_functions();
	for (const KeyValue<StringName, GDScriptFunction *> &E : funcs) {
		total++;
		String orig = String(E.key);
		String cpp_name = "fn_" + _sanitize(orig); // fn_ prefix avoids leading-digit / keyword clashes
		while (taken_fn.has(cpp_name)) {
			cpp_name += "_";
		}
		bool ok = false;
		String result = E.value->transpile_to_cpp(cls, cpp_name, src_lines, member_names, ok);
		if (ok) {
			taken_fn.insert(cpp_name);
			bodies += result + "\n";
			ok_names.push_back(Pair<String, String>(orig, cpp_name));
		}
	}

	// --- header ---
	String h;
	h += "#pragma once\n";
	h += "// GENERATED by gds2cpp from " + p_path + " -- do not edit; regenerate.\n";
	h += "#include \"core/string/string_name.h\"\n";
	h += "#include \"core/variant/variant.h\"\n";
	h += "class GDScriptInstance;\nclass GDScriptFunction;\n\n";
	h += "struct " + cls + " {\n";
	h += "\ttypedef Variant (*Fn)(GDScriptInstance *, GDScriptFunction *, const Variant **, int);\n";
	h += "\tstatic Fn lookup(const StringName &p_name);\n";
	for (const Pair<String, String> &n : ok_names) {
		h += "\tstatic Variant " + n.second + "(GDScriptInstance *, GDScriptFunction *, const Variant **, int);\n";
	}
	h += "};\n";

	// --- source ---
	String c;
	c += "// GENERATED by gds2cpp from " + p_path + " -- do not edit; regenerate.\n";
	c += "#include \"" + p_file_base + ".h\"\n\n";
	c += "#include \"core/object/class_db.h\"\n";
	c += "#include \"modules/gdscript/gdscript.h\"\n";
	c += "#include \"modules/gdscript/gdscript_function.h\"\n\n";
	c += member_block;
	c += bodies;
	c += cls + "::Fn " + cls + "::lookup(const StringName &p_name) {\n";
	for (const Pair<String, String> &n : ok_names) {
		c += "\tif (p_name == StringName(\"" + n.first + "\")) return &" + cls + "::" + n.second + ";\n";
	}
	c += "\treturn nullptr;\n}\n";

	Ref<FileAccess> fh = FileAccess::open(p_out_dir.path_join(p_file_base + ".h"), FileAccess::WRITE);
	if (fh.is_null()) {
		return "ERROR: cannot write header to " + p_out_dir;
	}
	fh->store_string(h);
	fh->close();
	Ref<FileAccess> fc = FileAccess::open(p_out_dir.path_join(p_file_base + ".cpp"), FileAccess::WRITE);
	fc->store_string(c);
	fc->close();
	return vformat("wrote %s.{h,cpp}: %d/%d functions", p_file_base, ok_names.size(), total);
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
			String res = E.value->transpile_to_cpp("X", String(E.key), Vector<String>(), HashMap<int, String>(), ok);
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
	ClassDB::bind_method(D_METHOD("transpile_script", "path", "cpp_class"), &Gds2cppTool::transpile_script);
	ClassDB::bind_method(D_METHOD("transpile_module_files", "path", "out_dir", "cpp_class", "file_base"), &Gds2cppTool::transpile_module_files);
}
