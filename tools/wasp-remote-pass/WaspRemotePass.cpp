//===- WaspRemotePass.cpp - lower wasp remote-memory address space --------===//
//
// Makes remote coordinator memory dereferenceable from plain C:
//
//   #define wasp_remote __attribute__((address_space(100)))
//   wasp_remote int *p = WASP_REMOTE_PTR(int, 1, 0);
//   int x = p[3];        // becomes __wasp_remote_load_i32(ref + 12)
//   *p = 7;              // becomes __wasp_remote_store_i32(ref, 7)
//
// The pass rewrites every load/store through address space 100 into a
// call to a __wasp_remote_* shim (tools/lib/wasp_remote_rt.c, linked
// into the module), and llvm.memcpy/memmove between AS100 and default
// memory into bulk __wasp_remote_read/write calls — so struct
// assignment through a remote pointer is one RPC, not a field-by-field
// storm. Pointer arithmetic, GEPs, comparisons, and passing remote
// pointers around need no rewriting: a remote pointer IS the packed
// (region:8 | offset:24) reference, 32 bits like any wasm pointer.
//
// Anything the pass cannot lower faithfully (atomics, address-space
// casts, remote memset, loads/stores of unsupported types) aborts the
// compile: a silent miscompile against remote memory must be
// impossible.
//
// AS 100 is deliberately outside the LLVM wasm backend's reserved
// spaces (1 = wasm globals, 10 = externref, 20 = funcref).
//
// Build (see tools/build_test_module.sh):
//   clang++-18 -shared -fPIC $(llvm-config-18 --cxxflags) \
//       WaspRemotePass.cpp -o libWaspRemotePass.so
// Use:
//   clang --target=wasm32 -O2 -fpass-plugin=./libWaspRemotePass.so ...
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

constexpr unsigned kWaspAS = 100;

[[noreturn]] void fail(const Instruction &I, const Twine &Why) {
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "wasp-remote: " << Why << "\n  in function '"
     << I.getFunction()->getName() << "': ";
  I.print(OS);
  report_fatal_error(Twine(OS.str()), /*gen_crash_diag=*/false);
}

bool isRemotePtr(const Value *V) {
  auto *PT = dyn_cast<PointerType>(V->getType());
  return PT && PT->getAddressSpace() == kWaspAS;
}

// Shim suffix for a loadable/storable scalar type; empty = unsupported.
StringRef typeSuffix(Type *T) {
  if (T->isIntegerTy(8))
    return "i8";
  if (T->isIntegerTy(16))
    return "i16";
  if (T->isIntegerTy(32))
    return "i32";
  if (T->isIntegerTy(64))
    return "i64";
  if (T->isFloatTy())
    return "f32";
  if (T->isDoubleTy())
    return "f64";
  return "";
}

class WaspRemoteLowering {
public:
  explicit WaspRemoteLowering(Module &M)
      : M(M), I32(Type::getInt32Ty(M.getContext())) {}

  bool run() {
    SmallVector<Instruction *, 32> Doomed;

    for (Function &F : M) {
      for (Instruction &I : instructions(F)) {
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          if (isRemotePtr(LI->getPointerOperand())) {
            lowerLoad(*LI);
            Doomed.push_back(LI);
          }
        } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
          if (isRemotePtr(SI->getPointerOperand())) {
            lowerStore(*SI);
            Doomed.push_back(SI);
          } else if (isRemotePtr(SI->getValueOperand()) &&
                     SI->getPointerAddressSpace() != 0) {
            fail(I, "cannot store a remote pointer into this address space");
          }
        } else if (auto *MT = dyn_cast<MemTransferInst>(&I)) {
          if (lowerMemTransfer(*MT))
            Doomed.push_back(MT);
        } else if (auto *MS = dyn_cast<MemSetInst>(&I)) {
          if (MS->getDestAddressSpace() == kWaspAS)
            fail(I, "memset of remote memory is not supported; "
                    "fill a local buffer and copy it out");
        } else if (auto *AC = dyn_cast<AddrSpaceCastInst>(&I)) {
          unsigned Src = AC->getSrcAddressSpace();
          unsigned Dst = AC->getDestAddressSpace();
          if (Src == kWaspAS || Dst == kWaspAS)
            fail(I, "cannot cast between remote and local pointers; "
                    "remote references only name coordinator memory");
        } else if (auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
          if (isRemotePtr(RMW->getPointerOperand()))
            fail(I, "atomic operations on remote memory are not "
                    "supported; use wasp_lock()");
        } else if (auto *CX = dyn_cast<AtomicCmpXchgInst>(&I)) {
          if (isRemotePtr(CX->getPointerOperand()))
            fail(I, "atomic operations on remote memory are not "
                    "supported; use wasp_lock()");
        }
      }
    }

    for (Instruction *I : Doomed)
      I->eraseFromParent();
    return !Doomed.empty();
  }

