#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "cglm/cglm.h"
#include "uthash.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <time.h>

#if defined(_WIN32) || defined(_WIN64)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif

    #include <windows.h>
    #include <psapi.h>

    typedef CRITICAL_SECTION EngineMutex;
    typedef CONDITION_VARIABLE EngineCondVar;
    typedef HANDLE EngineThread;

    #define MUTEX_INIT(mock) InitializeCriticalSection(mock)
    #define MUTEX_LOCK(mock) EnterCriticalSection(mock)
    #define MUTEX_UNLOCK(mock) LeaveCriticalSection(mock)
    #define MUTEX_DESTROY(mock) DeleteCriticalSection(mock)

    #define COND_INIT(cv)          InitializeConditionVariable(cv)
    #define COND_SIGNAL(cv)        WakeConditionVariable(cv)
    #define COND_DESTROY(cv)       /* binbows handle this native */
    #define THREAD_YIELD()         SwitchToThread()
    #define ATOMIC_CHECK(var)      InterlockedCompareExchange(&var, 0, 0)
    #define ATOMIC_SET(var, val)   InterlockedExchange(&var, val)

    volatile long isRunning = 1;
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <sched.h>
    #include <sys/time.h>

    typedef pthread_mutex_t        EngineMutex;
    typedef pthread_cond_t         EngineCondVar;
    typedef pthread_t              EngineThread;

    #define MUTEX_INIT(mock)       pthread_mutex_init(mock, NULL)
    #define MUTEX_LOCK(mock)       pthread_mutex_lock(mock)
    #define MUTEX_UNLOCK(mock)     pthread_mutex_unlock(mock)
    #define MUTEX_DESTROY(mock)    pthread_mutex_destroy(mock)
    
    #define COND_INIT(cv)          pthread_cond_init(cv, NULL)
    #define COND_SIGNAL(cv)        pthread_cond_signal(cv)
    #define COND_DESTROY(cv)       pthread_cond_destroy(cv)
    #define THREAD_YIELD()         sched_yield()
    #define ATOMIC_CHECK(var)      __sync_val_compare_and_swap(&var, 0, 0)
    #define ATOMIC_SET(var, val)   __sync_lock_test_and_set(&var, val)

    volatile int isRunning = 1;
#endif

const char* vertexSrc = R"(
#version 430

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in float aLayer;
layout (location = 3) in float aFaceId;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 uvPass;
out vec3 worldPos;

flat out int layerPass;
flat out int faceIdPass;

void main() {

    uvPass = aUV;
    layerPass = int(aLayer);
    faceIdPass = int(aFaceId);
    if (faceIdPass == 6 || faceIdPass == 9) {
        gl_Position = vec4(aPos.xy, -1.0, 1.0);
    } else {
        worldPos = aPos;
        gl_Position = projection * view * model * vec4(aPos, 1.0);
    }
}
)";

const char* fragmentSrc = R"(
#version 430

in vec2 uvPass;
in vec3 worldPos;

flat in int layerPass;
flat in int faceIdPass;

uniform sampler2DArray texAtlas;
uniform sampler2D dofColorTex;
uniform sampler2D dofDepthTex;

uniform vec3 sunDir;
uniform vec3 camPos;

uniform float dofNear;
uniform float dofFar;
uniform float dofFocusDist;
uniform float uTimeOfday;

uniform int dofEnabled;

uniform mat4 projection;

float pi = 3.14159265;

const vec3 faceNorms[6] = vec3[6](
    vec3(-1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, -1.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, -1.0),
    vec3(0.0, 0.0, 1.0)
);

const vec3 sideNorms[4] = vec3[4](
    vec3(0.0, 0.0, -1.0),
    vec3(0.0, 0.0, 1.0),
    vec3(-1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0)
);

out vec4 fragColor;

vec3 applyFog(vec3 baseColor, vec3 fragWorldPos, vec3 skyColor) {
    float dist = distance(camPos, fragWorldPos);
    float fogStart = 55.0;
    float fogEnd = 180.0;
    float fogAmount = clamp((dist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);

    vec3 fogColor = vec3(0.2, 0.3, 0.3);

    return mix(baseColor, skyColor, fogAmount);
}

void main() {
    float sunAngle = uTimeOfday * 2.0 * pi;

    vec3 sunDirDynamic = normalize(vec3(cos(sunAngle), sin(sunAngle), 0.3));

    float dayFactor = clamp(sin(sunAngle), 0.0, 1.0);
    float duskFactor = clamp(1.0 - abs(sin(sunAngle)) * 2.0, 0.0, 1.0);

    vec3 daySkyColor = vec3(0.2, 0.3, 0.3);
    vec3 nightSkyColor = vec3(0.01, 0.01, 0.05);
    vec3 duskSkyColor = vec3(0.6, 0.25, 0.1);
    vec3 skyColor = mix(mix(nightSkyColor, daySkyColor, dayFactor), duskSkyColor, duskFactor);

    float ambientMin = mix(0.05, 0.35, dayFactor);
    float aoFactor = 1.0;

    if (dofEnabled == 1 && faceIdPass != 6 && faceIdPass != 9) {
        float depthRaw = texture(dofDepthTex, uvPass).r;
        float ndc = depthRaw * 2.0 - 1.0;
        float linearDepth = (2.0 * dofNear * dofFar) / (dofFar + dofNear - ndc * (dofFar - dofNear));
        float radius = 0.005;

        int samples = 8;

        vec2 noise[4] = vec2[4](
            vec2(0.0, 1.0), vec2(0.866, 0.5), vec2(0.866, -0.5), vec2(0.0, -1.0)
        );

        vec2 randomVec = noise[int(gl_FragCoord.x) % 4];

        float occlusion = 0.0;

        for (int i = 0; i < samples; i++) {
            float angle = 6.28318 * float(i) / float(samples);

            vec2 offset = vec2(cos(angle), sin(angle)) * radius;

            offset += randomVec * radius * 0.5;

            vec2 sampleUv = uvPass + offset;

            float sampleDepthRaw = texture(dofDepthTex, sampleUv).r;
            float sampleNdc = sampleDepthRaw * 2.0 - 1.0;
            float sampleLinear = (2.0 * dofNear * dofFar) / (dofFar + dofNear - sampleNdc * (dofFar - dofNear));
            float rangeCheck = abs(linearDepth - sampleLinear) < 0.5 ? 1.0 : 0.0;

            occlusion += (sampleLinear < linearDepth ? 1.0 : 0.0) * rangeCheck;
        }
        occlusion /= float(samples);

        aoFactor = 1.0 - occlusion * 0.5;
    }

    if (faceIdPass == 6) {
        fragColor = vec4(1.0, 1.0, 1.0, 0.9);
        return;
    }

    if (faceIdPass == 7) {
        vec3 grassCol = vec3(0.168f, 0.3686f, 0.0471f);
        float diff = max(dot(vec3(0.0, 1.0, 0.0), sunDirDynamic), 0.0);
        float lighting = 0.35 + 0.65 * diff;
        fragColor = vec4(applyFog(grassCol * lighting, worldPos, skyColor) * aoFactor, 1.0);
        return;
    } 

    if (faceIdPass == 8) {
        vec3 dirtCol = vec3(0.4667f, 0.3059f, 0.0902f) * 0.7;
        vec3 normal = sideNorms[layerPass];

        float diff = max(dot(normal, sunDirDynamic), 0.0);
        float lighting = 0.35 + 0.65 * diff;

        fragColor = vec4(applyFog(dirtCol * lighting, worldPos, skyColor) * aoFactor, 1.0);
        return;
    }

    if (faceIdPass == 9) {
        float centerDepthRaw = texture(dofDepthTex, vec2(0.5, 0.5)).r;
        float centerNdc = centerDepthRaw * 2.0 - 1.0;
        float centerLinear = (2.0 * dofNear * dofFar) / (dofFar + dofNear - centerNdc * (dofFar - dofNear));

        float depthRaw = texture(dofDepthTex, uvPass).r;
        float ndc = depthRaw * 2.0 - 1.0;
        float linearDepth = (2.0 * dofNear * dofFar) / (dofFar + dofNear - ndc * (dofFar - dofNear));
        float coc = clamp(abs(linearDepth - centerLinear) / dofFocusDist, 0.0, 1.0);

        vec2 texel = 1.0 / vec2(textureSize(dofColorTex, 0));
        vec4 sharp = texture(dofColorTex, uvPass);

        vec2 offsets[12] = vec2[12](
            vec2(0.0, 1.0), vec2(0.866, 0.5), vec2(0.866, -0.5), vec2(0.0, -1.0),
            vec2(-0.866, -0.5), vec2(-0.866, 0.5), vec2(0.0, 2.0), vec2(1.732, 1.0),
            vec2(1.732, -1.0), vec2(0.0, -2.0), vec2(-1.732, -1.0), vec2(-1.732, 1.0)
        );

        vec4 blurred = sharp;
        for (int i = 0; i < 12; i++) {
            blurred += texture(dofColorTex, uvPass + offsets[i] * texel * 3.0 * coc);
        }
        blurred /= 13.0;

        fragColor = mix(sharp, blurred, coc);
        return;
    }

    if (faceIdPass == 10) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 texColor = texture(texAtlas, vec3(uvPass, layerPass));

    if (layerPass == 1) {
        texColor.rgb *= vec3(0.42, 0.68, 0.30);
    }

    vec3 normal = faceNorms[faceIdPass];

    float diff = max(dot(normal, sunDirDynamic), 0.0);
    float lighting = 0.35 + 0.65 * diff;

    fragColor = vec4(applyFog(texColor.rgb * lighting, worldPos, skyColor) * aoFactor, texColor.a);
}
)";

#define chunkSize 32
#define renderRad 2
#define lodTileSize 64
#define lodCellSize 4
#define lodRadius 6
#define maxTerrainHeight 140
#define noiseStep 4
#define noiseGridSize (chunkSize / noiseStep + 1)
#define maxDrawCommands 4096
#define masterVboSize (64 * 1024 * 1024 * sizeof(float))
#define masterEboSize (32 * 1024 * 1024 * sizeof(unsigned int))
#define maxUploadsPerFrame 256
#define dirtyQueueSize 512
#define lodBuildQueueSize 64

int width = 640;
int height = 480;
int goWireframe = 0;
int activeIdxCount = 0;
int dofFboWidth = 0;
int dofFboHeight = 0;
int winX;
int winY;
int winWidth;
int winHeight;
int highlightX;
int highlightY;
int highlightZ;
int globalCommandCount = 0;
int pwinX;
int pwinY;
int pwinWidth;
int pwinHeight;
int highlightRaycastCooldown = 0;
int pendingCount = 0;
int dirtyQueueHead = 0;
int dirtyQueueTail = 0;
int lodBuildHead = 0;
int lodBuildTail = 0;
int currentIndirectBuffer = 0;

float pcamSpeed = 4.3f;
float rotSpeed = 60.0f;
float sprintSpeed = 50.0f;
float sensitivity = 0.1f;
float baseSensitivity = 0.1f;
float bfov = 90.0f;
float cfov = 90.0f;
float scale = 0.015f;

bool f1WasPressed = false;
bool periodWasPressed = false;
bool keyWasTouched = false;

float camX = 0.0f;
float camY = 60.0f; 
float camZ = 25.0f;
float camYaw = -90.0f;
float camPitch = -20.0f;
float lastMouseX = 0;
float lastMouseY = 0;
float steveWidth = 0.6f;
float steveHeight = 1.8f;
float eyeHeight = 1.60f;
float velY = 0.0f;
float flyVelX = 0.0f;
float flyVelY = 0.0f;
float flyVelZ = 0.0f;
float PI = 3.14159265f;

double lastSpaceTapTime = -1.0;

bool isCameraRotateble = false;
bool firstMouseInput = true;
bool isFlying = false;
bool isGrounded = false;
bool isFullscreen = false;
bool spaceKeyWasDown = false;
bool hasBlockHighlight = false;
bool toggleDof = false;

unsigned int dofFbo = 0;
unsigned int dofColorTex = 0;
unsigned int dofDepthTex = 0;
unsigned int highlightVao;
unsigned int highlightVbo;
unsigned int masterVao = 0;
unsigned int masterVbo = 0;
unsigned int masterEbo = 0;
unsigned int masterIndirectBuffer = 0;

size_t masterVboOffsetBytes = 0;
size_t masterEboOffsetElements = 0;

const float gravity = 28.0f;
const float jumpSpeed = 8.0f;
const float terminalVel = -50.0f;
const float flyAccel = 6.0f;
const float flySpeedMax = 10.0f;

const double doubleTapWindow = 0.3f;

typedef struct {
    GLuint count;
    GLuint instanceCount;
    GLuint firstIndex;
    GLuint baseVertex;
    GLuint baseInstance;
} drawElementsIndirectCommand;

typedef struct {
    int indexCount;
    int firstIndex;
    int baseVertex;
} meshs;

typedef struct {
    char key[64];
    int x, y, z;
    UT_hash_handle hh;
} voxelBlock;

typedef struct {
    int cx, cy, cz;
} chunkCoord;

typedef struct {
    uint32_t voxels[chunkSize][chunkSize];
    meshs* mesh;
    int isDirty;
    int isGenerated;

    float* cpuVertices;
    unsigned int* cpuIndices;
    int cpuVertCount;
    int cpuIndCount;
    volatile bool isMeshReady;

    volatile bool isMeshing;
    int genFailCount;
    int isBroken;

    GLuint occlusionQuery;
    bool queryIssued;
    bool wasVisible;

    bool gpuUploaded;
    uint32_t meshVersion;
    uint32_t cpuMeshVersion;

    bool isQueued;
} chunks;

