/**************************************************************************/
/*  audio_editor_plugin.cpp												  */
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

#include "audio_editor_debugger.h"

#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/text_edit.h"
#include "scene/gui/control.h"

void AudioEditorDebugger::_bind_methods() {
}

bool AudioEditorDebugger::has_capture(const String &p_capture) const {
	return p_capture == "audio";
}

bool AudioEditorDebugger::capture(const String &p_msg, const Array &p_data, int p_index) {

	if (p_msg == "audio:play") {

		uint64_t id = p_data[0];
		String stream_path = p_data[1];

		AudioInfo info;
		info.path = p_data[1];
		info.steam_path = p_data[2];

		player_map[id] = info;

		dirty = true;
		return true;
	}

	if (p_msg == "audio:stop") {
		uint64_t id = p_data[0];
		if (player_map.erase(id)) {
			dirty = true;
		}
		return true;
	}


	return false;
}

void AudioEditorDebugger::setup_session(int p_session_id) {

	Ref<EditorDebuggerSession> session = get_session(p_session_id);
	ERR_FAIL_COND(session.is_null());

	VBoxContainer *audio = memnew(VBoxContainer);
	audio->set_name(TTR("Audio"));
	text_info = memnew(TextEdit);
	audio->add_child(text_info);
	text_info->set_editable(false);
	text_info->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	text_info->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	
	refresh_timer = memnew(Timer);
	refresh_timer->set_wait_time(0.5);
	refresh_timer->connect("timeout", callable_mp(this, &AudioEditorDebugger::refresh_display));
	refresh_timer->set_autostart(true);
	audio->add_child(refresh_timer);

	session->add_session_tab(audio);

}

void AudioEditorDebugger::refresh_display() {
	if (!dirty) {
		return;
	}

	dirty = false;
	String out = "Total Playing: " + itos(player_map.size()) + "\n";

	for (auto x : player_map) {
		AudioInfo info = x.value;
		out += info.path + " - " + info.steam_path + "\n";
	}

	text_info->set_text(out);

}
