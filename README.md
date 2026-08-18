# voxel-engine-c-opengl-glfw-for-lowend-cpus-and-windows-only
This is a voxel engine with optimizations. For windows only.

This is an extremely optimized voxel engine, more optimizations coming soon.

**PERFORMANCE STATS**
***this is based on a CELERON N3050 with 2GB of storage. May not apply to other users.***
---------------------
*Memory Usage*: ~18MB
*Average FPS*: ~60FPS
*Frametime Gaps*: ~8-4MS
---------------------

**KEYBINDS**
---------------------
*W A S D* - Movement
*MIDDLE MOUSE BUTTON HELD*  - To move the camera around
*CTRL + SHIFT* - Toggle fixed camera movement
*LEFT CLICK* - Destroy blocks
*RIGHT CLICK / PERIOD* - To place blocks
---------------------

- Naming is Self-Explanatory
- Settings are Inside "openglse.c"
> Be careful, there are critical parts for threading that can cause memory leaks if handles not properly.

**how to setup**:

1. **use GCC**: This is because the c code uses R"(..)" code.
2. **compile**: There will be a file, use the code and replace each path with its designated places.
3. **explaining**: Well, i also forgot to say that to read "UNZIP CGLM.ZIP".txt so, read that also before reading this. Whoops, too late.
