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
