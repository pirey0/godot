# GDScript devirtualization via explicit `@virtual` — design doc

Status: **draft for discussion**. Target: the `pvkk-godot` fork (Godot 4.6).

## 1. Goal

Make `.gd → .gd` method calls cheaper by removing the per-call **name-based
dynamic dispatch** for methods that are declared non-overridable. Introduce an
explicit `@virtual` annotation; a method **without** it is a compile-time
contract that it is never overridden, so any statically-resolved call to it can
jump straight to the target `GDScriptFunction` — no `StringName` lookup, no
class-chain walk, no per-call-site cache, no type guard.

This is the *caller-side* fix. It is a prerequisite for, but distinct from, a
later *callee-side* fix (inlining / unboxed frames) — see §9 phasing.

## 2. What we're actually paying today (measured)

On the PVKK combat capture: **~7,560 GDScript calls/frame**, ~**0.21 µs/call**
pure overhead ⇒ **~1.6–2.0 ms/frame (≈9–11% of the frame)** in call machinery
before any function body runs.

A `.gd → .gd` call pays on both sides (release path, `DEBUG_ENABLED` off):

- **Caller** — `gdscript_vm.cpp`
  - `OPCODE_CALL` (≈1893): `base->callp(StringName, …)` → `Object::callp` →
    dynamic dispatch by name.
  - `OPCODE_CALL_SELF_BASE` (≈2484): even a `self` call does
    `member_functions.find(methodname)` (a `HashMap<StringName,
    GDScriptFunction*>` lookup) **and walks the base-class chain**
    (`while (gds->base.ptr())`). No per-call-site cache.
- **Callee** — `GDScriptFunction::call` (≈498)
  - `alloca(frame)` then `memnew_placement(&stack[i], Variant(...))` for **every
    argument and every stack slot** (~574–625), plus the `self` Variant, all
    destructed on return. Every local is a boxed 24-byte `Variant`.

`@virtual` devirtualization removes the **caller** cost for the resolvable,
non-virtual subset. The callee cost is Phase 2.

## 3. Semantics

### 3.1 Default and the contract
Two options for the default (see §7 — this is the main open question):

- **(A) Opt-in per class**: a class marked `@strict` (name TBD) makes its
  methods non-virtual by default; `@virtual` re-enables overriding per method.
  Backwards-compatible: untouched code behaves exactly as today.
- **(B) Global flip**: non-virtual by default everywhere, `@virtual` to allow
  override. Maximum benefit, **breaks every existing override** in the
  ecosystem. Not viable as an upstream default; only conceivable as a
  project-level setting for a closed codebase.

Recommended for the fork: **(A)**, so PVKK opts hot classes in incrementally and
nothing else regresses.

**The contract:** a non-virtual method **must not be overridden**. Enforced by
the analyzer *from the subclass side* (§4.2), which is what makes this tractable
under GDScript's separate compilation — we never try to *prove* non-override,
we *declare* it and reject violations where they're visible.

### 3.2 What stays virtual regardless
- **Engine virtuals** (`_ready`, `_process`, `_input`, `_notification`, `_init`,
  …). These are called *by name from C++*; the annotation/opcode change never
  touches that path. The analyzer already knows Node's virtual set, so
  overriding them is always allowed without `@virtual`.
- **`Callable` / signal targets / `.call()` / `.callv()` / duck-typed access on
  a `Variant`**. Resolved by name at runtime; unaffected. The method stays in
  `member_functions` and remains name-callable — we only *additionally* emit
  direct calls where the compiler can prove the target.
- **`super.foo()`** — already statically bound to the parent; keep its existing
  opcode (can also be direct, cheap follow-up).

### 3.3 When a call is devirtualized
At a call site `recv.foo(args)` (or bare `foo(args)` = `self.foo(args)`), emit
the direct opcode iff **all** hold:
1. `foo` resolves to a **GDScript** method (not native / builtin / utility).
2. `foo` is **not** `@virtual` (and is not an engine virtual).
3. The **static type of `recv` is known** and resolves `foo` to exactly one
   `GDScriptFunction`:
   - `self.foo()` / bare `foo()` → always known (own class + base chain).
   - `var x: SomeClass; x.foo()` → known.
   - `var x = untyped; x.foo()` → **unknown → fall back to `OPCODE_CALL`.**

So the payoff scales with **static typing of receivers**. Self-calls are free;
everything else is "bought" with type hints.

## 4. Implementation, file by file

