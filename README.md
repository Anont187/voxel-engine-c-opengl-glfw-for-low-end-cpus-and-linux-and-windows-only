# voxel-engine-c-opengl-glfw-for-low-end-cpus-and-windows-only
This is a voxel engine with optimizations. For windows only.

This is an optimized voxel engine, more optimizations coming soon.
It also squeezes the performance out of low-end pc's.

***this is based on a CELERON N3050 with 2GB of RAM. May not apply to other users.***

|**PERFORMANCE STATS** 
|-------------------
| **Memory Usage**: ~18MB   
| **Average FPS**: ~60FPS   
| **Frametime Gaps**: ~12MS+

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
- Windows Machine, 7 and above, or a Linux machine. Drivers must support opengl 4.3

## OTHER STUFF
- Naming is Self-Explanatory
- Settings are Inside "openglse.c"
> Be careful, there are critical parts for threading that can cause memory leaks if handles not properly.

## Setup:

1. **to do**: You must read 'UNZIP CGLM.ZIP' first before compiling.
2. **use GCC**: This is because the c code uses C++ raw string literals (GNU extension).
3. **compile**: If you prefer the development version, or the Release version of the compiler, you can use the builds down below.

And it also depends on what platfrom you are on. If you are on LINUX, you can compile with gcc. 

Same with windows. You have to had/have MinGW64 installed, else you cant compile. 

Please note that if you are using a strict c standard (-std=c99), please remove it. This code relies on GNU extensions as said earlier. GCC must be an environmental variable on windows.

- step 1: Download the whole project to a folder.
- step 2: Go inside the folder and either run:
- *please install dependencies first if on linux (e.g., `sudo apt install libglfw3-dev libgl1-mesa-dev libx11-dev libxi-dev libxrandr-dev` on Ubuntu/Debian).*
- Windows Build (MinGW64 Required)
``` bash
gcc openglse.c glad.c -Iinclude -Llib -O3 -march=native -mtune=native -ffast-math -flto -funroll-loops -fprefetch-loop-arrays -s -lglfw3 -lgdi32 -lopengl32 -lm -static -lpsapi -o openglse.exe
```
- Linux Build (Native GCC)
```bash
gcc openglse.c glad.c -Iinclude -Llib -O3 -march=native -mtune=native -ffast-math -flto -funroll-loops -fprefetch-loop-arrays -s -glfw -lGL -lm -lpthreadi -o openglse
```
