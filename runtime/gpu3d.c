/* 3D engine: geometry commands (packed FIFO and command ports), matrix
 * stacks, lighting, polygon assembly and clipping, and a software
 * rasterizer. The geometry list is swapped at VBlank after SWAP_BUFFERS and
 * rendered in one go; the 2D engine then reads it line by line as BG0. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nds.h"
#include <time.h>
#ifdef DQIX_TIMING   /* render timing for performance work (Linux test builds) */
static double ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#else
static double ms_now(void) { return 0; }
#endif

#define IO16_(o) (*(uint16_t*)&g_io[o])
#define IO32_(o) (*(uint32_t*)&g_io[o])

/* ---------------------------------------------------------------- matrices
 * 4x4, 20.12 fixed point, row-major (m[row*4+col]); vectors are rows: v' = v M. */
typedef int32_t Mtx[16];

static Mtx m_proj, m_pos, m_vec, m_tex, m_clip;
static Mtx s_proj[1], s_pos[32], s_vec[32], s_tex[1];
static int sp_proj, sp_pos, sp_tex, stack_overflow;
static int mtx_mode;
static int clip_dirty = 1;

static void mtx_identity(int32_t* m) { memset(m, 0, sizeof(Mtx)); m[0] = m[5] = m[10] = m[15] = 0x1000; }

static void mtx_mul(int32_t* r, const int32_t* a, const int32_t* b) {   /* r = a * b */
    Mtx t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++) s += (int64_t)a[i * 4 + k] * b[k * 4 + j];
            t[i * 4 + j] = (int32_t)(s >> 12);
        }
    memcpy(r, t, sizeof t);
}

static void update_clip(void) {
    if (clip_dirty) { mtx_mul(m_clip, m_pos, m_proj); clip_dirty = 0; }
}

/* ---------------------------------------------------------------- polygons */
typedef struct {
    float x, y, z, w;        /* clip space (w in units of 1.0) */
    float r, g, b;           /* vertex color 0..31 */
    float s, t;              /* texel coordinates */
} Vtx;

typedef struct {             /* screen-space vertex */
    float x, y;              /* pixels */
    float zn;                /* z/w mapped to 0..1 */
    float iw;                /* 1/w */
    float r, g, b, s, t;
} SVtx;

#define MAX_POLY 2048
#define MAX_PV 10
typedef struct {
    SVtx v[MAX_PV];
    int n;
    uint32_t attr, teximg, pltt;
    int translucent;
    float ymin, ymax;
} Poly;

static Poly g_polybuf[2][MAX_POLY];
static int g_npoly[2], g_nvert[2];
static int g_geo = 0;                      /* buffer being filled by geometry */
static int g_swap_pending, g_swap_flags, g_render_flags;
static int g_render_ready;

/* current geometry state */
static uint32_t cur_color = 0x7FFF;        /* 15-bit */
static float cur_r = 31, cur_g = 31, cur_b = 31;
static int32_t raw_s, raw_t;               /* 1.11.4 */
static float cur_s, cur_t;                 /* transformed, texels */
static uint32_t poly_attr_pending, poly_attr;
static uint32_t teximg, pltt;
static int prim_type, in_begin;
static int16_t last_vx, last_vy, last_vz;  /* last vertex (for VTX_XY etc.), 4.12 */
static int vp_x1 = 0, vp_y1 = 0, vp_x2 = 255, vp_y2 = 191;
static Vtx strip[4];
static int strip_n, strip_count;

/* lighting */
static float light_dir[4][3];
static int light_col[4][3];
static int mat_dif[3], mat_amb[3], mat_spe[3], mat_emi[3];
static int use_shine;
static uint8_t shine_table[128];
static int shine_pos;

/* test results */
static int box_result;

static void rgb15(uint32_t c, int* r, int* g, int* b) { *r = c & 31; *g = (c >> 5) & 31; *b = (c >> 10) & 31; }

/* Clip a polygon against the 6 planes of the view volume (homogeneous). */
static int clip_plane(Vtx* in, int n, Vtx* out, int axis, int sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vtx* a = &in[i];
        Vtx* b = &in[(i + 1) % n];
        float ca = (axis == 0 ? a->x : axis == 1 ? a->y : a->z) * sign;
        float cb = (axis == 0 ? b->x : axis == 1 ? b->y : b->z) * sign;
        float da = a->w - ca, db = b->w - cb;      /* inside when >= 0 */
        if (da >= 0) { if (m < MAX_PV) out[m++] = *a; }
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            Vtx v;
            v.x = a->x + (b->x - a->x) * t; v.y = a->y + (b->y - a->y) * t;
            v.z = a->z + (b->z - a->z) * t; v.w = a->w + (b->w - a->w) * t;
            v.r = a->r + (b->r - a->r) * t; v.g = a->g + (b->g - a->g) * t; v.b = a->b + (b->b - a->b) * t;
            v.s = a->s + (b->s - a->s) * t; v.t = a->t + (b->t - a->t) * t;
            if (m < MAX_PV) out[m++] = v;
        }
    }
    return m;
}

static void emit_polygon(Vtx* src, int n) {
    if (g_npoly[g_geo] >= MAX_POLY) return;
    int mode = (poly_attr >> 4) & 3;
    if (mode == 3) return;                                   /* shadow polygons: not yet */
    /* far plane: hide the polygon unless "render clipped" is set */
    if (!(poly_attr & 0x1000)) for (int i = 0; i < n; i++) if (src[i].z > src[i].w) return;
    Vtx a[MAX_PV], b[MAX_PV];
    int m = n;
    memcpy(a, src, sizeof(Vtx) * n);
    m = clip_plane(a, m, b, 2, -1); if (m < 3) return;      /* near: z >= -w */
    m = clip_plane(b, m, a, 2, 1);  if (m < 3) return;      /* far */
    m = clip_plane(a, m, b, 0, -1); if (m < 3) return;
    m = clip_plane(b, m, a, 0, 1);  if (m < 3) return;
    m = clip_plane(a, m, b, 1, -1); if (m < 3) return;
    m = clip_plane(b, m, a, 1, 1);  if (m < 3) return;

    Poly* p = &g_polybuf[g_geo][g_npoly[g_geo]];
    float vw = (float)(vp_x2 - vp_x1 + 1), vh = (float)(vp_y2 - vp_y1 + 1);
    float area = 0;
    p->ymin = 1e9f; p->ymax = -1e9f;
    for (int i = 0; i < m; i++) {
        float w = a[i].w > 1e-6f ? a[i].w : 1e-6f;
        SVtx* v = &p->v[i];
        v->x = vp_x1 + (a[i].x / w + 1) * vw * 0.5f;
        v->y = 192 - (vp_y1 + (a[i].y / w + 1) * vh * 0.5f);
        v->zn = (a[i].z / w + 1) * 0.5f;
        v->iw = 1.0f / w;
        v->r = a[i].r; v->g = a[i].g; v->b = a[i].b; v->s = a[i].s; v->t = a[i].t;
        if (v->y < p->ymin) p->ymin = v->y;
        if (v->y > p->ymax) p->ymax = v->y;
    }
    for (int i = 0; i < m; i++) {
        SVtx* u = &p->v[i]; SVtx* v = &p->v[(i + 1) % m];
        area += u->x * v->y - v->x * u->y;
    }
    /* Facing: visually counter-clockwise (negative area with y down) is the front. */
    int front = area < 0;
    if (area == 0) { if (!(poly_attr & 0xC0)) return; }
    else if (front ? !(poly_attr & 0x80) : !(poly_attr & 0x40)) return;
    p->n = m;
    p->attr = poly_attr;
    p->teximg = teximg;
    p->pltt = pltt;
    int alpha = (poly_attr >> 16) & 31;
    int fmt = (teximg >> 26) & 7;
    p->translucent = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
    g_npoly[g_geo]++;
    g_nvert[g_geo] += m;
}

