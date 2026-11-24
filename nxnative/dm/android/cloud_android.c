// Android port: cloud saves (cloud_android.h). The library server keeps the newest 5 saves of each game (apps/server/src/saves/cloud-store.ts); this
// device talks to it over HTTPS (Tailscale), with java.net.HttpURLConnection through JNI: framework classes, so the APK still carries no code of its own.
//   POST <url>/status  {"localHash","baseHash"}       → {"state","latest"}      where the save stands
//   POST <url>?platform=android&device=<model>  tar     → {"save","created"}      store this device's save (the server reads and hashes the archive)
//   GET  <url>/<id>                                     → tar                      a cloud save
// A save travels as a plain ustar archive of save/ (the game's save, $DM_SAVE) and save_device/ ($DM_SAVE_device), the server's format. The sync base, the
// cloud save this device last uploaded or downloaded, is saves/cloud.txt (id= and hash= lines); a download's waits in saves/cloud-pending.txt until its
// import at the next start succeeds (saves_android.c)
#include "cloud_android.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef DM_CLOUD_HOST_TEST
#include "dm.h"
#include "saves_android.h"
#include <jni.h>
uint64_t dm_fs_commit_count(void);   // hle_fs.c: commits so far
#else
#define LOG(...) ((void)0)
#endif

// ---- the save's files: both trees, sorted by path (strcmp: byte order, as the server's Buffer.compare) ----
typedef struct { char *rel, *abs; int dir; long long size; } cl_ent;
typedef struct { cl_ent *e; int n, cap; } cl_list;
static int cl_add(cl_list *l, const char *rel, const char *abs, int dir, long long size) {
    if (l->n == l->cap) { int c = l->cap ? 2 * l->cap : 64; cl_ent *e = realloc(l->e, (size_t)c * sizeof *e); if (!e) return -ENOMEM; l->e = e; l->cap = c; }
    cl_ent *x = &l->e[l->n]; x->rel = strdup(rel); x->abs = strdup(abs); x->dir = dir; x->size = size;
    if (!x->rel || !x->abs) { free(x->rel); free(x->abs); return -ENOMEM; }
    l->n++; return 0;
}
static void cl_free(cl_list *l) { for (int i = 0; i < l->n; i++) { free(l->e[i].rel); free(l->e[i].abs); } free(l->e); memset(l, 0, sizeof *l); }
static int cl_walk(cl_list *l, const char *abs, const char *rel) {
    DIR *d = opendir(abs); if (!d) return -errno;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1200], q[1200]; struct stat st; snprintf(a, sizeof a, "%s/%s", abs, e->d_name); snprintf(q, sizeof q, "%s/%s", rel, e->d_name);
        if (lstat(a, &st)) { r = -errno; break; }
        if (S_ISDIR(st.st_mode)) { char qd[1210]; snprintf(qd, sizeof qd, "%s/", q); if (!(r = cl_add(l, qd, a, 1, 0))) r = cl_walk(l, a, q); }
        else if (S_ISREG(st.st_mode)) r = cl_add(l, q, a, 0, st.st_size);
    }
    closedir(d); return r;
}
static int cl_cmp(const void *a, const void *b) { return strcmp(((const cl_ent *)a)->rel, ((const cl_ent *)b)->rel); }
static int cl_collect(cl_list *l, const char *save, const char *dev) {
    const char *abs[2] = {save, dev}, *rel[2] = {"save", "save_device"};
    for (int k = 0; k < 2; k++) {
        struct stat st; if (!abs[k] || !abs[k][0] || stat(abs[k], &st) || !S_ISDIR(st.st_mode)) continue;
        char top[16]; snprintf(top, sizeof top, "%s/", rel[k]);
        int r = cl_add(l, top, abs[k], 1, 0); if (!r) r = cl_walk(l, abs[k], rel[k]);
        if (r) return r;
    }
    qsort(l->e, (size_t)l->n, sizeof *l->e, cl_cmp); return 0;
}
static int read_all(const char *p, uint8_t **out, size_t *len) {
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -errno;
    struct stat st; if (fstat(fd, &st)) { int e = errno; close(fd); return -e; }
    uint8_t *b = malloc(st.st_size ? (size_t)st.st_size : 1); size_t n = 0; if (!b) { close(fd); return -ENOMEM; }
    while (n < (size_t)st.st_size) { ssize_t k = read(fd, b + n, (size_t)st.st_size - n); if (k < 0) { if (errno == EINTR) continue; int e = errno; free(b); close(fd); return -e; } if (!k) break; n += (size_t)k; }
    close(fd); *out = b; *len = n; return 0;
}
static void hex32(const uint8_t md[32], char hex[65]) { for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]); hex[64] = 0; }

