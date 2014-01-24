/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Object.h"

#include "mozilla/ArrayUtils.h"

#include "jscntxt.h"

#include "frontend/BytecodeCompiler.h"
#include "vm/StringBuffer.h"

#include "jsobjinlines.h"

#include "vm/ObjectImpl-inl.h"

using namespace js;
using namespace js::types;

using js::frontend::IsIdentifier;
using mozilla::ArrayLength;


bool
js::obj_construct(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, nullptr);
    if (args.length() > 0 && !args[0].isNullOrUndefined()) {
        obj = ToObject(cx, args[0]);
        if (!obj)
            return false;
    } else {
        /* Make an object whether this was called with 'new' or not. */
        if (!NewObjectScriptedCall(cx, &obj))
            return false;
    }

    args.rval().setObject(*obj);
    return true;
}

/* ES5 15.2.4.7. */
static bool
obj_propertyIsEnumerable(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(0), &id))
        return false;

    /* Step 2. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 3. */
    RootedObject pobj(cx);
    RootedShape prop(cx);
    if (!JSObject::lookupGeneric(cx, obj, id, &pobj, &prop))
        return false;

    /* Step 4. */
    if (!prop) {
        args.rval().setBoolean(false);
        return true;
    }

    if (pobj != obj) {
        vp->setBoolean(false);
        return true;
    }

    /* Step 5. */
    unsigned attrs;
    if (!JSObject::getGenericAttributes(cx, pobj, id, &attrs))
        return false;

    args.rval().setBoolean((attrs & JSPROP_ENUMERATE) != 0);
    return true;
}

