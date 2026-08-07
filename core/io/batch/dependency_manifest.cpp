/**************************************************************************/
/*  dependency_manifest.cpp                                               */
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

#include "dependency_manifest.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"

static const uint32_t DEPENDENCY_MANIFEST_CACHE_MAGIC = 0x424C4443; // "BLDC"
static const uint32_t DEPENDENCY_MANIFEST_CACHE_VERSION = 3; // Bump on format change; mismatch = cache miss.

String DependencyManifest::resolve_dependency_string(const String &p_dep) {
	String uid_part = p_dep;
	int sep = p_dep.find("::::");
	if (sep != -1) {
		uid_part = p_dep.substr(0, sep);
	}

	if (uid_part.begins_with("uid://")) {
		ResourceUID::ID uid = ResourceUID::get_singleton()->text_to_id(uid_part);
		if (uid != ResourceUID::INVALID_ID && ResourceUID::get_singleton()->has_id(uid)) {
			return ResourceUID::get_singleton()->get_id_path(uid);
		}
	}

	if (sep != -1) {
		return p_dep.substr(sep + 4);
	}

	if (p_dep.begins_with("res://")) {
		return p_dep;
	}

	return String();
}

void DependencyManifest::scan_recursive(const String &p_path, HashMap<String, Vector<String>> &r_deps_of) {
	if (r_deps_of.has(p_path)) {
		return;
	}
	if (!ResourceLoader::exists(p_path)) {
		ERR_PRINT(vformat("DependencyManifest: path does not exist: %s", p_path));
		return;
	}

	// Placeholder to break cycles during recursion; overwritten below with the real list.
	r_deps_of.insert(p_path, Vector<String>());

	List<String> raw_deps;
	ResourceLoader::get_dependencies(p_path, &raw_deps, false);

	Vector<String> resolved_deps;
	for (const String &raw : raw_deps) {
		String resolved = resolve_dependency_string(raw);
		if (resolved.is_empty() || !ResourceLoader::exists(resolved)) {
			continue;
		}
		resolved_deps.push_back(resolved);
		scan_recursive(resolved, r_deps_of);
	}

	r_deps_of[p_path] = resolved_deps;
}

bool DependencyManifest::build_execution_graph() {
	paths.clear();
	initial_ready.clear();
	in_degree.clear();
	dependents.clear();
	script_paths.clear();
	combined_script_index = -1;

	// Pass 1: a node per non-script path; scripts collapse into one node below.
	HashMap<String, int> index_of;
	for (const KeyValue<String, Vector<String>> &kv : deps_of) {
		if (kv.key.get_extension() == "gd") {
			script_paths.push_back(kv.key);
			continue;
		}
		index_of.insert(kv.key, paths.size());
		paths.push_back(kv.key);
	}

	bool has_scripts = script_paths.size() > 0;
	if (has_scripts) {
		combined_script_index = paths.size();
		paths.push_back(String());
	}

	int n = paths.size();
	in_degree.resize(n);
	dependents.resize(n);
	for (int i = 0; i < n; i++) {
		in_degree.write[i] = 0;
	}

	// Pass 2: edges, redirecting any script dependency to the combined node.
	for (const KeyValue<String, Vector<String>> &kv : deps_of) {
		if (kv.key.get_extension() == "gd") {
			continue;
		}
		int i = index_of[kv.key];
		for (const String &dep : kv.value) {
			int dep_idx;
			if (dep.get_extension() == "gd") {
				if (!has_scripts) {
					continue;
				}
				dep_idx = combined_script_index;
			} else {
				const int *found = index_of.getptr(dep);
				if (!found) {
					continue;
				}
				dep_idx = *found;
			}
			in_degree.write[i] += 1;
			dependents.write[dep_idx].push_back(i);
		}
	}

	for (int i = 0; i < n; i++) {
		if (in_degree[i] == 0) {
			initial_ready.push_back(i);
		}
	}

	// Throwaway Kahn's traversal over a scratch copy, to detect a hard cycle.
	Vector<int> scratch_in_degree = in_degree;
	Vector<int> frontier = initial_ready;
	uint32_t processed = 0;
	while (frontier.size() > 0) {
		processed += frontier.size();
		Vector<int> next_frontier;
		for (int node : frontier) {
			for (int dependent : dependents[node]) {
				scratch_in_degree.write[dependent] -= 1;
				if (scratch_in_degree[dependent] == 0) {
					next_frontier.push_back(dependent);
				}
			}
		}
		frontier = next_frontier;
	}

	if (processed < (uint32_t)n) {
		// Leftover nodes never reached in-degree 0: a genuine cycle in the asset data.
		paths.clear();
		initial_ready.clear();
		in_degree.clear();
		dependents.clear();
		script_paths.clear();
		combined_script_index = -1;
		return false;
	}

	return true;
}