typedef struct {
    uint64_t key;
    int cx, cy, cz;
    chunks chunk;
    UT_hash_handle hh;
} chunkEntry;

typedef struct {
    float a, b, c, d;
} plane;

typedef struct {
    unsigned int vao, vbo;
    int vertCount;
    int cellSize;
    bool built;

    float* cpuVerts;
    int cpuVertCount;
    bool cpuReady;
    bool isBeingBuilt;
} lodTile;

typedef struct {
    long long key;
    int tx, tz;
    lodTile tile;
    UT_hash_handle hh;
} lodTileEntry;

typedef struct {
    float* verts;
    unsigned int* inds;

    int vertCount;
    int indCount;
    int meshBaseVertex;
    int meshFirstIndex;

    chunks* chunk;
} pendingUpload;

typedef struct {
    chunks* chunk;
    int cx, cy, cz;
} dirtyQueueEntry;

inline uint64_t getChunkkey(int cx, int cy, int cz) {
    uint64_t ucx = (uint64_t)(cx + 1000000);
    uint64_t ucy = (uint64_t)(cy + 1000000);
    uint64_t ucz = (uint64_t)(cz + 1000000);

    return (ucx<< 42) | (ucz << 21) | ucy;
}

lodTileEntry* loadedLodTiles = NULL;
lodTileEntry* lodBuildQueue[lodBuildQueueSize];
chunkEntry* loadedChunks = NULL;

drawElementsIndirectCommand hostCommands[maxDrawCommands];
pendingUpload pendingUploads[maxUploadsPerFrame];
dirtyQueueEntry dirtyQueue[dirtyQueueSize];

const char* stringed = "basic window";
const char* texturePath[] = { "textures/dirt.png", "textures/grass.png" };

EngineMutex chunkLock;
EngineMutex lodLock;

EngineCondVar chunkWorkReady;

static inline float noiseFade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline float noiseLerp(float t, float a, float b) {
    return a + t * (b - a);
}

static inline float noiseGrad(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

static inline int voxelIdx(int dx, int dy, int dz, int ly, int lz) {
    return (((dx * 3 + dy) * 3 + dz) * chunkSize + ly) * chunkSize + lz;
}

static inline float perlin2d(float x, float y) {
    int X = (int)floorf(x) & 255;
    int Y = (int)floorf(y) & 255;

    x -= floorf(x);
    y -= floorf(y);

    float u = noiseFade(x);
    float v = noiseFade(y);

    int a = (X + Y * 57) & 255;
    int b = (X + 1 + Y * 57) & 255;
    int c = (X + (Y + 1) * 57) & 255;
    int d = (X + 1 + (Y + 1) * 57) & 255;

    float res = noiseLerp(v, noiseLerp(u, noiseGrad(a, x, y), noiseGrad(b, x - 1, y)), noiseLerp(u, noiseGrad(c, x, y - 1), noiseGrad(d, x - 1, y - 1)));

    return (res+ 1.0f) * 0.5f;
}

static inline float fbm(float x, float z, int octaves, float persistence, float lacunarity) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxAmplitude = 0.0f;

    for (int i = 0; i < octaves; i++) {
        total += perlin2d(x * frequency, z * frequency) * amplitude;
        maxAmplitude += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return total / maxAmplitude;
}

static inline float getTerrainHeight(float wx, float wz) {
    float biomeN = perlin2d(wx * scale * 0.03f, wz * scale * 0.03f);
    
    int biome;
    if (biomeN < 0.25f) {
        biome = 0; // flat/oceean
    } else if (biomeN < 0.50f) {
        biome = 1; // plaine 
    } else if (biomeN < 0.75f) {
        biome = 2; // Hillls (second to mountain)
    } else {
        biome = 3; // montain
    }

    float height = 0.0f;
    switch (biome) {
        case 0:
            height = fbm(wx * scale * 0.6f, wz * scale * 0.6f, 3, 0.5f, 2.0f) * 5.0f - 10.0f;
            break;
        case 1:
            height = fbm(wx * scale * 0.5f, wz * scale * 0.5f, 3, 0.5f, 2.0f) * 20.0f;
            break;
        case 2:
            height = fbm(wx * scale * 0.7f, wz * scale * 0.7f, 4, 0.5f, 2.0f) * 45.0f;
            break;
        case 3: 
            float m = fbm(wx * scale * 0.4f, wz * scale * 0.4f, 4, 0.6f, 2.2f);
            m = 1.0f - fabs(m * 2.0f - 1.0f);
            height = pow(m, 2.0f) * 120.0f + 10.0f;
            break;
    }

    return height;
}

void buildCoarseHeightGrid(int cx, int cz, float grid[noiseGridSize][noiseGridSize]) {
    for (int gz = 0; gz < noiseGridSize; gz++) {
        for (int gx = 0; gx < noiseGridSize; gx++) {
            float wx = (float)(cx * chunkSize + gx * noiseStep);
            float wz = (float)(cz * chunkSize + gz * noiseStep);

            grid[gx][gz] = getTerrainHeight(wx, wz);
        }
    }
}

static inline float sampleUpsampledHeight(float grid[noiseGridSize][noiseGridSize], int localX, int localZ) {
    int gx0 = localX / noiseStep;
    int gz0 = localZ / noiseStep;
    int gx1 = gx0 + 1;
    int gz1 = gz0 + 1;

    float fx = (float)(localX % noiseStep) / (float)noiseStep;
    float fz = (float)(localZ % noiseStep) / (float)noiseStep;
    float h00 = grid[gx0][gz0];
    float h10 = grid[gx1][gz0];
    float h01 = grid[gx0][gz1];
    float h11 = grid[gx1][gz1];
    float hx0 = h00 + (h10 - h00) * fx;
    float hx1 = h01 + (h11 - h01) * fx;

    return hx0 + (hx1 - hx0) * fz;
}

void exactFrustumPlanes(mat4 vp, plane planes[6]) {
    planes[0] = (plane){ vp[0][3]+vp[0][0], vp[1][3]+vp[1][0], vp[2][3]+vp[2][0], vp[3][3]+vp[3][0] }; // left
    planes[1] = (plane){ vp[0][3]-vp[0][0], vp[1][3]-vp[1][0], vp[2][3]-vp[2][0], vp[3][3]-vp[3][0] }; // right
    planes[2] = (plane){ vp[0][3]+vp[0][1], vp[1][3]+vp[1][1], vp[2][3]+vp[2][1], vp[3][3]+vp[3][1] }; // bottom
    planes[3] = (plane){ vp[0][3]-vp[0][1], vp[1][3]-vp[1][1], vp[2][3]-vp[2][1], vp[3][3]-vp[3][1] }; // top
    planes[4] = (plane){ vp[0][3]+vp[0][2], vp[1][3]+vp[1][2], vp[2][3]+vp[2][2], vp[3][3]+vp[3][2] }; // near
    planes[5] = (plane){ vp[0][3]-vp[0][2], vp[1][3]-vp[1][2], vp[2][3]-vp[2][2], vp[3][3]-vp[3][2] }; // far

    for (int i = 0; i < 6; i++) {
        float len = sqrtf(planes[i].a * planes[i].a + planes[i].b * planes[i].b + planes[i].c * planes[i].c);
        planes[i].a /= len;
        planes[i].b /= len;
        planes[i].c /= len;
        planes[i].d /= len;
    }
}

int cellSizeForTileDist(int distTiles) {
    if (distTiles <= 3) return 4;
    if (distTiles <= 6) return 8;
    return 16;
}

void buildLodSizeCpu(lodTileEntry* entry) {
    int cellsPerSide = lodTileSize / entry->tile.cellSize;
    int maxVerts = cellsPerSide * cellsPerSide * (2*3 + 8*3) * 7;
    int vc = 0;

    float* verts = malloc(maxVerts * sizeof(float));
    if (!verts) return;

    float tileOrginX = entry->tx * (float)lodTileSize;
    float tileOrginZ = entry->tz * (float)lodTileSize;
    float minHeight = 1e9f;
    float maxHeight = -1e9f;
    float cellHeights[cellsPerSide + 1][cellsPerSide + 1];
    float cellAverages[cellsPerSide][cellsPerSide];

    for (int cx = 0; cx <= cellsPerSide; cx++) {
        for (int cz = 0; cz <= cellsPerSide; cz++) {
            float wx = tileOrginX + cx * entry->tile.cellSize;
            float wz = tileOrginZ + cz * entry->tile.cellSize;

            cellHeights[cx][cz] = getTerrainHeight(wx, wz);
        }
    }

    for (int cx = 0; cx < cellsPerSide; cx++) {
        for (int cz = 0; cz < cellsPerSide; cz++) {
            float wx0 = tileOrginX + cx * entry->tile.cellSize;
            float wz0 = tileOrginZ + cz * entry->tile.cellSize;
            float wx1 = wx0 + entry->tile.cellSize;
            float wz1 = wz0 + entry->tile.cellSize;

            float h00 = cellHeights[cx][cz];
            float h10 = cellHeights[cx + 1][cz];
            float h11 = cellHeights[cx + 1][cz + 1];
            float h01 = cellHeights[cx][cz + 1];
            float h = (h00 + h10 + h11 + h01) * 0.25f;

            cellAverages[cx][cz] = h;

            if (h < minHeight) minHeight = h;
            if (h > maxHeight) maxHeight = h;
        }
    }

    if (maxHeight - minHeight < 1e-6f) {
        maxHeight = minHeight + 1.0f;
    }

    float floorYs = minHeight - 1.0f;

    for (int cx = 0; cx < cellsPerSide; cx++) {
        for (int cz = 0; cz < cellsPerSide; cz++) {
            float wx0 = tileOrginX + cx * entry->tile.cellSize;
            float wz0 = tileOrginZ + cz * entry->tile.cellSize;
            float wx1 = wx0 + entry->tile.cellSize;
            float wz1 = wz0 + entry->tile.cellSize;

            float h00 = cellHeights[cx][cz];
            float h10 = cellHeights[cx + 1][cz];
            float h11 = cellHeights[cx + 1][cz + 1];
            float h01 = cellHeights[cx][cz + 1];
            float h = cellAverages[cx][cz];

            float p00[3] = { wx0, h00, wz0 };
            float p10[3] = { wx1, h10, wz0 };
            float p11[3] = { wx1, h11, wz1 };
            float p01[3] = { wx0, h01, wz1 };

            float* topTri1[3] = { p00, p11, p10 };
            float* topTri2[3] = { p00, p01, p11 };

            for (int t = 0; t < 2; t++) {
                float** tri = (t == 0) ? topTri1 : topTri2;
                if (vc + 7 * 3 > maxVerts) continue;
                for (int i = 0; i < 3; i++) {
                    verts[vc++] = tri[i][0];
                    verts[vc++] = tri[i][1];
                    verts[vc++] = tri[i][2];
                    verts[vc++] = (tri[i][0] - tileOrginX) / (float)entry->tile.cellSize;
                    verts[vc++] = (tri[i][2] - tileOrginZ) / (float)entry->tile.cellSize;
                    verts[vc++] = 1.0f;
                    verts[vc++] = 3.0f;
                }
            }

            int dirs[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} };

            for (int d = 0; d < 4; d++) {
                int nx = cx + dirs[d][0];
                int nz = cz + dirs[d][1];

                bool isTileBorder = (nx < 0 || nx >= cellsPerSide || nz < 0 || nz >= cellsPerSide);

                float nh;
                if (isTileBorder) {
                    float nwx = tileOrginX + (nx + 0.5f) * entry->tile.cellSize;
                    float nwz = tileOrginZ + (nz + 0.5f) * entry->tile.cellSize;
                    nh = getTerrainHeight(nwx, nwz);
                } else {
                    nh = cellAverages[nx][nz];
                }

                if (fabs(nh - h) < 0.01f) continue;
                if (nh > h) continue;

                float floorY = isTileBorder ? floorYs : nh;

                float v0[3], v1[3], v2[3], v3[3];

                if (dirs[d][0] == 0 && dirs[d][1] == -1) { // north
                    v0[0]=wx0; v0[1]=h00;   v0[2]=wz0;
                    v1[0]=wx1; v1[1]=h10;   v1[2]=wz0;
                    v2[0]=wx1; v2[1]=floorY;  v2[2]=wz0;
                    v3[0]=wx0; v3[1]=floorY;  v3[2]=wz0;
                } else if (dirs[d][0] == 0 && dirs[d][1] == 1) { // south
                    v0[0]=wx1; v0[1]=h11;   v0[2]=wz1;
                    v1[0]=wx0; v1[1]=h01;   v1[2]=wz1;
                    v2[0]=wx0; v2[1]=floorY;  v2[2]=wz1;
                    v3[0]=wx1; v3[1]=floorY;  v3[2]=wz1;
                } else if (dirs[d][0] == -1 && dirs[d][1] == 0) { // west
                    v0[0]=wx0; v0[1]=h00;   v0[2]=wz0;
                    v1[0]=wx0; v1[1]=h01;   v1[2]=wz1;
                    v2[0]=wx0; v2[1]=floorY;  v2[2]=wz1;
                    v3[0]=wx0; v3[1]=floorY;  v3[2]=wz0;
                } else if (dirs[d][0] == 1 && dirs[d][1] == 0) { // east
                    v0[0]=wx1; v0[1]=h10;   v0[2]=wz0;
                    v1[0]=wx1; v1[1]=h11;   v1[2]=wz1;
                    v2[0]=wx1; v2[1]=floorY;  v2[2]=wz1;
                    v3[0]=wx1; v3[1]=floorY;  v3[2]=wz0;
                }

                int fid = 8;

                float* tris[2][3] = { {v0, v1, v2}, {v0, v2, v3} };
                if (vc + 7 * 6 > maxVerts) continue;
                for (int t = 0; t < 2; t++) {
                    for (int i = 0; i < 3; i++) {
                        verts[vc++] = tris[t][i][0];
                        verts[vc++] = tris[t][i][1];
                        verts[vc++] = tris[t][i][2];
                        float u = (tris[t][i][0] - tileOrginX) / (float)lodTileSize;
                        float v = (tris[t][i][1] - minHeight) / (maxHeight - minHeight + 1.0f);
                        verts[vc++] = u;
                        verts[vc++] = v;
                        verts[vc++] = (float)d;
                        verts[vc++] = (float)fid;
                    }
                }
            }
        }
    }

    EnterCriticalSection(&lodLock);

    entry->tile.cpuVerts = verts;
    entry->tile.cpuVertCount = vc;
    entry->tile.cpuReady = true;

    LeaveCriticalSection(&lodLock);
}

