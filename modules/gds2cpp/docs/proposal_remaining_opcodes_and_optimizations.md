# gds2cpp — Remaining Opcodes & Open Optimizations

**Status at time of writing:** whole-program transpilation of pvkk covers **92.8%** of member
functions (6,590 / 7,104). The remaining 514 interpreted functions are blocked, as first-blocker,
by exactly three opcode families:

| First-blocker opcode | Functions | Family |
|---|---:|---|
| `OPCODE_AWAIT` (55) | 250 | coroutines |
| `OPCODE_CREATE_SELF_LAMBDA` (58) | 124 | lambdas |
| `OPCODE_CREATE_LAMBDA` (57) | 84 | lambdas |
| `OPCODE_CALL_ASYNC` (41) | 56 | coroutines |

This document has two parts:

- **Part 1** — a concrete transpilation proposal for each of the three families (await, async, lambdas).
- **Part 2** — the optimizations still open in the *already-generated* C++, ranked by benefit, each
  grounded in real counts from the current pvkk `wp/` corpus.

The guiding metric throughout is the one established earlier in the project: `faithful_us ÷ spec_us`
(the interpreter cancels within-process, so this is noise-immune). Faithful auto-transpiled code
currently sits ~2× slower than hand-specialized C++, and **LTO does not close that gap** (proven
negative result — the gap is genuine Variant lifecycle work, not dead code).

---

## Part 1 — The three remaining opcode families

### 1a. `CREATE_LAMBDA` / `CREATE_SELF_LAMBDA` — **RECOMMENDED, medium effort**

**What the VM does** (`gdscript_vm.cpp:2692` / `2721`): reads `captures_count` capture operands off
the instruction args, fetches the lambda body `GDScriptFunction*` from `_lambdas_ptr[lambda_index]`,
copies the captures into a `Vector<Variant>`, `memnew`s a `GDScriptLambdaCallable` (or
`GDScriptLambdaSelfCallable` bound to the owner) wrapping `(script, lambda, captures)`, and writes
`*result = Callable(callable)`.

**Why this is tractable — the capture machinery is reusable as-is.** When the resulting `Callable`
is later invoked, `GDScriptLambdaCallable::call` (`gdscript_lambda_callable.cpp:92`) *prepends* the
captures to the incoming args and calls `function->call(...)` with `total_argcount == captures +
args == lambda->_argument_count`. That means **if the lambda body has `_gds2cpp_fn` set, the existing
dispatch in `GDScriptFunction::call()` already routes it to C++** — the `p_argcount ==
_argument_count` guard is satisfied because captures fill the leading arguments. No new invocation
path is required.

So the work is three small, self-contained pieces:

1. **Transpile the lambda bodies.** They live in `GDScriptFunction::lambdas` (`_lambdas_ptr`), *not*
   in `get_member_functions()`, so `transpile_program` never visits them today. Extend the per-class
   walk to also iterate each member function's `lambdas` (recursively — lambdas can nest), emitting
   `fn_lambda_<N>` bodies. A lambda body is an ordinary `GDScriptFunction`; it transpiles with the
   *existing* emitter unchanged.
2. **Bind them.** In generated `bind()`, after installing member functions, install each lambda
   body's `_gds2cpp_fn` onto its `GDScriptFunction*` (reachable via a new
   `GDScriptFunction::get_lambda(idx)` accessor, or by iterating `lambdas`) and store the ptr in a
   `g_lambda[]` table alongside `g_gf[]`.
3. **Emit the two creation opcodes.** Instruction layout mirrors the other `LOAD_INSTRUCTION_ARGS`
   opcodes: `captures_count = code[ip+iac+1]`, `lambda_index = code[ip+iac+2]`, capture operands at
   `code[ip+2+i]`, result at instruction-arg `captures_count`, advance `iac+3`. Emit:
   ```cpp
   {
     Vector<Variant> caps; caps.resize(K);
     caps.write[0] = *<cap0>; ... caps.write[K-1] = *<capK-1>;
     GDScriptLambdaCallable *cb = memnew(GDScriptLambdaCallable(
         Ref<GDScript>(gf->get_script()), G_Class::g_lambda[N], caps));
     *<result> = Callable(cb);
   }
   ```
   `CREATE_SELF_LAMBDA` is identical except it constructs `GDScriptLambdaSelfCallable` from
   `inst->get_owner()` (with the `RefCounted` branch the VM uses at `:2744`).

**Expected coverage.** Unblocks the **208** member functions that create lambdas *and* newly
transpiles the lambda bodies themselves (additional functions not currently in the 7,104 denominator).
Realized gain is < 208 where a lambda-creating function *also* awaits later — but most lambda use in
pvkk is signal wiring / `Array.map` / sort predicates, which don't await.