#if JS_HAS_TOSOURCE
static bool
obj_toSource(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JS_CHECK_RECURSION(cx, return false);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    JSString *str = ObjectToSource(cx, obj);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

JSString *
js::ObjectToSource(JSContext *cx, HandleObject obj)
{
    /* If outermost, we need parentheses to be an expression, not a block. */
    bool outermost = (cx->cycleDetectorSet.count() == 0);

    AutoCycleDetector detector(cx, obj);
    if (!detector.init())
        return nullptr;
    if (detector.foundCycle())
        return js_NewStringCopyZ<CanGC>(cx, "{}");

    StringBuffer buf(cx);
    if (outermost && !buf.append('('))
        return nullptr;
    if (!buf.append('{'))
        return nullptr;

    RootedValue v0(cx), v1(cx);
    MutableHandleValue val[2] = {&v0, &v1};

    RootedString str0(cx), str1(cx);
    MutableHandleString gsop[2] = {&str0, &str1};

    AutoIdVector idv(cx);
    if (!GetPropertyNames(cx, obj, JSITER_OWNONLY, &idv))
        return nullptr;

    bool comma = false;
    for (size_t i = 0; i < idv.length(); ++i) {
        RootedId id(cx, idv[i]);
        RootedObject obj2(cx);
        RootedShape shape(cx);
        if (!JSObject::lookupGeneric(cx, obj, id, &obj2, &shape))
            return nullptr;

        /*  Decide early whether we prefer get/set or old getter/setter syntax. */
        int valcnt = 0;
        if (shape) {
            bool doGet = true;
            if (obj2->isNative() && !IsImplicitDenseElement(shape)) {
                unsigned attrs = shape->attributes();
                if (attrs & JSPROP_GETTER) {
                    doGet = false;
                    val[valcnt].set(shape->getterValue());
                    gsop[valcnt].set(cx->names().get);
                    valcnt++;
                }
                if (attrs & JSPROP_SETTER) {
                    doGet = false;
                    val[valcnt].set(shape->setterValue());
                    gsop[valcnt].set(cx->names().set);
                    valcnt++;
                }
            }
            if (doGet) {
                valcnt = 1;
                gsop[0].set(nullptr);
                if (!JSObject::getGeneric(cx, obj, obj, id, val[0]))
                    return nullptr;
            }
        }

        /* Convert id to a linear string. */
        RootedValue idv(cx, IdToValue(id));
        JSString *s = ToString<CanGC>(cx, idv);
        if (!s)
            return nullptr;
        Rooted<JSLinearString*> idstr(cx, s->ensureLinear(cx));
        if (!idstr)
            return nullptr;

        /*
         * If id is a string that's not an identifier, or if it's a negative
         * integer, then it must be quoted.
         */
        if (JSID_IS_ATOM(id)
            ? !IsIdentifier(idstr)
            : (!JSID_IS_INT(id) || JSID_TO_INT(id) < 0))
        {
            s = js_QuoteString(cx, idstr, jschar('\''));
            if (!s || !(idstr = s->ensureLinear(cx)))
                return nullptr;
        }

        for (int j = 0; j < valcnt; j++) {
            /*
             * Censor an accessor descriptor getter or setter part if it's
             * undefined.
             */
            if (gsop[j] && val[j].isUndefined())
                continue;

            /* Convert val[j] to its canonical source form. */
            RootedString valstr(cx, ValueToSource(cx, val[j]));
            if (!valstr)
                return nullptr;
            const jschar *vchars = valstr->getChars(cx);
            if (!vchars)
                return nullptr;
            size_t vlength = valstr->length();

            /*
             * Remove '(function ' from the beginning of valstr and ')' from the
             * end so that we can put "get" in front of the function definition.
             */
            if (gsop[j] && IsFunctionObject(val[j])) {
                const jschar *start = vchars;
                const jschar *end = vchars + vlength;

                uint8_t parenChomp = 0;
                if (vchars[0] == '(') {
                    vchars++;
                    parenChomp = 1;
                }

                /* Try to jump "function" keyword. */
                if (vchars)
                    vchars = js_strchr_limit(vchars, ' ', end);

                /*
                 * Jump over the function's name: it can't be encoded as part
                 * of an ECMA getter or setter.
                 */
                if (vchars)
                    vchars = js_strchr_limit(vchars, '(', end);

                if (vchars) {
                    if (*vchars == ' ')
                        vchars++;
                    vlength = end - vchars - parenChomp;
                } else {
                    gsop[j].set(nullptr);
                    vchars = start;
                }
            }

            if (comma && !buf.append(", "))
                return nullptr;
            comma = true;

            if (gsop[j])
                if (!buf.append(gsop[j]) || !buf.append(' '))
                    return nullptr;

            if (!buf.append(idstr))
                return nullptr;
            if (!buf.append(gsop[j] ? ' ' : ':'))
                return nullptr;

            if (!buf.append(vchars, vlength))
                return nullptr;
        }
    }

    if (!buf.append('}'))
        return nullptr;
    if (outermost && !buf.append(')'))
        return nullptr;

    return buf.finishString();
}
#endif /* JS_HAS_TOSOURCE */

JSString *
JS_BasicObjectToString(JSContext *cx, HandleObject obj)
{
    const char *className = JSObject::className(cx, obj);

    StringBuffer sb(cx);
    if (!sb.append("[object ") || !sb.appendInflated(className, strlen(className)) ||
        !sb.append("]"))
    {
        return nullptr;
    }
    return sb.finishString();
}

/* ES5 15.2.4.2.  Note steps 1 and 2 are errata. */
static bool
obj_toString(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    if (args.thisv().isUndefined()) {
        args.rval().setString(cx->names().objectUndefined);
        return true;
    }

    /* Step 2. */
    if (args.thisv().isNull()) {
        args.rval().setString(cx->names().objectNull);
        return true;
    }

    /* Step 3. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 4-5. */
    JSString *str = JS_BasicObjectToString(cx, obj);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

/* ES5 15.2.4.3. */
static bool
obj_toLocaleString(JSContext *cx, unsigned argc, Value *vp)
{
    JS_CHECK_RECURSION(cx, return false);

    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Steps 2-4. */
    RootedId id(cx, NameToId(cx->names().toString));
    return obj->callMethod(cx, id, 0, nullptr, args.rval());
}

static bool
obj_valueOf(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;
    args.rval().setObject(*obj);
    return true;
}

#if JS_OLD_GETTER_SETTER_METHODS

enum DefineType { GetterAccessor, SetterAccessor };

template<DefineType Type>
static bool
DefineAccessor(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!BoxNonStrictThis(cx, args))
        return false;

    if (args.length() < 2 || !js_IsCallable(args[1])) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                             JSMSG_BAD_GETTER_OR_SETTER,
                             Type == GetterAccessor ? js_getter_str : js_setter_str);
        return false;
    }

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[0], &id))
        return false;

    RootedObject descObj(cx, NewBuiltinClassInstance(cx, &JSObject::class_));
    if (!descObj)
        return false;

    JSAtomState &names = cx->names();
    RootedValue trueVal(cx, BooleanValue(true));

    /* enumerable: true */
    if (!JSObject::defineProperty(cx, descObj, names.enumerable, trueVal))
        return false;

    /* configurable: true */
    if (!JSObject::defineProperty(cx, descObj, names.configurable, trueVal))
        return false;

    /* enumerable: true */
    PropertyName *acc = (Type == GetterAccessor) ? names.get : names.set;
    RootedValue accessorVal(cx, args[1]);
    if (!JSObject::defineProperty(cx, descObj, acc, accessorVal))
        return false;

    RootedObject thisObj(cx, &args.thisv().toObject());

    bool dummy;
    RootedValue descObjValue(cx, ObjectValue(*descObj));
    if (!DefineOwnProperty(cx, thisObj, id, descObjValue, &dummy))
        return false;

    args.rval().setUndefined();
    return true;
}