private:
  Module &M;
  Type *I32;

  Value *refOf(IRBuilder<> &B, Value *RemotePtr) {
    return B.CreatePtrToInt(RemotePtr, I32, "wasp.ref");
  }

  void lowerLoad(LoadInst &LI) {
    if (LI.isAtomic())
      fail(LI, "atomic remote load is not supported; use wasp_lock()");

    IRBuilder<> B(&LI);
    Type *T = LI.getType();
    Value *Ref = refOf(B, LI.getPointerOperand());
    Value *Repl;

    if (auto *PT = dyn_cast<PointerType>(T)) {
      // A remote reference stored in remote memory: load the packed u32
      // and rebuild the pointer. Loading a *local* pointer from remote
      // memory is meaningless — addresses don't travel between machines.
      if (PT->getAddressSpace() != kWaspAS)
        fail(LI, "loading a local pointer from remote memory is "
                 "meaningless (addresses don't travel); store data or "
                 "remote references instead");
      Value *Raw = B.CreateCall(shim("__wasp_remote_load_i32", I32, {I32}),
                                {Ref});
      Repl = B.CreateIntToPtr(Raw, T);
    } else {
      StringRef Suffix = typeSuffix(T);
      if (Suffix.empty())
        fail(LI, "unsupported remote load type (only integer/float "
                 "scalars and whole-struct copies are lowered)");
      Repl = B.CreateCall(
          shim(("__wasp_remote_load_" + Suffix).str(), T, {I32}), {Ref});
    }
    LI.replaceAllUsesWith(Repl);
  }

  void lowerStore(StoreInst &SI) {
    if (SI.isAtomic())
      fail(SI, "atomic remote store is not supported; use wasp_lock()");

    IRBuilder<> B(&SI);
    Value *V = SI.getValueOperand();
    Type *T = V->getType();
    Value *Ref = refOf(B, SI.getPointerOperand());

    if (auto *PT = dyn_cast<PointerType>(T)) {
      if (PT->getAddressSpace() != kWaspAS)
        fail(SI, "storing a local pointer into remote memory is "
                 "meaningless (addresses don't travel); store data or "
                 "remote references instead");
      Value *Raw = B.CreatePtrToInt(V, I32);
      B.CreateCall(shim("__wasp_remote_store_i32",
                        Type::getVoidTy(M.getContext()), {I32, I32}),
                   {Ref, Raw});
      return;
    }

    StringRef Suffix = typeSuffix(T);
    if (Suffix.empty())
      fail(SI, "unsupported remote store type (only integer/float "
               "scalars and whole-struct copies are lowered)");
    B.CreateCall(shim(("__wasp_remote_store_" + Suffix).str(),
                      Type::getVoidTy(M.getContext()), {T}),
                 {Ref, V});
  }

  // memcpy/memmove: remote<->local becomes one bulk RPC. Returns true
  // if the intrinsic was lowered and must be erased.
  bool lowerMemTransfer(MemTransferInst &MT) {
    unsigned DstAS = MT.getDestAddressSpace();
    unsigned SrcAS = MT.getSourceAddressSpace();

    if (DstAS != kWaspAS && SrcAS != kWaspAS)
      return false;
    if (DstAS == kWaspAS && SrcAS == kWaspAS)
      fail(MT, "remote-to-remote copy is not supported; stage through "
               "a local buffer");
    if (MT.isVolatile())
      fail(MT, "volatile remote memcpy is not supported");

    IRBuilder<> B(&MT);
    Type *VoidTy = Type::getVoidTy(M.getContext());
    Type *P0 = PointerType::get(M.getContext(), 0);
    Value *Len = B.CreateZExtOrTrunc(MT.getLength(), I32);

    if (DstAS == kWaspAS) {
      // local -> remote
      B.CreateCall(shim("__wasp_remote_write", VoidTy, {I32, P0, I32}),
                   {refOf(B, MT.getRawDest()), MT.getRawSource(), Len});
    } else {
      // remote -> local
      B.CreateCall(shim("__wasp_remote_read", VoidTy, {P0, I32, I32}),
                   {MT.getRawDest(), refOf(B, MT.getRawSource()), Len});
    }
    return true;
  }

  FunctionCallee shim(const std::string &Name, Type *Ret,
                      ArrayRef<Type *> Args) {
    // Store shims take (ref, value); fix up the single-arg spelling used
    // by lowerStore for scalar types.
    SmallVector<Type *, 3> Params;
    if (Name.rfind("__wasp_remote_store_", 0) == 0 && Args.size() == 1) {
      Params = {I32, Args[0]};
    } else {
      Params.append(Args.begin(), Args.end());
    }
    return M.getOrInsertFunction(Name,
                                 FunctionType::get(Ret, Params, false));
  }
};

struct WaspRemotePass : PassInfoMixin<WaspRemotePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    return WaspRemoteLowering(M).run() ? PreservedAnalyses::none()
                                       : PreservedAnalyses::all();
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "wasp-remote", "0.1",
          [](PassBuilder &PB) {
            // Run before the optimizer: every AS100 access is still an
            // individual load/store/memcpy straight out of clang, and
            // afterwards the optimizer treats the shim calls as opaque.
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(WaspRemotePass());
                });
          }};
}
