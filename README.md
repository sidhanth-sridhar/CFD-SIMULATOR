# CFD-SIMULATOR

A 2D Reynolds-Averaged Navier-Stokes solver for NACA airfoil sections, with an
interactive viewer.

**Status: Phase 2 — computational domain and mesh. No CFD is implemented.**

The application generates NACA four-digit sections and builds a structured
C-grid around them. Type a designation such as `NACA 2412`, tick **Generate
mesh**, and the computational domain appears with its boundaries, cell counts
and quality measures.

There is still no discretisation of the flow equations and no solution. The
geometry and the mesh are verified against analytic results and against a set
of finite-volume identities (see [Validation](#validation)), but nothing here
has been compared with wind-tunnel or reference CFD data, because nothing here
computes a flow.

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
| `--mesh LEVEL` | Mesh at startup: `coarse`, `medium` or `fine` |
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
adjusted closed-trailing-edge form. The closed form is the default because it
is the one the mesher can wrap.

### Generating a mesh

Tick **Generate mesh** in the Mesh panel. A structured C-grid is built around
the section: grid lines wrap the nose like the letter C, then run downstream on
both sides of a cut along the wake.

| Control | Meaning |
|---|---|
| Resolution | `Coarse` / `Medium` / `Fine` — sets surface, wake and normal point counts and the near-wall spacing |
| Upstream / Downstream / Vertical | Domain extent in chord lengths; upstream and vertical are measured from the leading edge, downstream from the trailing edge |
| First layer | Height of the first cell off the wall, as a fraction of chord |
| Grid / Boundaries | Show the interior grid lines and the coloured boundary faces |
| Fit domain | Frame the whole domain rather than the section |

Boundaries are coloured by the condition they will carry: **wall** (white),
**wake cut** (amber), **far field** (blue) and **outlet** (green).

Defaults are 12 chords upstream, 25 downstream and 12 vertically.

| Resolution | Cells | Wall faces | First layer |
|---|---|---|---|
| Coarse | ~8,700 | 158 | 1×10⁻³ c |
| Medium | ~30,500 | 318 | 3×10⁻⁴ c |
| Fine | ~105,000 | 638 | 1×10⁻⁴ c |

A mesh needs a closed trailing edge, since the wake cut has to start from a
single point. Selecting the open trailing edge reports this rather than
producing a broken grid.

When the whole domain is on screen, drawing every grid line is neither fast nor
informative, so the view is decimated and the panel says so ("Drawing every
*n*th grid line"). Zoom in and the full mesh is drawn.

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
│   ├── mesh/                   Mesh, CGrid
│   └── app/                    Application, Camera2D
├── src/
│   ├── main.cpp                CLI parsing and startup
│   ├── core/                   cfd_core — no GUI, no CFD, no platform code
│   ├── geom/                   cfd_geometry — NACA equations and discretisation
│   ├── mesh/                   cfd_mesh — domain, C-grid, finite-volume metrics
│   └── app/                    cfd_app — GLFW, OpenGL and Dear ImGui live here only
└── tests/                      GoogleTest suite, registered with CTest
```

### Module boundaries

```
cfd_sim ──▶ cfd_app ──▶ cfd_mesh ──▶ cfd_geometry ──▶ cfd_core ──▶ std
                │
                └──▶ Dear ImGui, GLFW, OpenGL
```

`cfd_core` depends on nothing but the standard library; each layer above adds
exactly one concern. `cfd_app` is the only module that knows GLFW, OpenGL or
Dear ImGui exist. Solver code added in later phases sits at the mesh level or
below, so the numerics remain buildable and testable on a machine with no
display — which is what `-DCFD_BUILD_APP=OFF` verifies.

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

The mesh tests check the properties a finite-volume solver depends on:

- **Connectivity** — Euler's formula `V − E + F = 2`; every interior face
  referenced by exactly two cells and every boundary face by one; owners,
  neighbours and node indices all in range and consistent.
- **Validity** — every cell area strictly positive, no inverted cells at any
  resolution or on any section.
- **Metrics** — face normals are unit vectors pointing out of their owner; the
  outward area vectors of every cell sum to zero; cell areas recovered from
  their faces by the divergence theorem match the shoelace areas.
- **Boundaries** — wall, wake cut, far field and outlet faces are identified
  and counted, sit where the options say they should, and the two sides of the
  wake cut are matched, coincident and oppositely oriented.
- **Refinement** — cell count, wall resolution and near-wall spacing all move
  monotonically with resolution, and the total domain area converges to the
  analytic area of the domain.

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

Phases 0 to 2 are complete. Later phases build on them in order:

1. ~~NACA 4-digit geometry generation~~ — done
2. ~~Mesh generation around the section~~ — done
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
