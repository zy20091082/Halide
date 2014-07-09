#include <iostream>

#include "CodeGen_Posix.h"
#include "CodeGen_Internal.h"
#include "LLVM_Headers.h"
#include "IR.h"
#include "IROperator.h"
#include "Debug.h"
#include "IRPrinter.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::string;
using std::map;
using std::pair;
using std::make_pair;

using namespace llvm;

CodeGen_Posix::CodeGen_Posix(Target t) :
    CodeGen(t),

    // Vector types. These need an LLVMContext before they can be initialized.
    i8x8(NULL),
    i8x16(NULL),
    i8x32(NULL),
    i16x4(NULL),
    i16x8(NULL),
    i16x16(NULL),
    i32x2(NULL),
    i32x4(NULL),
    i32x8(NULL),
    i64x2(NULL),
    i64x4(NULL),
    f32x2(NULL),
    f32x4(NULL),
    f32x8(NULL),
    f64x2(NULL),
    f64x4(NULL),

    // Wildcards for pattern matching
    wild_i8x8(Variable::make(Int(8, 8), "*")),
    wild_i16x4(Variable::make(Int(16, 4), "*")),
    wild_i32x2(Variable::make(Int(32, 2), "*")),

    wild_u8x8(Variable::make(UInt(8, 8), "*")),
    wild_u16x4(Variable::make(UInt(16, 4), "*")),
    wild_u32x2(Variable::make(UInt(32, 2), "*")),

    wild_i8x16(Variable::make(Int(8, 16), "*")),
    wild_i16x8(Variable::make(Int(16, 8), "*")),
    wild_i32x4(Variable::make(Int(32, 4), "*")),
    wild_i64x2(Variable::make(Int(64, 2), "*")),

    wild_u8x16(Variable::make(UInt(8, 16), "*")),
    wild_u16x8(Variable::make(UInt(16, 8), "*")),
    wild_u32x4(Variable::make(UInt(32, 4), "*")),
    wild_u64x2(Variable::make(UInt(64, 2), "*")),

    wild_i8x32(Variable::make(Int(8, 32), "*")),
    wild_i16x16(Variable::make(Int(16, 16), "*")),
    wild_i32x8(Variable::make(Int(32, 8), "*")),
    wild_i64x4(Variable::make(Int(64, 4), "*")),

    wild_u8x32(Variable::make(UInt(8, 32), "*")),
    wild_u16x16(Variable::make(UInt(16, 16), "*")),
    wild_u32x8(Variable::make(UInt(32, 8), "*")),
    wild_u64x4(Variable::make(UInt(64, 4), "*")),

    wild_f32x2(Variable::make(Float(32, 2), "*")),

    wild_f32x4(Variable::make(Float(32, 4), "*")),
    wild_f64x2(Variable::make(Float(64, 2), "*")),

    wild_f32x8(Variable::make(Float(32, 8), "*")),
    wild_f64x4(Variable::make(Float(64, 4), "*")),

    // Bounds of types
    min_i8(Int(8).min()),
    max_i8(Int(8).max()),
    max_u8(UInt(8).max()),

    min_i16(Int(16).min()),
    max_i16(Int(16).max()),
    max_u16(UInt(16).max()),

    min_i32(Int(32).min()),
    max_i32(Int(32).max()),
    max_u32(UInt(32).max()),

    min_i64(Int(64).min()),
    max_i64(Int(64).max()),
    max_u64(UInt(64).max()),

    min_f32(Float(32).min()),
    max_f32(Float(32).max()),

    min_f64(Float(64).min()),
    max_f64(Float(64).max()) {

}

void CodeGen_Posix::init_module() {
    CodeGen::init_module();

    i8x8 = VectorType::get(i8, 8);
    i8x16 = VectorType::get(i8, 16);
    i8x32 = VectorType::get(i8, 32);
    i16x4 = VectorType::get(i16, 4);
    i16x8 = VectorType::get(i16, 8);
    i16x16 = VectorType::get(i16, 16);
    i32x2 = VectorType::get(i32, 2);
    i32x4 = VectorType::get(i32, 4);
    i32x8 = VectorType::get(i32, 8);
    i64x2 = VectorType::get(i64, 2);
    i64x4 = VectorType::get(i64, 4);
    f32x2 = VectorType::get(f32, 2);
    f32x4 = VectorType::get(f32, 4);
    f32x8 = VectorType::get(f32, 8);
    f64x2 = VectorType::get(f64, 2);
    f64x4 = VectorType::get(f64, 4);
}