int dm_cloud_hash_tree(const char *save, const char *dev, char hex[65], long *files, long long *bytes) {
    cl_list l = {0}; int r = cl_collect(&l, save, dev); hex[0] = 0; if (files) *files = 0; if (bytes) *bytes = 0;
    CC_SHA256_CTX c; CC_SHA256_Init(&c); int any = 0;
    for (int i = 0; !r && i < l.n; i++) {
        if (l.e[i].dir) continue;
        uint8_t *b; size_t n; if ((r = read_all(l.e[i].abs, &b, &n))) break;
        char sz[32]; int sl = snprintf(sz, sizeof sz, "%zu", n);
        CC_SHA256_Update(&c, l.e[i].rel, (CC_LONG)strlen(l.e[i].rel)); CC_SHA256_Update(&c, "", 1);
        CC_SHA256_Update(&c, sz, (CC_LONG)sl); CC_SHA256_Update(&c, "", 1);
        if (n) CC_SHA256_Update(&c, b, (CC_LONG)n);
        free(b); any = 1; if (files) ++*files; if (bytes) *bytes += (long long)n;
    }
    cl_free(&l);
    if (r) return r;
    if (any) { uint8_t md[32]; CC_SHA256_Final(md, &c); hex32(md, hex); }
    return 0;
}