static void submit_vertex(int16_t x, int16_t y, int16_t z) {
    last_vx = x; last_vy = y; last_vz = z;
    if (!in_begin) return;
    update_clip();
    int64_t vx = x, vy = y, vz = z;
    int32_t* c = m_clip;
    Vtx v;
    v.x = (float)((vx * c[0] + vy * c[4] + vz * c[8] + ((int64_t)c[12] << 12)) >> 12) / 4096.0f;
    v.y = (float)((vx * c[1] + vy * c[5] + vz * c[9] + ((int64_t)c[13] << 12)) >> 12) / 4096.0f;
    v.z = (float)((vx * c[2] + vy * c[6] + vz * c[10] + ((int64_t)c[14] << 12)) >> 12) / 4096.0f;
    v.w = (float)((vx * c[3] + vy * c[7] + vz * c[11] + ((int64_t)c[15] << 12)) >> 12) / 4096.0f;
    v.r = cur_r; v.g = cur_g; v.b = cur_b;
    if ((teximg >> 30) == 3) {                               /* texcoords from the vertex */
        int32_t* t = m_tex;
        cur_s = (float)(((vx * t[0] + vy * t[4] + vz * t[8]) >> 24) + raw_s) / 16.0f;
        cur_t = (float)(((vx * t[1] + vy * t[5] + vz * t[9]) >> 24) + raw_t) / 16.0f;
    }
    v.s = cur_s; v.t = cur_t;
    strip[strip_n % 4] = v;
    strip_n++;
    strip_count++;
    Vtx q[4];
    switch (prim_type) {
    case 0:                                                  /* triangles */
        if (strip_count == 3) { q[0] = strip[(strip_n - 3) % 4]; q[1] = strip[(strip_n - 2) % 4]; q[2] = strip[(strip_n - 1) % 4]; emit_polygon(q, 3); strip_count = 0; }
        break;
    case 1:                                                  /* quads */
        if (strip_count == 4) { for (int i = 0; i < 4; i++) q[i] = strip[(strip_n - 4 + i) % 4]; emit_polygon(q, 4); strip_count = 0; }
        break;
    case 2:                                                  /* triangle strip */
        if (strip_count >= 3) {
            Vtx* a = &strip[(strip_n - 3) % 4]; Vtx* b = &strip[(strip_n - 2) % 4]; Vtx* cc = &strip[(strip_n - 1) % 4];
            if (strip_count & 1) { q[0] = *a; q[1] = *b; } else { q[0] = *b; q[1] = *a; }
            q[2] = *cc;
            emit_polygon(q, 3);
        }
        break;
    case 3:                                                  /* quad strip */
        if (strip_count >= 4 && !(strip_count & 1)) {
            q[0] = strip[(strip_n - 4) % 4]; q[1] = strip[(strip_n - 3) % 4];
            q[2] = strip[(strip_n - 1) % 4]; q[3] = strip[(strip_n - 2) % 4];
            emit_polygon(q, 4);
        }
        break;
    }
}

static void do_lighting(int32_t param) {
    float n[3];
    for (int i = 0; i < 3; i++) n[i] = (float)((int32_t)((param >> (10 * i)) << 22) >> 22) / 512.0f;
    if ((teximg >> 30) == 2) {                               /* texcoords from the normal */
        int32_t nx = (int32_t)((param) << 22) >> 22, ny = (int32_t)((param >> 10) << 22) >> 22, nz = (int32_t)((param >> 20) << 22) >> 22;
        int64_t N[3] = { (int64_t)nx << 3, (int64_t)ny << 3, (int64_t)nz << 3 };
        int32_t* t = m_tex;
        cur_s = (float)(((N[0] * t[0] + N[1] * t[4] + N[2] * t[8]) >> 21) + raw_s) / 16.0f;
        cur_t = (float)(((N[0] * t[1] + N[1] * t[5] + N[2] * t[9]) >> 21) + raw_t) / 16.0f;
    }
    float tn[3];
    for (int j = 0; j < 3; j++) tn[j] = (n[0] * m_vec[0 * 4 + j] + n[1] * m_vec[1 * 4 + j] + n[2] * m_vec[2 * 4 + j]) / 4096.0f;
    float col[3] = { (float)mat_emi[0], (float)mat_emi[1], (float)mat_emi[2] };
    for (int l = 0; l < 4; l++) {
        if (!(poly_attr & (1u << l))) continue;
        float* L = light_dir[l];
        float dl = -(L[0] * tn[0] + L[1] * tn[1] + L[2] * tn[2]);
        if (dl < 0) dl = 0; if (dl > 1) dl = 1;
        float h[3] = { (L[0]) * 0.5f, (L[1]) * 0.5f, (L[2] - 1.0f) * 0.5f };
        float sl = -(h[0] * tn[0] + h[1] * tn[1] + h[2] * tn[2]);
        if (sl < 0) sl = 0; if (sl > 1) sl = 1;
        sl = sl * sl;
        if (use_shine) sl = shine_table[(int)(sl * 127.0f)] / 255.0f;
        for (int c = 0; c < 3; c++)
            col[c] += (mat_spe[c] * light_col[l][c] * sl + mat_dif[c] * light_col[l][c] * dl + mat_amb[c] * light_col[l][c]) / 31.0f;
    }
    cur_r = col[0] > 31 ? 31 : col[0]; cur_g = col[1] > 31 ? 31 : col[1]; cur_b = col[2] > 31 ? 31 : col[2];
}