void uploadLoadTile2Gpu(lodTileEntry* entry) {
    EnterCriticalSection(&lodLock);
    if (!entry->tile.cpuReady || !entry->tile.cpuVerts) {
        LeaveCriticalSection(&lodLock);
        return;
    }

    float* verts = entry->tile.cpuVerts;

    int vertCount = entry->tile.cpuVertCount;

    entry->tile.cpuVerts = NULL;
    entry->tile.cpuReady = false;

    LeaveCriticalSection(&lodLock);

    glGenVertexArrays(1, &entry->tile.vao);
    glGenBuffers(1, &entry->tile.vbo);

    glBindVertexArray(entry->tile.vao);
    glBindBuffer(GL_ARRAY_BUFFER, entry->tile.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertCount * sizeof(float), verts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    free(verts);

    entry->tile.vertCount = vertCount / 7;
    entry->tile.built = true;
}

lodTileEntry* getOrCreateLodTile(int tx, int tz) {
    long long key = ((long long)(tx + 100000) << 32) | (unsigned int)(tz + 100000);

    lodTileEntry* entry = NULL;

    EnterCriticalSection(&lodLock);

    HASH_FIND(hh, loadedLodTiles, &key, sizeof(long long), entry);
    if (!entry) {
        entry = malloc(sizeof(lodTileEntry));
        if (!entry) { LeaveCriticalSection(&lodLock); return NULL;}

        entry->key = key;
        entry->tx = tx;
        entry->tz = tz;
        entry->tile.vao = 0;
        entry->tile.vbo = 0;
        entry->tile.vertCount = 0;
        entry->tile.built = false;
        entry->tile.cellSize = 0;
        entry->tile.cpuVerts = NULL;
        entry->tile.cpuVertCount = 0;
        entry->tile.cpuReady = false;
        entry->tile.isBeingBuilt = false;
        
        HASH_ADD(hh, loadedLodTiles, key, sizeof(long long), entry);
    }

    LeaveCriticalSection(&lodLock);

    return entry;
}

bool aabbFrustum(plane planes[6], float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
    for (int i = 0; i < 6; i++) {
        plane p = planes[i];

        float px = (p.a >= 0) ? maxX : minX;
        float py = (p.b >= 0) ? maxY : minY;
        float pz = (p.c >= 0) ? maxZ : minZ;

        if (p.a * px + p.b * py + p.c * pz + p.d < 0) return false;
    }
    return true;
}

void enqueueDirtyChunk(chunks* chunk, int cx, int cy, int cz) {
    if (chunk->isQueued) return;

    int next = (dirtyQueueTail + 1) % dirtyQueueSize;
    if (next == dirtyQueueHead) return;

    dirtyQueue[dirtyQueueTail].chunk = chunk;
    dirtyQueue[dirtyQueueTail].cx = cx;
    dirtyQueue[dirtyQueueTail].cy = cy;
    dirtyQueue[dirtyQueueTail].cz = cz;
    dirtyQueueTail = next;

    chunk->isQueued = true;

    COND_SIGNAL(&chunkWorkReady);
}

bool dequeueDirtyChunk(chunks** chunk, int* cx, int* cy, int* cz) {
    while (dirtyQueueHead != dirtyQueueTail) {
        dirtyQueueEntry* e = &dirtyQueue[dirtyQueueHead];
        dirtyQueueHead = (dirtyQueueHead + 1) % dirtyQueueSize;

        e->chunk->isQueued = false;

        if (!e->chunk->isDirty || e->chunk->isMeshReady || e->chunk->isMeshing || e->chunk->isBroken) continue;

        *chunk = e->chunk;
        *cx = e->cx;
        *cy = e->cy;
        *cz = e->cz;

        return true;
    }

    return false;
}

chunks* getOrCreateChunk(int cx, int cy, int cz) {
    uint64_t key = getChunkkey(cx, cy, cz);

    MUTEX_LOCK(&chunkLock);

    chunkEntry* entry = NULL;

    HASH_FIND(hh, loadedChunks, &key, sizeof(uint64_t), entry);

    if (entry) {
        MUTEX_UNLOCK(&chunkLock);
        return &entry->chunk;
    }

    entry = malloc(sizeof(chunkEntry));

    if (!entry) {
        MUTEX_UNLOCK(&chunkLock);
        return NULL;
    }

    entry->key = key;
    entry->cx = cx;
    entry->cy = cy;
    entry->cz = cz;
    entry->chunk.mesh = NULL;
    entry->chunk.isDirty = 1;
    entry->chunk.isGenerated = 0;
    entry->chunk.cpuVertices = NULL;
    entry->chunk.cpuIndices = NULL;
    entry->chunk.isMeshReady = false;
    entry->chunk.isMeshing = false;
    entry->chunk.genFailCount = 0;
    entry->chunk.isBroken = 0;
    entry->chunk.occlusionQuery = 0;
    entry->chunk.queryIssued = false;
    entry->chunk.wasVisible = true;
    entry->chunk.gpuUploaded = false;
    entry->chunk.meshVersion = 0;
    entry->chunk.cpuMeshVersion = 0;
    entry->chunk.isQueued = false;

    enqueueDirtyChunk(&entry->chunk, cx, cy, cz);

    memset(entry->chunk.voxels, 0, sizeof(entry->chunk.voxels));

    HASH_ADD(hh, loadedChunks, key, sizeof(uint64_t), entry);;

    MUTEX_UNLOCK(&chunkLock);

    return &entry->chunk;
}

chunks* getChunkSilentUnlocked(int cx, int cy, int cz) {
    uint64_t key = getChunkkey(cx, cy, cz);

    chunkEntry* entry = NULL;

    HASH_FIND(hh, loadedChunks, &key, sizeof(uint64_t), entry);

    return entry ? &entry->chunk : NULL;
}

bool isSolidAtWorld(int x, int y, int z) {
    int cx = (int)floorf((float)x / chunkSize);
    int cy = (int)floorf((float)y / chunkSize);
    int cz = (int)floorf((float)z / chunkSize);

    int localX = x - cx * chunkSize;
    int localY = y - cy * chunkSize;
    int localZ = z - cz * chunkSize;

    if (localX < 0) localX += chunkSize;
    if (localY < 0) localY += chunkSize;
    if (localZ < 0) localZ += chunkSize;

    MUTEX_LOCK(&chunkLock);

    chunks* chunk = getChunkSilentUnlocked(cx, cy, cz);
    bool solid = false;

    if (chunk && chunk->isGenerated) {
        solid = (chunk->voxels[localY][localZ] & (1U << localX)) != 0;
    }

    MUTEX_UNLOCK(&chunkLock);

    return solid;
}

bool raycastVoxel(float ox, float oy, float oz, float dx, float dy, float dz, float maxDist, int* hitX, int* hitY, int* hitZ, int* placeX, int* placeY, int* placeZ) {
    int x = (int)floorf(ox);
    int y = (int)floorf(oy);
    int z = (int)floorf(oz);

    float stepX = (dx > 0) ? 1.0f : -1.0f;
    float stepY = (dy > 0) ? 1.0f : -1.0f;
    float stepZ = (dz > 0) ? 1.0f : -1.0f;

    float tDeltaX = (dx != 0.0f) ? fabsf(1.0f / dx) : 1e30f;
    float tDeltaY = (dy != 0.0f) ? fabsf(1.0f / dy) : 1e30f;
    float tDeltaZ = (dz != 0.0f) ? fabsf(1.0f / dz) : 1e30f;

    float tMaxX = (dx != 0.0f) ? fabsf((floorf(ox) + (dx > 0 ? 1.0f : 0.0f) - ox) / dx) : 1e30f;
    float tMaxY = (dy != 0.0f) ? fabsf((floorf(oy) + (dy > 0 ? 1.0f : 0.0f) - oy) / dy) : 1e30f;
    float tMaxZ = (dz != 0.0f) ? fabsf((floorf(oz) + (dz > 0 ? 1.0f : 0.0f) - oz) / dz) : 1e30f;

    int prevX = x;
    int prevY = y;
    int prevZ = z;
    float dist = 0.0f;

    MUTEX_LOCK(&chunkLock);

    while (dist < maxDist) {
        int cx = (int)floorf((float)x / chunkSize);
        int cy = (int)floorf((float)y / chunkSize);
        int cz = (int)floorf((float)z / chunkSize);
        int lx = x - cx * chunkSize; if (lx < 0) lx += chunkSize;
        int ly = y - cy * chunkSize; if (ly < 0) ly += chunkSize;
        int lz = z - cz * chunkSize; if (lz < 0) lz += chunkSize;

        chunks* chunk = getChunkSilentUnlocked(cx, cy, cz);

        if (chunk && chunk->isGenerated && (chunk->voxels[ly][lz] & (1U << lx)) != 0) {
            
            MUTEX_UNLOCK(&chunkLock);

            *hitX = x;
            *hitY = y;
            *hitZ = z;
            *placeX = prevX;
            *placeY = prevY;
            *placeZ = prevZ;
            return true;
        }

        prevX = x;
        prevY = y;
        prevZ = z;

        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += (int)stepX;
            dist = tMaxX;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += (int)stepY;
            dist = tMaxY;
            tMaxY += tDeltaY;
        } else {
            z += (int)stepZ;
            dist = tMaxZ;
            tMaxZ += tDeltaZ;
        }
    }

    MUTEX_UNLOCK(&chunkLock);

    return false;
}

bool checkPlayerCollision(float px, float feetY, float pz) {
    float halfWidth = steveWidth * 0.5f;

    int minX = (int)floorf(px - halfWidth);
    int maxX = (int)floorf(px + halfWidth);
    int minY = (int)floorf(feetY);
    int maxY = (int)floorf(feetY + steveHeight);
    int minZ = (int)floorf(pz - halfWidth);
    int maxZ = (int)floorf(pz + halfWidth);

    MUTEX_LOCK(&chunkLock);

    for (int x = minX; x <= maxX; x++) {
        for (int y = minY; y <= maxY; y++) {
            for (int z = minZ; z <= maxZ; z++) {
                int cx = (int)floorf((float)x / chunkSize);
                int cy = (int)floorf((float)y / chunkSize);
                int cz = (int)floorf((float)z / chunkSize);
                int lx = x - cx * chunkSize; if (lx < 0) lx += chunkSize;
                int ly = y - cy * chunkSize; if (ly < 0) ly += chunkSize;
                int lz = z - cz * chunkSize; if (lz < 0) lz += chunkSize;

                chunks* chunk = getChunkSilentUnlocked(cx, cy, cz);

                if (chunk && chunk->isGenerated) {
                    if ((chunk->voxels[ly][lz] & (1U << lx)) != 0) {
                        MUTEX_UNLOCK(&chunkLock);
                        return true;
                    }
                }
            }
        }
    }

    MUTEX_UNLOCK(&chunkLock);

    return false;
}

void tryMove(float dx, float dy, float dz) {
    float feetY = camY - eyeHeight;

    if (dx != 0.0f) {
        float newX = camX + dx;
        if (!checkPlayerCollision(newX, feetY, camZ)) {

            camX = newX;

        }
    }

    if (dz != 0.0f) {
        float newZ = camZ + dz;
        if (!checkPlayerCollision(camX, feetY, newZ)) {

            camZ = newZ;

        }
    }

    if (dy != 0.0f) {
        float newFeetY = feetY + dy;
        if (!checkPlayerCollision(camX, newFeetY, camZ)) {

            camY += dy;

        } else {
            if (dy < 0.0f) isGrounded = true;
            velY = 0.0f;
        }
    }
}

void unloadDistantChunks(int camChunkX, int camChunkY, int camChunkZ, int keepRadius) {
    MUTEX_LOCK(&chunkLock);
    chunkEntry *entry, *tmp;

    int deletedThisCall = 0;

    const int maxDeletesPerFrame = 8;

    HASH_ITER(hh, loadedChunks, entry, tmp) {
        if (deletedThisCall >= maxDeletesPerFrame) break;

        int dx = entry->cx - camChunkX;
        int dy = entry->cy - camChunkY;
        int dz = entry->cz - camChunkZ;

        if (abs(dx) > keepRadius || abs(dy) > keepRadius || abs(dz) > keepRadius) {
            if (entry->chunk.isMeshing) continue;
            if (entry->chunk.isMeshReady) continue;
            if (entry->chunk.cpuVertices != NULL) continue;

            if (entry->chunk.mesh != NULL) {
                free(entry->chunk.mesh);
                entry->chunk.mesh = NULL;
            }

            if (entry->chunk.cpuVertices) free(entry->chunk.cpuVertices);
            if (entry->chunk.cpuIndices) free(entry->chunk.cpuIndices);

            HASH_DEL(loadedChunks, entry);
            free(entry);

            deletedThisCall++;
        }
    }

    MUTEX_UNLOCK(&chunkLock);
}