String DependencyManifest::roots_cache_key(const Vector<String> &p_roots) {
	Vector<String> sorted = p_roots.duplicate();
	sorted.sort();
	String joined;
	for (const String &root : sorted) {
		joined += root;
		joined += "\n";
	}
	return joined.md5_text();
}

String DependencyManifest::cache_path_for_roots(const Vector<String> &p_roots) {
	String dir = GLOBAL_GET("io/batch_loader/depcache_path");
	if (!dir.ends_with("/")) {
		dir += "/";
	}
	return dir + roots_cache_key(p_roots) + ".depcache";
}

// Deliberately a plain project resource, not res://.godot/, so export_presets.cfg can ship it.
String DependencyManifest::baked_cache_path_for_roots(const Vector<String> &p_roots) {
	String dir = GLOBAL_GET("io/batch_loader/depcache_baked_path");
	if (!dir.ends_with("/")) {
		dir += "/";
	}
	return dir + roots_cache_key(p_roots) + ".depcache";
}

bool DependencyManifest::save_cache(const Vector<String> &p_roots) const {
	String path = cache_path_for_roots(p_roots);
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());

	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (err != OK || f.is_null()) {
		ERR_PRINT(vformat("DependencyManifest: failed to write cache for %d root(s)", p_roots.size()));
		return false;
	}

	int n = paths.size();

	f->store_32(DEPENDENCY_MANIFEST_CACHE_MAGIC);
	f->store_32(DEPENDENCY_MANIFEST_CACHE_VERSION);
	f->store_32((uint32_t)n);

	for (int i = 0; i < n; i++) {
		f->store_pascal_string(paths[i]);
		f->store_64(paths[i].is_empty() ? 0 : FileAccess::get_modified_time(paths[i]));
		f->store_32((uint32_t)in_degree[i]);
	}

	f->store_32((uint32_t)initial_ready.size());
	for (int idx : initial_ready) {
		f->store_32((uint32_t)idx);
	}

	f->store_32((uint32_t)script_paths.size());
	for (const String &script_path : script_paths) {
		f->store_pascal_string(script_path);
		f->store_64(FileAccess::get_modified_time(script_path));
	}
	f->store_32((uint32_t)combined_script_index);

	for (int i = 0; i < n; i++) {
		f->store_32((uint32_t)dependents[i].size());
		for (int dep : dependents[i]) {
			f->store_32((uint32_t)dep);
		}
	}

	return true;
}

String DependencyManifest::pck_reference_path() {
	String exec_path = OS::get_singleton()->get_executable_path();
	if (exec_path.is_empty()) {
		return String();
	}
	String candidate = exec_path.get_base_dir().path_join(exec_path.get_file().get_basename() + ".pck");
	if (FileAccess::exists(candidate)) {
		return candidate;
	}
	return exec_path;
}