static void set_color15(uint32_t c) {
    int r, g, b; rgb15(c, &r, &g, &b);
    cur_color = c & 0x7FFF; cur_r = (float)r; cur_g = (float)g; cur_b = (float)b;
}

/* ---------------------------------------------------------------- commands */
static const uint8_t param_count[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12,
    [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1, [0x27] = 1,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static int cmd_valid(uint8_t c) {
    return (c >= 0x10 && c <= 0x1C) || (c >= 0x20 && c <= 0x2B) || (c >= 0x30 && c <= 0x34) ||
           c == 0x40 || c == 0x41 || c == 0x50 || c == 0x60 || (c >= 0x70 && c <= 0x72);
}

static int32_t* cur_mtx_for_load(int* both) {
    *both = mtx_mode == 2;
    return mtx_mode == 0 ? m_proj : mtx_mode == 3 ? m_tex : m_pos;
}

static void mtx_changed(void) { clip_dirty = 1; }

static void load_mtx(const int32_t* m) {
    int both; int32_t* d = cur_mtx_for_load(&both);
    memcpy(d, m, sizeof(Mtx));
    if (both) memcpy(m_vec, m, sizeof(Mtx));
    mtx_changed();
}

static void mult_mtx(const int32_t* m) {
    int both; int32_t* d = cur_mtx_for_load(&both);
    mtx_mul(d, m, d);
    if (both) mtx_mul(m_vec, m, m_vec);
    mtx_changed();
}

static void exec(uint8_t cmd, const uint32_t* p) {
    switch (cmd) {
    case 0x10: mtx_mode = p[0] & 3; break;
    case 0x11:                                                       /* PUSH */
        if (mtx_mode == 0) { if (sp_proj) stack_overflow = 1; memcpy(s_proj[0], m_proj, sizeof(Mtx)); sp_proj = 1; }
        else if (mtx_mode == 3) { memcpy(s_tex[0], m_tex, sizeof(Mtx)); sp_tex = 1; }
        else {
            if (sp_pos >= 31) stack_overflow = 1;
            memcpy(s_pos[sp_pos & 31], m_pos, sizeof(Mtx)); memcpy(s_vec[sp_pos & 31], m_vec, sizeof(Mtx));
            sp_pos = (sp_pos + 1) & 63;
        }
        break;
    case 0x12: {                                                     /* POP */
        if (mtx_mode == 0) { sp_proj = 0; memcpy(m_proj, s_proj[0], sizeof(Mtx)); }
        else if (mtx_mode == 3) { sp_tex = 0; memcpy(m_tex, s_tex[0], sizeof(Mtx)); }
        else {
            int n = (int32_t)(p[0] << 26) >> 26;
            sp_pos = (sp_pos - n) & 63;
            if (sp_pos >= 31) stack_overflow = 1;
            memcpy(m_pos, s_pos[sp_pos & 31], sizeof(Mtx)); memcpy(m_vec, s_vec[sp_pos & 31], sizeof(Mtx));
        }
        mtx_changed();
        break;
    }
    case 0x13: {                                                     /* STORE */
        if (mtx_mode == 0) memcpy(s_proj[0], m_proj, sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(s_tex[0], m_tex, sizeof(Mtx));
        else { int n = p[0] & 31; if (n == 31) stack_overflow = 1; memcpy(s_pos[n], m_pos, sizeof(Mtx)); memcpy(s_vec[n], m_vec, sizeof(Mtx)); }
        break;
    }
    case 0x14: {                                                     /* RESTORE */
        if (mtx_mode == 0) memcpy(m_proj, s_proj[0], sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(m_tex, s_tex[0], sizeof(Mtx));
        else { int n = p[0] & 31; if (n == 31) stack_overflow = 1; memcpy(m_pos, s_pos[n], sizeof(Mtx)); memcpy(m_vec, s_vec[n], sizeof(Mtx)); }
        mtx_changed();
        break;
    }
    case 0x15: { Mtx m; mtx_identity(m); load_mtx(m); break; }
    case 0x16: { Mtx m; for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; load_mtx(m); break; }
    case 0x17: { Mtx m; for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c]; m[r * 4 + 3] = r == 3 ? 0x1000 : 0; } load_mtx(m); break; }
    case 0x18: { Mtx m; for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; mult_mtx(m); break; }
    case 0x19: { Mtx m; for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c]; m[r * 4 + 3] = r == 3 ? 0x1000 : 0; } mult_mtx(m); break; }
    case 0x1A: { Mtx m; mtx_identity(m); for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = (int32_t)p[r * 3 + c]; mult_mtx(m); break; }
    case 0x1B: {                                                     /* SCALE: not applied to the vector matrix */
        Mtx m; mtx_identity(m); m[0] = (int32_t)p[0]; m[5] = (int32_t)p[1]; m[10] = (int32_t)p[2];
        int32_t* d = mtx_mode == 0 ? m_proj : mtx_mode == 3 ? m_tex : m_pos;
        mtx_mul(d, m, d); mtx_changed();
        break;
    }
    case 0x1C: { Mtx m; mtx_identity(m); m[12] = (int32_t)p[0]; m[13] = (int32_t)p[1]; m[14] = (int32_t)p[2]; mult_mtx(m); break; }
    case 0x20: set_color15(p[0]); break;
    case 0x21: do_lighting((int32_t)p[0]); break;
    case 0x22:
        raw_s = (int16_t)(p[0] & 0xFFFF); raw_t = (int16_t)(p[0] >> 16);
        if ((teximg >> 30) == 1) {
            int32_t* t = m_tex;
            int64_t s = raw_s, tt = raw_t;
            cur_s = (float)((s * t[0] + tt * t[4] + t[8] + t[12]) >> 12) / 16.0f;
            cur_t = (float)((s * t[1] + tt * t[5] + t[9] + t[13]) >> 12) / 16.0f;
        } else if ((teximg >> 30) == 0) { cur_s = raw_s / 16.0f; cur_t = raw_t / 16.0f; }
        /* modes 2/3: the raw value is only an offset, applied at NORMAL / VTX */
        break;
    case 0x23: submit_vertex((int16_t)(p[0] & 0xFFFF), (int16_t)(p[0] >> 16), (int16_t)(p[1] & 0xFFFF)); break;
    case 0x24: submit_vertex((int16_t)((p[0] & 0x3FF) << 6), (int16_t)(((p[0] >> 10) & 0x3FF) << 6), (int16_t)(((p[0] >> 20) & 0x3FF) << 6)); break;
    case 0x25: submit_vertex((int16_t)(p[0] & 0xFFFF), (int16_t)(p[0] >> 16), last_vz); break;
    case 0x26: submit_vertex((int16_t)(p[0] & 0xFFFF), last_vy, (int16_t)(p[0] >> 16)); break;
    case 0x27: submit_vertex(last_vx, (int16_t)(p[0] & 0xFFFF), (int16_t)(p[0] >> 16)); break;
    case 0x28: {
        int16_t dx = (int16_t)((p[0] & 0x3FF) << 6) >> 6, dy = (int16_t)(((p[0] >> 10) & 0x3FF) << 6) >> 6, dz = (int16_t)(((p[0] >> 20) & 0x3FF) << 6) >> 6;
        submit_vertex((int16_t)(last_vx + dx), (int16_t)(last_vy + dy), (int16_t)(last_vz + dz));
        break;
    }
    case 0x29: poly_attr_pending = p[0]; break;
    case 0x2A: teximg = p[0]; break;
    case 0x2B: pltt = p[0] & 0x1FFF; break;
    case 0x30:
        rgb15(p[0], &mat_dif[0], &mat_dif[1], &mat_dif[2]);
        rgb15(p[0] >> 16, &mat_amb[0], &mat_amb[1], &mat_amb[2]);
        if (p[0] & 0x8000) set_color15(p[0]);
        break;
    case 0x31:
        rgb15(p[0], &mat_spe[0], &mat_spe[1], &mat_spe[2]);
        rgb15(p[0] >> 16, &mat_emi[0], &mat_emi[1], &mat_emi[2]);
        use_shine = (p[0] >> 15) & 1;
        break;
    case 0x32: {
        int l = p[0] >> 30;
        float v[3];
        for (int i = 0; i < 3; i++) v[i] = (float)((int32_t)((p[0] >> (10 * i)) << 22) >> 22) / 512.0f;
        for (int j = 0; j < 3; j++) light_dir[l][j] = (v[0] * m_vec[0 * 4 + j] + v[1] * m_vec[1 * 4 + j] + v[2] * m_vec[2 * 4 + j]) / 4096.0f;
        break;
    }
    case 0x33: { int l = p[0] >> 30; rgb15(p[0], &light_col[l][0], &light_col[l][1], &light_col[l][2]); break; }
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) shine_table[i * 4 + k] = (uint8_t)(p[i] >> (8 * k)); (void)shine_pos; break;
    case 0x40: poly_attr = poly_attr_pending; prim_type = p[0] & 3; in_begin = 1; strip_n = strip_count = 0; break;
    case 0x41: in_begin = 0; break;
    case 0x50: g_swap_pending = 1; g_swap_flags = p[0] & 3; break;
    case 0x60: vp_x1 = p[0] & 0xFF; vp_y1 = (p[0] >> 8) & 0xFF; vp_x2 = (p[0] >> 16) & 0xFF; vp_y2 = (p[0] >> 24) & 0xFF; break;
    case 0x70: {                                                     /* BOX_TEST */
        update_clip();
        int16_t bx = (int16_t)(p[0] & 0xFFFF), by = (int16_t)(p[0] >> 16), bz = (int16_t)(p[1] & 0xFFFF);
        int16_t bw = (int16_t)(p[1] >> 16), bh = (int16_t)(p[2] & 0xFFFF), bd = (int16_t)(p[2] >> 16);
        int out_all[6] = { 1, 1, 1, 1, 1, 1 };
        for (int k = 0; k < 8; k++) {
            int64_t x = bx + ((k & 1) ? bw : 0), y = by + ((k & 2) ? bh : 0), z = bz + ((k & 4) ? bd : 0);
            int32_t* c = m_clip;
            int64_t cx = (x * c[0] + y * c[4] + z * c[8] + ((int64_t)c[12] << 12)) >> 12;
            int64_t cy = (x * c[1] + y * c[5] + z * c[9] + ((int64_t)c[13] << 12)) >> 12;
            int64_t cz = (x * c[2] + y * c[6] + z * c[10] + ((int64_t)c[14] << 12)) >> 12;
            int64_t cw = (x * c[3] + y * c[7] + z * c[11] + ((int64_t)c[15] << 12)) >> 12;
            if (cx >= -cw) out_all[0] = 0; if (cx <= cw) out_all[1] = 0;
            if (cy >= -cw) out_all[2] = 0; if (cy <= cw) out_all[3] = 0;
            if (cz >= -cw) out_all[4] = 0; if (cz <= cw) out_all[5] = 0;
        }
        box_result = !(out_all[0] || out_all[1] || out_all[2] || out_all[3] || out_all[4] || out_all[5]);
        break;
    }
    case 0x71: {                                                     /* POS_TEST */
        update_clip();
        last_vx = (int16_t)(p[0] & 0xFFFF); last_vy = (int16_t)(p[0] >> 16); last_vz = (int16_t)(p[1] & 0xFFFF);
        int64_t x = last_vx, y = last_vy, z = last_vz;
        for (int j = 0; j < 4; j++) {
            int32_t* c = m_clip;
            int32_t v = (int32_t)((x * c[j] + y * c[4 + j] + z * c[8 + j] + ((int64_t)c[12 + j] << 12)) >> 12);
            memcpy(&g_io[0x620 + 4 * j], &v, 4);
        }
        break;
    }
    case 0x72: {                                                     /* VEC_TEST */
        int64_t v[3];
        for (int i = 0; i < 3; i++) v[i] = ((int32_t)((p[0] >> (10 * i)) << 22) >> 22) << 3;
        for (int j = 0; j < 3; j++) {
            int16_t r = (int16_t)(((v[0] * m_vec[j] + v[1] * m_vec[4 + j] + v[2] * m_vec[8 + j]) >> 12));
            if (r & 0x1000) r |= (int16_t)0xF000;
            memcpy(&g_io[0x630 + 2 * j], &r, 2);
        }
        break;
    }
    default: break;
    }
}

