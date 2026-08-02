# "IL2GDExtension" — AOT-transpile GDScript → C++ at export

Status: **draft for discussion**. Concept: keep normal interpreted GDScript in
the editor; at **export**, transpile the entire project's `.gd` to C++, compile
it into a GDExtension native library, and route the exported game to the native
code instead of the bytecode interpreter. Direct analog of Unity's IL2CPP
(IL → C++ → native).

## 1. The core idea and why it's different from the `@virtual` proposal

`@virtual` **optimizes the interpreter** (kills one class of per-call cost).
This **removes the interpreter** for the shipped build. The difference that
matters:

- **Zero developer effort per game.** No annotations, no typing campaign, no
  rewrites. You write ordinary GDScript; export makes it native. This is the
  strategic prize — `@virtual` and hand-porting to GDExtension both require
  ongoing per-codebase work; this does not.
- It's **additive**: a build step + a runtime shim. The interpreter stays for
  the editor; nothing in the language changes.

## 2. What to transpile *from*: bytecode, not source

Reuse the **entire existing frontend** — `gdscript_parser` + `gdscript_analyzer`
+ `gdscript_compiler` already produce a fully-typed AST and a compiled
`GDScriptFunction` (a stream of **158 opcodes** over a Variant stack). **Do not
re-implement the language.** Transpile from the compiled `GDScriptFunction`,
exactly as IL2CPP transpiles from IL, not from C#.

This is the single most important decision: the opcode set is **finite, closed,
and already semantically defined by the VM** (`gdscript_vm.cpp`). A
transpiler is then "a big, tedious `switch` over 158 opcodes that emits C++,"
not a compiler. Correctness reduces to "does my C++ for `OPCODE_X` match the
VM's handler for `OPCODE_X`" — a per-opcode, testable equivalence.

### Two fidelity levels
- **(F) Faithful** — each opcode → the equivalent C++ operating on `Variant`s.
  Removes *interpreter dispatch* and turns `.gd→.gd` calls into **direct C++
  calls**, but keeps Variant boxing. Complete; handles all dynamic features.
- **(S) Specialized** — for statically-typed locals/args, use native C++ types
  (`int`, `float`, `Vector3`) instead of `Variant`, and resolve typed calls
  directly. Removes boxing on typed paths → near-native. Partial; falls back to
  (F) for anything dynamic.

Ship **(F) first** (the whole game runs, faithfully faster), layer **(S)** on
top for typed hot paths. (S) is where the *big* numbers come from.

## 2.5 Language constraints that shrink the problem (adopted)

The exported dialect drops two dynamic features. These aren't cosmetic — they
change the problem class.

### No `set_script()` at runtime  ⇒ **closed world**
An instance's type is fixed at creation and never changes. Consequences:

- **Automatic devirtualization via Class Hierarchy Analysis.** Export is
  whole-program, so the transpiler sees every script and can *prove* whether a
  method is overridden anywhere. Non-overridden → bind the call directly. This
  delivers the **entire `@virtual` win with no annotations and no typing
  campaign** — the closed world lets us *infer* what `@virtual` had to *declare*.
  (The separate-compilation problem that killed inferred-`final` doesn't exist at
  export.)
- **Routing collapses to R2** (§5): static script refs → every script becomes a
  real native class, scene refs rewritten at export, the GDScript layer removed
  from the shipped build. The "fall back to interpreter if the script was
  swapped" path — the worst correctness hazard — **disappears entirely.**
- Enables **aggressive inlining**: every call target is statically known.
- **Companion constraint required:** also forbid *runtime script generation*
  (`GDScript.new()` + `set_source_code()` + `reload()`), else the world isn't
  closed. Rare in shipped games.

### No `preload()` (value preloads → `load()`)  ⇒ **decoupled, acyclic build graph**
`preload` bakes a compile-time constant resource ref and permits **preload
cycles** (A⇄B), which become **static-init cycles across generated TUs** — nasty.
Converting value-preloads to runtime `load()`:

- **Breaks the compile-time cycle**: each generated TU compiles independently
  (parallel/incremental builds; no static-init ordering puzzle).
- Runtime cost negligible — `ResourceLoader` caches (one hashmap hit after first
  load).
- **Precision:** only **value** preloads become `load()` (a scene/texture/script
  used *as a value*). Preloads used as a **type** — `const B = preload("b.gd")`
  then `extends B` / `var x: B` — **stay compile-time type references** (resolved
  by the analyzer; you cannot `extends` a runtime var). Rule: *value* preload →
  `load()`; *type* reference → static.

