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
	// WorkerThreadPool does this for its own workers automatically; this plain OS thread needs it explicit.
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

void BatchThreadedResourceLoader::_dispatch_index(int p_index) {
	WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &BatchThreadedResourceLoader::_load_index).bind(p_index));
}

bool BatchThreadedResourceLoader::_load_resource(const String &p_path) {
	CharString profile_path_utf8 = p_path.utf8();
	GodotProfileZoneDynamic("BatchThreadedResourceLoader::_load_resource", profile_path_utf8.get_data(), profile_path_utf8.length());

	// load_inline(), not load(): avoids registering a second, competing pool task per item.
	Error err = OK;
	Ref<Resource> res = ResourceLoader::load_inline(p_path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &err);
	bool success = err == OK && res.is_valid();

	if (!success) {
		ERR_PRINT(vformat("BatchThreadedResourceLoader: failed to load '%s' (err=%d, res_null=%s)", p_path, (int)err, res.is_null()));
	} else {
		{
			MutexLock lock(current_results_mutex);
			current_results[p_path] = res;
		}
		current_token->mark_items_completed(1);
	}
	return success;
}

void BatchThreadedResourceLoader::_load_index(int p_index) {
	bool success = _load_resource(current_manifest->paths[p_index]);
	_finish_index(p_index, success);
}

void BatchThreadedResourceLoader::_run_combined_script_node(int p_index) {
	GodotProfileZone("BatchThreadedResourceLoader: combined script node");

	bool all_success = true;
	for (const String &path : current_manifest->script_paths) {
		if (current_batch_failed.is_set()) {
			all_success = false;
			break;
		}
		if (!_load_resource(path)) {
			all_success = false;
		}
	}

	_finish_index(p_index, all_success);
}

void BatchThreadedResourceLoader::_finish_index(int p_index, bool p_success) {
	if (!p_success) {
		current_batch_failed.set();
	}

	Vector<int> newly_ready;
	if (p_success) {
		MutexLock lock(scheduling_mutex);
		for (int dependent : current_manifest->dependents[p_index]) {
			remaining_in_degree.write[dependent] -= 1;
			if (remaining_in_degree[dependent] == 0) {
				newly_ready.push_back(dependent);
			}
		}
	}

	if (!current_batch_failed.is_set()) {
		for (int idx : newly_ready) {
			pending_count.add(1);
			_dispatch_index(idx);
		}
	}

	// Reaches zero exactly when no more dispatched work remains anywhere.
	if (pending_count.add(-1) == 0) {
		completion_semaphore.post();
	}
}

void BatchThreadedResourceLoader::_process_request(Request &p_request) {
	GodotProfileZone("BatchThreadedResourceLoader::_process_request");

	Ref<BatchLoadToken> token = p_request.token;
	token->set_status(BatchLoadToken::STATUS_SCANNING);

	DependencyManifest manifest;
	if (!DependencyManifest::load_or_build(p_request.roots, manifest)) {
		ERR_PRINT("BatchThreadedResourceLoader: dependency cycle detected in requested roots, aborting batch.");
		token->set_status(BatchLoadToken::STATUS_FAILED);
		token->call_deferred(SNAME("emit_signal"), SNAME("completed"));
		return;
	}

	int total = manifest.paths.size();
	token->set_total(total);
	token->set_status(BatchLoadToken::STATUS_LOADING);

	current_manifest = &manifest;
	current_token = token.ptr();
	current_results.clear();
	current_batch_failed.clear();
	pending_count.set(0);

	remaining_in_degree = manifest.in_degree;

	if (total > 0) {
		pending_count.add(1);

		{
			GodotProfileZone("BatchThreadedResourceLoader: dispatch initial ready set");
			for (int idx : manifest.initial_ready) {
				if (idx == manifest.combined_script_index) {
					continue;
				}
				pending_count.add(1);
				_dispatch_index(idx);
			}
		}

		if (manifest.combined_script_index != -1) {
			// Runs serially on this thread, concurrently with everything dispatched to the pool above.
			pending_count.add(1);
			_run_combined_script_node(manifest.combined_script_index);
		}

		if (pending_count.add(-1) == 0) {
			completion_semaphore.post();
		}

		{
			GodotProfileZone("BatchThreadedResourceLoader: wait for completion");
			completion_semaphore.wait();
		}
	}

	current_manifest = nullptr;
	current_token = nullptr;

	if (current_batch_failed.is_set()) {
		token->set_status(BatchLoadToken::STATUS_FAILED);
	} else {
		token->set_results(current_results.duplicate());
		token->set_status(BatchLoadToken::STATUS_DONE);
	}
	// Clear now so this member never outlives the request
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
