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
static const uint32_t DEPENDENCY_MANIFEST_CACHE_VERSION = 1;

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
		// Already visited, or currently being visited higher up the call stack
		// (a genuine cycle in the resource data) -- either way, stop recursing.
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

DependencyManifest DependencyManifest::merge(const Vector<DependencyManifest> &p_subgraphs) {
	DependencyManifest merged;
	for (const DependencyManifest &sub : p_subgraphs) {
		for (const KeyValue<String, Vector<String>> &kv : sub.deps_of) {
			if (!merged.deps_of.has(kv.key)) {
				merged.deps_of.insert(kv.key, kv.value);
			}
		}
	}
	return merged;
}

bool DependencyManifest::compute_layers() {
	layers.clear();

	HashMap<String, int> in_degree;
	HashMap<String, Vector<String>> dependents_of;

	for (const KeyValue<String, Vector<String>> &kv : deps_of) {
		in_degree.insert(kv.key, 0);
		dependents_of.insert(kv.key, Vector<String>());
	}

	for (const KeyValue<String, Vector<String>> &kv : deps_of) {
		for (const String &dep : kv.value) {
			if (!in_degree.has(dep)) {
				continue; // Pruned/missing dependency; shouldn't happen after scan_recursive.
			}
			in_degree[kv.key] += 1;
			dependents_of[dep].push_back(kv.key);
		}
	}

	Vector<String> current_layer;
	for (const KeyValue<String, int> &kv : in_degree) {
		if (kv.value == 0) {
			current_layer.push_back(kv.key);
		}
	}

	uint32_t processed = 0;
	while (current_layer.size() > 0) {
		layers.push_back(current_layer);
		processed += current_layer.size();

		Vector<String> next_layer;
		for (const String &path : current_layer) {
			for (const String &dependent : dependents_of[path]) {
				in_degree[dependent] -= 1;
				if (in_degree[dependent] == 0) {
					next_layer.push_back(dependent);
				}
			}
		}
		current_layer = next_layer;
	}

	if (processed < deps_of.size()) {
		// Leftover nodes never reached in-degree 0: a genuine cycle in the asset data.
		layers.clear();
		return false;
	}

	return true;
}

String DependencyManifest::cache_path_for_root(const String &p_root) {
	String dir = GLOBAL_GET("io/batch_loader/depcache_path");
	if (!dir.ends_with("/")) {
		dir += "/";
	}
	return dir + p_root.md5_text() + ".depcache";
}

// The configured directory is deliberately expected to be a plain project resource, not under
// res://.godot/ -- that directory is editor-only housekeeping and is never scanned by the
// exporter, so a cache generated there could never ship with the game. A plain res:// path can
// be pre-generated at build time (see Game.gd's "generate_depcache" cmd), copied into place by
// the build pipeline, and bundled into the exported package via an export_presets.cfg
// include_filter entry, letting real players' first launch read a precomputed cache instead of
// paying for the dependency scan themselves.
String DependencyManifest::baked_cache_path_for_root(const String &p_root) {
	String dir = GLOBAL_GET("io/batch_loader/depcache_baked_path");
	if (!dir.ends_with("/")) {
		dir += "/";
	}
	return dir + p_root.md5_text() + ".depcache";
}

bool DependencyManifest::save_cache(const String &p_root) const {
	String path = cache_path_for_root(p_root);
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());

	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (err != OK || f.is_null()) {
		ERR_PRINT(vformat("DependencyManifest: failed to write cache for root '%s'", p_root));
		return false;
	}

	f->store_32(DEPENDENCY_MANIFEST_CACHE_MAGIC);
	f->store_32(DEPENDENCY_MANIFEST_CACHE_VERSION);
	f->store_32((uint32_t)deps_of.size());

	for (const KeyValue<String, Vector<String>> &kv : deps_of) {
		f->store_pascal_string(kv.key);
		f->store_64(FileAccess::get_modified_time(kv.key));
		f->store_32((uint32_t)kv.value.size());
		for (const String &dep : kv.value) {
			f->store_pascal_string(dep);
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
	return exec_path; // embed_pck=true builds, or a pck-less dev/editor run.
}

bool DependencyManifest::load_cache_file(const String &p_path, const String &p_root, DependencyManifest &r_manifest, bool p_verify) {
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

	uint32_t node_count = f->get_32();
	HashMap<String, Vector<String>> loaded_deps_of;

	for (uint32_t i = 0; i < node_count; i++) {
		String path = f->get_pascal_string();
		uint64_t stored_mtime = f->get_64();
		uint32_t dep_count = f->get_32();

		Vector<String> deps;
		deps.resize(dep_count);
		for (uint32_t j = 0; j < dep_count; j++) {
			deps.write[j] = f->get_pascal_string();
		}

		if (p_verify && (!FileAccess::exists(path) || FileAccess::get_modified_time(path) != stored_mtime)) {
			return false; // Stale -- something in the transitive closure changed on disk.
		}

		loaded_deps_of.insert(path, deps);
	}

	if (!loaded_deps_of.has(p_root)) {
		return false;
	}

	r_manifest.deps_of = loaded_deps_of;
	return true;
}

bool DependencyManifest::try_load_cache(const String &p_root, DependencyManifest &r_manifest) {
	// Baked seed: the pipeline only ever puts a fresh, correct-by-construction cache here
	// right before export, so if it exists for this root, trust it wholesale -- no per-file
	// check needed or wanted (see the cost discussion that led here: this closure can run to
	// thousands of files and gigabytes, so verifying it on every launch isn't viable).
	if (load_cache_file(baked_cache_path_for_root(p_root), p_root, r_manifest, /*p_verify=*/false)) {
		return true;
	}

	String live_path = cache_path_for_root(p_root);
	if (!FileAccess::exists(live_path)) {
		return false;
	}

	if (ProjectSettings::get_singleton()->is_using_datapack()) {
		// Once every dependency is read out of a packed .pck, FileAccessPack reports mtime 0
		// for all of them, so the per-file check below can never be trusted here. Instead,
		// check one cheap thing: is the live cache file itself newer than the .pck it would
		// have been generated against? If so, nothing in the (immutable, for this build's
		// lifetime) pack could have changed since -- trust the whole cache. If the .pck is
		// newer, a new build/update landed since this cache was last written, so it's stale.
		String pck_path = pck_reference_path();
		if (!pck_path.is_empty() && FileAccess::get_modified_time(live_path) < FileAccess::get_modified_time(pck_path)) {
			return false;
		}
		return load_cache_file(live_path, p_root, r_manifest, /*p_verify=*/false);
	}

	// Loose/dev: a real filesystem with real per-file mtimes -- check each one as before.
	return load_cache_file(live_path, p_root, r_manifest, /*p_verify=*/true);
}

DependencyManifest DependencyManifest::load_or_build(const String &p_root) {
	CharString profile_root_utf8 = p_root.utf8();
	GodotProfileZoneDynamic("DependencyManifest::load_or_build", profile_root_utf8.get_data(), profile_root_utf8.length());

	DependencyManifest manifest;
	if (try_load_cache(p_root, manifest)) {
		return manifest;
	}

	manifest.deps_of.clear();
	scan_recursive(p_root, manifest.deps_of);
	manifest.save_cache(p_root);
	return manifest;
}
