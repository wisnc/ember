#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorAAC.h"
#include "AudioOutput.h"

static const int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

static const char KEY_UP = ';', KEY_DOWN = '.', KEY_LEFT = ',', KEY_RIGHT = '/';
static const char KEY_NEXT = ']', KEY_PREV = '[';
static const char KEY_SHUFFLE = '\\';
static const char KEY_ENQUEUE = '\'';
static const char KEY_VOLUP = '=';
static const char KEY_VOLDN_A = '_', KEY_VOLDN_B = '-';
static const char KEY_SCAN_A = 's', KEY_SCAN_B = 'S';

static const uint8_t SCROLL_ADDR = 0x40, SCROLL_INC_REG = 0x50, SCROLL_FW_REG = 0xFE;
static const uint32_t SCROLL_FREQ = 400000;
static bool scrollPresent = false;
static unsigned long scrollLastPoll = 0, scrollLastProbe = 0;

static const int SCR_W = 240, SCR_H = 135;
static const int MARGIN = 5, GAP = 2, BORDER = 2, PAD = 2, INSET = BORDER + PAD;

static const int LEFT_X = MARGIN, LEFT_Y = MARGIN, LEFT_W = 116, LEFT_H = SCR_H - 2 * MARGIN;
static const int RIGHT_X = LEFT_X + LEFT_W + GAP, RIGHT_W = SCR_W - MARGIN - RIGHT_X;
static const int NP_Y = MARGIN, NP_H = 50;
static const int Q_Y = NP_Y + NP_H + GAP, Q_H = SCR_H - MARGIN - Q_Y;

static const int LC_X = LEFT_X + INSET, LC_Y = LEFT_Y + INSET, LC_W = LEFT_W - 2 * INSET, LC_H = LEFT_H - 2 * INSET;
static const int NC_X = RIGHT_X + INSET, NC_Y = NP_Y + INSET, NC_W = RIGHT_W - 2 * INSET, NC_H = NP_H - 2 * INSET;
static const int QC_X = RIGHT_X + INSET, QC_Y = Q_Y + INSET, QC_W = RIGHT_W - 2 * INSET, QC_H = Q_H - 2 * INSET;

static const lgfx::IFont* FONT = &fonts::lgfxJapanGothic_12;
static const int ROW_H = 13;
static const int BROWSER_ROWS = LC_H / ROW_H;
static const int QUEUE_ROWS   = QC_H / ROW_H;

static const char* CFG_DIR   = "/.ember";
static const char* CFG_PATH  = "/.ember/config";
static const char* IDX_TXT   = "/.ember/index.txt";
static const char* IDX_BIN   = "/.ember/index.bin";

static const int MY_PATH_MAX = 256;
static char     cfgMusicDir[MY_PATH_MAX] = "/Music";
static int      cfgBrightness = 255;
static uint32_t cfgScreenTimeoutMs = 30000;
static uint32_t cfgAccentRGB = 0xF88C00;
static uint32_t cfgBackgroundRGB = 0x000000;
static bool     cfgShuffle = false;

static uint16_t COL_ACCENT, COL_BG, COL_TEXT, COL_DIM;

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t rgb565From24(uint32_t c) { return rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }

static uint32_t blend24(uint32_t a, uint32_t b, int t) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + ((br - ar) * t) / 255, g = ag + ((bg - ag) * t) / 255, bl = ab + ((bb - ab) * t) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}
static int luma24(uint32_t c) { return (299 * ((c >> 16) & 0xFF) + 587 * ((c >> 8) & 0xFF) + 114 * (c & 0xFF)) / 1000; }

static void applyTheme() {
    COL_ACCENT = rgb565From24(cfgAccentRGB);
    COL_BG     = rgb565From24(cfgBackgroundRGB);
    uint32_t pole = luma24(cfgBackgroundRGB) < 128 ? 0xFFFFFF : 0x000000;
    COL_TEXT   = rgb565From24(blend24(cfgAccentRGB, pole, 190));
    COL_DIM    = rgb565From24(blend24(cfgAccentRGB, cfgBackgroundRGB, 130));
}

static bool parseHex24(const char* s, uint32_t &out) {
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    out = v; return true;
}

static void trimRight(char* s) {
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

static void saveConfig() {
    SD.mkdir(CFG_DIR);
    File f = SD.open(CFG_PATH, FILE_WRITE);
    if (!f) return;
    f.printf("music_dir=%s\n", cfgMusicDir);
    f.printf("brightness=%d\n", cfgBrightness);
    f.printf("screen_timeout=%lu\n", (unsigned long)cfgScreenTimeoutMs);
    f.printf("accent=%06lX\n", (unsigned long)cfgAccentRGB);
    f.printf("background=%06lX\n", (unsigned long)cfgBackgroundRGB);
    f.printf("shuffle=%d\n", cfgShuffle ? 1 : 0);
    f.close();
}

static void loadConfig() {
    File f = SD.open(CFG_PATH, FILE_READ);
    if (!f) { saveConfig(); return; }
    char line[320];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        trimRight(line);
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line; const char* val = eq + 1;
        if      (!strcmp(key, "music_dir"))      { if (val[0] == '/') { strncpy(cfgMusicDir, val, MY_PATH_MAX - 1); cfgMusicDir[MY_PATH_MAX - 1] = '\0'; } }
        else if (!strcmp(key, "brightness"))     { int v = atoi(val); if (v < 0) v = 0; if (v > 255) v = 255; cfgBrightness = v; }
        else if (!strcmp(key, "screen_timeout")) { cfgScreenTimeoutMs = (uint32_t)strtoul(val, nullptr, 10); }
        else if (!strcmp(key, "accent"))         { parseHex24(val, cfgAccentRGB); }
        else if (!strcmp(key, "background"))     { parseHex24(val, cfgBackgroundRGB); }
        else if (!strcmp(key, "shuffle"))        { cfgShuffle = atoi(val) != 0; }
    }
    f.close();
    int L = strlen(cfgMusicDir);
    if (L > 1 && cfgMusicDir[L-1] == '/') cfgMusicDir[L-1] = '\0';
}

static const int MAX_ENTRIES = 256, NAME_POOL_SIZE = 8192, MAX_DEPTH = 8;
static char     namePool[NAME_POOL_SIZE];
static uint16_t nameOffset[MAX_ENTRIES];
static bool     entryIsDir[MAX_ENTRIES];
static int      sortIdx[MAX_ENTRIES];
static int      entryCount = 0, poolUsed = 0;

static char currentPath[MY_PATH_MAX] = "/";
static int  cursor = 0, scroll = 0, depth = 0;
static int  cursorStack[MAX_DEPTH], scrollStack[MAX_DEPTH];
static bool rootOk = false;

static const int QUEUE_MAX = 256, QNAME_POOL = 16384;
static char     queuePool[QNAME_POOL];
static uint16_t queueOffset[QUEUE_MAX];
static int      queueCount = 0, queuePoolUsed = 0;
static char     queueFolder[MY_PATH_MAX] = "";
static int      queuePos = 0;

enum PlayState { STOPPED, PLAYING, PAUSED };
static PlayState playState = STOPPED;
static char nowPlaying[64] = "";
static int  volume = 50;

static char curArtist[96] = "";
static char curTitle[96]  = "";
static char curAlbum[96]  = "";

enum AudioFormat { FMT_MP3, FMT_FLAC, FMT_WAV, FMT_AAC };
static AudioFormat        curFormat = FMT_MP3;
static AudioGenerator     *decoder = nullptr;
static AudioFileSourceSD  *file = nullptr;
static AudioFileSourceID3 *id3  = nullptr;

class AudioOutputM5Speaker : public AudioOutput {
public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t ch = 0) { _m5sound = m5sound; _virtual_ch = ch; }
    bool begin() override { return true; }
    bool ConsumeSample(int16_t sample[2]) override {
        if (_tri_buffer_index < tri_buf_size) {
            _tri_buffer[_tri_index][_tri_buffer_index]   = sample[0];
            _tri_buffer[_tri_index][_tri_buffer_index+1] = sample[1];
            _tri_buffer_index += 2;
            return true;
        }
        flush();
        return false;
    }
    void flush() override {
        if (_tri_buffer_index) {
            _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
            _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
            _tri_buffer_index = 0;
        }
    }
    bool stop() override { flush(); _m5sound->stop(_virtual_ch); return true; }