JS_FRIEND_API(bool)
js::obj_defineGetter(JSContext *cx, unsigned argc, Value *vp)
{
    return DefineAccessor<GetterAccessor>(cx, argc, vp);
}

JS_FRIEND_API(bool)
js::obj_defineSetter(JSContext *cx, unsigned argc, Value *vp)
{
    return DefineAccessor<SetterAccessor>(cx, argc, vp);
}

static bool
obj_lookupGetter(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(0), &id))
        return false;
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;
    if (obj->is<ProxyObject>()) {
        // The vanilla getter lookup code below requires that the object is
        // native. Handle proxies separately.
        args.rval().setUndefined();
        Rooted<PropertyDescriptor> desc(cx);
        if (!Proxy::getPropertyDescriptor(cx, obj, id, &desc, 0))
            return false;
        if (desc.object() && desc.hasGetterObject() && desc.getterObject())
            args.rval().setObject(*desc.getterObject());
        return true;
    }
    RootedObject pobj(cx);
    RootedShape shape(cx);
    if (!JSObject::lookupGeneric(cx, obj, id, &pobj, &shape))
        return false;
    args.rval().setUndefined();
    if (shape) {
        if (pobj->isNative() && !IsImplicitDenseElement(shape)) {
            if (shape->hasGetterValue())
                args.rval().set(shape->getterValue());
        }
    }
    return true;
}

static bool
obj_lookupSetter(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(0), &id))
        return false;
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;
    if (obj->is<ProxyObject>()) {
        // The vanilla setter lookup code below requires that the object is
        // native. Handle proxies separately.
        args.rval().setUndefined();
        Rooted<PropertyDescriptor> desc(cx);
        if (!Proxy::getPropertyDescriptor(cx, obj, id, &desc, 0))
            return false;
        if (desc.object() && desc.hasSetterObject() && desc.setterObject())
            args.rval().setObject(*desc.setterObject());
        return true;
    }
    RootedObject pobj(cx);
    RootedShape shape(cx);
    if (!JSObject::lookupGeneric(cx, obj, id, &pobj, &shape))
        return false;
    args.rval().setUndefined();
    if (shape) {
        if (pobj->isNative() && !IsImplicitDenseElement(shape)) {
            if (shape->hasSetterValue())
                args.rval().set(shape->setterValue());
        }
    }
    return true;
}
#endif /* JS_OLD_GETTER_SETTER_METHODS */

/* ES5 15.2.3.2. */
static bool
obj_getPrototypeOf(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    if (args.length() == 0) {
        js_ReportMissingArg(cx, args.calleev(), 0);
        return false;
    }

    if (args[0].isPrimitive()) {
        RootedValue val(cx, args[0]);
        char *bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, val, NullPtr());
        if (!bytes)
            return false;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr,
                             JSMSG_UNEXPECTED_TYPE, bytes, "not an object");
        js_free(bytes);
        return false;
    }

    /* Step 2. */

    /*
     * Implement [[Prototype]]-getting -- particularly across compartment
     * boundaries -- by calling a cached __proto__ getter function.
     */
    InvokeArgs args2(cx);
    if (!args2.init(0))
        return false;
    args2.setCallee(cx->global()->protoGetter());
    args2.setThis(args[0]);
    if (!Invoke(cx, args2))
        return false;
    args.rval().set(args2.rval());
    return true;
}

