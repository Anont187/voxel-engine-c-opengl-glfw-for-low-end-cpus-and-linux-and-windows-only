#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <windows.h>
#include <psapi.h> 
#include <string.h>
#include "cglm/cglm.h"
#include "uthash.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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

const vec3 faceNorms[6] = vec3[6](
    vec3(-1.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, -1.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, -1.0),
    vec3(0.0, 0.0, 1.0)
);

out vec4 fragColor;

vec3 applyFog(vec3 baseColor, vec3 fragWorldPos) {
    float dist = distance(camPos, fragWorldPos);
    float fogStart = 100.0;
    float fogEnd = 180.0;
    float fogAmount = clamp((dist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);

    vec3 fogColor = vec3(0.2, 0.3, 0.3);

    return mix(baseColor, fogColor, fogAmount);
}

void main() {
    if (faceIdPass == 6) {
        fragColor = vec4(1.0, 1.0, 1.0, 0.9);
        return;
    }

    if (faceIdPass == 7) {
        vec3 grassCol = vec3(0.168f, 0.3686f, 0.0471f);
        float diff = max(dot(vec3(0.0, 1.0, 0.0), sunDir), 0.0);
        float lighting = 0.35 + 0.65 * diff;
        fragColor = vec4(applyFog(grassCol * lighting, worldPos), 1.0);
        return;
    } 

    if (faceIdPass == 8) {
        vec3 dirtCol = vec3(0.4667f, 0.3059f, 0.0902f) * 0.7;

        fragColor = vec4(applyFog(dirtCol, worldPos), 1.0);
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

    vec4 texColor = texture(texAtlas, vec3(uvPass, layerPass));

    if (layerPass == 1) {
        texColor.rgb *= vec3(0.42, 0.68, 0.30);
    }

    vec3 normal = faceNorms[faceIdPass];

    float diff = max(dot(normal, sunDir), 0.0);
    float lighting = 0.35 + 0.65 * diff;

    texColor.rgb *= lighting;

    fragColor = vec4(applyFog(texColor.rgb, worldPos), texColor.a);
}
)";

#define chunkSize 32
#define renderRad 2
#define lodTileSize 64
#define lodCellSize 8
#define lodRadius 6
#define maxTerrainHeight 125
#define noiseStep 4
#define noiseGridSize (chunkSize / noiseStep + 1)

int width = 640;
int height = 480;
int goWireframe = 0;
int activeIdxCount = 0;
int dofFboWidth = 0;
int dofFboHeight = 0;

float pcamSpeed = 4.3f;
float rotSpeed = 60.0f;
float sprintSpeed = 12.0f;
float sensitivity = 0.1f;
float baseSensitivity = 0.1f;
float bfov = 90.0f;
float cfov = 90.0f;
float scale = 0.015f;

bool spaceWasPressed = false;
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

double lastSpaceTapTime = -1.0;

bool isCameraRotateble = false;
bool firstMouseInput = true;
bool isFlying = false;
bool isGrounded = false;
bool spaceKeyWasDown = false;
bool toggleDof = true;

unsigned int dofFbo = 0;
unsigned int dofColorTex = 0;
unsigned int dofDepthTex = 0;

const float gravity = 28.0f;
const float jumpSpeed = 8.0f;
const float terminalVel = -50.0f;
const float flyAccel = 6.0f;
const float flySpeedMax = 10.0f;

const double doubleTapWindow = 0.3f;

typedef struct {
    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;
    int indexCount;
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
    bool voxels[chunkSize][chunkSize][chunkSize];
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
} chunks;

typedef struct {
    char key[64];
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
    bool built;
} lodTile;

typedef struct {
    long long key;
    int tx, tz;
    lodTile tile;
    UT_hash_handle hh;
} lodTileEntry;

lodTileEntry* loadedLodTiles = NULL;
chunkEntry* loadedChunks = NULL;

const char* stringed = "basic window";
const char* texturePath[] = { "textures/dirt.png", "textures/grass.png" };

CRITICAL_SECTION chunkLock;
volatile bool isRunning = true;

float noiseFade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float noiseLerp(float t, float a, float b) {
    return a + t * (b - a);
}

float noiseGrad(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

float perlin2d(float x, float y) {
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

float fbm(float x, float z, int octaves, float persistence, float lacunarity) {
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

float getTerrainHeight(float wx, float wz) {
    float continental = fbm(wx * scale * 0.15f, wz * scale * 0.15f, 4, 0.5f, 2.0f);
    float shaped = continental * continental * (3.0f - 2.0f * continental);
    float baseheight = shaped * maxTerrainHeight;
    float detail = fbm(wx * scale * 2.0f, wz * scale * 2.0f, 4, 0.5f, 2.0f) * 8.0f;
    detail *= (0.2f + shaped * 0.8f);

    return baseheight + detail;
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

float sampleUpsampledHeight(float grid[noiseGridSize][noiseGridSize], int localX, int localZ) {
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

void buildLodSize(lodTileEntry* entry) {
    int cellsPerSide = lodTileSize / lodCellSize;
    int maxVerts = cellsPerSide * cellsPerSide * 5 * 6 * 7;
    int vc = 0;

    float* verts = malloc(maxVerts * sizeof(float));
    float maxExpectedHeight = maxTerrainHeight;
    float floorY = -30.0f;

    for (int cx = 0; cx < cellsPerSide; cx++) {
        for (int cz = 0; cz < cellsPerSide; cz++) {
            float wx0 = entry->tx * (float)lodTileSize + cx * lodCellSize;
            float wz0 = entry->tz * (float)lodTileSize + cz * lodCellSize;
            float wx1 = wx0 + lodCellSize;
            float wz1 = wz0 + lodCellSize;

            float h = getTerrainHeight((wx0 + wx1) * 0.5f, (wz0 + wz1) * 0.5f);

            float topColorU = h / maxExpectedHeight;

            float p00[3] = { wx0, h, wz0 };
            float p10[3] = { wx1, h, wz0 };
            float p11[3] = { wx1, h, wz1 };
            float p01[3] = { wx0, h, wz1 };

            float* topTri1[3] = { p00, p11, p10 };
            float* topTri2[3] = { p00, p01, p11 };

            for (int t = 0; t < 2; t++) {
                float** tri = (t == 0) ? topTri1 : topTri2;
                for (int i = 0; i < 3; i++) {
                    verts[vc++] = tri[i][0];
                    verts[vc++] = tri[i][1];
                    verts[vc++] = tri[i][2];
                    verts[vc++] = topColorU;
                    verts[vc++] = 0.0f;
                    verts[vc++] = 0.0f;
                    verts[vc++] = 7.0f;
                }
            }

            float sideCorners[4][4][3] = {
                { {wx0,h,wz0}, {wx1,h,wz0}, {wx1,floorY,wz0}, {wx0,floorY,wz0} }, // front
                { {wx1,h,wz1}, {wx0,h,wz1}, {wx0,floorY,wz1}, {wx1,floorY,wz1} }, // back
                { {wx0,h,wz1}, {wx0,h,wz0}, {wx0,floorY,wz0}, {wx0,floorY,wz1} }, // left
                { {wx1,h,wz0}, {wx1,h,wz1}, {wx1,floorY,wz1}, {wx1,floorY,wz0} }  // right
            };

            for (int s = 0; s < 4; s++) {
                float* q0 = sideCorners[s][0];
                float* q1 = sideCorners[s][1];
                float* q2 = sideCorners[s][2];
                float* q3 = sideCorners[s][3];
                float* sideTri1[3] = { q0, q1, q2 };
                float* sideTri2[3] = { q0, q2, q3 };

                for (int t = 0; t < 2; t++) {
                    float** tri = (t == 0) ? sideTri1 : sideTri2;
                    for (int i = 0; i < 3; i++) {
                        verts[vc++] = tri[i][0];
                        verts[vc++] = tri[i][1];
                        verts[vc++] = tri[i][2];
                        verts[vc++] = 0.0f;
                        verts[vc++] = 0.0f;
                        verts[vc++] = 0.0f;
                        verts[vc++] = 8.0f;
                    }
                }
            }
        }
    }

    glGenVertexArrays(1, &entry->tile.vao);
    glGenBuffers(1, &entry->tile.vbo);

    glBindVertexArray(entry->tile.vao);
    glBindBuffer(GL_ARRAY_BUFFER, entry->tile.vbo);
    glBufferData(GL_ARRAY_BUFFER, vc * sizeof(float), verts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);

    glBindVertexArray(0);

    entry->tile.vertCount = vc / 7;
    entry->tile.built = true;

    free(verts);
}

lodTileEntry* getOrCreateLodTile(int tx, int tz) {
    long long key = ((long long)(tx + 100000) << 32) | (unsigned int)(tz + 100000);
    lodTileEntry* entry = NULL;
    HASH_FIND(hh, loadedLodTiles, &key, sizeof(long long), entry);
    if (!entry) {
        entry = malloc(sizeof(lodTileEntry));
        entry->key = key;
        entry->tx = tx;
        entry->tz = tz;
        entry->tile.built = false;
        HASH_ADD(hh, loadedLodTiles, key, sizeof(long long), entry);
    }
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

chunks* getOrCreateChunk(int cx, int cy, int cz) {
    char key[64];
    snprintf(key, sizeof(key), "%d,%d,%d", cx, cy, cz);

    chunkEntry* entry = NULL;

    EnterCriticalSection(&chunkLock);

    HASH_FIND_STR(loadedChunks, key, entry);

    if (entry) {
        LeaveCriticalSection(&chunkLock);
        return &entry->chunk;
    }

    entry = malloc(sizeof(chunkEntry));
    if (!entry) {
        LeaveCriticalSection(&chunkLock);
        return NULL;
    }

    strncpy(entry->key, key, sizeof(entry->key) - 1);
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

    memset(entry->chunk.voxels, 0, sizeof(entry->chunk.voxels));

    int chunkMinY = cy * chunkSize;
    int chunkMaxY = chunkMinY + chunkSize - 1;

    if (chunkMinY > (int)(maxTerrainHeight + 12.0f)) {
        entry->chunk.isGenerated = 1;
        HASH_ADD_STR(loadedChunks, key, entry);
        LeaveCriticalSection(&chunkLock);
        return &entry->chunk;
    }

    if (chunkMaxY < -12) {
        memset(entry->chunk.voxels, 1, sizeof(entry->chunk.voxels));
        entry->chunk.isGenerated = 1;
        HASH_ADD_STR(loadedChunks, key, entry);
        LeaveCriticalSection(&chunkLock);
        return &entry->chunk;
    }

    float heightGrid[noiseGridSize][noiseGridSize];
    buildCoarseHeightGrid(cx, cz, heightGrid);

    for (int z = 0; z < chunkSize; z++) {
        for (int x = 0; x < chunkSize; x++) {
            // int worldX = cx * chunkSize + x;
            // int worldZ = cz * chunkSize + z;

            int terrainHeight = (int)sampleUpsampledHeight(heightGrid, x, z);

            for (int y = 0; y < chunkSize; y++) {
                int worldY = cy * chunkSize + y;

                if (worldY <= terrainHeight) {
                    entry->chunk.voxels[x][y][z] = true;
                }
            }
        }
    }

    entry->chunk.isGenerated = 1;

    HASH_ADD_STR(loadedChunks, key, entry);

    LeaveCriticalSection(&chunkLock);

    return &entry->chunk;
}

chunks* getChunkSilent(int cx, int cy, int cz) {
    char key[64];
    snprintf(key, sizeof(key), "%d,%d,%d", cx, cy, cz);
    chunkEntry* entry = NULL;
    HASH_FIND_STR(loadedChunks, key, entry);
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

    chunks* chunk = getChunkSilent(cx, cy, cz);
    if (!chunk || !chunk->isGenerated) return false;

    return chunk->voxels[localX][localY][localZ];
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

    while (dist < maxDist) {
        if (isSolidAtWorld(x, y, z)) {
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

    for (int x = minX; x <= maxX; x++) {
        for (int y = minY; y <= maxY; y++) {
            for (int z = minZ; z <= maxZ; z++) {
                if (isSolidAtWorld(x, y, z)) return true;
            }
        }
    }

    return false;
}

float tryMove(float dx, float dy, float dz) {
    float feetY = camY - eyeHeight;

    if (dx != 0.0f) {
        float newX = camX + dx;
        if (!checkPlayerCollision(newX, feetY, camZ)) camX = newX;
    }

    if (dz != 0.0f) {
        float newZ = camZ + dz;
        if (!checkPlayerCollision(camX, feetY, newZ)) camZ = newZ;
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
    EnterCriticalSection(&chunkLock);
    chunkEntry *entry, *tmp;
    HASH_ITER(hh, loadedChunks, entry, tmp) {
        int dx = entry->cx - camChunkX;
        int dy = entry->cy - camChunkY;
        int dz = entry->cz - camChunkZ;

        if (abs(dx) > keepRadius || abs(dy) > keepRadius || abs(dz) > keepRadius) {
            if (entry->chunk.isMeshing) continue;

            if (entry->chunk.mesh != NULL) {
                glDeleteBuffers(1, &entry->chunk.mesh->vbo);
                glDeleteBuffers(1, &entry->chunk.mesh->ebo);
                glDeleteVertexArrays(1, &entry->chunk.mesh->vao);
                free(entry->chunk.mesh);
            }

            if (entry->chunk.cpuVertices) free(entry->chunk.cpuVertices);
            if (entry->chunk.cpuIndices) free(entry->chunk.cpuIndices);

            HASH_DEL(loadedChunks, entry);
            free(entry);
        }
    }

    LeaveCriticalSection(&chunkLock);
}

DWORD WINAPI backgroundChunkWorker(LPVOID lpParam) {
    while (isRunning) {
        chunks* targetChunk = NULL;

        int tCx = 0;
        int tCy = 0;
        int tCz = 0;

        EnterCriticalSection(&chunkLock);

        chunkEntry* entry, *tmp;

        HASH_ITER(hh, loadedChunks, entry, tmp) {
            if (entry->chunk.isDirty && !entry->chunk.isMeshReady && !entry->chunk.isMeshing && !entry->chunk.isBroken && entry->chunk.cpuVertices == NULL) {
                targetChunk = &entry->chunk;
                tCx = entry->cx;
                tCy = entry->cy;
                tCz = entry->cz;
                targetChunk->isMeshing = true;
                break;
            }
        }

        LeaveCriticalSection(&chunkLock);

        if (targetChunk == NULL) {
            Sleep(5);
            continue;
        }

        if (!isRunning) {
            EnterCriticalSection(&chunkLock);
            targetChunk->isMeshing = false;
            LeaveCriticalSection(&chunkLock);

            break;
        }

        if (!targetChunk->isGenerated) {
            for (int z = 0; z < chunkSize; z++) {
                for (int x = 0; x < chunkSize; x++) {
                    int worldX = tCx * chunkSize + x;
                    int worldZ = tCz * chunkSize + z;
                    float terrainHeight = getTerrainHeight((float)worldX, (float)worldZ);

                    for (int y = 0; y < chunkSize; y++) {
                        int worldY = tCy * chunkSize + y;

                        if (worldY <= terrainHeight) {
                            targetChunk->voxels[x][y][z] = true;
                        }
                    }
                }
            }
            targetChunk->isGenerated = 1;
        }

        int dim = chunkSize + 2;

        bool* blocks = calloc(dim * dim * dim, sizeof(bool));
        if (!blocks) {
            EnterCriticalSection(&chunkLock);
            targetChunk->isMeshing = false;
            LeaveCriticalSection(&chunkLock);
            continue;
        }

        EnterCriticalSection(&chunkLock);
        chunks* nChunks[3][3][3] = {NULL};
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    nChunks[dx + 1][dy + 1][dz + 1] = getChunkSilent(tCx + dx, tCy + dy, tCz + dz);
                }
            }
        }

        for (int lx = -1; lx <= chunkSize; lx++) {
            for (int ly = -1; ly <= chunkSize; ly++) {
                for (int lz = -1; lz <= chunkSize; lz++) {
                    int targetDx = (lx < 0) ? -1 : (lx >= chunkSize ? 1 : 0);
                    int targetDy = (ly < 0) ? -1 : (ly >= chunkSize ? 1 : 0);
                    int targetDz = (lz < 0) ? -1 : (lz >= chunkSize ? 1 : 0);

                    chunks* target = nChunks[targetDx + 1][targetDy + 1][targetDz + 1];

                    if (target) {
                        int tLx = (lx + chunkSize) % chunkSize;
                        int tLy = (ly + chunkSize) % chunkSize;
                        int tLz = (lz + chunkSize) % chunkSize;

                        blocks[(lx + 1) * dim * dim + (ly + 1) * dim + (lz + 1)] = target->voxels[tLx][tLy][tLz];
                    }
                }
            }
        }

        LeaveCriticalSection(&chunkLock);

        // int maxVertices = chunkSize * chunkSize * chunkSize * 6 * 4 * 6;
        // int maxIndices = chunkSize * chunkSize * chunkSize * 6 * 6;

        int maxVertices = 256 * 1024;
        int maxIndices = 128 * 1024;
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

        for (int d = 0; d < 3; d++) {
            if (!isRunning) { meshGenFailed = 2; break; }

            int u = (d + 1) % 3;
            int v = (d + 2) % 3;

            int x[3] = {0, 0, 0};
            int q[3] = {0, 0, 0};

            bool* mask = calloc(dim * dim, sizeof(bool));
            if (!mask) {
                meshGenFailed = 1;
                break;
            }

            q[d] = 1;

            for (x[d] = -1; x[d] < chunkSize && !meshGenFailed; ++x[d]) {
                memset(mask, 0, dim * dim * sizeof(bool));

                for (x[v] = 0; x[v] < chunkSize; ++x[v]) {
                    for (x[u] = 0; x[u] < chunkSize; ++x[u]) {
                        int blkCurX = x[0] + 1;
                        int blkCurY = x[1] + 1;
                        int blkCurZ = x[2] + 1;

                        bool curBlock = (x[d] >= 0) ? blocks[blkCurX * dim * dim + blkCurY * dim + blkCurZ] : false;

                        int nextX = blkCurX + q[0];
                        int nextY = blkCurY + q[1];
                        int nextZ = blkCurZ + q[2];
                        bool neighbourBlock = blocks[nextX * dim * dim + nextY * dim + nextZ];

                        mask[x[u] * dim + x[v]] = (curBlock != neighbourBlock);
                    }
                }

                for (int j = 0; j < chunkSize; j++) {
                    for (int i = 0; i < chunkSize; i++) {
                        if (mask[i * dim + j]) {
                            int width = 1;
                            while (i + width < chunkSize && mask[(i + width) * dim + j]) {
                                width++;
                            }

                            int height = 1;
                            bool done = false;
                            while (j + height < chunkSize) {
                                for (int k = 0; k < width; ++k) {
                                    if (!mask[(i + k) * dim + (j + height)]) {
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
                                        mask[idx] = false;
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

                            if (vertCount + 6 >= maxVerticesCap) {
                                if (maxVerticesCap >= hardVertexCap) { meshGenFailed = 1; break; }
                                maxVerticesCap *= 2;
                                if (maxVerticesCap > hardVertexCap) maxVerticesCap = hardVertexCap;
                                float* newVerts = realloc(tempVertices, maxVerticesCap * sizeof(float));
                                if (!newVerts) {
                                    meshGenFailed = 1;
                                    break;
                                }
                                tempVertices = newVerts;
                            }

                            if (indCount + 6 >= maxIndicesCap) {
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
            free(mask);
        }

        free(blocks);

        EnterCriticalSection(&chunkLock);

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

            if (targetChunk->cpuVertices) free(targetChunk->cpuVertices);
            if (targetChunk->cpuIndices) free(targetChunk->cpuIndices);
            targetChunk->cpuVertices = tempVertices;
            targetChunk->cpuIndices = tempIndices;
            targetChunk->cpuVertCount = vertCount;
            targetChunk->cpuIndCount = indCount;
            targetChunk->isMeshReady = true;
            targetChunk->genFailCount = 0;
        }
        targetChunk->isMeshing = false;
        LeaveCriticalSection(&chunkLock);
    }
    return 0;
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

int memUsage() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return (int)(pmc.WorkingSetSize / (1024 * 1024));
    }
    return 0;
}

void updFpsCounter(GLFWwindow* window, double time, size_t ramSize, float camX, float camY, float camZ) {
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

        snprintf(ts, sizeof(ts), "%s | FPS: %.1f | Frame Time: %.2fms | Ram Usage: %zuMB | cam x: %.2f | cam y: %.2f | cam z: %.2f", stringed, fps, mspf, ramSize, camX, camY, camZ);

        glfwSetWindowTitle(window, ts);
        prevT = ct;
        fc = 0;
    }
    fc++;
}

void drawWorldMesh(meshs* m) {
    if (m == NULL || m->indexCount == 0) return;

    glBindVertexArray(m->vao);
    glDrawElements(GL_TRIANGLES, m->indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
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

    EnterCriticalSection(&chunkLock);
    chunks* chunk = getOrCreateChunk(cx, cy, cz);
    chunk->voxels[localX][localY][localZ] = !chunk->voxels[localX][localY][localZ];
    chunk->isDirty = 1;
    LeaveCriticalSection(&chunkLock);
}

void processInput(GLFWwindow* window, float deltaTime) {
    if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }

    bool getF1 = (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS);

    if (getF1 && (!spaceWasPressed)) {
        goWireframe = (goWireframe == 0) ? 1 : 0;
    }

    spaceWasPressed = getF1;
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

    bool ctrlShiftPressed = (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) && (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)); 

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

    EnterCriticalSection(&chunkLock);
    chunks* chunk = getOrCreateChunk(cx, cy, cz);

    chunk->voxels[localX][localY][localZ] = value;

    chunk->isDirty = 1;
    LeaveCriticalSection(&chunkLock);
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

int main(){
    
    if (!glfwInit()) {
        printf("Couldn't initialize GLFW.\n");
        return -1;
    }

    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(width, height, stringed, NULL, NULL);

    if (!window) {

        printf("Couldn't initialize GLFW window.");
        glfwTerminate();
        return -1;

    }

    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("Couldn't Load GLAD.");
        glfwTerminate();
        return -1;
    }

    unsigned int blockTextures = loadTextureArray(texturePath, 2, 512);

    glViewport(0, 0, width, height);

    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetScrollCallback(window, scrollCallback);

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

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    double time = 0;

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

    int workChunks = 64;

    double lastTime = 0;

    glm_vec3_normalize(sunDir);

    InitializeCriticalSection(&chunkLock);
    HANDLE workerHandle = CreateThread(NULL, 0, backgroundChunkWorker, NULL, 0, NULL);

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

    while (!glfwWindowShouldClose(window)) {

        time = glfwGetTime();

        float deltaTime = (lastTime > 0.0) ? (float)(time - lastTime) : (1.0f / 60.0f);

        lastTime = time;

        if (deltaTime > 0.05f) deltaTime = 0.05f;
        size_t ramMb = memUsage();

        glfwGetFramebufferSize(window, &widths, &heights);

        float aspect = (float)widths / (float)heights;

        float crosshairVerts[4 * 7] = {
            -ch/aspect, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f,
            ch/aspect, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f,
            0.0f, -ch, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f,
            0.0f, ch, 0.0f, 0.0f, 0.0f, 0.0f, 6.0f
        };

        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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

        if (toggleDof) {
            ensureDofFramebuffer(widths, heights);
            glBindFramebuffer(GL_FRAMEBUFFER, dofFbo);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, (float *)model);
        glUniformMatrix4fv(viewLoc, 1, GL_FALSE, (float *)view);
        glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, (float *)projection);

        glUniform3fv(sunDirLoc, 1, sunDir);

        glUniform3f(camPosLoc, camX, camY, camZ);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, blockTextures);
        glUniform1i(texAtlasLoc, 0);

        for (int cx = camChunkX - renderRad; cx <= camChunkX + renderRad; cx++) {
            for (int cy = camChunkY - renderRad; cy <= camChunkY + renderRad; cy++) {
                for (int cz = camChunkZ - renderRad; cz <= camChunkZ + renderRad; cz++) { 
                    float wx = (float)(cx * chunkSize);
                    float wy = (float)(cy * chunkSize);
                    float wz = (float)(cz * chunkSize);

                    if (!aabbFrustum(frustumPlanes, wx, wy, wz, wx + chunkSize, wy + chunkSize, wz + chunkSize)) {
                        continue;
                    }

                    chunks* chunk = getOrCreateChunk(cx, cy, cz);

                    EnterCriticalSection(&chunkLock);
                    if (chunk->isMeshReady) {
                        if (chunk->mesh == NULL) {
                            chunk->mesh = malloc(sizeof(meshs));

                            if (chunk->mesh == NULL) {
                                LeaveCriticalSection(&chunkLock);
                                continue;
                            }

                            glGenVertexArrays(1, &chunk->mesh->vao);
                            glGenBuffers(1, &chunk->mesh->vbo);
                            glGenBuffers(1, &chunk->mesh->ebo);
                        }

                        glBindVertexArray(chunk->mesh->vao);

                        glBindBuffer(GL_ARRAY_BUFFER, chunk->mesh->vbo);
                        glBufferData(GL_ARRAY_BUFFER, chunk->cpuVertCount * sizeof(float), NULL, GL_STATIC_DRAW);
                        glBufferSubData(GL_ARRAY_BUFFER, 0, chunk->cpuVertCount * sizeof(float), chunk->cpuVertices);

                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, chunk->mesh->ebo);
                        glBufferData(GL_ELEMENT_ARRAY_BUFFER, chunk->cpuIndCount * sizeof(unsigned int), NULL, GL_STATIC_DRAW);
                        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, chunk->cpuIndCount * sizeof(unsigned int), chunk->cpuIndices);

                        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)0);
                        glEnableVertexAttribArray(0);

                        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(3 * sizeof(float)));
                        glEnableVertexAttribArray(1);

                        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(5 * sizeof(float)));
                        glEnableVertexAttribArray(2);

                        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void*)(6 * sizeof(float)));
                        glEnableVertexAttribArray(3);

                        glBindVertexArray(0);

                        chunk->mesh->indexCount = chunk->cpuIndCount;

                        free(chunk->cpuVertices);
                        free(chunk->cpuIndices);

                        chunk->cpuVertices = NULL;
                        chunk->cpuIndices = NULL;

                        chunk->isMeshReady = false;
                        chunk->isDirty = 0;
                    }
                    LeaveCriticalSection(&chunkLock);

                    if (chunk->mesh != NULL && chunk->mesh->indexCount > 0) {
                        drawWorldMesh(chunk->mesh);
                    }
                }
            }
        }

        {
            int camTileX = (int)floorf(camX / (float)lodTileSize);
            int camTileZ = (int)floorf(camZ / (float)lodTileSize);

            int realRadInTiles = (renderRad * chunkSize) / lodTileSize;
            int builtThisFrame = 0;

            for (int tx = camTileX - lodRadius; tx <= camTileX + lodRadius; tx++) {
                for (int tz = camTileZ - lodRadius; tz <= camTileZ + lodRadius; tz++) {
                    int distTilesX = abs(tx - camTileX);
                    int distTilesZ = abs(tz - camTileZ);
                    if (distTilesX <= realRadInTiles && distTilesZ <= realRadInTiles) continue;

                    float wx = tx * (float)lodTileSize;
                    float wz = tz * (float)lodTileSize;
                    if (!aabbFrustum(frustumPlanes, wx, -50.0f, wz, wx + lodTileSize, 300.0f, wz + lodTileSize)) continue;

                    lodTileEntry* tile = getOrCreateLodTile(tx, tz);
                    if (!tile->tile.built) {
                        if (builtThisFrame < 1) {
                            buildLodSize(tile);
                            builtThisFrame++;
                        } else {
                            continue;
                        }
                    }

                    glBindVertexArray(tile->tile.vao);
                    glDrawArrays(GL_TRIANGLES, 0, tile->tile.vertCount);
                    glBindVertexArray(0);
                }
            }
        }

        unloadDistantChunks(camChunkX, camChunkY, camChunkZ, renderRad + 1);

        updFpsCounter(window, time, ramMb, camX, camY, camZ);

        glUseProgram(shaderProgram);
        glBindBuffer(GL_ARRAY_BUFFER, crosshairVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(crosshairVerts), crosshairVerts);

        glBindVertexArray(crosshairVao);
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, 4);
        glBindVertexArray(0);

        if (toggleDof) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST);

            glUseProgram(shaderProgram);
            glActiveTexture(GL_TEXTURE1);

            glBindTexture(GL_TEXTURE_2D, dofColorTex);
            glUniform1i(dofColorTexLoc, 1);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, dofDepthTex);
            glUniform1i(dofDepthTexLoc, 2);

            glUniform1f(dofNearLoc, 0.1f);
            glUniform1f(dofFarLoc, 800.0f);
            glUniform1f(dofFocusDistLoc, 25.0f);

            glBindVertexArray(dofVao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);

            glEnable(GL_DEPTH_TEST);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    isRunning = false;
    WaitForSingleObject(workerHandle, INFINITE);
    CloseHandle(workerHandle);

    glDeleteProgram(shaderProgram);

    chunkEntry *entry, *tmp;
    HASH_ITER(hh, loadedChunks, entry, tmp) {
        chunks* chunk = &entry->chunk;

        if (chunk->mesh != NULL) {
            glDeleteBuffers(1, &chunk->mesh->vbo);
            glDeleteBuffers(1, &chunk->mesh->ebo);
            glDeleteVertexArrays(1, &chunk->mesh->vao);
            free(chunk->mesh);
        }

        voxelBlock *block, *tmp2;

        if (chunk->cpuVertices) free(chunk->cpuVertices);
        if (chunk->cpuIndices) free(chunk->cpuIndices);

        HASH_DEL(loadedChunks, entry);
        free(entry);

    }

    DeleteCriticalSection(&chunkLock);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}