### `await` stays interpreted (MVP constraint)
Any function containing `await` (a coroutine) is **left interpreted** — not
transpiled. This deletes the C#-style state-machine lowering entirely. The
boundary is **per-function**: a transpiled function calling an interpreted
`await` function just does a normal call across the ScriptInstance boundary, and
vice-versa. Per-frame hot loops almost never `await`, so this costs ~nothing
where it matters. (Coroutine lowering can be a much later phase if ever needed.)

### Net effect
The closed world **removes the hardest correctness category** (dynamic type
swap) *and* **raises the optimization ceiling** (CHA devirt + inlining, for free,
no annotations). The `preload` change makes the generated build tractable to
compile. What remains hard is unchanged: **coroutines (`await`)**, the
**per-platform export compile**, and **reflection/Callable coverage** — see §6,
§7. `await` is now the single dominant risk.

## 3. Pipeline

```
                 EDITOR (unchanged)                     EXPORT
  .gd ──parser/analyzer/compiler──► GDScriptFunction ──► [Transpiler] ──► *.cpp/*.h
                                     (bytecode)                │
  interpreter runs it (fast iteration)                        ▼
                                              scons/cmake per-platform build
                                                              │
                                                              ▼
                                              libgame_gdscript.{so,dll,dylib}
                                                              │
                                              export packs it + rewrites routing
```

1. **Transpiler** (new module / editor tool): iterate every `GDScript` resource
   and every `GDScriptFunction`, emit one C++ translation unit per script.
   - Members → a generated struct mirroring `GDScriptInstance` layout (or keep
     the GDScript instance and only replace the method bodies).
   - Each function → a C++ function; locals → C++ `Variant` (F) or native (S).
   - Each opcode → its C++ equivalent (see §4).
   - `.gd→.gd` calls → **direct calls** to the generated C++ functions.
   - Native/`callp`/`get`/`set`/Callable → the same runtime calls the VM makes.
2. **Build**: invoke the platform toolchain at export to compile the generated
   C++ into a GDExtension library (this is the part that makes export *slow* and
   toolchain-dependent — §7).
3. **Routing**: the exported game must use the native class where a scene/script
   references `res://foo.gd`. Options in §5.

## 4. Opcode coverage (the bulk of the work, but mechanical)

All 158 opcodes fall into buckets:

- **Trivial** (assign, operator, construct, index, jump, return, type-check):
  1:1 C++ using the same `Variant` APIs the VM uses.
- **Calls** (`OPCODE_CALL*`, ~16 variants): native/builtin/utility → same runtime
  entry points; **`.gd→.gd` → direct C++ call** (the whole point). Callables →
  `Callable` machinery.
- **Control flow**: `for`/`while`/`if` are already lowered to jumps in bytecode;
  emit `goto`/loops. (A source-AST transpiler would be prettier, but bytecode is
  more faithful and avoids re-deriving control flow.)
- **The hard ones**:
  - **`OPCODE_AWAIT` / `OPCODE_AWAIT_RESUME`** — coroutines. The VM suspends by
    saving the whole stack in `CallState` and returns a `GDScriptFunctionState`;
    resume re-enters `call()` with that state. In C++ you cannot just "return
    and resume" — you need a **state-machine transform** (split the function at
    each await into resumable segments, à la C#'s async lowering) or C++20
    coroutines. **This is the single biggest correctness/effort risk.**
    - Pragmatic escape hatch: **leave `await`-containing functions
      interpreted** (hybrid, §6). Most hot per-frame functions don't `await`.

## 5. Routing the exported game to native code (the integration crux)

A node in a `.tscn` has a `script` pointing at a `GDScript` resource; calls go
`Object → ScriptInstance → GDScript::callp → member_functions.find →
GDScriptFunction::call`. Two ways to substitute:

- **(R1) Custom `ScriptInstance` / language shim.** Keep the `GDScript`
  resources, but their `instance_create` produces a *native-backed* instance
  whose `callp` dispatches to the transpiled C++ (a generated jump table per
  script). Least disruption to scenes; the `GDScript` object becomes a thin
  handle over native methods. Still pays one virtual `callp` at the boundary
  from engine/dynamic callers — but internal `.gd→.gd` calls are direct.
- **(R2) Promote transpiled scripts to real GDExtension classes** and **rewrite
  scene/resource script references at export** (`res://foo.gd` → `Foo` native
  class). Cleanest runtime (no GDScript layer at all), but the generated class
  must be **behaviorally identical**: same properties (`@export`), signals,
  `class_name`, inheritance chain, tool flags, RPC config, default values. More
  export-time rewriting, fewer runtime layers.

R1 is the incremental path; R2 is the endgame.

## 6. Dynamic features — must all keep working (or fall back)

The exported game must behave **identically**. Each of these needs a plan:

