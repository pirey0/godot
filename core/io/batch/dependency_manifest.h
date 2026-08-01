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
	// Canonical resource path -> its direct, resolved dependency paths.
	HashMap<String, Vector<String>> deps_of;
	// Topological layers, leaves first. Populated by compute_layers().
	Vector<Vector<String>> layers;

	// Resolves a raw dependency string from ResourceLoader::get_dependencies()
	// (which may be "uid://...", "res://a.tres::::Type", or a plain path) to a
	// canonical res:// path. Returns an empty string if it can't be resolved.
	static String resolve_dependency_string(const String &p_dep);

	// Scans the transitive closure of p_root via ResourceLoader::get_dependencies(),
	// recursively, guarding against re-visiting (and infinite-looping on cycles in) a path.
	static void scan_recursive(const String &p_path, HashMap<String, Vector<String>> &r_deps_of);

	// Loads a valid cached subgraph for p_root, or scans fresh and caches the result.
	static DependencyManifest load_or_build(const String &p_root);

	// Unions any number of (possibly overlapping) subgraphs into one deduplicated graph.
	static DependencyManifest merge(const Vector<DependencyManifest> &p_subgraphs);

	// Kahn's-algorithm topological sort into layers. Returns false if a genuine
	// cycle exists in the resource data itself (treated as a hard failure).
	bool compute_layers();

	static String cache_path_for_root(const String &p_root);
	bool save_cache(const String &p_root) const;
	static bool try_load_cache(const String &p_root, DependencyManifest &r_manifest);
};
