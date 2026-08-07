/**************************************************************************/
/*  dependency_manifest.h                                                 */
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

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

class DependencyManifest {
public:
	HashMap<String, Vector<String>> deps_of;

	Vector<String> paths;
	Vector<int> initial_ready;
	Vector<int> in_degree; // Per-index direct-dependency count.
	Vector<Vector<int>> dependents; // Per-index reverse adjacency.
	Vector<String> script_paths; // Every .gd path in the closure, collapsed into one lane node below.
	int combined_script_index = -1; // Stand-in node for all scripts combined, or -1 if none.

	static String resolve_dependency_string(const String &p_dep);
	static void scan_recursive(const String &p_path, HashMap<String, Vector<String>> &r_deps_of);
	bool build_execution_graph();
	static bool load_or_build(const Vector<String> &p_roots, DependencyManifest &r_manifest);

	static String cache_path_for_roots(const Vector<String> &p_roots);
	static String baked_cache_path_for_roots(const Vector<String> &p_roots);
	bool save_cache(const Vector<String> &p_roots) const;
	static bool try_load_cache(const Vector<String> &p_roots, DependencyManifest &r_manifest);

private:
	static String roots_cache_key(const Vector<String> &p_roots);
	static bool load_cache_file(const String &p_path, DependencyManifest &r_manifest, bool p_verify);
	static String pck_reference_path();
};