#if JS_HAS_OBJ_WATCHPOINT

bool
js::WatchHandler(JSContext *cx, JSObject *obj_, jsid id_, JS::Value old,
                 JS::Value *nvp, void *closure)
{
    RootedObject obj(cx, obj_);
    RootedId id(cx, id_);

    /* Avoid recursion on (obj, id) already being watched on cx. */
    AutoResolving resolving(cx, obj, id, AutoResolving::WATCH);
    if (resolving.alreadyStarted())
        return true;

    JSObject *callable = (JSObject *)closure;
    Value argv[] = { IdToValue(id), old, *nvp };
    RootedValue rv(cx);
    if (!Invoke(cx, ObjectValue(*obj), ObjectOrNullValue(callable), ArrayLength(argv), argv, &rv))
        return false;

    *nvp = rv;
    return true;
}

static bool
obj_watch(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    if (!GlobalObject::warnOnceAboutWatch(cx, obj))
        return false;

    if (args.length() <= 1) {
        js_ReportMissingArg(cx, args.calleev(), 1);
        return false;
    }

    RootedObject callable(cx, ValueToCallable(cx, args[1], args.length() - 2));
    if (!callable)
        return false;

    RootedId propid(cx);
    if (!ValueToId<CanGC>(cx, args[0], &propid))
        return false;

    RootedValue tmp(cx);
    unsigned attrs;
    if (!CheckAccess(cx, obj, propid, JSACC_WATCH, &tmp, &attrs))
        return false;

    if (!JSObject::watch(cx, obj, propid, callable))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
obj_unwatch(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    if (!GlobalObject::warnOnceAboutWatch(cx, obj))
        return false;

    RootedId id(cx);
    if (args.length() != 0) {
        if (!ValueToId<CanGC>(cx, args[0], &id))
            return false;
    } else {
        id = JSID_VOID;
    }

    if (!JSObject::unwatch(cx, obj, id))
        return false;

    args.rval().setUndefined();
    return true;
}

#endif /* JS_HAS_OBJ_WATCHPOINT */

/* ECMA 15.2.4.5. */
static bool
obj_hasOwnProperty(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    HandleValue idValue = args.get(0);

    /* Step 1, 2. */
    jsid id;
    if (args.thisv().isObject() && ValueToId<NoGC>(cx, idValue, &id)) {
        JSObject *obj = &args.thisv().toObject(), *obj2;
        Shape *prop;
        if (!obj->is<ProxyObject>() &&
            HasOwnProperty<NoGC>(cx, obj->getOps()->lookupGeneric, obj, id, &obj2, &prop))
        {
            args.rval().setBoolean(!!prop);
            return true;
        }
    }

    /* Step 1. */
    RootedId idRoot(cx);
    if (!ValueToId<CanGC>(cx, idValue, &idRoot))
        return false;

    /* Step 2. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Non-standard code for proxies. */
    RootedObject obj2(cx);
    RootedShape prop(cx);
    if (obj->is<ProxyObject>()) {
        bool has;
        if (!Proxy::hasOwn(cx, obj, idRoot, &has))
            return false;
        args.rval().setBoolean(has);
        return true;
    }

    /* Step 3. */
    if (!HasOwnProperty<CanGC>(cx, obj->getOps()->lookupGeneric, obj, idRoot, &obj2, &prop))
        return false;
    /* Step 4,5. */
    args.rval().setBoolean(!!prop);
    return true;
}

/* ES5 15.2.4.6. */
static bool
obj_isPrototypeOf(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Step 1. */
    if (args.length() < 1 || !args[0].isObject()) {
        args.rval().setBoolean(false);
        return true;
    }

    /* Step 2. */
    RootedObject obj(cx, ToObject(cx, args.thisv()));
    if (!obj)
        return false;

    /* Step 3. */
    bool isDelegate;
    if (!IsDelegate(cx, obj, args[0], &isDelegate))
        return false;
    args.rval().setBoolean(isDelegate);
    return true;
}

/* ES5 15.2.3.5: Object.create(O [, Properties]) */
static bool
obj_create(JSContext *cx, unsigned argc, Value *vp)
{
    if (argc == 0) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "Object.create", "0", "s");
        return false;
    }

    CallArgs args = CallArgsFromVp(argc, vp);
    RootedValue v(cx, args[0]);
    if (!v.isObjectOrNull()) {
        char *bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, NullPtr());
        if (!bytes)
            return false;
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
                             bytes, "not an object or null");
        js_free(bytes);
        return false;
    }

    RootedObject proto(cx, v.toObjectOrNull());

    /*
     * Use the callee's global as the parent of the new object to avoid dynamic
     * scoping (i.e., using the caller's global).
     */
    RootedObject obj(cx, NewObjectWithGivenProto(cx, &JSObject::class_, proto, &args.callee().global()));
    if (!obj)
        return false;

    /* 15.2.3.5 step 4. */
    if (args.hasDefined(1)) {
        if (args[1].isPrimitive()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_NOT_NONNULL_OBJECT);
            return false;
        }

        RootedObject props(cx, &args[1].toObject());
        if (!DefineProperties(cx, obj, props))
            return false;
    }

    /* 5. Return obj. */
    args.rval().setObject(*obj);
    return true;
}

