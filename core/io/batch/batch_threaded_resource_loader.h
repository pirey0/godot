/**************************************************************************/
/*  batch_threaded_resource_loader.h                                      */
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

#include "core/io/batch/batch_load_token.h"
#include "core/object/ref_counted.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

// Standalone batch-loading facade: a single persistent background thread
// consumes a FIFO queue of load(roots) requests. Each request's dependency
// graph is precomputed and topologically layered up front (see
// DependencyManifest), then dispatched bottom-up as WorkerThreadPool group
// tasks, one per layer -- every dependency a layer needs is already
// ResourceCache-resident by construction, so no load ever waits on a sibling.
// Items are loaded via ResourceLoader::load_inline(), which always runs on
// the calling (pool) thread instead of registering a second, competing task
// the way plain load() would from inside a pool task.
//
// .gd files are the one exception: GDScriptParser::get_dependencies() is an
// unimplemented stub (always empty), so a script's real extends/preload
// targets are invisible to this graph and can't be safely parallelized
// against each other -- concurrently compiling two interdependent scripts
// can pile both up on GDScriptCache's own global compile mutex with neither
// able to proceed. So within each layer, .gd paths are loaded serially, in
// order, right here on this thread -- never on the main thread, which would
// otherwise freeze for the whole script batch -- running concurrently with
// the parallel group task for everything else in the same layer.
// ScriptServer::thread_enter()/thread_exit() bracket this thread's lifetime,
// the same requirement WorkerThreadPool satisfies automatically for its own
// workers before running anything that touches scripting.
class BatchThreadedResourceLoader : public Object {
	GDCLASS(BatchThreadedResourceLoader, Object);

	struct Request {
		Vector<String> roots;
		Ref<BatchLoadToken> token;
	};

	static inline BatchThreadedResourceLoader *singleton = nullptr;

	BinaryMutex queue_mutex; // Non-recursive: paired with queue_cv, which requires it.
	ConditionVariable queue_cv;
	List<Request> pending_queue;
	bool exit_thread = false;
	Thread worker_thread;

	// Transient state for the layer currently being loaded. The queue is
	// strictly FIFO -- only one batch is ever in flight -- so plain members
	// are safe here; only the results dict needs a lock, since multiple
	// group-task workers write different keys into it concurrently.
	const Vector<String> *current_layer = nullptr;
	BatchLoadToken *current_token = nullptr;
	Mutex current_results_mutex;
	Dictionary current_results;
	SafeFlag current_layer_failed;

	static void _thread_func(void *p_userdata);
	void _run();
	void _process_request(Request &p_request);
	void _load_path(const String &p_path);
	void _load_one_item(int p_index);

protected:
	static void _bind_methods();

public:
	static BatchThreadedResourceLoader *get_singleton() { return singleton; }

	Ref<BatchLoadToken> load(const Vector<String> &p_roots);

	BatchThreadedResourceLoader();
	~BatchThreadedResourceLoader();
};