protected:
    m5::Speaker_Class* _m5sound; uint8_t _virtual_ch;
    static constexpr size_t tri_buf_size = 2048;
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0, _tri_index = 0;
};
static AudioOutputM5Speaker *out = nullptr;

static bool needsFullRedraw = true;
static bool redrawBrowser = false, redrawNowPlaying = false, redrawQueue = false;

static bool displayOn = true;
static bool screenIsOff = false;
static unsigned long lastInputTime = 0;
static bool screenVisible() { return displayOn && !screenIsOff; }
static bool panelAsleep = false;
static void applyBacklight() {
    auto &d = M5Cardputer.Display;
    if (screenVisible()) {
        if (panelAsleep) { setCpuFrequencyMhz(240); d.wakeup(); panelAsleep = false; }
        d.setBrightness(cfgBrightness);
    } else {
        d.setBrightness(0);
        if (!panelAsleep) { d.sleep(); panelAsleep = true; setCpuFrequencyMhz(160); }
    }
}

static bool hasExt(const char* s, const char* ext) {
    int n = (int)strlen(s), el = (int)strlen(ext);
    if (n < el) return false;
    const char* e = s + n - el;
    for (int i = 0; i < el; i++) {
        char a = e[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
static AudioFormat formatForName(const char* nm) {
    if (hasExt(nm, ".flac")) return FMT_FLAC;
    if (hasExt(nm, ".wav") || hasExt(nm, ".wave")) return FMT_WAV;
    if (hasExt(nm, ".aac")) return FMT_AAC;
    return FMT_MP3;
}
static bool isAudioFile(const char* nm) {
    return hasExt(nm, ".mp3") || hasExt(nm, ".flac") || hasExt(nm, ".wav") || hasExt(nm, ".wave") || hasExt(nm, ".aac");
}
static const char* baseName(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static void stripExt(const char* in, char* out, size_t outSize) {
    strncpy(out, in, outSize - 1); out[outSize - 1] = '\0';
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = '\0';
}
static int nameCmp(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = *a, cb = *b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            int la = ea - a, lb = eb - b;
            if (la != lb) return la - lb;
            while (a < ea && b < eb) {
                if (*a != *b) return (int)*a - (int)*b;
                a++; b++;
            }
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void joinPath(char* dst, size_t n, const char* dir, const char* name) {
    if (strcmp(dir, "/") == 0) snprintf(dst, n, "/%s", name);
    else snprintf(dst, n, "%s/%s", dir, name);
}

static const char* entryName(int i) { return &namePool[nameOffset[i]]; }
static const char* nameAt(int i)    { return &namePool[nameOffset[sortIdx[i]]]; }
static bool isDirAt(int i)          { return entryIsDir[sortIdx[i]]; }

static void sortEntries() {
    for (int i = 0; i < entryCount; i++) sortIdx[i] = i;
    for (int i = 1; i < entryCount; i++) {
        int key = sortIdx[i], j = i - 1;
        while (j >= 0) {
            int a = sortIdx[j];
            bool swap;
            if (entryIsDir[a] != entryIsDir[key]) swap = (!entryIsDir[a] && entryIsDir[key]);
            else swap = (nameCmp(entryName(a), entryName(key)) > 0);
            if (!swap) break;
            sortIdx[j + 1] = sortIdx[j];
            j--;
        }
        sortIdx[j + 1] = key;
    }
}

static const char* SD_MOUNT = "/sd";
static DIR* openSdDir(const char* path) {
    char vfs[MY_PATH_MAX + 8];
    snprintf(vfs, sizeof(vfs), "%s%s", SD_MOUNT, path);
    return opendir(vfs);
}
static bool direntIsDir(const char* dirPath, const struct dirent* de) {
    if (de->d_type == DT_DIR) return true;
    if (de->d_type == DT_REG) return false;
    char vfs[MY_PATH_MAX + 8];
    snprintf(vfs, sizeof(vfs), "%s%s/%s", SD_MOUNT, dirPath, de->d_name);
    struct stat st;
    return stat(vfs, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool loadDir() {
    entryCount = 0; poolUsed = 0;
    DIR* dir = openSdDir(currentPath);
    if (!dir) return false;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr && entryCount < MAX_ENTRIES) {
        const char* nm = de->d_name;
        int len = strlen(nm);
        if (nm[0] != '.' && (poolUsed + len + 1) < NAME_POOL_SIZE) {
            bool isDir = direntIsDir(currentPath, de);
            if (isDir || isAudioFile(nm)) {
                nameOffset[entryCount] = poolUsed;
                entryIsDir[entryCount] = isDir;
                memcpy(&namePool[poolUsed], nm, len + 1);
                poolUsed += len + 1;
                entryCount++;
            }
        }
    }
    closedir(dir);
    sortEntries();
    cursor = 0; scroll = 0;
    return true;
}

static void buildQueue(const char* folder) {
    queueCount = 0; queuePoolUsed = 0;
    strncpy(queueFolder, folder, MY_PATH_MAX - 1);
    queueFolder[MY_PATH_MAX - 1] = '\0';

    DIR* dir = openSdDir(folder);
    if (!dir) return;
    struct dirent* de;
    char full[MY_PATH_MAX];
    while ((de = readdir(dir)) != nullptr && queueCount < QUEUE_MAX) {
        const char* nm = de->d_name;
        if (nm[0] == '.' || direntIsDir(folder, de) || !isAudioFile(nm)) continue;
        joinPath(full, sizeof(full), folder, nm);
        int len = strlen(full);
        if ((queuePoolUsed + len + 1) < QNAME_POOL) {
            queueOffset[queueCount] = queuePoolUsed;
            memcpy(&queuePool[queuePoolUsed], full, len + 1);
            queuePoolUsed += len + 1;
            queueCount++;
        }
    }
    closedir(dir);

    for (int i = 1; i < queueCount; i++) {
        uint16_t key = queueOffset[i]; int j = i - 1;
        while (j >= 0 && nameCmp(&queuePool[queueOffset[j]], &queuePool[key]) > 0) {
            queueOffset[j + 1] = queueOffset[j]; j--;
        }
        queueOffset[j + 1] = key;
    }
}

static const char* queueName(int i) { return &queuePool[queueOffset[i]]; }

static void utf16ToUtf8(const uint8_t* b, size_t n, bool bigEndian, char* dst, size_t dstSize) {
    size_t i = 0, o = 0;
    if (n >= 2) {
        if (b[0] == 0xFE && b[1] == 0xFF) { bigEndian = true; i = 2; }
        else if (b[0] == 0xFF && b[1] == 0xFE) { bigEndian = false; i = 2; }
    }
    for (; o + 4 < dstSize && i + 1 < n; i += 2) {
        uint16_t cu = bigEndian ? ((b[i] << 8) | b[i+1]) : (b[i] | (b[i+1] << 8));
        if (cu == 0) break;
        if (cu >= 0xD800 && cu <= 0xDFFF) continue;
        if (cu < 0x80) { dst[o++] = (char)cu; }
        else if (cu < 0x800) { dst[o++] = (char)(0xC0 | (cu >> 6)); dst[o++] = (char)(0x80 | (cu & 0x3F)); }
        else { dst[o++] = (char)(0xE0 | (cu >> 12)); dst[o++] = (char)(0x80 | ((cu >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (cu & 0x3F)); }
    }
    dst[o] = '\0';
}
static void latin1ToUtf8(const uint8_t* b, size_t n, char* dst, size_t dstSize) {
    size_t o = 0;
    for (size_t i = 0; i < n && b[i] && o + 3 < dstSize; i++) {
        uint8_t c = b[i];
        if (c < 0x80) dst[o++] = (char)c;
        else { dst[o++] = (char)(0xC0 | (c >> 6)); dst[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    dst[o] = '\0';
}
static void copyBounded(const uint8_t* b, size_t n, char* dst, size_t dstSize) {
    size_t o = 0;
    for (; o < n && o + 1 < dstSize && b[o]; o++) dst[o] = (char)b[o];
    dst[o] = '\0';
}

static void copyMeta(char* dst, size_t dstSize, bool isUnicode, const char* str) {
    if (!isUnicode) {
        strncpy(dst, str, dstSize - 1);
        dst[dstSize - 1] = '\0';
        return;
    }
    utf16ToUtf8((const uint8_t*)str, 63, false, dst, dstSize);
}
static void mp3MetadataCB(void* cbData, const char* type, bool isUnicode, const char* str) {
    (void)cbData;
    if      (!strcmp(type, "Title"))     copyMeta(curTitle,  sizeof(curTitle),  isUnicode, str);
    else if (!strcmp(type, "Performer")) copyMeta(curArtist, sizeof(curArtist), isUnicode, str);
    else if (!strcmp(type, "Album"))     copyMeta(curAlbum,  sizeof(curAlbum),  isUnicode, str);
}

static const int INDEX_MAX = 4096;
static uint32_t idxHash[INDEX_MAX];
static uint32_t idxOffset[INDEX_MAX];
static int      idxCount = 0;
static File     indexFile;
static const int TITLE_MAX = 96;

static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void sortIndex() {
    for (int gap = idxCount / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < idxCount; i++) {
            uint32_t h = idxHash[i], o = idxOffset[i]; int j = i;
            while (j >= gap && idxHash[j - gap] > h) { idxHash[j] = idxHash[j - gap]; idxOffset[j] = idxOffset[j - gap]; j -= gap; }
            idxHash[j] = h; idxOffset[j] = o;
        }
    }
}

static void closeIndex() { if (indexFile) indexFile.close(); idxCount = 0; }

static bool loadIndex() {
    closeIndex();
    File b = SD.open(IDX_BIN, FILE_READ);
    if (!b) return false;
    char magic[4]; uint32_t n = 0;
    bool ok = b.read((uint8_t*)magic, 4) == 4 && memcmp(magic, "EMIX", 4) == 0 && b.read((uint8_t*)&n, 4) == 4 && n <= (uint32_t)INDEX_MAX;
    if (ok) {
        for (uint32_t i = 0; i < n; i++) {
            if (b.read((uint8_t*)&idxHash[i], 4) != 4 || b.read((uint8_t*)&idxOffset[i], 4) != 4) { ok = false; break; }
        }
    }
    b.close();
    if (!ok) { idxCount = 0; return false; }
    idxCount = (int)n;
    indexFile = SD.open(IDX_TXT, FILE_READ);
    if (!indexFile) { idxCount = 0; return false; }
    return true;
}

static void takeField(char* dst, size_t n, const char* src) {
    if (!dst || !n) return;
    if (src) { strncpy(dst, src, n - 1); dst[n - 1] = '\0'; } else dst[0] = '\0';
}
static bool readIndexLine(uint32_t off, const char* wantPath, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    if (!indexFile) return false;
    if (!indexFile.seek(off)) return false;
    static char line[MY_PATH_MAX + 3 * TITLE_MAX + 8];
    int n = indexFile.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    trimRight(line);
    char* t1 = strchr(line, '\t'); if (!t1) return false;
    *t1 = '\0';
    if (strcmp(line, wantPath) != 0) return false;
    char* f2 = t1 + 1;
    char* t2 = strchr(f2, '\t'); char* f3 = nullptr; char* f4 = nullptr;
    if (t2) { *t2 = '\0'; f3 = t2 + 1; char* t3 = strchr(f3, '\t'); if (t3) { *t3 = '\0'; f4 = t3 + 1; } }
    takeField(title, tl, f2);
    takeField(artist, al, f3);
    takeField(album, abl, f4);
    return true;
}

static bool indexLookup(const char* path, char* title, size_t tl, char* artist = nullptr, size_t al = 0, char* album = nullptr, size_t abl = 0) {
    if (idxCount == 0) return false;
    uint32_t h = fnv1a(path);
    int lo = 0, hi = idxCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (idxHash[mid] < h) lo = mid + 1;
        else if (idxHash[mid] > h) hi = mid - 1;
        else {
            int i = mid;
            while (i > 0 && idxHash[i - 1] == h) i--;
            for (; i < idxCount && idxHash[i] == h; i++) {
                if (readIndexLine(idxOffset[i], path, title, tl, artist, al, album, abl)) return true;
            }
            return false;
        }
    }
    return false;
}

static void displayTitleFor(const char* path, char* out, size_t outSize) {
    if (indexLookup(path, out, outSize) && out[0]) return;
    stripExt(baseName(path), out, outSize);
}

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t be24(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static uint32_t le32(const uint8_t* p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint32_t syncsafe(const uint8_t* p) { return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) | ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F); }

static void decodeId3Text(const uint8_t* b, size_t n, char* dst, size_t dstSize) {
    if (n < 1) { dst[0] = '\0'; return; }
    uint8_t enc = b[0]; b++; n--;
    switch (enc) {
        case 1: utf16ToUtf8(b, n, false, dst, dstSize); break;
        case 2: utf16ToUtf8(b, n, true,  dst, dstSize); break;
        case 3: copyBounded(b, n, dst, dstSize); break;
        default: latin1ToUtf8(b, n, dst, dstSize); break;
    }
}

static bool readTagsID3(File &f, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    uint8_t hdr[10];
    f.seek(0);
    if (f.read(hdr, 10) != 10) return false;
    if (memcmp(hdr, "ID3", 3) != 0) return false;
    uint8_t ver = hdr[3], flags = hdr[5];
    uint32_t tagSize = syncsafe(hdr + 6);
    uint32_t pos = 10, end = 10 + tagSize;
    if (flags & 0x40) {
        uint8_t eh[4];
        if (f.read(eh, 4) != 4) return true;
        uint32_t ehSize = (ver == 4) ? syncsafe(eh) : be32(eh) + 4;
        pos += ehSize;
    }
    static uint8_t buf[512];
    int found = 0, frames = 0;
    while (pos + 6 <= end && found < 3 && frames < 400) {
        frames++;
        f.seek(pos);
        char id[5] = {0}; uint32_t fsz; uint32_t hlen;
        if (ver == 2) {
            uint8_t h[6]; if (f.read(h, 6) != 6) break;
            if (h[0] == 0) break;
            memcpy(id, h, 3); fsz = be24(h + 3); hlen = 6;
        } else {
            uint8_t h[10]; if (f.read(h, 10) != 10) break;
            if (h[0] == 0) break;
            memcpy(id, h, 4); fsz = (ver == 4) ? syncsafe(h + 4) : be32(h + 4); hlen = 10;
        }
        bool isTitle = !strcmp(id, "TIT2") || !strcmp(id, "TT2");
        bool isArtist = !strcmp(id, "TPE1") || !strcmp(id, "TP1");
        bool isAlbum = !strcmp(id, "TALB") || !strcmp(id, "TAL");
        if ((isTitle || isArtist || isAlbum) && fsz > 0) {
            size_t n = fsz < sizeof(buf) ? fsz : sizeof(buf);
            n = f.read(buf, n);
            if (isTitle && !title[0]) { decodeId3Text(buf, n, title, tl); found++; }
            else if (isArtist && !artist[0]) { decodeId3Text(buf, n, artist, al); found++; }
            else if (isAlbum && !album[0]) { decodeId3Text(buf, n, album, abl); found++; }
        }
        pos += hlen + fsz;
    }
    return true;
}

static bool readTagsFLAC(File &f, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    uint8_t hdr[4];
    f.seek(0);
    if (f.read(hdr, 4) != 4 || memcmp(hdr, "fLaC", 4) != 0) return false;
    uint32_t pos = 4;
    static uint8_t buf[512];
    for (int blocks = 0; blocks < 64; blocks++) {
        f.seek(pos);
        uint8_t bh[4]; if (f.read(bh, 4) != 4) break;
        bool last = bh[0] & 0x80; uint8_t type = bh[0] & 0x7F; uint32_t len = be24(bh + 1);
        if (type == 4) {
            uint8_t l4[4];
            if (f.read(l4, 4) != 4) break;
            uint32_t vendorLen = le32(l4);
            f.seek(pos + 8 + vendorLen);
            if (f.read(l4, 4) != 4) break;
            uint32_t count = le32(l4);
            uint32_t cpos = pos + 8 + vendorLen + 4;
            for (uint32_t i = 0; i < count && i < 128; i++) {
                f.seek(cpos);
                if (f.read(l4, 4) != 4) break;
                uint32_t clen = le32(l4);
                size_t n = clen < sizeof(buf) - 1 ? clen : sizeof(buf) - 1;
                n = f.read(buf, n); buf[n] = '\0';
                if (n >= 6 && !strncasecmp((char*)buf, "TITLE=", 6) && !title[0]) copyBounded(buf + 6, n - 6, title, tl);
                else if (n >= 7 && !strncasecmp((char*)buf, "ARTIST=", 7) && !artist[0]) copyBounded(buf + 7, n - 7, artist, al);
                else if (n >= 6 && !strncasecmp((char*)buf, "ALBUM=", 6) && !album[0]) copyBounded(buf + 6, n - 6, album, abl);
                cpos += 4 + clen;
                if (title[0] && artist[0] && album[0]) break;
            }
            break;
        }
        if (last) break;
        pos += 4 + len;
    }
    return true;
}

static void readTags(const char* path, char* title, size_t tl, char* artist, size_t al, char* album, size_t abl) {
    title[0] = '\0'; artist[0] = '\0'; album[0] = '\0';
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    if (hasExt(path, ".flac")) { if (!readTagsFLAC(f, title, tl, artist, al, album, abl)) readTagsID3(f, title, tl, artist, al, album, abl); }
    else readTagsID3(f, title, tl, artist, al, album, abl);
    f.close();
}

static void sanitizeField(char* s) {
    for (char* p = s; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    trimRight(s);
}

static void drawPaneFrames();
static void drawScanProgress(int tracks, const char* status) {
    if (!screenVisible()) return;
    auto &d = M5Cardputer.Display;
    d.fillRect(NC_X, NC_Y, NC_W, NC_H, COL_BG);
    d.setTextColor(COL_ACCENT, COL_BG);
    d.setCursor(NC_X, NC_Y);
    d.print(status);
    d.setTextColor(COL_TEXT, COL_BG);
    d.setCursor(NC_X, NC_Y + ROW_H);
    d.printf("%d tracks", tracks);
}

static File scanOut;
static int  scanTracks = 0, scanSkipped = 0;
static unsigned long scanLastDraw = 0;

static char scanPathBuf[MAX_DEPTH][MY_PATH_MAX];
static char scanNameBuf[MY_PATH_MAX];

static void scanDir(const char* path, int level) {
    if (level + 1 >= MAX_DEPTH) return;
    char* full = scanPathBuf[level + 1];
    {
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
        File e = dir.openNextFile();
        while (e) {
            strncpy(scanNameBuf, baseName(e.name()), MY_PATH_MAX - 1);
            scanNameBuf[MY_PATH_MAX - 1] = '\0';
            bool isDir = e.isDirectory();
            e.close();
            const char* nm = scanNameBuf;
            if (nm[0] != '.' && !isDir && isAudioFile(nm)) {
                joinPath(full, MY_PATH_MAX, path, nm);
                if (scanTracks < INDEX_MAX && strlen(full) < MY_PATH_MAX - 1) {
                    static char title[TITLE_MAX], artist[TITLE_MAX], album[TITLE_MAX];
                    readTags(full, title, sizeof(title), artist, sizeof(artist), album, sizeof(album));
                    sanitizeField(title); sanitizeField(artist); sanitizeField(album);
                    if (!title[0]) stripExt(nm, title, sizeof(title));
                    uint32_t off = scanOut.position();
                    scanOut.print(full); scanOut.print('\t'); scanOut.print(title); scanOut.print('\t'); scanOut.print(artist); scanOut.print('\t'); scanOut.print(album); scanOut.print('\n');
                    idxHash[scanTracks] = fnv1a(full);
                    idxOffset[scanTracks] = off;
                    scanTracks++;
                } else scanSkipped++;
                if (millis() - scanLastDraw > 150) { scanLastDraw = millis(); drawScanProgress(scanTracks, "Scanning"); }
            }
            e = dir.openNextFile();
        }
        dir.close();
    }
    for (int k = 0; ; k++) {
        File dir = SD.open(path);
        if (!dir) return;
        scanNameBuf[0] = '\0';
        int seen = 0;
        File e = dir.openNextFile();
        while (e) {
            const char* nm = baseName(e.name());
            if (e.isDirectory() && nm[0] != '.') {
                if (seen == k) { strncpy(scanNameBuf, nm, MY_PATH_MAX - 1); scanNameBuf[MY_PATH_MAX - 1] = '\0'; e.close(); break; }
                seen++;
            }
            e.close();
            e = dir.openNextFile();
        }
        dir.close();
        if (!scanNameBuf[0]) return;
        joinPath(full, MY_PATH_MAX, path, scanNameBuf);
        if (strlen(full) < MY_PATH_MAX - 2) scanDir(full, level + 1);
    }
}

static void stopPlayback();
static void runScan() {
    if (playState != STOPPED) { stopPlayback(); playState = STOPPED; nowPlaying[0] = '\0'; }
    closeIndex();
    SD.mkdir(CFG_DIR);
    SD.remove(IDX_TXT); SD.remove(IDX_BIN);
    scanOut = SD.open(IDX_TXT, FILE_WRITE);
    scanTracks = 0; scanSkipped = 0; scanLastDraw = 0;
    drawScanProgress(0, "Scanning");
    if (scanOut) {
        scanDir(cfgMusicDir, 0);
        scanOut.close();
        idxCount = scanTracks;
        sortIndex();
        File b = SD.open(IDX_BIN, FILE_WRITE);
        if (b) {
            uint32_t n = (uint32_t)idxCount;
            b.write((const uint8_t*)"EMIX", 4);
            b.write((const uint8_t*)&n, 4);
            for (int i = 0; i < idxCount; i++) { b.write((const uint8_t*)&idxHash[i], 4); b.write((const uint8_t*)&idxOffset[i], 4); }
            b.close();
        }
        drawScanProgress(scanTracks, scanSkipped ? "Done (full)" : "Done");
    } else {
        drawScanProgress(0, "Scan failed");
    }
    loadIndex();
    delay(700);
    needsFullRedraw = true;
}

static char npTitle[TITLE_MAX] = "";
static char npArtist[TITLE_MAX] = "";
static char npAlbum[TITLE_MAX] = "";
static char npLine2[2 * TITLE_MAX + 4] = "";
static void buildLine2() {
    if (npArtist[0] && npAlbum[0]) snprintf(npLine2, sizeof(npLine2), "%s | %s", npArtist, npAlbum);
    else if (npArtist[0]) snprintf(npLine2, sizeof(npLine2), "%s", npArtist);
    else if (npAlbum[0]) snprintf(npLine2, sizeof(npLine2), "%s", npAlbum);
    else npLine2[0] = '\0';
}

static void stopPlayback() {
    if (decoder) { if (decoder->isRunning()) decoder->stop(); delete decoder; decoder = nullptr; }
    if (id3)  { delete id3;  id3  = nullptr; }
    if (file) { delete file; file = nullptr; }
    M5Cardputer.Speaker.stop();
}

static uint32_t id3TagBytes(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    uint8_t h[10];
    int n = f.read(h, 10);
    f.close();
    if (n != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    uint32_t sz = 10 + syncsafe(h + 6);
    if (h[3] == 4 && (h[5] & 0x10)) sz += 10;
    return sz;
}

static void playQueuePos(int pos) {
    if (queueCount == 0) return;
    if (pos < 0) pos = queueCount - 1;
    if (pos >= queueCount) pos = 0;
    queuePos = pos;

    const char* full = queueName(pos);

    stopPlayback();
    curArtist[0] = curTitle[0] = curAlbum[0] = '\0';
    curFormat = formatForName(full);
    uint32_t tagBytes = id3TagBytes(full);
    readTags(full, curTitle, sizeof(curTitle), curArtist, sizeof(curArtist), curAlbum, sizeof(curAlbum));
    file = new AudioFileSourceSD(full);
    if (tagBytes && tagBytes + 4096 < file->getSize()) file->seek((int32_t)tagBytes, SEEK_SET);
    Serial.printf("open %s size=%lu id3=%lu\n", full, (unsigned long)file->getSize(), (unsigned long)tagBytes);
    id3  = new AudioFileSourceID3(file);
    id3->RegisterMetadataCB(mp3MetadataCB, nullptr);
    switch (curFormat) {
        case FMT_FLAC: decoder = new AudioGeneratorFLAC(); break;
        case FMT_WAV:  decoder = new AudioGeneratorWAV();  break;
        case FMT_AAC:  decoder = new AudioGeneratorAAC();  break;
        default:       decoder = new AudioGeneratorMP3();  break;
    }
    bool ok = decoder->begin(id3, out);
    Serial.printf("begin=%d\n", ok ? 1 : 0);
    if (ok) {
        playState = PLAYING;
        strncpy(nowPlaying, baseName(full), sizeof(nowPlaying) - 1);
        nowPlaying[sizeof(nowPlaying) - 1] = '\0';
        decoder->loop();
        npTitle[0] = npArtist[0] = npAlbum[0] = '\0';
        indexLookup(full, npTitle, sizeof(npTitle), npArtist, sizeof(npArtist), npAlbum, sizeof(npAlbum));
        if (!npTitle[0]) {
            if (curTitle[0]) takeField(npTitle, sizeof(npTitle), curTitle);
            else stripExt(nowPlaying, npTitle, sizeof(npTitle));
        }
        if (!npArtist[0]) takeField(npArtist, sizeof(npArtist), curArtist);
        if (!npAlbum[0])  takeField(npAlbum,  sizeof(npAlbum),  curAlbum);
        buildLine2();
    } else {
        stopPlayback(); playState = STOPPED; nowPlaying[0] = '\0'; npTitle[0] = npLine2[0] = '\0';
    }
    redrawNowPlaying = true; redrawQueue = true;
}

static int shuffleOrder[QUEUE_MAX];
static int shufflePos = 0;
static void buildShuffle(int firstQueueIdx) {
    for (int i = 0; i < queueCount; i++) shuffleOrder[i] = i;
    for (int i = queueCount - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        int t = shuffleOrder[i]; shuffleOrder[i] = shuffleOrder[j]; shuffleOrder[j] = t;
    }
    for (int i = 0; i < queueCount; i++) {
        if (shuffleOrder[i] == firstQueueIdx) { int t = shuffleOrder[0]; shuffleOrder[0] = shuffleOrder[i]; shuffleOrder[i] = t; break; }
    }
    shufflePos = 0;
}
static bool hasNextTrack() { return cfgShuffle ? (shufflePos + 1 < queueCount) : (queuePos + 1 < queueCount); }
static void nextTrack() {
    if (!cfgShuffle) { playQueuePos(queuePos + 1); return; }
    if (queueCount == 0) return;
    shufflePos = (shufflePos + 1) % queueCount;
    playQueuePos(shuffleOrder[shufflePos]);
}
static void prevTrack() {
    if (!cfgShuffle) { playQueuePos(queuePos - 1); return; }
    if (queueCount == 0) return;
    shufflePos = (shufflePos - 1 + queueCount) % queueCount;
    playQueuePos(shuffleOrder[shufflePos]);
}
static void toggleShuffle() {
    cfgShuffle = !cfgShuffle;
    if (cfgShuffle && queueCount) buildShuffle(queuePos);
    saveConfig();
    redrawNowPlaying = true;
}

static const int SEEK_STEP_PCT = 5;
static void seekBy(int pct) {
    if (playState == STOPPED || !file) return;
    uint32_t sz = file->getSize();
    if (sz < 4096) return;
    int64_t pos = (int64_t)file->getPos() + (int64_t)sz * pct / 100;
    if (pos < 0) pos = 0;
    if (pos > (int64_t)sz - 4096) pos = (int64_t)sz - 4096;
    pos &= ~(int64_t)3;
    file->seek((int32_t)pos, SEEK_SET);
    redrawNowPlaying = true;
}

static void togglePause() {
    if (playState == PLAYING) playState = PAUSED;
    else if (playState == PAUSED) playState = PLAYING;
    redrawNowPlaying = true;
}

static void enterFolder(int viewIdx) {
    if (depth >= MAX_DEPTH - 1) return;
    cursorStack[depth] = cursor; scrollStack[depth] = scroll;
    const char* nm = nameAt(viewIdx);
    char next[MY_PATH_MAX];
    joinPath(next, sizeof(next), currentPath, nm);
    if (strlen(next) >= MY_PATH_MAX - 2) return;
    strcpy(currentPath, next);
    depth++;
    loadDir();
    redrawBrowser = true;
}
static void goBack() {
    if (depth == 0) return;
    char* s = strrchr(currentPath, '/');
    if (s == currentPath) currentPath[1] = '\0';
    else if (s) *s = '\0';
    depth--;
    loadDir();
    cursor = cursorStack[depth]; scroll = scrollStack[depth];
    if (cursor >= entryCount) cursor = entryCount ? entryCount - 1 : 0;
    redrawBrowser = true;
}

static const int QCACHE = 8;
static int  qcIdx[QCACHE];
static char qcTitle[QCACHE][TITLE_MAX];
static int  qcNext = 0;
static void queueCacheReset() { for (int i = 0; i < QCACHE; i++) qcIdx[i] = -1; qcNext = 0; }
static const char* queueTitle(int i) {
    for (int k = 0; k < QCACHE; k++) if (qcIdx[k] == i) return qcTitle[k];
    int slot = qcNext; qcNext = (qcNext + 1) % QCACHE;
    displayTitleFor(queueName(i), qcTitle[slot], TITLE_MAX);
    qcIdx[slot] = i;
    return qcTitle[slot];
}

static void playPath(const char* full) {
    char folder[MY_PATH_MAX];
    strncpy(folder, full, sizeof(folder) - 1); folder[sizeof(folder) - 1] = '\0';
    char* sl = strrchr(folder, '/');
    if (sl == folder) folder[1] = '\0'; else if (sl) *sl = '\0';
    buildQueue(folder);
    queueCacheReset();
    int startPos = 0;
    for (int i = 0; i < queueCount; i++) {
        if (strcmp(queueName(i), full) == 0) { startPos = i; break; }
    }
    if (cfgShuffle) buildShuffle(startPos);
    playQueuePos(startPos);
}

static int queueInsert(const char* full, int at) {
    int len = strlen(full);
    if (queueCount >= QUEUE_MAX || (queuePoolUsed + len + 1) >= QNAME_POOL) return -1;
    bool wasEmpty = (queueCount == 0);
    uint16_t poolOff = queuePoolUsed;
    memcpy(&queuePool[queuePoolUsed], full, len + 1);
    queuePoolUsed += len + 1;
    int newIdx;
    if (!cfgShuffle) {
        if (at < 0 || at > queueCount) at = queueCount;
        for (int i = queueCount; i > at; i--) queueOffset[i] = queueOffset[i - 1];
        queueOffset[at] = poolOff;
        queueCount++;
        if (!wasEmpty && at <= queuePos) queuePos++;
        newIdx = at;
    } else {
        queueOffset[queueCount] = poolOff;
        newIdx = queueCount;
        queueCount++;
        if (wasEmpty) buildShuffle(0);
        else {
            int lo = shufflePos + 1, hi = newIdx;
            int sat = (at < 0) ? lo + (int)(esp_random() % (hi - lo + 1)) : at;
            if (sat < lo) sat = lo; if (sat > hi) sat = hi;
            for (int i = newIdx; i > sat; i--) shuffleOrder[i] = shuffleOrder[i - 1];
            shuffleOrder[sat] = newIdx;
        }
    }
    queueCacheReset();
    redrawQueue = true;
    return newIdx;
}

static int nextSlot(int k) { return cfgShuffle ? shufflePos + 1 + k : queuePos + 1 + k; }

static void startIfStopped(int idx) {
    if (idx < 0 || playState != STOPPED) return;
    if (cfgShuffle) { for (int i = 0; i < queueCount; i++) if (shuffleOrder[i] == idx) { shufflePos = i; break; } }
    playQueuePos(idx);
}

static void queueAppend(const char* full, bool next) {
    int idx = queueInsert(full, next ? nextSlot(0) : -1);
    startIfStopped(idx);
}

static const int FQ_MAX = 256, FQ_POOL = 8192;
static char     fqPool[FQ_POOL];
static uint16_t fqOff[FQ_MAX];
static void queueAppendFolder(const char* folder, bool next) {
    int n = 0, used = 0;
    DIR* dir = openSdDir(folder);
    if (!dir) return;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr && n < FQ_MAX) {
        const char* nm = de->d_name;
        int len = strlen(nm);
        if (nm[0] == '.' || direntIsDir(folder, de) || !isAudioFile(nm) || used + len + 1 >= FQ_POOL) continue;
        fqOff[n++] = used;
        memcpy(&fqPool[used], nm, len + 1);
        used += len + 1;
    }
    closedir(dir);
    for (int i = 1; i < n; i++) {
        uint16_t key = fqOff[i]; int j = i - 1;
        while (j >= 0 && nameCmp(&fqPool[fqOff[j]], &fqPool[key]) > 0) { fqOff[j + 1] = fqOff[j]; j--; }
        fqOff[j + 1] = key;
    }
    int first = -1;
    char full[MY_PATH_MAX];
    for (int i = 0; i < n; i++) {
        joinPath(full, sizeof(full), folder, &fqPool[fqOff[i]]);
        int idx = queueInsert(full, next ? nextSlot(i) : -1);
        if (idx < 0) break;
        if (first < 0) first = idx;
    }
    startIfStopped(first);
}

static bool searchMode = false;
static char searchQuery[25] = "";
static bool searchDirty = false;
static unsigned long searchDirtyAt = 0;
static const int RES_MAX = 64, RES_TITLE = 40;
static uint32_t resRef[RES_MAX];
static char     resTitle[RES_MAX][RES_TITLE];
static int      resCount = 0;
static bool     resFromIndex = false;
static int      savedCursor = 0, savedScroll = 0;

static int listCount() { return searchMode ? resCount : entryCount; }
static const char* listName(int i) { return searchMode ? resTitle[i] : nameAt(i); }
static bool listIsDir(int i) { return searchMode ? false : isDirAt(i); }

static bool ciContains(const char* hay, const char* needleLower) {
    size_t nl = strlen(needleLower);
    if (nl == 0) return true;
    for (const char* h = hay; *h; h++) {
        size_t k = 0;
        while (k < nl && h[k]) {
            char c = h[k]; if (c >= 'A' && c <= 'Z') c += 32;
            if (c != needleLower[k]) break;
            k++;
        }
        if (k == nl) return true;
    }
    return false;
}

static void drawSearchLabel();
static void runSearch() {
    resCount = 0;
    char q[sizeof(searchQuery)];
    for (size_t i = 0; i <= strlen(searchQuery); i++) { char c = searchQuery[i]; q[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
    if (idxCount > 0 && indexFile) {
        resFromIndex = true;
        static uint8_t sbuf[1024];
        static char line[MY_PATH_MAX + 3 * TITLE_MAX + 8];
        int lineLen = 0; uint32_t lineStart = 0, fileOff = 0;
        indexFile.seek(0);
        int n;
        while (resCount < RES_MAX && (n = indexFile.read(sbuf, sizeof(sbuf))) > 0) {
            for (int i = 0; i < n && resCount < RES_MAX; i++) {
                char c = (char)sbuf[i];
                if (c == '\n') {
                    line[lineLen] = '\0';
                    if (ciContains(line, q)) {
                        char* t1 = strchr(line, '\t');
                        char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
                        if (t2) *t2 = '\0';
                        resRef[resCount] = lineStart;
                        if (t1 && t1[1]) takeField(resTitle[resCount], RES_TITLE, t1 + 1);
                        else { if (t1) *t1 = '\0'; stripExt(baseName(line), resTitle[resCount], RES_TITLE); }
                        resCount++;
                    }
                    lineLen = 0; lineStart = fileOff + i + 1;
                } else if (lineLen < (int)sizeof(line) - 1) line[lineLen++] = c;
            }
            fileOff += n;
        }
    } else {
        resFromIndex = false;
        for (int i = 0; i < entryCount && resCount < RES_MAX; i++) {
            if (isDirAt(i) || !ciContains(nameAt(i), q)) continue;
            resRef[resCount] = i;
            stripExt(nameAt(i), resTitle[resCount], RES_TITLE);
            resCount++;
        }
    }
    cursor = 0; scroll = 0;
    redrawBrowser = true;
}

static bool resultPath(int i, char* out, size_t n) {
    if (i < 0 || i >= resCount) return false;
    if (!resFromIndex) { joinPath(out, n, currentPath, nameAt(resRef[i])); return true; }
    if (!indexFile || !indexFile.seek(resRef[i])) return false;
    int len = indexFile.readBytesUntil('\t', out, n - 1);
    out[len] = '\0';
    return len > 0;
}

static void enterSearch(char c) {
    savedCursor = cursor; savedScroll = scroll;
    searchMode = true;
    searchQuery[0] = c; searchQuery[1] = '\0';
    resCount = 0; cursor = 0; scroll = 0;
    searchDirty = true; searchDirtyAt = millis();
    redrawBrowser = true;
    drawSearchLabel();
}
static void exitSearch() {
    searchMode = false; searchQuery[0] = '\0'; searchDirty = false;
    cursor = savedCursor; scroll = savedScroll;
    if (cursor >= entryCount) cursor = entryCount ? entryCount - 1 : 0;
    redrawBrowser = true;
    drawSearchLabel();
}
static void searchType(char c) {
    size_t L = strlen(searchQuery);
    if (L + 1 >= sizeof(searchQuery)) return;
    searchQuery[L] = c; searchQuery[L + 1] = '\0';
    searchDirty = true; searchDirtyAt = millis();
    drawSearchLabel();
}
static void searchBackspace() {
    size_t L = strlen(searchQuery);
    if (L <= 1) { exitSearch(); return; }
    searchQuery[L - 1] = '\0';
    searchDirty = true; searchDirtyAt = millis();
    drawSearchLabel();
}
static bool isSearchChar(char c) { return c >= 0x20 && c < 0x7F; }

static void openSelected() {
    if (searchMode) {
        char full[MY_PATH_MAX];
        if (resultPath(cursor, full, sizeof(full))) playPath(full);
        return;
    }
    if (entryCount == 0) return;
    if (isDirAt(cursor)) { enterFolder(cursor); return; }
    char full[MY_PATH_MAX];
    joinPath(full, sizeof(full), currentPath, nameAt(cursor));
    playPath(full);
}

static void enqueueSelected(bool next) {
    char full[MY_PATH_MAX];
    if (searchMode) { if (resultPath(cursor, full, sizeof(full))) queueAppend(full, next); return; }
    if (entryCount == 0) return;
    joinPath(full, sizeof(full), currentPath, nameAt(cursor));
    if (isDirAt(cursor)) queueAppendFolder(full, next);
    else queueAppend(full, next);
}

static void drawBrowserRow(int idx);
static void moveCursor(int delta) {
    int count = listCount();
    if (count == 0) return;
    int oldCursor = cursor, oldScroll = scroll;
    cursor += delta;
    if (cursor < 0) cursor = count - 1;
    if (cursor >= count) cursor = 0;
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + BROWSER_ROWS) scroll = cursor - BROWSER_ROWS + 1;
    if (scroll != oldScroll) redrawBrowser = true;
    else if (cursor != oldCursor && screenVisible()) { drawBrowserRow(oldCursor); drawBrowserRow(cursor); }
}

static void changeVolume(int d);
static void pollScroll() {
    unsigned long now = millis();
    if (!scrollPresent) {
        if (now - scrollLastProbe < 2000) return;
        scrollLastProbe = now;
        uint8_t fw = 0;
        scrollPresent = M5.Ex_I2C.readRegister(SCROLL_ADDR, SCROLL_FW_REG, &fw, 1, SCROLL_FREQ);
        return;
    }
    if (now - scrollLastPoll < 30) return;
    scrollLastPoll = now;
    uint8_t b[2];
    if (!M5.Ex_I2C.readRegister(SCROLL_ADDR, SCROLL_INC_REG, b, 2, SCROLL_FREQ)) { scrollPresent = false; scrollLastProbe = now; return; }
    int16_t delta = (int16_t)(b[0] | (b[1] << 8));
    if (delta == 0) return;
    lastInputTime = now;
    int steps = delta < 0 ? -delta : delta;
    if (steps > 5) steps = 5;
    if (!screenVisible()) { changeVolume((delta > 0 ? +8 : -8) * steps); return; }
    for (int i = 0; i < steps; i++) moveCursor(delta > 0 ? +1 : -1);
}

static void drawNowPlayingStatus();
static void changeVolume(int d) {
    volume += d;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    M5Cardputer.Speaker.setVolume(volume);
    redrawNowPlaying = true;
}

static void changeBrightness(int d) {
    cfgBrightness += d;
    if (cfgBrightness < 0) cfgBrightness = 0;
    if (cfgBrightness > 255) cfgBrightness = 255;
    applyBacklight();
    saveConfig();
}

static bool glyphExists(uint32_t cp) {
    if (cp > 0xFFFF) return false;
    lgfx::FontMetrics fm;
    return FONT->updateFontMetric(&fm, (uint16_t)cp);
}

static void fontSanitize(const char* in, char* out, size_t outSize) {
    size_t o = 0;
    const uint8_t* p = (const uint8_t*)in;
    while (*p && o + 1 < outSize) {
        uint8_t c = *p;
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { p++; out[o++] = '?'; continue; }
        bool bad = false;
        for (int i = 1; i < len; i++) { if ((p[i] & 0xC0) != 0x80) { bad = true; break; } cp = (cp << 6) | (p[i] & 0x3F); }
        if (bad) { p++; out[o++] = '?'; continue; }
        p += len;
        if (!glyphExists(cp)) { out[o++] = '?'; continue; }
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { if (o + 2 >= outSize) break; out[o++] = 0xC0 | (cp >> 6); out[o++] = 0x80 | (cp & 0x3F); }
        else { if (o + 3 >= outSize) break; out[o++] = 0xE0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 0x3F); out[o++] = 0x80 | (cp & 0x3F); }
    }
    out[o] = '\0';
}

static String trimToWidth(LovyanGFX &d, const char* text, int maxW) {
    char clean[2 * TITLE_MAX + 4];
    fontSanitize(text, clean, sizeof(clean));
    String s(clean);
    while (s.length() > 0 && d.textWidth(s.c_str()) > maxW) {
        int n = s.length() - 1;
        while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
        s.remove(n);
    }
    return s;
}

static void drawPaneFrames() {
    auto &d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    const int pane[3][4] = { {LEFT_X, LEFT_Y, LEFT_W, LEFT_H}, {RIGHT_X, NP_Y, RIGHT_W, NP_H}, {RIGHT_X, Q_Y, RIGHT_W, Q_H} };
    for (int i = 0; i < 3; i++) {
        d.drawRect(pane[i][0], pane[i][1], pane[i][2], pane[i][3], COL_ACCENT);
        d.drawRect(pane[i][0] + 1, pane[i][1] + 1, pane[i][2] - 2, pane[i][3] - 2, COL_ACCENT);
    }
}

static const int MARQUEE_STEP = 1, MARQUEE_INTERVAL_MS = 35, MARQUEE_GAP = 28, MARQUEE_HOLD_MS = 900;
struct Marquee {
    bool active = false, scrolling = false, centered = false;
    int x = 0, y = 0, w = 0;
    uint16_t fg = 0, bg = 0;
    char text[2 * TITLE_MAX + 4] = "";
    int textW = 0, scrollX = 0;
    unsigned long holdUntil = 0, lastMove = 0;
    void (*after)() = nullptr;
};
static Marquee mqTitle, mqArtist, mqBrowser, mqQueue;
static const int MQ_MAX_W = 108;
static M5Canvas mqCanvas(&M5Cardputer.Display);

static void marqueeDraw(Marquee &m) {
    auto &d = M5Cardputer.Display;
    mqCanvas.fillSprite(m.bg);
    mqCanvas.setTextColor(m.fg);
    if (m.scrolling) {
        mqCanvas.setCursor(m.scrollX, 1); mqCanvas.print(m.text);
        mqCanvas.setCursor(m.scrollX + m.textW + MARQUEE_GAP, 1); mqCanvas.print(m.text);
    } else {
        mqCanvas.setCursor(m.centered ? (m.w - m.textW) / 2 : 0, 1); mqCanvas.print(m.text);
    }
    d.setClipRect(m.x, m.y, m.w, ROW_H);
    mqCanvas.pushSprite(m.x, m.y);
    d.clearClipRect();
    if (m.after) m.after();
}

static void marqueeSet(Marquee &m, const char* text, int x, int y, int w, uint16_t fg, uint16_t bg, bool centered, void (*after)()) {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    fontSanitize(text, m.text, sizeof(m.text));
    m.x = x; m.y = y; m.w = w; m.fg = fg; m.bg = bg; m.centered = centered; m.after = after;
    m.textW = d.textWidth(m.text);
    m.scrolling = m.textW > w;
    m.scrollX = 0;
    m.holdUntil = millis() + MARQUEE_HOLD_MS;
    m.lastMove = millis();
    m.active = true;
    marqueeDraw(m);
}

static void marqueeTickOne(Marquee &m, unsigned long now) {
    if (!m.active || !m.scrolling) return;
    if (now < m.holdUntil) return;
    if (now - m.lastMove < MARQUEE_INTERVAL_MS) return;
    m.lastMove = now;
    m.scrollX -= MARQUEE_STEP;
    if (m.scrollX <= -(m.textW + MARQUEE_GAP)) { m.scrollX = 0; m.holdUntil = now + MARQUEE_HOLD_MS; }
    marqueeDraw(m);
}

static void marqueeTickAll() {
    unsigned long now = millis();
    marqueeTickOne(mqTitle, now);
    marqueeTickOne(mqArtist, now);
    marqueeTickOne(mqBrowser, now);
    marqueeTickOne(mqQueue, now);
}

static void drawBrowserOutline() {
    int row = cursor - scroll;
    if (row < 0 || row >= BROWSER_ROWS) return;
    M5Cardputer.Display.drawRoundRect(LC_X, LC_Y + row * ROW_H, LC_W, ROW_H, 3, COL_ACCENT);
}

static void drawBrowserRow(int idx) {
    auto &d = M5Cardputer.Display;
    int row = idx - scroll;
    if (row < 0 || row >= BROWSER_ROWS) return;
    int y = LC_Y + row * ROW_H;
    d.setFont(FONT);
    d.fillRect(LC_X, y, LC_W, ROW_H, COL_BG);
    if (idx >= listCount()) return;
    uint16_t fg = listIsDir(idx) ? COL_ACCENT : COL_TEXT;
    if (idx == cursor) {
        marqueeSet(mqBrowser, listName(idx), LC_X + 3, y, LC_W - 6, fg, COL_BG, false, drawBrowserOutline);
    } else {
        d.setTextColor(fg);
        d.setCursor(LC_X + 3, y + 1);
        d.print(trimToWidth(d, listName(idx), LC_W - 6));
    }
}

static void drawBrowser() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    d.fillRect(LC_X, LC_Y, LC_W, LC_H, COL_BG);
    mqBrowser.active = false;
    if (searchMode) {
        if (resCount == 0) {
            d.setTextColor(COL_DIM);
            d.setCursor(LC_X, LC_Y + 1); d.print(searchDirty ? "searching" : "(no matches)");
            return;
        }
        for (int r = 0; r < BROWSER_ROWS; r++) drawBrowserRow(scroll + r);
        return;
    }
    if (!rootOk) {
        d.setTextColor(COL_ACCENT);
        d.setCursor(LC_X, LC_Y + 1); d.print("music_dir missing:");
        d.setTextColor(COL_TEXT);
        d.setCursor(LC_X, LC_Y + ROW_H + 1); d.print(trimToWidth(d, cfgMusicDir, LC_W));
        return;
    }
    if (entryCount == 0) {
        d.setTextColor(COL_DIM);
        d.setCursor(LC_X, LC_Y + 1); d.print("(empty)");
        return;
    }
    for (int r = 0; r < BROWSER_ROWS; r++) drawBrowserRow(scroll + r);
}

static void drawSearchLabel() {
    if (!screenVisible()) return;
    auto &d = M5Cardputer.Display;
    d.fillRect(LEFT_X, 0, LEFT_W, LC_Y, COL_BG);
    d.fillRect(LEFT_X, LEFT_Y, LEFT_W, BORDER, COL_ACCENT);
    d.fillRect(LEFT_X, LEFT_Y, BORDER, LC_Y - LEFT_Y, COL_ACCENT);
    d.fillRect(LEFT_X + LEFT_W - BORDER, LEFT_Y, BORDER, LC_Y - LEFT_Y, COL_ACCENT);
    if (!searchMode) return;
    d.setFont(&fonts::Font0);
    String label = trimToWidth(d, searchQuery, LEFT_W - 12);
    int w = d.textWidth(label.c_str()) + 4;
    int x = LEFT_X + 4;
    d.fillRect(x, 0, w, LC_Y, COL_BG);
    d.setTextColor(COL_ACCENT);
    d.setCursor(x + 2, 1);
    d.print(label);
    d.setFont(FONT);
}

static const int NP_LINE1_Y = NC_Y, NP_LINE2_Y = NC_Y + ROW_H, NP_STATUS_Y = NC_Y + 2 * ROW_H + 2;

static int progressPercent() {
    if (!file) return 0;
    uint32_t sz = file->getSize(), pos = file->getPos();
    if (sz == 0) return 0;
    if (pos > sz) pos = sz;
    return (int)((uint64_t)pos * 100 / sz);
}

static void drawNowPlayingStatus() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    int y = NP_STATUS_Y;
    d.fillRect(NC_X, y, NC_W, NC_H - (y - NC_Y), COL_BG);
    if (playState == STOPPED) {
        d.setTextColor(COL_DIM);
        d.setCursor(NC_X, y + 1); d.print("stopped");
        return;
    }
    int gx = NC_X, gy = y + 1;
    if (playState == PLAYING) d.fillTriangle(gx, gy, gx, gy + 10, gx + 8, gy + 5, COL_ACCENT);
    else { d.fillRect(gx, gy, 3, 11, COL_ACCENT); d.fillRect(gx + 5, gy, 3, 11, COL_ACCENT); }
    d.setTextColor(COL_TEXT);
    d.setCursor(NC_X + 14, y + 1);
    d.printf("%d%%", progressPercent());
    char vol[20];
    snprintf(vol, sizeof(vol), "%svol %d", cfgShuffle ? "shuf " : "", volume * 100 / 255);
    d.setTextColor(COL_DIM);
    d.setCursor(NC_X + NC_W - d.textWidth(vol), y + 1);
    d.print(vol);
}

static void drawNowPlaying() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    d.fillRect(NC_X, NC_Y, NC_W, NC_H, COL_BG);
    if (playState == STOPPED) {
        mqTitle.active = mqArtist.active = false;
        d.setTextColor(COL_DIM);
        d.setCursor(NC_X + (NC_W - d.textWidth("ember")) / 2, NP_LINE1_Y + 1); d.print("ember");
        drawNowPlayingStatus();
        return;
    }
    marqueeSet(mqTitle,  npTitle, NC_X, NP_LINE1_Y, NC_W, COL_TEXT, COL_BG, true, nullptr);
    marqueeSet(mqArtist, npLine2, NC_X, NP_LINE2_Y, NC_W, COL_DIM,  COL_BG, true, nullptr);
    drawNowPlayingStatus();
}

static void drawQueue() {
    auto &d = M5Cardputer.Display;
    d.setFont(FONT);
    d.fillRect(QC_X, QC_Y, QC_W, QC_H, COL_BG);
    mqQueue.active = false;
    if (queueCount == 0) {
        d.setTextColor(COL_DIM);
        d.setCursor(QC_X, QC_Y + 1); d.print("queue empty");
        return;
    }
    int start = queuePos - QUEUE_ROWS / 2;
    if (start > queueCount - QUEUE_ROWS) start = queueCount - QUEUE_ROWS;
    if (start < 0) start = 0;
    for (int r = 0; r < QUEUE_ROWS; r++) {
        int i = start + r;
        if (i >= queueCount) break;
        int y = QC_Y + r * ROW_H;
        if (i == queuePos) {
            d.fillRoundRect(QC_X, y, QC_W, ROW_H, 3, COL_ACCENT);
            marqueeSet(mqQueue, queueTitle(i), QC_X + 3, y, QC_W - 6, COL_BG, COL_ACCENT, false, nullptr);
        } else {
            d.setTextColor(COL_TEXT);
            d.setCursor(QC_X + 3, y + 1);
            d.print(trimToWidth(d, queueTitle(i), QC_W - 6));
        }
    }
}

static void drawAll() {
    drawPaneFrames();
    drawBrowser();
    drawSearchLabel();
    drawNowPlaying();
    drawQueue();
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    auto cfg = M5.config();
    cfg.external_speaker.hat_spk = true;
    M5Cardputer.begin(cfg, true);
    delay(100);
    M5.Ex_I2C.begin();

    auto spk_cfg = M5Cardputer.Speaker.config();
    spk_cfg.sample_rate      = 44100;
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    spk_cfg.dma_buf_count    = 8;
    spk_cfg.dma_buf_len      = 256;
    spk_cfg.task_priority    = 3;
    M5Cardputer.Speaker.config(spk_cfg);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(volume);

    auto &d = M5Cardputer.Display;
    d.setRotation(1);
    d.setFont(FONT);
    d.setTextWrap(false);
    mqCanvas.setColorDepth(16);
    mqCanvas.createSprite(MQ_MAX_W, ROW_H);
    mqCanvas.setFont(FONT);
    mqCanvas.setTextWrap(false);
    applyTheme();
    d.fillScreen(COL_BG);

    out = new AudioOutputM5Speaker(&M5Cardputer.Speaker, 0);
    out->begin();

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    bool ok = SD.begin(SD_CS, SPI, 25000000, "/sd", 8);
    if (!ok) ok = SD.begin(SD_CS, SPI, 4000000, "/sd", 8);
    if (!ok) {
        d.setBrightness(cfgBrightness);
        d.setTextColor(TFT_RED, COL_BG);
        d.setCursor(4, 4); d.print("SD init FAILED");
        Serial.println("SD init failed");
        return;
    }
    Serial.println("SD ok");

    loadConfig();
    applyTheme();
    loadIndex();
    queueCacheReset();

    strncpy(currentPath, cfgMusicDir, MY_PATH_MAX - 1);
    currentPath[MY_PATH_MAX - 1] = '\0';
    depth = 0;
    rootOk = loadDir();

    displayOn = true; screenIsOff = false;
    applyBacklight();
    lastInputTime = millis();

    drawAll();
    needsFullRedraw = false;
}

void loop() {
    M5Cardputer.update();

    if (playState == PLAYING && decoder && decoder->isRunning()) {
        if (!decoder->loop()) {
            if (hasNextTrack()) {
                nextTrack();
            } else {
                stopPlayback();
                playState = STOPPED;
                nowPlaying[0] = '\0';
                npTitle[0] = npLine2[0] = '\0';
                redrawNowPlaying = true; redrawQueue = true;
            }
        }
    }

    if (M5Cardputer.BtnA.wasPressed()) {
        lastInputTime = millis();
        if (screenVisible()) displayOn = false;
        else { displayOn = true; screenIsOff = false; needsFullRedraw = true; }
        applyBacklight();
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        lastInputTime = millis();
        auto ks = M5Cardputer.Keyboard.keysState();
        bool fn = ks.fn;
        if (ks.space) { if (searchMode) searchType(' '); else if (!fn) togglePause(); }
        if (ks.del && searchMode) searchBackspace();
        for (char c : ks.word) {
            if (c == ' ') continue;
            if (fn) {
                if      (c == KEY_VOLUP)                        changeBrightness(+16);
                else if (c == KEY_VOLDN_A || c == KEY_VOLDN_B)  changeBrightness(-16);
                else if (c == KEY_SCAN_A || c == KEY_SCAN_B)    runScan();
                else if (c == KEY_ENQUEUE)                      enqueueSelected(true);
                else if (c == KEY_RIGHT)                        seekBy(+SEEK_STEP_PCT);
                else if (c == KEY_LEFT)                         seekBy(-SEEK_STEP_PCT);
                continue;
            }
            if      (c == KEY_UP)    moveCursor(-1);
            else if (c == KEY_DOWN)  moveCursor(+1);
            else if (c == KEY_RIGHT) openSelected();
            else if (c == KEY_LEFT)  { if (searchMode) exitSearch(); else goBack(); }
            else if (c == KEY_ENQUEUE) enqueueSelected(false);
            else if (c == KEY_NEXT)  nextTrack();
            else if (c == KEY_PREV)  prevTrack();
            else if (c == KEY_SHUFFLE) toggleShuffle();
            else if (c == KEY_VOLUP) changeVolume(+15);
            else if (c == KEY_VOLDN_A || c == KEY_VOLDN_B) changeVolume(-15);
            else if (isSearchChar(c)) { if (searchMode) searchType(c); else enterSearch(c); }
        }
    }

    pollScroll();

    if (searchDirty && millis() - searchDirtyAt >= 200) { searchDirty = false; runSearch(); }

    if (displayOn && !screenIsOff && cfgScreenTimeoutMs != 0 && millis() - lastInputTime >= cfgScreenTimeoutMs) {
        screenIsOff = true;
        applyBacklight();
    }

    if (screenVisible()) {
        if (needsFullRedraw) {
            drawAll();
            needsFullRedraw = false;
            redrawBrowser = redrawNowPlaying = redrawQueue = false;
        }
        if (redrawBrowser)    { drawBrowser();    redrawBrowser = false; }
        if (redrawNowPlaying) { drawNowPlaying(); redrawNowPlaying = false; }
        if (redrawQueue)      { drawQueue();      redrawQueue = false; }

        marqueeTickAll();
        static unsigned long lastProgressDraw = 0;
        if (playState != STOPPED && millis() - lastProgressDraw >= 500) {
            lastProgressDraw = millis();
            drawNowPlayingStatus();
        }
    }
}
