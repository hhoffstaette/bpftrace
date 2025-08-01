#include <filesystem>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>

#include "arch/arch.h"
#include "ast/async_event_types.h"
#include "ast/codegen_helper.h"
#include "ast/irbuilderbpf.h"
#include "async_action.h"
#include "bpfmap.h"
#include "bpftrace.h"
#include "globalvars.h"
#include "log.h"
#include "types.h"
#include "util/bpf_funcs.h"
#include "util/exceptions.h"

namespace bpftrace::ast {

namespace {
std::string probeReadHelperName(libbpf::bpf_func_id id)
{
  switch (id) {
    case libbpf::BPF_FUNC_probe_read:
      return "probe_read";
    case libbpf::BPF_FUNC_probe_read_user:
      return "probe_read_user";
    case libbpf::BPF_FUNC_probe_read_kernel:
      return "probe_read_kernel";
    case libbpf::BPF_FUNC_probe_read_str:
      return "probe_read_str";
    case libbpf::BPF_FUNC_probe_read_user_str:
      return "probe_read_user_str";
    case libbpf::BPF_FUNC_probe_read_kernel_str:
      return "probe_read_kernel_str";
    default:
      LOG(BUG) << "unknown probe_read id: " << std::to_string(id);
  }
  __builtin_unreachable();
}
} // namespace

libbpf::bpf_func_id IRBuilderBPF::selectProbeReadHelper(AddrSpace as, bool str)
{
  libbpf::bpf_func_id fn;
  // Assume that if a kernel has probe_read_kernel it has the other 3 too
  if (bpftrace_.feature_->has_helper_probe_read_kernel()) {
    if (as == AddrSpace::kernel) {
      fn = str ? libbpf::BPF_FUNC_probe_read_kernel_str
               : libbpf::BPF_FUNC_probe_read_kernel;
    } else if (as == AddrSpace::user) {
      fn = str ? libbpf::BPF_FUNC_probe_read_user_str
               : libbpf::BPF_FUNC_probe_read_user;
    } else {
      // if the kernel has the new helpers but AS is still none it is a bug
      // in bpftrace, assert catches it for debug builds.
      // assert(as != AddrSpace::none);
      static bool warnonce = false;
      if (!warnonce) {
        warnonce = true;
        LOG(WARNING) << "Addrspace is not set";
      }
      fn = str ? libbpf::BPF_FUNC_probe_read_str : libbpf::BPF_FUNC_probe_read;
    }
  } else {
    fn = str ? libbpf::BPF_FUNC_probe_read_str : libbpf::BPF_FUNC_probe_read;
  }

  return fn;
}

// This constant is defined in the Linux kernel's proc_ns.h
// It represents the inode of the initial (global) PID namespace
constexpr uint32_t PROC_PID_INIT_INO = 0xeffffffc;

Value *IRBuilderBPF::CreateGetPid(const Location &loc, bool force_init)
{
  const auto &pidns = bpftrace_.get_pidns_self_stat();
  if (!force_init && pidns && pidns->st_ino != PROC_PID_INIT_INO) {
    // Get namespaced target PID when we're running in a namespace
    AllocaInst *res = CreateAllocaBPF(BpfPidnsInfoType(), "bpf_pidns_info");
    CreateGetNsPidTgid(
        getInt64(pidns->st_dev), getInt64(pidns->st_ino), res, loc);
    Value *pid = CreateLoad(
        getInt32Ty(),
        CreateGEP(BpfPidnsInfoType(), res, { getInt32(0), getInt32(0) }));
    CreateLifetimeEnd(res);
    return pid;
  }

  // Get global target PID otherwise
  Value *pidtgid = CreateGetPidTgid(loc);
  Value *pid = CreateTrunc(CreateLShr(pidtgid, 32), getInt32Ty(), "pid");
  return pid;
}

Value *IRBuilderBPF::CreateGetTid(const Location &loc, bool force_init)
{
  const auto &pidns = bpftrace_.get_pidns_self_stat();
  if (!force_init && pidns && pidns->st_ino != PROC_PID_INIT_INO) {
    // Get namespaced target TID when we're running in a namespace
    AllocaInst *res = CreateAllocaBPF(BpfPidnsInfoType(), "bpf_pidns_info");
    CreateGetNsPidTgid(
        getInt64(pidns->st_dev), getInt64(pidns->st_ino), res, loc);
    Value *tid = CreateLoad(
        getInt32Ty(),
        CreateGEP(BpfPidnsInfoType(), res, { getInt32(0), getInt32(1) }));
    CreateLifetimeEnd(res);
    return tid;
  }

  // Get global target TID otherwise
  Value *pidtgid = CreateGetPidTgid(loc);
  Value *tid = CreateTrunc(pidtgid, getInt32Ty(), "tid");
  return tid;
}

AllocaInst *IRBuilderBPF::CreateUSym(Value *val,
                                     int probe_id,
                                     const Location &loc)
{
  std::vector<llvm::Type *> elements = {
    getInt64Ty(), // addr
    getInt32Ty(), // pid
    getInt32Ty(), // probe id
  };
  StructType *usym_t = GetStructType("usym_t", elements, false);
  AllocaInst *buf = CreateAllocaBPF(usym_t, "usym");

  Value *pid = CreateGetPid(loc, false);
  Value *probe_id_val = Constant::getIntegerValue(getInt32Ty(),
                                                  APInt(32, probe_id));

  // The extra 0 here ensures the type of addr_offset will be int64
  Value *addr_offset = CreateGEP(usym_t, buf, { getInt64(0), getInt32(0) });
  Value *pid_offset = CreateGEP(usym_t, buf, { getInt64(0), getInt32(1) });
  Value *probeid_offset = CreateGEP(usym_t, buf, { getInt64(0), getInt32(2) });

  CreateStore(val, addr_offset);
  CreateStore(pid, pid_offset);
  CreateStore(probe_id_val, probeid_offset);
  return buf;
}

StructType *IRBuilderBPF::GetStackStructType(bool is_ustack)
{
  // Kernel stacks should not be differentiated by pid, since the kernel
  // address space is the same between pids (and when aggregating you *want*
  // to be able to correlate between pids in most cases). User-space stacks
  // are special because of ASLR, hence we also store the pid; probe id is
  // stored for cases when only ELF resolution works (e.g. ASLR disabled and
  // process exited).
  if (is_ustack) {
    std::vector<llvm::Type *> elements{
      getInt64Ty(), // stack id
      getInt64Ty(), // nr_stack_frames
      getInt32Ty(), // pid
      getInt32Ty(), // probe id
    };
    return GetStructType("ustack_key", elements, false);
  } else {
    std::vector<llvm::Type *> elements{
      getInt64Ty(), // stack id
      getInt64Ty(), // nr_stack_frames
    };
    return GetStructType("kstack_key", elements, false);
  }
}

StructType *IRBuilderBPF::GetStructType(
    const std::string &name,
    const std::vector<llvm::Type *> &elements,
    bool packed)
{
  auto search = structs_.find(name);
  if (search != structs_.end())
    return search->second;

  StructType *s = StructType::create(elements, name, packed);
  structs_.insert({ name, s });
  return s;
}

IRBuilderBPF::IRBuilderBPF(LLVMContext &context,
                           Module &module,
                           BPFtrace &bpftrace,
                           AsyncIds &async_ids)
    : IRBuilder<>(context),
      module_(module),
      bpftrace_(bpftrace),
      async_ids_(async_ids)
{
  // Declare external LLVM function
  FunctionType *pseudo_func_type = FunctionType::get(
      getInt64Ty(), { getInt64Ty(), getInt64Ty() }, false);
  llvm::Function::Create(pseudo_func_type,
                         GlobalValue::ExternalLinkage,
                         "llvm.bpf.pseudo",
                         &module_);
}

void IRBuilderBPF::hoist(const std::function<void()> &functor)
{
  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock &entry_block = parent->getEntryBlock();

  auto ip = saveIP();
  if (entry_block.empty())
    SetInsertPoint(&entry_block);
  else
    SetInsertPoint(&entry_block.front());

  functor();
  restoreIP(ip);
}

AllocaInst *IRBuilderBPF::CreateAllocaBPF(llvm::Type *ty,
                                          const std::string &name)
{
  // Anything this large should be allocated in a scratch map instead
  assert(module_.getDataLayout().getTypeAllocSize(ty) <= 256);

  AllocaInst *alloca;
  hoist([this, ty, &name, &alloca]() {
    alloca = CreateAlloca(ty, nullptr, name);
  });

  CreateLifetimeStart(alloca);
  return alloca;
}

AllocaInst *IRBuilderBPF::CreateAllocaBPF(const SizedType &stype,
                                          const std::string &name)
{
  llvm::Type *ty = GetType(stype);
  return CreateAllocaBPF(ty, name);
}

void IRBuilderBPF::CreateAllocationInit(const SizedType &stype, Value *alloc)
{
  if (needMemcpy(stype)) {
    CreateMemsetBPF(alloc, getInt8(0), stype.GetSize());
  } else {
    CreateStore(ConstantInt::get(GetType(stype), 0), alloc);
  }
}

AllocaInst *IRBuilderBPF::CreateAllocaBPFInit(const SizedType &stype,
                                              const std::string &name)
{
  // Anything this large should be allocated in a scratch map instead
  assert(stype.GetSize() <= 256);

  AllocaInst *alloca;
  hoist([this, &stype, &name, &alloca]() {
    llvm::Type *ty = GetType(stype);
    alloca = CreateAlloca(ty, nullptr, name);
    CreateLifetimeStart(alloca);
    CreateAllocationInit(stype, alloca);
  });
  return alloca;
}

AllocaInst *IRBuilderBPF::CreateAllocaBPF(int bytes, const std::string &name)
{
  llvm::Type *ty = ArrayType::get(getInt8Ty(), bytes);
  return CreateAllocaBPF(ty, name);
}

void IRBuilderBPF::CreateMemsetBPF(Value *ptr, Value *val, uint32_t size)
{
  if (size > 512 && bpftrace_.feature_->has_helper_probe_read_kernel()) {
    // Note we are "abusing" bpf_probe_read_kernel() by reading from NULL
    // which triggers a call into the kernel-optimized memset().
    //
    // Upstream blesses this trick so we should be able to count on them
    // to maintain these semantics.
    //
    // Also note we are avoiding a call to CreateProbeRead(), as it wraps
    // calls to probe read helpers with the -k error reporting feature.
    // The call here will always fail and we want it that way. So avoid
    // reporting errors to the user.
    auto probe_read_id = libbpf::BPF_FUNC_probe_read_kernel;
    FunctionType *proberead_func_type = FunctionType::get(
        getInt64Ty(),
        { ptr->getType(), getInt32Ty(), GetNull()->getType() },
        false);
    PointerType *proberead_func_ptr_type = PointerType::get(proberead_func_type,
                                                            0);
    Constant *proberead_func = ConstantExpr::getCast(Instruction::IntToPtr,
                                                     getInt64(probe_read_id),
                                                     proberead_func_ptr_type);
    createCall(proberead_func_type,
               proberead_func,
               { ptr, getInt32(size), GetNull() },
               probeReadHelperName(probe_read_id));
  } else {
    // Use unrolled memset for memsets less than 512 bytes mostly for
    // correctness.
    //
    // It appears that helper based memsets obscure LLVM stack optimizer view
    // into memory usage such that programs that were below stack limit with
    // builtin memsets will bloat with helper based memsets enough to where
    // LLVM BPF backend will barf.
    //
    // So only use helper based memset when we really need it. And that's when
    // we're memset()ing off-stack. We know it's off stack b/c 512 is program
    // stack limit.
    CreateMemSet(ptr, val, getInt64(size), MaybeAlign(1));
  }
}

void IRBuilderBPF::CreateMemcpyBPF(Value *dst, Value *src, uint32_t size)
{
  if (size > 512 && bpftrace_.feature_->has_helper_probe_read_kernel()) {
    // Note we are avoiding a call to CreateProbeRead(), as it wraps
    // calls to probe read helpers with the -k error reporting feature.
    //
    // Errors are not ever expected, as memcpy should only be used when
    // you're sure src and dst are both in BPF memory.
    auto probe_read_id = libbpf::BPF_FUNC_probe_read_kernel;
    FunctionType *probe_read_func_type = FunctionType::get(
        getInt64Ty(), { dst->getType(), getInt32Ty(), src->getType() }, false);
    PointerType *probe_read_func_ptr_type = PointerType::get(
        probe_read_func_type, 0);
    Constant *probe_read_func = ConstantExpr::getCast(Instruction::IntToPtr,
                                                      getInt64(probe_read_id),
                                                      probe_read_func_ptr_type);
    createCall(probe_read_func_type,
               probe_read_func,
               { dst, getInt32(size), src },
               probeReadHelperName(probe_read_id));
  } else {
    CreateMemCpy(dst, MaybeAlign(1), src, MaybeAlign(1), size);
  }
}

llvm::ConstantInt *IRBuilderBPF::GetIntSameSize(uint64_t C, llvm::Type *ty)
{
  assert(ty->isIntegerTy());
  unsigned size = ty->getIntegerBitWidth();
  return getIntN(size, C);
}

llvm::ConstantInt *IRBuilderBPF::GetIntSameSize(uint64_t C, llvm::Value *expr)
{
  unsigned size = expr->getType()->getIntegerBitWidth();
  return getIntN(size, C);
}

/// Convert internal SizedType to a corresponding LLVM type.
///
/// For convenience, some types are not converted into a directly corresponding
/// type but instead into a type which is easy to work with in BPF programs
/// (e.g. store it in maps, etc.). This is the case for two particular types:
/// - pointers are represented as i64
/// - structs (records) are represented as byte arrays.
///
/// Setting `emit_codegen_types` to false (it is true by default) will change
/// this behaviour and emit the exact corresponding types. This is typically
/// necessary when creating a type which must exactly match the type in the
/// kernel BTF (e.g. a kernel function (kfunc) prototype).
///
/// At the moment, `emit_codegen_types=false` only applies to pointers as it is
/// sufficient for our use cases (and we don't need to bother with emitting
/// struct types with all the fields). This should be changed eventually.
llvm::Type *IRBuilderBPF::GetType(const SizedType &stype,
                                  bool emit_codegen_types)
{
  llvm::Type *ty;
  if (stype.IsByteArray() || stype.IsRecordTy()) {
    ty = ArrayType::get(getInt8Ty(), stype.GetSize());
  } else if (stype.IsArrayTy()) {
    ty = ArrayType::get(GetType(*stype.GetElementTy()), stype.GetNumElements());
  } else if (stype.IsTupleTy()) {
    std::vector<llvm::Type *> llvm_elems;
    std::string ty_name;

    for (const auto &elem : stype.GetFields()) {
      const auto &elemtype = elem.type;
      llvm_elems.emplace_back(GetType(elemtype));
      ty_name += typestr(elemtype, true) + "_";
    }
    ty_name += "_tuple_t";

    ty = GetStructType(ty_name, llvm_elems, false);
  } else if (stype.IsStack()) {
    ty = GetStackStructType(stype.IsUstackTy());
  } else if (stype.IsPtrTy()) {
    if (emit_codegen_types)
      ty = getInt64Ty();
    else
      ty = getPtrTy();
  } else if (stype.IsVoidTy()) {
    ty = getVoidTy();
  } else {
    switch (stype.GetSize()) {
      case 16:
        ty = getInt128Ty();
        break;
      case 8:
        ty = getInt64Ty();
        break;
      case 4:
        ty = getInt32Ty();
        break;
      case 2:
        ty = getInt16Ty();
        break;
      case 1:
        ty = getInt8Ty();
        break;
      default:
        LOG(BUG) << stype.GetSize() << " is not a valid type size for GetType";
    }
  }
  return ty;
}

llvm::Type *IRBuilderBPF::GetMapValueType(const SizedType &stype)
{
  llvm::Type *ty;
  if (stype.IsMinTy() || stype.IsMaxTy()) {
    // The first field is the value
    // The second field is the "value is set" flag
    std::vector<llvm::Type *> llvm_elems = { getInt64Ty(), getInt64Ty() };
    ty = GetStructType("min_max_val", llvm_elems, false);
  } else if (stype.IsAvgTy() || stype.IsStatsTy()) {
    // The first field is the total value
    // The second is the count value
    std::vector<llvm::Type *> llvm_elems = { getInt64Ty(), getInt64Ty() };
    ty = GetStructType("avg_stas_val", llvm_elems, false);
  } else if (stype.IsTSeriesTy()) {
    std::vector<llvm::Type *> llvm_elems = { getInt64Ty(),
                                             getInt64Ty(),
                                             getInt64Ty() };
    ty = GetStructType("t_series_val", llvm_elems, false);
  } else {
    ty = GetType(stype);
  }

  return ty;
}

/// Creates a call to a BPF helper function
///
/// A call to a helper function can be marked as "pure" to allow LLVM to
/// optimise around it.
///
/// ** BE VERY CAREFUL when marking a helper function as pure - marking an
/// impure helper as pure can result in undefined behaviour. **
///
/// Guidelines for deciding if a helper can be considered pure:
/// - It must always return the same value when called repeatedly with the same
///   arguments within a single probe's context
/// - It must not read or write any memory in the BPF address space (e.g. it
///   mustn't take any pointers to BPF memory as parameters)
/// - It must not have any intentional side effects outside of the BPF address
///   space, otherwise these may be optimised out, e.g. pushing to the ring
///   buffer, signalling a process
CallInst *IRBuilderBPF::CreateHelperCall(libbpf::bpf_func_id func_id,
                                         FunctionType *helper_type,
                                         ArrayRef<Value *> args,
                                         bool is_pure,
                                         const Twine &Name,
                                         const Location &loc)
{
  bpftrace_.helper_use_loc_[func_id].emplace_back(RuntimeErrorId::HELPER_ERROR,
                                                  func_id,
                                                  loc);
  PointerType *helper_ptr_type = PointerType::get(helper_type, 0);
  Constant *helper_func = ConstantExpr::getCast(Instruction::IntToPtr,
                                                getInt64(func_id),
                                                helper_ptr_type);
  CallInst *call = createCall(helper_type, helper_func, args, Name);

  // When we tell LLVM that this function call "does not access memory", this
  // only refers to BPF memory. Accessing kernel or user memory is fine within
  // a "pure helper", as we can consider kernel/user memory as constant.
  if (is_pure)
    call->setDoesNotAccessMemory();
  return call;
}

CallInst *IRBuilderBPF::createCall(FunctionType *callee_type,
                                   Value *callee,
                                   ArrayRef<Value *> args,
                                   const Twine &Name)
{
  return CreateCall(callee_type, callee, args, Name);
}

Value *IRBuilderBPF::GetMapVar(const std::string &map_name)
{
  return module_.getGlobalVariable(bpf_map_name(map_name));
}

Value *IRBuilderBPF::GetNull()
{
  return ConstantExpr::getCast(Instruction::IntToPtr, getInt64(0), getPtrTy());
}

CallInst *IRBuilderBPF::CreateMapLookup(Map &map,
                                        Value *key,
                                        const std::string &name)
{
  return createMapLookup(map.ident, key, name);
}

CallInst *IRBuilderBPF::createMapLookup(const std::string &map_name,
                                        Value *key,
                                        const std::string &name)
{
  Value *map_ptr = GetMapVar(map_name);
  // void *map_lookup_elem(struct bpf_map * map, void * key)
  // Return: Map value or NULL

  assert(key->getType()->isPointerTy());
  FunctionType *lookup_func_type = FunctionType::get(
      getPtrTy(), { map_ptr->getType(), key->getType() }, false);
  PointerType *lookup_func_ptr_type = PointerType::get(lookup_func_type, 0);
  Constant *lookup_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_map_lookup_elem),
      lookup_func_ptr_type);
  return createCall(lookup_func_type, lookup_func, { map_ptr, key }, name);
}