**Risk.** Low. The bodies use the proven emitter; creation is a mechanical opcode. Verify with the
same A/B pattern (a fixture whose method builds a capturing lambda, then invokes it — compare the
returned value interpreted vs C++), plus the sort-predicate and `Callable.bind` cases.

**Caveat.** `Callable` identity / equality on lambdas: two lambda Callables compare by
`(function, captures)`. The transpiled path constructs the *same* `GDScriptLambdaCallable` type the
VM uses, so identity semantics are preserved — but this is worth an explicit test (e.g. a lambda
stored and later `disconnect`ed by value).

---

### 1b. `CALL_ASYNC` — **do NOT do in isolation (zero standalone value)**

`CALL_ASYNC` shares the VM's `CALL` handler (`gdscript_vm.cpp:1920`); in a release build it is
*identical* to `CALL_RETURN` except for one `DEBUG_ENABLED`-only guard ("async function called
without await"). Transpiling it is therefore trivial — a by-name `callp` returning into the dst.

**But it yields nothing on its own.** `CALL_ASYNC` is the opcode GDScript emits for the *call* part
of `await some_coroutine()`; it is *always* immediately followed by `OPCODE_AWAIT`. The 56 functions
it blocks as first-blocker would simply re-block on the following `AWAIT`. It is only worth emitting
as a **sub-step of full await support** (§1c), never alone.

---

### 1c. `AWAIT` / `AWAIT_RESUME` — **NOT recommended (high effort, low value)**

**What the VM does** (`gdscript_vm.cpp:2582`): on `await <signal>`, it `memnew`s a
`GDScriptFunctionState`, **snapshots the entire VM stack** (`stack[i]` for `i` in
`FIXED_ADDRESSES_MAX.._stack_size`) into `gdfs->state.stack`, saves `ip+2`, line, script, instance,
and defarg, connects the signal to `_signal_callback` (one-shot), and returns the state object.
`AWAIT_RESUME` (`:2678`) is the landing pad: on resume the interpreter re-enters
`function->call(..., &state)`, jumps to the saved `ip`, and reads `p_state->result` back into the dst.

**Why this is fundamentally hard to transpile.** This is a full **coroutine suspend/resume via raw
stack snapshot + instruction-pointer restore**. A transpiled C++ function cannot be suspended and
resumed at an arbitrary interior point: once it returns, its C++ locals and control position are
gone, and you cannot `longjmp` *back into* a returned frame. Faithfully reproducing it requires a
**coroutine transformation** of the body — one of:

- **State-machine split (CPS):** cut each function at every `await` into segments, each its own C++
  function, with an explicit resume-index and an explicit spilled-locals struct that mirrors the VM
  state layout so `GDScriptFunctionState`-based resume can re-enter the right segment. This is a
  second backend, not an opcode handler.
- **C++20 coroutines (`co_await`):** cleaner in theory, but the suspension is driven by Godot's
  signal/`GDScriptFunctionState` machinery, not a C++ awaiter; bridging the two (and matching the
  exact stack-snapshot resume contract other code depends on) is heavy and invasive.

**And the payoff is small.** `await` points are, by nature, **cold**: waiting on a signal, a timer,
a frame boundary, an HTTP request. They are not hot inner loops. Transpiling the *suspension* buys
almost nothing; the interpreter handles the cold wait perfectly well.

**Recommended posture:** leave `await`/`async` functions interpreted. If a specific awaiting function
ever shows up hot in a Tracy capture, the pragmatic option is the **hybrid prefix**: transpile the
straight-line segment *before the first `await`* into C++ and hand back to the interpreter at the
suspension point — capturing any real compute that happens to sit ahead of the wait, without
attempting to transpile the suspension itself. Only build this if profiling justifies it.

---

### Part 1 summary

| Family | Recommendation | Effort | Coverage unblocked | Value |
|---|---|---|---|---|
| Lambdas (`CREATE_LAMBDA`/`SELF_LAMBDA`) | **Do it** | Medium | ~208 fns + lambda bodies | Real |
| `CALL_ASYNC` | Only as part of await | Trivial | 0 standalone | None alone |
| `await`/`AWAIT_RESUME` | **Leave interpreted** | Very high | 250 fns | Low (cold code) |

Doing lambdas takes coverage to roughly **95–96%**. The remaining ~4% is `await`, which is a
deliberate, well-justified stopping point.

---

## Part 2 — Open optimizations in the generated C++

These are ordered by **benefit-to-effort**, grounded in counts from the current pvkk `wp/` corpus
(1,008 classes, 622k lines of generated C++):

| Signal | Count | Meaning |
|---|---:|---|
| `->callp(` | **11,516** | dynamic by-name method calls (the "dynamic tail") |
| `Variant::construct(` | **19,420** | per-op typed-slot inits (TYPE_ADJUST) |
| `gf->get_global_name(` | **29,765** | per-execution StringName re-lookups through `gf` |
| `Variant s[` | **7,581** | boxed Variant stack frames (one per function) |
| `::fn_…(inst` | 3,230 | already-direct devirt / cross-class / super calls |
| `->validated_call(` | 53 | resolved static/native validated calls |
| `VariantInternal::` | 236 | native-builtin fast-path expressions |

A representative slice of a real generated body (`PlayerInteractionHandler`, the picking loop)
illustrates every issue at once:

```cpp
// new_state = process_picking_state(delta)          // a SELF method — went dynamic!
{ const Variant *ca[] = { (&t3) };
  Variant cret; Callable::CallError ce;
  (&s[0])->callp(gf->get_global_name(GN_process_picking_state), ca, 1, cret, ce);
  *(&t13) = cret; }
*(&t4) = *(&t13);                                     // redundant boxed copy
gf->gds2cpp_operator_func(0)((&t5), gf->gds2cpp_constant_ptr(3), (&t9));  // indirected op + const
{ bool valid; Variant::evaluate((Variant::Operator)0, *(&t6), *(&t7), *(&t11), valid); }  // generic op
```

Everything is a `Variant` temporary (`t3…t13`), every name/const/operator is re-fetched through `gf`
on every execution, and a call to a method *of this very class* fell back to a by-name hash lookup.

---

### OPT-1 — Unbox the Variant frame (typed-local backend) — **highest certain benefit, large effort**

**The problem.** Every transpiled function allocates a `Variant s[N]` stack (7,581 of them) and
routes typed locals — ints, floats, bools, Vector2/3, etc. — through boxed `Variant` temporaries,
with per-operation type-tag writes and real refcount inc/dec on the live object/string slots. This
*is* the proven ~2× `faithful ÷ spec` gap. It is **not** reachable by a build flag: the LTO
experiment (whole-engine `/GL /LTCG`) moved `has` from 2.05× to 2.07× and `ofOr` slightly *worse* —
the Variant lifecycle is genuine work, not DCE-able.

**The fix.** Emit **native C++ scalars** for slots whose type is statically known (the transpiler
already recovers many via `temporary_slots` + typed args + `ASSIGN_TYPED_*`), instead of `Variant`
temporaries: `int64_t t3; double t5; bool t8;` with direct arithmetic and comparisons, boxing back to
`Variant` only at genuine boundaries (dynamic calls, returns, container stores). This is the
"frame-elision endgame" the project already scoped incrementally (arg-aliasing and `get_owner`
elision were steps 1–2, each +0.3–0.5× on `has`/`ofOr`). A full typed-local backend is the big one.

**Expected impact.** Closes most of the 2× gap on compute-bound bodies — the hot `_process` /
numeric / loop functions where it matters most. On hand-spec micro-benchmarks the ceiling was
**~4.2× on dispatch-bound accessors** (`has` 4.35×); unboxing is what unlocks that band automatically.

**Effort / risk.** Large — it is a real second lowering mode. Highest risk item, so keep the existing
per-function A/B verification and expand the opcode-test corpus. Mitigation: do it **type-class by
type-class** (ints first, then float/bool, then the small math structs), gating each on 61/61 +
whole-program regen, exactly as the earlier frame-elision steps were gated. Allocation-bound bodies
(string concat / substr) will *not* benefit — dispatch savings are negligible there — so scope it to
numeric/dispatch-bound slots and don't chase string-heavy code.

---

### OPT-2 — Hoist per-call-site indirections resolved at bind time — **broad, low effort, do first**

**The problem.** `gf->get_global_name(GN_x)` appears **29,765** times, and
`gf->gds2cpp_operator_func(i)` / `gf->gds2cpp_constant_ptr(n)` similarly — all re-evaluated *every
time the line executes*, each an indirect load through the `gf` parameter (a `Vector` index or table
load). In a hot loop this is a per-iteration indirection for a value that is **constant for the life
of the bound function**.

**The fix.** Each generated C++ function is bound 1:1 to a single `GDScriptFunction`, so these are
loop-invariant. Two options, increasing in payoff:

- **Cheap:** hoist to function-local `static const` (`static const StringName _m =
  gf->get_global_name(GN_x);`) — one-time lazy init, kills the repeated `Vector` indexing.
- **Better:** resolve them **at `bind()` time** into per-class static arrays populated once
  (`StringName g_name[]`, `MethodBind* g_mb[]`, operator-fn-ptr `g_op[]`, `Variant* g_const[]`) and
  reference those directly — eliminating the `gf->` indirection entirely.

**Expected impact.** Small per-site but it touches essentially every hot line; near-zero risk (pure
mechanical hoist of already-correct expressions). This is the cheapest broad win and a good warm-up
that also lays the `g_mb[]` groundwork OPT-4 needs.

---

### OPT-3 — Eliminate dead typed-slot constructs — **medium benefit, medium effort/risk**

**The problem.** `Variant::construct(...)` for `TYPE_ADJUST` slot-typing appears **19,420** times. A
large fraction are **dead**: the slot is default-constructed to a type and then fully overwritten by
the next validated write before ever being read. This is exactly the deferred "frame-elision step 3."

**The fix.** A per-slot liveness pass: drop the `TYPE_ADJUST` construct when the slot is provably
written (by a union-writer op) before any read on every path out.

**Risk — the sharp edge.** Mis-classifying a slot as dead produces a **silent wrong result** (this is
precisely the `ofOr → -1` bug class). The known union-writers — `OPERATOR_VALIDATED`,
`CALL_BUILTIN_TYPE_VALIDATED`, the validated getters — write into the value union *without* setting
the type tag, so the preceding `TYPE_ADJUST` is what makes them correct. Be conservative: only elide
when the very next op on that slot is a full-value assignment, not a union write. Gate hard on 61/61 +
the whole-program A/B.

**Note:** OPT-1 (unboxing) *subsumes* much of this — a native `int64_t` local has no `TYPE_ADJUST` at
all. If OPT-1 is on the roadmap, treat OPT-3 as a cheaper interim that OPT-1 later absorbs, and don't
invest heavily in both.

---

### OPT-4 — Shrink the dynamic call tail — **large surface, uncertain yield, honest caveat**

**The problem.** **11,516** `->callp(` by-name dynamic calls — ~73% of all call sites, the single
biggest category. Each is a StringName hash lookup + generic argument marshaling.

**The honest framing.** These map to GDScript's *own* plain `CALL`/`CALL_RETURN` opcodes. GDScript
already emits `CALL_METHOD_BIND*` (which we transpile to direct validated calls) *whenever it knows
the receiver type*. A plain by-name `CALL` therefore means **GDScript itself did not know the type** —
untyped `var x = get_thing(); x.foo()`, `Node` receivers fetched dynamically, `Variant`-typed
returns. The transpiler's cross-class + local-dataflow passes already recover the *script-typed*
subset (that work moved cross-class 63 → 72 — modestly). The residual tail is dominated by genuinely
untyped-at-GDScript-level and native-object receivers.

**What could still be won.** A transpiler-side dataflow that infers *native* receiver types beyond
GDScript's intra-statement inference (mirroring the existing `s_slot_classes` machinery for
`ASSIGN_TYPED_NATIVE` and native-typed args), then resolves `ClassDB::get_method(native_class,
method)` to a `MethodBind*` **cached at bind time** (OPT-2's `g_mb[]`) and emits `mb->validated_call`
/ `ptrcall` instead of `callp`. Also: a handful of self-calls that missed devirt (overridden methods,
or self reached via a copied slot) can be recovered.

**Expected impact.** Real but bounded and **uncertain** — diminishing returns, because the majority
of the tail is untyped by construction. Recommend this *after* OPT-1/OPT-2, and **measure yield on a
sample before committing** (instrument how many `callp` sites have a statically-inferable native
receiver type). Do not expect the 73% to collapse.

**Why it compounds with the rest.** The devirt/cross/super paths already emit direct `G_X::fn(inst,
gf, …)` calls that **bypass `GDScriptFunction::call()` entirely** — so within a transpiled call tree,
execution stays in C++-land and pays the interpreter-entry cost only at the boundary (the live-
dispatch measurement was 2.04× real-path vs 3.04× micro-bench precisely because of that fixed entry
cost). Every `callp` converted to a direct call keeps the tree in C++ one level deeper, so OPT-4's
value is partly in *compounding* OPT-1's per-function wins across call chains.

---

### Recommended sequence

1. **OPT-2** (bind-time hoist) — cheapest, broad, near-zero risk; also builds the `g_mb[]`/name-table
   infrastructure OPT-4 will reuse. *Warm-up.*
2. **Lambdas** (§1a) — closes the coverage story to ~95–96% at medium effort/low risk.
3. **OPT-1** (typed-local unboxing) — the headline performance win; the only thing that closes the
   proven ~2× gap. Do it incrementally, type-class by type-class, gated on A/B + 61/61.
4. **OPT-3** (dead-construct elision) — only if OPT-1 is deferred (OPT-1 subsumes it).
5. **OPT-4** (native-receiver inference) — last; measure inferable-receiver yield before committing.
6. **`await`** — leave interpreted. Revisit only via the hybrid-prefix approach if a specific
   awaiting function ever profiles hot.

**One-line takeaway:** the *coverage* gap is closed by lambdas; the *performance* gap is closed by
unboxing the Variant frame (OPT-1) — and neither is reachable by a compiler flag, only by emitting
different code.
