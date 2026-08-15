# Engineering Journal

A running technical record of this project: what was built in each phase, why
it was built that way, and the concepts needed to follow it. It is written to
be read in order.

---

# Phase 0 — Project Foundation

**Completed:** 2026-08-13
**Outcome:** configure, build and test all succeed; the application launches.

## 0. Scope, and an honest statement of what this is not

Phase 0 builds the *container* for a CFD solver, not the solver. There is no
geometry, no mesh, no discretisation, no turbulence model and no flow solution
in this codebase. The viewport shows an empty coordinate system because an
empty coordinate system is genuinely all there is.

Nothing here has been validated against experimental data, reference solutions
or another CFD code, because nothing here computes anything physical yet. The
only claims this journal makes are: the code compiles without warnings, the 39
automated tests pass, and the application opens a window and draws its
interface. Each of those was run and observed, not assumed.

## 1. What was implemented

| Component | Location | Purpose |
|---|---|---|
| Build system | `CMakeLists.txt`, `cmake/` | Modular CMake, pinned dependencies, strict warnings |
| `cfd_core` | `src/core/`, `include/cfd/core/` | Logging, error handling, build metadata. No GUI, no platform code |
| `cfd_app` | `src/app/`, `include/cfd/app/` | Window, GUI shell, viewport, camera |
| `cfd_sim` | `src/main.cpp` | Executable: CLI parsing, startup, self-check |
| `cfd_tests` | `tests/` | 38 GoogleTest cases plus one binary-level smoke test |

The dependency direction is strictly one-way:

```
cfd_sim  ──▶  cfd_app  ──▶  cfd_core  ──▶  C++ standard library
                  │
                  └──▶  Dear ImGui, GLFW, OpenGL
```

`cfd_core` cannot reach upward. That is enforced structurally rather than by
convention: nothing in `cfd_core`'s CMake target links the GUI libraries, so an
accidental `#include <imgui.h>` there fails to compile. This matters because
every numerical component added later will live at or below `cfd_core`'s level,
and the moment a solver depends on a window it can no longer be tested on a
machine without a display. `cmake -DCFD_BUILD_APP=OFF` builds and tests
everything except the GUI, and that configuration was verified to work.

---

## 2. The build system

### 2.1 The modern CMake mental model

Old CMake was a pile of global switches: `include_directories()`,
`add_definitions()`, `CMAKE_CXX_FLAGS`. Everything applied to everything, and
in a project with a dozen components you could no longer say what any one of
them actually compiled with.

Modern CMake is about **targets** and **properties that propagate along
dependency edges**. A target is a library or executable. You attach include
directories, compile options and dependencies to that target, and CMake works
out what the consumers need. Three keywords control propagation:

| Keyword | Applies to the target itself | Applies to consumers |
|---|---|---|
| `PRIVATE` | yes | no |
| `PUBLIC` | yes | yes |
| `INTERFACE` | no | yes |

The rule of thumb: if it appears in your **header**, it is `PUBLIC`; if it only
appears in your **.cpp**, it is `PRIVATE`.

Both appear in `src/core/CMakeLists.txt`:

```cmake
target_link_libraries(cfd_core PRIVATE cfd::warnings)
target_link_libraries(cfd_core PUBLIC Threads::Threads)
```

Our strict warning set is `PRIVATE` — it governs how `cfd_core` is compiled but
is not inflicted on anything that links it. Threads is `PUBLIC` because
`Log.hpp` includes `<mutex>`, so anyone compiling against that header needs the
same threading support.

`cfd_warnings` is an `INTERFACE` library: it contains no code at all. It exists
purely as a bundle of compile options that other targets opt into. That is the
mechanism keeping `-Wconversion` on our code and off Dear ImGui's.

### 2.2 Generator expressions

```cmake
target_compile_definitions(cfd_core PRIVATE CFD_BUILD_TYPE="$<CONFIG>")
```

`$<CONFIG>` is a **generator expression** — it is not evaluated when CMake
runs, but when the build files are generated, per configuration. This exists
because generators like Xcode and Visual Studio build Debug and Release from
one project file; there is no single build type at configure time. Writing
`${CMAKE_BUILD_TYPE}` would silently produce an empty string there.

### 2.3 Pinned dependencies

`cmake/Dependencies.cmake` fetches each library from a release tarball with an
expected hash:

```cmake
URL      "https://github.com/glfw/glfw/archive/refs/tags/3.5.1.tar.gz"
URL_HASH "SHA256=5234f4f29473e9a06bc7847d8371858dd135d38466eeeaa652fdc9f8f9ff0c20"
```

A git tag is a movable label; a hash is not. If the contents ever change, the
configure step fails instead of quietly building different code. For a project
whose output is numbers people may act on, "which exact sources produced this
result" has to be answerable.

Dear ImGui ships no `CMakeLists.txt`, so `cfd_add_imgui()` declares the target
by hand, listing the four core sources and exactly two backends. `imgui_demo.cpp`
is deliberately excluded — it is a large showcase of every widget in the
library and does not belong in a shipping binary.

### 2.4 Warnings

`cmake/CompilerWarnings.cmake` enables the usual `-Wall -Wextra -Wpedantic` plus
a set aimed at numerical code:

- `-Wconversion`, `-Wsign-conversion` — implicit narrowing and signed/unsigned
  mixing. In a solver, a `double` truncated to `float` does not crash; it
  produces slightly wrong numbers, which is far harder to notice.
- `-Wdouble-promotion` — a `float` silently widening to `double` in a mixed
  expression, which usually means the types were not thought through.
- `-Wold-style-cast` — forces `static_cast`, making every conversion visible.
- `-Wshadow` — a local hiding an outer name.

The build is currently clean under all of these, and clean with
`-DCFD_WARNINGS_AS_ERRORS=ON`. That option is off by default so that a new
compiler version introducing a new warning cannot break someone's build, but it
is the setting to use before committing.

### 2.5 Why `RelWithDebInfo` by default

Single-config generators leave `CMAKE_BUILD_TYPE` empty unless told otherwise,
which means **no optimisation and no debug information** — the worst of both.
The top-level `CMakeLists.txt` defaults it to `RelWithDebInfo`: optimised, but
with symbols, so a stack trace is still readable.

This becomes a physics-adjacent concern later. Optimisation can change
floating-point results — by keeping intermediates in wider registers, or by
reordering operations. So a residual history is only meaningful alongside the
build that produced it, which is why `BuildInfo` records the configuration and
`--version` prints it.

---

## 3. Error handling: `Result<T>` and `Status`

### 3.1 The problem

A CFD run has two very different kinds of failure:

1. **Bugs** — an index out of bounds, a broken invariant. The correct response
   is to fail loudly and immediately. Assertions handle these.
2. **Expected failures** — a mesh file that will not parse, a self-intersecting
   geometry, a solve that diverges at iteration 4000. These are ordinary
   outcomes of running the program on real input. The caller almost always
   wants to inspect and report them.

The second kind is what `include/cfd/core/Error.hpp` is for.

### 3.2 Why not exceptions

Exceptions would work, but they have two properties that fit badly here. They
are invisible in the function signature — nothing about `Mesh generate(...)`
tells you it can fail — and they are awkward to propagate through the tight
numeric loops that dominate a solver. Returning the failure as a *value* makes
it part of the type, so it cannot be overlooked:

```cpp
Result<Airfoil> makeNaca(std::string_view designation);   // this can fail, visibly
```

### 3.3 How it works

`Result<T>` holds either a `T` or an `Error`, using `std::variant<T, Error>` —
a **tagged union**: storage for one of several types, plus a tag recording
which is currently held. Unlike a C union it knows what it contains and
destroys it correctly.

Three details do the real work:

- **`[[nodiscard]]`** on the class. Ignoring a returned `Result` is a compile
  warning; you cannot silently drop a failure.
- **`value()` throws if the result is an error.** Not undefined behaviour, not
  a default-constructed value — a `BadResultAccess` exception. A solver that
  reads a garbage mesh after a failed load produces plausible-looking nonsense,
  which is much worse than a crash.
- **Implicit constructors**, so `return mesh;` and `return Error{...};` both
  compile without ceremony.

`Result<void>` is specialised for operations that succeed or fail but produce
no value, and aliased as `Status`, which reads better at call sites.

### 3.4 `std::source_location`

```cpp
Error(ErrorCode code, std::string message,
      std::source_location origin = std::source_location::current());
```

