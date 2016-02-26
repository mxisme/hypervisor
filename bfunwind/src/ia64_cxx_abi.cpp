//
// Bareflank Unwind Library
//
// Copyright (C) 2015 Assured Information Security, Inc.
// Author: Rian Quinn        <quinnr@ainfosec.com>
// Author: Brendan Kerrigan  <kerriganb@ainfosec.com>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include <abort.h>
#include <eh_frame.h>
#include <registers.h>
#include <ia64_cxx_abi.h>

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static _Unwind_Reason_Code
private_personality(_Unwind_Action action, struct _Unwind_Context *context)
{
    auto pl = context->fde.cie().personality_function();

    if (pl == 0)
        return _URC_CONTINUE_UNWIND;

    auto pr = ((__personality_routine *)pl)[0];

    if (pr == NULL)
        ABORT("personality routine is NULL");

    return pr(1, action,
              context->exception_object->exception_class,
              context->exception_object, context);
}

// -----------------------------------------------------------------------------
// Implementation
// -----------------------------------------------------------------------------

typedef bool (* step_func_t)(_Unwind_Context *context);

_Unwind_Reason_Code
private_step(struct _Unwind_Context *context, step_func_t func)
{
    if ((context->fde = eh_frame::find_fde(context->state)) == false)
        return _URC_END_OF_STACK;

    if (func != 0 && func(context) == true)
        return _URC_NO_REASON;

    dwarf4::unwind(context->fde, context->state);

    return _URC_CONTINUE_UNWIND;
}

_Unwind_Reason_Code
private_step_loop(struct _Unwind_Context *context, step_func_t func)
{
    while (1)
    {
        auto ret = private_step(context, func);
        if (ret != _URC_CONTINUE_UNWIND)
            return ret;
    }
}

static _Unwind_Reason_Code
private_phase1(struct _Unwind_Context *context)
{
    log("\n");
    log("============================================================\n");
    log("Phase #1\n\n");

    private_step(context, 0);

    return private_step_loop(context, [](_Unwind_Context *context) -> bool {
        switch (private_personality(_UA_SEARCH_PHASE, context))
        {
            case _URC_HANDLER_FOUND:
                context->exception_object->private_1 = context->fde.pc_begin();
                return true;

            case _URC_CONTINUE_UNWIND:
                return false;

            default:
                ABORT("phase 1 personality routine failed");
                return true;
        }
    });
}

static _Unwind_Reason_Code
private_phase2(struct _Unwind_Context *context)
{
    log("\n");
    log("============================================================\n");
    log("Phase #2\n\n");

    private_step(context, 0);

    return private_step_loop(context, [](_Unwind_Context *context) -> bool {
        auto action = _UA_CLEANUP_PHASE;

        if (context->exception_object->private_1 == context->fde.pc_begin())
            action |= _UA_HANDLER_FRAME;

        switch (private_personality(action, context))
        {
            case _URC_INSTALL_CONTEXT:
                context->state->resume();

            case _URC_CONTINUE_UNWIND:
                return false;

            default:
                ABORT("phase 2 personality routine failed");
                return true;
        }
    });
}

extern "C" _Unwind_Reason_Code
_Unwind_RaiseException(_Unwind_Exception *exception_object)
{
    auto ret = _URC_END_OF_STACK;

    auto registers = registers_intel_x64();
    __store_registers_intel_x64(&registers);

    exception_object->private_1 = 0;
    exception_object->private_2 = 0;

    auto state = register_state_intel_x64(registers);
    auto context = _Unwind_Context(&state, exception_object);

    ret = private_phase1(&context);
    if (ret != _URC_NO_REASON)
        return ret;

    state = register_state_intel_x64(registers);
    context = _Unwind_Context(&state, exception_object);

    ret = private_phase2(&context);
    if (ret != _URC_NO_REASON)
        return ret;

    return _URC_FATAL_PHASE2_ERROR;
}

extern "C" void
_Unwind_Resume(_Unwind_Exception *exception_object)
{
    auto registers = registers_intel_x64();
    __store_registers_intel_x64(&registers);

    auto state = register_state_intel_x64(registers);
    auto context = _Unwind_Context(&state, exception_object);

    private_phase2(&context);
}

extern "C" void
_Unwind_DeleteException(_Unwind_Exception *exception_object)
{
    if (exception_object->exception_cleanup != NULL)
        (*exception_object->exception_cleanup)(_URC_FOREIGN_EXCEPTION_CAUGHT,
                                               exception_object);
}

extern "C" uintptr_t
_Unwind_GetGR(struct _Unwind_Context *context, int index)
{
    return context->state->get(index);
}

extern "C" void
_Unwind_SetGR(struct _Unwind_Context *context, int index, uintptr_t value)
{
    context->state->set(index, value);
    context->state->commit();
}

extern "C" uintptr_t
_Unwind_GetIP(struct _Unwind_Context *context)
{
    return context->state->get_ip();
}

extern "C" void
_Unwind_SetIP(struct _Unwind_Context *context, uintptr_t value)
{
    context->state->set_ip(value);
    context->state->commit();
}

extern "C" uintptr_t
_Unwind_GetLanguageSpecificData(struct _Unwind_Context *context)
{
    return context->fde.lsda();
}

extern "C" uintptr_t
_Unwind_GetRegionStart(struct _Unwind_Context *context)
{
    return context->fde.pc_begin();
}

extern "C" uintptr_t
_Unwind_GetIPInfo(struct _Unwind_Context *context, int *ip_before_insn)
{
    if (ip_before_insn == NULL)
        ABORT("ip_before_insn == NULL");

    *ip_before_insn = 0;
    return _Unwind_GetIP(context);
}
