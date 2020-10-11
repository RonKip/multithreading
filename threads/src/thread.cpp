#define LIB_NAME "thread"
#define MODULE_NAME "thread"

#include <dmsdk/dlib/thread.h>
#include <dmsdk/sdk.h>
// lualib.h no exposed by defold

#include "math.c"
#include "string.c"
#include "table.c"
#include "base.c"

struct kiprono_threading_Context{
    lua_State* thread_state;
    //reuse the state, instead of recreating in each thread

    const char* result;
  
    const char* arg; //argument to pass to worker
    const char* worker; //background function to run
    
    const char* chunk; //filename or string to evaluate
    bool isfile; //for determing if to dofile or dostring

    bool running; //to control number of threads, only 1 allowed
};


struct kiprono_threading_LuaCallbackInfo
{
    kiprono_threading_LuaCallbackInfo() : m_L(0), m_Callback(LUA_NOREF), m_Self(LUA_NOREF) {}
    lua_State* m_L;
    int        m_Callback;
    int        m_Self;
};

kiprono_threading_Context global_data;
kiprono_threading_LuaCallbackInfo g_MyCallbackInfo;


static void stackDump(lua_State* lua) {
    if(lua == NULL){
        dmLogInfo("-----lua_State* is null-------- ");
        return;
    }
    int i, t;
    int top = lua_gettop(lua);
    dmLogInfo("-----STACK DUMP -------- ")
    for (i = 1; i <= top; i++) {
        t = lua_type(lua, i);
        switch (t) {
            case LUA_TSTRING:
                dmLogInfo("%d : \"%s\"", i, lua_tostring(lua, i));
                break;
            case LUA_TBOOLEAN:
                dmLogInfo("%d : %s", i, lua_toboolean(lua, i) ? "true" : "false");
                break;
            case LUA_TNUMBER:
                dmLogInfo("%d : %d ", i, lua_tonumber(lua, i));
                break;
            default:
                dmLogInfo("%d : Type(%s)", i, lua_typename(lua, i));
                break;
        }
    }
    dmLogInfo("----------------------");
}

static void RegisterCallback(lua_State* L, int index, kiprono_threading_LuaCallbackInfo* cbk)
{
    if(cbk->m_Callback != LUA_NOREF)
    {
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Callback);
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Self);
    }

    cbk->m_L = dmScript::GetMainThread(L);

    luaL_checktype(L, index, LUA_TFUNCTION);
    lua_pushvalue(L, index);
    cbk->m_Callback = dmScript::Ref(L, LUA_REGISTRYINDEX);

    dmScript::GetInstance(L);
    cbk->m_Self = dmScript::Ref(L, LUA_REGISTRYINDEX);
}

static void UnregisterCallback(kiprono_threading_LuaCallbackInfo* cbk)
{
    if(cbk->m_Callback != LUA_NOREF)
    {
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Callback);
        dmScript::Unref(cbk->m_L, LUA_REGISTRYINDEX, cbk->m_Self);
        cbk->m_Callback = LUA_NOREF;
    }
}