/* Packed FIFO state */
static uint8_t fifo_cmds[4];
static int fifo_ncmd, fifo_idx;
static uint32_t fifo_params[32];
static int fifo_np;

static void fifo_run_zero_param(void) {
    while (fifo_idx < fifo_ncmd) {
        uint8_t c = fifo_cmds[fifo_idx];
        if (!cmd_valid(c)) { fifo_idx++; continue; }               /* NOP / unused */
        if (param_count[c] != 0) return;
        exec(c, fifo_params);
        fifo_idx++;
    }
}

static void fifo_write(uint32_t v) {
    if (fifo_idx >= fifo_ncmd) {                                    /* a new command word */
        fifo_ncmd = 0; fifo_idx = 0; fifo_np = 0;
        for (int i = 0; i < 4; i++) fifo_cmds[fifo_ncmd++] = (uint8_t)(v >> (8 * i));
        /* trailing zero bytes are padding */
        while (fifo_ncmd > 0 && fifo_cmds[fifo_ncmd - 1] == 0) fifo_ncmd--;
        fifo_run_zero_param();
        return;
    }
    uint8_t c = fifo_cmds[fifo_idx];
    fifo_params[fifo_np++] = v;
    if (fifo_np >= param_count[c]) {
        exec(c, fifo_params);
        fifo_np = 0;
        fifo_idx++;
        fifo_run_zero_param();
    }
}