Value *CodeGen_Posix::codegen_allocation_size(const std::string &name, Type type, const std::vector<Expr> &extents) {
    // Compute size from list of extents checking for 32-bit signed overflow.
    // Math is done using 64-bit intergers as overflow checked 32-bit mutliply
    // does not work with NaCl at the moment.
    Value *overflow = NULL;
    int bytes_per_item = type.width * type.bytes();
    Value *llvm_size_wide = ConstantInt::get(i64, bytes_per_item);
    for (size_t i = 0; i < extents.size(); i++) {
        llvm_size_wide = builder->CreateMul(llvm_size_wide, codegen(Cast::make(Int(64), extents[i])));
        if (overflow == NULL) {
            overflow = llvm_size_wide;
        } else {
            overflow = builder->CreateOr(overflow, llvm_size_wide);
        }
    }
    Value *llvm_size = builder->CreateTrunc(llvm_size_wide, i32);

    if (overflow != NULL) {
        Constant *zero = ConstantInt::get(i64, 0);
        create_assertion(builder->CreateICmpEQ(builder->CreateLShr(overflow, 31), zero),
                         std::string("32-bit signed overflow computing size of allocation ") + name);
    }

    return llvm_size;
}

CodeGen_Posix::Allocation CodeGen_Posix::create_allocation(const std::string &name, Type type,
                                                           const std::vector<Expr> &extents, Expr condition) {

    Allocation allocation;
    Value *llvm_size = NULL;

    int32_t constant_size;
    if (constant_allocation_size(extents, name, constant_size)) {
        int64_t stack_bytes = constant_size * type.bytes();

        if (stack_bytes > ((int64_t(1) << 31) - 1)) {
            user_error << "Total size for allocation " << name << " is constant but exceeds 2^31 - 1.";
        } else if (stack_bytes <= 1024 * 8) {
            // Round up to nearest multiple of 32.
            allocation.stack_size = static_cast<int32_t>(((stack_bytes + 31)/32)*32);
        } else {
            allocation.stack_size = 0;
            llvm_size = codegen(Expr(static_cast<int32_t>(stack_bytes)));
        }
    } else {
        allocation.stack_size = 0;
        llvm_size = codegen_allocation_size(name, type, extents);
    }

    // Only allocate memory if the condition is true, otherwise 0.
    if (llvm_size != NULL) {
        Value *llvm_condition = codegen(condition);
        llvm_size = builder->CreateSelect(llvm_condition,
                                          llvm_size,
                                          ConstantInt::get(llvm_size->getType(), 0));
    }

    if (allocation.stack_size) {
        // Try to find a free stack allocation we can use.
        vector<Allocation>::iterator free = free_stack_allocs.end();
        for (free = free_stack_allocs.begin(); free != free_stack_allocs.end(); ++free) {
            if (((Instruction *)free->ptr)->getParent()->getParent() == builder->GetInsertBlock()->getParent() &&
                free->stack_size >= allocation.stack_size) {
                break;
            }
        }
        if (free != free_stack_allocs.end()) {
            debug(4) << "Reusing freed stack allocation of " << free->stack_size << " bytes for allocation " << name << " of " << allocation.stack_size << " bytes.\n";
            // Use a free alloc we found.
            allocation = *free;
            // This allocation isn't free anymore.
            free_stack_allocs.erase(free);
        } else {
            debug(4) << "Allocating " << allocation.stack_size << " bytes on the stack for " << name << "\n";
            // We used to do the alloca locally and save and restore the
            // stack pointer, but this makes llvm generate streams of
            // spill/reloads.
            allocation.ptr = create_alloca_at_entry(i32x8, allocation.stack_size/32, name);
        }
    } else {
        // call malloc
        llvm::Function *malloc_fn = module->getFunction("halide_malloc");
        malloc_fn->setDoesNotAlias(0);
        internal_assert(malloc_fn) << "Could not find halide_malloc in module\n";

        llvm::Function::arg_iterator arg_iter = malloc_fn->arg_begin();
        ++arg_iter;  // skip the user context *
        llvm_size = builder->CreateIntCast(llvm_size, arg_iter->getType(), false);

        debug(4) << "Creating call to halide_malloc\n";
        Value *args[2] = { get_user_context(), llvm_size };

        CallInst *call = builder->CreateCall(malloc_fn, args);
        allocation.ptr = call;

        // Assert that the allocation worked.
        Value *check = builder->CreateIsNotNull(allocation.ptr);
        Value *zero_size = builder->CreateIsNull(llvm_size);
        check = builder->CreateOr(check, zero_size);

        create_assertion(check, "Out of memory (malloc returned NULL)");
    }

    // Push the allocation base pointer onto the symbol table
    debug(3) << "Pushing allocation called " << name << ".host onto the symbol table\n";

    allocations.push(name, allocation);

    return allocation;
}