bool DependencyManifest::load_cache_file(const String &p_path, DependencyManifest &r_manifest, bool p_verify) {
	if (!FileAccess::exists(p_path)) {
		return false;
	}

	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	if (err != OK || f.is_null()) {
		return false;
	}

	if (f->get_32() != DEPENDENCY_MANIFEST_CACHE_MAGIC || f->get_32() != DEPENDENCY_MANIFEST_CACHE_VERSION) {
		return false;
	}

	uint32_t n = f->get_32();

	Vector<String> loaded_paths;
	Vector<uint64_t> stored_mtimes;
	Vector<int> loaded_in_degree;
	loaded_paths.resize(n);
	stored_mtimes.resize(n);
	loaded_in_degree.resize(n);

	for (uint32_t i = 0; i < n; i++) {
		loaded_paths.write[i] = f->get_pascal_string();
		stored_mtimes.write[i] = f->get_64();
		loaded_in_degree.write[i] = (int)f->get_32();
	}

	if (p_verify) {
		for (uint32_t i = 0; i < n; i++) {
			if (loaded_paths[i].is_empty()) {
				continue;
			}
			if (!FileAccess::exists(loaded_paths[i]) || FileAccess::get_modified_time(loaded_paths[i]) != stored_mtimes[i]) {
				return false;
			}
		}
	}

	uint32_t initial_ready_count = f->get_32();
	Vector<int> loaded_initial_ready;
	loaded_initial_ready.resize(initial_ready_count);
	for (uint32_t i = 0; i < initial_ready_count; i++) {
		loaded_initial_ready.write[i] = (int)f->get_32();
	}

	uint32_t script_count = f->get_32();
	Vector<String> loaded_script_paths;
	Vector<uint64_t> loaded_script_mtimes;
	loaded_script_paths.resize(script_count);
	loaded_script_mtimes.resize(script_count);
	for (uint32_t i = 0; i < script_count; i++) {
		loaded_script_paths.write[i] = f->get_pascal_string();
		loaded_script_mtimes.write[i] = f->get_64();
	}
	int loaded_combined_script_index = (int)f->get_32();

	if (p_verify) {
		for (uint32_t i = 0; i < script_count; i++) {
			if (!FileAccess::exists(loaded_script_paths[i]) || FileAccess::get_modified_time(loaded_script_paths[i]) != loaded_script_mtimes[i]) {
				return false;
			}
		}
	}

	Vector<Vector<int>> loaded_dependents;
	loaded_dependents.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t dep_count = f->get_32();
		Vector<int> deps;
		deps.resize(dep_count);
		for (uint32_t j = 0; j < dep_count; j++) {
			deps.write[j] = (int)f->get_32();
		}
		loaded_dependents.write[i] = deps;
	}

	if (f->get_error() != OK) {
		return false;
	}

	r_manifest.paths = loaded_paths;
	r_manifest.in_degree = loaded_in_degree;
	r_manifest.initial_ready = loaded_initial_ready;
	r_manifest.script_paths = loaded_script_paths;
	r_manifest.combined_script_index = loaded_combined_script_index;
	r_manifest.dependents = loaded_dependents;
	return true;
}

bool DependencyManifest::try_load_cache(const Vector<String> &p_roots, DependencyManifest &r_manifest) {
	if (load_cache_file(baked_cache_path_for_roots(p_roots), r_manifest, /*p_verify=*/false)) {
		return true;
	}

	String live_path = cache_path_for_roots(p_roots);
	if (!FileAccess::exists(live_path)) {
		return false;
	}

	if (ProjectSettings::get_singleton()->is_using_datapack()) {
		// Packed files report mtime 0, so compare the cache's mtime to the .pck's instead.
		String pck_path = pck_reference_path();
		if (!pck_path.is_empty() && FileAccess::get_modified_time(live_path) < FileAccess::get_modified_time(pck_path)) {
			return false;
		}
		return load_cache_file(live_path, r_manifest, /*p_verify=*/false);
	}

	return load_cache_file(live_path, r_manifest, /*p_verify=*/true);
}

bool DependencyManifest::load_or_build(const Vector<String> &p_roots, DependencyManifest &r_manifest) {
	String profile_key = roots_cache_key(p_roots);
	CharString profile_key_utf8 = profile_key.utf8();
	GodotProfileZoneDynamic("DependencyManifest::load_or_build", profile_key_utf8.get_data(), profile_key_utf8.length());

	if (try_load_cache(p_roots, r_manifest)) {
		return true;
	}

	DependencyManifest manifest;
	for (const String &root : p_roots) {
		scan_recursive(root, manifest.deps_of);
	}

	if (!manifest.build_execution_graph()) {
		return false;
	}

	manifest.save_cache(p_roots);
	r_manifest = manifest;
	return true;
}