This is a C++20 feature that replaces the old `__FILE__`/`__LINE__` macro
trick. A defaulted argument is evaluated at the **call site**, so
`std::source_location::current()` records where the `Error` was constructed,
not where the constructor is defined. The caller writes nothing extra and the
error still knows where it came from. `ErrorTests.cpp` asserts exactly this by
comparing against `__LINE__`.

---

## 4. Logging

### 4.1 Sinks

A log record has to reach two places: the terminal, and the console panel
inside the application. Rather than special-case that, `Logger` holds a list of
`LogSink` implementations and writes each record to all of them.

- `ConsoleSink` writes to the terminal, with warnings and above going to
  `stderr` so `cfd_sim > run.log` still shows problems. ANSI colour is enabled
  only when `stderr` is a terminal, so redirected output stays clean.
- `RingBufferSink` keeps the most recent N records in memory. This is what the
  GUI console reads, which is why the panel and the terminal can never
  disagree — they are two views of one stream.

`RingBufferSink` is **bounded**: at capacity it discards the oldest record and
counts the eviction. An unbounded log would grow without limit during a long
solver run. The drop count is displayed in the panel, so the buffer never lies
about having lost history.

### 4.2 Why the macros exist

```cpp
#define CFD_LOG(level_, category_, ...)                          \
  do {                                                           \
    const ::cfd::LogLevel cfd_log_level_ = (level_);             \
    if (::cfd::Logger::instance().shouldLog(cfd_log_level_)) {   \
      ::cfd::Logger::instance().log(cfd_log_level_, (category_), \
                                    ::std::format(__VA_ARGS__)); \
    }                                                            \
  } while (false)
```

The level check happens **before** `std::format` runs. With a plain function
call, arguments are evaluated before the call, so a suppressed message would
still pay for formatting its numbers. Once the solver logs a residual every
iteration, that difference is the difference between free and expensive.

`LogTests.cpp` verifies this behaviourally, with a lambda that increments a
counter: after a filtered `CFD_LOG_DEBUG`, the counter is still zero.

The `do { ... } while (false)` wrapper makes the macro a single statement, so
it behaves correctly as the body of an unbraced `if`.

### 4.3 Thread safety, paid for now

Phase 0 is single-threaded. The logger is not, because later the solver will
run on a worker thread while the UI redraws on the main thread, and both will
log. Retrofitting locking onto a logger already used in fifty places is far
worse than paying for it up front.

Two details are worth understanding:

- The level is a `std::atomic<LogLevel>` read with `memory_order_relaxed`. It
  is checked on every single log call, so it must not take a lock; relaxed
  ordering is sufficient because we only need the read to be non-torn, not to
  synchronise anything around it.
- `Logger::log` copies the sink list under the mutex and then writes **outside**
  it. Calling into a sink while holding the logger's own lock would deadlock
  the moment any sink logged something itself, and would let one slow sink
  block every other thread.

`Logger` is a singleton via a function-local `static`, which C++11 onwards
guarantees is initialised exactly once and thread-safely, and which avoids the
static-initialisation-order problem a namespace-scope global would have. The
sink list stays injectable, so tests remain possible: each test swaps in its
own buffer and asserts on what was recorded.

---

## 5. The application shell

### 5.1 Immediate-mode GUI

Dear ImGui is an **immediate-mode** toolkit, which inverts the usual model.

In a *retained-mode* toolkit (Qt, the web DOM) you construct widget objects
once, and afterwards mutate them; the toolkit owns a tree of state that you must
keep synchronised with your own data. In *immediate mode* there are no
persistent widget objects. Every frame, you call functions that both draw a
control and return its current interaction:

```cpp
if (ImGui::Button("Reset View")) { resetView(ui); }
ImGui::Checkbox("Grid", &ui.showGrid);
```

The entire interface is re-declared from scratch, 60 times a second. There is
no synchronisation problem because there is no second copy of the state — which
is why `UiState` is one plain struct rather than a hierarchy of widget classes.
For a tool whose display is derived from simulation data that changes every
iteration, this is a very good fit.

The cost is that the UI is redrawn continuously. That is addressed below.

### 5.2 The layout

This release of Dear ImGui has no docking support (docking lives on a separate
branch), so the tiled layout is built explicitly. Every frame,
`layoutAndDrawPanels()` computes rectangles for the toolbar, session panel,
viewport, console and status bar, and pins a window to each with
`ImGuiWindowFlags_NoMove | NoResize | NoTitleBar | ...`.

The dividers are real: `drawSplitter()` places a thin borderless window on each
seam containing an `InvisibleButton`, and while that button is held it adds the
mouse delta to the tracked panel size. An `InvisibleButton` is the idiomatic way
to claim a rectangle of input without drawing anything — it participates in
ImGui's hover and active-item arbitration, so a drag that starts on the splitter
cannot be stolen by the panel beneath it.

The same technique gives the viewport its pan and zoom: the canvas is one
large `InvisibleButton`, and hover/active state drives the camera.

### 5.3 The visual design

The aim was the look of established engineering software rather than of a
consumer app. Concretely, in `Theme.cpp`:

- **Every rounding radius is zero.** Rounded panels read as consumer software;
  technical tools are built from rectangles, and square corners make aligned
  edges across panels visibly aligned.
- **Hairline borders, no shadows.** Regions are separated by a 1 px line rather
  than by elevation or glow. `ImGuiCol_BorderShadow` is set fully transparent.
- **Compact spacing** (3 px vertical frame padding, 4 px item spacing), because
  the point of the interface is to show many labelled values at once.
- **A narrow, desaturated palette** with exactly one saturated accent, spent
  only on selection and focus.
- **Small type** (15 px base). Oversized text is the fastest way to make a
  technical interface look unserious.
- **Fixed-width digits.** Numeric readouts use a monospace face so columns line
  up and a changing value does not make the layout twitch.

Fonts are resolved at startup from a list of candidate system paths, falling
back to ImGui's built-in bitmap font if none is found. On this machine it
loaded `SFNS.ttf` and `SFNSMono.ttf`.

Everything displayed is a real measured value: compiler and configuration baked
in at build time, `glGetString` output from the driver, live camera state, and
actual log records. Where a capability does not exist, the UI says
"not implemented" rather than showing a plausible number.

### 5.4 The frame loop

```
poll/wait for events
  → ImGui NewFrame
  → declare the whole interface
  → ImGui Render (produces vertex buffers)
  → glClear, RenderDrawData
  → swap buffers
```

**Double buffering.** The GPU draws into a back buffer while the front buffer is
displayed; `glfwSwapBuffers` exchanges them. Drawing directly to the visible
buffer would show partially-rendered frames.

**Vsync.** `glfwSwapInterval(1)` makes the swap wait for the display's refresh,
capping the frame rate and preventing tearing.

**Event-driven idling.** The loop uses `glfwWaitEventsTimeout(0.25)` rather than
a busy `glfwPollEvents()`, so an idle window costs essentially no CPU. The
timeout bounds the wait so time-based updates still happen.

This has a consequence for measurement. The interval between frames is now
mostly a measure of how long the user sat still, so reporting it as "frame
time" would be meaningless. The status bar instead reports the CPU cost of
building and submitting a frame, sampled **before** `glfwSwapBuffers` — because
with vsync enabled that call blocks until the next refresh, and including it
would report the monitor's refresh interval rather than our own cost.

**Framebuffer size vs window size.** On a Retina display these differ by the
backing scale factor: the window is 1600×980 points, the framebuffer 3200×1960
pixels. `glViewport` takes pixels, so it must use `glfwGetFramebufferSize`.
Using the window size instead renders to a quarter of the window — a classic
macOS bug. ImGui's coordinates stay in points and the backend handles the
scaling.

### 5.5 The pimpl idiom

`Application.hpp` mentions neither GLFW nor ImGui. All of it lives behind:

```cpp
struct Impl;
std::unique_ptr<Impl> impl_;
```

**Pointer to implementation**: the header declares an incomplete type, and the
definition exists only in the `.cpp`. Callers do not inherit GUI headers
transitively, and changing the windowing library does not force a rebuild of
everything that includes this header.

Two mechanical consequences: the destructor must be defined in the `.cpp` where
`Impl` is complete, and moves must be declared there too.

### 5.6 Initialisation is explicit, not RAII-on-construction

`Application` has a trivial constructor and a separate `initialize()` returning
`Status`. Constructors cannot return errors, and creating a window genuinely can
fail — no display, no OpenGL 3.2. The alternative would be throwing from a
constructor, which conflicts with the value-based error handling used
everywhere else.