// ---- ustar ----
static void oct(uint8_t *h, int off, int len, unsigned long long v) { char t[24]; snprintf(t, sizeof t, "%0*llo", len - 1, v); memcpy(h + off, t, (size_t)len - 1); h[off + len - 1] = 0; }
static int split_name(const char *p, const char **name, char *prefix) {   // ustar: name ≤ 100 bytes, or prefix ≤ 155 + '/' + name ≤ 100
    size_t L = strlen(p); prefix[0] = 0; *name = p;
    if (L <= 100) return 0;
    for (size_t i = L - 1; i > 0; i--) if (p[i] == '/' && i <= 155 && L - i - 1 <= 100 && L - i - 1 > 0) { memcpy(prefix, p, i); prefix[i] = 0; *name = p + i + 1; return 0; }
    return -ENAMETOOLONG;
}
typedef struct { uint8_t *b; size_t n, cap; } cl_buf;
static int buf_put(cl_buf *o, const void *p, size_t n) {
    if (o->n + n > o->cap) { size_t c = o->cap ? o->cap : 1 << 16; while (c < o->n + n) c *= 2; uint8_t *b = realloc(o->b, c); if (!b) return -ENOMEM; o->b = b; o->cap = c; }
    if (p) memcpy(o->b + o->n, p, n); else memset(o->b + o->n, 0, n);
    o->n += n; return 0;
}
int dm_cloud_tar(const char *save, const char *dev, uint8_t **out, size_t *len, char *err, int ecap) {
    cl_list l = {0}; cl_buf o = {0}; int r = cl_collect(&l, save, dev); long mt = (long)time(NULL);
    if (r) snprintf(err, (size_t)ecap, "reading the save: %s", strerror(-r));
    for (int i = 0; !r && i < l.n; i++) {
        uint8_t h[512] = {0}, *data = NULL; size_t n = 0; const char *name; char prefix[160];
        if (split_name(l.e[i].rel, &name, prefix)) { r = -ENAMETOOLONG; snprintf(err, (size_t)ecap, "a path too long for the archive: %s", l.e[i].rel); break; }
        if (!l.e[i].dir && (r = read_all(l.e[i].abs, &data, &n))) { snprintf(err, (size_t)ecap, "reading %s: %s", l.e[i].rel, strerror(-r)); break; }
        memcpy(h, name, strlen(name)); oct(h, 100, 8, l.e[i].dir ? 0755 : 0644); oct(h, 108, 8, 0); oct(h, 116, 8, 0); oct(h, 124, 12, n); oct(h, 136, 12, (unsigned long long)mt);
        memset(h + 148, ' ', 8); h[156] = l.e[i].dir ? '5' : '0'; memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2); memcpy(h + 345, prefix, strlen(prefix));
        unsigned sum = 0; for (int k = 0; k < 512; k++) sum += h[k];
        char cs[8]; snprintf(cs, sizeof cs, "%06o", sum); memcpy(h + 148, cs, 6); h[154] = 0; h[155] = ' ';
        r = buf_put(&o, h, 512);
        if (!r && n) r = buf_put(&o, data, n);
        if (!r && n % 512) r = buf_put(&o, NULL, 512 - n % 512);
        free(data);
        if (r) snprintf(err, (size_t)ecap, "out of memory for the archive");
    }
    if (!r && (r = buf_put(&o, NULL, 1024))) snprintf(err, (size_t)ecap, "out of memory for the archive");
    cl_free(&l);
    if (r) { free(o.b); return r; }
    *out = o.b; *len = o.n; return 0;
}
static int mkdirs(const char *p) {   // p and its parents
    char t[1200]; snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(t, 0770) && errno != EEXIST) return -errno; *s = '/'; }
    return mkdir(t, 0770) && errno != EEXIST ? -errno : 0;
}
static size_t field_len(const uint8_t *f, size_t cap) { size_t n = 0; while (n < cap && f[n]) n++; return n; }
int dm_cloud_untar(const uint8_t *buf, size_t len, const char *dest_save, const char *dest_dev, int *has_dev, char *err, int ecap) {
    *has_dev = 0; int files = 0;
    for (size_t off = 0; off + 512 <= len;) {
        const uint8_t *h = buf + off; int zero = 1; for (int k = 0; k < 512 && zero; k++) zero = !h[k];
        if (zero) break;
        unsigned sum = 0; for (int k = 0; k < 512; k++) sum += k >= 148 && k < 156 ? ' ' : h[k];
        char t[16] = {0}; memcpy(t, h + 148, 8); if (strtoul(t, NULL, 8) != sum) { snprintf(err, (size_t)ecap, "not a save archive (bad header)"); return -EINVAL; }
        char path[300] = {0}; size_t pl = field_len(h + 345, 155), nl = field_len(h, 100);
        if (!memcmp(h + 257, "ustar", 5) && pl) { memcpy(path, h + 345, pl); path[pl] = '/'; memcpy(path + pl + 1, h, nl); } else memcpy(path, h, nl);
        memset(t, 0, sizeof t); memcpy(t, h + 124, 12); unsigned long long size = strtoull(t, NULL, 8);
        char type = (char)h[156];
        off += 512;
        if (size > len - off) { snprintf(err, (size_t)ecap, "the save archive is cut short"); return -EINVAL; }
        size_t L = strlen(path); int dir = type == '5' || (L && path[L - 1] == '/');
        if (!dir && type != '0' && type != 0) { snprintf(err, (size_t)ecap, "the save archive holds an unsupported entry (%s)", path); return -EINVAL; }
        while (L && path[L - 1] == '/') path[--L] = 0;
        const char *rest = NULL, *dest = NULL;
        if (!strncmp(path, "save_device", 11) && (!path[11] || path[11] == '/')) { dest = dest_dev; rest = path + 11; *has_dev = 1; }
        else if (!strncmp(path, "save", 4) && (!path[4] || path[4] == '/')) { dest = dest_save; rest = path + 4; }
        if (!dest || strstr(path, "/../") || strstr(path, "/./") || (L >= 3 && !strcmp(path + L - 3, "/..")) || path[0] == '/') {
            snprintf(err, (size_t)ecap, "the save archive has a path outside save/ and save_device/: %s", path); return -EINVAL; }
        char abs[1200]; snprintf(abs, sizeof abs, "%s%s", dest, rest);
        if (dir) { int r = mkdirs(abs); if (r) { snprintf(err, (size_t)ecap, "%s: %s", abs, strerror(-r)); return r; } }
        else {
            char parent[1200]; snprintf(parent, sizeof parent, "%s", abs); char *sl = strrchr(parent, '/'); if (sl) { *sl = 0; int r = mkdirs(parent); if (r) { snprintf(err, (size_t)ecap, "%s: %s", parent, strerror(-r)); return r; } }
            int fd = open(abs, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660); if (fd < 0) { int e = errno; snprintf(err, (size_t)ecap, "%s: %s", abs, strerror(e)); return -e; }
            for (size_t w = 0; w < size;) { ssize_t k = write(fd, buf + off + w, (size_t)(size - w)); if (k < 0) { if (errno == EINTR) continue; int e = errno; close(fd); snprintf(err, (size_t)ecap, "%s: %s", abs, strerror(e)); return -e; } w += (size_t)k; }
            if (fsync(fd) || close(fd)) { int e = errno; snprintf(err, (size_t)ecap, "%s: %s", abs, strerror(e)); return -e; }
            files++;
        }
        off += (size_t)((size + 511) / 512 * 512);
    }
    if (!files) { snprintf(err, (size_t)ecap, "the cloud save holds no files"); return -ENOENT; }
    return 0;
}