CallInst *IRBuilderBPF::createPerCpuMapLookup(const std::string &map_name,
                                              Value *key,
                                              Value *cpu,
                                              const std::string &name)
{
  Value *map_ptr = GetMapVar(map_name);
  // void *map_lookup_percpu_elem(struct bpf_map * map, void * key, u32 cpu)
  // Return: Map value or NULL

  assert(key->getType()->isPointerTy());
  FunctionType *lookup_func_type = FunctionType::get(
      getPtrTy(), { map_ptr->getType(), key->getType(), getInt32Ty() }, false);
  PointerType *lookup_func_ptr_type = PointerType::get(lookup_func_type, 0);
  Constant *lookup_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_map_lookup_percpu_elem),
      lookup_func_ptr_type);
  return createCall(lookup_func_type, lookup_func, { map_ptr, key, cpu }, name);
}

CallInst *IRBuilderBPF::CreateGetJoinMap(BasicBlock *failure_callback,
                                         const Location &loc)
{
  return createGetScratchMap(
      to_string(MapType::Join), "join", loc, failure_callback);
}

CallInst *IRBuilderBPF::CreateGetStackScratchMap(StackType stack_type,
                                                 BasicBlock *failure_callback,
                                                 const Location &loc)
{
  SizedType value_type = CreateArray(stack_type.limit, CreateUInt64());
  return createGetScratchMap(StackType::scratch_name(),
                             StackType::scratch_name(),
                             loc,
                             failure_callback);
}

Value *IRBuilderBPF::CreateGetStrAllocation(const std::string &name,
                                            const Location &loc,
                                            uint64_t pad)
{
  const auto max_strlen = bpftrace_.config_->max_strlen + pad;
  const auto str_type = CreateArray(max_strlen, CreateInt8());
  return createAllocation(bpftrace::globalvars::GET_STR_BUFFER,
                          GetType(str_type),
                          name,
                          loc,
                          [](AsyncIds &async_ids) { return async_ids.str(); });
}

Value *IRBuilderBPF::CreateGetFmtStringArgsAllocation(StructType *struct_type,
                                                      const std::string &name,
                                                      const Location &loc)
{
  return createAllocation(
      bpftrace::globalvars::FMT_STRINGS_BUFFER, struct_type, name, loc);
}

Value *IRBuilderBPF::CreateTupleAllocation(const SizedType &tuple_type,
                                           const std::string &name,
                                           const Location &loc)
{
  return createAllocation(bpftrace::globalvars::TUPLE_BUFFER,
                          GetType(tuple_type),
                          name,
                          loc,
                          [](AsyncIds &async_ids) {
                            return async_ids.tuple();
                          });
}

Value *IRBuilderBPF::CreateReadMapValueAllocation(const SizedType &value_type,
                                                  const std::string &name,
                                                  const Location &loc)
{
  return createAllocation(bpftrace::globalvars::READ_MAP_VALUE_BUFFER,
                          GetType(value_type),
                          name,
                          loc,
                          [](AsyncIds &async_ids) {
                            return async_ids.read_map_value();
                          });
}

Value *IRBuilderBPF::CreateWriteMapValueAllocation(const SizedType &value_type,
                                                   const std::string &name,
                                                   const Location &loc)
{
  return createAllocation(bpftrace::globalvars::WRITE_MAP_VALUE_BUFFER,
                          GetType(value_type),
                          name,
                          loc);
}

Value *IRBuilderBPF::CreateVariableAllocationInit(const SizedType &value_type,
                                                  const std::string &name,
                                                  const Location &loc)
{
  // Hoist variable declaration and initialization to entry point of
  // probe/subprogram. While we technically do not need this as variables
  // are properly scoped, it eases debugging and is consistent with previous
  // stack-only variable implementation.
  Value *alloc;
  hoist([this, &value_type, &name, &loc, &alloc] {
    alloc = createAllocation(bpftrace::globalvars::VARIABLE_BUFFER,
                             GetType(value_type),
                             name,
                             loc,
                             [](AsyncIds &async_ids) {
                               return async_ids.variable();
                             });
    CreateAllocationInit(value_type, alloc);
  });
  return alloc;
}

Value *IRBuilderBPF::CreateMapKeyAllocation(const SizedType &value_type,
                                            const std::string &name,
                                            const Location &loc)
{
  return createAllocation(bpftrace::globalvars::MAP_KEY_BUFFER,
                          GetType(value_type),
                          name,
                          loc,
                          [](AsyncIds &async_ids) {
                            return async_ids.map_key();
                          });
}

Value *IRBuilderBPF::createAllocation(
    std::string_view global_var_name,
    llvm::Type *obj_type,
    const std::string &name,
    const Location &loc,
    std::optional<std::function<size_t(AsyncIds &)>> gen_async_id_cb)
{
  const auto obj_size = module_.getDataLayout().getTypeAllocSize(obj_type);
  const auto on_stack_limit = bpftrace_.config_->on_stack_limit;
  if (obj_size > on_stack_limit) {
    return createScratchBuffer(global_var_name,
                               loc,
                               gen_async_id_cb ? (*gen_async_id_cb)(async_ids_)
                                               : 0);
  }
  return CreateAllocaBPF(obj_type, name);
}

Value *IRBuilderBPF::createScratchBuffer(std::string_view global_var_name,
                                         const Location &loc,
                                         size_t key)
{
  // These specific global variables are nested arrays
  // (see get_sized_type in globalvars.cpp).
  // The top level array is for each CPU where the length is
  // MAX_CPU_ID + 1. This is so there is no contention between CPUs
  // when accessing this global value.

  // The second level array is for each key where the length is
  // the number of elements for this specific global, e.g. if
  // there are multiple strings that can't fit on the BPF stack
  // then there will be one element per string.

  // The last level is either an array of bytes (e.g. for strings)
  // or a single value (e.g. for ints like the EVENT_LOSS_COUNTER)
  const auto global_name = std::string(global_var_name);
  auto sized_type = bpftrace_.resources.global_vars.get_sized_type(
      global_name, bpftrace_.resources, *bpftrace_.config_);
  auto *cpu_id = CreateGetCpuId(loc);
  auto *max = CreateLoad(getInt64Ty(),
                         module_.getGlobalVariable(
                             std::string(bpftrace::globalvars::MAX_CPU_ID)));
  // Mask CPU ID by MAX_CPU_ID to ensure BPF verifier knows CPU ID is bounded
  // on older kernels. Note this means MAX_CPU_ID must be 2^N - 1 for some N.
  // See get_max_cpu_id() for more details.
  auto *bounded_cpu_id = CreateAnd(cpu_id, max, "cpu.id.bounded");

  // Note the 1st index is 0 because we're pointing to
  // ValueType var[MAX_CPU_ID + 1][num_elements]
  // More details on using GEP: https://llvm.org/docs/LangRef.html#id236
  if (sized_type.GetElementTy()->GetElementTy()->IsArrayTy()) {
    return CreateGEP(
        GetType(sized_type),
        module_.getGlobalVariable(global_name),
        { getInt64(0), bounded_cpu_id, getInt64(key), getInt64(0) });
  }

  return CreateGEP(GetType(sized_type),
                   module_.getGlobalVariable(global_name),
                   { getInt64(0), bounded_cpu_id, getInt64(key) });
}