`shutdown()` releases in **strict reverse order** of acquisition: OpenGL
backend, platform backend, ImGui context, window, GLFW. Destroying the ImGui
context before its backends would leave the backends pointing at freed state.
Each step is guarded by a flag so shutdown is idempotent and safe to call after
a partially failed init — which is exactly what happens when
`ImGui_ImplOpenGL3_Init` fails and the code calls `shutdown()` before returning
the error.

---

## 6. The camera transform

This is the only real mathematics in Phase 0, and it is worth doing carefully
because Phase 1 draws the airfoil through it.

### 6.1 Physical meaning

The viewport is a window onto a physical plane measured in metres. A NACA
section is conventionally defined on a **unit chord**, running from x = 0 at the
leading edge to x = 1 at the trailing edge, with y the thickness/camber
direction. We need a rule that turns a point in that physical plane into a pixel
on screen, and that lets the user move and magnify the view without distorting
what is drawn.

Two coordinate systems meet here, and confusing them is the classic source of
"why is my airfoil upside down":

- **World space** — metres. x to the right, **y up**. This is how aerodynamic
  figures are drawn.
- **Screen space** — pixels. x to the right, **y down**, because that is how
  every raster display addresses its framebuffer.

### 6.2 The equations

$$s_x = \frac{W}{2} + (w_x - c_x)\,k$$
$$s_y = \frac{H}{2} - (w_y - c_y)\,k$$

### 6.3 Each term

| Symbol | Meaning | Units |
|---|---|---|
| $w_x, w_y$ | the point being drawn, in world space | m |
| $c_x, c_y$ | camera centre: the world point currently at the middle of the viewport | m |
| $k$ | scale factor | px/m |
| $W, H$ | viewport width and height | px |
| $s_x, s_y$ | resulting pixel, relative to the viewport's top-left corner | px |

Read it as three steps. $(w - c)$ re-expresses the point **relative to whatever
the camera is looking at**. Multiplying by $k$ converts metres to pixels.
Adding $W/2$, $H/2$ places the camera centre at the middle of the viewport.

Two deliberate choices:

**The minus sign on $s_y$** is the world-up to screen-down flip. Without it
every plot is mirrored, and a cambered airfoil appears upside down.

**The same $k$ on both axes** is not an accident of notation. A uniform scale is
what makes the mapping a *similarity transform*: it preserves angles and
shapes. Separate $k_x$ and $k_y$ would stretch the section, so measured angles
and curvature would be wrong, and later a circular vortex core would render as
an ellipse. `Camera2DTests.cpp` asserts this explicitly on a 4:1 viewport.

The inverse, used to turn the mouse position into physical coordinates:

$$w_x = c_x + \frac{s_x - W/2}{k} \qquad w_y = c_y - \frac{s_y - H/2}{k}$$

### 6.4 Zoom about the cursor

Zooming about the viewport centre makes a tool unusable as soon as you are
inspecting something off to one side. The requirement is: **the world point
under the cursor stays under the cursor**.

Let $a$ be the cursor position in screen space and $p$ the world point beneath
it, computed with the current scale. After scaling $k' = f\,k$, we need
$p$ to still map to $a$. Substituting into the inverse transform and solving for
the new centre:

$$c'_x = p_x - \frac{a_x - W/2}{k'} \qquad c'_y = p_y + \frac{a_y - H/2}{k'}$$

So zooming is: read the world point under the cursor, change $k$, then move the
centre so that point lands back where it was.

The factor per wheel notch is **geometric**, $f = 1.15^{n}$, not additive. Equal
notches then produce equal *proportional* changes, which is what feels linear to
a user, and zoom can never reach or cross zero.

### 6.5 Panning

Dragging by $\Delta$ pixels should carry the contents *with* the mouse, so the
camera centre moves the opposite way:

$$c_x \leftarrow c_x - \frac{\Delta_x}{k} \qquad c_y \leftarrow c_y + \frac{\Delta_y}{k}$$

The sign flip on $y$ is the same screen-down/world-up flip as before.

### 6.6 Fitting a box

"Zoom to fit" must show the whole extent, so the more restrictive axis wins:

$$k = \min\left(\frac{W(1-2m)}{\Delta x},\ \frac{H(1-2m)}{\Delta y}\right)$$

with $m$ the margin as a fraction of the viewport. Taking the maximum instead
would crop the geometry — for a long, thin airfoil in a square window, it would
cut off the leading and trailing edges.

### 6.7 Readable grid spacing

A grid line every 0.3718 m is useless. `niceStep()` decomposes the desired
spacing as $r = n \cdot 10^{e}$ with $n \in [1, 10)$, then snaps $n$ to one of
$\{1, 2, 5, 10\}$. The result is always a spacing a human reads instantly, and
it adapts automatically as you zoom. The number of decimals printed on the axis
labels is derived from the step, so the labels carry exactly enough precision to
distinguish adjacent lines and no more.

---

## 7. Testing

39 tests run under CTest: 38 GoogleTest cases plus `app_self_check`, which
executes the real binary with `--self-check`.

The suite targets behaviour that would be expensive to debug later:

- **`Result` / `Error`** — value and error paths, that reading the wrong one
  throws, that move-only payloads move out, that the source location is the
  caller's.
- **Logging** — severity filtering, sink fan-out, that a filtered message does
  not evaluate its arguments, ring-buffer eviction and drop counting, and a
  four-thread concurrent write test that would fail without the mutex.
- **`Camera2D`** — round-trip invertibility, the y-up flip, uniform scale on a
  non-square viewport, that zoom pins the point under the cursor, that pan moves
  content with the mouse, that fit uses the restrictive axis, and that a
  collapsed viewport produces no NaNs.
- **`BuildInfo`** — the generated version header is cross-checked against
  `${PROJECT_VERSION}` injected by a different route, so a stale build directory
  or a mistyped `@VAR@` is caught.

Two deliberate choices. `gtest_discover_tests` registers each `TEST()` with
CTest individually, so failures are named rather than hidden behind one opaque
pass/fail. And `--self-check` is **honest about its limits**: it exercises
logging, filtering, error propagation and build metadata, but it does *not* open
a window, because CTest must run without a display. Graphics initialisation is
genuinely untested by the automated suite; it was verified by hand.

---

## 8. Problems encountered, and how they were solved

**No CMake and no Homebrew on the machine.** The acceptance criteria require
`cmake` to work. Installed CMake and Ninja as user-local pip packages into
`~/Library/Python/3.14/bin` rather than modifying system directories. That
directory is not on `PATH` by default — see §11.

**Dear ImGui 1.92.9b has no docking.** Docking is on a separate branch. Rather
than switch to an unreleased branch, the tiled layout is built explicitly with
pinned windows and hand-written splitters. This turned out to suit the design
goal better anyway: a fixed, predictable layout is more CAD-like than
free-floating dockable panels.

**Splitter windows were clamped to 32 px.** ImGui enforces
`style.WindowMinSize` (default 32×32) on every window, so a 4 px splitter came
out eight times too wide. Fixed by pushing `ImGuiStyleVar_WindowMinSize` to
(1, 1) around the splitter.

**`gl.h` and `gl3.h` both included.** This surfaced *only* under
`-DCFD_WARNINGS_AS_ERRORS=ON`, because it originates in a system header that the
usual first-party-only warning grep does not cover. GLFW includes an OpenGL
header of its own choosing unless told otherwise — on macOS the legacy
`gl.h` — while `Application.cpp` includes the core-profile `gl3.h`. Fixed with
`GLFW_INCLUDE_NONE`, set both in the source and in the target's compile
definitions. Good argument for running the strict configuration regularly.

**`-Wdouble-promotion` in the viewport code.** ImGui works in `float`, the
camera in `double`, and `origin.x + camera.worldToScreen(...).x` silently
promoted. Fixed with explicit `static_cast<double>` so the widening is visible.
Exactly the class of sloppiness that warning exists to catch, and worth keeping
out of numeric code.

**The initial "fit" produced an absurd zoom.** `frameBox()` needs the viewport's
pixel size, which is not known until the first frame has been laid out, so
fitting during construction divided by a placeholder size. Fixed with a
`viewInitialized` flag: the camera is fitted on the first frame that has a real
viewport size, then left alone so user panning and zooming persist.