static void InvokeCallback(kiprono_threading_LuaCallbackInfo* cbk)
{
    if(cbk->m_Callback == LUA_NOREF)
    {
        return;
    }

    lua_State* L = cbk->m_L;
    int top = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, cbk->m_Callback);

    // Setup self (the script instance)
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbk->m_Self);
    lua_pushvalue(L, -1);

    dmScript::SetInstance(L);
    lua_pushstring(L, global_data.result);

    int number_of_arguments = 2; // instance + 2
    int ret = lua_pcall(L, number_of_arguments, 0, 0);
    if(ret != 0) {
        dmLogError("Error running callback: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    assert(top == lua_gettop(L));
}


static void Worker(void* _ctx)
{
   
    //dmLogInfo("-------------Running Worker---------");
    int status;
    if(global_data.isfile){
        status = luaL_dofile(global_data.thread_state , global_data.chunk);
    }
    else {
        status = luaL_dostring(global_data.thread_state , global_data.chunk);
    }
    
    if(status != 0){
        dmLogInfo("Error loading file/string: %s" , lua_tostring(global_data.thread_state, -1));
        return;
    }

    //stackDump(global_data.thread_state);
    if(global_data.isfile){
        lua_getfield(global_data.thread_state, -1, global_data.worker);
    }
    else {
        lua_getfield(global_data.thread_state, LUA_GLOBALSINDEX, global_data.worker);
    }
 
    lua_pushstring(global_data.thread_state, global_data.arg);

    
    //dmLogInfo("------------New Thread Stack Dump ---------------");
    //stackDump(global_data.thread_state);

    status = lua_pcall(global_data.thread_state, 1, 1, 0);

    //dmLogInfo("------------New Thread Stack Dump After Resume ---------------");
    //stackDump(t);
    if(status == 0) {
       if (lua_type(global_data.thread_state, -1) == LUA_TSTRING) {
           global_data.result = lua_tostring(global_data.thread_state, -1); //return result
       }
       else {
            dmLogInfo("Error : Expecting string return type"); 
       }
    }
    else {
        dmLogInfo("Error : In Executing %s, Error code:%d" , global_data.worker,  status); 
        dmLogInfo("Error : %s" , lua_tostring(global_data.thread_state, -1));
    }
    dmLogInfo("----Thread Done----");

    InvokeCallback(&g_MyCallbackInfo);
    UnregisterCallback(&g_MyCallbackInfo);
    global_data.running = false;
    
}


static int RunThread(lua_State* L){
    
    if (global_data.running){
        dmLogInfo("Error : Another thread is already running");
        return 0;
    }
    
    int top = lua_gettop(L);

    //dmLogInfo("About to execute worker thread");
    //stackDump(L);
    
    for (int i = 1 ; i < 4 ; i++){
        if (lua_type(L, i) != LUA_TSTRING) {
            stackDump(L);
            dmLogError("Error : Expected a string argument : %g", i);
            return 0;
        }
    }

    global_data.chunk    = lua_tostring(L, 1);
    global_data.arg      = lua_tostring(L, 2);
    global_data.worker   = lua_tostring(L, 3);
    global_data.result   = "";

    RegisterCallback(L, 4, &g_MyCallbackInfo);
    
    dmThread::Thread thread = dmThread::New(Worker, 0x80000, 0, "defold_thread");
    //dmLogInfo("Thread is now running.");
    global_data.running = true;
    
    return 0;
}

static int RunThread1(lua_State* L){
    global_data.isfile  = false;
    return RunThread(L);
}

static int RunThread2(lua_State* L){
    global_data.isfile  = true;
    return RunThread(L);
}
// Functions exposed to Lua
static const luaL_reg Module_methods[] =
{
    {"run", RunThread1},
    {"run2", RunThread2},
    {0, 0}
};

static void LuaInit(lua_State* L)
{
    int top = lua_gettop(L);

    // Register lua names
    luaL_register(L, MODULE_NAME, Module_methods);
    lua_pop(L, 1);
    
    assert(top == lua_gettop(L));
}


dmExtension::Result AppInitializeThread(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

dmExtension::Result InitializeThread(dmExtension::Params* params)
{
    lua_State* t = luaL_newstate();
    //luaL_openlibs(t); //no expose by sdk.h

    //Registered basic libraries
    luaL_register(t, "math", mathlib);
    lua_pop(t, 1);

    luaL_register(t, "string", strlib);
    lua_pop(t, 1);

    luaL_register(t, "table", tab_funcs);
    lua_pop(t, 1);

    lua_pushvalue(t, LUA_GLOBALSINDEX);
    lua_setglobal(t, "_G");
    luaL_register(t, "_G", base_funcs); //register globally
    lua_pop(t, 1);
    //you can add your own 

    global_data.thread_state = t;
    global_data.running = false;
    
    // Init Lua
    LuaInit(params->m_L);
    printf("Registered %s Extension\n", MODULE_NAME);
    return dmExtension::RESULT_OK;
}

dmExtension::Result AppFinalizeThread(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

dmExtension::Result FinalizeThread(dmExtension::Params* params)
{
    
    
    lua_close(global_data.thread_state);
    return dmExtension::RESULT_OK;
}


// Defold SDK uses a macro for setting up extension entry points:
//
// DM_DECLARE_EXTENSION(symbol, name, app_init, app_final, init, update, on_event, final)

// MyExtension is the C++ symbol that holds all relevant extension data.
// It must match the name field in the `ext.manifest`
DM_DECLARE_EXTENSION(thread, LIB_NAME, AppInitializeThread, AppFinalizeThread, InitializeThread, 0, 0, FinalizeThread)