static bool
obj_getOwnPropertyDescriptor(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.getOwnPropertyDescriptor", &obj))
        return false;
    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(1), &id))
        return false;
    return GetOwnPropertyDescriptor(cx, obj, id, args.rval());
}

static bool
obj_keys(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.keys", &obj))
        return false;

    AutoIdVector props(cx);
    if (!GetPropertyNames(cx, obj, JSITER_OWNONLY, &props))
        return false;

    AutoValueVector vals(cx);
    if (!vals.reserve(props.length()))
        return false;
    for (size_t i = 0, len = props.length(); i < len; i++) {
        jsid id = props[i];
        if (JSID_IS_STRING(id)) {
            vals.infallibleAppend(StringValue(JSID_TO_STRING(id)));
        } else if (JSID_IS_INT(id)) {
            JSString *str = Int32ToString<CanGC>(cx, JSID_TO_INT(id));
            if (!str)
                return false;
            vals.infallibleAppend(StringValue(str));
        } else {
            JS_ASSERT(JSID_IS_OBJECT(id));
        }
    }

    JS_ASSERT(props.length() <= UINT32_MAX);
    JSObject *aobj = NewDenseCopiedArray(cx, uint32_t(vals.length()), vals.begin());
    if (!aobj)
        return false;

    args.rval().setObject(*aobj);
    return true;
}

/* ES6 draft 15.2.3.16 */
static bool
obj_is(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool same;
    if (!SameValue(cx, args.get(0), args.get(1), &same))
        return false;

    args.rval().setBoolean(same);
    return true;
}

static bool
obj_getOwnPropertyNames(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.getOwnPropertyNames", &obj))
        return false;

    AutoIdVector keys(cx);
    if (!GetPropertyNames(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN, &keys))
        return false;

    AutoValueVector vals(cx);
    if (!vals.resize(keys.length()))
        return false;

    for (size_t i = 0, len = keys.length(); i < len; i++) {
         jsid id = keys[i];
         if (JSID_IS_INT(id)) {
             JSString *str = Int32ToString<CanGC>(cx, JSID_TO_INT(id));
             if (!str)
                 return false;
             vals[i].setString(str);
         } else if (JSID_IS_ATOM(id)) {
             vals[i].setString(JSID_TO_STRING(id));
         } else {
             vals[i].setObject(*JSID_TO_OBJECT(id));
         }
    }

    JSObject *aobj = NewDenseCopiedArray(cx, vals.length(), vals.begin());
    if (!aobj)
        return false;

    args.rval().setObject(*aobj);
    return true;
}

/* ES5 15.2.3.6: Object.defineProperty(O, P, Attributes) */
static bool
obj_defineProperty(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.defineProperty", &obj))
        return false;

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args.get(1), &id))
        return false;

    bool junk;
    if (!DefineOwnProperty(cx, obj, id, args.get(2), &junk))
        return false;

    args.rval().setObject(*obj);
    return true;
}

