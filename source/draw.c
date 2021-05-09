#include <string.h>
#include <stdlib.h>
#include <tonc.h>

#include "globals.h"
#include "math.h"
#include "draw.h"
#include "logutils.h"
#include "model.h"

#define DRAW_MAX_TRIANGLES 256
EWRAM_DATA static RasterTriangle screenTriangles[DRAW_MAX_TRIANGLES]; 

#define RASTERPOINT_IN_BOUNDS_M5(vert) (vert.x >= 0 && vert.x < M5_SCALED_W && vert.y >= 0 && vert.y < M5_SCALED_H)
#define BEHIND_CAM(vert) (vert.z > -cam->near ) // True if the Vec3 is behind the near plane of the camera (i.e. invisible).

static int perfFill, perfModelProcessing, perfPolygonSort, perfProject;


void drawInit() 
{
    REG_DISPCNT = g_mode | DCNT_BG2;
    txt_init_std();
    perfFill = performanceDataRegister("draw.c: drawFillTris (rasterisation)");
    perfModelProcessing = performanceDataRegister("draw.c: drawModelInstances (pre-rasterisation)");
    perfPolygonSort = performanceDataRegister("draw.c: Polygon-sort");
    perfProject = performanceDataRegister("draw.c: drawModelInstance perspective");
}

void drawBefore(Camera *cam) 
{ 
    // Invariant: drawBefore is assumed to be called each frame once before the other draw functions are invoked.
    cameraComputeWorldToCamSpace(cam);
}

void drawPoints(const Camera *cam, Vec3 *points, int num, COLOR clr) 
{
    for (int i = 0; i < num; ++i) {
        Vec3 pointCamSpace = vecTransformed(cam->world2cam, points[i]);
        if (BEHIND_CAM(pointCamSpace)) {
            continue;
        }
        vecTransform(cam->perspMat, &pointCamSpace);
        RasterPoint rp = {.x = fx2int(pointCamSpace.x), .y=fx2int(pointCamSpace.y)};
        if (RASTERPOINT_IN_BOUNDS_M5(rp)) {
            m5_plot(rp.x, rp.y, clr);
        }
    }
}

static int triangleDepthCmp(const void *a, const void *b) 
{
        RasterTriangle *triA = (RasterTriangle*)a;
        RasterTriangle *triB = (RasterTriangle*)b;
        return triA->centroidZ - triB->centroidZ; // Smaller/"more negative" z values mean the triangle is farther away from the camera.
}

INLINE void swapRp(RasterPoint *a, RasterPoint *b) 
{
    RasterPoint tmp = *a;
    *a = *b;
    *b = tmp;
}