int dm_cloud_decide(const char *local, const char *base, const char *cloud) {   // the library's decideSync
    int L = local && *local, B = base && *base, C = cloud && *cloud;
    if (!L && !C) return DMC_BOTH_EMPTY;
    if (!C) return DMC_CLOUD_EMPTY;
    if (!L) return DMC_LOCAL_EMPTY;
    if (!strcmp(local, cloud)) return DMC_IN_SYNC;
    if (B && !strcmp(cloud, base)) return DMC_LOCAL_NEWER;
    if (B && !strcmp(local, base)) return DMC_CLOUD_NEWER;
    return DMC_CONFLICT;
}

int dm_cloud_json(const char *json, const char *key, char *out, int cap) {
    char pat[80]; snprintf(pat, sizeof pat, "\"%s\":", key); out[0] = 0;
    const char *p = json ? strstr(json, pat) : NULL; if (!p) return -1;
    p += strlen(pat); while (*p == ' ') p++;
    int n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && n < cap - 1; p++) {
            if (*p == '\\' && p[1]) { p++; char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                if (*p == 'u') { unsigned u = 0; for (int k = 1; k <= 4 && p[k]; k++) u = u * 16 + (unsigned)(p[k] <= '9' ? p[k] - '0' : (p[k] | 32) - 'a' + 10); p += 4;
                    if (u < 0x80) c = (char)u; else if (u < 0x800 && n < cap - 2) { out[n++] = (char)(0xC0 | u >> 6); c = (char)(0x80 | (u & 63)); }
                    else if (n < cap - 3) { out[n++] = (char)(0xE0 | u >> 12); out[n++] = (char)(0x80 | (u >> 6 & 63)); c = (char)(0x80 | (u & 63)); } else c = '?'; }
                out[n++] = c; }
            else out[n++] = *p;
        }
    } else while (*p && *p != ',' && *p != '}' && *p != ']' && n < cap - 1) out[n++] = *p++;
    out[n] = 0; return 0;
}

#ifndef DM_CLOUD_HOST_TEST
// ---- the device side ----
static JavaVM *g_cvm;
void dm_cloud_start(void *vm) { g_cvm = vm; const char *u = dm_cloud_url(); if (u) LOG("[cloud] cloud saves: %s", u); }
const char *dm_cloud_url(void) { const char *u = getenv("DM_CLOUD_URL"); return u && (!strncmp(u, "https://", 8) || !strncmp(u, "http://", 7)) ? u : NULL; }