| Feature | Plan |
|---|---|
| `call()/callv()`, `get()/set()` by name, `has_method` | keep a generated name→function jump table per class; route reflective calls through it |
| Signals / `Callable` / lambdas | Callables bind to generated C++ functions; lambdas → generated functor structs (they already compile to hidden `GDScriptFunction`s) |
| `await` / coroutines | state-machine transform **or** interpret those functions (hybrid) |
| Script inheritance, `super`, inner classes | generated class hierarchy mirrors it; `super` → direct base call |
| `@export`/`@onready`/`@tool`/`@rpc` | emit the same property/rpc registration metadata the interpreter builds |
| `preload`/`load`, cyclic script refs | **resolved by constraint** (§2.5): value preloads → `load()`, breaking compile-time cycles; type-preloads stay static |
| `set_script()` / runtime script gen / cross-language subclass | **forbidden by constraint** (§2.5) → closed world; no runtime fallback path needed |
| `@tool` (editor) scripts | never transpiled — editor always interprets |

**The hybrid principle** (what makes this *shippable incrementally*): a function
that hits an unsupported construct is **left interpreted**. You transpile the
easy 90% (the per-frame hot loops — which are simple, typed, `await`-free),
measure, and expand coverage over time. You never need 100% on day one. This is
the key difference from "reimplement GDScript."

## 7. Cost of the pipeline itself
- **Export becomes a native compile.** Every platform export now needs a working
  C++ toolchain and minutes-to-tens-of-minutes of compile time. Cross-platform
  exports (Windows/Linux/Mac/Deck/consoles) each need their toolchain — this is
  the same burden IL2CPP imposes and a real UX regression for export.
- **Generated code volume** is large (a big game = a lot of C++), so build times
  and binary size grow.
- **Debugging**: stack traces point into generated C++, not `.gd`. Need a
  source-map back to GDScript lines (the bytecode carries line info; propagate
  it).

## 8. Performance — what actually gets faster

Removed:
- **Interpreter dispatch** (opcode decode, computed-goto, ip management) — gone;
  becomes straight-line C++ the optimizer can inline and register-allocate.
- **`.gd→.gd` call overhead** (the 1.6–2.0 ms/frame we measured) — becomes a
  **direct C++ call** (~0.002 µs). This is the headline: the entire call-overhead
  problem from the other doc *evaporates* for internal calls.
- **Member/local access** — native field/stack access.

Kept (in faithful mode F): **Variant boxing** of dynamic values, and the
`callp` boundary for reflective/engine/dynamic calls.

**Amplified by the closed world (§2.5):** because CHA proves call targets at
export, *every* `.gd→.gd` call is devirtualized and small ones are inlined —
**automatically, not just the typed subset**. So the full 1.6–2.0 ms of call
overhead is reclaimed in faithful mode, not a fraction of it.

Estimated:
- **Faithful (F):** ~**2–4×** on GDScript-heavy frame work. The ~5.76 ms of node
  processing → roughly **~1.3–2.2 ms**. (Dispatch + *all* call overhead removed,
  small calls inlined; Variant ops remain.)
- **+ Specialization (S)** on typed hot paths: **5–10×** there → node processing
  plausibly **~1.0–1.5 ms**.
- For reference, IL2CPP over the Mono *interpreter* is ~2–4×; GDScript's
  interpreter is comparable-or-slower, so the multiplier should be in that range
  or better.

This is a **whole-codebase, automatic** win — not limited to a resolvable/typed
subset like `@virtual`, and not limited to hand-picked systems like GDExtension.

## 9. Effort — honest
| Component | Effort |
|---|---|
| Transpiler: trivial + call + control-flow opcodes (faithful) | **L** |
| Instance/member layout + property/signal/rpc metadata gen | L |
| Routing shim (R1) | M–L |
| `await`/coroutine transform (or hybrid fallback) | **XL** (or M for hybrid) |
| Dynamic-feature coverage (reflection, Callables, lambdas, inheritance) | **XL** |
| Export build integration (per-platform toolchains) | L |
| Source-mapped debugging | M |
| Specialization layer (S) | XL (separate phase) |
| Test: run the *entire* game identically on every path | **XL** |
| **Faithful (F) hybrid MVP** | **~4–8 months, a small dedicated team** |
| **Production-grade (all platforms, S, coroutines)** | **~1–2 years / funded** |

This is the scope of a **second GDScript backend** — comparable to Godot's
C#/Mono integration effort, which took a team years.

## 10. Is it *more realistically doable* than `@virtual`?

