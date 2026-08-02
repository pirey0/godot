# gds2cpp benchmark: interpreted Data.ofOr vs transpiled-C++ Gds2cppData.of_or.
# Run headless:  godot --headless -s modules/gds2cpp/bench.gd
extends SceneTree

var values := {}

# Interpreted copy of systems/data/Data.gd : ofOr (identical logic).
func _of_or(property, default):
	if values.has(property):
		return values[property]
	return values.get(property.to_lower(), default)

func _init():
	var N := 3000000
	values["power.output"] = 42.0
	values["battery.charge"] = 1234.0
	values["space.sizex"] = 4000.0

	var key := "power.output"      # fast-path hit
	var default = 0.0

	# ---- warm up (fill caches / trigger any lazy init) ----
	var warm = 0.0
	for i in 100000:
		warm += _of_or(key, default)

	# ---- interpreted: gd loop calling gd ofOr (the call-overhead we want to remove) ----
	var sink := 0.0
	var t0 := Time.get_ticks_usec()
	for i in N:
		sink += _of_or(key, default)
	var t_interp := Time.get_ticks_usec() - t0

	# ---- native: transpiled C++, loop internal (models a transpiled caller) ----
	var d := Gds2cppData.new()
	d.values = values
	d.bench_native(100000, key, default) # warm
	var t_native := d.bench_native(N, key, default)

	print("=== Data.ofOr : interpreted vs transpiled-C++ (faithful) ===")
	print("N = %d  key='%s' (fast-path hit)  sink=%.1f" % [N, key, sink])
	print("interpreted : %8.1f ms   %7.4f us/call" % [t_interp / 1000.0, t_interp / float(N)])
	print("native      : %8.1f ms   %7.4f us/call" % [t_native / 1000.0, t_native / float(N)])
	print("speedup     : %.2fx" % [t_interp / float(t_native)])
	quit()