/* Direct command ports */
static uint8_t port_cmd;
static uint32_t port_params[32];
static int port_np;

static void port_write(uint8_t cmd, uint32_t v) {
    if (!cmd_valid(cmd)) return;
    if (param_count[cmd] == 0) { exec(cmd, port_params); return; }
    if (port_cmd != cmd) { port_cmd = cmd; port_np = 0; }
    port_params[port_np++] = v;
    if (port_np >= param_count[cmd]) { exec(cmd, port_params); port_np = 0; port_cmd = 0; }
}

static int gxstat_irq_mode;

void gpu3d_init(void);
/* The GXFIFO interrupt is level-triggered: with our always-empty FIFO it is
 * asserted whenever an interrupt condition (less than half full / empty) is selected. */
int gx_irq_line(void) { return gxstat_irq_mode == 1 || gxstat_irq_mode == 2; }

void gx_write(uint32_t off, uint32_t v) {
    static int inited;
    if (!inited) { inited = 1; gpu3d_init(); }
    if (off >= 0x400 && off < 0x440) { fifo_write(v); return; }
    if (off >= 0x440 && off < 0x600) { port_write((uint8_t)(0x10 + (off - 0x440) / 4), v); return; }
    if (off == 0x600) {
        if (v & 0x8000) stack_overflow = 0;
        gxstat_irq_mode = (v >> 30) & 3;
        if (gxstat_irq_mode) irq_raise(&g_c9, IRQ_GXFIFO);          /* the FIFO is always empty */
    }
}

uint32_t gx_read(uint32_t off) {
    off &= ~3u;
    if (off == 0x600) {
        uint32_t v = 0;
        v |= (uint32_t)box_result << 1;
        v |= (uint32_t)(sp_pos & 31) << 8;
        v |= (uint32_t)(sp_proj & 1) << 13;
        v |= (uint32_t)stack_overflow << 15;
        v |= 1u << 25 | 1u << 26;                                    /* FIFO less than half full, empty */
        v |= (uint32_t)gxstat_irq_mode << 30;
        return v;
    }
    if (off == 0x604) return (uint32_t)g_npoly[g_geo] | ((uint32_t)g_nvert[g_geo] << 16);
    if (off >= 0x640 && off < 0x680) { update_clip(); return (uint32_t)m_clip[(off - 0x640) / 4]; }
    if (off >= 0x680 && off < 0x6A4) { int i = (off - 0x680) / 4; return (uint32_t)m_vec[(i / 3) * 4 + i % 3]; }
    uint32_t v; memcpy(&v, &g_io[off], 4); return v;
}

/* ---------------------------------------------------------------- rasterizer */
static uint16_t fb_color[192][256];
static uint8_t  fb_alpha[192][256];
static float    fb_depth[192][256];
static uint8_t  fb_id[192][256];        /* opaque polygon id */
static uint8_t  fb_tid[192][256];       /* translucent polygon id + 1 (0 = none) */
static uint8_t  fb_fog[192][256];
static uint8_t  fb_edge[192][256];      /* opaque pixel on its polygon's boundary (for edge marking) */

/* Edge lines of the polygon being drawn, normalised so the value is the
 * distance in pixels to that edge (positive inside). */
static float g_pe_a[MAX_PV], g_pe_b[MAX_PV], g_pe_c[MAX_PV];
static int g_pe_n;
static void poly_edges(const Poly* p) {
    float area = 0;
    for (int k = 0; k < p->n; k++) { const SVtx* u = &p->v[k]; const SVtx* v = &p->v[(k + 1) % p->n]; area += u->x * v->y - v->x * u->y; }
    float sg = area < 0 ? -1.0f : 1.0f;
    g_pe_n = 0;
    for (int k = 0; k < p->n; k++) {
        const SVtx* u = &p->v[k]; const SVtx* v = &p->v[(k + 1) % p->n];
        float a = (u->y - v->y) * -sg, b = (v->x - u->x) * -sg, len = sqrtf(a * a + b * b);
        if (len < 1e-6f) continue;
        g_pe_a[g_pe_n] = a / len; g_pe_b[g_pe_n] = b / len; g_pe_c[g_pe_n] = -(g_pe_a[g_pe_n] * u->x + g_pe_b[g_pe_n] * u->y);
        g_pe_n++;
    }
}
static int on_poly_edge(float px, float py) {
    for (int k = 0; k < g_pe_n; k++) {
        float d = g_pe_a[k] * px + g_pe_b[k] * py + g_pe_c[k];
        if (d < 1.0f && d > -1.0f) return 1;
    }
    return 0;
}
static int g_3d_have_frame;

static inline uint8_t tex8(uint32_t a) { uint8_t* p = g_tex_slot[(a >> 17) & 3]; return p ? p[a & 0x1FFFF] : 0; }
static inline uint16_t tex16(uint32_t a) { return (uint16_t)(tex8(a) | (tex8(a + 1) << 8)); }
static inline uint16_t pal16(uint32_t a) {
    uint32_t s = a >> 14;
    if (s >= 6) return 0;
    uint8_t* p = g_texpal_slot[s];
    return p ? (uint16_t)(p[a & 0x3FFF] | (p[(a & 0x3FFF) + 1] << 8)) : 0;
}

