# voxel-engine-c-opengl-glfw-for-low-end-cpus-and-windows-only
This is a voxel engine with optimizations. For windows only.

This is an optimized voxel engine, more optimizations coming soon.
It also squeezes the performance out of low-end pc's.

***this is based on a CELERON N3050 with 2GB of storage. May not apply to other users.***

|**PERFORMANCE STATS** 
|-------------------
| **Memory Usage**: ~18MB   
| **Average FPS**: ~60FPS   
| **Frametime Gaps**: ~8-4MS

|**KEYBINDS**
| ---------
| *W A S D* - Movement
| *MIDDLE MOUSE BUTTON HELD*  - To move the camera around
| *CTRL + SHIFT* - Toggle fixed camera movement
| *LEFT CLICK* - Destroy blocks
| *RIGHT CLICK / PERIOD* - To place blocks

## OPTIMIZATIONS:
- Greedy Meshing
- Multithreaded Rendering
- Backface Culling
- And more...

## Requirements:
- Opengl 4.3 Availability, Your system must support this.
- Windows Machine, 7 and above, drivers must support opengl 4.3

## OTHER STUFF
- Naming is Self-Explanatory
- Settings are Inside "openglse.c"
> Be careful, there are critical parts for threading that can cause memory leaks if handles not properly.

## Setup:

1. **use GCC**: This is because the c code uses R"(..)" code.
2. **compile**: There will be a file, use the code and replace each path with its designated places.
3. **explaining**: Also, you must read 'UNZIP CGLM.ZIP' or just do what it says.