static void drawFillTris(RasterTriangle *tri, int numTris) 
{ 
    /* 
        DDA triangle-filling (flat-shaded). Uses a top-left fill convention to avoid overdraw/gaps. 
        The parts of triangles outside of the screen are simply not drawn (we avoid having to do 2d-clipping this way; it's faster than the Sutherland-Hodgman 2d-clipping approach I've tested).
        TODO: This is not particularly correct and not at all efficient, but my more complicated implementations I've tried so far were even more inconsistent, and not faster. 
        TODO: Subpixel-accuracy, cf. fatmap.txt.
    */
    RasterPoint v1, v2, v3;
    for (int i = 0; i < numTris; ++i) {

        v1 = tri[i].vert[0];
        v2 = tri[i].vert[1];
        v3 = tri[i].vert[2];
        // Order vertices: v1 top, v2 middle, v3 bottom.
        if (v1.y > v2.y) {
            swapRp(&v1, &v2);
        }
        if (v2.y > v3.y) {
            swapRp(&v2, &v3);
        }
        if (v1.y > v2.y) {
            swapRp(&v1, &v2);
        }

        if (v1.y == v3.y) { // Degenerate triangle, we don't draw those.
            continue;
        }
        // assertion(v1.y <= v2.y && v2.y <= v3.y, "draw.c: drawFillTris correct vertex ordering");
     
        const bool middleLeft = v2.x <= v1.x; // If middleLeft is true, the triangle's left side consists of two edges, otherwise, it's the right side which consists of two edges.

        // First, we fill the top section of the triangle:
        FIXED invslopeLong = fxdiv(int2fx(v3.x - v1.x), int2fx(v3.y - v1.y));
        FIXED invslopeShort; 
        if (v2.y - v1.y) { // For invslopeShort, it's important to avoid a division by zero in case we have a triangle with a flat top.
            invslopeShort = fxdiv(int2fx(v2.x - v1.x), int2fx(v2.y - v1.y));
        } else {
            assertion(v3.y - v2.y, "draw.c: drawFillTris: v3.y - v2.y != 0");
            invslopeShort = fxdiv(int2fx(v3.x - v2.x), int2fx(v3.y - v2.y)); 
        }
        int yStart = MAX(0, v1.y);
        int yEnd = MIN(M5_SCALED_H - 1, v2.y);
        int dy = yStart - v1.y; 
        FIXED leftDeltaX = middleLeft ? invslopeShort : invslopeLong;
        FIXED rightDeltaX = middleLeft ? invslopeLong : invslopeShort;
        FIXED xLeft = int2fx(v1.x) + fxmul(int2fx(dy), leftDeltaX);
        FIXED xRight = int2fx(v1.x) + fxmul(int2fx(dy), rightDeltaX);
        for (int y = yStart; y < yEnd; ++y) { 
            xLeft += leftDeltaX;
            xRight += rightDeltaX;
            const int left = MIN(MAX(0, fx2int(xLeft)), M5_SCALED_W - 1);
            const int right = MIN(MAX(0, fx2int(xRight)), M5_SCALED_W - 1);
            m5_hline(left, y, right - 1, tri[i].color);
        }
        // Finally, we fill the bottom section of the triangle:
        if (v2.y < v3.y) { // Avoid division by zero in case there is no bottom half.
            invslopeShort = fxdiv(int2fx(v3.x - v2.x), int2fx(v3.y - v2.y));
        } else {
            continue;
        }
        leftDeltaX = middleLeft ? invslopeShort : invslopeLong;
        rightDeltaX = middleLeft ? invslopeLong : invslopeShort;
        yStart = MAX(0, v2.y);
        yEnd = MIN(M5_SCALED_H - 1, v3.y);
        dy = yStart - v2.y; 
        xLeft = middleLeft ? int2fx(v2.x) + fxmul(int2fx(dy), leftDeltaX) : xLeft + leftDeltaX;
        xRight = middleLeft ? xRight + rightDeltaX : int2fx(v2.x) + fxmul(int2fx(dy), rightDeltaX);
        for (int y = yStart; y < yEnd; ++y) { 
            const int left = MIN(MAX(0, fx2int(xLeft)), M5_SCALED_W - 1);
            const int right = MIN(MAX(0, fx2int(xRight)), M5_SCALED_W - 1);
            m5_hline(left, y, right - 1, tri[i].color);
            xLeft += leftDeltaX;
            xRight += rightDeltaX;
        }
    }
}


static void drawWireframeTris(RasterTriangle *tri, int numTris) 
{
    // m5_line(v1.x, v1.y, v2.x, v2.y, clr);
    // m5_line(v2.x, v2.y, v3.x, v3.y, clr);
    // m5_line(v3.x, v3.y, v1.x, v1.y, clr);
}


