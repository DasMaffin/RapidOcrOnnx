// host.cpp - loads ocr.dll the way MCCP does and runs it on 1920x1080 screenshots.
//
//   ocr_host [path\to\ocr.dll] [samples folder] [--full-range]
//
// It implements just enough of mccp::Lua (a value stack, tables, references) to
// call the module's functions, turns each PNG into an NV12 frame (BT.709 limited
// range, like a capture card; --full-range for 0-255 luma), and reports text,
// score and time per line in "line" mode and per frame in "auto" mode.
// The screenshots come from DasMaffin/Sw-Sh-Help-Overlay, dev/samples/.

#include "mccp_module.h"

#include <windows.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ---- a minimal Lua ---------------------------------------------------------------

struct Table;

struct Value {
    int type = mccp::TNIL;
    bool b = false;
    double n = 0;
    std::string s;
    std::shared_ptr<Table> t;
    mccp::CFunc f = nullptr;
    const mccp::Frame *frame = nullptr;   // a frame is a userdata here

    static Value number(double v) { Value x; x.type = mccp::TNUMBER; x.n = v; return x; }
    static Value string(const std::string &v) { Value x; x.type = mccp::TSTRING; x.s = v; return x; }
    static Value table();
};

// Number and string keys only: all the module uses.
struct Table {
    std::map<double, Value> numbers;
    std::map<std::string, Value> strings;

    Value get(const Value &key) const {
        if (key.type == mccp::TNUMBER) { auto it = numbers.find(key.n); return it == numbers.end() ? Value() : it->second; }
        if (key.type == mccp::TSTRING) { auto it = strings.find(key.s); return it == strings.end() ? Value() : it->second; }
        return Value();
    }
    void set(const Value &key, const Value &value) {
        if (key.type == mccp::TNUMBER) numbers[key.n] = value;
        else if (key.type == mccp::TSTRING) strings[key.s] = value;
    }
    Value get(const char *key) const { return get(Value::string(key)); }
    Value get(int i) const { return get(Value::number(i)); }
    void set(const char *key, const Value &value) { set(Value::string(key), value); }
    void set(int i, const Value &value) { set(Value::number(i), value); }
};

Value Value::table() { Value x; x.type = mccp::TTABLE; x.t = std::make_shared<Table>(); return x; }

std::map<int, Value> gRegistry;   // references, shared by every call
int gNextRef = 1;

class FakeLua : public mccp::Lua {
public:
    std::vector<Value> stack;
    std::string error;   // set by ArgError / Error / a failed Check*

    Value &at(int idx) {
        static Value none;
        none = Value();
        none.type = mccp::TNONE;
        int i = idx > 0 ? idx - 1 : static_cast<int>(stack.size()) + idx;
        return i >= 0 && i < static_cast<int>(stack.size()) ? stack[i] : none;
    }

    int Top() override { return static_cast<int>(stack.size()); }
    void Pop(int n) override { stack.resize(stack.size() - std::min<size_t>(n, stack.size())); }
    void Push(int idx) override { stack.push_back(at(idx)); }

    int GetType(int idx) override { return at(idx).type; }
    const char *GetTypeName(int type) override {
        static const char *names[] = {"no value", "nil", "boolean", "userdata", "number", "string",
                                      "table", "function", "userdata", "thread"};
        return type >= -1 && type <= 8 ? names[type + 1] : "?";
    }
    bool IsType(int idx, int type) override { return at(idx).type == type; }

