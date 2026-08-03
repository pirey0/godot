extends RefCounted
# Exhaustive opcode exercise for gds2cpp. Each t_* function is pure (args in,
# value out) so the harness can compare interpreted vs transpiled results.
# Naming: t_<area>. The harness enumerates every "t_" method.

var m_int: int = 7
var m_str: String = "member"
var m_arr: Array = [1, 2, 3]
var m_dict: Dictionary = {"a": 1}

# --- operators (validated + generic) ---
func t_op_add(a: int, b: int) -> int: return a + b
func t_op_arith(a: float, b: float) -> float: return a * b - a / b + fmod(a, b)
func t_op_cmp(a: int, b: int) -> bool: return a < b and a <= b or a > b and a >= b
func t_op_eq(a, b) -> bool: return a == b or a != b
func t_op_bit(a: int, b: int) -> int: return (a & b) | (a ^ b) << 1 >> 1
func t_op_not(a: bool) -> bool: return not a
func t_op_neg(a: int) -> int: return -a
func t_op_mod(a: int, b: int) -> int: return a % b
func t_op_in(a, arr: Array) -> bool: return a in arr
func t_op_str_concat(a: String, b: String) -> String: return a + b

# --- constructs / builtins ---
func t_construct_vec2(x: float, y: float) -> Vector2: return Vector2(x, y)
func t_construct_vec3(x: float, y: float, z: float) -> Vector3: return Vector3(x, y, z)
func t_construct_color(r: float, g: float) -> Color: return Color(r, g, 0.0)
func t_construct_array() -> Array: return [1, 2, 3, "four"]
func t_construct_dict() -> Dictionary: return {"x": 1, "y": 2}
func t_typed_array() -> Array[int]: return [1, 2, 3]
func t_typed_dict() -> Dictionary:
	var d: Dictionary = {}
	d[1] = "one"
	return d

# --- indexed / keyed / named ---
func t_get_indexed(arr: Array, i: int): return arr[i]
func t_set_indexed(arr: Array, i: int, v):
	arr[i] = v
	return arr
func t_get_keyed(d: Dictionary, k): return d[k]
func t_set_keyed(d: Dictionary, k, v):
	d[k] = v
	return d
func t_get_named(v: Vector2) -> float: return v.x
func t_set_named(v: Vector2, x: float) -> Vector2:
	v.x = x
	return v

# --- member access ---
func t_get_member() -> int: return m_int
func t_set_member(v: int) -> int:
	m_int = v
	return m_int
func t_member_arr() -> Array: return m_arr
func t_member_dict_str() -> String: return m_str

# --- casts ---
func t_cast_int(x) -> int: return x as int
func t_cast_float(x) -> float: return x as float
func t_cast_string(x) -> String: return str(x)

# --- type tests ---
func t_is_builtin(x) -> bool: return x is int
func t_is_array(x) -> bool: return x is Array
func t_is_string(x) -> bool: return x is String

# --- control flow ---
func t_if(a: int) -> String:
	if a > 10: return "big"
	elif a > 0: return "small"
	else: return "neg"
func t_while(n: int) -> int:
	var s := 0
	var i := 0
	while i < n:
		s += i
		i += 1
	return s
func t_ternary(a: int) -> String: return "yes" if a > 0 else "no"

# --- iteration (typed) ---
func t_for_range(n: int) -> int:
	var s := 0
	for i in n: s += i
	return s
func t_for_range3(a: int, b: int, c: int) -> int:
	var s := 0
	for i in range(a, b, c): s += i
	return s
func t_for_array(arr: Array) -> int:
	var s := 0
	for x in arr: s += int(x)
	return s
func t_for_int_array(arr: Array[int]) -> int:
	var s := 0
	for x in arr: s += x
	return s
func t_for_float(arr: Array[float]) -> float:
	var s := 0.0
	for x in arr: s += x
	return s
func t_for_string(s: String) -> int:
	var n := 0
	for c in s: n += 1
	return n
func t_for_dict(d: Dictionary) -> Array:
	var keys := []
	for k in d: keys.append(k)
	return keys
func t_for_packed_int(arr: PackedInt32Array) -> int:
	var s := 0
	for x in arr: s += x
	return s
func t_for_packed_str(arr: PackedStringArray) -> String:
	var out := ""
	for x in arr: out += x
	return out
func t_for_vec2(arr: PackedVector2Array) -> float:
	var s := 0.0
	for v in arr: s += v.x
	return s

# --- calls ---
func t_call_utility(a: float) -> float: return abs(a) + sqrt(absf(a))
func t_call_len(arr: Array) -> int: return len(arr)
func t_call_self(a: int) -> int: return t_op_add(a, 1)
func t_call_builtin_static() -> Color: return Color.from_hsv(0.5, 1.0, 1.0)
func t_call_method(s: String) -> String: return s.to_upper().strip_edges()

# --- typed returns ---
func t_ret_int() -> int: return 42
func t_ret_array() -> Array[int]: return [9, 8, 7]
func t_ret_dict() -> Dictionary: return {"k": "v"}

# --- assert / misc ---
func t_assert(a: int) -> int:
	assert(a >= 0)
	return a * 2
func t_string_ops(s: String) -> String:
	return s.substr(0, 1).to_upper() + s.substr(1)