void unloadDistantLodChunks(int camTileX, int camTileZ) {
    lodTileEntry *lt, *lttmp;
    HASH_ITER(hh, loadedLodTiles, lt, lttmp) {
        int dTx = abs(lt->tx - camTileX);
        int dTz = abs(lt->tz - camTileZ);

        if (dTx > lodRadius + 2 || dTz > lodRadius + 2) {
            
            EnterCriticalSection(&lodLock);

            for (int i = lodBuildHead; i != lodBuildTail; i = (i + 1) % lodBuildQueueSize) {
                if (lodBuildQueue[i] == lt) {
                    lodBuildQueue[i] = NULL;
                }
            }

            bool safeToFree = !lt->tile.isBeingBuilt && !lt->tile.cpuReady;

            LeaveCriticalSection(&lodLock);

            if (!safeToFree) continue;

            if (lt->tile.built) {
                glDeleteBuffers(1, &lt->tile.vbo);
                glDeleteVertexArrays(1, &lt->tile.vao);
            }

            if (lt->tile.cpuVerts) {
                free(lt->tile.cpuVerts);

                lt->tile.cpuVerts = NULL;
            }

            HASH_DEL(loadedLodTiles, lt);
            free(lt);
        }
    }
}
#if defined(_WIN32) || defined(_WIN64)
DWORD WINAPI backgroundChunkWorker(LPVOID lpParam) {
#else
void* backgroundChunkWorker(void* lpParam) {
#endif
    while (ATOMIC_CHECK(isRunning)) {
        MUTEX_LOCK(&chunkLock);

        chunks* targetChunk = NULL;

        int tCx = 0;
        int tCy = 0;
        int tCz = 0;

        if (!dequeueDirtyChunk(&targetChunk, &tCx, &tCy, &tCz)) {
            chunkEntry* entry, *tmp;
            chunkEntry* bestEntry = NULL;

            float bestDist = 1e30f;

            bool bestNeedsGen = false;

            HASH_ITER(hh, loadedChunks, entry, tmp) {
                if (!entry->chunk.isDirty || entry->chunk.isMeshReady || entry->chunk.isMeshing || entry->chunk.isBroken || (entry->chunk.cpuVertices != NULL && !entry->chunk.gpuUploaded)) {
                    continue;
                }

                float dx = (entry->cx * chunkSize + chunkSize / 2.0f) - camX;
                float dy = (entry->cy * chunkSize + chunkSize / 2.0f) - camY;
                float dz = (entry->cz * chunkSize + chunkSize / 2.0f) - camZ;
                float dist = dx * dx + dy * dy + dz * dz;

                bool needsGen = !entry->chunk.isGenerated;

                if (needsGen && !bestNeedsGen) {
                    bestEntry = entry;
                    bestDist = dist;
                    bestNeedsGen = true;
                } else if (needsGen == bestNeedsGen && dist < bestDist) {
                    bestEntry = entry;
                    bestDist = dist;
                }
            }

            if (bestEntry) {
                targetChunk = &bestEntry->chunk;
                tCx = bestEntry->cx;
                tCy = bestEntry->cy;
                tCz = bestEntry->cz;
            }
        }

        if (targetChunk) {
            targetChunk->isMeshing = true;
        }

        MUTEX_UNLOCK(&chunkLock);

        EnterCriticalSection(&lodLock);

        if (lodBuildHead != lodBuildTail) {
            lodTileEntry* lodTile = lodBuildQueue[lodBuildHead];
            lodBuildHead = (lodBuildHead + 1) % lodBuildQueueSize;

            if (lodTile && !lodTile->tile.cpuReady && !lodTile->tile.built) {
                lodTile->tile.isBeingBuilt = true;
            } else {
                lodTile = NULL;
            }

            LeaveCriticalSection(&lodLock);

            if (lodTile) {
                buildLodSizeCpu(lodTile);

                EnterCriticalSection(&lodLock);

                lodTile->tile.isBeingBuilt = false;

                LeaveCriticalSection(&lodLock);
            }
        } else {
            LeaveCriticalSection(&lodLock);
        }

        if (targetChunk == NULL) {
            MUTEX_LOCK(&chunkLock);

            #if defined(_WIN32) || defined(_WIN64)
                SleepConditionVariableCS(&chunkWorkReady, &chunkLock, 5);
            #else
                struct timespec outtime;

                clock_gettime(CLOCK_REALTIME, &outtime);

                outtime.tv_sec += 5;
                
                pthread_cond_timedwait(&chunkWorkReady, &chunkLock, &outtime);
            #endif

            MUTEX_UNLOCK(&chunkLock);
            continue;
        }

        uint32_t versionAtStart = targetChunk->meshVersion;

        if (!ATOMIC_CHECK(isRunning)) {
            MUTEX_LOCK(&chunkLock);
            targetChunk->isMeshing = false;
            MUTEX_UNLOCK(&chunkLock);

            break;
        }

        if (!targetChunk->isGenerated) {
            uint32_t tempVoxels[chunkSize][chunkSize] = {{0}};

            int chunkMinY = tCy * chunkSize;
            int chunkMaxY = chunkMinY + chunkSize - 1;

            chunkEntry *entry;

            if (chunkMinY > (int)(maxTerrainHeight + 12.0f)) {
                // a i r
            } else if (chunkMaxY < -22) {
                memset(tempVoxels, 0xFF, sizeof(tempVoxels));
            } else {
                float grid[noiseGridSize][noiseGridSize];

                buildCoarseHeightGrid(tCx, tCz, grid);

                for (int z = 0; z < chunkSize; z++) {
                    for (int x = 0; x < chunkSize; x++) {
                        int terrainHeight = (int)sampleUpsampledHeight(grid, x, z);

                        for (int y = 0; y < chunkSize; y++) {
                            int worldY = chunkMinY + y;

                            if (worldY <= terrainHeight) {
                                tempVoxels[y][z] |= (1U << x);
                            }
                        }
                    }
                }
            }

            MUTEX_LOCK(&chunkLock);

            memcpy(targetChunk->voxels, tempVoxels, sizeof(tempVoxels));

            targetChunk->isGenerated = 1;

            MUTEX_UNLOCK(&chunkLock);
        }

        MUTEX_LOCK(&chunkLock);

        bool neighboursReady = true;

        int nd[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

        for (int n = 0; n < 6; n++) {
            chunks* nb = getChunkSilentUnlocked(tCx + nd[n][0], tCy + nd[n][1], tCz + nd[n][2]);

            if (!nb || !nb->isGenerated) {
                neighboursReady = false;
                break;
            }
        }

        if (!neighboursReady) {
            targetChunk->isMeshing = false;

            SleepConditionVariableCS(&chunkWorkReady, &chunkLock, 10);

            MUTEX_UNLOCK(&chunkLock);

            continue;
        }

        MUTEX_UNLOCK(&chunkLock);

        int dim = chunkSize + 2;

        uint8_t* blocks = calloc(dim * dim * dim, sizeof(uint8_t));
        if (!blocks) {
            MUTEX_LOCK(&chunkLock);
            targetChunk->isMeshing = false;
            MUTEX_UNLOCK(&chunkLock);
            continue;
        }

        uint32_t *localVoxels = malloc(3 * 3 * 3 * chunkSize * chunkSize * sizeof(uint32_t));
        if (!localVoxels) {
            free(blocks);

            MUTEX_LOCK(&chunkLock);

            targetChunk->isMeshing = false;

            MUTEX_UNLOCK(&chunkLock);

            continue;
        }

        bool hasTarget[3][3][3] = {false};

        MUTEX_LOCK(&chunkLock);

        for (int dx = 0; dx < 3; dx++) {
            for (int dy = 0; dy < 3; dy++) {
                for (int dz = 0; dz < 3; dz++) {
                    chunks* neighbourChunk = getChunkSilentUnlocked(tCx + (dx - 1), tCy + (dy - 1), tCz + (dz - 1));

                    if (neighbourChunk && neighbourChunk->isGenerated) {
                        hasTarget[dx][dy][dz] = true;

                        memcpy(&localVoxels[voxelIdx(dx, dy, dz, 0, 0)], neighbourChunk->voxels, chunkSize * chunkSize * sizeof(uint32_t));
                    } else {
                        hasTarget[dx][dy][dz] = false;
                        memset(&localVoxels[voxelIdx(dx, dy, dz, 0, 0)], 0, chunkSize * chunkSize * sizeof(uint32_t));
                    }
                }
            }
        }

        MUTEX_UNLOCK(&chunkLock);

        for (int y = 0; y < chunkSize; y++) {
            for (int z = 0; z < chunkSize; z++) {
                uint32_t row = localVoxels[voxelIdx(1, 1, 1, y, z)];

                for (int x = 0; x < chunkSize; x++) {
                    blocks[(x + 1) * dim * dim + (y + 1) * dim + (z + 1)] = (row >> x) & 1;
                }
            }
        }

        for (int lx = -1; lx <= chunkSize; lx++) {
            for (int ly = -1; ly <= chunkSize; ly++) {
                for (int lz = -1; lz <= chunkSize; lz++) {
                    int targetDx = (lx < 0) ? -1 : (lx >= chunkSize ? 1 : 0);
                    int targetDy = (ly < 0) ? -1 : (ly >= chunkSize ? 1 : 0);
                    int targetDz = (lz < 0) ? -1 : (lz >= chunkSize ? 1 : 0);

                    if (hasTarget[targetDx+1][targetDy+1][targetDz+1]) {
                        int tLx = (lx + chunkSize) % chunkSize;
                        int tLy = (ly + chunkSize) % chunkSize;
                        int tLz = (lz + chunkSize) % chunkSize;

                        blocks[(lx+1)*dim*dim + (ly+1)*dim + (lz+1)] = (localVoxels[voxelIdx(targetDx+1, targetDy+1, targetDz+1, tLy, tLz)] >> tLx) & 1;
                    }
                }
            }
        }

        free(localVoxels);

        // int maxVertices = chunkSize * chunkSize * chunkSize * 6 * 4 * 6;
        // int maxIndices = chunkSize * chunkSize * chunkSize * 6 * 6;

        int maxVertices = 64 * 1024;
        int maxIndices = 32 * 1024;
        int maxVerticesCap = maxVertices;
        int maxIndicesCap = maxIndices;

        const int hardVertexCap = 4 * 1024 * 1024;
        const int hardIndexCap = 2 * 1024 * 1024;

        float* tempVertices = malloc(maxVertices * sizeof(float));
        unsigned int* tempIndices = malloc(maxIndices * sizeof(unsigned int));

        if (!tempVertices || !tempIndices) {
            free(tempVertices);
            free(tempIndices);
            free(blocks);
            targetChunk->isMeshing = false;
            continue;
        }

        int vertCount = 0;
        int indCount = 0;
        int meshGenFailed = 0;

        int colors[6] = {
            0, // left side
            0, // right side
            0, // down side
            1, // up side
            0, // forward side
            0  // back side
        };

        bool* mask = calloc(chunkSize * chunkSize, sizeof(bool));
        if (!mask) {
            meshGenFailed = 1;

            printf("ERROR: maskalloc (mask allocation) has succesfully failed gracefully!\n");
            fflush(stdout);
        }

        for (int d = 0; d < 3; d++) {
            if (!ATOMIC_CHECK(isRunning)) { meshGenFailed = 2; break; }

            int u = (d + 1) % 3;
            int v = (d + 2) % 3;
            int x[3] = {0, 0, 0};
            int q[3] = {0, 0, 0};

            q[d] = 1;

            for (x[d] = -1; x[d] < chunkSize && !meshGenFailed; ++x[d]) {
                memset(mask, 0, chunkSize * chunkSize * sizeof(bool));

                for (x[v] = 0; x[v] < chunkSize; ++x[v]) {
                    for (x[u] = 0; x[u] < chunkSize; ++x[u]) {
                        if (!ATOMIC_CHECK(isRunning)) {
                            meshGenFailed = 2;

                            break;
                        }

                        int blkCurX = x[0] + 1;
                        int blkCurY = x[1] + 1;
                        int blkCurZ = x[2] + 1;

                        bool curBlock = (x[d] >= 0) ? blocks[blkCurX * dim * dim + blkCurY * dim + blkCurZ] : false;

                        int nextX = blkCurX + q[0];
                        int nextY = blkCurY + q[1];
                        int nextZ = blkCurZ + q[2];
                        bool neighbourBlock = blocks[nextX * dim * dim + nextY * dim + nextZ];

                        mask[x[u] * chunkSize + x[v]] = (curBlock != neighbourBlock);
                    }
                }

                for (int j = 0; j < chunkSize; j++) {
                    for (int i = 0; i < chunkSize; i++) {
                        if (mask[i * chunkSize + j]) {
                            int width = 1;
                            while (i + width < chunkSize && mask[(i + width) * chunkSize + j]) {
                                width++;
                            }

                            int height = 1;
                            bool done = false;
                            while (j + height < chunkSize) {
                                for (int k = 0; k < width; ++k) {
                                    if (!mask[(i + k) * chunkSize + (j + height)]) {
                                        done = true;
                                        break;
                                    }
                                }

                                if (done) break;
                                height++;
                            }

                            for (int l = 0; l < height; ++l) {
                                for (int k = 0; k < width; ++k) {
                                    int idx = (i + k) * dim + (j + l);
                                    if (idx < dim * dim) {
                                        mask[(i + k) * chunkSize + (j + l)] = false;
                                    }
                                }
                            }

                            x[u] = i;
                            x[v] = j;

                            int blkCurX = x[0] + 1;
                            int blkCurY = x[1] + 1;
                            int blkCurZ = x[2] + 1;

                            bool backFace = (x[d] >= 0) && blocks[blkCurX * dim * dim + blkCurY * dim + blkCurZ];

                            float baseCoords[3] = {
                                (float)(tCx * chunkSize + x[0]),
                                (float)(tCy * chunkSize + x[1]),
                                (float)(tCz * chunkSize + x[2])
                            };

                            baseCoords[d] += 1.0f;

                            float px = baseCoords[0];
                            float py = baseCoords[1];
                            float pz = baseCoords[2];

                            float du[3] = {0, 0, 0};
                            float dv[3] = {0, 0, 0};

                            du[u] = (float)width;
                            dv[v] = (float)height;

                            int faceIdx = d * 2 + (backFace ? 1 : 0);

                            float v0[3] = {px, py, pz};
                            float v1[3] = {v0[0] + du[0], v0[1] + du[1], v0[2] + du[2]};
                            float v2[3] = {v0[0] + du[0] + dv[0], v0[1] + du[1] + dv[1], v0[2] + du[2] + dv[2]};
                            float v3[3] = {v0[0] + dv[0], v0[1] + dv[1], v0[2] + dv[2]};

                            float* corners[4] = {v0, v1, v2, v3};

                            if (!backFace) {
                                corners[0] = v0;
                                corners[1] = v3;
                                corners[2] = v2;
                                corners[3] = v1;
                            }

                            float uv0[2] = {0.0f, 0.0f};
                            float uv1[2] = {(float)width, 0.0f};
                            float uv2[2] = {(float)width, (float)height};
                            float uv3[2] = {0.0f, (float)height};

                            float* uvs[4] = {uv0, uv1, uv2, uv3};

                            if (!backFace) {
                                uvs[0] = uv0;
                                uvs[1] = uv3;
                                uvs[2] = uv2;
                                uvs[3] = uv1;
                            }

                            if (vertCount + 28 > maxVerticesCap) {
                                if (maxVerticesCap >= hardVertexCap) { meshGenFailed = 1; printf("ERROR: uh oh! the cap has exeeded! vertCount: %d | maxVertCount: %d\n", vertCount, maxVerticesCap); fflush(stdout); break; }
                                maxVerticesCap *= 2;
                                if (maxVerticesCap > hardVertexCap) maxVerticesCap = hardVertexCap;
                                float* newVerts = realloc(tempVertices, maxVerticesCap * sizeof(float));
                                if (!newVerts) {
                                    meshGenFailed = 1;
                                    break;
                                }
                                tempVertices = newVerts;
                            }

                            if (indCount + 6 > maxIndicesCap) {
                                if (maxIndicesCap >= hardIndexCap) { meshGenFailed = 1; break; }
                                maxIndicesCap *= 2;
                                if (maxIndicesCap > hardIndexCap) maxIndicesCap = hardIndexCap;
                                unsigned int* newInds = realloc(tempIndices, maxIndicesCap * sizeof(unsigned int));
                                if (!newInds) {
                                    meshGenFailed = 1;
                                    break;
                                }
                                tempIndices = newInds;
                            }

                            for (int c = 0; c < 4; c++) {
                                tempVertices[vertCount++] = corners[c][0];
                                tempVertices[vertCount++] = corners[c][1];
                                tempVertices[vertCount++] = corners[c][2];
                                tempVertices[vertCount++] = uvs[c][0];
                                tempVertices[vertCount++] = uvs[c][1];
                                tempVertices[vertCount++] = (float)colors[faceIdx];
                                tempVertices[vertCount++] = (float)faceIdx;
                            }      

                            unsigned int base = vertCount / 7    - 4;
                            tempIndices[indCount++] = base;
                            tempIndices[indCount++] = base + 1;
                            tempIndices[indCount++] = base + 2;
                            tempIndices[indCount++] = base + 2;
                            tempIndices[indCount++] = base + 3;
                            tempIndices[indCount++] = base;

                            i += (width - 1);
                        }
                    }
                }
            }
        }

        free(mask);
        free(blocks);

        MUTEX_LOCK(&chunkLock);

        if (meshGenFailed == 2) {
            free(tempVertices);
            free(tempIndices);
        } else if (meshGenFailed) {
            free(tempVertices);
            free(tempIndices);
            targetChunk->genFailCount++;

            if (targetChunk->genFailCount >= 5) {
                targetChunk->isBroken = 1;
                targetChunk->isDirty = 0;
            }

        } else {
            if (vertCount > 0 && vertCount < maxVerticesCap) {
                float* shrunkVerts = realloc(tempVertices, vertCount * sizeof(float));
                if (shrunkVerts) tempVertices = shrunkVerts;
            }

            if (indCount > 0 && indCount < maxIndicesCap) {
                unsigned int* shrunkInds = realloc(tempIndices, indCount * sizeof(unsigned int));
                if (shrunkInds) tempIndices = shrunkInds;
            }

            if (versionAtStart != targetChunk->meshVersion) {
                free(tempVertices);
                free(tempIndices);

                targetChunk->isMeshing = false;

                MUTEX_UNLOCK(&chunkLock);

                continue;
            }

            if (targetChunk->cpuVertices) { free(targetChunk->cpuVertices); targetChunk->cpuVertices = NULL; }
            if (targetChunk->cpuIndices) { free(targetChunk->cpuIndices); targetChunk->cpuIndices = NULL; }
        
            targetChunk->queryIssued = false;
            targetChunk->cpuVertices = tempVertices;
            targetChunk->cpuIndices = tempIndices;
            targetChunk->gpuUploaded = false;
            targetChunk->cpuVertCount = vertCount;
            targetChunk->cpuIndCount = indCount;
            targetChunk->isMeshReady = true;
            targetChunk->genFailCount = 0;
            targetChunk->cpuMeshVersion = targetChunk->meshVersion;
        }

        targetChunk->isMeshing = false;
        MUTEX_UNLOCK(&chunkLock);

        SwitchToThread();
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

unsigned int loadTextureArray(const char** paths, int count, int texSize) {
    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);

    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, texSize, texSize, count, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    for (int i = 0; i < count; i++) {
        int w, h, ch;

        unsigned char* data = stbi_load(paths[i], &w, &h, &ch, 4);
        if (!data) {
            printf("Failed to load texture: %s\n", paths[i]);
            continue;
        }

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, data);

        stbi_image_free(data);
    }

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

    return tex;
}

void framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    cfov -= (float)yoffset;

    if (cfov < 1.0f)  cfov = 1.0f;
    if (cfov > 90.0f) cfov = 90.0f;
}

void keyCallback(GLFWwindow* window, int key, int scancodde, int action, int mods) {
    if (key == GLFW_KEY_F11 && action == GLFW_PRESS) {
        isFullscreen = !isFullscreen;

        if (isFullscreen) {
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            
            int x, y, width, height;

            glfwGetWindowPos(window, &pwinX, &pwinY);
            glfwGetWindowSize(window, &pwinWidth, &pwinHeight);
            glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowPos(window, x, y);
            glfwSetWindowSize(window, width, height);
        } else {
            glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowPos(window, pwinX, pwinY);
            glfwSetWindowSize(window, pwinWidth, pwinHeight);
        }
    } 
}

int memUsage() {
#if defined(_WIN32) || defined(_WIN64)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return (int)(pmc.WorkingSetSize / (1024 * 1024));
    }
    return 0;
#else
    FILE* file = fopen("/proc/self/statm", "r");

    if (!file) return 0;

    long totalPages = 0;
    long residentPages = 0;

    if (fscanf(file, "%ld %ld", &totalPages, &residentPages) != 2) {
        fclose(file);
        return 0;
    }

    fclose(file);

    long pageSize = sysconf(_SC_PAGESIZE);

    if (pageSize <= 0) return 0;

    return (int)((residentPages * pageSize) / (1024 * 1024));
#endif
}