    double GetNumber(int idx) override {
        Value &v = at(idx);
        return v.type == mccp::TNUMBER ? v.n : v.type == mccp::TSTRING ? atof(v.s.c_str()) : 0;
    }
    long long GetInteger(int idx) override { return static_cast<long long>(GetNumber(idx)); }
    bool GetBool(int idx) override { Value &v = at(idx); return v.type != mccp::TNIL && v.type != mccp::TNONE && !(v.type == mccp::TBOOL && !v.b); }
    const char *GetString(int idx, size_t *len) override {
        Value &v = at(idx);
        if (v.type == mccp::TNUMBER) { char buf[64]; snprintf(buf, sizeof buf, "%.14g", v.n); v = Value::string(buf); }
        if (v.type != mccp::TSTRING) return nullptr;
        if (len) *len = v.s.size();
        return v.s.c_str();
    }
    double CheckNumber(int idx) override {
        if (at(idx).type != mccp::TNUMBER) { ArgError(idx, "number expected"); return 0; }
        return GetNumber(idx);
    }
    long long CheckInteger(int idx) override { return static_cast<long long>(CheckNumber(idx)); }
    const char *CheckString(int idx, size_t *len) override {
        const char *s = GetString(idx, len);
        if (!s) { ArgError(idx, "string expected"); return ""; }
        return s;
    }

    void PushNil() override { stack.push_back(Value()); }
    void PushBool(bool v) override { Value x; x.type = mccp::TBOOL; x.b = v; stack.push_back(x); }
    void PushNumber(double v) override { stack.push_back(Value::number(v)); }
    void PushInteger(long long v) override { stack.push_back(Value::number(static_cast<double>(v))); }
    void PushString(const char *s, size_t len) override {
        stack.push_back(Value::string(len == static_cast<size_t>(-1) ? std::string(s) : std::string(s, len)));
    }
    void PushCFunction(mccp::CFunc f) override { Value x; x.type = mccp::TFUNCTION; x.f = f; stack.push_back(x); }

    void CreateTable() override { stack.push_back(Value::table()); }
    void GetField(int idx, const char *key) override {
        Value t = at(idx);
        stack.push_back(t.t ? t.t->get(key) : Value());
    }
    void SetField(int idx, const char *key) override {
        Value t = at(idx);   // resolve before popping: idx may be relative to the top
        if (t.t) t.t->set(key, stack.back());
        Pop(1);
    }
    void GetTable(int idx) override {
        Value t = at(idx);
        Value key = stack.back();
        stack.back() = t.t ? t.t->get(key) : Value();
    }
    void SetTable(int idx) override {
        Value t = at(idx);
        if (t.t) t.t->set(stack[stack.size() - 2], stack.back());
        Pop(2);
    }
    void PushGlobals() override { static Value globals = Value::table(); stack.push_back(globals); }

    bool Call(int, int) override { PushString("Call is not supported by ocr_host", static_cast<size_t>(-1)); return false; }

    int ReferenceCreate() override {
        Value v = stack.back();
        Pop(1);
        if (v.type == mccp::TNIL) return -1;   // LUA_REFNIL
        gRegistry[gNextRef] = v;
        return gNextRef++;
    }
    void ReferencePush(int ref) override { auto it = gRegistry.find(ref); stack.push_back(it == gRegistry.end() ? Value() : it->second); }
    void ReferenceFree(int ref) override { gRegistry.erase(ref); }

    int ArgError(int arg, const char *msg) override {
        if (error.empty()) error = "bad argument #" + std::to_string(arg) + " (" + msg + ")";
        return 0;
    }
    int Error(const char *msg) override { if (error.empty()) error = msg; return 0; }

    bool ToFrame(int idx, mccp::Frame *out) override {
        Value &v = at(idx);
        if (v.type != mccp::TUSERDATA || !v.frame) return false;
        *out = *v.frame;
        return true;
    }
    bool ToBuffer(int, mccp::Buffer *) override { return false; }
    void Log(const char *text) override { printf("[ocr] %s\n", text); }
};

// Calls a module function with the given arguments. Returns its results, or
// sets `error` when the call ended in a Lua error.
std::vector<Value> call(const Value &fn, std::vector<Value> args, std::string *error = nullptr) {
    FakeLua L;
    L.stack = std::move(args);
    int n = fn.f(&L);
    if (error) *error = L.error;
    else if (!L.error.empty()) printf("  Lua error: %s\n", L.error.c_str());
    if (!L.error.empty()) return {};
    return std::vector<Value>(L.stack.end() - n, L.stack.end());
}

