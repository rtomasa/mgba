/* Copyright (c) 2013-2022 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/script/lua.h>

#include <mgba-util/string.h>

#include <lualib.h>
#include <lauxlib.h>

static struct mScriptEngineContext* _luaCreate(struct mScriptEngine2*, struct mScriptContext*);

static void _luaDestroy(struct mScriptEngineContext*);
static struct mScriptValue* _luaGetGlobal(struct mScriptEngineContext*, const char* name);
static bool _luaSetGlobal(struct mScriptEngineContext*, const char* name, struct mScriptValue*);
static bool _luaLoad(struct mScriptEngineContext*, struct VFile*, const char** error);
static bool _luaRun(struct mScriptEngineContext*);
static const char* _luaGetError(struct mScriptEngineContext*);

static bool _luaCall(struct mScriptFrame*, void* context);

struct mScriptEngineContextLua;
static bool _luaPushFrame(struct mScriptEngineContextLua*, struct mScriptList*);
static bool _luaPopFrame(struct mScriptEngineContextLua*, struct mScriptList*);
static bool _luaInvoke(struct mScriptEngineContextLua*, struct mScriptFrame*);

static struct mScriptValue* _luaCoerce(struct mScriptEngineContextLua* luaContext);
static bool _luaWrap(struct mScriptEngineContextLua* luaContext, struct mScriptValue*);

static void _luaDeref(struct mScriptValue*);

static int _luaThunk(lua_State* lua);
static int _luaGetObject(lua_State* lua);
static int _luaSetObject(lua_State* lua);
static int _luaGcObject(lua_State* lua);

#if LUA_VERSION_NUM < 503
#define lua_pushinteger lua_pushnumber
#endif

const struct mScriptType mSTLuaFunc = {
	.base = mSCRIPT_TYPE_FUNCTION,
	.size = 0,
	.name = "lua-" LUA_VERSION_ONLY "::function",
	.details = {
		.function = {
			.parameters = {
				.variable = true
			},
			.returnType = {
				.variable = true
			}
		}
	},
	.alloc = NULL,
	.free = _luaDeref,
	.hash = NULL,
	.equal = NULL,
	.cast = NULL,
};

struct mScriptEngineContextLua {
	struct mScriptEngineContext d;
	lua_State* lua;
	int func;
	char* lastError;
};

struct mScriptEngineContextLuaRef {
	struct mScriptEngineContextLua* context;
	int ref;
};

static struct mScriptEngineLua {
	struct mScriptEngine2 d;
} _engineLua = {
	.d = {
		.name = "lua-" LUA_VERSION_ONLY,
		.init = NULL,
		.deinit = NULL,
		.create = _luaCreate
	}
};

struct mScriptEngine2* const mSCRIPT_ENGINE_LUA = &_engineLua.d;

static const luaL_Reg _mSTStruct[] = {
	{ "__index", _luaGetObject },
	{ "__newindex", _luaSetObject },
	{ "__gc", _luaGcObject },
	{ NULL, NULL }
};

struct mScriptEngineContext* _luaCreate(struct mScriptEngine2* engine, struct mScriptContext* context) {
	UNUSED(engine);
	struct mScriptEngineContextLua* luaContext = calloc(1, sizeof(*luaContext));
	luaContext->d = (struct mScriptEngineContext) {
		.context = context,
		.destroy = _luaDestroy,
		.getGlobal = _luaGetGlobal,
		.setGlobal = _luaSetGlobal,
		.load = _luaLoad,
		.run = _luaRun,
		.getError = _luaGetError
	};
	luaContext->lua = luaL_newstate();
	luaContext->func = -1;

	luaL_openlibs(luaContext->lua);

	luaL_newmetatable(luaContext->lua, "mSTStruct");
#if LUA_VERSION_NUM < 502
	luaL_register(luaContext->lua, NULL, _mSTStruct);
#else
	luaL_setfuncs(luaContext->lua, _mSTStruct, 0);
#endif
	lua_pop(luaContext->lua, 1);

	return &luaContext->d;
}

void _luaDestroy(struct mScriptEngineContext* ctx) {
	struct mScriptEngineContextLua* luaContext = (struct mScriptEngineContextLua*) ctx;
	if (luaContext->lastError) {
		free(luaContext->lastError);
		luaContext->lastError = NULL;
	}
	if (luaContext->func > 0) {
		luaL_unref(luaContext->lua, LUA_REGISTRYINDEX, luaContext->func);
	}
	lua_close(luaContext->lua);
	free(luaContext);
}

struct mScriptValue* _luaGetGlobal(struct mScriptEngineContext* ctx, const char* name) {
	struct mScriptEngineContextLua* luaContext = (struct mScriptEngineContextLua*) ctx;
	lua_getglobal(luaContext->lua, name);
	return _luaCoerce(luaContext);
}

bool _luaSetGlobal(struct mScriptEngineContext* ctx, const char* name, struct mScriptValue* value) {
	struct mScriptEngineContextLua* luaContext = (struct mScriptEngineContextLua*) ctx;
	if (!_luaWrap(luaContext, value)) {
		return false;
	}
	lua_setglobal(luaContext->lua, name);
	return true;
}

struct mScriptValue* _luaCoerceFunction(struct mScriptEngineContextLua* luaContext) {
	struct mScriptValue* value = mScriptValueAlloc(&mSTLuaFunc);
	struct mScriptFunction* fn = calloc(1, sizeof(*fn));
	struct mScriptEngineContextLuaRef* ref = calloc(1, sizeof(*ref));
	fn->call = _luaCall;
	fn->context = ref;
	ref->context = luaContext;
	ref->ref = luaL_ref(luaContext->lua, LUA_REGISTRYINDEX);
	value->value.opaque = fn;
	return value;
}

struct mScriptValue* _luaCoerce(struct mScriptEngineContextLua* luaContext) {
	if (lua_isnone(luaContext->lua, -1)) {
		lua_pop(luaContext->lua, 1);
		return NULL;
	}

	struct mScriptValue* value = NULL;
	switch (lua_type(luaContext->lua, -1)) {
	case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503
		if (lua_isinteger(luaContext->lua, -1)) {
			value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S64);
			value->value.s64 = lua_tointeger(luaContext->lua, -1);
			break;
		}
#endif
		value = mScriptValueAlloc(mSCRIPT_TYPE_MS_F64);
		value->value.f64 = lua_tonumber(luaContext->lua, -1);
		break;
	case LUA_TBOOLEAN:
		value = mScriptValueAlloc(mSCRIPT_TYPE_MS_S32);
		value->value.s32 = lua_toboolean(luaContext->lua, -1);
		break;
	case LUA_TSTRING:
		break;
	case LUA_TFUNCTION:
		return _luaCoerceFunction(luaContext);
	case LUA_TUSERDATA:
		if (!lua_getmetatable(luaContext->lua, -1)) {
			break;
		}
		luaL_getmetatable(luaContext->lua, "mSTStruct");
		if (!lua_rawequal(luaContext->lua, -1, -2)) {
			lua_pop(luaContext->lua, 2);
			break;
		}
		lua_pop(luaContext->lua, 2);
		value = lua_touserdata(luaContext->lua, -1);
	}
	lua_pop(luaContext->lua, 1);
	return value;
}

bool _luaWrap(struct mScriptEngineContextLua* luaContext, struct mScriptValue* value) {
	if (value->type == mSCRIPT_TYPE_MS_WRAPPER) {
		value = mScriptValueUnwrap(value);
	}
	bool ok = true;
	struct mScriptValue* newValue;
	switch (value->type->base) {
	case mSCRIPT_TYPE_SINT:
		if (value->type->size <= 4) {
			lua_pushinteger(luaContext->lua, value->value.s32);
		} else if (value->type->size == 8) {
			lua_pushinteger(luaContext->lua, value->value.s64);
		} else {
			ok = false;
		}
		break;
	case mSCRIPT_TYPE_UINT:
		if (value->type->size <= 4) {
			lua_pushinteger(luaContext->lua, value->value.u32);
		} else if (value->type->size == 8) {
			lua_pushinteger(luaContext->lua, value->value.u64);
		} else {
			ok = false;
		}
		break;
	case mSCRIPT_TYPE_FLOAT:
		if (value->type->size == 4) {
			lua_pushnumber(luaContext->lua, value->value.f32);
		} else if (value->type->size == 8) {
			lua_pushnumber(luaContext->lua, value->value.f64);
		} else {
			ok = false;
		}
		break;
	case mSCRIPT_TYPE_FUNCTION:
		newValue = lua_newuserdata(luaContext->lua, sizeof(*newValue));
		newValue->type = value->type;
		newValue->refs = mSCRIPT_VALUE_UNREF;
		newValue->type->alloc(newValue);
		lua_pushcclosure(luaContext->lua, _luaThunk, 1);
		break;
	case mSCRIPT_TYPE_OBJECT:
		newValue = lua_newuserdata(luaContext->lua, sizeof(*newValue));
		mScriptValueWrap(value, newValue);
		luaL_setmetatable(luaContext->lua, "mSTStruct");
		break;
	default:
		ok = false;
		break;
	}

	return ok;
}

#define LUA_BLOCKSIZE 0x1000
struct mScriptEngineLuaReader {
	struct VFile* vf;
	char block[LUA_BLOCKSIZE];
};

static const char* _reader(lua_State* lua, void* context, size_t* size) {
	UNUSED(lua);
	struct mScriptEngineLuaReader* reader = context;
	ssize_t s = reader->vf->read(reader->vf, reader->block, sizeof(reader->block));
	if (s < 0) {
		return NULL;
	}
	*size = s;
	return reader->block;
}

bool _luaLoad(struct mScriptEngineContext* ctx, struct VFile* vf, const char** error) {
	struct mScriptEngineContextLua* luaContext = (struct mScriptEngineContextLua*) ctx;
	struct mScriptEngineLuaReader data = {
		.vf = vf
	};
	int ret = lua_load(luaContext->lua, _reader, &data, NULL, "t");
	switch (ret) {
	case LUA_OK:
		if (error) {
			*error = NULL;
		}
		luaContext->func = luaL_ref(luaContext->lua, LUA_REGISTRYINDEX);
		return true;
	case LUA_ERRSYNTAX:
		if (error) {
			*error = "Syntax error";
		}
		break;
	default:
		if (error) {
			*error = "Unknown error";
		}
		break;
	}
	return false;
}

bool _luaRun(struct mScriptEngineContext* context) {
	struct mScriptEngineContextLua* luaContext = (struct mScriptEngineContextLua*) context;
	lua_rawgeti(luaContext->lua, LUA_REGISTRYINDEX, luaContext->func);
	return _luaInvoke(luaContext, NULL);
}

const char* _luaGetError(struct mScriptEngineContext* context) {
	struct mScriptEngineContextLua* luaContext = (struct mScriptEngineContextLua*) context;
	return luaContext->lastError;
}

bool _luaPushFrame(struct mScriptEngineContextLua* luaContext, struct mScriptList* frame) {
	bool ok = true;
	if (frame) {
		size_t i;
		for (i = 0; i < mScriptListSize(frame); ++i) {
			if (!_luaWrap(luaContext, mScriptListGetPointer(frame, i))) {
				ok = false;
				break;
			}
		}
	}
	if (!ok) {
		lua_pop(luaContext->lua, lua_gettop(luaContext->lua));
	}
	return ok;
}

bool _luaPopFrame(struct mScriptEngineContextLua* luaContext, struct mScriptList* frame) {
	int count = lua_gettop(luaContext->lua);
	bool ok = true;
	if (frame) {
		int i;
		for (i = 0; i < count; ++i) {
			struct mScriptValue* value = _luaCoerce(luaContext);
			if (!value) {
				ok = false;
				break;
			}
			mScriptValueWrap(value, mScriptListAppend(frame));
			mScriptValueDeref(value);
		}
		if (count > i) {
			lua_pop(luaContext->lua, count - i);
		}

		if (ok) {
			for (i = 0; i < (ssize_t) (mScriptListSize(frame) / 2); ++i) {
				struct mScriptValue buffer;
				memcpy(&buffer, mScriptListGetPointer(frame, i), sizeof(buffer));
				memcpy(mScriptListGetPointer(frame, i), mScriptListGetPointer(frame, mScriptListSize(frame) - i - 1), sizeof(buffer));
				memcpy(mScriptListGetPointer(frame, mScriptListSize(frame) - i - 1), &buffer, sizeof(buffer));
			}
		}
	}
	return ok;
}

bool _luaCall(struct mScriptFrame* frame, void* context) {
	struct mScriptEngineContextLuaRef* ref = context;
	lua_rawgeti(ref->context->lua, LUA_REGISTRYINDEX, ref->ref);
	if (!_luaInvoke(ref->context, frame)) {
		return false;
	}
	return true;
}

bool _luaInvoke(struct mScriptEngineContextLua* luaContext, struct mScriptFrame* frame) {
	int nargs = 0;
	if (frame) {
		nargs = mScriptListSize(&frame->arguments);
	}

	if (luaContext->lastError) {
		free(luaContext->lastError);
		luaContext->lastError = NULL;
	}

	if (frame && !_luaPushFrame(luaContext, &frame->arguments)) {
		return false;
	}

	lua_pushliteral(luaContext->lua, "mCtx");
	lua_pushlightuserdata(luaContext->lua, luaContext);
	lua_rawset(luaContext->lua, LUA_REGISTRYINDEX);
	int ret = lua_pcall(luaContext->lua, nargs, LUA_MULTRET, 0);
	lua_pushliteral(luaContext->lua, "mCtx");
	lua_pushnil(luaContext->lua);
	lua_rawset(luaContext->lua, LUA_REGISTRYINDEX);

	if (ret == LUA_ERRRUN) {
		luaContext->lastError = strdup(lua_tostring(luaContext->lua, -1));
		lua_pop(luaContext->lua, 1);
	}
	if (ret) {
		return false;
	}

	if (frame && !_luaPopFrame(luaContext, &frame->returnValues)) {
		return false;
	}

	return true;
}

void _luaDeref(struct mScriptValue* value) {
	struct mScriptEngineContextLuaRef* ref;
	if (value->type->base == mSCRIPT_TYPE_FUNCTION) {
		struct mScriptFunction* function = value->value.opaque;
		ref = function->context;
		free(function);
	} else {
		return;
	}
	luaL_unref(ref->context->lua, LUA_REGISTRYINDEX, ref->ref);
	free(ref);
}

static struct mScriptEngineContextLua* _luaGetContext(lua_State* lua) {
	lua_pushliteral(lua, "mCtx");
	int type = lua_rawget(lua, LUA_REGISTRYINDEX);
	if (type != LUA_TLIGHTUSERDATA) {
		lua_pop(lua, 1);
		lua_pushliteral(lua, "Function called from invalid context");
		lua_error(lua);
	}

	struct mScriptEngineContextLua* luaContext = lua_touserdata(lua, -1);
	lua_pop(lua, 1);
	if (luaContext->lua != lua) {
		lua_pushliteral(lua, "Function called from invalid context");
		lua_error(lua);
	}
	return luaContext;
}

int _luaThunk(lua_State* lua) {
	struct mScriptEngineContextLua* luaContext = _luaGetContext(lua);
	struct mScriptFrame frame;
	mScriptFrameInit(&frame);
	if (!_luaPopFrame(luaContext, &frame.arguments)) {
		mScriptFrameDeinit(&frame);
		lua_pushliteral(lua, "Error calling function (setting arguments)");
		lua_error(lua);
	}

	struct mScriptValue* fn = lua_touserdata(lua, lua_upvalueindex(1));
	if (!fn || !mScriptInvoke(fn, &frame)) {
		mScriptFrameDeinit(&frame);
		lua_pushliteral(lua, "Error calling function (invoking)");
		lua_error(lua);
	}

	if (!_luaPushFrame(luaContext, &frame.returnValues)) {
		mScriptFrameDeinit(&frame);
		lua_pushliteral(lua, "Error calling function (getting return values)");
		lua_error(lua);
	}
	mScriptFrameDeinit(&frame);

	return lua_gettop(luaContext->lua);
}

int _luaGetObject(lua_State* lua) {
	struct mScriptEngineContextLua* luaContext = _luaGetContext(lua);
	const char* key = lua_tostring(lua, -1);
	struct mScriptValue* obj = lua_touserdata(lua, -2);
	struct mScriptValue val;

	if (!mScriptObjectGet(obj, key, &val)) {
		lua_pop(lua, 2);
		lua_pushliteral(lua, "Invalid key");
		lua_error(lua);
	}

	lua_pop(lua, 2);
	if (!_luaWrap(luaContext, &val)) {
		lua_pushliteral(lua, "Invalid value");
		lua_error(lua);
	}
	return 1;
}


int _luaSetObject(lua_State* lua) {
	struct mScriptEngineContextLua* luaContext = _luaGetContext(lua);
	const char* key = lua_tostring(lua, -2);
	struct mScriptValue* obj = lua_touserdata(lua, -3);
	struct mScriptValue* val = _luaCoerce(luaContext);

	lua_pop(lua, 2);
	if (!val) {
		lua_pushliteral(lua, "Invalid value");
		lua_error(lua);
	}

	if (!mScriptObjectSet(obj, key, val)) {
		mScriptValueDeref(val);
		lua_pushliteral(lua, "Invalid key");
		lua_error(lua);
	}
	mScriptValueDeref(val);
	return 0;
}

static int _luaGcObject(lua_State* lua) {
	struct mScriptValue* val = lua_touserdata(lua, -1);
	val = mScriptValueUnwrap(val);
	if (!val) {
		return 0;
	}
	mScriptValueDeref(val);
	return 0;
}
