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

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/templates/hash_set.h"
#include "core/templates/pair.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"

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

void Gds2cppTool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("analyze_script", "path"), &Gds2cppTool::analyze_script);
	ClassDB::bind_method(D_METHOD("transpile_script", "path", "cpp_class"), &Gds2cppTool::transpile_script);
}