// Failure to lookup a scratch map will result in a jump to the
// failure_callback, if non-null.
//
// In practice, a properly constructed percpu lookup will never fail. The only
// way it can fail is if we have a bug in our code. So a null failure_callback
// simply causes a blind 0 return. See comment in function for why this is ok.
CallInst *IRBuilderBPF::createGetScratchMap(const std::string &map_name,
                                            const std::string &name,
                                            const Location &loc,
                                            BasicBlock *failure_callback,
                                            int key)
{
  AllocaInst *keyAlloc = CreateAllocaBPF(getInt32Ty(),
                                         "lookup_" + name + "_key");
  CreateStore(getInt32(key), keyAlloc);

  CallInst *call = createMapLookup(map_name,
                                   keyAlloc,
                                   "lookup_" + name + "_map");
  CreateLifetimeEnd(keyAlloc);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *lookup_failure_block = BasicBlock::Create(
      module_.getContext(), "lookup_" + name + "_failure", parent);
  BasicBlock *lookup_merge_block = BasicBlock::Create(
      module_.getContext(), "lookup_" + name + "_merge", parent);
  Value *condition = CreateICmpNE(CreateIntCast(call, getPtrTy(), true),
                                  GetNull(),
                                  "lookup_" + name + "_cond");
  CreateCondBr(condition, lookup_merge_block, lookup_failure_block);

  SetInsertPoint(lookup_failure_block);
  CreateDebugOutput("unable to find the scratch map value for " + name,
                    std::vector<Value *>{},
                    loc);
  if (failure_callback) {
    CreateBr(failure_callback);
  } else {
    // Think of this like an assert(). In practice, we cannot fail to lookup a
    // percpu array map unless we have a coding error. Rather than have some
    // kind of complicated fallback path where we provide an error string for
    // our caller, just indicate to verifier we want to terminate execution.
    //
    // Note that we blindly return 0 in contrast to the logic inside
    // CodegenLLVM::createRet(). That's b/c the return value doesn't matter
    // if it'll never get executed.
    CreateRet(getInt64(0));
  }

  SetInsertPoint(lookup_merge_block);
  return call;
}

Value *IRBuilderBPF::CreateMapLookupElem(Map &map,
                                         Value *key,
                                         const Location &loc)
{
  return CreateMapLookupElem(map.ident, key, map.value_type, loc);
}

Value *IRBuilderBPF::CreateMapLookupElem(const std::string &map_name,
                                         Value *key,
                                         SizedType &type,
                                         const Location &loc)
{
  CallInst *call = createMapLookup(map_name, key);

  // Check if result == 0
  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *lookup_success_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_success",
                                                        parent);
  BasicBlock *lookup_failure_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_failure",
                                                        parent);
  BasicBlock *lookup_merge_block = BasicBlock::Create(module_.getContext(),
                                                      "lookup_merge",
                                                      parent);

  Value *value = CreateReadMapValueAllocation(type, "lookup_elem_val", loc);
  Value *condition = CreateICmpNE(CreateIntCast(call, getPtrTy(), true),
                                  GetNull(),
                                  "map_lookup_cond");
  CreateCondBr(condition, lookup_success_block, lookup_failure_block);

  SetInsertPoint(lookup_success_block);
  if (needMemcpy(type))
    CreateMemcpyBPF(value, call, type.GetSize());
  else {
    assert(GetType(type) == getInt64Ty());
    CreateStore(CreateLoad(getInt64Ty(), call), value);
  }
  CreateBr(lookup_merge_block);

  SetInsertPoint(lookup_failure_block);
  if (needMemcpy(type))
    CreateMemsetBPF(value, getInt8(0), type.GetSize());
  else
    CreateStore(getInt64(0), value);
  CreateRuntimeError(RuntimeErrorId::HELPER_ERROR,
                     getInt32(0),
                     libbpf::BPF_FUNC_map_lookup_elem,
                     loc);
  CreateBr(lookup_merge_block);

  SetInsertPoint(lookup_merge_block);
  if (needMemcpy(type))
    return value;

  // value is a pointer to i64
  Value *ret = CreateLoad(getInt64Ty(), value);
  if (dyn_cast<AllocaInst>(value))
    CreateLifetimeEnd(value);
  return ret;
}

Value *IRBuilderBPF::CreatePerCpuMapAggElems(Map &map,
                                             Value *key,
                                             const SizedType &type,
                                             const Location &loc)
{
  // int ret = 0;
  // int i = 0;
  // while (i < nr_cpus) {
  //   int * cpu_value = map_lookup_percpu_elem(map, key, i);
  //   if (cpu_value == NULL) {
  //     if (i == 0)
  //        log_error("Key not found")
  //     else
  //        debug("No cpu found for cpu id: %lu", i) // Mostly for AOT
  //     break;
  //   }
  //   // update ret for sum, count, avg, min, max
  //   i++;
  // }
  // return ret;

  const std::string &map_name = map.ident;

  AllocaInst *i = CreateAllocaBPF(getInt32Ty(), "i");
  AllocaInst *val_1 = CreateAllocaBPF(getInt64Ty(), "val_1");
  // used for min/max/avg
  AllocaInst *val_2 = CreateAllocaBPF(getInt64Ty(), "val_2");

  CreateStore(getInt32(0), i);
  CreateStore(getInt64(0), val_1);
  CreateStore(getInt64(0), val_2);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *while_cond = BasicBlock::Create(module_.getContext(),
                                              "while_cond",
                                              parent);
  BasicBlock *while_body = BasicBlock::Create(module_.getContext(),
                                              "while_body",
                                              parent);
  BasicBlock *while_end = BasicBlock::Create(module_.getContext(),
                                             "while_end",
                                             parent);
  CreateBr(while_cond);
  SetInsertPoint(while_cond);

  auto *cond = CreateICmp(CmpInst::ICMP_ULT,
                          CreateLoad(getInt32Ty(), i),
                          CreateLoad(getInt32Ty(),
                                     module_.getGlobalVariable(std::string(
                                         bpftrace::globalvars::NUM_CPUS))),
                          "num_cpu.cmp");
  CreateCondBr(cond, while_body, while_end);

  SetInsertPoint(while_body);

  CallInst *call = createPerCpuMapLookup(map_name,
                                         key,
                                         CreateLoad(getInt32Ty(), i));

  llvm::Function *lookup_parent = GetInsertBlock()->getParent();
  BasicBlock *lookup_success_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_success",
                                                        lookup_parent);
  BasicBlock *lookup_failure_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_failure",
                                                        lookup_parent);
  Value *condition = CreateICmpNE(CreateIntCast(call, getPtrTy(), true),
                                  GetNull(),
                                  "map_lookup_cond");
  CreateCondBr(condition, lookup_success_block, lookup_failure_block);

  SetInsertPoint(lookup_success_block);

  if (type.IsMinTy() || type.IsMaxTy()) {
    createPerCpuMinMax(val_1, val_2, call, type);
  } else if (type.IsAvgTy()) {
    createPerCpuAvg(val_1, val_2, call, type);
  } else if (type.IsSumTy() || type.IsCountTy()) {
    createPerCpuSum(val_1, call, type);
  } else {
    LOG(BUG) << "Unsupported map aggregation type: " << type;
  }

  // ++i;
  CreateStore(CreateAdd(CreateLoad(getInt32Ty(), i), getInt32(1)), i);

  CreateBr(while_cond);
  SetInsertPoint(lookup_failure_block);

  llvm::Function *error_parent = GetInsertBlock()->getParent();
  BasicBlock *error_success_block = BasicBlock::Create(module_.getContext(),
                                                       "error_success",
                                                       error_parent);
  BasicBlock *error_failure_block = BasicBlock::Create(module_.getContext(),
                                                       "error_failure",
                                                       error_parent);

  // If the CPU is 0 and the map lookup fails it means the key doesn't exist
  Value *error_condition = CreateICmpEQ(CreateLoad(getInt32Ty(), i),
                                        getInt32(0),
                                        "error_lookup_cond");
  CreateCondBr(error_condition, error_success_block, error_failure_block);

  SetInsertPoint(error_success_block);

  CreateRuntimeError(RuntimeErrorId::HELPER_ERROR,
                     getInt32(0),
                     libbpf::BPF_FUNC_map_lookup_percpu_elem,
                     loc);
  CreateBr(while_end);

  SetInsertPoint(error_failure_block);

  // This should only get triggered in the AOT case
  CreateDebugOutput("No cpu found for cpu id: %lu",
                    std::vector<Value *>{ CreateLoad(getInt32Ty(), i) },
                    loc);

  CreateBr(while_end);

  SetInsertPoint(while_end);

  CreateLifetimeEnd(i);

  Value *ret_reg;

  if (type.IsAvgTy()) {
    AllocaInst *ret = CreateAllocaBPF(getInt64Ty(), "ret");
    // BPF doesn't yet support a signed division so we have to check if
    // the value is negative, flip it, do an unsigned division, and then
    // flip it back
    if (type.IsSigned()) {
      llvm::Function *avg_parent = GetInsertBlock()->getParent();
      BasicBlock *is_negative_block = BasicBlock::Create(module_.getContext(),
                                                         "is_negative",
                                                         avg_parent);
      BasicBlock *is_positive_block = BasicBlock::Create(module_.getContext(),
                                                         "is_positive",
                                                         avg_parent);
      BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                                   "is_negative_merge_block",
                                                   avg_parent);

      Value *is_negative_condition = CreateICmpSLT(
          CreateLoad(getInt64Ty(), val_1), getInt64(0), "is_negative_cond");
      CreateCondBr(is_negative_condition, is_negative_block, is_positive_block);

      SetInsertPoint(is_negative_block);

      Value *pos_total = CreateAdd(CreateNot(CreateLoad(getInt64Ty(), val_1)),
                                   getInt64(1));
      Value *pos_avg = CreateUDiv(pos_total, CreateLoad(getInt64Ty(), val_2));
      CreateStore(CreateNeg(pos_avg), ret);

      CreateBr(merge_block);

      SetInsertPoint(is_positive_block);

      CreateStore(CreateUDiv(CreateLoad(getInt64Ty(), val_1),
                             CreateLoad(getInt64Ty(), val_2)),
                  ret);

      CreateBr(merge_block);

      SetInsertPoint(merge_block);
      ret_reg = CreateLoad(getInt64Ty(), ret);
      CreateLifetimeEnd(ret);
    } else {
      ret_reg = CreateUDiv(CreateLoad(getInt64Ty(), val_1),
                           CreateLoad(getInt64Ty(), val_2));
    }
  } else {
    ret_reg = CreateLoad(getInt64Ty(), val_1);
  }

  CreateLifetimeEnd(val_1);
  CreateLifetimeEnd(val_2);
  return ret_reg;
}

void IRBuilderBPF::createPerCpuSum(AllocaInst *ret,
                                   CallInst *call,
                                   const SizedType &type)
{
  CreateStore(CreateAdd(CreateLoad(GetType(type), call),
                        CreateLoad(getInt64Ty(), ret)),
              ret);
}

void IRBuilderBPF::createPerCpuMinMax(AllocaInst *ret,
                                      AllocaInst *is_ret_set,
                                      CallInst *call,
                                      const SizedType &type)
{
  auto *value_type = GetMapValueType(type);
  bool is_max = type.IsMaxTy();

  Value *mm_val = CreateLoad(
      getInt64Ty(), CreateGEP(value_type, call, { getInt64(0), getInt32(0) }));

  Value *is_val_set = CreateLoad(
      getInt64Ty(), CreateGEP(value_type, call, { getInt64(0), getInt32(1) }));

  // (ret, is_ret_set, min_max_val, is_val_set) {
  // // if the min_max_val is 0, which is the initial map value,
  // // we need to know if it was explicitly set by user
  // if (!is_val_set == 1) {
  //   return;
  // }
  // if (!is_ret_set == 1) {
  //   ret = min_max_val;
  //   is_ret_set = 1;
  // } else if (min_max_val > ret) { // or min_max_val < ret if min operation
  //   ret = min_max_val;
  //   is_ret_set = 1;
  // }

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *val_set_success = BasicBlock::Create(module_.getContext(),
                                                   "val_set_success",
                                                   parent);
  BasicBlock *min_max_success = BasicBlock::Create(module_.getContext(),
                                                   "min_max_success",
                                                   parent);
  BasicBlock *ret_set_success = BasicBlock::Create(module_.getContext(),
                                                   "ret_set_success",
                                                   parent);
  BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                               "min_max_merge",
                                               parent);

  Value *val_set_condition = CreateICmpEQ(is_val_set,
                                          getInt64(1),
                                          "val_set_cond");

  Value *ret_set_condition = CreateICmpEQ(CreateLoad(getInt64Ty(), is_ret_set),
                                          getInt64(1),
                                          "ret_set_cond");

  Value *min_max_condition;

  if (is_max) {
    min_max_condition =
        type.IsSigned()
            ? CreateICmpSGT(mm_val, CreateLoad(getInt64Ty(), ret), "max_cond")
            : CreateICmpUGT(mm_val, CreateLoad(getInt64Ty(), ret), "max_cond");
  } else {
    min_max_condition =
        type.IsSigned()
            ? CreateICmpSLT(mm_val, CreateLoad(getInt64Ty(), ret), "min_cond")
            : CreateICmpULT(mm_val, CreateLoad(getInt64Ty(), ret), "max_cond");
  }

  // if (is_val_set == 1)
  CreateCondBr(val_set_condition, val_set_success, merge_block);

  SetInsertPoint(val_set_success);

  // if (is_ret_set == 1)
  CreateCondBr(ret_set_condition, ret_set_success, min_max_success);

  SetInsertPoint(ret_set_success);

  // if (min_max_val > ret) or if (min_max_val < ret)
  CreateCondBr(min_max_condition, min_max_success, merge_block);

  SetInsertPoint(min_max_success);

  // ret = cpu_value;
  CreateStore(mm_val, ret);
  // is_ret_set = 1;
  CreateStore(getInt64(1), is_ret_set);

  CreateBr(merge_block);

  SetInsertPoint(merge_block);
}