/* ES5 15.2.3.7: Object.defineProperties(O, Properties) */
static bool
obj_defineProperties(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Steps 1 and 7. */
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.defineProperties", &obj))
        return false;
    args.rval().setObject(*obj);

    /* Step 2. */
    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_MORE_ARGS_NEEDED,
                             "Object.defineProperties", "0", "s");
        return false;
    }
    RootedValue val(cx, args[1]);
    RootedObject props(cx, ToObject(cx, val));
    if (!props)
        return false;

    /* Steps 3-6. */
    return DefineProperties(cx, obj, props);
}

static bool
obj_isExtensible(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.isExtensible", &obj))
        return false;

    bool extensible;
    if (!JSObject::isExtensible(cx, obj, &extensible))
        return false;
    args.rval().setBoolean(extensible);
    return true;
}

static bool
obj_preventExtensions(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.preventExtensions", &obj))
        return false;

    args.rval().setObject(*obj);

    bool extensible;
    if (!JSObject::isExtensible(cx, obj, &extensible))
        return false;
    if (!extensible)
        return true;

    return JSObject::preventExtensions(cx, obj);
}

static bool
obj_freeze(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.freeze", &obj))
        return false;

    args.rval().setObject(*obj);

    return JSObject::freeze(cx, obj);
}

static bool
obj_isFrozen(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.preventExtensions", &obj))
        return false;

    bool frozen;
    if (!JSObject::isFrozen(cx, obj, &frozen))
        return false;
    args.rval().setBoolean(frozen);
    return true;
}

static bool
obj_seal(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.seal", &obj))
        return false;

    args.rval().setObject(*obj);

    return JSObject::seal(cx, obj);
}

static bool
obj_isSealed(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx);
    if (!GetFirstArgumentAsObject(cx, args, "Object.isSealed", &obj))
        return false;

    bool sealed;
    if (!JSObject::isSealed(cx, obj, &sealed))
        return false;
    args.rval().setBoolean(sealed);
    return true;
}

const JSFunctionSpec js::object_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,             obj_toSource,                0,0),
#endif
    JS_FN(js_toString_str,             obj_toString,                0,0),
    JS_FN(js_toLocaleString_str,       obj_toLocaleString,          0,0),
    JS_FN(js_valueOf_str,              obj_valueOf,                 0,0),
#if JS_HAS_OBJ_WATCHPOINT
    JS_FN(js_watch_str,                obj_watch,                   2,0),
    JS_FN(js_unwatch_str,              obj_unwatch,                 1,0),
#endif
    JS_FN(js_hasOwnProperty_str,       obj_hasOwnProperty,          1,0),
    JS_FN(js_isPrototypeOf_str,        obj_isPrototypeOf,           1,0),
    JS_FN(js_propertyIsEnumerable_str, obj_propertyIsEnumerable,    1,0),
#if JS_OLD_GETTER_SETTER_METHODS
    JS_FN(js_defineGetter_str,         js::obj_defineGetter,        2,0),
    JS_FN(js_defineSetter_str,         js::obj_defineSetter,        2,0),
    JS_FN(js_lookupGetter_str,         obj_lookupGetter,            1,0),
    JS_FN(js_lookupSetter_str,         obj_lookupSetter,            1,0),
#endif
    JS_FS_END
};

const JSFunctionSpec js::object_static_methods[] = {
    JS_FN("getPrototypeOf",            obj_getPrototypeOf,          1,0),
    JS_FN("getOwnPropertyDescriptor",  obj_getOwnPropertyDescriptor,2,0),
    JS_FN("keys",                      obj_keys,                    1,0),
    JS_FN("is",                        obj_is,                      2,0),
    JS_FN("defineProperty",            obj_defineProperty,          3,0),
    JS_FN("defineProperties",          obj_defineProperties,        2,0),
    JS_FN("create",                    obj_create,                  2,0),
    JS_FN("getOwnPropertyNames",       obj_getOwnPropertyNames,     1,0),
    JS_FN("isExtensible",              obj_isExtensible,            1,0),
    JS_FN("preventExtensions",         obj_preventExtensions,       1,0),
    JS_FN("freeze",                    obj_freeze,                  1,0),
    JS_FN("isFrozen",                  obj_isFrozen,                1,0),
    JS_FN("seal",                      obj_seal,                    1,0),
    JS_FN("isSealed",                  obj_isSealed,                1,0),
    JS_FS_END
};
