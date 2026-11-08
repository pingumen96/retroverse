#version 450 core
// A panel of the VR mode's UI (I10 step 5, rv::gl::PanelPass): the UI's picture as RmlUi drew it (premultiplied alpha,
// display-encoded), blended over the view's display-ready output with ONE, ONE_MINUS_SRC_ALPHA like the UI over the
// window, faded by uPanelOpacity. I11 step 14: a captured game's sRGB texture (uPanelOpaqueLinear) is read as linear
// colour, encoded for the display and drawn opaque, as the large screen.
#include "common/common.glsl"
#include "common/srgb.glsl"

layout(binding = 14) uniform sampler2D uPanel;
uniform float uPanelOpacity;
uniform int uPanelOpaqueLinear;

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

void main() {
    vec4 picture = texture(uPanel, vUv);
    if (uPanelOpaqueLinear != 0) {
        picture = vec4(linearToSrgb(picture.rgb), 1.0);
    }
    oColor = picture * uPanelOpacity;
}