static int jx(JNIEnv *E) { if ((*E)->ExceptionCheck(E)) { (*E)->ExceptionClear(E); return 1; } return 0; }
// an exception's message (the Java side's reason: "Unable to resolve host …", "timeout"), then cleared
static void jx_msg(JNIEnv *E, char *err, int ecap, const char *what) {
    jthrowable t = (*E)->ExceptionOccurred(E); (*E)->ExceptionClear(E);
    char m[200] = "";
    if (t) { jclass c = (*E)->GetObjectClass(E, t); jmethodID ts = (*E)->GetMethodID(E, c, "toString", "()Ljava/lang/String;");
        jstring s = ts ? (*E)->CallObjectMethod(E, t, ts) : NULL; if (jx(E)) s = NULL;
        if (s) { const char *u = (*E)->GetStringUTFChars(E, s, NULL); if (u) { snprintf(m, sizeof m, "%s", u); (*E)->ReleaseStringUTFChars(E, s, u); } (*E)->DeleteLocalRef(E, s); }
        (*E)->DeleteLocalRef(E, c); (*E)->DeleteLocalRef(E, t); }
    snprintf(err, (size_t)ecap, "%s: %s", what, m[0] ? m : "failed");
}
static JNIEnv *attach(int *did) {
    JNIEnv *E = NULL; *did = 0; if (!g_cvm) return NULL;
    if ((*g_cvm)->GetEnv(g_cvm, (void **)&E, JNI_VERSION_1_6) == JNI_OK) return E;
    if ((*g_cvm)->AttachCurrentThread(g_cvm, &E, NULL) != JNI_OK) return NULL;
    *did = 1; return E;
}
static void model(char *out, int cap) {   // android.os.Build.MODEL ("2511FPC34G"), the device's name in the cloud's list
    snprintf(out, (size_t)cap, "Android"); int did; JNIEnv *E = attach(&did); if (!E) return;
    jclass b = (*E)->FindClass(E, "android/os/Build"); jfieldID f = b ? (*E)->GetStaticFieldID(E, b, "MODEL", "Ljava/lang/String;") : NULL;
    jstring s = f ? (*E)->GetStaticObjectField(E, b, f) : NULL; if (jx(E)) s = NULL;
    if (s) { const char *u = (*E)->GetStringUTFChars(E, s, NULL); if (u) { snprintf(out, (size_t)cap, "%s", u); (*E)->ReleaseStringUTFChars(E, s, u); } (*E)->DeleteLocalRef(E, s); }
    if (b) (*E)->DeleteLocalRef(E, b);
    if (did) (*g_cvm)->DetachCurrentThread(g_cvm);
}