void CodeGen_Posix::free_allocation(const std::string &name) {
    Allocation alloc = allocations.get(name);

    internal_assert(alloc.ptr);

    CallInst *call_inst = dyn_cast<CallInst>(alloc.ptr);
    llvm::Function *allocated_in = call_inst ? call_inst->getParent()->getParent() : NULL;
    llvm::Function *current_func = builder->GetInsertBlock()->getParent();

    if (alloc.stack_size) {
        // Remember this allocation so it can be re-used by a later allocation.
        free_stack_allocs.push_back(alloc);
    } else if (allocated_in == current_func) { // Skip over allocations from outside this function.
        // Call free
        llvm::Function *free_fn = module->getFunction("halide_free");
        internal_assert(free_fn) << "Could not find halide_free in module.\n";
        debug(4) << "Creating call to halide_free\n";
        Value *args[2] = { get_user_context(), alloc.ptr };
        builder->CreateCall(free_fn, args);
    }

    allocations.pop(name);
}

void CodeGen_Posix::destroy_allocation(Allocation alloc) {
    // Heap allocations have already been freed.
}

void CodeGen_Posix::visit(const Allocate *alloc) {

    if (sym_exists(alloc->name + ".host")) {
        user_error << "Can't have two different buffers with the same name: "
                   << alloc->name << "\n";
    }

    Allocation allocation = create_allocation(alloc->name, alloc->type,
                                              alloc->extents, alloc->condition);
    sym_push(alloc->name + ".host", allocation.ptr);

    codegen(alloc->body);

    // Should have been freed
    internal_assert(!sym_exists(alloc->name + ".host"));
    internal_assert(!allocations.contains(alloc->name));

    debug(2) << "Destroying allocation " << alloc->name << "\n";
    destroy_allocation(allocation);
}

void CodeGen_Posix::visit(const Free *stmt) {
    free_allocation(stmt->name);
    sym_pop(stmt->name + ".host");
}

void CodeGen_Posix::prepare_for_early_exit() {
    // We've jumped to a code path that will be called just before
    // bailing out. Free everything outstanding.
    vector<string> names;
    for (Scope<Allocation>::iterator iter = allocations.begin();
         iter != allocations.end(); ++iter) {
        names.push_back(iter.name());
    }

    for (size_t i = 0; i < names.size(); i++) {
        std::vector<Allocation> stash;
        while (allocations.contains(names[i])) {
            // The value in the symbol table is not necessarily the
            // one in the allocation - it may have been forwarded
            // inside a parallel for loop
            stash.push_back(allocations.get(names[i]));
            free_allocation(names[i]);
        }

        // Restore all the allocations before we jump back to the main
        // code path.
        for (size_t j = stash.size(); j > 0; j--) {
            allocations.push(names[i], stash[j-1]);
        }
    }

    free_stack_allocs.clear();
}

}}