**`BeginTable`/`EndTable` mispairing.** The first version of the info-table
helper called `BeginTable` internally while callers unconditionally called
`EndTable` — but `EndTable` must not be called when `BeginTable` returns false
(a clipped or collapsed table). Changed the helper to return `bool` and made
every call site honour it.

**A misleading frame-time readout.** With event-driven idling the inter-frame
interval measures user inactivity. Changed to measure CPU cost per frame,
sampled before the vsync-blocking buffer swap. See §5.4.

**Screenshots came back as bare wallpaper.** macOS `screencapture` returns the
desktop without window contents unless the calling process holds Screen
Recording permission, so the interface could not be checked that way. Solved by
adding `--screenshot`, which reads the window's own framebuffer with
`glReadPixels` and writes a BMP. That needs no OS permission, works over SSH,
and gives later phases a way to capture flow-field renders. BMP specifically
because it needs no encoder library and stores rows bottom-to-top — the same
order `glReadPixels` returns them, so no vertical flip is needed. This is how
the layout issues below were found.

**Two visual defects, found from that screenshot.** The scale bar overlapped the
row of x-axis tick labels, and the x axis ran between the two lines of the
empty-state message so it read as a strikethrough. Both repositioned.

**GoogleTest 1.18.0 was three days old.** Pinned 1.17.0 instead — a foundation
should not sit on a release that has had no time to shake out.

---

## 9. Design decisions worth remembering

| Decision | Reason |
|---|---|
| `cfd_core` knows nothing about the GUI | Numerics must be testable headlessly; enforced by the target graph |
| Value-based errors, not exceptions | Failure becomes part of the signature and cannot be overlooked |
| Logging is a singleton, sinks are injectable | Logging is genuinely ambient; testability is preserved via sinks |
| Level check inside the macro | A suppressed per-iteration log must cost one comparison |
| Thread-safe logger in a single-threaded phase | Retrofitting locks across a whole codebase later is much worse |
| Pimpl on `Application` | Keeps GUI headers out of the public include tree |
| Explicit `initialize()` returning `Status` | Window creation can fail; constructors cannot report that |
| Hash-pinned dependencies | Reproducibility of the exact sources behind a result |
| `--self-check` does not open a window | So it can run in CI; and it does not pretend to test graphics |
| No `imgui.ini` | Layout is defined in code; nothing worth persisting, and no stray file |
| `Camera2D` header-only, GUI-free | Lets the transform be unit tested without a window |

---

## 10. What to understand before Phase 1

Phase 1 generates NACA 4-digit geometry. To follow it comfortably:

**From this codebase**

1. **The CMake target model** (§2.1). Phase 1 adds a `cfd_geometry` library;
   you should be able to predict which of its properties are `PUBLIC` and which
   are `PRIVATE`.
2. **`Result<T>` and `Status`** (§3). `makeNaca("2412")` will return
   `Result<Airfoil>`, because "2412x" is a plausible thing for a user to type.
3. **The camera transform** (§6). The airfoil is defined in metres and drawn in
   pixels; every point goes through `worldToScreen`. `resetView()` already
   frames roughly the region a unit-chord section will occupy.
4. **The immediate-mode model** (§5.1). Geometry will be re-drawn from scratch
   every frame from the current parameters; there is no retained scene graph to
   update.

**Background concepts**

5. **Chord, camber and thickness.** The *chord* is the straight line from
   leading to trailing edge, and the conventional length unit for a section.
   The *mean camber line* is the curve midway between the upper and lower
   surfaces; its deviation from the chord is what makes an airfoil asymmetric
   and lets it generate lift at zero incidence. *Thickness* is distributed
   perpendicular to the camber line.
6. **What the four digits mean.** In NACA MPXX: M is the maximum camber as a
   percentage of chord, P is the chordwise position of that maximum in tenths,
   and XX is the maximum thickness as a percentage of chord. So 2412 is 2%
   camber at 40% chord, 12% thick; 0012 has no camber and is symmetric.
7. **Parametric curves and discretisation.** The airfoil is a continuous
   analytic curve that must be sampled into a finite list of points. *How* you
   distribute those points matters enormously — uniform spacing under-resolves
   the leading edge, where curvature is highest. Cosine spacing clusters points
   at both edges. This is the first genuinely numerical decision in the project,
   and it directly affects the accuracy of everything downstream.

**C++ features that will keep appearing**

8. `std::span` for non-owning views over point and field arrays, `std::vector`
   for ownership, and why the distinction matters for a solver's inner loops.

---

## 11. Practical notes

CMake was installed as a user-local pip package. Its directory is not on `PATH`
by default, so either add this to your shell profile:

```sh
export PATH="$HOME/Library/Python/3.14/bin:$PATH"
```

or invoke it by full path. Verify with `cmake --version` (expect 4.4.2).

---

## 12. What I learned

- **Modern CMake is a dependency graph, not a script.** Once you think in terms
  of targets and `PUBLIC`/`PRIVATE`/`INTERFACE` propagation, architectural
  boundaries become things the build system enforces rather than things a
  README asks for politely. "`cfd_core` must not depend on the GUI" is a fact
  about the target graph, not a rule people have to remember.

- **Making failure part of the type changes what you can forget.** A
  `[[nodiscard]] Result<T>` that throws on unchecked access converts a whole
  class of silent-wrong-answer bugs into loud ones. In numerical software, where
  a wrong number looks exactly like a right number, that trade is strongly
  worth making.

- **Cheap abstractions have to be cheap at the call site, not just in
  principle.** The logging macro exists solely so a suppressed message costs one
  comparison instead of a formatted string. That is invisible in an interface
  and decisive in a loop that runs a million times.

- **Strict warnings pay for themselves, but only if you actually run them.**
  The `gl.h`/`gl3.h` conflict existed for several builds and never showed up,
  because it lived in a system header outside the paths being checked. It
  appeared the moment `-Werror` was switched on.

- **Coordinate systems deserve explicit thought, once, in writing.** World is
  y-up in metres, screen is y-down in pixels, the scale must be uniform to
  preserve shape. Writing that down and testing it costs an hour now and
  removes an entire category of "why does this look wrong" later.

- **Build the tool that lets you check your work.** Not being able to see the
  interface was a real obstacle, and the fix — having the application read back
  its own framebuffer — took a few dozen lines, found two genuine layout
  defects immediately, and left behind something the project will want anyway
  once there are flow fields to compare between runs.

- **An empty state should look empty.** The temptation in a Phase 0 shell is to
  fill the viewport with something impressive. Every value on screen is
  measured from the running program, and the pipeline stages are all labelled
  "not implemented", because a UI that implies capabilities it does not have
  will mislead its own author first.

---

## 13. Status at the end of Phase 0

**Verified by running it**

- `cmake -S . -B build`, `cmake --build build`, `ctest --test-dir build` all
  succeed from a clean tree.
- 39/39 tests pass.
- Zero compiler warnings, including with `-DCFD_WARNINGS_AS_ERRORS=ON`.
- `cfd_sim` opens a window, obtains an OpenGL 4.1 core context (Apple M2),
  loads system fonts, renders the interface, responds to pan/zoom, and exits
  cleanly.
- `--version`, `--help`, `--self-check`, `--screenshot` and invalid-argument
  handling all behave as documented.
- `-DCFD_BUILD_APP=OFF` builds and tests the core with no GUI dependencies.

**Not verified**

- Any physics, numerics or CFD result — none is implemented.
- Any platform other than macOS 15.7 on arm64 with Apple Clang 17. The Linux
  and Windows font paths and GL includes are written but untested.
- Graphics initialisation is not covered by the automated suite; it was checked
  by hand.
- No performance work has been done, and none is warranted yet.

**Next:** Phase 1 — NACA 4-digit geometry generation, on explicit instruction.

---

# Phase 1 — NACA Airfoil Geometry

**Completed:** 2026-08-13
**Outcome:** 97/97 tests pass; typing `NACA 2412` draws the correct section.

## 1. Scope

This phase turns a four-digit designation into a discrete airfoil outline and
draws it. It adds a new module, `cfd_geometry`, sitting directly on `cfd_core`
and knowing nothing about windows or OpenGL.

Still no CFD: no mesh, no discretisation of the flow equations, no solution.
The geometry is validated against the NACA equations and against calculus, but
not against wind-tunnel data — §9 says exactly what was and was not checked.

## 2. What was implemented

