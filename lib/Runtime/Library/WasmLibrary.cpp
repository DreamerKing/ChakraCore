//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

#ifdef ENABLE_WASM
#include "../WasmReader/WasmReaderPch.h"
// Included for AsmJsDefaultEntryThunk
#include "Language/InterpreterStackFrame.h"

namespace Js
{
    const unsigned int WasmLibrary::experimentalVersion = Wasm::experimentalVersion;

    Var WasmLibrary::instantiateModule(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray, _u("[Wasm].instantiateModule(typedArray,)"));
        }
        if (args.Info.Count < 3)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject, _u("[Wasm].instantiateModule(,ffi)"));
        }

        const BOOL isTypedArray = Js::TypedArrayBase::Is(args[1]);
        const BOOL isArrayBuffer = Js::ArrayBuffer::Is(args[1]);

        if (!isTypedArray && !isArrayBuffer)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedTypedArray, _u("[Wasm].instantiateModule(typedArray,)"));
        }
        BYTE* buffer;
        uint byteLength;
        if (isTypedArray)
        {
            Js::TypedArrayBase* array = Js::TypedArrayBase::FromVar(args[1]);
            buffer = array->GetByteBuffer();
            byteLength = array->GetByteLength();
        }
        else
        {
            Js::ArrayBuffer* arrayBuffer = Js::ArrayBuffer::FromVar(args[1]);
            buffer = arrayBuffer->GetBuffer();
            byteLength = arrayBuffer->GetByteLength();
        }

        if (!Js::JavascriptObject::Is(args[2]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedObject, _u("[Wasm].instantiateModule(,ffi)"));
        }
        WebAssemblyModule * module = WebAssemblyModule::CreateModule(scriptContext, buffer, byteLength);
        return WebAssemblyInstance::CreateInstance(module, args[2]);
    }

    Var WasmLibrary::WasmLazyTrapCallback(RecyclableObject *callee, CallInfo, ...)
    {
        AsmJsScriptFunction* asmFunction = static_cast<AsmJsScriptFunction*>(callee);
        Assert(asmFunction);
        ScriptContext * scriptContext = asmFunction->GetScriptContext();
        Assert(scriptContext);
        auto error = asmFunction->GetFunctionBody()->GetAsmJsFunctionInfo()->GetLazyError();
        JavascriptExceptionOperators::Throw(error, scriptContext);
    }

#if _M_IX86
    __declspec(naked)
        Var WasmLibrary::WasmDeferredParseExternalThunk(RecyclableObject* function, CallInfo callInfo, ...)
    {
        // Register functions
        __asm
        {
            push ebp
            mov ebp, esp
            lea eax, [esp + 8]
            push 0
            push eax
            call WasmLibrary::WasmDeferredParseEntryPoint
#ifdef _CONTROL_FLOW_GUARD
            // verify that the call target is valid
            mov  ecx, eax
            call[__guard_check_icall_fptr]
            mov eax, ecx
#endif
            pop ebp
            // Although we don't restore ESP here on WinCE, this is fine because script profiler is not shipped for WinCE.
            jmp eax
        }
    }

    __declspec(naked)
        Var WasmLibrary::WasmDeferredParseInternalThunk(RecyclableObject* function, CallInfo callInfo, ...)
    {
        // Register functions
        __asm
        {
            push ebp
            mov ebp, esp
            lea eax, [esp + 8]
            push 1
            push eax
            call WasmLibrary::WasmDeferredParseEntryPoint
#ifdef _CONTROL_FLOW_GUARD
            // verify that the call target is valid
            mov  ecx, eax
            call[__guard_check_icall_fptr]
            mov eax, ecx
#endif
            pop ebp
            // Although we don't restore ESP here on WinCE, this is fine because script profiler is not shipped for WinCE.
            jmp eax
        }
    }
#elif defined(_M_X64)
    // Do nothing: the implementation of WasmLibrary::WasmDeferredParseExternalThunk is declared (appropriately decorated) in
    // Language\amd64\amd64_Thunks.asm.
#endif // _M_IX86

}

#endif // ENABLE_WASM

Js::JavascriptMethod Js::WasmLibrary::WasmDeferredParseEntryPoint(Js::AsmJsScriptFunction** funcPtr, int internalCall)
{
#ifdef ENABLE_WASM
    AsmJsScriptFunction* func = *funcPtr;

    FunctionBody* body = func->GetFunctionBody();
    AsmJsFunctionInfo* info = body->GetAsmJsFunctionInfo();
    ScriptContext* scriptContext = func->GetScriptContext();

    Js::FunctionEntryPointInfo * entypointInfo = (Js::FunctionEntryPointInfo*)func->GetEntryPointInfo();
    Wasm::WasmReaderInfo* readerInfo = info->GetWasmReaderInfo();
    info->SetWasmReaderInfo(nullptr);
    try
    {
        Wasm::WasmBytecodeGenerator::GenerateFunctionBytecode(scriptContext, readerInfo);
        func->GetDynamicType()->SetEntryPoint(Js::AsmJsExternalEntryPoint);
        entypointInfo->jsMethod = AsmJsDefaultEntryThunk;
        // Do MTJRC/MAIC:0 check
#if ENABLE_DEBUG_CONFIG_OPTIONS
        if (CONFIG_FLAG(ForceNative) || CONFIG_FLAG(MaxAsmJsInterpreterRunCount) == 0)
        {
            GenerateFunction(scriptContext->GetNativeCodeGenerator(), func->GetFunctionBody(), func);
        }
#endif
    }
    catch (Wasm::WasmCompilationException& ex)
    {
        char16* originalMessage = ex.ReleaseErrorMessage();
        intptr_t offset = readerInfo->m_module->GetReader()->GetCurrentOffset();
        intptr_t start = readerInfo->m_funcInfo->m_readerInfo.startOffset;
        uint32 size = readerInfo->m_funcInfo->m_readerInfo.size;

        Wasm::WasmCompilationException newEx = Wasm::WasmCompilationException(
            _u("function %s at offset %d/%d: %s"),
            body->GetDisplayName(),
            offset - start,
            size,
            originalMessage
        );
        SysFreeString(originalMessage);
        char16* msg = newEx.ReleaseErrorMessage();
        JavascriptLibrary *library = scriptContext->GetLibrary();
        JavascriptError *pError = library->CreateWebAssemblyCompileError();
        JavascriptError::SetErrorMessage(pError, JSERR_WasmCompileError, msg, scriptContext);

        func->GetDynamicType()->SetEntryPoint(WasmLazyTrapCallback);
        entypointInfo->jsMethod = WasmLazyTrapCallback;
        info->SetLazyError(pError);
    }
    if (internalCall)
    {
        return entypointInfo->jsMethod;
    }
    return func->GetDynamicType()->GetEntryPoint();
#else
    Js::Throw::InternalError();
#endif
}