void drawModelInstances(const Camera *cam, const ModelInstance *instances, int numInstances, Vec3 lightDir) 
{ 
    performanceStart(perfModelProcessing);
    int screenTriangleCount = 0;
    for (int instanceNum = 0; instanceNum < numInstances; ++instanceNum) {
        const ModelInstance instance = instances[instanceNum];
        
        // FIXED dx, dy, dz;
        // dx = ABS(instance.pos.x - cam->pos.x);
        // dy = ABS(instance.pos.y - cam->pos.y);
        // dz = ABS(instance.pos.z - cam->pos.z);
        // FIXED dist = fxmul(dx, dx) + fxmul(dy, dy) + fxmul(dz, dz);
        // dist = Sqrt(dist << FIX_SHIFT); // sqrt(2**8) * sqrt(2**8) = 2**8
        // if (dist > cam->far) {
        //     continue;
        // }

        FIXED instanceRotMat[16];
        matrix4x4createYawPitchRoll(instanceRotMat, instance.yaw, instance.pitch, instance.roll);
        Vec3 vertsCamSpace[MAX_MODEL_VERTS];
        for (int i = 0; i < instance.mod.numVerts; ++i) {
            // Model space to world space:
            vertsCamSpace[i] = vecScaled(instance.mod.verts[i], instance.scale);
            vecTransform(instanceRotMat, vertsCamSpace + i );
            // We translate manually so that instanceRotMat stays as is (so we can rotate our normals with the instanceRotMat in model space to calculate lighting):
            vertsCamSpace[i].x += instance.pos.x;
            vertsCamSpace[i].y += instance.pos.y;
            vertsCamSpace[i].z += instance.pos.z;
            vecTransform(cam->world2cam, vertsCamSpace + i); // And finally, we're in camera space.
        }

        for (int faceNum = 0; faceNum < instance.mod.numFaces; ++faceNum) { // For each face (triangle, really) of the ModelInstace. 
            const Face face = instance.mod.faces[faceNum];
             // Backface culling (assumes a clockwise winding order):
            const Vec3 a = vecSub(vertsCamSpace[face.vertexIndex[1]], vertsCamSpace[face.vertexIndex[0]]);
            const Vec3 b = vecSub(vertsCamSpace[face.vertexIndex[2]], vertsCamSpace[face.vertexIndex[0]]);
            const Vec3 triNormal = vecCross(a, b);
            const Vec3 camToTri = vertsCamSpace[face.vertexIndex[0]]; // Remember, vertsCamSpace[] is in camera space already, so it doesn't make sense subtract the camera's wolrd position!
            if (vecDot(triNormal, camToTri) <= 0) { // If the angle between camera and normal is not between 90 degs and 270 degs, the face is invisible and to be culled.
                continue;
            }

            Vec3 triVerts[3] = {vertsCamSpace[face.vertexIndex[0]], vertsCamSpace[face.vertexIndex[1]], vertsCamSpace[face.vertexIndex[2]]};
            // TODO: Proper 3d clipping against the near plane (so far, triangles are discared even if only one vertex is behind the near plane).
            if (BEHIND_CAM(triVerts[0]) || BEHIND_CAM(triVerts[1]) || BEHIND_CAM(triVerts[2])) {  // Triangle extends behind the near clipping plane -> reject the whole thing. 
                continue; // TODO!
            } else if (triVerts[0].z < -cam->far || triVerts[1].z < -cam->far || triVerts[1].z < -cam->far) { // Triangle extends beyond the far-plane -> reject the whole thing.
                continue;
            }
            RasterTriangle clippedTri;
            performanceStart(perfProject);
            for (int i = 0; i < 3; ++i) { // Perspective projection, and conversion of the fixed point numbers to integers.
                assertion(triVerts[i].z <= -cam->near, "draw.c: drawModelInstances: Perspective division in front of near-plane");
                vecTransform(cam->perspMat, triVerts + i);
                clippedTri.vert[i] = (RasterPoint){.x=fx2int(triVerts[i].x), .y=fx2int(triVerts[i].y)};   
            }
            performanceEnd(perfProject);
            // Check if all vertices of the face are to the "outside-side" of a given clipping plane. If so, the face is invisible and we can skip it.
            if (triVerts[0].x < 0 && triVerts[1].x < 0 && triVerts[2].x < 0) { // All vertices are to the left of the left-plane.
                continue;
            } else if (triVerts[0].x >= cam->canvasWidth && triVerts[1].x >= cam->canvasWidth && triVerts[2].x >= cam->canvasWidth ) { // All vertices are to the right of the right-plane.
                continue;
            } else if (triVerts[0].y < 0 && triVerts[1].y < 0 && triVerts[2].y < 0) { // All vertices are to the top of the top-plane.
                continue;
            } else if (triVerts[0].y >= cam->canvasHeight && triVerts[1].y >= cam->canvasHeight && triVerts[2].y >= cam->canvasHeight) { // All vertices are to the bottom of the bottom-plane.
                continue;
            }
            // Color/lighting:
            const Vec3 faceNormal = vecTransformed(instanceRotMat, face.normal); // Rotate the face's normal (in model space).
            const FIXED lightAlpha = vecDot(lightDir, faceNormal);
            if (lightAlpha > 0) {
                COLOR shade = fx2int(fxmul(lightAlpha, int2fx(31)));
                shade = MAX(2, shade);
                clippedTri.color = RGB15(shade, shade, shade);
            } else {
                clippedTri.color = RGB15(2,2,2);
            }     

            clippedTri.centroidZ = vertsCamSpace[face.vertexIndex[0]].z; // TODO HACK: That's not the actual centroid (we'd need an expensive division for that), but this is good enough for small faces.
            assertion(screenTriangleCount < DRAW_MAX_TRIANGLES, "draw.c: drawModelInstances: screenTriangleCount < DRAW_MAX_TRIANGLES");
            screenTriangles[screenTriangleCount++] = clippedTri;
        }
    }
    performanceEnd(perfModelProcessing);
    // All our Triangles are ready for drawing! We only should order them according to their depth, as we sadly don't have a z-buffer.
    performanceStart(perfPolygonSort);
    qsort(screenTriangles, screenTriangleCount, sizeof(screenTriangles[0]), triangleDepthCmp);
    performanceEnd(perfPolygonSort);

    performanceStart(perfFill); 
    drawFillTris(screenTriangles, screenTriangleCount);    
    performanceEnd(perfFill);
    char txt[128];
    sprintf(txt, "tris: %d", screenTriangleCount);
    m5_puts(8, 80, txt, CLR_FUCHSIA);
}