| Component | Location | Purpose |
|---|---|---|
| Designation parsing | `Naca4.hpp/.cpp` | `"NACA 2412"` → validated `Naca4Digit` |
| NACA equations | `Naca4.hpp/.cpp` | Thickness, camber line, camber slope, cosine spacing |
| `Airfoil` | `Airfoil.hpp/.cpp` | Surfaces, closed contour, measured properties |
| Geometry panel | `Panels.cpp` | Designation input, discretisation controls, readout |
| Viewport rendering | `Panels.cpp` | Fill, outline, camber and chord lines, edge markers |
| Tests | `Naca4Tests.cpp`, `AirfoilTests.cpp` | 58 new cases |

`Vec2` moved from `cfd::app` into `cfd::core`, so geometry and the viewport can
exchange coordinates without geometry depending on anything graphical.

---

## 3. The mathematics

### 3.1 The idea: camber and thickness are separate

**Physical meaning.** An airfoil does two jobs that are largely independent.
Its *curvature* — how much the section arches — mostly determines how much lift
it makes at a given angle, and in particular whether it still lifts at zero
incidence. Its *bulk* — how fat it is — mostly determines drag, structural
depth and how it behaves near stall.

The NACA four-digit family is built on exactly that separation. A section is a
curved line with a symmetric thickness envelope wrapped around it:

- the **mean camber line** `y_c(x)`, running midway between the two surfaces;
- the **thickness distribution** `y_t(x)`, the half-thickness laid on
  symmetrically either side of that line.

This is why the digits split the way they do, and why a symmetric section
(`0012`) is just the cambered construction with the camber set to zero.

### 3.2 Thickness distribution

**Physical meaning.** How fat the section is at each point along the chord.
Zero at the nose, rising quickly to a maximum around 30% chord, then tapering
to the tail.

**The equation** (unit chord, `x` from 0 at the leading edge to 1 at the
trailing edge):

$$y_t(x) = 5t\left(0.2969\sqrt{x} - 0.1260\,x - 0.3516\,x^2 + 0.2843\,x^3 - 0.1015\,x^4\right)$$

**Each term**

| Symbol | Meaning |
|---|---|
| $y_t$ | half-thickness: distance from the camber line to one surface |
| $t$ | maximum thickness as a fraction of chord (the last two digits ÷ 100) |
| $x$ | chordwise station, 0 at the leading edge, 1 at the trailing edge |
| $5t$ | scale factor making the maximum come out at very nearly $t$ |

The **$\sqrt{x}$ term is the important one**. Its derivative is infinite at
$x = 0$, so the surface meets the leading edge *vertically* rather than in a
sharp point — that is what gives an airfoil its rounded nose. Physically that
roundness is what lets the section tolerate a range of angles of attack without
the flow separating instantly at the nose. The four polynomial terms after it
are a curve fit, chosen to put the maximum near 30% chord and to bring the
section back down smoothly at the tail.

Two details worth knowing:

**The maximum is not exactly $t$.** Evaluating the bracket at its peak gives
0.100028, so $y_t^{max} = 0.50014\,t$ and the full thickness is $1.0003\,t$.
A 12% section is really 12.004% thick. That is a property of the published fit,
not an error, and the tests assert the 1.0003 factor explicitly so nobody
later "corrects" it.

**The trailing edge does not close.** The five coefficients sum to 0.0021
rather than zero, so $y_t(1) = 0.0105\,t$ and the section ends in a small blunt
base — 0.25% of chord for a 12% section. This is what the standard equations
produce and what reference ordinate tables list. Changing the last coefficient
from $-0.1015$ to $-0.1036$ makes them sum to exactly zero and closes the
trailing edge to a point. Both are offered, because a blunt base is awkward to
mesh but the open form is the published one.

### 3.3 Camber line

**Physical meaning.** The skeleton curve the thickness is wrapped around. Its
height above the chord at each station is what makes the section asymmetric.

**The equations.** Two parabolic arcs joined at the point of maximum camber:

$$y_c(x) = \frac{m}{p^2}\left(2px - x^2\right), \qquad 0 \le x \le p$$

$$y_c(x) = \frac{m}{(1-p)^2}\left((1 - 2p) + 2px - x^2\right), \qquad p \le x \le 1$$

**Each term**

| Symbol | Meaning |
|---|---|
| $y_c$ | camber line height above the chord, fraction of chord |
| $m$ | maximum camber, fraction of chord (first digit ÷ 100) |
| $p$ | chordwise position of that maximum (second digit ÷ 10) |
| $x$ | chordwise station |

Both branches are parabolas in $x$; the denominators $p^2$ and $(1-p)^2$ are
what scale each arc so that it reaches height exactly $m$ at $x = p$. Both give
$y_c = 0$ at the chord ends and $y_c = m$ at $x = p$ — that identity *is* the
definition of $m$ and $p$, and the tests assert it to machine precision rather
than to a tolerance.

Why two arcs rather than one curve? A single parabola would force the camber
maximum to sit at mid-chord. Splitting at $p$ lets the designer move it, which
is the whole point of the second digit.

### 3.4 Camber slope

**Physical meaning.** The local inclination of the skeleton curve. Needed
because the thickness must be measured perpendicular to it.

$$\frac{dy_c}{dx} = \frac{2m}{p^2}(p - x), \qquad \frac{dy_c}{dx} = \frac{2m}{(1-p)^2}(p - x)$$

for the forward and aft branches respectively. Both carry the factor $(p - x)$,
so both vanish at $x = p$ — as they must at a maximum. The tests compare these
against a central-difference derivative of `camberLine`, which catches an
algebra slip in either formula.

Note the slope is *continuous* at the join but its derivative is not: the two
arcs have different curvature there. That kink is inherent to the four-digit
family and is one reason the later 5-digit and 6-series sections exist.

### 3.5 Building the surfaces

**Physical meaning.** Wrap the thickness around the skeleton. The thickness is
a measurement *across* the section, so it must be laid off perpendicular to the
camber line, not vertically.

With $\theta = \arctan(dy_c/dx)$ the local camber inclination:

$$x_u = x - y_t\sin\theta, \qquad y_u = y_c + y_t\cos\theta$$
$$x_l = x + y_t\sin\theta, \qquad y_l = y_c - y_t\cos\theta$$

**Each term**

| Symbol | Meaning |
|---|---|
| $\theta$ | angle of the camber line to the chord at station $x$ |
| $y_t\cos\theta$ | the part of the offset that lands across the chord |
| $y_t\sin\theta$ | the part that lands *along* the chord |
| $(x_u, y_u)$, $(x_l, y_l)$ | the resulting upper and lower surface points |

The $\sin\theta$ terms are the ones people drop. Offsetting vertically instead —
$y_u = y_c + y_t$ — is a common shortcut, and it *thins* the section wherever
the camber line is steep, which is precisely the nose region that matters most.
It is invisible on a symmetric section, where $\theta = 0$ and the two agree
exactly, so the mistake survives casual testing. `AirfoilTests` checks
perpendicularity directly, on cambered sections only, by taking the dot product
of the upper-minus-lower vector with the camber tangent.

A consequence worth stating because it looks like a bug: because the two
surfaces are displaced in opposite directions *along* the chord, **the upper
and lower surfaces of a cambered section do not share x stations.** Near the
nose $y_t \sim \sqrt{x}$ grows faster than $x$ itself, so the upper surface
reaches slightly *ahead* of the leading edge point — a fraction of a percent of
chord. Both behaviours have their own tests, so the next reader meets them as
documented facts rather than as anomalies.

### 3.6 Cosine spacing

**Physical meaning.** Where to put the points. Curvature is wildly uneven along
an airfoil: enormous at the nose, almost nil over the middle. Points should go
where the shape is changing.

$$x_i = \frac{1 - \cos\beta_i}{2}, \qquad \beta_i = \frac{i\pi}{n-1}, \qquad i = 0 \ldots n-1$$

Sampling uniformly in the angle $\beta$ rather than in $x$ maps equal angular
steps onto stations bunched at both ends. Near the nose, $x \approx \beta^2/4$,
so $\sqrt{x} \approx \beta/2$ — which means **cosine spacing linearises exactly
the $\sqrt{x}$ behaviour that makes the nose hard to resolve.** That is not a
coincidence; it is why every airfoil code uses it. The area refinement study in
the tests converges cleanly at second order as a result, which it would not do
with uniform spacing.

This is the first genuinely *numerical* decision in the project: the equations
are exact, but the moment they are sampled the accuracy of everything
downstream — panel methods, mesh quality, pressure integration — is set by how
those points are placed.