void IRBuilderBPF::createPerCpuAvg(AllocaInst *total,
                                   AllocaInst *count,
                                   CallInst *call,
                                   const SizedType &type)
{
  auto *value_type = GetMapValueType(type);

  Value *total_val = CreateLoad(
      getInt64Ty(), CreateGEP(value_type, call, { getInt64(0), getInt32(0) }));

  Value *count_val = CreateLoad(
      getInt64Ty(), CreateGEP(value_type, call, { getInt64(0), getInt32(1) }));

  CreateStore(CreateAdd(total_val, CreateLoad(getInt64Ty(), total)), total);
  CreateStore(CreateAdd(count_val, CreateLoad(getInt64Ty(), count)), count);
}

void IRBuilderBPF::CreateMapUpdateElem(const std::string &map_ident,
                                       Value *key,
                                       Value *val,
                                       const Location &loc,
                                       int64_t flags)
{
  Value *map_ptr = GetMapVar(map_ident);

  assert(key->getType()->isPointerTy());
  assert(val->getType()->isPointerTy());

  Value *flags_val = getInt64(flags);

  // long map_update_elem(struct bpf_map * map, void *key, void * value, u64
  // flags) Return: 0 on success or negative error
  FunctionType *update_func_type = FunctionType::get(
      getInt64Ty(),
      { map_ptr->getType(), key->getType(), val->getType(), getInt64Ty() },
      false);
  PointerType *update_func_ptr_type = PointerType::get(update_func_type, 0);
  Constant *update_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_map_update_elem),
      update_func_ptr_type);
  CallInst *call = createCall(update_func_type,
                              update_func,
                              { map_ptr, key, val, flags_val },
                              "update_elem");
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_map_update_elem, loc);
}

CallInst *IRBuilderBPF::CreateMapDeleteElem(Map &map,
                                            Value *key,
                                            bool ret_val_discarded,
                                            const Location &loc)
{
  assert(key->getType()->isPointerTy());
  Value *map_ptr = GetMapVar(map.ident);

  // long map_delete_elem(&map, &key)
  // Return: 0 on success or negative error
  FunctionType *delete_func_type = FunctionType::get(
      getInt64Ty(), { map_ptr->getType(), key->getType() }, false);
  PointerType *delete_func_ptr_type = PointerType::get(delete_func_type, 0);
  Constant *delete_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_map_delete_elem),
      delete_func_ptr_type);
  CallInst *call = createCall(
      delete_func_type, delete_func, { map_ptr, key }, "delete_elem");
  CreateHelperErrorCond(
      call, libbpf::BPF_FUNC_map_delete_elem, loc, !ret_val_discarded);

  return call;
}

Value *IRBuilderBPF::CreateForRange(Value *iters,
                                    Value *callback,
                                    Value *callback_ctx,
                                    const Location &loc)
{
  // long bpf_loop(__u32 nr_loops, void *callback_fn, void *callback_ctx, u64
  // flags)
  //
  // Return: 0 on success or negative error
  //
  // callback is long (*callback_fn)(u64 index, void *ctx);
  iters = CreateIntCast(iters, getInt32Ty(), true);
  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *is_positive_block = BasicBlock::Create(module_.getContext(),
                                                     "is_positive",
                                                     parent);
  BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                               "merge",
                                               parent);
  Value *is_positive = CreateICmpSGT(iters, getInt32(0), "is_positive_cond");
  CreateCondBr(is_positive, is_positive_block, merge_block);
  SetInsertPoint(is_positive_block);

  FunctionType *bpf_loop_type = FunctionType::get(
      getInt64Ty(),
      { getInt32Ty(), callback->getType(), getPtrTy(), getInt64Ty() },
      false);
  PointerType *bpf_loop_ptr_type = PointerType::get(bpf_loop_type, 0);

  Constant *bpf_loop_func = ConstantExpr::getCast(Instruction::IntToPtr,
                                                  getInt64(
                                                      libbpf::BPF_FUNC_loop),
                                                  bpf_loop_ptr_type);
  CallInst *call = createCall(
      bpf_loop_type,
      bpf_loop_func,
      { iters,
        callback,
        callback_ctx ? CreateIntToPtr(callback_ctx, getPtrTy()) : GetNull(),
        /*flags=*/getInt64(0) },
      "bpf_loop");
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_loop, loc);
  CreateBr(merge_block);

  SetInsertPoint(merge_block);
  return call;
}

Value *IRBuilderBPF::CreateForEachMapElem(Map &map,
                                          Value *callback,
                                          Value *callback_ctx,
                                          const Location &loc)
{
  Value *map_ptr = GetMapVar(map.ident);

  // long bpf_for_each_map_elem(struct bpf_map *map, void *callback_fn, void
  // *callback_ctx, u64 flags)
  //
  // Return: 0 on success or negative error
  //
  // callback is long (*callback_fn)(struct bpf_map *map, const void *key, void
  // *value, void *ctx);

  FunctionType *for_each_map_type = FunctionType::get(
      getInt64Ty(),
      { map_ptr->getType(), callback->getType(), getPtrTy(), getInt64Ty() },
      false);
  PointerType *for_each_map_ptr_type = PointerType::get(for_each_map_type, 0);

  Constant *for_each_map_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_for_each_map_elem),
      for_each_map_ptr_type);
  CallInst *call = createCall(
      for_each_map_type,
      for_each_map_func,
      { map_ptr,
        callback,
        callback_ctx ? CreateIntToPtr(callback_ctx, getPtrTy()) : GetNull(),
        /*flags=*/getInt64(0) },
      "for_each_map_elem");
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_for_each_map_elem, loc);
  return call;
}

void IRBuilderBPF::CreateCheckSetRecursion(const Location &loc,
                                           int early_exit_ret)
{
  const std::string map_ident = to_string(MapType::RecursionPrevention);

  AllocaInst *key = CreateAllocaBPF(getInt32Ty(), "lookup_key");
  CreateStore(getInt32(0), key);

  CallInst *call = createMapLookup(map_ident, key);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *lookup_success_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_success",
                                                        parent);
  BasicBlock *lookup_failure_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_failure",
                                                        parent);
  BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                               "lookup_merge",
                                               parent);

  // Make the verifier happy with a null check even though the value should
  // never be null for key 0.
  Value *condition = CreateICmpNE(
      CreateIntCast(call, getPtrTy(), true),
      ConstantExpr::getCast(Instruction::IntToPtr, getInt64(0), getPtrTy()),
      "map_lookup_cond");
  CreateCondBr(condition, lookup_success_block, lookup_failure_block);

  SetInsertPoint(lookup_success_block);

  CreateLifetimeEnd(key);

  // createMapLookup  returns an u8*
  auto *cast = CreatePtrToInt(call, getInt64Ty(), "cast");

  Value *prev_value = CREATE_ATOMIC_RMW(AtomicRMWInst::BinOp::Xchg,
                                        cast,
                                        getInt64(1),
                                        8,
                                        AtomicOrdering::SequentiallyConsistent);

  llvm::Function *set_parent = GetInsertBlock()->getParent();
  BasicBlock *value_is_set_block = BasicBlock::Create(module_.getContext(),
                                                      "value_is_set",
                                                      set_parent);
  Value *set_condition = CreateICmpEQ(prev_value,
                                      getInt64(0),
                                      "value_set_condition");
  CreateCondBr(set_condition, merge_block, value_is_set_block);

  SetInsertPoint(value_is_set_block);
  // The counter is set, we need to exit early from the probe.
  // Most of the time this will happen for the functions that can lead
  // to a crash e.g. "queued_spin_lock_slowpath" but it can also happen
  // for nested probes e.g. "page_fault_user" -> "print".
  CreateIncEventLossCounter(loc);
  CreateRet(getInt64(early_exit_ret));

  SetInsertPoint(lookup_failure_block);

  CreateDebugOutput(
      "Value for per-cpu map key 0 is null. This shouldn't happen.",
      std::vector<Value *>{},
      loc);
  CreateRet(getInt64(0));

  SetInsertPoint(merge_block);
}

void IRBuilderBPF::CreateUnSetRecursion(const Location &loc)
{
  const std::string map_ident = to_string(MapType::RecursionPrevention);

  AllocaInst *key = CreateAllocaBPF(getInt32Ty(), "lookup_key");
  CreateStore(getInt32(0), key);

  CallInst *call = createMapLookup(map_ident, key);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *lookup_success_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_success",
                                                        parent);
  BasicBlock *lookup_failure_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_failure",
                                                        parent);
  BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                               "lookup_merge",
                                               parent);

  // Make the verifier happy with a null check even though the value should
  // never be null for key 0.
  Value *condition = CreateICmpNE(
      CreateIntCast(call, getPtrTy(), true),
      ConstantExpr::getCast(Instruction::IntToPtr, getInt64(0), getPtrTy()),
      "map_lookup_cond");
  CreateCondBr(condition, lookup_success_block, lookup_failure_block);

  SetInsertPoint(lookup_success_block);

  CreateLifetimeEnd(key);

  // createMapLookup  returns an u8*
  auto *cast = CreatePtrToInt(call, getInt64Ty(), "cast");
  CreateStore(getInt64(0), cast);

  CreateBr(merge_block);

  SetInsertPoint(lookup_failure_block);

  CreateDebugOutput(
      "Value for per-cpu map key 0 is null. This shouldn't happen.",
      std::vector<Value *>{},
      loc);

  CreateBr(merge_block);

  SetInsertPoint(merge_block);
}

void IRBuilderBPF::CreateProbeRead(Value *dst,
                                   llvm::Value *size,
                                   Value *src,
                                   AddrSpace as,
                                   const Location &loc)
{
  assert(size && size->getType()->getIntegerBitWidth() <= 32);
  size = CreateIntCast(size, getInt32Ty(), false);

  // int bpf_probe_read(void *dst, int size, void *src)
  // Return: 0 on success or negative error

  auto read_fn = selectProbeReadHelper(as, false);

  FunctionType *proberead_func_type = FunctionType::get(
      getInt64Ty(), { dst->getType(), getInt32Ty(), src->getType() }, false);
  PointerType *proberead_func_ptr_type = PointerType::get(proberead_func_type,
                                                          0);
  Constant *proberead_func = ConstantExpr::getCast(Instruction::IntToPtr,
                                                   getInt64(read_fn),
                                                   proberead_func_ptr_type);
  CallInst *call = createCall(proberead_func_type,
                              proberead_func,
                              { dst, size, src },
                              probeReadHelperName(read_fn));
  CreateHelperErrorCond(call, read_fn, loc);
}

CallInst *IRBuilderBPF::CreateProbeReadStr(Value *dst,
                                           size_t size,
                                           Value *src,
                                           AddrSpace as,
                                           const Location &loc)
{
  return CreateProbeReadStr(dst, getInt32(size), src, as, loc);
}

CallInst *IRBuilderBPF::CreateProbeReadStr(Value *dst,
                                           llvm::Value *size,
                                           Value *src,
                                           AddrSpace as,
                                           const Location &loc)
{
  assert(size && size->getType()->isIntegerTy());
  if ([[maybe_unused]] auto *dst_alloca = dyn_cast<AllocaInst>(dst)) {
    assert(dst_alloca->getAllocatedType()->isArrayTy() &&
           dst_alloca->getAllocatedType()->getArrayElementType() ==
               getInt8Ty());
  }

  auto *size_i32 = size;
  if (size_i32->getType()->getScalarSizeInBits() != 32)
    size_i32 = CreateIntCast(size_i32, getInt32Ty(), false);

  auto read_fn = selectProbeReadHelper(as, true);
  // int bpf_probe_read_str(void *dst, int size, const void *unsafe_ptr)
  FunctionType *probereadstr_func_type = FunctionType::get(
      getInt64Ty(), { dst->getType(), getInt32Ty(), src->getType() }, false);
  PointerType *probereadstr_func_ptr_type = PointerType::get(
      probereadstr_func_type, 0);
  Constant *probereadstr_callee = ConstantExpr::getCast(
      Instruction::IntToPtr, getInt64(read_fn), probereadstr_func_ptr_type);
  CallInst *call = createCall(probereadstr_func_type,
                              probereadstr_callee,
                              { dst, size_i32, src },
                              probeReadHelperName(read_fn));
  CreateHelperErrorCond(call, read_fn, loc);
  return call;
}