void updFpsCounter(GLFWwindow* window, double time, size_t ramSize, float camX, float camY, float camZ, int commandCount, int triangles) {
    static double prevT = 0.0f;
    static int fc = 0;

    double ct = time;
    double et = ct - prevT;
    double mspf;

    if(et >= 0.5) {
        double fps = (double)fc / et;
        if (fps > 0.0) {
            mspf = 1000.0 / fps;
        } else {
            mspf = 0.0f;
        }
        
        char ts[256];

        snprintf(ts, sizeof(ts), "%s | FPS: %.1f | Frame Time: %.2fms | Ram Usage: %zuMB | cam x: %.2f | cam y: %.2f | cam z: %.2f | draw Calls: %d | triangles: %d", stringed, fps, mspf, ramSize, camX, camY, camZ, commandCount, triangles);

        glfwSetWindowTitle(window, ts);
        prevT = ct;
        fc = 0;
    }
    fc++;
}

void drawWorldMesh(int commandCount) {
    if (commandCount == 0) return;

    glBindVertexArray(masterVao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, masterIndirectBuffer);
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (void*)0, commandCount, 0);
    glBindVertexArray(0);
}

void enqueueDirtyChunkFront(chunks* chunk, int cx, int cy, int cz) {
    if (chunk->isQueued) return;

    int prev = (dirtyQueueHead - 1 + dirtyQueueSize) % dirtyQueueSize;

    if (prev == dirtyQueueTail) return;

    dirtyQueueHead = prev;

    dirtyQueue[dirtyQueueHead].chunk = chunk;
    dirtyQueue[dirtyQueueHead].cx = cx;
    dirtyQueue[dirtyQueueHead].cy = cy;
    dirtyQueue[dirtyQueueHead].cz = cz;

    chunk->isQueued = true;

    COND_SIGNAL(&chunkWorkReady);
}

void toggleVoxel(int x, int y, int z) {
    int cx = (int)floorf((float)x / chunkSize);
    int cy = (int)floorf((float)y / chunkSize);
    int cz = (int)floorf((float)z / chunkSize);

    int localX = x - cx * chunkSize;
    int localY = y - cy * chunkSize;
    int localZ = z - cz * chunkSize;

    if (localX < 0) localX += chunkSize;
    if (localY < 0) localY += chunkSize;
    if (localZ < 0) localZ += chunkSize;

    chunks* chunk = getOrCreateChunk(cx, cy, cz);
    if (!chunk) return;

    MUTEX_LOCK(&chunkLock);

    chunk->voxels[localY][localZ] ^= (1U << localX);
    chunk->isDirty = 1;
    chunk->meshVersion++;

    enqueueDirtyChunk(chunk, cx, cy, cz);

    if (localX == 0) {
        chunks* n = getChunkSilentUnlocked(cx - 1, cy, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx - 1, cy, cz); }
    }

    if (localX == chunkSize - 1) {
        chunks* n = getChunkSilentUnlocked(cx + 1, cy, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx + 1, cy, cz); }
    }

    if (localY == 0) {
        chunks* n = getChunkSilentUnlocked(cx, cy - 1, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy - 1, cz); }
    }

    if (localY == chunkSize - 1) {
        chunks* n = getChunkSilentUnlocked(cx, cy + 1, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy + 1, cz); }
    }

    if (localZ == 0) {
        chunks* n = getChunkSilentUnlocked(cx, cy, cz - 1);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy, cz - 1); }
    }

    if (localZ == chunkSize - 1) {
        chunks* n = getChunkSilentUnlocked(cx , cy, cz + 1);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy, cz + 1); }
    }

    MUTEX_UNLOCK(&chunkLock);
}

