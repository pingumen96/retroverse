# I10 step 5: the UI in VR

Roadmap row: I10 step 5. Decision: [ADR-0017](../decisions/ADR-0017-vr-through-openxr.md) §6. Built on step 4's
controllers ([step-4.md](step-4.md)).

## What was built

- **One panel for the product UI** (`XrUi`, `src/app/src/xr_ui.cpp`): each VR frame the UI's whole canvas (the window's
  layout, every document of the context) is drawn into a texture (`ProductUi::renderTo` into a GL_SRGB8_ALPHA8 canvas
  written raw, copied into an RGBA8 picture with mipmaps), and that picture stands on a panel in the world
  (`xr::PanelPose`, `rv_xr_model`'s `panels.hpp`):
  - **World**: while the UI has the input (the quick catalog, the directory, the media viewer, a launch's panel), the
    panel stands where the user looked when it took the input: 1.3 m ahead along the head's heading, 12 cm below the
    eyes, 1.8 m wide at the window's aspect, upright and facing back. It stays there while the user turns or walks.
  - **Head**: otherwise the canvas is a heads-up display locked to the head, 1.6 m ahead across a 64-degree field at
    the window's aspect (`xr::hudField`). The hover's label is projected through that field (`XrUi::hudCamera`), so it
    lands over its item; the prompts, the notices and the card sit where the window's layout puts them.
- **The panel in the eyes** (`gl::PanelPass`, `assets/shaders/xr/panel.*`): drawn into each eye after its
  post-processing, blended in the display's encoding with premultiplied alpha as the UI over the window, on top of the
  scene, faded with the view (a ride's fade, a teleport's blink). The window's mirror and the screenshots show it
  through the eyes; their 2D overlay leaves the product UI out in VR (the developer overlay stays in the window).
- **The ray as the UI's mouse**: on the world panel the pointing controller's ray moves RmlUi's mouse to the hit's
  pixel (`ProductUi::xrPointer`), its trigger presses and releases the left button (and is then no interact), the right
  stick scrolls; off the panel the mouse leaves the context. The pad and the controllers' buttons still navigate as
  before (A activates, B goes back, the left stick moves the focus).
- **The fake runtime's triggers**: an input script's `leftSelect` and `rightSelect` steps ([0, 1]).
- **The run report's `xr` section** adds the frames the panel stood in the world, those the ray was on it, the clicks.

## Proof

- **Unit tests**: the world panel's place and axes, its corners; the ray's pixel on a panel (front only, within it);
  the heads-up display showing a point where its field projects it (turned and rolled heads), its field at the canvas's
  aspect; the triggers in input scripts; the shader list and the post block unchanged otherwise.
- **GPU smoke test** (`tests/smoke/test_xr.cpp`, the quick catalog): the fake headset in the test room with a mock
  catalog; the catalog opens at frame 5 and stands in the world, the right controller's ray rests on its left half from
  frame 10, one click at frame 30; the screenshot holds both eyes with the panel.
- **By eye:** that screenshot; the step 4 run's card in the heads-up display beside the held cartridge.

## By hand, in the headset

- Y opens the quick catalog in front of you: point at a game with either controller, pull the trigger to select it, the
  right stick scrolls the list, A starts the selected game ("Avvia"), B closes it.
- The left stick's click opens the directory the same way; a floor or a room is a click.
- Point at a cover: its title floats over it; inspect it: the card shows at the right of the view.

## Deviations and left for later

- ADR-0017 §6 asks for quad layers of the compositor; the panel is drawn into the eyes instead (its fallback), on every
  runtime alike. Compositor layers (sharper text) are parked in the BACKLOG.
- The card shows in the heads-up display's right third rather than beside the item in the world; the labels and
  prompts are not separate small quads. Both follow the window's layout, which keeps one UI for the desktop and VR.
- The developer overlay (ImGui) stays in the window.
