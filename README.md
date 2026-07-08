# inspectrum
inspectrum is a tool for analysing captured signals, primarily from software-defined radio receivers.

![inspectrum screenshot](/screenshot.jpg)

## About this fork

This is a downstream fork of inspectrum. Its lineage:

1. **[miek/inspectrum](https://github.com/miek/inspectrum)** — the upstream project by Mike Walters, considered the community mainline.
2. **[Jacob Gilbert](https://github.com/jacobagilbert)'s SigMF improvements** — SigMF annotation support (rendering annotation boxes, labels and colours from a recording's `.sigmf-meta`), maintained as a patch set rebased on upstream.
3. **This fork** ([rajb245/inspectrum](https://github.com/rajb245/inspectrum)) — builds on both with Windows/macOS support and an assortment of quality-of-life tweaks:
   - **Windows build support** (MSVC + vcpkg) — see [Build from source](#windows-msvc--vcpkg) below.
   - **macOS support**, including an OpenGL-accelerated spectrogram renderer.
   - **SigMF annotation inspector** — click an annotation box to view its full fields in a collapsible tree (nested objects/arrays expand), with a right-click *Copy value*.
   - **[JSON-RPC remote-control interface](docs/remote-control.md)** — drive the app from any other program over a local socket (open a file, seek, snapshot the canvas to PNG, query state).
   - **Drag-and-drop** file opening from Finder / file managers.
   - **File reload** — a manual button plus auto-reload when the open recording changes on disk.
   - **Session persistence** — window size/position and the last-opened file are remembered across runs (stored in an INI file next to the executable).

These changes live on the `rebase-jacobagilbert` branch. Everything below documents inspectrum in general.

## Features
 * Large (100GB+) file support
 * Spectrogram with zoom/pan
 * Plots of amplitude, frequency, phase and IQ samples
 * Cursors for measuring period, symbol rate and extracting symbols
 * Export of selected time period, filtered samples and demodulated data

## Install
### Linux
Install inspectrum with your package manager, it should be present in most distros.

### macOS
 * [Homebrew](https://formulae.brew.sh/formula/inspectrum)
 * [MacPorts](https://ports.macports.org/port/inspectrum/)

### Windows
 * [radioconda](https://github.com/ryanvolz/radioconda)
 * [conda](https://anaconda.org/conda-forge/inspectrum)

## Build from source

### Linux / macOS

#### Prerequisites

 * cmake >= 3.1
 * fftw 3.x
 * [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) >= v1.3.0
 * pkg-config
 * qt5

#### Build instructions

Build instructions can be found here: https://github.com/miek/inspectrum/wiki/Build

#### Run

    ./inspectrum [filename]

### Windows (MSVC + vcpkg)

#### Prerequisites

 * [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
 * [CMake](https://cmake.org/download/) >= 3.11
 * [Git](https://git-scm.com/download/win)
 * [vcpkg](https://github.com/microsoft/vcpkg) — follow the [Getting Started](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started) guide to bootstrap it

Set the `VCPKG_ROOT` environment variable to your vcpkg install location:

    set VCPKG_ROOT=C:\path\to\vcpkg

#### Build instructions

Configure and build using the included CMake preset:

    cmake --preset windows
    cmake --build build --config RelWithDebInfo

The first configure will download and build Qt5 and FFTW from source via vcpkg (this takes ~30 minutes on first run). Prebuilt [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) binaries are fetched automatically via CMake FetchContent.

#### Install

Collect the executable and all required DLLs into a self-contained directory:

    cmake --install build --prefix build/install --config RelWithDebInfo

The output will be in `build\install\bin\` and can be run directly or copied anywhere.

#### Run

    build\install\bin\inspectrum.exe [filename]

## Input
inspectrum supports the following file types:
 * `*.sigmf-meta, *.sigmf-data` - SigMF recordings
 * `*.cf32`, `*.fc32`, `*.cfile` - Complex 32-bit floating point samples (GNU Radio, osmocom_fft)
 * `*.cf64`, `*.fc64` - Complex 64-bit floating point samples
 * `*.cs32`, `*.sc32`, `*.c32` - Complex 32-bit signed integer samples (SDRAngel)
 * `*.cs16`, `*.sc16`, `*.c16` - Complex 16-bit signed integer samples (BladeRF)
 * `*.cs8`, `*.sc8`, `*.c8` - Complex 8-bit signed integer samples (HackRF)
 * `*.cu8`, `*.uc8` - Complex 8-bit unsigned integer samples (RTL-SDR)
 * `*.f32` - Real 32-bit floating point samples
 * `*.f64` - Real 64-bit floating point samples (MATLAB)
 * `*.s16` - Real 16-bit signed integer samples
 * `*.s8` - Real 8-bit signed integer samples
 * `*.u8` - Real 8-bit unsigned integer samples

If an unknown file extension is loaded, inspectrum will default to `*.cf32`.

Note: 64-bit samples will be truncated to 32-bit before processing, as inspectrum only supports 32-bit internally.