// ---- frames ----------------------------------------------------------------------

struct Nv12 {
    std::vector<uint8_t> bytes;
    mccp::Frame frame{};
    Value value() const { Value v; v.type = mccp::TUSERDATA; v.frame = &frame; return v; }
};

// A PNG as an NV12 frame whose rows are 64 bytes longer than the picture, so a
// wrong pitch shows. Only luma matters to the module; chroma is left grey.
bool loadNv12(const std::string &path, bool fullRange, Nv12 &out) {
    cv::Mat bgr = cv::imread(path, cv::IMREAD_COLOR);
    if (bgr.empty()) return false;
    int w = bgr.cols, h = bgr.rows, pitch = w + 64;
    out.bytes.assign(static_cast<size_t>(pitch) * h * 3 / 2, 128);
    for (int y = 0; y < h; ++y) {
        const uint8_t *src = bgr.ptr<uint8_t>(y);
        uint8_t *dst = out.bytes.data() + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < w; ++x) {
            double b = src[3 * x], g = src[3 * x + 1], r = src[3 * x + 2];
            double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;   // BT.709
            dst[x] = static_cast<uint8_t>(fullRange ? luma + 0.5 : 16 + luma * 219 / 255 + 0.5);
        }
    }
    out.frame = {out.bytes.data(), out.bytes.size(), w, h, pitch, "SDL_PIXELFORMAT_NV12"};
    return true;
}

// ---- the tests -------------------------------------------------------------------

struct LineRegion {
    const char *key;
    int x0, y0, x1, y1;
    const char *expected;
};

struct Sample {
    const char *file;
    std::vector<LineRegion> lines;
};

const std::vector<Sample> kSamples = {
    {"leon_grookey_woods.png", {
        {"dialogue 1", 400, 889, 1492, 956, "The Grass-type Pokémon Grookey, eh?"},
        {"dialogue 2", 400, 967, 1492, 1034, "It’s soothing—like a nice, long walk in the woods."},
        {"name plate", 345, 792, 728, 859, "Leon"}}},
    {"system_scorbunny.png", {
        {"system 1", 530, 888, 1376, 939, "Scorbunny has been added to"},
        {"system 2", 530, 948, 1376, 999, "your party."}}},
    {"subtitle_leon_1.png", {
        {"subtitle 1", 404, 922, 1516, 989, "All right!"},
        {"subtitle 2", 404, 1000, 1516, 1067, "Line up, everyone!"}}},
};

Value gOcr;   // what require("ocr") returned

Value region(int x, int y, int w, int h, const char *mode, const Value &key) {
    Value r = Value::table();
    r.t->set("x", Value::number(x)); r.t->set("y", Value::number(y));
    r.t->set("w", Value::number(w)); r.t->set("h", Value::number(h));
    r.t->set("mode", Value::string(mode));
    r.t->set("key", key);
    return r;
}

Value options(const std::string &lang) {
    Value o = Value::table();
    o.t->set("lang", Value::string(lang));
    return o;
}

// ocr.poll() until a job finishes (or 60 s pass).
Value waitForResult() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<Value> r = call(gOcr.t->get("poll"), {});
        if (!r.empty() && r[0].type == mccp::TTABLE) return r[0];
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Value();
}

// Runs one job `runs` times; returns the last result and the mean ms of all
// runs but the first (which may include loading the language's sessions).
Value runJob(const Nv12 &frame, const Value &regions, const std::string &lang, int runs, double &meanMs) {
    Value result;
    double total = 0;
    for (int i = 0; i < runs; ++i) {
        std::vector<Value> id = call(gOcr.t->get("read"), {frame.value(), regions, options(lang)});
        if (id.empty() || id[0].type != mccp::TNUMBER) { printf("  read failed\n"); return Value(); }
        result = waitForResult();
        if (result.type != mccp::TTABLE) { printf("  no result\n"); return Value(); }
        if (result.t->get("error").type == mccp::TSTRING) { printf("  job error: %s\n", result.t->get("error").s.c_str()); return Value(); }
        if (i > 0 || runs == 1) total += result.t->get("ms").n;
    }
    meanMs = total / (runs > 1 ? runs - 1 : 1);
    return result;
}

