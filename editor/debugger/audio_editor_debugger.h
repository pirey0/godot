/**************************************************************************/
/*  audio_editor_plugin.h									              */
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

#ifndef AUDIO_EDITOR_PLUGIN_H
#define AUDIO_EDITOR_PLUGIN_H

#include "editor/editor_plugin.h"
#include "editor/plugins/editor_debugger_plugin.h"

class TextEdit;
class Tree;

class AudioEditorDebugger : public EditorDebuggerPlugin {
	GDCLASS(AudioEditorDebugger, EditorDebuggerPlugin);

public:
	struct AudioInfo {
		ObjectID instance_id;
		String instance_path;
		String stream_path;
		unsigned char type = 0;
		float volume = 0.0f;
		StringName bus;
		Vector2 direction = Vector2();
		float playback_position = 0.0f;

		AudioInfo() {}
	};

private:
	Tree *tree = nullptr;
	Timer *refresh_timer = nullptr;
	bool dirty = false;

	HashMap<uint64_t, AudioInfo> player_map;

protected:
	static void _bind_methods();

	void refresh_display();

public:
	virtual bool has_capture(const String &p_capture) const override;
	virtual bool capture(const String &p_message, const Array &p_data, int p_index) override;
	virtual void setup_session(int p_session_id) override;

	AudioEditorDebugger() {}
};




#endif // AUDIO_EDITOR_PLUGIN_H