### 3.7 Shoelace area, used for validation

**Physical meaning.** The area enclosed by a closed polygon, obtained from its
vertices alone.

$$A = \frac{1}{2}\sum_i \left(x_i\,y_{i+1} - x_{i+1}\,y_i\right)$$

Each term is twice the signed area of the triangle formed by the origin and one
edge; contributions outside the polygon cancel. The **sign** encodes traversal
direction — positive for counter-clockwise — which is how the tests confirm the
contour ordering without inspecting individual points.

Comparing this against the analytic integral of the thickness polynomial,

$$A = 2\int_0^1 y_t\,dx = 10t\left(\tfrac{2}{3}a_0 + \tfrac{1}{2}a_1 + \tfrac{1}{3}a_2 + \tfrac{1}{4}a_3 + \tfrac{1}{5}a_4\right)$$

is the strongest single check in the suite: it validates the coefficients, the
surface construction, the point distribution and the contour ordering all at
once, against calculus rather than against itself.

---

## 4. Design decisions

| Decision | Reason |
|---|---|
| `cfd_geometry` is a separate module on `cfd_core` | The mesher and solver will consume geometry from a headless batch run; it must not need a window |
| `Vec2` promoted to `cfd_core` | Geometry and viewport must share a coordinate type without geometry depending on the GUI |
| Properties *measured* from generated points | Makes them a check on the geometry rather than a restatement of the input |
| Open trailing edge is the default | It is what the standard equations produce and what reference tables list; closed is offered for meshing |
| Contour stores the closing point (`front() == back()`) | Callers can draw or integrate the loop without special-casing the wrap-around |
| Conventional ordering: TE → upper → LE → lower → TE | The standard airfoil coordinate convention; panel methods expect it |
| Analytic functions exposed in the header | Lets the tests attack the equations directly, not only through generated geometry |
| Reject `2012` and `0412` | `2012` divides by zero; `0412` claims a camber position with no camber. Both are user typos worth naming precisely |
| Keep the last valid shape while typing | Regeneration runs per keystroke; blanking the viewport for `2`, `24`, `241` would be unusable |
| `Result<Airfoil>` throughout | Bad input is expected, not exceptional — exactly what Phase 0 built the type for |

---

## 5. Problems encountered, and how they were solved

**A wrong test, not wrong code.** Two tests failed asserting that both trailing
edge points sit at `x = c`. They do not, and should not: on a cambered section
with an open trailing edge, $y_t(1) \neq 0$ and the camber line is inclined, so
the perpendicular offset displaces the two points along the chord by
$\pm y_t(1)\sin\theta$ — for NACA 2412 that is $8.4\times10^{-5}c$, which
matched the observed failure exactly. Only their *midpoint* is at `x = c`, and
that is exact because the offsets are equal and opposite. The tests were
rewritten to assert the midpoint identity, plus a new test pinning the straddle
to its predicted magnitude and confirming it vanishes for a symmetric section.
Worth recording because the instinct on a red test is to change the code.