Value *IRBuilderBPF::CreateUSDTReadArgument(Value *ctx,
                                            struct bcc_usdt_argument *argument,
                                            Builtin &builtin,
                                            AddrSpace as,
                                            const Location &loc)
{
  // Argument size must be 1, 2, 4, or 8. See
  // https://sourceware.org/systemtap/wiki/UserSpaceProbeImplementation
  int abs_size = std::abs(argument->size);
  assert(abs_size == 1 || abs_size == 2 || abs_size == 4 || abs_size == 8);
  if (argument->valid & BCC_USDT_ARGUMENT_DEREF_IDENT)
    builtin.addError() << "deref ident is not handled yet ["
                       << argument->deref_ident << "]";
  // USDT arguments can be any valid gas (GNU asm) operand.
  // BCC normalises these into the bcc_usdt_argument and supports most
  // valid gas operands.
  //
  // This code handles the following argument types:
  // * A constant (ARGUMENT_CONSTANT)
  //
  // * The value of a register (ARGUMENT_BASE_REGISTER_NAME without
  // ARGUMENT_DEREF_OFFSET set).
  //
  // * The value at address: base_register + offset + (index_register * scale)
  // Where index_register and scale are optional.
  // Note: Offset is optional in the gas operand, however will be set as zero
  // if the register needs to be dereferenced.

  if (argument->valid & BCC_USDT_ARGUMENT_CONSTANT) {
    // Correctly sign extend and convert to a 64-bit int
    return CreateIntCast(getIntN(abs_size * 8, argument->constant),
                         getInt64Ty(),
                         argument->size < 0);
  }

  if (argument->valid & BCC_USDT_ARGUMENT_INDEX_REGISTER_NAME &&
      !(argument->valid & BCC_USDT_ARGUMENT_BASE_REGISTER_NAME)) {
    // Invalid combination??
    builtin.addError() << "index register set without base register;"
                       << " this case is not yet handled";
  }
  Value *result = nullptr;
  if (argument->valid & BCC_USDT_ARGUMENT_BASE_REGISTER_NAME) {
    auto offset = arch::Host::register_to_pt_regs_offset(
        argument->base_register_name);
    if (!offset) {
      builtin.addError() << "offset for register "
                         << argument->base_register_name << " not known";
      return getInt32(0);
    }

    // bpftrace's args are internally represented as 64 bit integers. However,
    // the underlying argument (of the target program) may be less than 64
    // bits. So we must be careful to zero out unused bits.
    Value *reg = CreateSafeGEP(
        getInt8Ty(), ctx, getInt64(offset.value()), "load_register");
    AllocaInst *dst = CreateAllocaBPF(builtin.builtin_type, builtin.ident);
    Value *index_offset = nullptr;
    if (argument->valid & BCC_USDT_ARGUMENT_INDEX_REGISTER_NAME) {
      auto ioffset = arch::Host::register_to_pt_regs_offset(
          argument->index_register_name);
      if (!ioffset) {
        builtin.addError() << "offset for register "
                           << argument->index_register_name << " not known";
        return getInt32(0);
      }
      index_offset = CreateSafeGEP(
          getInt8Ty(), ctx, getInt64(ioffset.value()), "load_register");
      index_offset = CreateLoad(getInt64Ty(), index_offset);
      if (argument->valid & BCC_USDT_ARGUMENT_SCALE) {
        index_offset = CreateMul(index_offset, getInt64(argument->scale));
      }
    }
    if (argument->valid & BCC_USDT_ARGUMENT_DEREF_OFFSET) {
      Value *ptr = CreateAdd(CreateLoad(getInt64Ty(), reg),
                             getInt64(argument->deref_offset));
      if (index_offset) {
        ptr = CreateAdd(ptr, index_offset);
      }
      CreateProbeRead(dst, getInt32(abs_size), ptr, as, loc);
      result = CreateLoad(getIntNTy(abs_size * 8), dst);
    } else {
      result = CreateLoad(getIntNTy(abs_size * 8), reg);
    }
    // Sign extend and convert to a bpftools standard 64-bit integer type
    result = CreateIntCast(result, getInt64Ty(), argument->size < 0);
    CreateLifetimeEnd(dst);
  }
  return result;
}

Value *IRBuilderBPF::CreateUSDTReadArgument(Value *ctx,
                                            AttachPoint *attach_point,
                                            int usdt_location_index,
                                            int arg_num,
                                            Builtin &builtin,
                                            std::optional<pid_t> pid,
                                            AddrSpace as,
                                            const Location &loc)
{
  struct bcc_usdt_argument argument;

  void *usdt;

  if (pid.has_value()) {
    if (!attach_point->target.empty()) {
      auto real_path = std::filesystem::absolute(attach_point->target).string();
      usdt = bcc_usdt_new_frompid(*pid, real_path.c_str());
    } else {
      usdt = bcc_usdt_new_frompid(*pid, nullptr);
    };
  } else {
    usdt = bcc_usdt_new_frompath(attach_point->target.c_str());
  }

  if (usdt == nullptr) {
    throw util::FatalUserException(
        "failed to initialize usdt context for probe " + attach_point->target);
  }

  std::string ns = attach_point->usdt.provider;
  std::string func = attach_point->usdt.name;

  if (bcc_usdt_get_argument(usdt,
                            ns.c_str(),
                            func.c_str(),
                            usdt_location_index,
                            arg_num,
                            &argument) != 0) {
    throw util::FatalUserException("couldn't get argument " +
                                   std::to_string(arg_num) + " for " +
                                   attach_point->target + ":" +
                                   attach_point->ns + ":" + attach_point->func);
  }

  Value *result = CreateUSDTReadArgument(ctx, &argument, builtin, as, loc);

  bcc_usdt_close(usdt);
  return result;
}

Value *IRBuilderBPF::CreateStrncmp(Value *str1,
                                   Value *str2,
                                   uint64_t n,
                                   bool inverse)
{
  // This function compares each character of the two string. It returns true
  // if all are equal and false if any are different.
  //
  //  strcmp(String val1, String val2)
  //  {
  //     for (size_t i = 0; i < n; i++)
  //     {
  //
  //       if (val1[i] != val2[i])
  //       {
  //         return false;
  //       }
  //       if (val1[i] == NULL)
  //       {
  //         return true;
  //       }
  //     }
  //
  //     return true;
  //  }

  llvm::Function *parent = GetInsertBlock()->getParent();
  AllocaInst *store = CreateAllocaBPF(getInt1Ty(), "strcmp.result");
  BasicBlock *str_ne = BasicBlock::Create(module_.getContext(),
                                          "strcmp.false",
                                          parent);
  BasicBlock *done = BasicBlock::Create(module_.getContext(),
                                        "strcmp.done",
                                        parent);

  CreateStore(getInt1(!inverse), store);

  Value *null_byte = getInt8(0);
  for (size_t i = 0; i < n; i++) {
    BasicBlock *char_eq = BasicBlock::Create(module_.getContext(),
                                             "strcmp.loop",
                                             parent);
    BasicBlock *loop_null_check = BasicBlock::Create(module_.getContext(),
                                                     "strcmp.loop_null_cmp",
                                                     parent);

    Value *l;
    auto *ptr_l = CreateGEP(getInt8Ty(), str1, { getInt32(i) });
    l = CreateLoad(getInt8Ty(), ptr_l);

    Value *r;
    auto *ptr_r = CreateGEP(getInt8Ty(), str2, { getInt32(i) });
    r = CreateLoad(getInt8Ty(), ptr_r);

    Value *cmp = CreateICmpNE(l, r, "strcmp.cmp");
    CreateCondBr(cmp, str_ne, loop_null_check);

    SetInsertPoint(loop_null_check);

    Value *cmp_null = CreateICmpEQ(l, null_byte, "strcmp.cmp_null");
    CreateCondBr(cmp_null, done, char_eq);

    SetInsertPoint(char_eq);
  }

  CreateBr(done);
  SetInsertPoint(done);
  CreateStore(getInt1(inverse), store);

  CreateBr(str_ne);
  SetInsertPoint(str_ne);

  // store is a pointer to bool (i1 *)
  Value *result = CreateLoad(getInt1Ty(), store);
  CreateLifetimeEnd(store);
  result = CreateIntCast(result, getInt64Ty(), false);

  return result;
}

Value *IRBuilderBPF::CreateStrcontains(Value *haystack,
                                       uint64_t haystack_sz,
                                       Value *needle,
                                       uint64_t needle_sz)
{
  // This function compares whether the string haystack contains the string
  // needle. It returns true if needle is contained by haystack, false if not
  // contained.
  //
  // clang-format off
  //
  // bool strcontains(char *haystack, size_t haystack_sz, char *needle, size_t needle_sz) {
  //   // Explicit check needed for haystack="", needle="" case
  //   if (needle[0] == '\0')
  //     return true;
  //
  //   for (u64 i = 0; i < haystack_sz && haystack[i] != '\0'; i++) {
  //     u64 j;
  //     for (j = 0; j < needle_sz; j++) {
  //       if (needle[j] == '\0')
  //         return true;
  //
  //       if ((i+j) >= haystack_sz || haystack[i+j] != needle[j])
  //         break;
  //     }
  //   }
  //
  //   return false;
  // }
  //
  // clang-format on

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *empty_check = BasicBlock::Create(module_.getContext(),
                                               "strcontains.empty.check",
                                               parent);
  BasicBlock *outer_cond = BasicBlock::Create(module_.getContext(),
                                              "strcontains.outer.cond",
                                              parent);
  BasicBlock *outer_cond_cmp = BasicBlock::Create(module_.getContext(),
                                                  "strcontains.outer.cond.cmp",
                                                  parent);
  BasicBlock *outer_body = BasicBlock::Create(module_.getContext(),
                                              "strcontains.outer.body",
                                              parent);
  BasicBlock *inner_cond = BasicBlock::Create(module_.getContext(),
                                              "strcontains.inner.cond",
                                              parent);
  BasicBlock *inner_body = BasicBlock::Create(module_.getContext(),
                                              "strcontains.inner.body",
                                              parent);
  BasicBlock *inner_notnull = BasicBlock::Create(module_.getContext(),
                                                 "strcontains.inner.notnull",
                                                 parent);
  BasicBlock *inner_cmp = BasicBlock::Create(module_.getContext(),
                                             "strcontains.inner.cmp",
                                             parent);
  BasicBlock *inner_incr = BasicBlock::Create(module_.getContext(),
                                              "strcontains.inner.incr",
                                              parent);
  BasicBlock *outer_incr = BasicBlock::Create(module_.getContext(),
                                              "strcontains.outer.incr",
                                              parent);
  BasicBlock *done_true = BasicBlock::Create(module_.getContext(),
                                             "strcontains.true",
                                             parent);
  BasicBlock *done_false = BasicBlock::Create(module_.getContext(),
                                              "strcontains.false",
                                              parent);
  BasicBlock *done = BasicBlock::Create(module_.getContext(),
                                        "strcontains.done",
                                        parent);

  AllocaInst *i = CreateAllocaBPF(getInt64Ty(), "strcontains.i");
  CreateStore(getInt64(0), i);
  AllocaInst *j = CreateAllocaBPF(getInt64Ty(), "strcontains.j");
  Value *null_byte = getInt8(0);

  CreateBr(empty_check);
  SetInsertPoint(empty_check);
  Value *needle_first = CreateLoad(
      getInt8Ty(), CreateGEP(getInt8Ty(), needle, { getInt64(0) }));
  Value *needle_first_null = CreateICmpEQ(needle_first, null_byte);
  CreateCondBr(needle_first_null, done_true, outer_cond);

  SetInsertPoint(outer_cond);
  Value *i_val = CreateLoad(getInt64Ty(), i);
  Value *haystack_inbounds = CreateICmpULT(i_val, getInt64(haystack_sz));
  CreateCondBr(haystack_inbounds, outer_cond_cmp, done_false);

  SetInsertPoint(outer_cond_cmp);
  Value *haystack_char = CreateLoad(
      getInt8Ty(), CreateGEP(getInt8Ty(), haystack, { i_val }));
  Value *haystack_valid = CreateICmpNE(haystack_char, null_byte);
  CreateCondBr(haystack_valid, outer_body, done_false);

  SetInsertPoint(outer_body);
  CreateStore(getInt64(0), j);
  CreateBr(inner_cond);

  SetInsertPoint(inner_cond);
  Value *j_val = CreateLoad(getInt64Ty(), j);
  Value *needle_inbounds = CreateICmpULT(j_val, getInt64(needle_sz));
  CreateCondBr(needle_inbounds, inner_body, outer_incr);

  SetInsertPoint(inner_body);
  Value *needle_char = CreateLoad(getInt8Ty(),
                                  CreateGEP(getInt8Ty(), needle, { j_val }));
  Value *needle_null = CreateICmpEQ(needle_char, null_byte);
  CreateCondBr(needle_null, done_true, inner_notnull);

  SetInsertPoint(inner_notnull);
  Value *haystack_cmp_idx = CreateAdd(i_val, j_val);
  Value *haystack_cmp_outbounds = CreateICmpUGE(haystack_cmp_idx,
                                                getInt64(haystack_sz));
  CreateCondBr(haystack_cmp_outbounds, outer_incr, inner_cmp);

  // Inner conditional matching haystack char with needle char
  SetInsertPoint(inner_cmp);
  Value *haystack_cmp_char = CreateLoad(
      getInt8Ty(), CreateGEP(getInt8Ty(), haystack, { haystack_cmp_idx }));
  Value *haystack_cmp_needle = CreateICmpNE(haystack_cmp_char, needle_char);
  CreateCondBr(haystack_cmp_needle, outer_incr, inner_incr);

  SetInsertPoint(inner_incr);
  CreateStore(CreateAdd(CreateLoad(getInt64Ty(), j), getInt64(1)), j);
  CreateBr(inner_cond);

  SetInsertPoint(outer_incr);
  CreateStore(CreateAdd(CreateLoad(getInt64Ty(), i), getInt64(1)), i);
  CreateBr(outer_cond);

  SetInsertPoint(done_false);
  CreateBr(done);

  SetInsertPoint(done_true);
  CreateBr(done);

  SetInsertPoint(done);
  auto *phi = CreatePHI(getInt1Ty(), 2, "result");
  phi->addIncoming(getInt1(false), done_false);
  phi->addIncoming(getInt1(true), done_true);
  CreateLifetimeEnd(j);
  CreateLifetimeEnd(i);

  return CreateIntCast(phi, getInt64Ty(), false);
}