/* Fetch a texel: returns 15-bit color, alpha 0..31 in *a. */
static uint16_t texel(const Poly* p, int si, int ti, int* a) {
    uint32_t ti_ = p->teximg;
    int w = 8 << ((ti_ >> 20) & 7), h = 8 << ((ti_ >> 23) & 7);
    if (ti_ & (1 << 16)) {
        if (ti_ & (1 << 18)) { si &= 2 * w - 1; if (si >= w) si = 2 * w - 1 - si; } else si &= w - 1;
    } else si = si < 0 ? 0 : si >= w ? w - 1 : si;
    if (ti_ & (1 << 17)) {
        if (ti_ & (1 << 19)) { ti &= 2 * h - 1; if (ti >= h) ti = 2 * h - 1 - ti; } else ti &= h - 1;
    } else ti = ti < 0 ? 0 : ti >= h ? h - 1 : ti;
    uint32_t base = (ti_ & 0xFFFF) * 8;
    uint32_t pb = p->pltt * 16;
    int fmt = (ti_ >> 26) & 7;
    int c0t = (ti_ >> 29) & 1;
    uint32_t idx = (uint32_t)(ti * w + si);
    *a = 31;
    switch (fmt) {
    case 1: { uint8_t b = tex8(base + idx); int al = b >> 5; *a = al * 4 + al / 2; return pal16(pb + (b & 31) * 2); }
    case 2: { int i = (tex8(base + idx / 4) >> (2 * (idx & 3))) & 3; if (c0t && !i) *a = 0; return pal16(p->pltt * 8 + i * 2); }
    case 3: { int i = (tex8(base + idx / 2) >> (4 * (idx & 1))) & 15; if (c0t && !i) *a = 0; return pal16(pb + i * 2); }
    case 4: { int i = tex8(base + idx); if (c0t && !i) *a = 0; return pal16(pb + i * 2); }
    case 5: {
        uint32_t blk = (uint32_t)((ti / 4) * (w / 4) + si / 4);
        uint32_t ba = base + blk * 4;
        int i = (tex8(ba + (ti & 3)) >> (2 * (si & 3))) & 3;
        uint32_t ia = 0x20000 + ((ba & 0x1FFFF) >> 1) + (ba >= 0x40000 ? 0x10000 : 0);
        uint16_t info = tex16(ia);
        uint32_t pa = pb + (info & 0x3FFF) * 4;
        int mode = info >> 14;
        uint16_t c0 = pal16(pa), c1 = pal16(pa + 2);
        switch (i) {
        case 0: return c0;
        case 1: return c1;
        case 2:
            if (mode == 0 || mode == 2) return pal16(pa + 4);
            if (mode == 1) {
                int r = ((c0 & 31) + (c1 & 31)) / 2, g = (((c0 >> 5) & 31) + ((c1 >> 5) & 31)) / 2, b = (((c0 >> 10) & 31) + ((c1 >> 10) & 31)) / 2;
                return (uint16_t)(r | g << 5 | b << 10);
            } else {
                int r = ((c0 & 31) * 5 + (c1 & 31) * 3) / 8, g = (((c0 >> 5) & 31) * 5 + ((c1 >> 5) & 31) * 3) / 8, b = (((c0 >> 10) & 31) * 5 + ((c1 >> 10) & 31) * 3) / 8;
                return (uint16_t)(r | g << 5 | b << 10);
            }
        default:
            if (mode == 2) return pal16(pa + 6);
            if (mode == 3) {
                int r = ((c0 & 31) * 3 + (c1 & 31) * 5) / 8, g = (((c0 >> 5) & 31) * 3 + ((c1 >> 5) & 31) * 5) / 8, b = (((c0 >> 10) & 31) * 3 + ((c1 >> 10) & 31) * 5) / 8;
                return (uint16_t)(r | g << 5 | b << 10);
            }
            *a = 0; return 0;
        }
    }
    case 6: { uint8_t b = tex8(base + idx); *a = b >> 3; return pal16(pb + (b & 7) * 2); }
    case 7: { uint16_t c = tex16(base + idx * 2); *a = (c & 0x8000) ? 31 : 0; return c & 0x7FFF; }
    default: return 0x7FFF;
    }
}

static int clamp31(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }

