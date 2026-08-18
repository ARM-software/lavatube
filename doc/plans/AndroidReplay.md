# Android replayer

The current implementation follows gfxreconstruct's NativeActivity lifecycle
and command-line intent design.

See [Android design](../Android.md) document for more information on the
current design.

## To be done

* Android system-log collection
* Multi-window Java/JNI support

## Multi-window support (written by codex)

The current Java-free `NativeActivity` implementation receives one
`ANativeWindow` from Android and uses it for the replay surface. JNI still lets
C++ call ordinary Java APIs, but it does not conveniently provide custom Java
subclasses or callback objects. Multiple surfaces require Android UI objects
whose creation and lifetime are asynchronous and confined to the UI thread.

Gfxreconstruct therefore uses a small Java bridge for its multi-window
replayer. Its custom activity extends `NativeActivity`, owns a `FrameLayout`,
and creates a `SurfaceView` for each replay window. A `SurfaceHolder.Callback`
reports when each Java `Surface` is created, changed, or destroyed. The surface
is passed through JNI to C++, which converts it with
`ANativeWindow_fromSurface`. Native code calls back into the activity to add or
remove views on the Android UI thread.

This design creates multiple replay surfaces inside one activity. It should not
be confused with Android's system-level multi-window mode or using multiple
activities.

When we implement this, we should:

* Add a minimal Java or Kotlin activity derived from `NativeActivity` and set
  `android:hasCode` to true.
* Keep view creation, removal, and `SurfaceHolder` callbacks on the UI thread.
* Pass surface creation and destruction events to C++ through a narrow JNI
  bridge, with stable surface indices.
* Convert each Java `Surface` to a separately owned `ANativeWindow` and map it
  to the corresponding replay `VkSurfaceKHR`; do not reuse the current single
  pending window for every surface.
* Store the bridge and lifetime state in the Android replay context rather than
  adding globals. Acquire and release every native window explicitly.
* Handle surface destruction, activity shutdown, rotation, and replay stop
  without leaving replay threads waiting for a surface callback.
* Add an Android trace test that creates at least two surfaces, destroys them in
  a different order, and verifies normal replay plus `lava-cli` stepping and
  shutdown in service mode.
