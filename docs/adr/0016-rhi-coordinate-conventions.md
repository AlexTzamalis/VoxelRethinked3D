# ADR-016: RHI coordinate conventions follow OpenGL with zero to one depth

- **Status:** Accepted
- **Date:** 2026-10-27

## Context
Graphics APIs disagree on where pixel (0, 0) is and how depth is mapped. OpenGL puts the origin of
framebuffers, viewports, scissors and texture rows at the lower left and maps clip space depth from -1 to 1
by default. Vulkan and Direct3D put the origin at the upper left and map depth from 0 to 1. The RHI has to
pick one meaning for viewports, scissors, texture uploads and readbacks, or code above it would behave
differently per backend.

## Decision
- **Origin: lower left**, as in OpenGL. Viewport and scissor rectangles, render areas, and the rows of
  `TextureUpload` data and `Device::readTexture` results start at the bottom of the image. Clip space y
  points up.
- **Depth: 0 to 1**, as in Vulkan and Direct3D. The OpenGL backend calls
  `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` at startup, which reversed Z (ADR-010) needs anyway.
- **Front faces are counterclockwise** in the default `RasterState`, as seen with y up.
- **The window (swapchain) shows values as written.** sRGB encoding on write is only enabled for offscreen
  targets; post processing (E8.4) decides the display transform.

## Why
- The only backend today is OpenGL, so its convention costs nothing now and keeps the GL backend free of
  flips in every upload, readback and viewport call.
- Zero to one depth is not optional: reversed Z only keeps its precision with it (ADR-010).
- A future Vulkan backend can match this convention cheaply and in one place: a negative viewport height
  (core since Vulkan 1.1) flips y, and image rows are only reordered on upload and readback paths, which are
  not per frame hot paths.

## Consequences
- Image files (PNG and so on) store the top row first. Texture pack loading (G2.2) and screenshots flip
  rows when converting between files and the RHI.
- Tests that read pixels back (engine/tests/rhi/gl_device_tests.cpp) index rows from the bottom.