static void draw_triangle(const Poly* p, const SVtx* a, const SVtx* b, const SVtx* c, int wbuf, uint16_t disp3d) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    if (area < 0) { const SVtx* t = b; b = c; c = t; area = -area; }
    int miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    int maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    int minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    int maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    if (miny < 0) miny = 0; if (maxy > 191) maxy = 191;
    if (minx < 0) minx = 0; if (maxx > 255) maxx = 255;
    const SVtx* V[3] = { a, b, c };
    /* edge k is opposite vertex k: from V[k+1] to V[k+2] */
    float A[3], B[3], C[3]; int tl[3];
    for (int k = 0; k < 3; k++) {
        const SVtx* u = V[(k + 1) % 3]; const SVtx* v = V[(k + 2) % 3];
        A[k] = u->y - v->y; B[k] = v->x - u->x; C[k] = u->x * v->y - v->x * u->y;
        tl[k] = (A[k] > 0) || (A[k] == 0 && B[k] < 0);
    }
    float inv = 1.0f / area;
    uint32_t attr = p->attr;
    int alpha_poly = (attr >> 16) & 31;
    int wire = alpha_poly == 0;
    if (wire) alpha_poly = 31;
    int mode = (attr >> 4) & 3;
    int fmt = (p->teximg >> 26) & 7;
    int textured = fmt != 0 && (disp3d & 1);
    int depth_eq = (attr >> 14) & 1;
    int id = (attr >> 24) & 63;
    int alpha_test = (disp3d >> 2) & 1, aref = IO16_(0x340) & 31;
    int blend = (disp3d >> 3) & 1;
    int highlight = (disp3d >> 1) & 1;
    int edge_mark = (disp3d >> 5) & 1;
    float elen[3];
    if (wire) for (int k = 0; k < 3; k++) elen[k] = sqrtf(A[k] * A[k] + B[k] * B[k]);
    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f;
        /* this row's span from the three edges (one pixel of margin; the exact
         * test below still decides every pixel) */
        float lo = (float)minx, hi = (float)maxx;
        int empty = 0;
        for (int k = 0; k < 3; k++) {
            float r = B[k] * py + C[k];
            if (A[k] > 0) { float b = -r / A[k] - 0.5f; if (b > lo) lo = b; }
            else if (A[k] < 0) { float b = -r / A[k] - 0.5f; if (b < hi) hi = b; }
            else if (r < 0) { empty = 1; break; }
        }
        if (empty || lo > hi + 2) continue;
        int x0 = (int)floorf(lo) - 1, x1 = (int)ceilf(hi) + 1;
        if (x0 < minx) x0 = minx;
        if (x1 > maxx) x1 = maxx;
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float e[3]; int in = 1;
            for (int k = 0; k < 3; k++) {
                e[k] = A[k] * px + B[k] * py + C[k];
                if (e[k] < 0 || (e[k] == 0 && !tl[k])) { in = 0; break; }
            }
            if (!in) continue;
            if (wire) {
                int near_edge = 0;
                for (int k = 0; k < 3; k++) if (elen[k] > 0 && e[k] / elen[k] < 1.0f) near_edge = 1;
                if (!near_edge) continue;
            }
            float l0 = e[0] * inv, l1 = e[1] * inv, l2 = e[2] * inv;
            float iw = l0 * a->iw + l1 * b->iw + l2 * c->iw;
            float depth = wbuf ? 1.0f / iw : (l0 * a->zn + l1 * b->zn + l2 * c->zn);
            float dst = fb_depth[y][x];
            if (depth_eq) { if (fabsf(depth - dst) > (wbuf ? 0.01f : 0x200 / 16777215.0f)) continue; }
            else if (!(depth < dst)) continue;
            float k0 = l0 * a->iw / iw, k1 = l1 * b->iw / iw, k2 = l2 * c->iw / iw;
            int vr = clamp31((int)(k0 * a->r + k1 * b->r + k2 * c->r + 0.5f));
            int vg = clamp31((int)(k0 * a->g + k1 * b->g + k2 * c->g + 0.5f));
            int vb = clamp31((int)(k0 * a->b + k1 * b->b + k2 * c->b + 0.5f));
            int r, g, bl, al;
            if (mode == 2) {                                           /* toon / highlight */
                uint16_t tc = IO16_(0x380 + (vr & 31) * 2);
                if (!highlight) { vr = tc & 31; vg = (tc >> 5) & 31; vb = (tc >> 10) & 31; }
                else { vg = vb = vr; }
                if (textured) { (void)0; }
                (void)tc;
            }
            if (textured) {
                float s = k0 * a->s + k1 * b->s + k2 * c->s;
                float t = k0 * a->t + k1 * b->t + k2 * c->t;
                int ta;
                uint16_t tcol = texel(p, (int)floorf(s), (int)floorf(t), &ta);
                int tr = tcol & 31, tg = (tcol >> 5) & 31, tb = (tcol >> 10) & 31;
                if (mode == 1) {                                       /* decal */
                    r = (tr * ta + vr * (31 - ta)) / 31; g = (tg * ta + vg * (31 - ta)) / 31; bl = (tb * ta + vb * (31 - ta)) / 31;
                    al = alpha_poly;
                } else {                                               /* modulate (also toon/highlight) */
                    r = ((tr + 1) * (vr + 1) - 1) >> 5; g = ((tg + 1) * (vg + 1) - 1) >> 5; bl = ((tb + 1) * (vb + 1) - 1) >> 5;
                    al = ((ta + 1) * (alpha_poly + 1) - 1) >> 5;
                }
            } else { r = vr; g = vg; bl = vb; al = alpha_poly; }
            if (mode == 2 && highlight) {
                uint16_t tc = IO16_(0x380 + (vr & 31) * 2);
                r = clamp31(r + (tc & 31)); g = clamp31(g + ((tc >> 5) & 31)); bl = clamp31(bl + ((tc >> 10) & 31));
            }
            if (al == 0) continue;
            if (alpha_test && al <= aref) continue;
            uint16_t col = (uint16_t)(r | g << 5 | bl << 10);
            if (al >= 31) {
                fb_color[y][x] = col; fb_alpha[y][x] = 31; fb_depth[y][x] = depth;
                fb_id[y][x] = (uint8_t)id; fb_tid[y][x] = 0; fb_fog[y][x] = (attr >> 15) & 1;
                fb_edge[y][x] = edge_mark ? (uint8_t)on_poly_edge(px, py) : 0;
            } else {
                if (fb_tid[y][x] == id + 1) continue;                  /* same translucent polygon id */
                int da = fb_alpha[y][x];
                if (blend && da) {
                    uint16_t d = fb_color[y][x];
                    int dr = d & 31, dg = (d >> 5) & 31, db = (d >> 10) & 31;
                    r = (r * (al + 1) + dr * (31 - al)) / 32; g = (g * (al + 1) + dg * (31 - al)) / 32; bl = (bl * (al + 1) + db * (31 - al)) / 32;
                    col = (uint16_t)(r | g << 5 | bl << 10);
                    fb_alpha[y][x] = (uint8_t)(al > da ? al : da);
                } else fb_alpha[y][x] = (uint8_t)al;
                fb_color[y][x] = col;
                fb_tid[y][x] = (uint8_t)(id + 1);
                fb_fog[y][x] &= (attr >> 15) & 1;
                if (attr & 0x800) fb_depth[y][x] = depth;
            }
        }
    }
}

static int poly_cmp_y(const void* x, const void* y) {
    const Poly* a = *(const Poly* const*)x; const Poly* b = *(const Poly* const*)y;
    if (a->ymax != b->ymax) return a->ymax < b->ymax ? -1 : 1;
    if (a->ymin != b->ymin) return a->ymin < b->ymin ? -1 : 1;
    return a < b ? -1 : a > b ? 1 : 0;
}