### 4.1 Parser — `gdscript_parser.{h,cpp}`
- **Register the annotation.** Mirror `@abstract` (`gdscript_parser.cpp:152`,
  `AnnotationInfo::FUNCTION`):
  ```cpp
  register_annotation(MethodInfo("@virtual"), AnnotationInfo::FUNCTION,
      &GDScriptParser::virtual_annotation);
  ```
  Add `virtual_annotation()` (trivial: set a flag, like `abstract_annotation`).
- **Store on the node.** `FunctionNode` (`gdscript_parser.h:850`) already carries
  `is_static`, `rpc_config` (:860). Add `bool is_virtual = false;` set by the
  annotation. (For option A, also a class-level `bool strict_dispatch` on
  `ClassNode`.)
- Cost: **XS**. This is a well-trodden path.

### 4.2 Analyzer — `gdscript_analyzer.cpp` (the semantic heart)
- **Override enforcement.** When resolving a class's functions (around the
  per-function analysis at :1873), for each method that shadows a base-class
  method, look up the base method and **error if the base method is
  non-virtual** ("Cannot override non-`@virtual` method 'foo' declared in
  'Base'"). This is the whole safety story and the thing that lets callers trust
  the contract. Needs base-class method lookup, which the analyzer already does
  for signature checks (`get_function_signature`, :5764).
  - Edge: multi-level inheritance — walk to the *first* declaration; virtualness
    is inherited (once `@virtual`, always overridable down the chain).
  - Edge: a non-virtual method that *happens* to share a name with an engine
    virtual — treat engine virtuals as implicitly virtual (allow).
- **Mark the call site.** `reduce_call` (:3226) already computes the callee's
  base type and `MethodFlags`. Extend it to record, on the `CallNode`, a
  "direct-callable" resolution when §3.3 holds: the target's declaring
  class/script + method name (enough for codegen to request a slot). Add a
  `MethodFlags::METHOD_FLAG_VIRTUAL`-style bit or a dedicated field.
- Cost: **L**. This is where correctness lives; most review effort goes here.

### 4.3 Compiler — `gdscript_compiler.cpp`
- In the `CALL` case (:603–680), where it currently chooses between
  `write_call_method_bind_validated` (:649, native), `write_call_self` (:666),
  and the generic `write_call` (:660): add a branch that, when the analyzer
  marked the call direct-callable (§4.2), calls a new
  `gen->write_call_gdscript_direct(result, base, target_ref, args)`.
- Cost: **S** — one new branch, gated by the analyzer's flag.

### 4.4 Byte codegen + function tables — `gdscript_byte_codegen.{h,cpp}`, `gdscript_function.h`
- **A callee-reference table**, parallel to `method_bind_map` →
  `methods[]` (`gdscript_byte_codegen.h:123,339`; `gdscript_function.h:512`
  `_methods_count`). Add `_gdscript_calls_ptr/_count` to `GDScriptFunction`
  holding entries that describe the target: `{ target script/class ref, method
  StringName }`, and a resolved-cache slot `GDScriptFunction *resolved = nullptr`.
- `write_call_gdscript_direct` appends the new opcode + an index into that table.
- Cost: **M** — mechanical but touches serialization of `GDScriptFunction` (the
  bytecode has a save/load path that must learn the new table).

### 4.5 Linking / resolution (the genuinely hard part)
The target `GDScriptFunction*` may not exist at compile time (cross-script,
lazy-loaded). Strategy:

- **Same-script / base-chain targets** (the common case incl. all self-calls):
  resolve at the script's own link step — after its `member_functions` are built
  — to a raw `GDScriptFunction*`. Zero runtime cost thereafter.
- **Cross-script targets**: resolve **lazily on first execution** and cache in
  the table's `resolved` slot. Because the method is non-virtual, the target is
  **unique and immutable** — so this is a *resolve-once*, **not** a polymorphic
  inline cache: no type guard on subsequent calls. (This is precisely the "skip
  the cache" property.)
- **Invalidation**: on `@tool`/hot-reload or script reload, clear resolved slots
  for affected scripts. GDScript already has reload machinery to hook.
- Cost: **M–L**, mostly around reload correctness.

### 4.6 VM — `gdscript_vm.cpp`, `gdscript_function.h`
- Add `OPCODE_CALL_GDSCRIPT_DIRECT` (+ `_RET`) to the opcode enum and the
  computed-goto table (the `&&OPCODE_*` block ~295).
- Handler: read the table index → get `resolved` (or resolve+cache once) →
  `target->call(base_instance, argptrs, argc, err)` directly, like the tail of
  `OPCODE_CALL_SELF_BASE` (:2484) **minus** the `member_functions.find` and the
  base-chain `while`. Copy the return into the target slot as today.
- Cost: **S–M**.

## 5. What this does *not* fix
- **Callee frame setup** (Variant boxing of args/locals). Untouched — that's
  Phase 2 (inlining or typed/unboxed frames).
- **Untyped-receiver calls.** Still `OPCODE_CALL`. The benefit tracks typing
  coverage.
- **Native / Callable / signal calls.** Already on their own paths.

## 6. Expected impact (recap, honest)
Caller dispatch is ~0.08–0.10 µs of the ~0.21 µs/call.
- **Devirt of ~75% of calls** (self + typed): ~0.09 µs × ~5,700 ≈ **~0.5
  ms/frame**.
- **+ Phase 2 inlining** of the small resolved ones: up to **~1.0–1.3 ms/frame**
  total of the 1.6–2.0 ms.
- Native/untyped/virtual/Variant calls remain — so this is **not** "whole frame
  to zero," it's "zero out the resolvable small-call subset." Payoff is roughly
  proportional to how much you type the hot receivers.

## 7. Backwards compatibility — the main decision
- **Option A (opt-in class annotation)** — recommended. No existing code
  changes behavior. PVKK marks hot classes `@strict` and types their hot
  receivers. Upstreamable.
- **Option B (global default flip)** — biggest automatic win, but a
  language-breaking change; only as a per-project setting, never an upstream
  default.

## 8. Risks
- **Silent wrong-dispatch** if the contract is under-enforced (a subclass in a
  file the analyzer didn't see at the call site). Mitigation: the enforcement is
  from the *subclass* compilation, which always sees its base — so the override
  is rejected where it's written; a call site can trust any non-virtual method.
  The residual risk is dynamic tricks (`set_script`, cross-language subclasses)
  — for those we must **fall back to name dispatch** when the runtime type isn't
  the compile-time type. Needs a cheap runtime assertion in debug builds.
- **Reload / `@tool`** cache-clearing correctness.
- **Bytecode format bump** (the new table) — versioned load path.
- **Analyzer complexity** — the override rule interacts with abstract classes,
  multiple inheritance chains, inner classes, lambdas.

## 9. Phasing
1. **Phase 0 — inline cache (optional down-payment).** A monomorphic cache on
   `OPCODE_CALL`/`_SELF_BASE` ships independently, helps *all* repeated calls
   (incl. untyped), no language change. Good de-risking step and immediately
   useful. (`@virtual` later supersedes it for the resolved subset.)
2. **Phase 1 — `@virtual` + direct-call opcode** (this doc). Caller-side win.
3. **Phase 2 — leaf inliner.** Splice small non-virtual resolved callees into
   the caller (remap stack addresses, bind args, turn `return` into a jump).
   Kills the callee frame too → the call *vanishes*. Depends on Phase 1's
   resolution.

## 10. Effort estimate
| Component | Effort |
|---|---|
| Parser: annotation + node flag | XS |
| Analyzer: override enforcement + call-site marking | **L** |
| Compiler: call codegen branch | S |
| Byte codegen + function table + serialization | M |
| Linking / lazy resolution + reload | M–L |
| VM: new opcode(s) | S–M |
| Tests (unit + GDScript test suite + reload) | M |
| **Phase 1 total** | **~3–5 focused weeks, one engine-fluent dev** |
| Phase 0 inline cache (standalone) | ~1 week |
| Phase 2 inliner | ~4–8 weeks |

## 11. Open questions to discuss
1. **Opt-in (A) vs global (B)** — for the fork specifically. A is safe; B is
   bigger but a migration project.
2. **Runtime safety check** for `set_script`/cross-language subclassing — always
   on, debug-only, or rely on the analyzer + fall back?
3. **Do we even want Phase 1 without Phase 2?** ~0.5 ms alone vs ~1.3 ms with the
   inliner — is the caller-only win worth shipping first, or bundle?
4. **vs GDExtension.** Is this fork investment (3–5 wk Phase 1, +4–8 wk Phase 2)
   worth it over porting the ~5 hottest per-object systems to C++ for a
   comparable, lower-risk frame win this cycle? (My current lean: GDExtension to
   ship, `@virtual` as the long-game upstream contribution.)
