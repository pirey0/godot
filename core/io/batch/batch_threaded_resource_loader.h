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
#include "core/io/batch/dependency_manifest.h"
#include "core/object/ref_counted.h"
#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class BatchThreadedResourceLoader : public Object {
	GDCLASS(BatchThreadedResourceLoader, Object);

	struct Request {
		Vector<String> roots;
		Ref<BatchLoadToken> token;
	};

	static inline BatchThreadedResourceLoader *singleton = nullptr;

	BinaryMutex queue_mutex;
	ConditionVariable queue_cv;
	List<Request> pending_queue;
	bool exit_thread = false;
	Thread worker_thread;

	const DependencyManifest *current_manifest = nullptr;
	BatchLoadToken *current_token = nullptr;

	Mutex current_results_mutex; // Guards current_results (write-only, keyed by path).
	Dictionary current_results;

	Mutex scheduling_mutex; // Guards remaining_in_degree (read-modify-write per completion).
	Vector<int> remaining_in_degree;

	SafeNumeric<int> pending_count; // Nodes dispatched but not finished; zero means no work left.
	Semaphore completion_semaphore;
	SafeFlag current_batch_failed;

	static void _thread_func(void *p_userdata);
	void _run();
	void _process_request(Request &p_request);
	void _dispatch_index(int p_index);
	void _load_index(int p_index);
	bool _load_resource(const String &p_path);
	void _run_combined_script_node(int p_index);
	void _finish_index(int p_index, bool p_success);

protected:
	static void _bind_methods();

public:
	static BatchThreadedResourceLoader *get_singleton() { return singleton; }

	Ref<BatchLoadToken> load(const Vector<String> &p_roots);

	BatchThreadedResourceLoader();
	~BatchThreadedResourceLoader();
};
