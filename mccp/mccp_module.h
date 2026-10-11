// mccp_module.h - write a native module for MCCP. The only file you need.
//
// A native module is a DLL that Lua mods load with require() and call like any
// other function, the way Garry's Mod binary modules work:
//
//     #include "mccp_module.h"
//
//     LUA_FUNCTION(Add) {
//         LUA->PushNumber(LUA->CheckNumber(1) + LUA->CheckNumber(2));
//         return 1;                                  // values returned to Lua
//     }
//
//     MCCP_MODULE_OPEN() {                           // on the first require
//         LUA->CreateTable();
//         LUA->PushCFunction(Add); LUA->SetField(-2, "add");
//         return 1;                                  // what require() gives back
//     }
//
//     MCCP_MODULE_CLOSE() { return 0; }              // optional; at exit
//
// and in a Lua mod:
//
//     local mymod = require("mymod")                 -- bin/mymod.dll
//     print(mymod.add(1, 2))                         -- 3
//
// Nothing to link: MCCP hands the module an interface (mccp::Lua) of virtual
// functions, so a module never touches Lua's own library and keeps working
// when MCCP updates its Lua.
//
// THE RULES
//   * Only on the thread that called you. LUA is valid during the call it was
//     handed to, on that thread, and nowhere else: never keep it, never use it
//     from a thread of your own. Do slow work on your own thread and let Lua
//     ask for the result (a poll function), or the frame waits for you.
//   * Errors: ArgError/Error, or a Check* that fails, end the call with a Lua
//     error once your function returns - they do not jump out of it, so return
//     right away (`return LUA->ArgError(1, "...")`). A C++ exception that
//     leaves your function is caught and becomes a Lua error too.
//   * Frame and Buffer pointers are valid only during the call that got them.
//   * Stack indices work like Lua's: 1 is the first argument, -1 the top.
//
// The interface only ever grows at the end, and MCCP_MODULE_API counts it. A
// module built against an older header keeps working with a newer MCCP; one
// built against a newer header than the MCCP it runs in is not loaded.
#pragma once
#include <cstddef>
#include <cstdint>

#define MCCP_MODULE_API 1

namespace mccp {

// Lua's own type numbers.
enum Type {
    TNONE = -1, TNIL = 0, TBOOL = 1, TLIGHTUSERDATA = 2, TNUMBER = 3, TSTRING = 4,
    TTABLE = 5, TFUNCTION = 6, TUSERDATA = 7, TTHREAD = 8
};

// A captured frame, as OnFrameReceived hands it to Lua. The bytes are the
// frame itself, not a copy: writing them changes what is shown, exactly as
// frame:setByte() does.
struct Frame {
    uint8_t*    data;     // the pixel buffer
    size_t      size;     // bytes in it (on NV12: the luma plane, then chroma)
    int         width;    // pixels
    int         height;   // pixels
    int         pitch;    // bytes per row of the first plane
    const char* format;   // e.g. "SDL_PIXELFORMAT_NV12"
};

// A Lua mod's buffer.new(n): bytes it owns, read and written in place.
struct Buffer {
    uint8_t* data;
    size_t   size;
};

class Lua;
typedef int (*CFunc)(Lua* LUA);

class Lua {
public:
    // ---- the stack ---------------------------------------------------------
    virtual int  Top() = 0;                        // number of values on it
    virtual void Pop(int n = 1) = 0;
    virtual void Push(int idx) = 0;                // a copy of the value at idx

    // ---- types -------------------------------------------------------------
    virtual int         GetType(int idx) = 0;      // a mccp::Type
    virtual const char* GetTypeName(int type) = 0;
    virtual bool        IsType(int idx, int type) = 0;

    // ---- reading -----------------------------------------------------------
    // Get* never fail: the wrong type reads as 0 / nullptr, and GetBool is
    // Lua's truth (only nil and false are false). Numbers read as strings and
    // numeric strings as numbers, as in Lua. GetInteger truncates 2.7 to 2.
    // GetString's pointer lives as long as the value stays on the stack.
    virtual double      GetNumber(int idx) = 0;
    virtual long long   GetInteger(int idx) = 0;
    virtual bool        GetBool(int idx) = 0;
    virtual const char* GetString(int idx, size_t* len = nullptr) = 0;
    // Check* fail the call (see THE RULES) when the type is wrong, and return
    // 0 / "" meanwhile - never nullptr - so the code up to your return is safe.
    virtual double      CheckNumber(int idx) = 0;
    virtual long long   CheckInteger(int idx) = 0;
    virtual const char* CheckString(int idx, size_t* len = nullptr) = 0;

    // ---- pushing -----------------------------------------------------------
    virtual void PushNil() = 0;
    virtual void PushBool(bool v) = 0;
    virtual void PushNumber(double v) = 0;
    virtual void PushInteger(long long v) = 0;
    // Any bytes, zeros included, when len is given; else up to the first zero.
    virtual void PushString(const char* s, size_t len = static_cast<size_t>(-1)) = 0;
    virtual void PushCFunction(CFunc f) = 0;

    // ---- tables ------------------------------------------------------------
    virtual void CreateTable() = 0;
    virtual void GetField(int idx, const char* key) = 0;   // pushes t[key]
    virtual void SetField(int idx, const char* key) = 0;   // t[key] = top; pops it
    virtual void GetTable(int idx) = 0;                    // key on top -> t[key]
    virtual void SetTable(int idx) = 0;                    // t[key] = value; pops both
    virtual void PushGlobals() = 0;                        // the mods' shared _G

    // ---- calling Lua -------------------------------------------------------
    // The function, then nargs arguments, on top. Protected: true and
    // nresults values, or false and the error message.
    virtual bool Call(int nargs, int nresults) = 0;

    // ---- keeping a value between calls (a callback, a table) ---------------
    virtual int  ReferenceCreate() = 0;            // pops the top, returns its ref
    virtual void ReferencePush(int ref) = 0;
    virtual void ReferenceFree(int ref) = 0;

    // ---- errors ------------------------------------------------------------
    // Both return 0 so they can be returned. The call ends in a Lua error.
    virtual int ArgError(int arg, const char* msg) = 0;
    virtual int Error(const char* msg) = 0;

    // ---- MCCP's own values -------------------------------------------------
    virtual bool ToFrame(int idx, Frame* out) = 0;    // false: not a live frame
    virtual bool ToBuffer(int idx, Buffer* out) = 0;  // false: not a buffer
    virtual void Log(const char* text) = 0;           // MCCP's log, under your name

protected:
    ~Lua() = default;   // MCCP owns it; a module never deletes one
};

}  // namespace mccp

#ifdef _WIN32
#define MCCP_EXPORT extern "C" __declspec(dllexport)
#else
#define MCCP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// A function Lua can call: LUA_FUNCTION(Name) { ...; return <values pushed>; }
#define LUA_FUNCTION(name) static int name(mccp::Lua* LUA)

#define MCCP_MODULE_OPEN()                                          \
    MCCP_EXPORT int mccp_module_api() { return MCCP_MODULE_API; }  \
    MCCP_EXPORT int mccp_module_open(mccp::Lua* LUA)

#define MCCP_MODULE_CLOSE() MCCP_EXPORT int mccp_module_close(mccp::Lua* LUA)