**Short answer: still no for a *small* team in the near term — but the closed-world
constraints (§2.5) meaningfully narrowed the gap.** They delete the single worst
correctness category (dynamic type swap → interpreter-fallback routing), turn the
transpiler into a *whole-program* compiler (full information = easier, not
harder), and hand you `@virtual`-grade devirtualization + inlining *for free*.
What did **not** get easier — and still gates a production build — is `await`
coroutine lowering, per-platform export compiles, and full reflection/Callable
coverage. So the residual risk is now concentrated in **one** place (`await`)
plus **build/test logistics**, rather than smeared across the whole dynamic
surface.

- **Near-term / small-team feasibility:** `@virtual` wins decisively. It's a
  surgical, incremental change to systems that already exist (~3–5 wk for a
  caller-side win), ships in pieces, and carries no build-pipeline or
  dynamic-feature-coverage risk. The AOT transpiler is a multi-month-to-year,
  essentially-a-second-implementation project whose correctness bar is "the
  entire game runs bit-identically."
- **The one thing that makes AOT *more tractable than it sounds*:** it **reuses
  the whole frontend** and transpiles a **finite, closed opcode set**, and the
  **hybrid fallback** (interpret what you can't transpile) means it's
  *incrementally shippable* — you don't need coroutines or `set_script` on day
  one. A faithful-mode MVP that transpiles the `await`-free typed hot loops and
  interprets everything else is a *bounded* first milestone, and it's exactly
  the code that dominates the frame.
- **Ceiling & effort-location:** AOT moves the effort **into the engine, once**,
  and every game benefits automatically forever. `@virtual`/GDExtension put
  ongoing effort **into each game**. If the goal is "make *all* GDScript fast
  with no game-side work," AOT is the only option that delivers it — at
  engine-team cost.

**My honest recommendation, unchanged in spirit:**
1. **Ship this cycle:** GDExtension on the ~5 hottest per-object systems. Days,
   not months; bounded risk.
2. **Fork medium-term:** `@virtual` Phase 1 (+ inliner) — real, general, ~1 ms.
3. **AOT transpiler:** the right *strategic* bet **only** if this becomes a
   funded, multi-person, multi-year investment (or a community project you
   join). As a solo/small-team effort it's not realistically completable to the
   correctness bar an exported game demands — but a **hybrid faithful MVP** is a
   legitimate research spike if you want to prove the numbers on your own
   hot loops.

## 11. Open questions — resolutions

- **Q1 Faithful vs specialization → RESOLVED: faithful only** for MVP (and near
  term). Specialization is a much-later phase.
- **Q2 Hybrid boundary → RESOLVED: only `await`-containing functions (and
  await-capturing lambdas) are interpreted.** Faithful mode handles reflection,
  dynamic dispatch, duck typing, and all Variant ops natively; reflection routes
  through the generated name→function table. Boundary is per-function, clean.
- **Q3 Routing → RESOLVED for MVP: R1 shim** (per-method dispatch in a custom
  `ScriptInstance`; transpiled↔interpreted call across that boundary;
  transpiled→transpiled calls are direct C++, CHA-resolved). **R2** (native
  classes + scene-ref rewrite, GDScript layer deleted) is the production endgame.
- **Q4 Export UX → RESOLVED: ACCEPTED.** Per-platform native compile at export is
  an acceptable tradeoff for the team. This makes the **production** path real →
  **R2 (native classes + scene-ref rewrite) is the production routing target**;
  R1 is just the MVP scaffold. Path forward: MVP first (lock in the speed
  number), then the production lift (full opcode coverage → R2 → per-platform
  export builds → whole-game equivalence test).
- **Q5 MVP spike → GO.** See §12.

## 12. MVP definition

- **Constraints:** faithful only · no specialization · `await` interpreted ·
  closed-world (no `set_script`, no runtime script gen, value-preloads→`load()`)
  · R1 shim · single platform.
- **Scope bound:** the transpiler implements only the **opcode subset the chosen
  functions use** (~30–50 of 158), not the whole ISA.
- **Targets (PVKK, ~1.5 ms/frame between them):** `RenderedShip._physics_process`
  + `process_post_appearance`; projectile `_process`/`_physics_process`
  (`CannonSkyShot`, `ProjectileFlak`); coordinate converter
  (`path_local_to_global` / `convert_*`); `Data.of`/`ofOr`.
- **Deliverable:** those functions as native C++ with direct inter-calls; the
  rest interpreted; a **measured before/after** on the same combat capture.
- **Success criterion:** ~2–4× on those functions ⇒ validates the whole-project
  projection before committing to the production lift.
- **What the MVP does NOT answer:** production shippability (full opcode
  coverage, all functions, per-platform export builds, whole-game bit-identical
  test) — that's the Q4 lift.