void testSample(const std::string &folder, const Sample &sample, const std::string &lang, bool fullRange) {
    Nv12 frame;
    if (!loadNv12(folder + "\\" + sample.file, fullRange, frame)) {
        printf("cannot read %s\\%s\n", folder.c_str(), sample.file);
        return;
    }
    printf("\n== %s  [lang %s]\n", sample.file, lang.c_str());

    // mode = "line": all of the sample's lines in one job.
    Value regions = Value::table();
    for (size_t i = 0; i < sample.lines.size(); ++i) {
        const LineRegion &l = sample.lines[i];
        regions.t->set(static_cast<int>(i + 1), region(l.x0, l.y0, l.x1 - l.x0, l.y1 - l.y0, "line", Value::string(l.key)));
    }
    double ms = 0;
    Value result = runJob(frame, regions, lang, 6, ms);
    if (result.type != mccp::TTABLE) return;
    printf("line mode: %.1f ms per line (mean of 5 jobs of %zu lines)\n", ms / sample.lines.size(), sample.lines.size());
    for (size_t i = 0; i < sample.lines.size(); ++i) {
        Value r = result.t->get("regions").t->get(static_cast<int>(i + 1));
        Value line = r.t->get("lines").t->get(1);
        bool exact = line.t->get("text").s == sample.lines[i].expected;
        printf("  %-11s %.3f %s \"%s\"\n", r.t->get("key").s.c_str(), line.t->get("score").n,
               exact ? "OK  " : "DIFF", line.t->get("text").s.c_str());
        if (!exact) printf("  %-11s       want \"%s\"\n", "", sample.lines[i].expected);
        // The characters an ASCII-only model cannot spell.
        for (const char *c : {u8"’", u8"—", u8"é"}) {
            if (!strstr(sample.lines[i].expected, c)) continue;
            printf("  %-11s       %s %s\n", "", c, line.t->get("text").s.find(c) != std::string::npos ? "read" : "MISSING");
        }
    }

    // mode = "auto": the whole frame.
    Value whole = Value::table();
    whole.t->set(1, region(0, 0, frame.frame.width, frame.frame.height, "auto", Value()));
    result = runJob(frame, whole, lang, 4, ms);
    if (result.type != mccp::TTABLE) return;
    Value lines = result.t->get("regions").t->get(1).t->get("lines");
    printf("auto mode: %.1f ms per 1080p frame (mean of 3), %zu lines\n", ms, lines.t->numbers.size());
    for (auto &entry : lines.t->numbers) {
        const Table &l = *entry.second.t;
        printf("  [%4.0f,%4.0f - %4.0f,%4.0f] %.3f \"%s\"\n", l.get("x0").n, l.get("y0").n, l.get("x1").n,
               l.get("y1").n, l.get("score").n, l.get("text").s.c_str());
    }
}

