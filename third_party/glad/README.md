# glad (vendored)

OpenGL function loader generated with [glad2](https://github.com/Dav1dde/glad) 2.0.8 from the Khronos XML
registry bundled with the generator (`--reproducible`).

Why vendored instead of the vcpkg `glad` port: that port is glad 1 (0.1.36), it generates at build time with
Python and defaults to the compatibility profile. Retroverse requires an exact OpenGL 4.5 **core** profile
loader, and RmlUi's GL3 backend can reuse this same copy through `RMLUI_GL3_CUSTOM_LOADER`.

Regenerate (from a scratch virtual environment with `pip install glad2==2.0.8`):

```powershell
python -m glad --quiet --reproducible `
  --api gl:core=4.5 `
  --extensions GL_KHR_debug,GL_EXT_texture_filter_anisotropic,GL_ARB_texture_filter_anisotropic,GL_ARB_clip_control,GL_EXT_texture_compression_s3tc `
  --out-path third_party/glad c
```

The generated loader has no built-in function loader: call `gladLoadGL(SDL_GL_GetProcAddress)` after the
context is current. Generated files are licensed as described in the glad project (MIT/Apache-2.0 for the
generator output, Apache-2.0/MIT for the Khronos registry content); `KHR/khrplatform.h` carries its own
Khronos license header.
