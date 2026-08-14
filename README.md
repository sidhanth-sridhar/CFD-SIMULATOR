# CFD-SIMULATOR

A 2D Reynolds-Averaged Navier-Stokes solver for NACA airfoil sections, with an
interactive viewer.

**Status: Phase 1 — airfoil geometry. No CFD is implemented.**

The application generates and draws NACA four-digit sections. Type a
designation such as `NACA 2412` and the section appears, along with its
measured thickness, camber, area and perimeter.

There is still no mesh, no discretisation and no flow solution. The geometry is
verified against the analytic NACA equations and against calculus (see
[Validation](#validation)), but nothing here has been compared with wind-tunnel
or reference CFD data, because nothing here computes a flow.

---

## Requirements

| | |
|---|---|
| Compiler | C++20 (tested with Apple Clang 17) |
| Build system | CMake ≥ 3.24 |
| Graphics | OpenGL 3.2 core profile |

Third-party dependencies are downloaded automatically at configure time and
pinned by SHA-256; no package manager is required. An internet connection is
needed for the first configure only.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Useful options:

```sh
# Headless: core library and tests only, no window system needed.
cmake -S . -B build -DCFD_BUILD_APP=OFF

# Treat compiler warnings as errors (recommended before committing).
cmake -S . -B build -DCFD_WARNINGS_AS_ERRORS=ON

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

| Option | Default | Effect |
|---|---|---|
| `CFD_BUILD_APP` | `ON` | Build the GUI shell and the `cfd_sim` executable |
| `CFD_BUILD_TESTS` | `ON` | Build the unit test suite |
| `CFD_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror` to first-party targets |

The default build type for single-config generators is `RelWithDebInfo`.

## Test

```sh
ctest --test-dir build
```

Add `--output-on-failure` to see diagnostics from failing tests.

## Run

```sh
./build/bin/cfd_sim
```

| Flag | Purpose |
|---|---|
| `-h`, `--help` | Usage summary |
| `-V`, `--version` | Version, build type, compiler |
| `--section NAME` | Load a section at startup, e.g. `--section "NACA 2412"` |
| `--log-level LEVEL` | `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off` |
| `--self-check` | Headless subsystem check, then exit (this is what CTest runs) |
| `--screenshot FILE` | Render a few frames, save the window as a BMP, exit |

### Generating a section

Type a four-digit designation into **Section** in the Geometry panel. The
`NACA` prefix and any whitespace are optional, so `NACA 2412`, `naca2412` and
`2412` are equivalent. The shape updates as you type; while the input is
incomplete the last valid section stays on screen and the problem is reported
beneath the field.

The four digits are maximum camber in percent of chord, its position in tenths
of chord, and maximum thickness in percent of chord. So `2412` is 2% camber at
40% chord and 12% thick, while `0012` is symmetric and 12% thick.

You can also set the number of points per surface, the chord length, and
whether the trailing edge uses the standard (slightly blunt) polynomial or the
adjusted closed-trailing-edge form.

### Viewport controls

| Input | Action |
|---|---|
| Left or middle drag | Pan |
| Scroll wheel | Zoom about the cursor |
| `F` | Reset view |

Panel dividers are draggable; panels can be toggled from the **View** menu.

## Layout

```
CFD-SIMULATOR/
├── CMakeLists.txt              Top-level build: options, warnings, subdirectories
├── cmake/
│   ├── CompilerWarnings.cmake  Strict warning set for first-party targets
│   ├── Dependencies.cmake      Hash-pinned FetchContent declarations
│   └── Version.hpp.in          Template for the generated version header
├── include/cfd/                Public headers
│   ├── core/                   BuildInfo, Error/Result, Log, Vec2
│   ├── geom/                   Naca4, Airfoil
│   └── app/                    Application, Camera2D
├── src/
│   ├── main.cpp                CLI parsing and startup
│   ├── core/                   cfd_core — no GUI, no CFD, no platform code
│   ├── geom/                   cfd_geometry — NACA equations and discretisation
│   └── app/                    cfd_app — GLFW, OpenGL and Dear ImGui live here only
└── tests/                      GoogleTest suite, registered with CTest
```

### Module boundaries

```
cfd_sim ──▶ cfd_app ──▶ cfd_geometry ──▶ cfd_core ──▶ standard library
                │
                └──▶ Dear ImGui, GLFW, OpenGL
```

`cfd_core` depends on nothing but the standard library, and `cfd_geometry`
depends only on `cfd_core`. `cfd_app` is the only module that knows GLFW,
OpenGL or Dear ImGui exist. Solver code added in later phases sits at the
geometry level or below, so the numerics remain buildable and testable on a
machine with no display — which is what `-DCFD_BUILD_APP=OFF` verifies.

## Validation

The geometry tests check three different kinds of property:

- **Exact construction identities** — the midpoint of corresponding upper and
  lower surface points lies on the camber line; the line joining them is
  perpendicular to the camber line; a symmetric section's surfaces are exact
  mirror images.
- **Agreement with the designation** — thickness, camber and their chordwise
  positions are measured back out of the generated points and compared with
  what the digits specify. The camber line reaches exactly `m` at exactly `p`.
- **Agreement with calculus** — the area enclosed by the discretised contour,
  computed with the shoelace formula, is compared against the analytic integral
  of the thickness polynomial, including a refinement study confirming the
  error falls quadratically as points are added.

The thickness polynomial is also checked against the tabulated NACA 0012
ordinate of 0.06002c at 30% chord.

No comparison has been made against wind-tunnel data or another CFD code.

## Dependencies

| Library | Version | Role | Fetched |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | Immediate-mode GUI | configure time |
| [GLFW](https://github.com/glfw/glfw) | 3.5.1 | Window, OpenGL context, input | configure time |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | Unit tests | configure time |
| OpenGL | 3.2 core | Rendering | system |

Each is pinned to a release tarball plus a SHA-256 hash, so a configure either
reproduces the exact same sources or fails loudly.

## Roadmap

Phases 0 and 1 are complete. Later phases build on them in order:

1. ~~NACA 4-digit geometry generation~~ — done
2. Mesh generation around the section
3. Navier-Stokes discretisation
4. Reynolds averaging (RANS)
5. k-ω SST turbulence closure
6. Force and moment integration (lift, drag, moment coefficients)
7. Boundary-layer separation detection
8. Vortex structures and stall behaviour

## Documentation

[`JOURNAL.md`](JOURNAL.md) is a technical journal explaining what each phase
implements, why it is built that way, and the concepts behind it.

## License

Not yet chosen.
