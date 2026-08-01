/**************************************************************************/
/*  batch_threaded_resource_loader.cpp                                    */
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

#include "batch_threaded_resource_loader.h"

#include "core/io/batch/dependency_manifest.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_method_pointer.h"
#include "core/object/script_language.h"
#include "core/object/worker_thread_pool.h"
#include "core/profiling/profiling.h"

void BatchThreadedResourceLoader::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load", "roots"), &BatchThreadedResourceLoader::load);
}

void BatchThreadedResourceLoader::_thread_func(void *p_userdata) {
	((BatchThreadedResourceLoader *)p_userdata)->_run();
}

void BatchThreadedResourceLoader::_run() {
	// Required before this thread can safely load scripts: WorkerThreadPool does this
	// automatically for its own worker threads, but this is a plain OS thread, not a
	// pool worker, and .gd loads are routed here specifically (see _process_request).
	ScriptServer::thread_enter();

	while (true) {
		Request request;
		{
			MutexLock lock(queue_mutex);
			while (pending_queue.is_empty() && !exit_thread) {
				queue_cv.wait(lock);
			}
			if (pending_queue.is_empty() && exit_thread) {
				break;
			}
			request = pending_queue.front()->get();
			pending_queue.pop_front();
		}
		_process_request(request);
	}

	ScriptServer::thread_exit();
}

void BatchThreadedResourceLoader::_load_path(const String &p_path) {
	CharString profile_path_utf8 = p_path.utf8();
	GodotProfileZoneDynamic("BatchThreadedResourceLoader::_load_path", profile_path_utf8.get_data(), profile_path_utf8.length());

	// load_inline(), not load(): this may run inside a WorkerThreadPool group task,
	// and load() would otherwise register a second, competing task in the same
	// pool for every single item. See resource_loader.h for the full rationale.
	Error err = OK;
	Ref<Resource> res = ResourceLoader::load_inline(p_path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &err);
	if (err != OK || res.is_null()) {
		ERR_PRINT(vformat("BatchThreadedResourceLoader: failed to load '%s' (err=%d, res_null=%s)", p_path, (int)err, res.is_null()));
		current_layer_failed.set();
		return;
	}

	{
		MutexLock lock(current_results_mutex);
		current_results[p_path] = res;
	}
	current_token->mark_items_completed(1);
}

void BatchThreadedResourceLoader::_load_one_item(int p_index) {
	_load_path((*current_layer)[p_index]);
}

void BatchThreadedResourceLoader::_process_request(Request &p_request) {
	GodotProfileZone("BatchThreadedResourceLoader::_process_request");

	Ref<BatchLoadToken> token = p_request.token;
	token->set_status(BatchLoadToken::STATUS_SCANNING);

	Vector<DependencyManifest> subgraphs;
	for (const String &root : p_request.roots) {
		subgraphs.push_back(DependencyManifest::load_or_build(root));
	}

	DependencyManifest merged = DependencyManifest::merge(subgraphs);
	if (!merged.compute_layers()) {
		ERR_PRINT("BatchThreadedResourceLoader: dependency cycle detected in requested roots, aborting batch.");
		token->set_status(BatchLoadToken::STATUS_FAILED);
		token->call_deferred(SNAME("emit_signal"), SNAME("completed"));
		return;
	}

	uint32_t total = 0;
	for (const Vector<String> &layer : merged.layers) {
		total += layer.size();
	}
	token->set_total((int)total);
	token->set_status(BatchLoadToken::STATUS_LOADING);

	current_results.clear();
	current_token = token.ptr();
	bool failed = false;

	for (const Vector<String> &layer : merged.layers) {
		GodotProfileZone("BatchThreadedResourceLoader: layer");
		if (layer.is_empty()) {
			continue;
		}

		// .gd files are pulled out and loaded serially, right here, in order: GDScriptParser::
		// get_dependencies() never reports a script's real extends/preload targets, so scripts
		// can't be safely ordered or parallelized against each other via this graph. Everything
		// else still loads in parallel via the group task below, running concurrently with the
		// serial script loop.
		Vector<String> scripts;
		Vector<String> parallel_items;
		for (const String &path : layer) {
			if (path.get_extension() == "gd") {
				scripts.push_back(path);
			} else {
				parallel_items.push_back(path);
			}
		}

		current_layer = &parallel_items;
		current_layer_failed.clear();

		bool has_group = !parallel_items.is_empty();
		WorkerThreadPool::GroupID group = -1;
		if (has_group) {
			group = WorkerThreadPool::get_singleton()->add_group_task(
					callable_mp(this, &BatchThreadedResourceLoader::_load_one_item),
					parallel_items.size());
		}

		if (!scripts.is_empty()) {
			GodotProfileZone("BatchThreadedResourceLoader: serial script batch");
			for (const String &script_path : scripts) {
				_load_path(script_path);
				if (current_layer_failed.is_set()) {
					break;
				}
			}
		}

		if (has_group) {
			GodotProfileZone("BatchThreadedResourceLoader: wait for group task");
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		}

		if (current_layer_failed.is_set()) {
			failed = true;
			break;
		}
	}

	current_layer = nullptr;
	current_token = nullptr;

	if (failed) {
		token->set_status(BatchLoadToken::STATUS_FAILED);
	} else {
		// duplicate(), not a plain copy: Dictionary is a shared reference type in Godot
		// (like Array), so a plain assignment would leave current_results.clear() below
		// wiping out the token's results too, since they'd be the same underlying data.
		token->set_results(current_results.duplicate());
		token->set_status(BatchLoadToken::STATUS_DONE);
	}
	// current_results is a member (reused across requests via the FIFO queue), not
	// request-local -- clear it now so it never holds resource references (scripts,
	// in particular) any longer than the single request that produced them. Otherwise
	// it can end up as the last live reference to a Resource at engine shutdown, and
	// this class (living in core) is destroyed after modules like GDScript have
	// already finalized -- dropping a Script reference at that point crashes.
	current_results.clear();
	token->call_deferred(SNAME("emit_signal"), SNAME("completed"));
}

Ref<BatchLoadToken> BatchThreadedResourceLoader::load(const Vector<String> &p_roots) {
	Ref<BatchLoadToken> token;
	token.instantiate();

	Request request;
	request.roots = p_roots;
	request.token = token;

	{
		MutexLock lock(queue_mutex);
		pending_queue.push_back(request);
	}
	queue_cv.notify_one();

	return token;
}

BatchThreadedResourceLoader::BatchThreadedResourceLoader() {
	singleton = this;
	worker_thread.start(&BatchThreadedResourceLoader::_thread_func, this);
}

BatchThreadedResourceLoader::~BatchThreadedResourceLoader() {
	{
		MutexLock lock(queue_mutex);
		exit_thread = true;
	}
	queue_cv.notify_all();
	worker_thread.wait_to_finish();
	singleton = nullptr;
}