void processInput(GLFWwindow* window, float deltaTime) {
    if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }

    bool getF1 = (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS);

    if (getF1 && (!f1WasPressed)) {
        goWireframe = (goWireframe == 0) ? 1 : 0;
    }

    f1WasPressed = getF1;
    keyWasTouched = false;

    if (goWireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    static bool rightClickWasDown = false;
    static bool leftClickWasDown = false;
    static bool ctrlShiftToggleActive = false;
    static bool ctrlShiftWasDown = false;

    bool ctrlShiftPressed = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) && (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS); 

    bool middleHeld = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS) || ctrlShiftPressed;

    if (ctrlShiftPressed && !ctrlShiftWasDown) {
        ctrlShiftToggleActive = !ctrlShiftToggleActive;
    }
    ctrlShiftWasDown = ctrlShiftPressed;

    bool shouldRotateCam = middleHeld || ctrlShiftToggleActive;

    if (shouldRotateCam != isCameraRotateble) {
        isCameraRotateble = shouldRotateCam;
        if (isCameraRotateble) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            
            double freshX, freshY;
            glfwGetCursorPos(window, &freshX, &freshY);
            lastMouseX = (float)freshX;
            lastMouseY = (float)freshY;
            firstMouseInput = false;
            keyWasTouched = true;
        } else {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }

    float tfov = bfov;

    if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS) {
        tfov = 10.0f;
        sensitivity = 0.015;
    } else {
        tfov = bfov;
        sensitivity = baseSensitivity;
    }

    cfov += (tfov - cfov) * 10.0f * 0.016;
    
    if (cfov < 1.0f) cfov = 1.0f;
    if (cfov > 90.0f) cfov = 90.0f;

    int unstuckTries = 0;
    while (checkPlayerCollision(camX, camY - eyeHeight, camZ) && unstuckTries < 200.0f) {
        camY += 1.0f;
        unstuckTries++; 
    }

    bool currentPeriodPress = (glfwGetKey(window, GLFW_KEY_PERIOD) == GLFW_PRESS);
    bool leftClick = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    bool rightClick = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

    if ((currentPeriodPress && !periodWasPressed) || (leftClick && !leftClickWasDown) || (rightClick && !rightClickWasDown)) {
        float radYaw = glm_rad(camYaw);
        float radPitch = glm_rad(camPitch);

        float dirX = cosf(radPitch) * cosf(radYaw);
        float dirY = sinf(radPitch);
        float dirZ = cosf(radPitch) * sinf(radYaw);

        int hitX, hitY, hitZ, placeX, placeY, placeZ;
        if (raycastVoxel(camX, camY, camZ, dirX, dirY, dirZ, 6.0f, &hitX, &hitY, &hitZ, &placeX, &placeY, &placeZ)) {
            if (leftClick && !leftClickWasDown) {
                toggleVoxel(hitX, hitY, hitZ);
            } else if (rightClick && !rightClickWasDown) {
                toggleVoxel(placeX, placeY, placeZ);
            } else if (currentPeriodPress && !periodWasPressed) {
                toggleVoxel(hitX, hitY, hitZ);
            }
        }
    }

    {
        float radYaw = glm_rad(camYaw);
        float radPitch = glm_rad(camPitch);
        float hdX = cosf(radPitch) * cosf(radYaw);
        float hdY = sinf(radPitch);
        float hdZ = cosf(radPitch) * sinf(radYaw);

        int hx, hy, hz, px, py, pz;

        if (highlightRaycastCooldown <= 1) {
            hasBlockHighlight = raycastVoxel(camX, camY, camZ, hdX, hdY, hdZ, 6.0f, &hx, &hy, &hz, &px, &py, &pz);

            if (hasBlockHighlight) {
                highlightX = hx;
                highlightY = hy;
                highlightZ = hz;
            }

            highlightRaycastCooldown = 3;
        }

        highlightRaycastCooldown--;

        // if (hasBlockHighlight) {
        //     highlightX = hx;
        //     highlightY = hy;
        //     highlightZ = hz;
        // }
    }

    periodWasPressed = currentPeriodPress;
    leftClickWasDown = leftClick;
    rightClickWasDown = rightClick;

    float radYaw = glm_rad(camYaw);
    float forwardX = cosf(radYaw);
    float forwardZ = sinf(radYaw);

    float rightX = -forwardZ;
    float rightZ = forwardX;

    float camSpeed = pcamSpeed * deltaTime;

    bool wDown = (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS);
    bool sDown = (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS);
    bool aDown = (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS);
    bool dDown = (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS);
    bool shiftDown = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS);
    bool spaceIsDown = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);

    if (wDown && shiftDown) camSpeed = sprintSpeed * deltaTime;
    if (wDown || sDown || aDown || dDown) keyWasTouched = true;

    bool justToggleFly = false;
    if (spaceIsDown && !spaceKeyWasDown) {
        double now = glfwGetTime();

        if (lastSpaceTapTime > 0.0f && (now - lastSpaceTapTime) < doubleTapWindow) {
            isFlying = !isFlying;
            velY = 0.0f;
            flyVelX = flyVelY = flyVelZ = 0.0f;
            lastSpaceTapTime = -1.0;
            justToggleFly = true;
        } else {
            lastSpaceTapTime = now;
        }

        keyWasTouched = true;
    }

    spaceKeyWasDown = spaceIsDown;

    if (isFlying) {
        float targetVelX = 0.0f;
        float targetVelY = 0.0f;
        float targetVelZ = 0.0f;

        if (wDown) {
            targetVelX += forwardX;
            targetVelZ += forwardZ;
        }

        if (sDown) {
            targetVelX -= forwardX;
            targetVelZ -= forwardZ;
        }

        if (aDown) {
            targetVelX -= rightX;
            targetVelZ -= rightZ;
        }

        if (dDown) {
            targetVelX += rightX;
            targetVelZ += rightZ;
        }

        if (spaceIsDown) targetVelY += 1.0f;
        if (shiftDown) targetVelY -= 1.0f;

        float len = sqrtf(targetVelX * targetVelX + targetVelY * targetVelY + targetVelZ * targetVelZ);

        if (len > 0.0001f) {
            targetVelX = (targetVelX / len) * flySpeedMax;
            targetVelY = (targetVelY / len) * flySpeedMax;
            targetVelZ = (targetVelZ / len) * flySpeedMax; 
        }

        flyVelX += (targetVelX - flyVelX) * flyAccel * deltaTime;
        flyVelY += (targetVelY - flyVelY) * flyAccel * deltaTime;
        flyVelZ += (targetVelZ - flyVelZ) * flyAccel * deltaTime;

        tryMove(flyVelX * deltaTime, flyVelY * deltaTime, flyVelZ * deltaTime);
    } else {
        float moveX = 0.0f;
        float moveZ = 0.0f;

        if (wDown) {
            moveX += forwardX * camSpeed;
            moveZ += forwardZ * camSpeed;
        }

        if (sDown) {
            moveX -= forwardX * camSpeed;
            moveZ -= forwardZ * camSpeed;
        }

        if (aDown) {
            moveX -= rightX * camSpeed;
            moveZ -= rightZ * camSpeed;
        }

        if (dDown) {
            moveX += rightX * camSpeed;
            moveZ += rightZ * camSpeed;
        }

        tryMove(moveX, 0.0f, moveZ);

        if (spaceIsDown && !justToggleFly && isGrounded) {
            velY = jumpSpeed;
            isGrounded = false;           
        }

        velY -= gravity * deltaTime;

        if (velY < terminalVel) velY = terminalVel;

        isGrounded = false;

        tryMove(0.0f, velY * deltaTime, 0.0f);
    }

    if (!isCameraRotateble) {
        return;
    }

    keyWasTouched = true;

    double xPos, yPos;
    glfwGetCursorPos(window, &xPos, &yPos);

    if (firstMouseInput) {
        lastMouseX = (float)xPos;
        lastMouseY = (float)yPos;
        firstMouseInput = false;
    }

    float xOffset = (float)xPos - lastMouseX;
    float yOffset = lastMouseY - (float)yPos;

    lastMouseX = (float)xPos;
    lastMouseY = (float)yPos;

    xOffset *= sensitivity;
    yOffset *= sensitivity;

    camYaw += xOffset;
    camPitch += yOffset;

    if (camPitch > 89.0f) camPitch = 89.0f;
    if (camPitch < -89.0f) camPitch = -89.0f;

}

void placeBlock(int x, int y, int z, bool value) {
    int cx = (int)floorf((float)x / chunkSize);
    int cy = (int)floorf((float)y / chunkSize);
    int cz = (int)floorf((float)z / chunkSize);

    int localX = x - cx * chunkSize;
    int localY = y - cy * chunkSize;
    int localZ = z - cz * chunkSize;

    if (localX < 0) localX += chunkSize;
    if (localY < 0) localY += chunkSize;
    if (localZ < 0) localZ += chunkSize;

    if (localX < 0 || localX >= chunkSize || localY < 0 || localY >= chunkSize || localZ < 0 || localZ >= chunkSize) {
        return;
    }

    chunks* chunk = getOrCreateChunk(cx, cy, cz);
    if (!chunk) return;

    MUTEX_LOCK(&chunkLock);

    if (localX == 0) {
        chunks* n = getChunkSilentUnlocked(cx - 1, cy, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx - 1, cy, cz); }
    }

    if (localX == chunkSize - 1) {
        chunks* n = getChunkSilentUnlocked(cx + 1, cy, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx + 1, cy, cz); }
    }

    if (localY == 0) {
        chunks* n = getChunkSilentUnlocked(cx, cy - 1, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy - 1, cz); }
    }

    if (localY == chunkSize - 1) {
        chunks* n = getChunkSilentUnlocked(cx, cy + 1, cz);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy + 1, cz); }
    }

    if (localZ == 0) {
        chunks* n = getChunkSilentUnlocked(cx, cy, cz - 1);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy, cz - 1); }
    }

    if (localZ == chunkSize - 1) {
        chunks* n = getChunkSilentUnlocked(cx , cy, cz + 1);
        if (n) { n->isDirty = 1; n->meshVersion++; enqueueDirtyChunk(n, cx, cy, cz + 1); }
    }

    if (value) {
        chunk->voxels[localY][localZ] |= (1U << localX);
    } else {
        chunk->voxels[localY][localZ] &= ~(1U << localX);
    }

    chunk->isDirty = 1;
    chunk->meshVersion++;

    enqueueDirtyChunkFront(chunk, cx, cy, cz); 

    MUTEX_UNLOCK(&chunkLock);
}

void ensureDofFramebuffer(int width, int height) {
    if (dofFboWidth == width && dofFboHeight == height && dofFbo != 0) return;

    if (dofFbo != 0) {
        glDeleteFramebuffers(1, &dofFbo);
        glDeleteTextures(1, &dofDepthTex);
        glDeleteTextures(1, &dofColorTex);
    }

    glGenFramebuffers(1, &dofFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, dofFbo);
    glGenTextures(1, &dofColorTex);
    glBindTexture(GL_TEXTURE_2D, dofColorTex);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dofColorTex, 0);
    glGenTextures(1, &dofDepthTex);

    glBindTexture(GL_TEXTURE_2D, dofDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dofDepthTex, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    dofFboWidth = width;
    dofFboHeight = height;
}

void reuploadAllChunks(void) {
    masterVboOffsetBytes = 0;
    masterEboOffsetElements = 0;

    glBindBuffer(GL_ARRAY_BUFFER, masterVbo);
    glBufferData(GL_ARRAY_BUFFER, masterVboSize, NULL, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, masterEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, masterEboSize, NULL, GL_DYNAMIC_DRAW);

    chunkEntry *entry, *tmp;

    HASH_ITER(hh, loadedChunks, entry, tmp) {
        chunks* chunk = &entry->chunk;

        if (!chunk->mesh || chunk->mesh->indexCount == 0) continue;
        if (!chunk->cpuVertices || !chunk->cpuIndices) continue;

        size_t vertSizeBytes = chunk->cpuVertCount * sizeof(float);
        size_t indSizeBytes = chunk->cpuIndCount * sizeof(unsigned int);

        int meshBaseVertex = (int)(masterVboOffsetBytes / (7 * sizeof(float)));
        int meshFirstIndex = (int)masterEboOffsetElements;

        chunk->mesh->baseVertex = meshBaseVertex;
        chunk->mesh->firstIndex = meshFirstIndex;

        if (masterVboOffsetBytes + vertSizeBytes > masterVboSize || (masterEboOffsetElements + chunk->cpuIndCount) * sizeof(unsigned int) > masterEboSize) {
            printf("ERROR: this function (reiploadAllChunks) exeeded master buffer cap \n");
            break;
        }

        glBufferSubData(GL_ARRAY_BUFFER, masterVboOffsetBytes, vertSizeBytes, chunk->cpuVertices);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, masterEboOffsetElements * sizeof(unsigned int), indSizeBytes, chunk->cpuIndices);

        masterVboOffsetBytes += vertSizeBytes;
        masterEboOffsetElements += chunk->cpuIndCount;
    }
}