CallInst *IRBuilderBPF::CreateGetNs(TimestampMode ts, const Location &loc)
{
  // Random default value to silence compiler warning
  libbpf::bpf_func_id fn = libbpf::BPF_FUNC_ktime_get_ns;
  switch (ts) {
    case TimestampMode::monotonic:
      fn = libbpf::BPF_FUNC_ktime_get_ns;
      break;
    case TimestampMode::boot:
      fn = bpftrace_.feature_->has_helper_ktime_get_boot_ns()
               ? libbpf::BPF_FUNC_ktime_get_boot_ns
               : libbpf::BPF_FUNC_ktime_get_ns;
      break;
    case TimestampMode::tai:
      fn = libbpf::BPF_FUNC_ktime_get_tai_ns;
      break;
    case TimestampMode::sw_tai:
      LOG(BUG) << "Invalid timestamp mode: "
               << std::to_string(
                      static_cast<std::underlying_type_t<TimestampMode>>(ts));
  }

  // u64 ktime_get_*ns()
  // Return: current ktime
  FunctionType *gettime_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(fn, gettime_func_type, {}, false, "get_ns", loc);
}

CallInst *IRBuilderBPF::CreateJiffies64(const Location &loc)
{
  // u64 bpf_jiffies64()
  // Return: jiffies (BITS_PER_LONG == 64) or jiffies_64 (otherwise)
  FunctionType *jiffies64_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_jiffies64,
                          jiffies64_func_type,
                          {},
                          false,
                          "jiffies64",
                          loc);
}

Value *IRBuilderBPF::CreateIntegerArrayCmp(Value *val1,
                                           Value *val2,
                                           const SizedType &val1_type,
                                           const SizedType &val2_type,
                                           const bool inverse,
                                           const Location &loc,
                                           MDNode *metadata)
{
  // This function compares each character of the two arrays.  It returns true
  // if all are equal and false if any are different.
  //
  //  cmp([]char val1, []char val2)
  //  {
  //    for (size_t i = 0; i < n; i++)
  //    {
  //      if (val1[i] != val2[i])
  //      {
  //        return false;
  //      }
  //    }
  //    return true;
  //  }

  auto elem_type = *val1_type.GetElementTy();
  const size_t num = val1_type.GetNumElements();

  Value *val1_elem_i, *val2_elem_i, *cmp;
  AllocaInst *v1 = CreateAllocaBPF(elem_type, "v1");
  AllocaInst *v2 = CreateAllocaBPF(elem_type, "v2");
  AllocaInst *store = CreateAllocaBPF(getInt1Ty(), "arraycmp.result");
  CreateStore(getInt1(inverse), store);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *while_cond = BasicBlock::Create(module_.getContext(),
                                              "while_cond",
                                              parent);
  BasicBlock *while_body = BasicBlock::Create(module_.getContext(),
                                              "while_body",
                                              parent);
  BasicBlock *arr_ne = BasicBlock::Create(module_.getContext(),
                                          "arraycmp.false",
                                          parent);
  BasicBlock *done = BasicBlock::Create(module_.getContext(),
                                        "arraycmp.done",
                                        parent);

  Value *ptr_val1 = CreateIntToPtr(val1, getPtrTy());
  Value *ptr_val2 = CreateIntToPtr(val2, getPtrTy());
  AllocaInst *i = CreateAllocaBPF(getInt32Ty(), "i");
  AllocaInst *n = CreateAllocaBPF(getInt32Ty(), "n");
  CreateStore(getInt32(0), i);
  CreateStore(getInt32(num), n);
  CreateBr(while_cond);

  SetInsertPoint(while_cond);
  auto *cond = CreateICmpSLT(CreateLoad(getInt32Ty(), i),
                             CreateLoad(getInt32Ty(), n),
                             "size_check");
  Instruction *loop_hdr = CreateCondBr(cond, while_body, done);
  loop_hdr->setMetadata(LLVMContext::MD_loop, metadata);

  SetInsertPoint(while_body);
  BasicBlock *arr_eq = BasicBlock::Create(module_.getContext(),
                                          "arraycmp.loop",
                                          parent);
  auto *ptr_val1_elem_i = CreateGEP(GetType(val1_type),
                                    ptr_val1,
                                    { getInt32(0),
                                      CreateLoad(getInt32Ty(), i) });
  if (inBpfMemory(val1_type)) {
    val1_elem_i = CreateLoad(GetType(elem_type), ptr_val1_elem_i);
  } else {
    CreateProbeRead(v1,
                    getInt32(elem_type.GetSize()),
                    ptr_val1_elem_i,
                    val1_type.GetAS(),
                    loc);
    val1_elem_i = CreateLoad(GetType(elem_type), v1);
  }

  auto *ptr_val2_elem_i = CreateGEP(GetType(val2_type),
                                    ptr_val2,
                                    { getInt32(0),
                                      CreateLoad(getInt32Ty(), i) });
  if (inBpfMemory(val2_type)) {
    val2_elem_i = CreateLoad(GetType(elem_type), ptr_val2_elem_i);
  } else {
    CreateProbeRead(v2,
                    getInt32(elem_type.GetSize()),
                    ptr_val2_elem_i,
                    val2_type.GetAS(),
                    loc);
    val2_elem_i = CreateLoad(GetType(elem_type), v2);
  }

  cmp = CreateICmpNE(val1_elem_i, val2_elem_i, "arraycmp.cmp");
  CreateCondBr(cmp, arr_ne, arr_eq);

  SetInsertPoint(arr_eq);
  CreateStore(CreateAdd(CreateLoad(getInt32Ty(), i), getInt32(1)), i);
  CreateBr(while_cond);

  SetInsertPoint(arr_ne);
  CreateStore(getInt1(!inverse), store);
  CreateBr(done);

  SetInsertPoint(done);
  Value *result = CreateLoad(getInt1Ty(), store);
  CreateLifetimeEnd(store);
  CreateLifetimeEnd(v1);
  CreateLifetimeEnd(v2);
  result = CreateIntCast(result, getInt64Ty(), false);
  return result;
}

CallInst *IRBuilderBPF::CreateGetPidTgid(const Location &loc)
{
  // u64 bpf_get_current_pid_tgid(void)
  // Return: current->tgid << 32 | current->pid
  FunctionType *getpidtgid_func_type = FunctionType::get(getInt64Ty(), false);
  auto *res = CreateHelperCall(libbpf::BPF_FUNC_get_current_pid_tgid,
                               getpidtgid_func_type,
                               {},
                               true,
                               "get_pid_tgid",
                               loc);
  return res;
}

void IRBuilderBPF::CreateGetNsPidTgid(Value *dev,
                                      Value *ino,
                                      AllocaInst *ret,
                                      const Location &loc)
{
  // long bpf_get_ns_current_pid_tgid(
  //   u64 dev, u64 ino, struct bpf_pidns_info *nsdata, u32 size)
  // Return: 0 on success
  const auto &layout = module_.getDataLayout();
  auto struct_size = layout.getTypeAllocSize(BpfPidnsInfoType());

  FunctionType *getnspidtgid_func_type = FunctionType::get(getInt64Ty(),
                                                           {
                                                               getInt64Ty(),
                                                               getInt64Ty(),
                                                               getPtrTy(),
                                                               getInt32Ty(),
                                                           },
                                                           false);
  CallInst *call = CreateHelperCall(libbpf::BPF_FUNC_get_ns_current_pid_tgid,
                                    getnspidtgid_func_type,
                                    { dev, ino, ret, getInt32(struct_size) },
                                    false,
                                    "get_ns_pid_tgid",
                                    loc);
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_get_ns_current_pid_tgid, loc);
}

llvm::Type *IRBuilderBPF::BpfPidnsInfoType()
{
  return GetStructType("bpf_pidns_info",
                       {
                           getInt32Ty(),
                           getInt32Ty(),
                       },
                       false);
}

CallInst *IRBuilderBPF::CreateGetCurrentCgroupId(const Location &loc)
{
  // u64 bpf_get_current_cgroup_id(void)
  // Return: 64-bit cgroup-v2 id
  FunctionType *getcgroupid_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_current_cgroup_id,
                          getcgroupid_func_type,
                          {},
                          true,
                          "get_cgroup_id",
                          loc);
}

CallInst *IRBuilderBPF::CreateGetUidGid(const Location &loc)
{
  // u64 bpf_get_current_uid_gid(void)
  // Return: current_gid << 32 | current_uid
  FunctionType *getuidgid_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_current_uid_gid,
                          getuidgid_func_type,
                          {},
                          true,
                          "get_uid_gid",
                          loc);
}

CallInst *IRBuilderBPF::CreateGetNumaId(const Location &loc)
{
  // long bpf_get_numa_node_id(void)
  // Return: NUMA Node ID
  FunctionType *numaid_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_numa_node_id,
                          numaid_func_type,
                          {},
                          true,
                          "get_numa_id",
                          loc);
}

CallInst *IRBuilderBPF::CreateGetCpuId(const Location &loc)
{
  // u32 bpf_raw_smp_processor_id(void)
  // Return: SMP processor ID
  FunctionType *getcpuid_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_smp_processor_id,
                          getcpuid_func_type,
                          {},
                          true,
                          "get_cpu_id",
                          loc);
}

CallInst *IRBuilderBPF::CreateGetCurrentTask(const Location &loc)
{
  // u64 bpf_get_current_task(void)
  // Return: current task_struct
  FunctionType *getcurtask_func_type = FunctionType::get(getInt64Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_current_task,
                          getcurtask_func_type,
                          {},
                          true,
                          "get_cur_task",
                          loc);
}

CallInst *IRBuilderBPF::CreateGetRandom(const Location &loc)
{
  // u32 bpf_get_prandom_u32(void)
  // Return: random
  FunctionType *getrandom_func_type = FunctionType::get(getInt32Ty(), false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_prandom_u32,
                          getrandom_func_type,
                          {},
                          false,
                          "get_random",
                          loc);
}

CallInst *IRBuilderBPF::CreateGetStack(Value *ctx,
                                       bool ustack,
                                       Value *buf,
                                       StackType stack_type,
                                       const Location &loc)
{
  int flags = 0;
  if (ustack)
    flags |= (1 << 8);
  Value *flags_val = getInt64(flags);
  Value *stack_size = getInt32(stack_type.limit * sizeof(uint64_t));

  // long bpf_get_stack(void *ctx, void *buf, u32 size, u64 flags)
  // Return: The non-negative copied *buf* length equal to or less than
  // *size* on success, or a negative error in case of failure.
  FunctionType *getstack_func_type = FunctionType::get(
      getInt64Ty(),
      { getPtrTy(), getPtrTy(), getInt32Ty(), getInt64Ty() },
      false);
  CallInst *call = CreateHelperCall(libbpf::BPF_FUNC_get_stack,
                                    getstack_func_type,
                                    { ctx, buf, stack_size, flags_val },
                                    false,
                                    "get_stack",
                                    loc);
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_get_stack, loc);
  return call;
}

CallInst *IRBuilderBPF::CreateGetFuncIp(Value *ctx, const Location &loc)
{
  // u64 bpf_get_func_ip(void *ctx)
  // Return:
  // 		Address of the traced function for kprobe.
  //		0 for kprobes placed within the function (not at the entry).
  //		Address of the probe for uprobe and return uprobe.
  FunctionType *getfuncip_func_type = FunctionType::get(getInt64Ty(),
                                                        { getPtrTy() },
                                                        false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_func_ip,
                          getfuncip_func_type,
                          { ctx },
                          true,
                          "get_func_ip",
                          loc);
}

CallInst *IRBuilderBPF::CreatePerCpuPtr(Value *var,
                                        Value *cpu,
                                        const Location &loc)
{
  // void *bpf_per_cpu_ptr(const void *percpu_ptr, u32 cpu)
  // Return:
  //    A pointer pointing to the kernel percpu variable on
  //    cpu, or NULL, if cpu is invalid.
  FunctionType *percpuptr_func_type = FunctionType::get(
      getPtrTy(), { getPtrTy(), getInt64Ty() }, false);
  return CreateHelperCall(libbpf::BPF_FUNC_per_cpu_ptr,
                          percpuptr_func_type,
                          { var, cpu },
                          true,
                          "per_cpu_ptr",
                          loc);
}

CallInst *IRBuilderBPF::CreateThisCpuPtr(Value *var, const Location &loc)
{
  // void *bpf_per_cpu_ptr(const void *percpu_ptr)
  // Return:
  //    A pointer pointing to the kernel percpu variable on
  //    this cpu. May never be NULL.
  FunctionType *percpuptr_func_type = FunctionType::get(getPtrTy(),
                                                        { getPtrTy() },
                                                        false);
  return CreateHelperCall(libbpf::BPF_FUNC_this_cpu_ptr,
                          percpuptr_func_type,
                          { var },
                          true,
                          "this_cpu_ptr",
                          loc);
}

void IRBuilderBPF::CreateGetCurrentComm(AllocaInst *buf,
                                        size_t size,
                                        const Location &loc)
{
  assert(buf->getAllocatedType()->isArrayTy() &&
         buf->getAllocatedType()->getArrayNumElements() >= size &&
         buf->getAllocatedType()->getArrayElementType() == getInt8Ty());

  // long bpf_get_current_comm(char *buf, int size_of_buf)
  // Return: 0 on success or negative error
  FunctionType *getcomm_func_type = FunctionType::get(
      getInt64Ty(), { buf->getType(), getInt64Ty() }, false);
  CallInst *call = CreateHelperCall(libbpf::BPF_FUNC_get_current_comm,
                                    getcomm_func_type,
                                    { buf, getInt64(size) },
                                    false,
                                    "get_comm",
                                    loc);
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_get_current_comm, loc);
}