**`AddPolyline` argument order changed.** Dear ImGui 1.92.8 swapped `thickness`
and `flags`. Because Phase 0 compiled ImGui with
`IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, the old overload is `= delete` and this was
a hard compile error naming the exact call — rather than a silent
reinterpretation of `ImDrawFlags_Closed` as a line thickness. A good advert for
turning compatibility shims off.

**Concave, not convex.** `AddConvexPolyFilled` is the usual ImGui fill, but an
airfoil is not convex. Using it bridges the shape across the camber. Switched
to `AddConcavePolyFilled`.

**Duplicate library on the link line.** `cfd_tests` named both `cfd::core` and
`cfd::geometry`, and geometry already carries core as a `PUBLIC` dependency, so
the linker warned about a duplicate. Removed the redundant entries — a reminder
that in modern CMake you link what you *directly* use and let propagation do
the rest.

**No way to see other sections.** The designation is typed into the GUI, so
checking that `0012` and `4412` render correctly meant either editing the
default or adding a way in. Added `--section`, which seeds the panel's input
field — so it goes through exactly the same parsing and error path as typed
input, and later batch runs get section selection for free.

**Live validation feedback.** Regenerating on every keystroke means the input is
invalid most of the time while typing. Solved by separating "the section shown"
from "the current input": a failed parse updates the error message but leaves
the previous geometry on screen.

---

## 6. Technical concepts worth carrying forward

- **Analytic vs discrete.** The NACA equations are exact; the point list is an
  approximation of them. Every property the program reports is measured from
  the discrete points, which is why `maxThicknessPosition` reads 0.297 rather
  than 0.300 — the stations simply do not land on the true maximum. Knowing
  which numbers are exact and which are sampled is a habit worth having before
  the solver starts reporting residuals.

- **Convergence as a test.** Asserting one computed area against one expected
  value only checks a single discretisation. Asserting that the error *falls by
  roughly four when the spacing halves* checks that the method is
  second-order — a much stronger statement, and the standard way numerical code
  is verified.

- **Exact identities are better tests than tolerances.** `y_c(p) = m` and
  "midpoint of the surfaces lies on the camber line" hold to machine precision
  because of how the geometry is built. Testing them at `1e-15` catches errors
  that a loose tolerance would wave through.

- **Symmetric cases hide bugs.** The perpendicular-offset error is invisible on
  `0012`. Test suites need a case where each term actually does something.

---

## 7. What to understand before Phase 2

Phase 2 generates a mesh around the section.

1. **The contour is the boundary condition.** The closed, correctly-ordered
   loop from `contour()` is the wall the mesh must wrap. Its ordering and
   orientation are what let a mesher tell inside from outside.
2. **Point distribution propagates.** Cosine spacing put points where curvature
   is; the mesh inherits that. A mesh cell can be no better than the surface
   discretisation it starts from.
3. **The blunt trailing edge is a decision you now have to live with.** An open
   trailing edge means the mesh must either resolve a small base region or the
   geometry must be regenerated closed. This is why both forms exist in the API.
4. **Boundary layers are thin.** The physically interesting region near the
   wall is orders of magnitude thinner than the chord, so mesh spacing normal
   to the surface will have to be graded very aggressively — the same
   "put points where the action is" idea as cosine spacing, one dimension out.
5. **Concepts to read up on:** structured vs unstructured meshes, O-grids and
   C-grids around airfoils, cell aspect ratio and skewness as quality measures,
   and what the far-field boundary distance does to a solution.

---

## 8. What I learned

- **Deriving the geometry is easy; knowing which properties are exact is the
  real work.** The equations took an afternoon. Working out that `y_c(p) = m`
  holds exactly, that the maximum thickness is `1.0003t` rather than `t`, and
  that the trailing edge points straddle the chord station on a cambered
  section — those are what made the tests meaningful instead of decorative.

- **A failing test is a hypothesis, not a verdict.** Two tests failed and the
  code was right both times. Working out *why* the trailing edge points were
  displaced by exactly `8.4e-5` turned a red test into a documented property of
  the geometry.

- **The shortcut that only breaks in the interesting case.** Offsetting
  thickness vertically instead of perpendicular gives identical results for
  every symmetric section — so it passes any test suite that only checks
  `0012`. Correctness testing has to include the case where the term you might
  have dropped is non-zero.

- **Checking numerics against calculus beats checking them against yourself.**
  Comparing the shoelace area of the discretised contour with the analytic
  integral validates four separate things at once, and none of it is circular.

- **Turning off compatibility shims turns silent bugs into compile errors.**
  The `AddPolyline` signature swap would have been a wrong-looking outline with
  no diagnostic. Instead it was a one-line fix pointed at exactly.

---

## 9. Status at the end of Phase 1

**Verified by running it**

- Clean configure, build and test from scratch; zero warnings including with
  `-DCFD_WARNINGS_AS_ERRORS=ON`.
- 97/97 tests pass (96 in the headless `-DCFD_BUILD_APP=OFF` configuration,
  which excludes only the application self-check).
- NACA 0012, 2412, 4412 and 0006 were generated and inspected visually: the
  symmetric sections are mirror-symmetric, the cambered ones show the expected
  camber line, all have rounded noses and maximum thickness near 30% chord.
- Measured properties match the designations: 2412 reports 0.1200c thickness at
  0.297c and 0.0200c camber at 0.396c.
- Invalid input (`NACA 24`) is rejected with a specific message and no geometry
  is drawn.

**Checked numerically**

- Thickness polynomial against the tabulated NACA 0012 ordinate 0.06002c at 30%
  chord.
- Camber line reaching exactly `m` at exactly `p`, to machine precision.
- Camber slope against a central-difference derivative of the camber line.
- Surface midpoints on the camber line, and thickness perpendicular to it, to
  `1e-15`.
- Discretised contour area against the analytic integral, with second-order
  convergence confirmed over three refinement levels.

**Not verified**

- No comparison against wind-tunnel data, another CFD code, or full published
  ordinate tables beyond the single station above.
- No aerodynamic quantity of any kind — no lift, drag, pressure or velocity
  exists yet.
- Still only built and run on macOS 15.7 / arm64 / Apple Clang 17.

**Next:** Phase 2 — mesh generation, on explicit instruction.

---

# Phase 2 — Computational Domain and Mesh

**Completed:** 2026-08-15
**Outcome:** 134/134 tests pass; `NACA 2412` produces a valid C-grid with no
inverted cells at any resolution.

## 1. Scope

This phase builds the computational domain: the region of fluid around the
section, chopped into cells with all the geometric quantities a finite-volume
solver needs. It adds `cfd_mesh`, sitting on `cfd_geometry`.

Still no CFD. Nothing here discretises a flow equation or computes a velocity.
What is verified is that the *domain* is sound — §9 says exactly what was and
was not checked.

## 2. What was implemented

| Component | Location | Purpose |
|---|---|---|
| `Mesh` | `Mesh.hpp/.cpp` | Nodes, cells, faces, metrics, boundary tagging, quality |
| `buildStructured` | `Mesh.cpp` | Turns a block of nodes into a face-based mesh |
| C-grid generator | `CGrid.hpp/.cpp` | Domain shape, clustering, marching, boundary tagging |
| Resolution presets | `CGrid.cpp` | Coarse / Medium / Fine |
| Mesh panel + renderer | `Panels.cpp` | Controls, statistics, culled and decimated drawing |
| Tests | `MeshTests.cpp`, `CGridTests.cpp` | 37 new cases |

---

## 3. Why face-based, and why that is the interesting choice

The grid is structured, so every quantity could be recomputed on demand from
`(i, j)`. It is stored explicitly as a list of faces anyway, because that is
the shape of the thing a finite-volume method actually consumes.

**Physical meaning.** A finite-volume method does not approximate derivatives
at points. It enforces conservation over each cell: whatever enters through the
boundary must accumulate inside. The divergence theorem turns the volume
integral of a flux divergence into a sum over the cell's boundary,

$$\frac{d}{dt}\int_V u \, dV + \sum_{\text{faces}} (\mathbf{F} \cdot \mathbf{n})\, A = 0$$

**Each term**

| Symbol | Meaning | Units (2D) |
|---|---|---|
| $u$ | conserved quantity per unit volume (mass, momentum, energy) | varies |
| $V$ | cell volume — an *area* in two dimensions | m² |
| $\mathbf{F}$ | flux of $u$ | varies |
| $\mathbf{n}$ | outward unit normal of a face | – |
| $A$ | face area — a *length* in two dimensions | m |

The solver never asks "who are my neighbours in $i$ and $j$". It asks, for each
face: what is the flux, which cell owns it, and what is on the other side. So
the mesh stores exactly that. Writing the solver against faces also means an
unstructured mesh can be substituted later without touching it.

Two conventions are fixed once, which removes a sign question from every flux
that will ever be assembled:

- The normal always points **out of the owner** — towards the neighbour for an
  interior face, out of the domain for a boundary face.
- Cell corners are stored anticlockwise, so the shoelace area is positive
  whenever the $(i, j)$ parametrisation is right-handed.

In 2D the 3D vocabulary collapses by one order: a "volume" is an area, a face
"area" is a length. The names are kept because the literature uses them.

### Centroids are not corner averages

The cell value in a finite-volume method lives at the **centroid**, the
area-weighted first moment:

$$\mathbf{C} = \frac{1}{6A}\sum_i (\mathbf{P}_i + \mathbf{P}_{i+1})\,(x_i y_{i+1} - x_{i+1} y_i)$$

For a square this equals the average of the corners; for a skewed cell it does
not, and using the corner average would put a first-order error into every
skewed cell in the mesh. Boundary-layer cells are extremely skewed, so this is
not a nicety.

---

## 4. Why a C-grid

Two structured topologies are standard for an aerofoil:

- **O-grid** — lines close around the body like tree rings.
- **C-grid** — lines wrap the nose like the letter C, then run downstream on
  both sides of a *cut* along the wake.

The C-grid was chosen for two reasons, both of which come straight from the
requirements:

1. The domain is deliberately asymmetric — 12 chords upstream, 25 downstream.
   An O-grid has one outer boundary and cannot express that.
2. The wake is where separation and stall live. A C-grid puts a line of
   well-aligned, refined cells straight down it; an O-grid's cells behind the
   trailing edge fan outwards and smear exactly what matters most.

The price is the **wake cut**: a slit from the trailing edge to the outflow,
opened so the grid can be a topological rectangle. The two sides lie on top of
each other in space but are separate faces. They are not really a boundary —
fluid crosses freely — so each stores its `partner`, and a solver will join
them rather than apply a condition. The tests check the pairing is symmetric,
coincident, oppositely oriented and bounds different cells.

This is also why the mesher requires a **closed** trailing edge: the cut has to
start from a single point, and a blunt base leaves a gap the topology cannot
represent. Phase 1's `TrailingEdge::Closed` existed for exactly this, and the
application's default moved to it this phase.

### Layout

```
j = 0      outflow -> wake cut -> TRAILING EDGE -> lower surface ->
           LEADING EDGE -> upper surface -> TRAILING EDGE -> wake cut -> outflow
j = max    two straight lines at y = +/- vertical extent, joined round the
           front by a half ellipse