int main(){
    printf("STARTUP\n");
    fflush(stdout);

    EngineThread workerHandle;

    MUTEX_INIT(&chunkLock);
    MUTEX_INIT(&lodLock);
    COND_INIT(&chunkWorkReady);
    
    if (!glfwInit()) {
        printf("Couldn't initialize GLFW.\n");
        fflush(stdout);
        return -1;
    }

    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(width, height, stringed, NULL, NULL);

    if (!window) {

        printf("Couldn't initialize GLFW window.");
        fflush(stdout);
        glfwTerminate();
        return -1;

    }
    printf("window made\n");
    fflush(stdout);

    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("Couldn't Load GLAD.");
        fflush(stdout);
        glfwTerminate();
        return -1;
    }
    printf("glad initialized\n");
    fflush(stdout);

    unsigned int blockTextures = loadTextureArray(texturePath, 2, 512);
    printf("the textures loaded hooray\n");
    fflush(stdout);

    glViewport(0, 0, width, height);

    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyCallback);

    glfwSwapInterval(0);

    int success;
    char infolog[512];

    unsigned int vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexSrc, NULL);
    glCompileShader(vertexShader);

    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, sizeof(infolog), NULL, infolog);
        printf("Vertex Shader Ran into a problem:\n%s\n", infolog);
    }

    unsigned int fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentSrc, NULL);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, sizeof(infolog), NULL, infolog);
        printf("Fragment Shader Ran into a problem:\n%s\n", infolog);
    }

    unsigned int shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);

    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(shaderProgram, sizeof(infolog), NULL, infolog);
        printf("Program linking Ran into a problem:\n%s\n", infolog);
    }
    printf("shaders compiled\n");
    fflush(stdout);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    double time = 0;

    glGenVertexArrays(1, &masterVao);
    glGenBuffers(1, &masterVbo);
    glGenBuffers(1, &masterEbo);
    glGenBuffers(1, &masterIndirectBuffer);

    glBindVertexArray(masterVao);

    glBindBuffer(GL_ARRAY_BUFFER, masterVbo);
    glBufferData(GL_ARRAY_BUFFER, masterVboSize, NULL, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, masterEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, masterEboSize, NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, masterIndirectBuffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER, maxDrawCommands * sizeof(drawElementsIndirectCommand), NULL, GL_STREAM_DRAW);

    glBindVertexArray(0);

    // float vertices[] = {
    //     // pos, col

    //     -0.5f, -0.5f, -0.5f,
    //     0.5f, -0.5f, -0.5f,
    //     0.5f, 0.5f, -0.5f,
    //     -0.5f, 0.5f, -0.5f,

    //     -0.5f, -0.5f, 0.5f,
    //     0.5f, -0.5f, 0.5f,
    //     0.5f, 0.5f, 0.5f,
    //     -0.5f, 0.5f, 0.5f
    // };

    // unsigned int indices[] = {
    //     4, 5, 6,
    //     6, 7, 4,

    //     1, 0, 3, 
    //     3, 2, 1,

    //     0, 4, 7,
    //     7, 3, 0,

    //     5, 1, 2,
    //     2, 6, 5,

    //     3, 7, 6,
    //     6, 2, 3,

    //     0, 1, 5,
    //     5, 4, 0 
    // };

    // GLenum type = GL_STATIC_DRAW;

    // unsigned int vbo, vao, ebo, ivbo;
    // glGenBuffers(1, &vbo);
    // glGenBuffers(1, &ivbo);
    // glGenBuffers(1, &ebo);
    // glGenVertexArrays(1, &vao);

    // glBindVertexArray(vao);

    // glBindBuffer(GL_ARRAY_BUFFER, vbo);
    // glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, type);

    // glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    // glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, type);

    // glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    // glEnableVertexAttribArray(0);

    // glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    glCullFace(GL_BACK);

    mat4 model;
    glm_mat4_identity(model);

    mat4 view;
    glm_mat4_identity(view);

    vec3 translation = {0.0f, 0.0f, -3.0f};
    glm_translate(view, translation);

    vec3 axis = {1.0f, 0.0f, 0.0f};

    mat4 projection;
    mat4 viewProj;
    plane frustumPlanes[6];

    int widths, heights;

    vec3 cameraFront;
    vec3 cameraTarget;
    vec3 upVector = {0.0f, 1.0f, 0.0f};
    vec3 sunDir = { 0.4f, 0.8f, 0.3f };

    float halfChunkY = 0.0f;

    GLint modelLoc = glGetUniformLocation(shaderProgram, "model");
    GLint viewLoc = glGetUniformLocation(shaderProgram, "view");
    GLint projectionLoc = glGetUniformLocation(shaderProgram, "projection");
    GLint texAtlasLoc = glGetUniformLocation(shaderProgram, "texAtlas");
    GLint sunDirLoc = glGetUniformLocation(shaderProgram, "sunDir");
    GLint dofColorTexLoc = glGetUniformLocation(shaderProgram, "dofColorTex");
    GLint dofDepthTexLoc = glGetUniformLocation(shaderProgram, "dofDepthTex");
    GLint dofNearLoc = glGetUniformLocation(shaderProgram, "dofNear");
    GLint dofFarLoc = glGetUniformLocation(shaderProgram, "dofFar");
    GLint dofFocusDistLoc = glGetUniformLocation(shaderProgram, "dofFocusDist");
    GLint camPosLoc = glGetUniformLocation(shaderProgram, "camPos");
    GLint dofOnLoc = glGetUniformLocation(shaderProgram, "dofEnabled");
    GLint timeOfDayLoc = glGetUniformLocation(shaderProgram, "uTimeOfday");

    float dofQuadVerts[6 * 7] = {
        -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,  0.0f, 9.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,  0.0f, 9.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 9.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,  0.0f, 9.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 9.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,  0.0f, 9.0f
    };

    unsigned int dofVao, dofVbo;
    glGenVertexArrays(1, &dofVao);
    glGenBuffers(1, &dofVbo);
    glBindVertexArray(dofVao);

    glBindBuffer(GL_ARRAY_BUFFER, dofVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(dofQuadVerts), dofQuadVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    glGenVertexArrays(1, &highlightVao);
    glGenBuffers(1, &highlightVbo);

    glBindVertexArray(highlightVao);

    glBindBuffer(GL_ARRAY_BUFFER, highlightVbo);
    glBufferData(GL_ARRAY_BUFFER, 24 * 7 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    printf("buffers made\n");
    fflush(stdout);

    int workChunks = 64;

    double lastTime = 0;

    glm_vec3_normalize(sunDir);

    printf("locks made\n");
    fflush(stdout);

    glUseProgram(shaderProgram);

    unsigned int crosshairVao, crosshairVbo;
    glGenVertexArrays(1, &crosshairVao);
    glGenBuffers(1, &crosshairVbo);

    glBindVertexArray(crosshairVao);
    glBindBuffer(GL_ARRAY_BUFFER, crosshairVbo);
    glBufferData(GL_ARRAY_BUFFER, 4 * 7 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    float ch = 0.02f;

    static float tod = 0.25f;

#if defined(_WIN32) || defined(_WIN64)
    workerHandle = CreateThread(NULL, 0, backgroundChunkWorker, NULL, 0, NULL);
        
    if (!workerHandle) {
        printf("ERROR: failed to create worker...\n");
        return -1;
    }

    SetThreadPriority(workerHandle, THREAD_PRIORITY_BELOW_NORMAL);
#else
    if (pthread_create(&workerHandle, NULL, backgroundChunkWorker, NULL) != 0) {
        printf("ERROR: failed to create worker...\n");
        return -1;
    }
#endif

    printf("the worker thread(s) have been initialized\n");

    printf("entering main loop...\n");
    printf("END OF STARTUP INITIALIZATION\n");
    fflush(stdout);

    while (!glfwWindowShouldClose(window)) {
        time = glfwGetTime();

        float deltaTime = (lastTime > 0.0) ? (float)(time - lastTime) : (1.0f / 60.0f);

        lastTime = time;

        if (deltaTime > 0.05f) deltaTime = 0.05f;
        size_t ramMb = memUsage();

        glfwGetFramebufferSize(window, &widths, &heights);
        if (widths <= 0 || heights <= 0) {
            glfwPollEvents();
            continue;
        }

        float aspect = (float)widths / (float)heights;

        float crosshairVerts[4 * 7] = {
            -ch/aspect, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f,
            ch/aspect, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f,
            0.0f, -ch, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f,
            0.0f, ch, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f
        };

        tod += deltaTime * 0.005f;

        if (tod > 1.0f) tod -= 1.0f;

        float sunAngle = tod * 1.0f * PI;
        float dayFactor = fmaxf(sinf(sunAngle), 0.0f);
        float duskFactor = fmaxf(1.0f - fabsf(sinf(sunAngle)) * 2.0f, 0.0f);
        float skyR = 0.01f + 0.19f * dayFactor + 0.59f * duskFactor;
        float skyG = 0.01f + 0.29f * dayFactor + 0.24f * duskFactor;
        float skyB = 0.05f + 0.25f * dayFactor + 0.05f * duskFactor;

        processInput(window, deltaTime);

        glm_perspective(glm_rad(cfov), aspect, 0.1f, 800.0f, projection);
        // glm_rotate(model, glm_rad(-0.5f), axis);

        vec3 camPos = {camX, camY, camZ};

        cameraFront[0] = cosf(glm_rad(camYaw)) * cosf(glm_rad(camPitch));
        cameraFront[1] = sinf(glm_rad(camPitch));
        cameraFront[2] = sinf(glm_rad(camYaw)) * cosf(glm_rad(camPitch));
        glm_vec3_normalize(cameraFront);

        glm_vec3_add(camPos, cameraFront, cameraTarget);

        glm_lookat(camPos, cameraTarget, upVector, view);
        glm_mat4_mul(projection, view, viewProj);
        exactFrustumPlanes(viewProj, frustumPlanes);

        float halfChunkX = (float)chunkSize / 2.0f;
        float halfChunkZ = (float)chunkSize / 2.0f;

        int camChunkX = (int)floorf(camX / chunkSize);
        int camChunkY = (int)floorf(camY / chunkSize);
        int camChunkZ = (int)floorf(camZ / chunkSize);

        glUseProgram(shaderProgram);

        ensureDofFramebuffer(widths, heights);
        glBindFramebuffer(GL_FRAMEBUFFER, dofFbo);


        glClearColor(skyR, skyG, skyB, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, (float *)model);
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, (float *)view);
        glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, (float *)projection);

        glUniform3fv(sunDirLoc, 1, sunDir);

        glUniform3f(camPosLoc, camX, camY, camZ);

        glUniform1i(dofOnLoc, toggleDof ? 1 : 0);

        glUniform1f(timeOfDayLoc, tod);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, blockTextures);
        glUniform1i(texAtlasLoc, 0);

        globalCommandCount = 0;
        pendingCount = 0;

        int totalVerticesPerFrame = 0;
        int totalIndicesPerFrame = 0;

        glBindVertexArray(masterVao);

        int preloadRad = renderRad + 1;
        int preloaded = 0;

        MUTEX_LOCK(&chunkLock);

        for (int cx = camChunkX - preloadRad; cx <= camChunkX + preloadRad && preloaded < 2; cx++) {
            for (int cy = camChunkY - preloadRad; cy <= camChunkY + preloadRad && preloaded < 2; cy++) {
                for (int cz = camChunkZ - preloadRad; cz <= camChunkZ + preloadRad && preloaded < 2; cz++) {
                    if (!getChunkSilentUnlocked(cx, cy, cz)) {

                        MUTEX_UNLOCK(&chunkLock);

                        getOrCreateChunk(cx, cy, cz);

                        MUTEX_LOCK(&chunkLock);

                        preloaded++;
                    }
                }
            }
        }

        for (int cx = camChunkX - renderRad; cx <= camChunkX + renderRad; cx++) {
            for (int cy = camChunkY - renderRad; cy <= camChunkY + renderRad; cy++) {
                for (int cz = camChunkZ - renderRad; cz <= camChunkZ + renderRad; cz++) {
                    chunks* chunk = getChunkSilentUnlocked(cx, cy, cz);
                    if (!chunk) continue;

                    float* vertsToUpload = NULL;
                    unsigned int* indsToUpload = NULL;

                    int vertCount = 0;
                    int indCount = 0;
                    int meshBaseVertex = 0;
                    int meshFirstIndex = 0;

                    if (chunk->cpuMeshVersion != chunk->meshVersion) {
                        free(chunk->cpuVertices);
                        free(chunk->cpuIndices);

                        chunk->cpuVertices = NULL;
                        chunk->cpuIndices = NULL;

                        chunk->isMeshReady = false;
                        chunk->isDirty = 1;

                        enqueueDirtyChunk(chunk, cx, cy, cz); 

                        continue;
                    }

                    if (chunk->isMeshReady && chunk->cpuVertices && chunk->cpuIndices) {
                        if (pendingCount < maxUploadsPerFrame) {
                            vertCount = chunk->cpuVertCount;
                            indCount = chunk->cpuIndCount;

                            vertsToUpload = chunk->cpuVertices;
                            indsToUpload = chunk->cpuIndices;

                            chunk->cpuVertices = NULL;
                            chunk->cpuIndices = NULL;
                            
                            if (indCount > 0 && vertCount > 0) {
                                size_t vertSizeBytes = vertCount * sizeof(float);
                                size_t indSizeBytes = indCount * sizeof(unsigned int);

                                if (masterVboOffsetBytes + vertSizeBytes > masterVboSize || (masterEboOffsetElements + indCount) * sizeof(unsigned int) > masterEboSize) {
                                    reuploadAllChunks();
                                }

                                meshBaseVertex = (int)(masterVboOffsetBytes / (7 * sizeof(float)));
                                meshFirstIndex = (int)masterEboOffsetElements;

                                if (chunk->mesh == NULL) chunk->mesh = malloc(sizeof(meshs));
                                if (!chunk->mesh) {
                                    printf("Couldnt alloc mesh metasomething for a reason or not\n");

                                    chunk->isDirty = 1;

                                    enqueueDirtyChunk(chunk, cx, cy, cz); 

                                    chunk->isMeshReady = false;

                                    continue;
                                }

                                chunk->mesh->baseVertex = meshBaseVertex;
                                chunk->mesh->firstIndex = meshFirstIndex;
                                chunk->mesh->indexCount = indCount;
                            
                                masterVboOffsetBytes += vertSizeBytes;
                                masterEboOffsetElements += indCount;
                            }

                        } else {
                            free(chunk->cpuVertices);
                            free(chunk->cpuIndices);

                            chunk->cpuVertices = NULL;
                            chunk->cpuIndices = NULL;

                            if (chunk->mesh) {
                                free(chunk->mesh);

                                chunk->mesh = NULL;
                            }

                            chunk->isDirty = 1;

                            enqueueDirtyChunk(chunk, cx, cy, cz);
                        }

                        chunk->isMeshReady = false;
                        chunk->isDirty = 0;
                        chunk->cpuMeshVersion = chunk->meshVersion;
                    }

                    if (vertsToUpload && pendingCount < maxUploadsPerFrame) {
                        pendingUploads[pendingCount].verts = vertsToUpload;
                        pendingUploads[pendingCount].inds = indsToUpload;
                        pendingUploads[pendingCount].vertCount = vertCount;
                        pendingUploads[pendingCount].indCount = indCount;
                        pendingUploads[pendingCount].meshBaseVertex = meshBaseVertex;
                        pendingUploads[pendingCount].meshFirstIndex = meshFirstIndex;
                        pendingUploads[pendingCount].chunk = chunk;
                        pendingCount++;
                    }

                    chunk->gpuUploaded = true;
                }
            }       
        }

        MUTEX_UNLOCK(&chunkLock);

        for (int i = 0; i < pendingCount; i++) {
            pendingUpload* u = &pendingUploads[i];

            glBindBuffer(GL_ARRAY_BUFFER, masterVbo);
            glBufferSubData(GL_ARRAY_BUFFER, u->meshBaseVertex * (7 * sizeof(float)), u->vertCount * sizeof(float), u->verts);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, masterEbo);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, u->meshFirstIndex * sizeof(unsigned int), u->indCount * sizeof(unsigned int), u->inds);

            free(u->verts);
            free(u->inds);
        }

        MUTEX_LOCK(&chunkLock);

        for (int cx = camChunkX - renderRad; cx <= camChunkX + renderRad; cx++) {
            for (int cy = camChunkY - renderRad; cy <= camChunkY + renderRad; cy++) {
                for (int cz = camChunkZ - renderRad; cz <= camChunkZ + renderRad; cz++) { 
                    chunks* chunk = getChunkSilentUnlocked(cx, cy, cz);
                    if (!chunk) continue;

                    float wx = (float)(cx * chunkSize);
                    float wy = (float)(cy * chunkSize);
                    float wz = (float)(cz * chunkSize);

                    if (!aabbFrustum(frustumPlanes, wx, wy, wz, wx + chunkSize, wy + chunkSize, wz + chunkSize)) {
                        chunk->wasVisible = false;

                        continue;
                    }

                    chunk->wasVisible = true;

                    int localIdxCount = 0;
                    int localFirstIndx = 0;
                    int localBaseVertex = 0;

                    bool validMesh = false;

                    if (chunk->mesh != NULL && chunk->mesh->indexCount > 0) {
                        localIdxCount = chunk->mesh->indexCount;
                        localFirstIndx = chunk->mesh->firstIndex;
                        localBaseVertex = chunk->mesh->baseVertex;
                        validMesh = true;
                    }

                    chunk->wasVisible = true;

                    if (validMesh) {
                        float distToCam = sqrtf((wx - camX) * (wx - camX) + (wy - camY) * (wy - camY) + (wz - camZ) * (wz - camZ));

                        bool forceVisible = distToCam < chunkSize * 2.0f;

                        if (forceVisible || chunk->wasVisible) {
                            hostCommands[globalCommandCount].count = localIdxCount;
                            hostCommands[globalCommandCount].instanceCount = 1;
                            hostCommands[globalCommandCount].firstIndex = localFirstIndx;
                            hostCommands[globalCommandCount].baseVertex = localBaseVertex;
                            hostCommands[globalCommandCount].baseInstance = 0;
                            globalCommandCount++;

                            if (globalCommandCount >= maxDrawCommands) {

                                MUTEX_UNLOCK(&chunkLock);

                                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, masterIndirectBuffer);
                                glBufferData(GL_DRAW_INDIRECT_BUFFER, maxDrawCommands * sizeof(drawElementsIndirectCommand), NULL, GL_STREAM_DRAW);
                                glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, globalCommandCount * sizeof(drawElementsIndirectCommand), hostCommands);
                                drawWorldMesh(globalCommandCount);

                                globalCommandCount = 0;

                                MUTEX_LOCK(&chunkLock);
                            }
                        }
                    }
                }
            }
        }

        MUTEX_UNLOCK(&chunkLock);

        int drawnIndices = 0;

        for (int i = 0; i < globalCommandCount; i++) {
            drawnIndices += hostCommands[i].count;
        }

        int drawnTriangles = drawnIndices / 3;

        static int lastCommandCount = -1;

        if (globalCommandCount > 0 && globalCommandCount != lastCommandCount) {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, masterIndirectBuffer);
            glBufferData(GL_DRAW_INDIRECT_BUFFER, maxDrawCommands * sizeof(drawElementsIndirectCommand), NULL, GL_STREAM_DRAW);
            glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, globalCommandCount * sizeof(drawElementsIndirectCommand), hostCommands);
            lastCommandCount = globalCommandCount;
        }

        if (globalCommandCount > 0) {
            drawWorldMesh(globalCommandCount);
        }

        glBindVertexArray(0);

        int lodTriangles = 0;

        {
            int camTileX = (int)floorf(camX / (float)lodTileSize);
            int camTileZ = (int)floorf(camZ / (float)lodTileSize);
            int builtThisFrame = 0;

            for (int tx = camTileX - lodRadius; tx <= camTileX + lodRadius; tx++) {
                for (int tz = camTileZ - lodRadius; tz <= camTileZ + lodRadius; tz++) {
                    int distTilesX = abs(tx - camTileX);
                    int distTilesZ = abs(tz - camTileZ);

                    bool coveredByRealChunks = true;

                    int tileMinChunkX = (int)floorf((float)(tx * lodTileSize) / chunkSize);
                    int tileMaxChunkX = (int)floorf((float)(tx * lodTileSize + lodTileSize - 1) / chunkSize);
                    int tileMinChunkZ = (int)floorf((float)(tz * lodTileSize) / chunkSize);
                    int tileMaxChunkZ = (int)floorf((float)(tz * lodTileSize + lodTileSize - 1) / chunkSize);

                    for (int ccx = tileMinChunkX; ccx <= tileMaxChunkX && coveredByRealChunks; ccx++) {
                        for (int ccz = tileMinChunkZ; ccz <= tileMaxChunkZ && coveredByRealChunks; ccz++) {
                            int dx = abs(ccx - camChunkX);
                            int dz = abs(ccz - camChunkZ);

                            if (dx > renderRad || dz > renderRad) {
                                coveredByRealChunks = false;
                            }
                        }
                    }

                    if (coveredByRealChunks) continue;

                    float wx = tx * (float)lodTileSize;
                    float wz = tz * (float)lodTileSize;
                    if (!aabbFrustum(frustumPlanes, wx, -50.0f, wz, wx + lodTileSize, 300.0f, wz + lodTileSize)) continue;

                    lodTileEntry* tile = getOrCreateLodTile(tx, tz);
                    
                    EnterCriticalSection(&lodLock);

                    if (!tile->tile.built && tile->tile.cellSize == 0) {
                        int distForTier = (distTilesX > distTilesZ) ? distTilesX : distTilesZ;
                        tile->tile.cellSize = cellSizeForTileDist(distForTier);
                    }

                    bool tileCpuReady = tile->tile.cpuReady;
                    bool tileBuilt = tile->tile.built;
                    unsigned int tileVao = tile->tile.vao;
                    int tileVertCount = tile->tile.vertCount;

                    LeaveCriticalSection(&lodLock);

                    if (tileCpuReady) {
                        uploadLoadTile2Gpu(tile);
                        continue;
                    }

                    if (!tileBuilt) {
                        int next = (lodBuildTail + 1) % lodBuildQueueSize;
                        if (next != lodBuildHead) {

                            EnterCriticalSection(&lodLock);

                            lodBuildQueue[lodBuildTail] = tile;
                            lodBuildTail = next;

                            LeaveCriticalSection(&lodLock);

                            COND_SIGNAL(&chunkWorkReady);
                        }

                        continue;
                    }

                    glBindVertexArray(tileVao);
                    glDrawArrays(GL_TRIANGLES, 0, tileVertCount);
                    glBindVertexArray(0);

                    lodTriangles += tileVertCount / 3;
                }
            }

            static int unloadCooldown = 0;

            if (--unloadCooldown <= 0) {
                unloadDistantLodChunks(camTileX, camTileZ);
                unloadCooldown = 30;
            }
        }

        unloadDistantChunks(camChunkX, camChunkY, camChunkZ, renderRad + 1);

        drawnTriangles += lodTriangles;

        updFpsCounter(window, time, ramMb, camX, camY, camZ, globalCommandCount, drawnTriangles);

        glUseProgram(shaderProgram);
        glBindBuffer(GL_ARRAY_BUFFER, crosshairVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(crosshairVerts), crosshairVerts);

        glBindVertexArray(crosshairVao);
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, 4);
        glBindVertexArray(0);

        if (hasBlockHighlight) {
            float e = 0.002f;
            float x0 = (float)highlightX - e;
            float x1 = (float)highlightX + 1.0f + e;
            float y0 = (float)highlightY - e;
            float y1 = (float)highlightY + 1.0f + e;
            float z0 = (float)highlightZ - e;
            float z1 = (float)highlightZ + 1.0f + e;
            float p[8][3] = {
                {x0,y0,z0}, {x1,y0,z0}, {x1,y0,z1}, {x0,y0,z1},
                {x0,y1,z0}, {x1,y1,z0}, {x1,y1,z1}, {x0,y1,z1}
            };

            int edges[12][2] = {
                {0,1},{1,2},{2,3},{3,0},
                {4,5},{5,6},{6,7},{7,4},
                {0,4},{1,5},{2,6},{3,7}
            };

            float highlightVerts[24 * 7];

            int hv = 0;

            for (int e2 = 0; e2 < 12; e2++) {
                for (int v = 0; v < 2; v++) {
                    float* pt = p[edges[e2][v]];
                    highlightVerts[hv++] = pt[0];
                    highlightVerts[hv++] = pt[1];
                    highlightVerts[hv++] = pt[2];
                    highlightVerts[hv++] = 0.0f;
                    highlightVerts[hv++] = 0.0f;
                    highlightVerts[hv++] = 0.0f;
                    highlightVerts[hv++] = 10.0f;
                }
            }

            glUseProgram(shaderProgram);
            glBindBuffer(GL_ARRAY_BUFFER, highlightVbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(highlightVerts), highlightVerts);

            glBindVertexArray(highlightVao);
            glLineWidth(2.0f);
            glDrawArrays(GL_LINES, 0, 24);
            glBindVertexArray(0);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        glUseProgram(shaderProgram);
        glActiveTexture(GL_TEXTURE1);

        glBindTexture(GL_TEXTURE_2D, dofColorTex);
        glUniform1i(dofColorTexLoc, 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, dofDepthTex);
        glUniform1i(dofDepthTexLoc, 2);

        glUniform1f(dofNearLoc, 0.1f);
        glUniform1f(dofFarLoc, 800.0f);

        if (toggleDof) {
            glUniform1f(dofFocusDistLoc, 25.0f);
        } else {
            glUniform1f(dofFocusDistLoc, 1000.0f);
        }

        glBindVertexArray(dofVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);


        glEnable(GL_DEPTH_TEST);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    printf("EXIT: i will signal worker\n");
    fflush(stdout);

    ATOMIC_SET(isRunning, 0);

    COND_SIGNAL(&chunkWorkReady);

    #if defined(_WIN32) || defined(_WIN64)
        WaitForSingleObject(workerHandle, INFINITE);
        CloseHandle(workerHandle);
    #else
        MUTEX_LOCK(&chunkLock);
        pthread_cond_broadcast(&chunkWorkReady);
        MUTEX_UNLOCK(&chunkLock);
        pthread_join(workerHandle, NULL);
    #endif

    printf("EXIT: worker stopped\n");
    fflush(stdout);

    glDeleteProgram(shaderProgram);
    printf("EXIT: the shader got deleted\n");
    fflush(stdout);

    chunkEntry *entry, *tmp;
    int chunkIdx = 0;
    HASH_ITER(hh, loadedChunks, entry, tmp) {
        chunks* chunk = &entry->chunk;
        printf("EXIT: chunk %d mesh=%p verts=%p inds=%p\n", chunkIdx++, (void*)chunk->mesh, (void*)chunk->cpuVertices, (void*)chunk->cpuIndices);
        fflush(stdout);

        if (chunk->mesh != NULL) {
            free(chunk->mesh);
            chunk->mesh = NULL;
        }

        if (chunk->cpuVertices) free(chunk->cpuVertices);
        if (chunk->cpuIndices) free(chunk->cpuIndices);

        HASH_DEL(loadedChunks, entry);
        free(entry);
        printf("EXIT: chunk %d freed ok\n", chunkIdx - 1);
        fflush(stdout);
    }
    printf("EXIT: allat of chunks freed\n");
    fflush(stdout);

    glDeleteVertexArrays(1, &masterVao);
    glDeleteBuffers(1, &masterVbo);
    glDeleteBuffers(1, &masterEbo);
    glDeleteBuffers(1, &masterIndirectBuffer);
    glDeleteTextures(1, &blockTextures);
    printf("EXIT: the gl resources got incinerated\n");
    fflush(stdout);

    glDeleteVertexArrays(1, &dofVao);
    glDeleteBuffers(1, &dofVbo);
    glDeleteVertexArrays(1, &highlightVao);
    glDeleteBuffers(1, &highlightVbo);
    glDeleteVertexArrays(1, &crosshairVao);
    glDeleteBuffers(1, &crosshairVbo);
    glDeleteFramebuffers(1, &dofFbo);
    glDeleteTextures(1, &dofColorTex);
    glDeleteTextures(1, &dofDepthTex);
    printf("EXIT: bye bye OPENGL UI resources\n");
    fflush(stdout);

    lodTileEntry *lt, *lttemp;

    HASH_ITER(hh, loadedLodTiles, lt, lttemp) {
        if (lt->tile.built) {
            glDeleteBuffers(1, &lt->tile.vbo);
            glDeleteVertexArrays(1, &lt->tile.vao);
        }

        HASH_DEL(loadedLodTiles, lt);

        free(lt);
    }
    printf("EXIT: the lod tiles are gone\n");
    fflush(stdout);

    MUTEX_DESTROY(&chunkLock);
    MUTEX_UNLOCK(&lodLock);
    COND_DESTROY(&chunkWorkReady);

    glfwDestroyWindow(window);
    glfwTerminate();
    printf("EXIT: the exit was clean\n");
    fflush(stdout);

    return 0;
}