CallInst *IRBuilderBPF::CreateGetSocketCookie(Value *var, const Location &loc)
{
  // u64 bpf_get_socket_cookie(struct sock *sk)
  // Return:
  //    A 8-byte long unique number or 0 if *sk* is NULL.
  FunctionType *get_socket_cookie_func_type = FunctionType::get(
      getInt64Ty(), { var->getType() }, false);
  return CreateHelperCall(libbpf::BPF_FUNC_get_socket_cookie,
                          get_socket_cookie_func_type,
                          { var },
                          true,
                          "get_socket_cookie",
                          loc);
}

void IRBuilderBPF::CreateOutput(Value *data, size_t size, const Location &loc)
{
  assert(data && data->getType()->isPointerTy());
  CreateRingbufOutput(data, size, loc);
}

void IRBuilderBPF::CreateRingbufOutput(Value *data,
                                       size_t size,
                                       const Location &loc)
{
  Value *map_ptr = GetMapVar(to_string(MapType::Ringbuf));

  // long bpf_ringbuf_output(void *ringbuf, void *data, u64 size, u64 flags)
  FunctionType *ringbuf_output_func_type = FunctionType::get(
      getInt64Ty(),
      { map_ptr->getType(), data->getType(), getInt64Ty(), getInt64Ty() },
      false);

  Value *ret = CreateHelperCall(libbpf::BPF_FUNC_ringbuf_output,
                                ringbuf_output_func_type,
                                { map_ptr, data, getInt64(size), getInt64(0) },
                                false,
                                "ringbuf_output",
                                loc);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *loss_block = BasicBlock::Create(module_.getContext(),
                                              "event_loss_counter",
                                              parent);
  BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                               "counter_merge",
                                               parent);
  Value *condition = CreateICmpSLT(ret, getInt64(0), "ringbuf_loss");
  CreateCondBr(condition, loss_block, merge_block);

  SetInsertPoint(loss_block);
  CreateIncEventLossCounter(loc);
  CreateBr(merge_block);

  SetInsertPoint(merge_block);
}

void IRBuilderBPF::CreateIncEventLossCounter(const Location &loc)
{
  auto *value = createScratchBuffer(bpftrace::globalvars::EVENT_LOSS_COUNTER,
                                    loc,
                                    0);
  CreateStore(CreateAdd(CreateLoad(getInt64Ty(), value), getInt64(1)), value);
}

void IRBuilderBPF::CreatePerCpuMapElemInit(Map &map,
                                           Value *key,
                                           Value *val,
                                           const Location &loc)
{
  AllocaInst *initValue = CreateAllocaBPF(val->getType(), "initial_value");
  CreateStore(val, initValue);
  CreateMapUpdateElem(map.ident, key, initValue, loc, BPF_ANY);
  CreateLifetimeEnd(initValue);
}

void IRBuilderBPF::CreatePerCpuMapElemAdd(Map &map,
                                          Value *key,
                                          Value *val,
                                          const Location &loc)
{
  CallInst *call = CreateMapLookup(map, key);

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *lookup_success_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_success",
                                                        parent);
  BasicBlock *lookup_failure_block = BasicBlock::Create(module_.getContext(),
                                                        "lookup_failure",
                                                        parent);
  BasicBlock *lookup_merge_block = BasicBlock::Create(module_.getContext(),
                                                      "lookup_merge",
                                                      parent);

  AllocaInst *value = CreateAllocaBPF(map.value_type, "lookup_elem_val");
  Value *condition = CreateICmpNE(CreateIntCast(call, getPtrTy(), true),
                                  GetNull(),
                                  "map_lookup_cond");
  CreateCondBr(condition, lookup_success_block, lookup_failure_block);

  SetInsertPoint(lookup_success_block);

  // createMapLookup  returns an u8*
  auto *cast = CreatePtrToInt(call, value->getType(), "cast");
  CreateStore(CreateAdd(CreateLoad(value->getAllocatedType(), cast), val),
              cast);

  CreateBr(lookup_merge_block);

  SetInsertPoint(lookup_failure_block);

  CreatePerCpuMapElemInit(map, key, val, loc);

  CreateBr(lookup_merge_block);
  SetInsertPoint(lookup_merge_block);
  CreateLifetimeEnd(value);
}

void IRBuilderBPF::CreateDebugOutput(std::string fmt_str,
                                     const std::vector<Value *> &values,
                                     const Location &loc)
{
  if (!bpftrace_.debug_output_)
    return;
  fmt_str = "[BPFTRACE_DEBUG_OUTPUT] " + fmt_str;
  Constant *const_str = ConstantDataArray::getString(module_.getContext(),
                                                     fmt_str,
                                                     true);
  AllocaInst *fmt = CreateAllocaBPF(
      ArrayType::get(getInt8Ty(), fmt_str.length() + 1), "fmt_str");
  CreateMemsetBPF(fmt, getInt8(0), fmt_str.length() + 1);
  CreateStore(const_str, fmt);
  CreateTracePrintk(fmt, getInt32(fmt_str.length() + 1), values, loc);
}

void IRBuilderBPF::CreateTracePrintk(Value *fmt_ptr,
                                     Value *fmt_size,
                                     const std::vector<Value *> &values,
                                     const Location &loc)
{
  std::vector<Value *> args = { fmt_ptr, fmt_size };
  for (auto *val : values) {
    args.push_back(val);
  }

  // long bpf_trace_printk(const char *fmt, u32 fmt_size, ...)
  FunctionType *traceprintk_func_type = FunctionType::get(
      getInt64Ty(), { getPtrTy(), getInt32Ty() }, true);

  CreateHelperCall(libbpf::BPF_FUNC_trace_printk,
                   traceprintk_func_type,
                   args,
                   false,
                   "trace_printk",
                   loc);
}

void IRBuilderBPF::CreateSignal(Value *sig, const Location &loc)
{
  // long bpf_send_signal(u32 sig)
  // Return: 0 or error
  FunctionType *signal_func_type = FunctionType::get(getInt64Ty(),
                                                     { getInt32Ty() },
                                                     false);
  PointerType *signal_func_ptr_type = PointerType::get(signal_func_type, 0);
  Constant *signal_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_send_signal),
      signal_func_ptr_type);
  CallInst *call = createCall(signal_func_type, signal_func, { sig }, "signal");
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_send_signal, loc);
}

void IRBuilderBPF::CreateOverrideReturn(Value *ctx, Value *rc)
{
  // long bpf_override_return(struct pt_regs *regs, u64 rc)
  // Return: 0
  FunctionType *override_func_type = FunctionType::get(
      getInt64Ty(), { getPtrTy(), getInt64Ty() }, false);
  PointerType *override_func_ptr_type = PointerType::get(override_func_type, 0);
  Constant *override_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_override_return),
      override_func_ptr_type);
  createCall(override_func_type, override_func, { ctx, rc }, "override");
}

CallInst *IRBuilderBPF::CreateSkbOutput(Value *skb,
                                        Value *len,
                                        AllocaInst *data,
                                        size_t size)
{
  Value *flags, *map_ptr, *size_val;

  map_ptr = GetMapVar(to_string(MapType::PerfEvent));

  flags = len;
  flags = CreateShl(flags, 32);
  flags = CreateOr(flags, getInt64(BPF_F_CURRENT_CPU));

  size_val = getInt64(size);

  // long bpf_skb_output(void *skb, struct bpf_map *map, u64 flags,
  //                     void *data, u64 size)
  FunctionType *skb_output_func_type = FunctionType::get(getInt64Ty(),
                                                         { skb->getType(),
                                                           map_ptr->getType(),
                                                           getInt64Ty(),
                                                           data->getType(),
                                                           getInt64Ty() },
                                                         false);

  PointerType *skb_output_func_ptr_type = PointerType::get(skb_output_func_type,
                                                           0);
  Constant *skb_output_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_skb_output),
      skb_output_func_ptr_type);
  CallInst *call = createCall(skb_output_func_type,
                              skb_output_func,
                              { skb, map_ptr, flags, data, size_val },
                              "skb_output");
  return call;
}

Value *IRBuilderBPF::CreateKFuncArg(Value *ctx,
                                    SizedType &type,
                                    std::string &name)
{
  assert(type.IsIntTy() || type.IsPtrTy());
  Value *expr = CreateLoad(
      GetType(type),
      CreateSafeGEP(getInt64Ty(), ctx, getInt64(type.funcarg_idx)),
      name);

  // LLVM 7.0 <= does not have CreateLoad(*Ty, *Ptr, isVolatile, Name),
  // so call setVolatile() manually
  dyn_cast<LoadInst>(expr)->setVolatile(true);
  return expr;
}

Value *IRBuilderBPF::CreateRawTracepointArg(Value *ctx,
                                            const std::string &builtin)
{
  // argX
  int offset = atoi(builtin.substr(3).c_str());
  llvm::Type *type = getInt64Ty();

  // All arguments are coerced into Int64.
  Value *expr = CreateLoad(type,
                           CreateSafeGEP(type, ctx, getInt64(offset)),
                           builtin);

  return expr;
}

Value *IRBuilderBPF::CreateUprobeArgsRecord(Value *ctx,
                                            const SizedType &args_type)
{
  assert(args_type.IsRecordTy());

  auto *args_t = UprobeArgsType(args_type);
  AllocaInst *result = CreateAllocaBPF(args_t, "args");

  for (auto &arg : args_type.GetFields()) {
    assert(arg.type.is_funcarg);
    Value *arg_read = CreateRegisterRead(
        ctx, "arg" + std::to_string(arg.type.funcarg_idx));
    if (arg.type.GetSize() != 8)
      arg_read = CreateTrunc(arg_read, GetType(arg.type));
    CreateStore(arg_read,
                CreateGEP(args_t,
                          result,
                          { getInt64(0), getInt32(arg.type.funcarg_idx) }));
  }
  return result;
}

llvm::Type *IRBuilderBPF::UprobeArgsType(const SizedType &args_type)
{
  auto type_name = args_type.GetName();
  type_name.erase(0, strlen("struct "));

  std::vector<llvm::Type *> arg_types;
  for (auto &arg : args_type.GetFields())
    arg_types.push_back(GetType(arg.type));
  return GetStructType(type_name, arg_types, false);
}

Value *IRBuilderBPF::CreateRegisterRead(Value *ctx, const std::string &builtin)
{
  std::optional<std::string> reg;
  if (builtin == "retval") {
    reg = arch::Host::return_value();
  } else if (builtin == "func") {
    reg = arch::Host::pc_value();
  } else if (builtin.starts_with("arg")) {
    size_t n = static_cast<size_t>(atoi(builtin.substr(3).c_str()));
    const auto &arguments = arch::Host::arguments();
    if (n < arguments.size()) {
      reg = arguments[n];
    }
  }
  if (!reg.has_value()) {
    LOG(BUG) << "unknown builtin: " << builtin;
    __builtin_unreachable();
  }

  auto offset = arch::Host::register_to_pt_regs_offset(reg.value());
  if (!offset.has_value()) {
    LOG(BUG) << "invalid register `" << reg.value()
             << " for builtin: " << builtin;
    __builtin_unreachable();
  }

  return CreateRegisterRead(ctx, offset.value(), builtin);
}

Value *IRBuilderBPF::CreateRegisterRead(Value *ctx,
                                        size_t offset,
                                        const std::string &name)
{
  // Bitwidth of register values in struct pt_regs is the same as the kernel
  // pointer width on all supported architectures.
  //
  // FIXME(#3873): Not clear if this applies as a general rule, best to allow
  // these to be resolved via field names and BTF directly in the future.
  llvm::Type *registerTy = getKernelPointerStorageTy();

  Value *result = CreateLoad(registerTy,
                             CreateSafeGEP(getInt8Ty(), ctx, getInt64(offset)),
                             name);
  // LLVM 7.0 <= does not have CreateLoad(*Ty, *Ptr, isVolatile, Name),
  // so call setVolatile() manually
  dyn_cast<LoadInst>(result)->setVolatile(true);
  // Caller expects an int64, so add a cast if the register size is different.
  if (result->getType()->getIntegerBitWidth() != 64) {
    result = CreateIntCast(result, getInt64Ty(), false);
  }
  return result;
}

static bool return_zero_if_err(libbpf::bpf_func_id func_id)
{
  switch (func_id) {
    // When these function fails, bpftrace stores zero as a result.
    // A user script can check an error by seeing the value.
    // Therefore error checks of these functions are omitted if
    // helper_check_level == 1.
    case libbpf::BPF_FUNC_probe_read:
    case libbpf::BPF_FUNC_probe_read_str:
    case libbpf::BPF_FUNC_probe_read_kernel:
    case libbpf::BPF_FUNC_probe_read_kernel_str:
    case libbpf::BPF_FUNC_probe_read_user:
    case libbpf::BPF_FUNC_probe_read_user_str:
    case libbpf::BPF_FUNC_map_lookup_elem:
      return true;
    default:
      return false;
  }
  return false;
}

void IRBuilderBPF::CreateRuntimeError(RuntimeErrorId rte_id,
                                      const Location &loc)
{
  CreateRuntimeError(
      rte_id, getInt64(0), static_cast<libbpf::bpf_func_id>(-1), loc);
}