i = 0,max  the downstream outflow plane
```

The trailing edge appears **twice** in the $i$ sequence — once where the lower
wake meets it and once where the upper wake leaves it. Those two nodes are the
same point in space. That is not a bug; it is where the slit closes.

---

## 5. Clustering: where the cells go

### Normal to the wall

Layers grow geometrically: each is a fixed multiple of the one before.

$$\text{first} \cdot \frac{r^n - 1}{r - 1} = \text{total}$$

Fixing the first layer height and the total distance determines the ratio $r$
implicitly, and there is no closed form, so it is solved by bisection — the sum
is monotone in $r$, so bisection cannot fail. The near-wall spacing has to span
four orders of magnitude to reach the far field, and a geometric progression is
the standard way to bridge that without the solver ever seeing an abrupt jump
in cell size.

Physically this is the whole game for a viscous calculation: the boundary layer
is resolved only if several cells fit inside it, which is why `firstLayerHeight`
is the single most consequential number in the options.

### Along the wall

Inherited from Phase 1's cosine spacing, which clusters at the leading and
trailing edges. The wake then starts with the *same* spacing the surface ends
with, so cells do not jump in size where the wall becomes a cut.

---

## 6. The hard part: marching outwards without folding

Each grid line has to leave the wall along its normal (for orthogonality where
the boundary layer is) and arrive exactly at its far-field node. How it turns
between those two decides whether the mesh is usable, and I got it wrong twice.

### The failure

Cells with small negative areas — folded over. First on the forward lower
surface, later at the trailing edge. A negative area means the cell is
inside-out, and no finite-volume solver can do anything sensible with one.

### Diagnosis 1: the line doubles back on itself

The first instinct was that adjacent rays were crossing, driven by concave
surface curvature. The numbers said otherwise: the concave radius on the lower
surface there is about 0.7 c, but the fold was at 0.037 c — twenty times
closer.

Printing the layer-to-layer step directions showed the real mechanism. At one
station the wall normal was −94°, the direction to the far field −160°, but the
steps between successive layers read −179° then +169°. The *ray itself* was
doubling back.

For $\mathbf{P}(r) = \mathbf{P}_0 + r\,\mathbf{d}(r)$,

$$\frac{d\mathbf{P}}{dr} = \mathbf{d} + r\,\frac{d\mathbf{d}}{dr}$$

so the line reverses once $r\,|d\theta/dr| > 1$. Two things were making that
happen:

- **Interpolating the two direction vectors.** A normalised lerp between unit
  vectors separated by $\Delta$ sweeps at up to $2\tan(\Delta/2)$ per unit of
  weight, which runs away as $\Delta$ approaches 180°.
- **Blending over linear $r$.** Because $r \cdot \frac{d}{dr}f(r/L)$ depends
  only on $r/L$, the product is **scale invariant** — widening the blend zone
  does not help at all. That was the part I had wrong: my instinct was to give
  the turn more room, and more room changes nothing.

At the failing station, $\Delta = 65.8°$ gave $0.889 \times 2\tan(32.9°) = 1.15$
— just over 1, which is why only a narrow band folded, with tiny negative areas.

**The fix**, two changes that make the criterion satisfiable:

1. **Rotate at a constant angular rate** rather than interpolating vectors.
   Slerp in 2D is just a rotation, so the sweep rate is exactly $\Delta$.
2. **Blend over $\log r$.** Then $r\,dw/dr = f'/\ln(r_1/r_0)$, which can be made
   as small as we like by widening the *ratio* of the two radii, not their
   difference.

Together the condition becomes $\ln(r_1/r_0) > 1.5\,|\Delta|$, and the code
picks the span as $2.5\,|\Delta|$ for margin. Inside $r_0$ — a few first-layer
heights, where the boundary layer will sit — the grid is exactly orthogonal.

That took the failures from 5 to 3.

### Diagnosis 2: averaging quietly undid the constraint

The remaining folds sat at the trailing edge and got *worse* with refinement:
2 cells at Medium, 41 at Fine. The wake cut leaves the trailing edge along the
chord while the surfaces arrive at a few degrees to it — a concave corner, where
marching normals converge to a focus a distance $1/\kappa$ away. Cosine spacing
concentrates points there, so refining shrinks the discrete spacing, raises
$\kappa$, and pulls the focus closer to the wall.

There *was* a curvature limiter capping the blend length at $0.3/\kappa$. The
bug was in how I smoothed it: a min-filter followed by eight averaging passes.
**Averaging can lift a station's value back above the limit its own curvature
demands.** The limiter was computing the right answer and the smoothing was
throwing it away.

Replaced with a Lipschitz sweep — one pass forward, one back, allowing the zone
to thicken only at a bounded rate with distance from a constrained station:

```
blend[i] = min(blend[i], blend[i±1] + rate * spacing)
```

That is smooth *and* never exceeds the limit anywhere, which averaging cannot
promise. Zero inverted cells at every resolution after that.

---

## 7. Validation strategy

The strongest checks are the ones that compare against something independent.

**Finite-volume identities.** For any closed cell the outward area vectors must
sum to zero:

$$\sum_{\text{faces}} \mathbf{n}\,A = 0$$

This is what makes a constant field have zero discrete divergence, so a solver
on a mesh that fails it cannot even preserve a uniform flow. Separately, cell
areas are recovered from their own faces by the divergence theorem applied to
$\mathbf{F} = (x, y)$, whose divergence is 2:

$$A = \tfrac{1}{2}\sum_{\text{faces}} (\mathbf{c} \cdot \mathbf{n})\,A_{\text{face}}$$

and compared with the shoelace areas — cross-checking centroids, normals and
face areas against a completely different route to the same number.

**Topology.** Euler's formula $V - E + F = 2$ is a single scalar that catches
any inconsistency in the node, face and cell counts.

**Agreement with calculus.** The total meshed area is compared against the
analytic domain area — a rectangle behind the trailing edge plus a half ellipse
round the front, less the section — and the error is required to *shrink* under
refinement, since the outer boundary is a polygon inscribed in the true ellipse.

---

## 8. Rendering a mesh you can trust

A fine grid is ~830 × 128 nodes: over 200,000 line segments. Two mechanisms
keep that usable.

**Culling.** Segments outside the visible world rectangle are skipped and runs
of visible ones batched into single polylines. This is what makes zooming into
the leading edge cheap — almost the whole grid is off screen.

**Decimation.** If what survives culling is still over budget, every *n*th grid
line is drawn.

The second one is a lie about the mesh, so the stride is reported back to the
panel and displayed: *"Drawing every 4th grid line. Zoom in to see all of
them."* A thinned-out mesh that looks like the real one would be exactly the
kind of quietly misleading display this project is trying to avoid.

---

## 9. Status at the end of Phase 2

**Verified by running it**

- Clean configure, build and test; zero warnings including with
  `-DCFD_WARNINGS_AS_ERRORS=ON`.
- 134/134 tests pass (133 headless with `-DCFD_BUILD_APP=OFF`).
- Zero inverted cells at Coarse, Medium and Fine, on 0012, 2412, 4412, 0006
  and 6409.
- The rendered domain matches the requested extents: front arc at $x = -12c$,
  top and bottom at $y = \pm 12c$, outlet at $x = 26$ (trailing edge + 25c),
  wake cut along $y = 0$.
- Meshed area 844.9 c² against an analytic 845 c².

**Measured quality**

| | Cells | Wall faces | Max aspect | Max non-orth | Wall step |
|---|---|---|---|---|---|
| Coarse | 8,658 | 158 | 6,700 | 77.9° | 1.0×10⁻³ c |
| Medium | 30,530 | 318 | 14,700 | 74.0° | 3.0×10⁻⁴ c |
| Fine | 105,410 | 638 | 33,800 | 70.9° | 1.0×10⁻⁴ c |

The aspect ratios are extreme by design — boundary-layer cells are thin normal
to the wall and long along it. The non-orthogonality is the number I am least
happy with: ~75° occurs at the trailing edge, where the curvature limiter
deliberately gives up orthogonality to avoid folding. It is survivable but it
will cost accuracy in gradient reconstruction there, and is the obvious
candidate for improvement (elliptic smoothing with control functions) if the
solver later proves sensitive to it.

**Not verified**

- No flow of any kind. No discretisation of the governing equations, no
  boundary conditions applied, nothing solved.
- The wake cut is *represented* correctly but has never been exercised as an
  interior connection, because nothing crosses it yet.
- No comparison with another mesh generator or with wind-tunnel data.
- Still only built and run on macOS 15.7 / arm64 / Apple Clang 17.

## 10. What to understand before Phase 3

1. **The finite-volume statement** in §3 — Phase 3 discretises it. Every term
   there becomes code.
2. **Owner/neighbour and the outward normal convention.** Flux assembly loops
   over faces, adds to the owner and subtracts from the neighbour; the sign
   convention is what makes that work.
3. **Non-orthogonality costs accuracy.** A gradient reconstructed across a face
   is exact only when the face normal is parallel to the line joining the two
   centroids. Ours reaches 75° at the trailing edge, so the solver will need a
   non-orthogonal correction.
4. **Aspect ratio and time steps.** Cells 30,000× longer than they are tall have
   a very small dimension, and explicit schemes are limited by the smallest one.
   This is the argument for implicit time stepping, and it is already decided by
   the mesh.
5. **Concepts to read up on:** the CFL condition, upwinding and why central
   differencing on convection goes unstable, and how pressure and velocity are
   coupled in an incompressible solver.

## 11. What I learned

- **A scale-invariant failure cannot be fixed by changing the scale.** The
  instinct on a folded grid was to give the turn more room. Working out that
  $r\,\frac{d}{dr}f(r/L)$ depends only on $r/L$ showed that widening the zone
  does *nothing*, and pointed straight at changing the blend variable to
  $\log r$ instead. Doing the derivative rather than guessing saved a long
  session of tuning constants that could never have worked.

- **Smoothing can silently destroy a constraint.** The curvature limiter was
  computing the correct bound the whole time; eight averaging passes were
  throwing it away. When a value means "never exceed this", the only safe
  smoothing is one that can lower it — a Lipschitz sweep, not an average.

- **Diagnose before fixing.** Both folds looked like the same bug and were not.
  The first was a ray doubling back; the second was neighbouring rays
  converging at a corner. The measured fold distance (0.037 c) versus the
  concave radius (0.7 c) is what ruled out the wrong explanation in seconds.

- **Identities make better tests than tolerances.** $\sum \mathbf{n}A = 0$ and
  Euler's formula are exact statements about any valid mesh. They caught
  nothing in the end — the container was right first time — but they are the
  reason I could be confident the container was right, and could aim all the
  debugging at the generator.

- **Honesty has a rendering cost.** Decimating the mesh for speed is fine;
  decimating it silently is not. Reporting the stride was three lines and is the
  difference between a display you can reason from and one that quietly lies.

**Next:** Phase 3 — the Navier-Stokes discretisation, on explicit instruction.