// Argument errors, the single-slot queue and keys.
void testApi(const std::string &folder) {
    printf("\n== API checks\n");
    Nv12 frame;
    if (!loadNv12(folder + "\\" + kSamples[2].file, false, frame)) return;
    Value read = gOcr.t->get("read");
    std::string error;

    call(read, {Value::number(1), Value::table()}, &error);
    printf("read(1, {})                -> %s\n", error.c_str());
    Value bad = Value::table();
    Value badRegion = Value::table();
    badRegion.t->set("x", Value::number(1));
    bad.t->set(1, badRegion);
    call(read, {frame.value(), bad}, &error);
    printf("read(frame, {{x=1}})       -> %s\n", error.c_str());
    call(read, {frame.value(), Value::table(), options("xx")}, &error);
    printf("read(frame, {}, {lang=xx}) -> %s\n", error.c_str());
    printf("references after errors: %zu (want 0)\n", gRegistry.size());

    // Three reads back to back: one runs, one waits, the third finds the slot taken.
    Value key = Value::table();   // any Lua value comes back as it went in
    Value regions = Value::table();
    regions.t->set(1, region(404, 922, 1112, 67, "line", key));
    for (int i = 0; i < 3; ++i) {
        std::vector<Value> r = call(read, {frame.value(), regions});
        printf("read #%d -> %s\n", i + 1, r.size() == 2 ? ("nil, \"" + r[1].s + "\"").c_str()
                                                       : std::to_string(static_cast<long long>(r[0].n)).c_str());
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));   // both jobs finish; only the newest is kept
    Value result = waitForResult();
    std::vector<Value> again = call(gOcr.t->get("poll"), {});
    printf("after both finished: poll -> id %.0f, then %s\n", result.t->get("id").n,
           again[0].type == mccp::TNIL ? "nil" : "another result");
    Value back = result.t->get("regions").t->get(1).t->get("key");
    printf("key returned untouched: %s\n", back.t == key.t ? "yes (same table)" : "NO");
    printf("references held: %zu (want 0)\n", gRegistry.size());

    // A region partly outside the frame is clamped; one fully outside reads nothing.
    Value edge = Value::table();
    edge.t->set(1, region(1800, 1000, 500, 500, "line", Value()));
    edge.t->set(2, region(-50, -50, 10, 10, "auto", Value()));
    call(read, {frame.value(), edge});
    result = waitForResult();
    Value e1 = result.t->get("regions").t->get(1).t->get("lines").t->get(1);
    printf("clamped region -> [%.0f,%.0f - %.0f,%.0f]; outside region -> %zu lines\n", e1.t->get("x0").n,
           e1.t->get("y0").n, e1.t->get("x1").n, e1.t->get("y1").n,
           result.t->get("regions").t->get(2).t->get("lines").t->numbers.size());
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string dll = "ocr.dll", folder = "samples";
    bool fullRange = false;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--full-range") fullRange = true;
        else positional.push_back(argv[i]);
    }
    if (positional.size() > 0) dll = positional[0];
    if (positional.size() > 1) folder = positional[1];

    HMODULE module = LoadLibraryA(dll.c_str());
    if (!module) { printf("cannot load %s (error %lu)\n", dll.c_str(), GetLastError()); return 1; }
    auto api = reinterpret_cast<int (*)()>(GetProcAddress(module, "mccp_module_api"));
    auto open = reinterpret_cast<int (*)(mccp::Lua *)>(GetProcAddress(module, "mccp_module_open"));
    auto close = reinterpret_cast<int (*)(mccp::Lua *)>(GetProcAddress(module, "mccp_module_close"));
    if (!api || !open) { printf("%s is not an MCCP module\n", dll.c_str()); return 1; }
    printf("module API %d (header %d)\n", api(), MCCP_MODULE_API);

    {
        FakeLua L;
        int n = open(&L);
        if (n < 1 || L.stack.back().type != mccp::TTABLE) { printf("open failed: %s\n", L.error.c_str()); return 1; }
        gOcr = L.stack.back();
    }
    printf("%s\n", call(gOcr.t->get("version"), {})[0].s.c_str());
    Value langs = call(gOcr.t->get("languages"), {})[0];
    std::vector<std::string> languages;
    for (auto &entry : langs.t->numbers) languages.push_back(entry.second.s);
    printf("languages:");
    for (auto &l : languages) printf(" %s", l.c_str());
    printf("\nluma: %s\n", fullRange ? "full range (0-255)" : "limited range (16-235)");

    testApi(folder);
    for (const std::string &lang : languages)
        for (const Sample &sample : kSamples) testSample(folder, sample, lang, fullRange);

    if (close) {
        FakeLua L;
        close(&L);
    }
    printf("\nreferences after close: %zu (want 0)\n", gRegistry.size());
    FreeLibrary(module);
    return 0;
}