void IRBuilderBPF::CreateRuntimeError(RuntimeErrorId rte_id,
                                      Value *return_value,
                                      libbpf::bpf_func_id func_id,
                                      const Location &loc)
{
  if (rte_id == RuntimeErrorId::HELPER_ERROR) {
    assert(return_value && return_value->getType() == getInt32Ty());

    if (bpftrace_.helper_check_level_ == 0 ||
        (bpftrace_.helper_check_level_ == 1 && return_zero_if_err(func_id)))
      return;
  }

  int error_id = async_ids_.runtime_error();
  bpftrace_.resources.runtime_error_info.try_emplace(
      error_id, RuntimeErrorInfo(rte_id, func_id, loc));
  auto elements = AsyncEvent::RuntimeError().asLLVMType(*this);
  StructType *runtime_error_struct = GetStructType("runtime_error_t",
                                                   elements,
                                                   true);
  AllocaInst *buf = CreateAllocaBPF(runtime_error_struct, "runtime_error_t");
  CreateStore(
      GetIntSameSize(static_cast<int64_t>(
                         async_action::AsyncAction::runtime_error),
                     elements.at(0)),
      CreateGEP(runtime_error_struct, buf, { getInt64(0), getInt32(0) }));
  CreateStore(
      GetIntSameSize(error_id, elements.at(1)),
      CreateGEP(runtime_error_struct, buf, { getInt64(0), getInt32(1) }));
  CreateStore(
      return_value,
      CreateGEP(runtime_error_struct, buf, { getInt64(0), getInt32(2) }));

  const auto &layout = module_.getDataLayout();
  auto struct_size = layout.getTypeAllocSize(runtime_error_struct);
  CreateOutput(buf, struct_size, loc);
  CreateLifetimeEnd(buf);
}

void IRBuilderBPF::CreateHelperErrorCond(Value *return_value,
                                         libbpf::bpf_func_id func_id,
                                         const Location &loc,
                                         bool suppress_error)
{
  if (bpftrace_.helper_check_level_ == 0 || suppress_error ||
      (bpftrace_.helper_check_level_ == 1 && return_zero_if_err(func_id)))
    return;

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *helper_failure_block = BasicBlock::Create(module_.getContext(),
                                                        "helper_failure",
                                                        parent);
  BasicBlock *helper_merge_block = BasicBlock::Create(module_.getContext(),
                                                      "helper_merge",
                                                      parent);
  // A return type of most helper functions are Int64Ty but they actually
  // return int and we need to use Int32Ty value to check if a return
  // value is negative. TODO: use Int32Ty as a return type
  auto *ret = CreateIntCast(return_value, getInt32Ty(), true);
  Value *condition = CreateICmpSGE(ret, Constant::getNullValue(ret->getType()));
  CreateCondBr(condition, helper_merge_block, helper_failure_block);
  SetInsertPoint(helper_failure_block);
  CreateRuntimeError(RuntimeErrorId::HELPER_ERROR, ret, func_id, loc);
  CreateBr(helper_merge_block);
  SetInsertPoint(helper_merge_block);
}

void IRBuilderBPF::CreatePath(Value *buf,
                              Value *path,
                              Value *sz,
                              const Location &loc)
{
  // int bpf_d_path(struct path *path, char *buf, u32 sz)
  // Return: 0 or error
  FunctionType *d_path_func_type = FunctionType::get(
      getInt64Ty(), { getPtrTy(), buf->getType(), getInt32Ty() }, false);
  CallInst *call = CreateHelperCall(libbpf::bpf_func_id::BPF_FUNC_d_path,
                                    d_path_func_type,
                                    { path, buf, sz },
                                    false,
                                    "d_path",
                                    loc);
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_d_path, loc);
}

void IRBuilderBPF::CreateSeqPrintf(Value *ctx,
                                   Value *fmt,
                                   Value *fmt_size,
                                   Value *data,
                                   Value *data_len,
                                   const Location &loc)
{
  // long bpf_seq_printf(struct seq_file *m, const char *fmt, __u32 fmt_size,
  //                     const void *data, __u32 data_len)
  // Return: 0 or error
  FunctionType *seq_printf_func_type = FunctionType::get(
      getInt64Ty(),
      { getInt64Ty(), getPtrTy(), getInt32Ty(), getPtrTy(), getInt32Ty() },
      false);
  PointerType *seq_printf_func_ptr_type = PointerType::get(seq_printf_func_type,
                                                           0);
  Constant *seq_printf_func = ConstantExpr::getCast(
      Instruction::IntToPtr,
      getInt64(libbpf::BPF_FUNC_seq_printf),
      seq_printf_func_ptr_type);

  LoadInst *meta = CreateLoad(getPtrTy(),
                              CreateSafeGEP(getInt64Ty(), ctx, getInt64(0)),
                              "meta");
  meta->setVolatile(true);

  Value *seq = CreateLoad(getInt64Ty(),
                          CreateGEP(getInt64Ty(), meta, getInt64(0)),
                          "seq");

  CallInst *call = createCall(seq_printf_func_type,
                              seq_printf_func,
                              { seq, fmt, fmt_size, data, data_len },
                              "seq_printf");
  CreateHelperErrorCond(call, libbpf::BPF_FUNC_seq_printf, loc);
}

StoreInst *IRBuilderBPF::createAlignedStore(Value *val,
                                            Value *ptr,
                                            unsigned int align)
{
  return CreateAlignedStore(val, ptr, MaybeAlign(align));
}

void IRBuilderBPF::CreateProbeRead(Value *dst,
                                   const SizedType &type,
                                   Value *src,
                                   const Location &loc,
                                   std::optional<AddrSpace> addrSpace)
{
  AddrSpace as = addrSpace ? addrSpace.value() : type.GetAS();

  if (!type.IsPtrTy()) {
    CreateProbeRead(dst, getInt32(type.GetSize()), src, as, loc);
    return;
  }

  // Pointers are internally always represented as 64-bit integers, matching the
  // BPF register size (BPF is a 64-bit ISA). This helps to avoid BPF codegen
  // issues such as truncating PTR_TO_STACK registers using shift operations,
  // which is disallowed (see https://github.com/bpftrace/bpftrace/pull/2361).
  // However, when reading pointers from kernel or user memory, we need to use
  // the appropriate size for the target system.
  const size_t ptr_size = getPointerStorageTy(as)->getIntegerBitWidth() / 8;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  // TODO: support 32-bit big-endian systems
  assert(ptr_size == type.GetSize());
#endif

  if (ptr_size != type.GetSize())
    CreateMemsetBPF(dst, getInt8(0), type.GetSize());

  CreateProbeRead(dst, getInt32(ptr_size), src, as, loc);
}

llvm::Value *IRBuilderBPF::CreateDatastructElemLoad(
    const SizedType &type,
    llvm::Value *ptr,
    bool isVolatile,
    std::optional<AddrSpace> addrSpace)
{
  AddrSpace as = addrSpace ? addrSpace.value() : type.GetAS();
  llvm::Type *ptr_storage_ty = getPointerStorageTy(as);

  if (!type.IsPtrTy() || ptr_storage_ty == getInt64Ty())
    return CreateLoad(GetType(type), ptr, isVolatile);

  assert(GetType(type) == getInt64Ty());

  // Pointer size for the given address space doesn't match the BPF-side
  // representation. Use ptr_storage_ty as the load type and cast the result
  // back to int64.
  llvm::Value *expr = CreateLoad(ptr_storage_ty, ptr, isVolatile);

  return CreateIntCast(expr, getInt64Ty(), false);
}

llvm::Value *IRBuilderBPF::CreatePtrOffset(const SizedType &type,
                                           llvm::Value *index,
                                           AddrSpace as)
{
  size_t elem_size = type.IsPtrTy()
                         ? getPointerStorageTy(as)->getIntegerBitWidth() / 8
                         : type.GetSize();

  return CreateMul(index, getInt64(elem_size));
}

llvm::Value *IRBuilderBPF::CreateSafeGEP(llvm::Type *ty,
                                         llvm::Value *ptr,
                                         llvm::ArrayRef<Value *> offsets,
                                         const llvm::Twine &name)
{
  if (!ptr->getType()->isPointerTy()) {
    assert(ptr->getType()->isIntegerTy());
    ptr = CreateIntToPtr(ptr, getPtrTy());
  }

#if LLVM_VERSION_MAJOR >= 18
  if (!preserve_static_offset_) {
#if LLVM_VERSION_MAJOR >= 20
    preserve_static_offset_ = llvm::Intrinsic::getOrInsertDeclaration(
        &module_, llvm::Intrinsic::preserve_static_offset);
#else
    preserve_static_offset_ = llvm::Intrinsic::getDeclaration(
        &module_, llvm::Intrinsic::preserve_static_offset);
#endif
  }
  ptr = CreateCall(preserve_static_offset_, ptr);
#endif

  // Create the GEP itself; on newer versions of LLVM this coerces the pointer
  // value into a pointer to the given type, and older versions have guaranteed
  // a suitable cast above (first from integer, then to the right pointer).
  return CreateGEP(ty, ptr, offsets, name);
}

llvm::Type *IRBuilderBPF::getPointerStorageTy(AddrSpace as)
{
  switch (as) {
    case AddrSpace::user:
      return getUserPointerStorageTy();
    default:
      return getKernelPointerStorageTy();
  }
}

llvm::Type *IRBuilderBPF::getKernelPointerStorageTy()
{
  static int ptr_width = arch::Host::kernel_ptr_width();
  return getIntNTy(ptr_width);
}

llvm::Type *IRBuilderBPF::getUserPointerStorageTy()
{
  // TODO: we don't currently have an easy way of determining the pointer size
  // of the uprobed process, so assume it's the same as the kernel's for now.
  return getKernelPointerStorageTy();
}

void IRBuilderBPF::CreateMinMax(Value *val,
                                Value *val_ptr,
                                Value *is_set_ptr,
                                bool max,
                                bool is_signed)
{
  llvm::Function *parent = GetInsertBlock()->getParent();

  BasicBlock *is_set_block = BasicBlock::Create(module_.getContext(),
                                                "is_set",
                                                parent);
  BasicBlock *min_max_block = BasicBlock::Create(module_.getContext(),
                                                 "min_max",
                                                 parent);
  BasicBlock *merge_block = BasicBlock::Create(module_.getContext(),
                                               "merge",
                                               parent);

  Value *curr = CreateLoad(getInt64Ty(), val_ptr);
  Value *is_set_condition = CreateICmpEQ(CreateLoad(getInt64Ty(), is_set_ptr),
                                         getInt64(1),
                                         "is_set_cond");

  CreateCondBr(is_set_condition, is_set_block, min_max_block);

  SetInsertPoint(is_set_block);

  Value *min_max_condition;

  if (max) {
    min_max_condition = is_signed ? CreateICmpSGE(val, curr)
                                  : CreateICmpUGE(val, curr);
  } else {
    min_max_condition = is_signed ? CreateICmpSGE(curr, val)
                                  : CreateICmpUGE(curr, val);
  }

  CreateCondBr(min_max_condition, min_max_block, merge_block);

  SetInsertPoint(min_max_block);

  CreateStore(val, val_ptr);

  CreateStore(getInt64(1), is_set_ptr);

  CreateBr(merge_block);

  SetInsertPoint(merge_block);
}

llvm::Value *IRBuilderBPF::CreateCheckedBinop(Binop &binop,
                                              Value *lhs,
                                              Value *rhs)
{
  assert(binop.op == Operator::DIV || binop.op == Operator::MOD);
  // We need to do an explicit 0 check or else a Clang compiler optimization
  // will assume that the value can't ever be 0, as this is undefined behavior,
  // and remove a conditional null check for map values which will return 0 if
  // the map value is null (this happens in CreateMapLookupElem). This would be
  // fine but the BPF verifier will complain about the lack of a null check.
  // Issue: https://github.com/bpftrace/bpftrace/issues/4379
  // From Google's AI: "LLVM, like other optimizing compilers, is allowed to
  // make assumptions based on the absence of undefined behavior. If a program's
  // code, after optimization, would result in undefined behavior (like division
  // by zero by CreateURem), the compiler is free to make transformations that
  // assume such a situation will never occur."
  AllocaInst *op_result = CreateAllocaBPF(getInt64Ty(), "op_result");

  llvm::Function *parent = GetInsertBlock()->getParent();
  BasicBlock *is_zero = BasicBlock::Create(module_.getContext(),
                                           "is_zero",
                                           parent);
  BasicBlock *not_zero = BasicBlock::Create(module_.getContext(),
                                            "not_zero",
                                            parent);
  BasicBlock *zero_merge = BasicBlock::Create(module_.getContext(),
                                              "zero_merge",
                                              parent);

  Value *cond = CreateICmpEQ(rhs, getInt64(0), "zero_cond");

  CreateCondBr(cond, is_zero, not_zero);
  SetInsertPoint(is_zero);
  CreateStore(getInt64(1), op_result);
  CreateRuntimeError(RuntimeErrorId::DIVIDE_BY_ZERO, binop.loc);
  CreateBr(zero_merge);

  SetInsertPoint(not_zero);
  if (binop.op == Operator::MOD) {
    CreateStore(CreateURem(lhs, rhs), op_result);
  } else if (binop.op == Operator::DIV) {
    CreateStore(CreateUDiv(lhs, rhs), op_result);
  }

  CreateBr(zero_merge);

  SetInsertPoint(zero_merge);
  auto *result = CreateLoad(getInt64Ty(), op_result);
  CreateLifetimeEnd(op_result);
  return result;
}

} // namespace bpftrace::ast