double g_t3d_part[4];
static void render_frame(int buf) {
    double tA = ms_now();
    uint16_t disp3d = IO16_(0x060);
    uint32_t clear = IO32_(0x350);
    uint16_t cdepth = IO16_(0x354) & 0x7FFF;
    int wbuf = (g_render_flags >> 1) & 1;
    float clear_d;
    uint32_t d24 = (uint32_t)cdepth * 0x200 + ((cdepth + 1) / 0x8000) * 0x1FF;
    if (cdepth == 0x7FFF) clear_d = INFINITY;
    else clear_d = wbuf ? d24 / 4096.0f : d24 / 16777215.0f;
    uint16_t ccol = clear & 0x7FFF;
    uint8_t calpha = (clear >> 16) & 31, cid = (clear >> 24) & 63, cfog = (clear >> 15) & 1;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) {
            fb_color[y][x] = ccol; fb_alpha[y][x] = calpha; fb_depth[y][x] = clear_d;
            fb_id[y][x] = cid; fb_tid[y][x] = 0; fb_fog[y][x] = cfog; fb_edge[y][x] = 0;
        }
    double tB = ms_now(); g_t3d_part[0] += tB - tA;
    int n = g_npoly[buf];
    static const Poly* order[MAX_POLY];
    int no = 0;
    for (int i = 0; i < n; i++) if (!g_polybuf[buf][i].translucent) order[no++] = &g_polybuf[buf][i];
    qsort(order, no, sizeof order[0], poly_cmp_y);
    int nt = no;
    for (int i = 0; i < n; i++) if (g_polybuf[buf][i].translucent) order[nt++] = &g_polybuf[buf][i];
    if (!(g_render_flags & 1)) qsort(order + no, nt - no, sizeof order[0], poly_cmp_y);
    for (int i = 0; i < nt; i++) {
        const Poly* p = order[i];
        poly_edges(p);
        for (int k = 1; k + 1 < p->n; k++) draw_triangle(p, &p->v[0], &p->v[k], &p->v[k + 1], wbuf, disp3d);
    }
    double tC = ms_now(); g_t3d_part[1] += tC - tB;
    /* edge marking */
    if (disp3d & 0x20) {
        static uint16_t out[192][256];
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                out[y][x] = fb_color[y][x];
                if (fb_alpha[y][x] == 0 || !fb_edge[y][x]) continue;
                uint8_t id = fb_id[y][x]; float d = fb_depth[y][x];
                static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || nx > 255 || ny < 0 || ny > 191) continue;
                    if (fb_id[ny][nx] != id && d < fb_depth[ny][nx]) { out[y][x] = IO16_(0x330 + (id >> 3) * 2) & 0x7FFF; break; }
                }
            }
        for (int y = 0; y < 192; y++) memcpy(fb_color[y], out[y], sizeof out[y]);
    }
    double tD = ms_now(); g_t3d_part[2] += tD - tC;
    /* fog */
    if (disp3d & 0x80) {
        uint16_t fcol = IO16_(0x358) & 0x7FFF;
        int falpha = g_io[0x35A] & 31;
        int foff = IO16_(0x35C) & 0x7FFF;
        int shift = (disp3d >> 8) & 15;
        int alpha_only = (disp3d >> 6) & 1;
        int step = 0x400 >> shift;
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++) {
                if (!fb_fog[y][x]) continue;
                float d = fb_depth[y][x];
                if (isinf(d)) d = 1.0f;
                int z15 = wbuf ? (int)(d * 4096.0f) >> 9 : (int)(d * 16777215.0f) >> 9;
                int density;
                if (step <= 0) density = (g_io[0x360 + 31] & 127);
                else {
                    int i = (z15 - foff) / step;
                    if (i < 0) density = g_io[0x360] & 127;
                    else if (i >= 31) density = g_io[0x360 + 31] & 127;
                    else {
                        int d0 = g_io[0x360 + i] & 127, d1 = g_io[0x360 + i + 1] & 127;
                        int frac = (z15 - foff) - i * step;
                        density = d0 + (d1 - d0) * frac / step;
                    }
                }
                if (density >= 127) density = 128;
                uint16_t c = fb_color[y][x];
                if (!alpha_only) {
                    int r = ((fcol & 31) * density + (c & 31) * (128 - density)) >> 7;
                    int g = (((fcol >> 5) & 31) * density + ((c >> 5) & 31) * (128 - density)) >> 7;
                    int b = (((fcol >> 10) & 31) * density + ((c >> 10) & 31) * (128 - density)) >> 7;
                    fb_color[y][x] = (uint16_t)(r | g << 5 | b << 10);
                }
                fb_alpha[y][x] = (uint8_t)((falpha * density + fb_alpha[y][x] * (128 - density)) >> 7);
            }
    }
    g_t3d_part[3] += ms_now() - tD;
    g_3d_have_frame = 1;
    /* DQIX_POLYDUMP="frame,x,y": list the polygons covering pixel (x,y) */
    const char* pd = getenv("DQIX_POLYDUMP");
    unsigned long long pf; int px, py;
    if (pd && sscanf(pd, "%llu,%d,%d", &pf, &px, &py) == 3 && g_frame >= pf && g_frame < pf + 1) {
        fprintf(stderr, "VRAMCNT A-G: %02x %02x %02x %02x %02x %02x %02x\n", g_io[0x240], g_io[0x241], g_io[0x242], g_io[0x243], g_io[0x244], g_io[0x245], g_io[0x246]);
        fprintf(stderr, "3D frame %llu: %d polygons, disp3dcnt %04x, pixel (%d,%d) = %04x a%d\n", (unsigned long long)g_frame, n, disp3d, px, py,
                fb_color[py][px], fb_alpha[py][px]);
        for (int i = 0; i < nt; i++) {
            const Poly* p = order[i];
            int inside = 1; float sgn = 0;
            for (int k = 0; k < p->n; k++) {
                const SVtx* u = &p->v[k]; const SVtx* v = &p->v[(k + 1) % p->n];
                float e = (v->x - u->x) * (py + 0.5f - u->y) - (v->y - u->y) * (px + 0.5f - u->x);
                if (e != 0) { if (sgn == 0) sgn = e; else if ((e > 0) != (sgn > 0)) { inside = 0; break; } }
            }
            if (!inside) continue;
            fprintf(stderr, "  poly %d: attr %08x teximg %08x (fmt %u, %ux%u, mode %u) pltt %04x, %d verts\n", (int)(p - g_polybuf[buf]), p->attr, p->teximg,
                    (p->teximg >> 26) & 7, 8u << ((p->teximg >> 20) & 7), 8u << ((p->teximg >> 23) & 7), p->teximg >> 30, p->pltt, p->n);
            if (((p->teximg >> 26) & 7) == 3 && ((p->teximg >> 20) & 7) <= 2 && ((p->teximg >> 23) & 7) <= 2) {
                int w = 8 << ((p->teximg >> 20) & 7), h = 8 << ((p->teximg >> 23) & 7);
                uint32_t base = (p->teximg & 0xFFFF) * 8;
                for (int y = 0; y < h; y++) {
                    fprintf(stderr, "     ");
                    for (int x = 0; x < w; x++) { int i = y * w + x; fprintf(stderr, "%x", (tex8(base + i / 2) >> (4 * (i & 1))) & 15); }
                    fprintf(stderr, "\n");
                }
            }
            fprintf(stderr, "     palette:");
            for (int k = 0; k < 16; k++) fprintf(stderr, " %04x", pal16(p->pltt * 16 + k * 2));
            fprintf(stderr, "\n");
            for (int k = 0; k < p->n; k++)
                fprintf(stderr, "     v%d: (%.1f,%.1f) z %.4f rgb %.1f,%.1f,%.1f st %.2f,%.2f\n", k, p->v[k].x, p->v[k].y, p->v[k].zn, p->v[k].r, p->v[k].g, p->v[k].b, p->v[k].s, p->v[k].t);
        }
    }
}

/* VBlank: SWAP_BUFFERS takes effect, the new list is rendered. */
double g_t3d_ms;
void gpu3d_vblank(void) {
    if (gxstat_irq_mode) irq_raise(&g_c9, IRQ_GXFIFO);
    if (!g_swap_pending) return;
    g_swap_pending = 0;
    g_render_flags = g_swap_flags;
    int buf = g_geo;
    g_geo ^= 1;
    g_npoly[g_geo] = 0; g_nvert[g_geo] = 0;
    double t0 = ms_now();
    render_frame(buf);
    g_t3d_ms += ms_now() - t0;
}

extern uint16_t g_3d_line[256];
extern uint8_t g_3d_alpha[256];
extern int g_3d_present;

void gpu3d_render_line(int line) {
    if (!g_3d_have_frame) { g_3d_present = 0; return; }
    g_3d_present = 1;
    memcpy(g_3d_line, fb_color[line], sizeof g_3d_line);
    memcpy(g_3d_alpha, fb_alpha[line], sizeof g_3d_alpha);
}

void gpu3d_init(void) {
    mtx_identity(m_proj); mtx_identity(m_pos); mtx_identity(m_vec); mtx_identity(m_tex);
    clip_dirty = 1;
}