// One HTTP request: 0 and the response's status and body (malloc'd, NUL-terminated), or -1 and why. ctype NULL: no body
static int http(const char *method, const char *url, const char *ctype, const void *body, size_t blen, int *status, uint8_t **resp, size_t *rlen, char *err, int ecap) {
    *resp = NULL; *rlen = 0; *status = 0;
    int did; JNIEnv *E = attach(&did); if (!E) { snprintf(err, (size_t)ecap, "no Java VM for the network"); return -1; }
    if ((*E)->PushLocalFrame(E, 32)) { jx(E); if (did) (*g_cvm)->DetachCurrentThread(g_cvm); snprintf(err, (size_t)ecap, "out of memory"); return -1; }
    int r = -1; jobject conn = NULL; cl_buf o = {0};
    jclass cURL = (*E)->FindClass(E, "java/net/URL"), cH = (*E)->FindClass(E, "java/net/HttpURLConnection"), cOut = (*E)->FindClass(E, "java/io/OutputStream"), cIn = (*E)->FindClass(E, "java/io/InputStream");
    if (jx(E) || !cURL || !cH || !cOut || !cIn) { snprintf(err, (size_t)ecap, "java.net is missing"); goto done; }
    jmethodID mNew = (*E)->GetMethodID(E, cURL, "<init>", "(Ljava/lang/String;)V"), mOpen = (*E)->GetMethodID(E, cURL, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID mMethod = (*E)->GetMethodID(E, cH, "setRequestMethod", "(Ljava/lang/String;)V"), mCT = (*E)->GetMethodID(E, cH, "setConnectTimeout", "(I)V"), mRT = (*E)->GetMethodID(E, cH, "setReadTimeout", "(I)V");
    jmethodID mDo = (*E)->GetMethodID(E, cH, "setDoOutput", "(Z)V"), mProp = (*E)->GetMethodID(E, cH, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V"), mFixed = (*E)->GetMethodID(E, cH, "setFixedLengthStreamingMode", "(I)V");
    jmethodID mGetOut = (*E)->GetMethodID(E, cH, "getOutputStream", "()Ljava/io/OutputStream;"), mCode = (*E)->GetMethodID(E, cH, "getResponseCode", "()I");
    jmethodID mGetIn = (*E)->GetMethodID(E, cH, "getInputStream", "()Ljava/io/InputStream;"), mGetErr = (*E)->GetMethodID(E, cH, "getErrorStream", "()Ljava/io/InputStream;"), mDisc = (*E)->GetMethodID(E, cH, "disconnect", "()V");
    jmethodID mWrite = (*E)->GetMethodID(E, cOut, "write", "([BII)V"), mOClose = (*E)->GetMethodID(E, cOut, "close", "()V"), mRead = (*E)->GetMethodID(E, cIn, "read", "([B)I"), mIClose = (*E)->GetMethodID(E, cIn, "close", "()V");
    if (jx(E)) { snprintf(err, (size_t)ecap, "java.net is missing a method"); goto done; }
    jobject u = (*E)->NewObject(E, cURL, mNew, (*E)->NewStringUTF(E, url)); if (jx(E) || !u) { snprintf(err, (size_t)ecap, "bad cloud URL %s", url); goto done; }
    conn = (*E)->CallObjectMethod(E, u, mOpen); if ((*E)->ExceptionCheck(E) || !conn) { jx_msg(E, err, ecap, "cannot open the connection"); conn = NULL; goto done; }
    (*E)->CallVoidMethod(E, conn, mMethod, (*E)->NewStringUTF(E, method)); (*E)->CallVoidMethod(E, conn, mCT, 15000); (*E)->CallVoidMethod(E, conn, mRT, 120000);
    (*E)->CallVoidMethod(E, conn, mProp, (*E)->NewStringUTF(E, "Accept"), (*E)->NewStringUTF(E, "application/json, application/x-tar"));
    if (jx(E)) { snprintf(err, (size_t)ecap, "cannot set up the request"); goto done; }
    if (ctype) {
        (*E)->CallVoidMethod(E, conn, mDo, JNI_TRUE); (*E)->CallVoidMethod(E, conn, mProp, (*E)->NewStringUTF(E, "Content-Type"), (*E)->NewStringUTF(E, ctype));
        (*E)->CallVoidMethod(E, conn, mFixed, (jint)blen);
        jobject os = (*E)->CallObjectMethod(E, conn, mGetOut); if ((*E)->ExceptionCheck(E) || !os) { jx_msg(E, err, ecap, "cannot reach the library"); goto done; }
        jbyteArray chunk = (*E)->NewByteArray(E, 1 << 16); if (!chunk) { jx(E); snprintf(err, (size_t)ecap, "out of memory"); goto done; }
        for (size_t w = 0; w < blen;) { jint k = (jint)(blen - w > (1 << 16) ? 1 << 16 : blen - w);
            (*E)->SetByteArrayRegion(E, chunk, 0, k, (const jbyte *)body + w); (*E)->CallVoidMethod(E, os, mWrite, chunk, 0, k);
            if ((*E)->ExceptionCheck(E)) { jx_msg(E, err, ecap, "sending the save failed"); goto done; } w += (size_t)k; }
        (*E)->CallVoidMethod(E, os, mOClose); if ((*E)->ExceptionCheck(E)) { jx_msg(E, err, ecap, "sending the save failed"); goto done; }
    }
    *status = (*E)->CallIntMethod(E, conn, mCode); if ((*E)->ExceptionCheck(E)) { jx_msg(E, err, ecap, "cannot reach the library (is Tailscale on?)"); goto done; }
    jobject in = (*E)->CallObjectMethod(E, conn, *status >= 400 ? mGetErr : mGetIn); if ((*E)->ExceptionCheck(E)) { jx(E); in = NULL; }
    if (in) {
        jbyteArray chunk = (*E)->NewByteArray(E, 1 << 16); if (!chunk) { jx(E); snprintf(err, (size_t)ecap, "out of memory"); goto done; }
        for (;;) { jint k = (*E)->CallIntMethod(E, in, mRead, chunk); if ((*E)->ExceptionCheck(E)) { jx_msg(E, err, ecap, "reading the reply failed"); goto done; }
            if (k < 0) break; if (buf_put(&o, NULL, (size_t)k)) { snprintf(err, (size_t)ecap, "out of memory"); goto done; }
            (*E)->GetByteArrayRegion(E, chunk, 0, k, (jbyte *)o.b + o.n - k); }
        (*E)->CallVoidMethod(E, in, mIClose); jx(E);
    }
    if (buf_put(&o, "", 1)) { snprintf(err, (size_t)ecap, "out of memory"); goto done; }
    *resp = o.b; *rlen = o.n - 1; o.b = NULL; r = 0;
done:
    if (conn) { (*E)->CallVoidMethod(E, conn, mDisc); jx(E); }
    free(o.b);
    (*E)->PopLocalFrame(E, NULL);
    if (did) (*g_cvm)->DetachCurrentThread(g_cvm);
    return r;
}
// the server's {"error": ...} for a failed request, else its status
static void http_error(int status, const uint8_t *resp, char *err, int ecap) {
    char m[300]; if (resp && !dm_cloud_json((const char *)resp, "error", m, sizeof m) && m[0]) snprintf(err, (size_t)ecap, "the library says: %s", m);
    else snprintf(err, (size_t)ecap, "the library answered HTTP %d", status);
}
static void parse_save(const char *json, dm_cloud_save *s) {
    char t[40]; memset(s, 0, sizeof *s);
    dm_cloud_json(json, "id", s->id, sizeof s->id); dm_cloud_json(json, "hash", s->hash, sizeof s->hash);
    dm_cloud_json(json, "device", s->device, sizeof s->device); dm_cloud_json(json, "platform", s->platform, sizeof s->platform);
    if (!dm_cloud_json(json, "size", t, sizeof t)) s->size = atoll(t);
    if (!dm_cloud_json(json, "uploadedAt", t, sizeof t)) s->uploaded_ms = atoll(t);
}

// saves/cloud.txt: the sync base
static void read_base(char hash[65], char id[96]) {
    hash[0] = id[0] = 0; char p[700], line[200]; snprintf(p, sizeof p, "%s/cloud.txt", dm_saves_dir());
    FILE *f = fopen(p, "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "hash=", 5)) snprintf(hash, 65, "%s", line + 5); else if (!strncmp(line, "id=", 3)) snprintf(id, 96, "%s", line + 3); }
    fclose(f);
}
static int write_base(const char *file, const dm_cloud_save *s) {
    char p[700], tmp[710]; snprintf(p, sizeof p, "%s/%s", dm_saves_dir(), file); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    fprintf(f, "# cloud saves (cloud_android.c): the cloud save this device last uploaded or downloaded\nid=%s\nhash=%s\ndevice=%s\n", s->id, s->hash, s->device); fclose(f);
    return rename(tmp, p) ? -1 : 0;
}
// the committed save's hash, again while the game commits during it
static int local_hash(char hex[65], char *err, int ecap) {
    const char *save = dm_saves_save_path(), *dev = dm_saves_device_path();
    for (int k = 0; k < 3; k++) {
        uint64_t c0 = dm_fs_commit_count(); int r = dm_cloud_hash_tree(save, dev, hex, NULL, NULL);
        if (!r && dm_fs_commit_count() == c0) return 0;
        if (r && r != -ENOENT && dm_fs_commit_count() == c0) { snprintf(err, (size_t)ecap, "reading the save: %s", strerror(-r)); return -1; }
    }
    snprintf(err, (size_t)ecap, "the game kept saving while the save was read"); return -1;
}

int dm_cloud_check(dm_cloud_status *out, char *err, int ecap) {
    memset(out, 0, sizeof *out); err[0] = 0;
    const char *url = dm_cloud_url(); if (!url) { snprintf(err, (size_t)ecap, "cloud saves are off (install from the game library)"); return -1; }
    char id[96]; read_base(out->base, id);
    if (local_hash(out->local, err, ecap)) return -1;
    char u[700], body[300]; snprintf(u, sizeof u, "%s/status", url); snprintf(body, sizeof body, "{\"localHash\":\"%s\",\"baseHash\":\"%s\"}", out->local, out->base);
    int st; uint8_t *resp; size_t n;
    if (http("POST", u, "application/json", body, strlen(body), &st, &resp, &n, err, ecap)) return -1;
    if (st != 200) { http_error(st, resp, err, ecap); free(resp); return -1; }
    const char *lat = strstr((const char *)resp, "\"latest\":{");
    if (lat) { parse_save(lat, &out->latest); out->has_latest = out->latest.hash[0] != 0; }
    free(resp);
    out->state = dm_cloud_decide(out->local, out->base, out->has_latest ? out->latest.hash : NULL);   // the same rule as the server's, on what it sent
    return 0;
}

static void url_encode(const char *s, char *out, int cap) {
    int n = 0; for (; *s && n < cap - 4; s++) { unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[n++] = (char)c;
        else n += snprintf(out + n, (size_t)(cap - n), "%%%02X", c); }
    out[n] = 0;
}

int dm_cloud_upload(dm_cloud_save *out, char *err, int ecap) {
    memset(out, 0, sizeof *out); err[0] = 0;
    const char *url = dm_cloud_url(); if (!url) { snprintf(err, (size_t)ecap, "cloud saves are off"); return -1; }
    char name[96], dir[700], devdir[720];   // a snapshot through the export (a commit while it copies starts it again), archived, then removed
    if (dm_saves_export("-cloud-upload", name, sizeof name, err, ecap)) return -1;
    snprintf(dir, sizeof dir, "%s/%s", dm_saves_dir(), name); snprintf(devdir, sizeof devdir, "%s.device", dir);
    uint8_t *tar = NULL; size_t len = 0; int r = dm_cloud_tar(dir, devdir, &tar, &len, err, ecap);
    dm_saves_remove(name);
    if (r) return -1;
    char who[64], enc[200], u[1000]; model(who, sizeof who); url_encode(who, enc, sizeof enc);
    snprintf(u, sizeof u, "%s?platform=android&device=%s", url, enc);
    int st; uint8_t *resp; size_t n;
    r = http("POST", u, "application/x-tar", tar, len, &st, &resp, &n, err, ecap); free(tar);
    if (r) return -1;
    if (st != 200 && st != 201) { http_error(st, resp, err, ecap); free(resp); return -1; }
    const char *sv = strstr((const char *)resp, "\"save\":{"); if (sv) parse_save(sv, out);
    free(resp);
    if (!out->hash[0]) { snprintf(err, (size_t)ecap, "the library's answer named no save"); return -1; }
    write_base("cloud.txt", out);
    LOG("[cloud] uploaded the save: %s (%lld bytes, %s)", out->id, out->size, out->hash);
    return 0;
}

int dm_cloud_download(const dm_cloud_save *which, char *name, int ncap, char *err, int ecap) {
    err[0] = name[0] = 0;
    const char *url = dm_cloud_url(); if (!url) { snprintf(err, (size_t)ecap, "cloud saves are off"); return -1; }
    if (!which || !which->id[0]) { snprintf(err, (size_t)ecap, "there is no save in the cloud yet"); return -1; }
    char u[800]; snprintf(u, sizeof u, "%s/%s", url, which->id);
    int st; uint8_t *resp; size_t n;
    if (http("GET", u, NULL, NULL, 0, &st, &resp, &n, err, ecap)) return -1;
    if (st != 200) { http_error(st, resp, err, ecap); free(resp); return -1; }
    char stamp[32]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(name, (size_t)ncap, "%s-cloud", stamp);
    char dir[700], devdir[720]; snprintf(dir, sizeof dir, "%s/%s", dm_saves_dir(), name); snprintf(devdir, sizeof devdir, "%s.device", dir);
    dm_saves_remove(name);
    int has_dev; int r = dm_cloud_untar(resp, n, dir, devdir, &has_dev, err, ecap); free(resp);
    if (r) { dm_saves_remove(name); return -1; }
    char got[65]; if (dm_cloud_hash_tree(dir, devdir, got, NULL, NULL) || strcmp(got, which->hash)) {   // what arrived is the save the cloud listed
        dm_saves_remove(name); snprintf(err, (size_t)ecap, "the downloaded save does not match the cloud's hash"); return -1; }
    if (write_base("cloud-pending.txt", which)) { dm_saves_remove(name); snprintf(err, (size_t)ecap, "cannot write saves/cloud-pending.txt"); return -1; }
    if (dm_saves_request_import_as(name, "cloud")) { dm_saves_remove(name); snprintf(err, (size_t)ecap, "cannot ask for the import (saves/import.txt)"); return -1; }
    LOG("[cloud] downloaded %s from %s into saves/%s: imported at the next start", which->id, which->device, name);
    return 0;
}
#endif
