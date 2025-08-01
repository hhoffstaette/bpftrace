#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <ctime>

// Required for LLVM_VERSION_MAJOR.
#include <llvm/IR/GlobalValue.h>

#if LLVM_VERSION_MAJOR <= 16
#include <llvm-c/Transforms/IPO.h>
#endif
#include <llvm/ADT/FunctionExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/CodeGen/UnreachableBlockElim.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/Cloning.h>
#if LLVM_VERSION_MAJOR <= 16
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#endif
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>

#include "arch/arch.h"
#include "ast/ast.h"
#include "ast/async_event_types.h"
#include "ast/async_ids.h"
#include "ast/codegen_helper.h"
#include "ast/context.h"
#include "ast/dibuilderbpf.h"
#include "ast/irbuilderbpf.h"
#include "ast/location.h"
#include "ast/passes/clang_build.h"
#include "ast/passes/codegen_llvm.h"
#include "ast/passes/link.h"
#include "ast/passes/named_param.h"
#include "ast/passes/probe_expansion.h"
#include "ast/signal_bt.h"
#include "ast/visitor.h"
#include "async_action.h"
#include "bpfmap.h"
#include "bpftrace.h"
#include "codegen_resources.h"
#include "format_string.h"
#include "globalvars.h"
#include "kfuncs.h"
#include "log.h"
#include "map_info.h"
#include "required_resources.h"
#include "tracepoint_format_parser.h"
#include "types.h"
#include "usdt.h"
#include "util/bpf_names.h"
#include "util/cgroup.h"
#include "util/cpus.h"
#include "util/exceptions.h"

namespace bpftrace::ast {

using namespace llvm;

static constexpr char LLVMTargetTriple[] = "bpf";
static constexpr auto LICENSE = "LICENSE";

static auto getTargetMachine()
{
  static auto *target = []() {
    std::string error_str;
    const auto *target = llvm::TargetRegistry::lookupTarget(LLVMTargetTriple,
                                                            error_str);
    if (!target) {
      throw util::FatalUserException(
          "Could not find bpf llvm target, does your llvm support it?");
    }
    auto *machine = target->createTargetMachine(LLVMTargetTriple,
                                                "generic",
                                                "",
                                                TargetOptions(),
                                                std::optional<Reloc::Model>());
#if LLVM_VERSION_MAJOR >= 18
    machine->setOptLevel(llvm::CodeGenOptLevel::Aggressive);
#else
    machine->setOptLevel(llvm::CodeGenOpt::Aggressive);
#endif
    return machine;
  }();
  return target;
}

static bool shouldForceInitPidNs(const ExpressionList &args)
{
  return args.size() == 1 && args.at(0).as<Identifier>()->ident == "init";
}

namespace {

class InternalError : public ErrorInfo<InternalError> {
public:
  InternalError(std::string msg) : msg_(std::move(msg)) {};
  static char ID;
  void log(llvm::raw_ostream &OS) const override
  {
    OS << msg_;
  }

private:
  std::string msg_;
};

char InternalError::ID;

struct VariableLLVM {
  llvm::Value *value;
  llvm::Type *type;
};

// ScopedExpr ties an SSA value to a "delete" function, that typically will end
// the lifetime of some needed storage. You must explicitly construct a
// ScopedExpr from either:
// * A value only, with no associated function when out of scope.
// * A value and associated function to run when out of scope.
// * A value and another ScopedExpr, whose lifetime will be preserved until
//   this value is out of scope.
class ScopedExpr {
public:
  // Neither a value nor a deletion method.
  explicit ScopedExpr() = default;

  // Value only.
  explicit ScopedExpr(Value *value) : value_(value)
  {
  }

  // Value with an explicit deletion method.
  explicit ScopedExpr(Value *value, llvm::unique_function<void()> &&deleter)
      : value_(value), deleter_(std::move(deleter))
  {
  }

  // Value with another ScopedExpr whose lifetime should be bound.
  explicit ScopedExpr(Value *value, ScopedExpr &&other) : value_(value)
  {
    deleter_.swap(other.deleter_);
  }

  ScopedExpr(ScopedExpr &&other) : value_(other.value_)
  {
    deleter_.swap(other.deleter_);
  }

  ScopedExpr &operator=(ScopedExpr &&other)
  {
    value_ = other.value_;
    deleter_.swap(other.deleter_);
    return *this;
  }

  ScopedExpr(const ScopedExpr &other) = delete;
  ScopedExpr &operator=(const ScopedExpr &other) = delete;

  ~ScopedExpr()
  {
    if (deleter_) {
      deleter_.value()();
      deleter_.reset();
    }
  }

  Value *value()
  {
    return value_;
  }

  // May be used to disable the deletion method, essentially leaking some
  // memory within the frame. The use of this function should be generally
  // considered a bug, as it will make dealing with larger functions and
  // multiple scopes more problematic over time.
  void disarm()
  {
    deleter_.reset();
  }

private:
  Value *value_ = nullptr;
  std::optional<llvm::unique_function<void()>> deleter_;
};

class CodegenLLVM : public Visitor<CodegenLLVM, ScopedExpr> {
public:
  explicit CodegenLLVM(ASTContext &ast,
                       BPFtrace &bpftrace,
                       CDefinitions &c_definitions,
                       NamedParamDefaults &named_param_defaults,
                       LLVMContext &llvm_ctx,
                       USDTHelper &usdt_helper,
                       ExpansionResult &expansions);

  using Visitor<CodegenLLVM, ScopedExpr>::visit;
  ScopedExpr visit(Integer &integer);
  ScopedExpr visit(NegativeInteger &integer);
  ScopedExpr visit(Boolean &boolean);
  ScopedExpr visit(String &string);
  ScopedExpr visit(Identifier &identifier);
  ScopedExpr visit(Builtin &builtin);
  ScopedExpr visit(Call &call);
  ScopedExpr visit(Sizeof &szof);
  ScopedExpr visit(Offsetof &offof);
  ScopedExpr visit(Map &map);
  ScopedExpr visit(Variable &var);
  ScopedExpr visit(Binop &binop);
  ScopedExpr visit(Unop &unop);
  ScopedExpr visit(Ternary &ternary);
  ScopedExpr visit(FieldAccess &acc);
  ScopedExpr visit(ArrayAccess &arr);
  ScopedExpr visit(TupleAccess &acc);
  ScopedExpr visit(MapAccess &acc);
  ScopedExpr visit(Cast &cast);
  ScopedExpr visit(Tuple &tuple);
  ScopedExpr visit(ExprStatement &expr);
  ScopedExpr visit(AssignMapStatement &assignment);
  ScopedExpr visit(AssignVarStatement &assignment);
  ScopedExpr visit(VarDeclStatement &decl);
  ScopedExpr visit(If &if_node);
  ScopedExpr visit(Unroll &unroll);
  ScopedExpr visit(While &while_block);
  ScopedExpr visit(For &f, Map &map);
  ScopedExpr visit(For &f, Range &range);
  ScopedExpr visit(For &f);
  ScopedExpr visit(Jump &jump);
  ScopedExpr visit(Predicate &pred);
  ScopedExpr visit(Probe &probe);
  ScopedExpr visit(Subprog &subprog);
  ScopedExpr visit(Program &program);
  ScopedExpr visit(Block &block);
  ScopedExpr visit(BlockExpr &block_expr);

  // compile is the primary entrypoint; it will return the generated LLVMModule.
  // Only one call to `compile` is permitted per instantiation.
  std::unique_ptr<llvm::Module> compile();

private:
  int getNextIndexForProbe();
  ScopedExpr createLogicalAnd(Binop &binop);
  ScopedExpr createLogicalOr(Binop &binop);

  void createFormatStringCall(Call &call,
                              int id,
                              const std::vector<Field> &call_args,
                              const std::string &call_name,
                              async_action::AsyncAction async_action);

  void createPrintMapCall(Call &call);
  void createPrintNonMapCall(Call &call, int id);
  void createJoinCall(Call &call, int id);

  void createMapDefinition(const std::string &name,
                           libbpf::bpf_map_type map_type,
                           uint64_t max_entries,
                           const SizedType &key_type,
                           const SizedType &value_type);
  Value *createTuple(
      const SizedType &tuple_type,
      const std::vector<std::pair<llvm::Value *, Location>> &vals,
      const std::string &name,
      const Location &loc);
  void createTupleCopy(const SizedType &expr_type,
                       const SizedType &var_type,
                       Value *dst_val,
                       Value *src_val);

  void generate_maps(const RequiredResources &required_resources,
                     const CodegenResources &codegen_resources);
  void generate_global_vars(const RequiredResources &resources,
                            const ::bpftrace::Config &bpftrace_config);

  // Generate a probe for `current_attach_point_`
  //
  // If `dummy` is passed, then code is generated but immediately thrown away.
  // This is used to progress state (eg. asyncids) in this class instance for
  // invalid probes that still need to be visited.
  void generateProbe(Probe &probe,
                     const std::string &name,
                     FunctionType *func_type,
                     std::optional<int> usdt_location_index = std::nullopt,
                     bool dummy = false);

  // Generate a probe and register it to the BPFtrace class.
  void add_probe(AttachPoint &ap, Probe &probe, FunctionType *func_type);

  [[nodiscard]] ScopedExpr getMapKey(Map &map, Expression &key_expr);
  [[nodiscard]] ScopedExpr getMultiMapKey(
      Map &map,
      Expression &key_expr,
      const std::vector<Value *> &extra_keys,
      const Location &loc);

  void compareStructure(SizedType &our_type, llvm::Type *llvm_type);

  llvm::Function *createLog2Function();
  llvm::Function *createLinearFunction();
  MDNode *createLoopMetadata();

  std::pair<ScopedExpr, uint64_t> getString(Expression &expr);

  ScopedExpr binop_string(Binop &binop);
  ScopedExpr binop_integer_array(Binop &binop);
  ScopedExpr binop_buf(Binop &binop);
  ScopedExpr binop_int(Binop &binop);
  ScopedExpr binop_ptr(Binop &binop);

  ScopedExpr unop_int(Unop &unop);
  ScopedExpr unop_ptr(Unop &unop);

  ScopedExpr kstack_ustack(const std::string &ident,
                           StackType stack_type,
                           const Location &loc);

  int get_probe_id();

  // Create return instruction
  //
  // If null, return value will depend on current attach point (void in subprog)
  void createRet(Value *value = nullptr);
  int getReturnValueForProbe(ProbeType probe_type);

  // Every time we see a watchpoint that specifies a function + arg pair, we
  // generate a special "setup" probe that:
  //
  // * sends SIGSTOP to the tracee
  // * pulls out the function arg
  // * sends an asyncaction to the bpftrace runtime and specifies the arg value
  //   and which of the "real" probes to attach to the addr in the arg
  //
  // We need a separate "setup" probe per probe because we hard code the index
  // of the "real" probe the setup probe is to be replaced by.
  Result<> generateWatchpointSetupProbe(FunctionType *func_type,
                                        const std::string &expanded_probe_name,
                                        int arg_num,
                                        int index,
                                        const Location &loc);

  ScopedExpr readDatastructElemFromStack(ScopedExpr &&scoped_src,
                                         Value *index,
                                         const SizedType &data_type,
                                         const SizedType &elem_type);
  ScopedExpr readDatastructElemFromStack(ScopedExpr &&scoped_src,
                                         Value *index,
                                         llvm::Type *data_type,
                                         const SizedType &elem_type);
  ScopedExpr probereadDatastructElem(ScopedExpr &&scoped_src,
                                     Value *offset,
                                     const SizedType &data_type,
                                     const SizedType &elem_type,
                                     const Location &loc,
                                     const std::string &temp_name);

  ScopedExpr createIncDec(Unop &unop);

  llvm::Function *createMapLenCallback();

  // This function creates the context type and value for callbacks. Extra
  // fields for this context may be passed as the `extra_fields` argument.
  //
  // The context created here is suitable for use in `createForCallback`.
  std::pair<llvm::Type *, llvm::Value *> createForContext(
      const For &f,
      std::vector<llvm::Type *> &&extra_fields = {});

  // This creates and invokes a callback function that captures all required
  // arguments in the context type. Note that all callbacks require some
  // argument for the context; this is found by finding a parameter named `ctx`
  // in the `debug_arg`. This must be provided by the caller.
  //
  // The provided `decl` function is invoked to construct the scoped value for
  // the local context.
  llvm::Function *createForCallback(
      For &f,
      const std::string &name,
      ArrayRef<llvm::Type *> args,
      const Struct &debug_args,
      llvm::Type *ctx_t,
      std::function<llvm::Value *(llvm::Function *)> decl);

  llvm::Function *createForEachMapCallback(const For &f,
                                           const Map &map,
                                           llvm::Type *ctx_t);
  llvm::Function *createMurmurHash2Func();

  Value *createFmtString(int print_id);

  bool canAggPerCpuMapElems(libbpf::bpf_map_type map_type,
                            const SizedType &val_type);

  void maybeAllocVariable(const std::string &var_ident,
                          const SizedType &var_type,
                          const Location &loc);
  VariableLLVM *maybeGetVariable(const std::string &var_ident);
  VariableLLVM &getVariable(const std::string &var_ident);

  llvm::Function *DeclareKernelFunc(Kfunc kfunc, Node &call);

  CallInst *CreateKernelFuncCall(Kfunc kfunc,
                                 ArrayRef<Value *> args,
                                 const Twine &name,
                                 Node &call);

  GlobalVariable *DeclareKernelVar(const std::string &name);

  ASTContext &ast_;
  BPFtrace &bpftrace_;
  CDefinitions &c_definitions_;
  NamedParamDefaults &named_param_defaults_;
  LLVMContext &llvm_ctx_;
  USDTHelper &usdt_helper_;
  ExpansionResult &expansions_;
  std::unique_ptr<Module> module_;
  AsyncIds async_ids_;

  IRBuilderBPF b_;
  DIBuilderBPF debug_;

  const DataLayout &datalayout() const
  {
    return module_->getDataLayout();
  }

  Value *ctx_;
  llvm::DILocalScope *scope_ = nullptr;
  AttachPoint *current_attach_point_ = nullptr;
  std::string probefull_;
  uint64_t probe_count_ = 0;
  // Probes and attach points are indexed from 1, 0 means no index
  // (no index is used for probes whose attach points are indexed individually)
  int next_probe_index_ = 1;
  // Used if there are duplicate USDT entries
  int current_usdt_location_index_{ 0 };
  bool inside_subprog_ = false;

  std::vector<Node *> scope_stack_;
  std::unordered_map<Node *, std::map<std::string, VariableLLVM>> variables_;

  std::unordered_map<std::string, libbpf::bpf_map_type> map_types_;

  llvm::Function *linear_func_ = nullptr;
  llvm::Function *log2_func_ = nullptr;
  llvm::Function *murmur_hash_2_func_ = nullptr;
  llvm::Function *map_len_func_ = nullptr;
  MDNode *loop_metadata_ = nullptr;

  size_t getStructSize(StructType *s)
  {
    return module_->getDataLayout().getTypeAllocSize(s);
  }

  std::vector<std::tuple<BasicBlock *, BasicBlock *>> loops_;
  std::unordered_map<std::string, bool> probe_names_;
  std::unordered_map<std::string, llvm::Function *> extern_funcs_;

  llvm::Value *createGetNsSwTAI(const Location &loc);
};

} // namespace

CodegenLLVM::CodegenLLVM(ASTContext &ast,
                         BPFtrace &bpftrace,
                         CDefinitions &c_definitions,
                         NamedParamDefaults &named_param_defaults,
                         LLVMContext &llvm_ctx,
                         USDTHelper &usdt_helper,
                         ExpansionResult &expansions)
    : ast_(ast),
      bpftrace_(bpftrace),
      c_definitions_(c_definitions),
      named_param_defaults_(named_param_defaults),
      llvm_ctx_(llvm_ctx),
      usdt_helper_(usdt_helper),
      expansions_(expansions),
      module_(std::make_unique<Module>("bpftrace", llvm_ctx)),

      b_(llvm_ctx, *module_, bpftrace, async_ids_),
      debug_(*module_)
{
  module_->setTargetTriple(LLVMTargetTriple);
  module_->setDataLayout(getTargetMachine()->createDataLayout());

  debug_.createCompileUnit(dwarf::DW_LANG_C,
                           debug_.file,
                           "bpftrace",
                           false,
                           "",
                           0,
                           StringRef(),
                           DICompileUnit::DebugEmissionKind::LineTablesOnly);
  module_->addModuleFlag(llvm::Module::Warning,
                         "Debug Info Version",
                         llvm::DEBUG_METADATA_VERSION);

  // The unwind table causes problems when linking via libbpf.
  module_->setUwtable(llvm::UWTableKind::None);

  // Set license of BPF programs.
  const std::string &license = bpftrace_.config_->license;
  auto license_size = license.size() + 1;
  auto *license_var = llvm::dyn_cast<GlobalVariable>(
      module_->getOrInsertGlobal(LICENSE,
                                 ArrayType::get(b_.getInt8Ty(), license_size)));
  license_var->setInitializer(
      ConstantDataArray::getString(module_->getContext(), license));
  license_var->setSection("license");
  license_var->addDebugInfo(
      debug_.createGlobalVariable(LICENSE, CreateString(license_size)));
}

ScopedExpr CodegenLLVM::visit(Integer &integer)
{
  return ScopedExpr(b_.getInt64(integer.value));
}

ScopedExpr CodegenLLVM::visit(NegativeInteger &integer)
{
  return ScopedExpr(b_.getInt64(integer.value));
}

ScopedExpr CodegenLLVM::visit(Boolean &boolean)
{
  return ScopedExpr(b_.getInt8(boolean.value ? 1 : 0));
}

ScopedExpr CodegenLLVM::visit(String &string)
{
  std::string s(string.value);
  auto *string_var = llvm::dyn_cast<GlobalVariable>(module_->getOrInsertGlobal(
      s, ArrayType::get(b_.getInt8Ty(), string.string_type.GetSize())));
  string_var->setInitializer(
      ConstantDataArray::getString(module_->getContext(), s));
  return ScopedExpr(string_var);
}

// NB: we do not resolve identifiers that are structs. That is because in
// bpftrace you cannot really instantiate a struct.
ScopedExpr CodegenLLVM::visit(Identifier &identifier)
{
  if (c_definitions_.enums.contains(identifier.ident)) {
    return ScopedExpr(
        b_.getInt64(std::get<0>(c_definitions_.enums[identifier.ident])));
  } else {
    LOG(BUG) << "unknown identifier \"" << identifier.ident << "\"";
    __builtin_unreachable();
  }
}

ScopedExpr CodegenLLVM::kstack_ustack(const std::string &ident,
                                      StackType stack_type,
                                      const Location &loc)
{
  if (!murmur_hash_2_func_)
    murmur_hash_2_func_ = createMurmurHash2Func();

  const bool is_ustack = ident == "ustack";
  const auto uint64_size = sizeof(uint64_t);

  StructType *stack_key_struct = b_.GetStackStructType(is_ustack);
  AllocaInst *stack_key = b_.CreateAllocaBPF(stack_key_struct, "stack_key");
  b_.CreateMemsetBPF(stack_key,
                     b_.getInt8(0),
                     datalayout().getTypeStoreSize(stack_key_struct));

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *stack_scratch_failure = BasicBlock::Create(
      module_->getContext(), "stack_scratch_failure", parent);
  BasicBlock *merge_block = BasicBlock::Create(module_->getContext(),
                                               "merge_block",
                                               parent);

  Value *stack_trace = b_.CreateGetStackScratchMap(stack_type,
                                                   stack_scratch_failure,
                                                   loc);
  b_.CreateMemsetBPF(stack_trace,
                     b_.getInt8(0),
                     uint64_size * stack_type.limit);

  BasicBlock *get_stack_success = BasicBlock::Create(module_->getContext(),
                                                     "get_stack_success",
                                                     parent);
  BasicBlock *get_stack_fail = BasicBlock::Create(module_->getContext(),
                                                  "get_stack_fail",
                                                  parent);

  Value *stack_size = b_.CreateGetStack(
      ctx_, is_ustack, stack_trace, stack_type, loc);
  Value *condition = b_.CreateICmpSGE(stack_size, b_.getInt64(0));
  b_.CreateCondBr(condition, get_stack_success, get_stack_fail);

  b_.SetInsertPoint(get_stack_fail);
  b_.CreateDebugOutput("Failed to get stack. Error: %d",
                       std::vector<Value *>{ stack_size },
                       loc);
  b_.CreateBr(merge_block);
  b_.SetInsertPoint(get_stack_success);

  Value *num_frames = b_.CreateUDiv(stack_size, b_.getInt64(uint64_size));
  b_.CreateStore(num_frames,
                 b_.CreateGEP(stack_key_struct,
                              stack_key,
                              { b_.getInt64(0), b_.getInt32(1) }));
  // A random seed (or using pid) is probably unnecessary in this situation
  // and might hurt storage as the same pids may have the same stack and
  // we don't need to store it twice
  Value *seed = b_.getInt64(1);

  // LLVM-12 produces code that fails the BPF verifier because it
  // can't determine the bounds of nr_stack_frames. The only thing that seems
  // to work is truncating the type, which is fine because 255 is long enough.
  Value *trunc_nr_stack_frames = b_.CreateTrunc(num_frames, b_.getInt8Ty());

  // Here we use the murmur2 hash function to create the stack ids because
  // bpf_get_stackid() is kind of broken by design and can suffer from hash
  // collisions.
  // More details here: https://github.com/bpftrace/bpftrace/issues/2962
  Value *murmur_hash_2 = b_.CreateCall(
      murmur_hash_2_func_,
      { stack_trace, trunc_nr_stack_frames, seed },
      "murmur_hash_2");

  b_.CreateStore(murmur_hash_2,
                 b_.CreateGEP(stack_key_struct,
                              stack_key,
                              { b_.getInt64(0), b_.getInt32(0) }));
  // Add the stack and id to the stack map
  b_.CreateMapUpdateElem(
      stack_type.name(), stack_key, stack_trace, loc, BPF_ANY);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(stack_scratch_failure);
  b_.CreateDebugOutput("Failed to get stack from scratch map.",
                       std::vector<Value *>{},
                       loc);
  b_.CreateBr(merge_block);
  b_.SetInsertPoint(merge_block);

  // ustack keys are special: see IRBuilderBPF::GetStackStructType()
  if (is_ustack) {
    // store pid
    b_.CreateStore(b_.CreateGetPid(loc, false),
                   b_.CreateGEP(stack_key_struct,
                                stack_key,
                                { b_.getInt64(0), b_.getInt32(2) }));
    // store probe id
    b_.CreateStore(b_.GetIntSameSize(get_probe_id(),
                                     stack_key_struct->getTypeAtIndex(3)),
                   b_.CreateGEP(stack_key_struct,
                                stack_key,
                                { b_.getInt64(0), b_.getInt32(3) }));
  }

  return ScopedExpr(stack_key);
}

int CodegenLLVM::get_probe_id()
{
  auto begin = bpftrace_.resources.probe_ids.begin();
  auto end = bpftrace_.resources.probe_ids.end();
  auto found = std::find(begin, end, probefull_);
  if (found == end) {
    bpftrace_.resources.probe_ids.push_back(probefull_);
  }
  return std::distance(begin, found);
}

ScopedExpr CodegenLLVM::visit(Builtin &builtin)
{
  if (builtin.ident == "nsecs") {
    return ScopedExpr(b_.CreateGetNs(TimestampMode::boot, builtin.loc));
  } else if (builtin.ident == "elapsed") {
    AllocaInst *key = b_.CreateAllocaBPF(b_.getInt64Ty(), "elapsed_key");
    b_.CreateStore(b_.getInt64(0), key);

    auto type = CreateUInt64();
    auto *start = b_.CreateMapLookupElem(
        to_string(MapType::Elapsed), key, type, builtin.loc);
    Value *ns_value = b_.CreateGetNs(TimestampMode::boot, builtin.loc);
    Value *ns_delta = b_.CreateSub(ns_value, start);
    // start won't be on stack, no need to LifeTimeEnd it
    b_.CreateLifetimeEnd(key);
    return ScopedExpr(ns_delta);
  } else if (builtin.ident == "kstack" || builtin.ident == "ustack") {
    return kstack_ustack(builtin.ident,
                         builtin.builtin_type.stack_type,
                         builtin.loc);
  } else if (builtin.ident == "pid") {
    return ScopedExpr(b_.CreateGetPid(builtin.loc, false));
  } else if (builtin.ident == "tid") {
    return ScopedExpr(b_.CreateGetTid(builtin.loc, false));
  } else if (builtin.ident == "cgroup") {
    return ScopedExpr(b_.CreateGetCurrentCgroupId(builtin.loc));
  } else if (builtin.ident == "uid" || builtin.ident == "gid" ||
             builtin.ident == "username") {
    Value *uidgid = b_.CreateGetUidGid(builtin.loc);
    if (builtin.ident == "uid" || builtin.ident == "username") {
      return ScopedExpr(b_.CreateAnd(uidgid, 0xffffffff));
    } else if (builtin.ident == "gid") {
      return ScopedExpr(b_.CreateLShr(uidgid, 32));
    }
    __builtin_unreachable();
  } else if (builtin.ident == "usermode") {
    if (arch::Host::Machine == arch::Machine::X86_64) {
      auto cs_offset = arch::Host::register_to_pt_regs_offset("cs");
      if (!cs_offset) {
        builtin.addError() << "No CS register?";
        return ScopedExpr(b_.getInt64(0));
      }
      Value *cs = b_.CreateRegisterRead(ctx_, cs_offset.value(), "reg_cs");
      Value *mask = b_.getInt64(0x3);
      Value *is_usermode = b_.CreateICmpEQ(b_.CreateAnd(cs, mask),
                                           b_.getInt64(3),
                                           "is_usermode");
      Value *expr = b_.CreateZExt(is_usermode,
                                  b_.GetType(builtin.builtin_type),
                                  "usermode_result");
      return ScopedExpr(expr);
    } else {
      // We lack an implementation.
      builtin.addError() << "not supported on architecture "
                         << arch::Host::Machine;
      return ScopedExpr(b_.getInt64(0));
    }
  } else if (builtin.ident == "numaid") {
    return ScopedExpr(b_.CreateGetNumaId(builtin.loc));
  } else if (builtin.ident == "cpu") {
    Value *cpu = b_.CreateGetCpuId(builtin.loc);
    return ScopedExpr(b_.CreateZExt(cpu, b_.getInt64Ty()));
  } else if (builtin.ident == "ncpus") {
    return ScopedExpr(b_.CreateLoad(b_.getInt64Ty(),
                                    module_->getGlobalVariable(std::string(
                                        bpftrace::globalvars::NUM_CPUS)),
                                    "num_cpu.cmp"));
  } else if (builtin.ident == "curtask") {
    return ScopedExpr(b_.CreateGetCurrentTask(builtin.loc));
  } else if (builtin.ident == "rand") {
    Value *random = b_.CreateGetRandom(builtin.loc);
    return ScopedExpr(b_.CreateZExt(random, b_.getInt64Ty()));
  } else if (builtin.ident == "comm") {
    AllocaInst *buf = b_.CreateAllocaBPF(builtin.builtin_type, "comm");
    // initializing memory needed for older kernels:
    b_.CreateMemsetBPF(buf, b_.getInt8(0), builtin.builtin_type.GetSize());
    b_.CreateGetCurrentComm(buf, builtin.builtin_type.GetSize(), builtin.loc);
    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  } else if (builtin.ident == "func") {
    // fentry/fexit probes do not have access to registers, so require use of
    // the get_func_ip helper to get the instruction pointer.
    //
    // For [ku]retprobes, the IP register will not be pointing to the function
    // we want to trace. It may point to a kernel trampoline, or it may point to
    // the caller of the traced function, as it fires after the "ret"
    // instruction has executed.
    //
    // The get_func_ip helper resolves these issues for us.
    //
    // But do not use the it for non-ret [ku]probes (which can be used with
    // offsets), as the helper will fail for probes placed within a function
    // (not at the entry).
    Value *value = nullptr;
    auto probe_type = probetype(current_attach_point_->provider);
    if (probe_type == ProbeType::fentry || probe_type == ProbeType::fexit ||
        probe_type == ProbeType::kretprobe ||
        probe_type == ProbeType::uretprobe) {
      value = b_.CreateGetFuncIp(ctx_, builtin.loc);
    } else {
      value = b_.CreateRegisterRead(ctx_, builtin.ident);
    }

    if (builtin.builtin_type.IsUsymTy()) {
      value = b_.CreateUSym(value, get_probe_id(), builtin.loc);
      return ScopedExpr(value,
                        [this, value]() { b_.CreateLifetimeEnd(value); });
    }
    return ScopedExpr(value);
  } else if (builtin.is_argx() || builtin.ident == "retval") {
    auto probe_type = probetype(current_attach_point_->provider);

    if (builtin.builtin_type.is_funcarg) {
      return ScopedExpr(
          b_.CreateKFuncArg(ctx_, builtin.builtin_type, builtin.ident));
    }

    if (builtin.ident.find("arg") != std::string::npos &&
        probe_type == ProbeType::usdt) {
      return ScopedExpr(
          b_.CreateUSDTReadArgument(ctx_,
                                    current_attach_point_,
                                    current_usdt_location_index_,
                                    atoi(builtin.ident.substr(3).c_str()),
                                    builtin,
                                    bpftrace_.pid(),
                                    AddrSpace::user,
                                    builtin.loc));
    }

    Value *value = nullptr;
    if (builtin.is_argx() && probe_type == ProbeType::rawtracepoint)
      value = b_.CreateRawTracepointArg(ctx_, builtin.ident);
    else
      value = b_.CreateRegisterRead(ctx_, builtin.ident);

    if (builtin.builtin_type.IsUsymTy()) {
      value = b_.CreateUSym(value, get_probe_id(), builtin.loc);
      return ScopedExpr(value,
                        [this, value]() { b_.CreateLifetimeEnd(value); });
    }
    return ScopedExpr(value);

  } else if (!builtin.ident.compare(0, 4, "sarg") &&
             builtin.ident.size() == 5 && builtin.ident.at(4) >= '0' &&
             builtin.ident.at(4) <= '9') {
    auto sp_offset = arch::Host::register_to_pt_regs_offset(
        arch::Host::sp_value());
    if (!sp_offset) {
      builtin.addError() << "no stack offset available";
      return ScopedExpr(b_.getInt64(0));
    }
    int arg_num = atoi(builtin.ident.substr(4).c_str());
    Value *sp = b_.CreateRegisterRead(ctx_, sp_offset.value(), "reg_sp");
    AllocaInst *dst = b_.CreateAllocaBPF(builtin.builtin_type, builtin.ident);

    // Pointer width is used when calculating the SP offset and the number of
    // bytes to read from stack for each argument. We pass a pointer SizedType
    // to CreateProbeRead to make sure it uses the correct read size while
    // keeping builtin.type an int64.
    size_t arg_width = b_.getPointerStorageTy(builtin.builtin_type.GetAS())
                           ->getIntegerBitWidth() /
                       8;
    SizedType arg_type = CreatePointer(CreateInt8(),
                                       builtin.builtin_type.GetAS());
    assert(builtin.builtin_type.GetSize() == arg_type.GetSize());

    Value *src = b_.CreateAdd(sp,
                              b_.getInt64((arg_num * arg_width) +
                                          arch::Host::argument_stack_offset()));
    b_.CreateProbeRead(dst, arg_type, src, builtin.loc);
    Value *expr = b_.CreateLoad(b_.GetType(builtin.builtin_type), dst);
    b_.CreateLifetimeEnd(dst);
    return ScopedExpr(expr);
  } else if (builtin.ident == "probe") {
    auto probe_str = probefull_;
    probe_str.resize(builtin.builtin_type.GetSize() - 1);
    auto *probe_var = llvm::dyn_cast<GlobalVariable>(module_->getOrInsertGlobal(
        probe_str,
        ArrayType::get(b_.getInt8Ty(), builtin.builtin_type.GetSize())));
    probe_var->setInitializer(
        ConstantDataArray::getString(module_->getContext(), probe_str));
    return ScopedExpr(probe_var);
  } else if (builtin.ident == "args" &&
             probetype(current_attach_point_->provider) == ProbeType::uprobe) {
    // uprobe args record is built on stack
    return ScopedExpr(b_.CreateUprobeArgsRecord(ctx_, builtin.builtin_type));
  } else if (builtin.ident == "args" || builtin.ident == "ctx") {
    // ctx is undocumented builtin: for debugging.
    return ScopedExpr(ctx_);
  } else if (builtin.ident == "cpid") {
    pid_t cpid = bpftrace_.child_->pid();
    if (cpid < 1) {
      LOG(BUG) << "Invalid cpid: " << cpid;
    }
    return ScopedExpr(b_.getInt64(cpid));
  } else if (builtin.ident == "jiffies") {
    return ScopedExpr(b_.CreateJiffies64(builtin.loc));
  } else if (builtin.ident == "__session_is_return") {
    return ScopedExpr(CreateKernelFuncCall(
        Kfunc::bpf_session_is_return, {}, "is_return", builtin));
  } else {
    LOG(BUG) << "unknown builtin \"" << builtin.ident << "\"";
    __builtin_unreachable();
  }
}

ScopedExpr CodegenLLVM::visit(Call &call)
{
  if (call.func == "count") {
    Map &map = *call.vargs.at(0).as<Map>();
    auto scoped_key = getMapKey(map, call.vargs.at(1));
    b_.CreatePerCpuMapElemAdd(
        map, scoped_key.value(), b_.getInt64(1), call.loc);
    return ScopedExpr();

  } else if (call.func == "sum") {
    Map &map = *call.vargs.at(0).as<Map>();
    ScopedExpr scoped_key = getMapKey(map, call.vargs.at(1));
    ScopedExpr scoped_expr = visit(call.vargs.at(2));

    // promote int to 64-bit
    Value *cast = b_.CreateIntCast(scoped_expr.value(),
                                   b_.getInt64Ty(),
                                   call.vargs.front().type().IsSigned());
    b_.CreatePerCpuMapElemAdd(map, scoped_key.value(), cast, call.loc);
    return ScopedExpr();

  } else if (call.func == "max" || call.func == "min") {
    bool is_max = call.func == "max";
    Map &map = *call.vargs.at(0).as<Map>();
    ScopedExpr scoped_key = getMapKey(map, call.vargs.at(1));

    CallInst *lookup = b_.CreateMapLookup(map, scoped_key.value());
    ScopedExpr scoped_expr = visit(call.vargs.at(2));
    // promote int to 64-bit
    Value *expr = b_.CreateIntCast(scoped_expr.value(),
                                   b_.getInt64Ty(),
                                   call.vargs.front().type().IsSigned());

    llvm::Type *mm_struct_ty = b_.GetMapValueType(map.value_type);

    llvm::Function *parent = b_.GetInsertBlock()->getParent();
    BasicBlock *lookup_success_block = BasicBlock::Create(module_->getContext(),
                                                          "lookup_success",
                                                          parent);
    BasicBlock *lookup_failure_block = BasicBlock::Create(module_->getContext(),
                                                          "lookup_failure",
                                                          parent);
    BasicBlock *lookup_merge_block = BasicBlock::Create(module_->getContext(),
                                                        "lookup_merge",
                                                        parent);

    Value *lookup_condition = b_.CreateICmpNE(
        b_.CreateIntCast(lookup, b_.getPtrTy(), true),
        b_.GetNull(),
        "lookup_cond");
    b_.CreateCondBr(lookup_condition,
                    lookup_success_block,
                    lookup_failure_block);

    b_.SetInsertPoint(lookup_success_block);

    b_.CreateMinMax(
        expr,
        b_.CreateGEP(mm_struct_ty, lookup, { b_.getInt64(0), b_.getInt32(0) }),
        b_.CreateGEP(mm_struct_ty, lookup, { b_.getInt64(0), b_.getInt32(1) }),
        is_max,
        map.value_type.IsSigned());

    b_.CreateBr(lookup_merge_block);

    b_.SetInsertPoint(lookup_failure_block);

    AllocaInst *mm_struct = b_.CreateAllocaBPF(mm_struct_ty, "mm_struct");

    b_.CreateStore(expr,
                   b_.CreateGEP(mm_struct_ty,
                                mm_struct,
                                { b_.getInt64(0), b_.getInt32(0) }));
    b_.CreateStore(b_.getInt64(1),
                   b_.CreateGEP(mm_struct_ty,
                                mm_struct,
                                { b_.getInt64(0), b_.getInt32(1) }));

    b_.CreateMapUpdateElem(map.ident, scoped_key.value(), mm_struct, call.loc);

    b_.CreateLifetimeEnd(mm_struct);

    b_.CreateBr(lookup_merge_block);
    b_.SetInsertPoint(lookup_merge_block);

    return ScopedExpr();

  } else if (call.func == "avg" || call.func == "stats") {
    Map &map = *call.vargs.at(0).as<Map>();
    ScopedExpr scoped_key = getMapKey(map, call.vargs.at(1));
    CallInst *lookup = b_.CreateMapLookup(map, scoped_key.value());
    ScopedExpr scoped_expr = visit(call.vargs.at(2));

    // promote int to 64-bit
    Value *expr = b_.CreateIntCast(scoped_expr.value(),
                                   b_.getInt64Ty(),
                                   call.vargs.at(2).type().IsSigned());

    llvm::Type *avg_struct_ty = b_.GetMapValueType(map.value_type);

    llvm::Function *parent = b_.GetInsertBlock()->getParent();
    BasicBlock *lookup_success_block = BasicBlock::Create(module_->getContext(),
                                                          "lookup_success",
                                                          parent);
    BasicBlock *lookup_failure_block = BasicBlock::Create(module_->getContext(),
                                                          "lookup_failure",
                                                          parent);
    BasicBlock *lookup_merge_block = BasicBlock::Create(module_->getContext(),
                                                        "lookup_merge",
                                                        parent);

    Value *lookup_condition = b_.CreateICmpNE(
        b_.CreateIntCast(lookup, b_.getPtrTy(), true),
        b_.GetNull(),
        "lookup_cond");
    b_.CreateCondBr(lookup_condition,
                    lookup_success_block,
                    lookup_failure_block);

    b_.SetInsertPoint(lookup_success_block);

    Value *total_val = b_.CreateLoad(b_.getInt64Ty(),
                                     b_.CreateGEP(avg_struct_ty,
                                                  lookup,
                                                  { b_.getInt64(0),
                                                    b_.getInt32(0) }));

    Value *count_val = b_.CreateLoad(b_.getInt64Ty(),
                                     b_.CreateGEP(avg_struct_ty,
                                                  lookup,
                                                  { b_.getInt64(0),
                                                    b_.getInt32(1) }));

    b_.CreateStore(b_.CreateAdd(total_val, expr),
                   b_.CreateGEP(avg_struct_ty,
                                lookup,
                                { b_.getInt64(0), b_.getInt32(0) }));
    b_.CreateStore(b_.CreateAdd(b_.getInt64(1), count_val),
                   b_.CreateGEP(avg_struct_ty,
                                lookup,
                                { b_.getInt64(0), b_.getInt32(1) }));

    b_.CreateBr(lookup_merge_block);

    b_.SetInsertPoint(lookup_failure_block);

    AllocaInst *avg_struct = b_.CreateAllocaBPF(avg_struct_ty, "avg_struct");

    b_.CreateStore(expr,
                   b_.CreateGEP(avg_struct_ty,
                                avg_struct,
                                { b_.getInt64(0), b_.getInt32(0) }));
    b_.CreateStore(b_.getInt64(1),
                   b_.CreateGEP(avg_struct_ty,
                                avg_struct,
                                { b_.getInt64(0), b_.getInt32(1) }));

    b_.CreateMapUpdateElem(map.ident, scoped_key.value(), avg_struct, call.loc);

    b_.CreateLifetimeEnd(avg_struct);

    b_.CreateBr(lookup_merge_block);
    b_.SetInsertPoint(lookup_merge_block);

    return ScopedExpr();

  } else if (call.func == "hist") {
    if (!log2_func_)
      log2_func_ = createLog2Function();

    Map &map = *call.vargs.at(0).as<Map>();
    ScopedExpr scoped_arg = visit(call.vargs.at(2));

    // There is only one log2_func_ so the second argument must be passed
    // as an argument even though it is a constant 0..5
    // Possible optimization is create one function per different value
    // of the second argument.
    ScopedExpr scoped_arg2 = visit(call.vargs.at(3));
    Value *k = b_.CreateIntCast(scoped_arg2.value(), b_.getInt64Ty(), false);

    // promote int to 64-bit
    Value *expr = b_.CreateIntCast(scoped_arg.value(),
                                   b_.getInt64Ty(),
                                   call.vargs.at(2).type().IsSigned());
    Value *log2 = b_.CreateCall(log2_func_, { expr, k }, "log2");
    ScopedExpr scoped_key = getMultiMapKey(
        map, call.vargs.at(1), { log2 }, call.loc);
    b_.CreatePerCpuMapElemAdd(
        map, scoped_key.value(), b_.getInt64(1), call.loc);

    return ScopedExpr();

  } else if (call.func == "lhist") {
    if (!linear_func_)
      linear_func_ = createLinearFunction();

    Map &map = *call.vargs.at(0).as<Map>();

    // prepare arguments
    auto &value_arg = call.vargs.at(2);
    auto &min_arg = call.vargs.at(3);
    auto &max_arg = call.vargs.at(4);
    auto &step_arg = call.vargs.at(5);
    auto scoped_value_arg = visit(value_arg);
    auto scoped_min_arg = visit(min_arg);
    auto scoped_max_arg = visit(max_arg);
    auto scoped_step_arg = visit(step_arg);

    // promote int to 64-bit
    Value *value = b_.CreateIntCast(scoped_value_arg.value(),
                                    b_.getInt64Ty(),
                                    call.vargs.at(2).type().IsSigned());
    Value *min = b_.CreateIntCast(scoped_min_arg.value(),
                                  b_.getInt64Ty(),
                                  false);
    Value *max = b_.CreateIntCast(scoped_max_arg.value(),
                                  b_.getInt64Ty(),
                                  false);
    Value *step = b_.CreateIntCast(scoped_step_arg.value(),
                                   b_.getInt64Ty(),
                                   false);

    Value *linear = b_.CreateCall(linear_func_,
                                  { value, min, max, step },
                                  "linear");

    ScopedExpr scoped_key = getMultiMapKey(
        map, call.vargs.at(1), { linear }, call.loc);
    b_.CreatePerCpuMapElemAdd(
        map, scoped_key.value(), b_.getInt64(1), call.loc);

    return ScopedExpr();
  } else if (call.func == "tseries") {
    // tseries decides what the current bucket is based on the timestamp then
    // updates the bucket's value.
    //
    // void tseries(uint64_t n, uint64_t interval_ns, uint64_t num_intervals) {
    //   uint64_t now = bpf_ktime_get_boot_ns();
    //   struct ts_struct ts_struct_alloc = {};
    //   uint64_t epoch = now / interval_ns;
    //   uint64_t bucket = epoch % num_intervals;
    //   struct ts_struct *bucket_value;
    //   bool key_exists;
    //
    //   bucket_value = bpf_map_lookup_elem(&tseries_map, &bucket);
    //   if (!bucket_value) {
    //     key_exists = false;
    //     bucket_value = &ts_struct_alloc;
    //   } else {
    //     key_exists = true;
    //   }
    //
    // #if defined(SUM) || defined(MIN) || defined(MAX) || defined(AVG)
    //   if (epoch != bucket_value->epoch) {
    //     bucket_value->value = 0;
    //     bucket_value->meta = 0;
    //   }
    // #endif
    //
    // #if defined(SUM)
    //   bucket_value->value += n;
    // #elif defined(MIN) || defined(MAX)
    //   if (!bucket_value->meta) {
    //     bucket_value->value = n;
    //   } else {
    // #if defined(MIN)
    //     bucket_value->value = min(bucket_value->value, n);
    // #else
    //     bucket_value->value = max(bucket_value->value, n);
    // #endif
    //   }
    //   bucket_value->meta = 1;
    // #elif defined(AVG)
    //   bucket_value->value += n;
    //   bucket_value->meta++;
    // #else
    //   bucket_value->value = n;
    //   bucket_value->meta = now;
    // #endif
    //
    //   if (!key_exists) {
    //     bpf_map_update_elem(&tseries_map, &bucket, bucket_value);
    //   }
    // }

    Map &map = *call.vargs.at(0).as<Map>();
    llvm::Function *parent = b_.GetInsertBlock()->getParent();
    llvm::Type *ts_struct_ty = b_.GetMapValueType(map.type());
    AllocaInst *ts_struct_ptr = b_.CreateAllocaBPF(
        PointerType::get(ts_struct_ty, 0), "ts_struct_ptr");

    // Step 1) Figure out which bucket we're using.
    auto map_info = bpftrace_.resources.maps_info.find(map.ident);
    if (map_info == bpftrace_.resources.maps_info.end()) {
      LOG(BUG) << "map name: \"" << map.ident << "\" not found";
    }
    auto &tseries_args = std::get<TSeriesArgs>(map_info->second.detail);
    Value *interval_ns = b_.getInt64(tseries_args.interval_ns);
    Value *num_intervals = b_.getInt64(tseries_args.num_intervals);
    Value *now = createGetNsSwTAI(call.loc);
    Value *epoch = b_.CreateUDiv(now, interval_ns);
    Value *bucket = b_.CreateURem(epoch, num_intervals);
    auto scoped_key = getMultiMapKey(
        map, call.vargs.at(1), { bucket }, call.loc);
    AllocaInst *key_exists = b_.CreateAllocaBPF(b_.getInt8Ty(), "key_exists");

    // Step 2) If the bucket already exists in the map, assign ts_struct_ptr to
    //         the result of the map lookup. Otherwise, assign ts_struct_ptr to
    //         a local AllocaInst.
    CallInst *lookup = b_.CreateMapLookup(map, scoped_key.value());

    BasicBlock *lookup_success_block = BasicBlock::Create(module_->getContext(),
                                                          "lookup_success",
                                                          parent);
    BasicBlock *lookup_failure_block = BasicBlock::Create(module_->getContext(),
                                                          "lookup_failure",
                                                          parent);
    BasicBlock *maybe_clear_block = BasicBlock::Create(module_->getContext(),
                                                       "maybe_clear",
                                                       parent);
    BasicBlock *merge_block = BasicBlock::Create(module_->getContext(),
                                                 "merge",
                                                 parent);
    BasicBlock *update_block = BasicBlock::Create(module_->getContext(),
                                                  "update",
                                                  parent);
    BasicBlock *exit_block = BasicBlock::Create(module_->getContext(),
                                                "exit",
                                                parent);
    Value *lookup_condition = b_.CreateICmpNE(
        b_.CreateIntCast(lookup, b_.getPtrTy(), true),
        b_.GetNull(),
        "map_lookup_cond");
    b_.CreateCondBr(lookup_condition,
                    lookup_success_block,
                    lookup_failure_block);

    b_.SetInsertPoint(lookup_success_block);

    // Success: ts_struct_ptr just points to what's in the map.
    b_.CreateStore(
        b_.CreatePointerCast(lookup, PointerType::get(ts_struct_ty, 0), "cast"),
        ts_struct_ptr);

    b_.CreateStore(b_.getInt8(1), key_exists);

    b_.CreateBr(maybe_clear_block);

    b_.SetInsertPoint(lookup_failure_block);

    // Failure: ts_struct_ptr points to a zero-initialized ts_struct_ty.
    AllocaInst *ts_struct = b_.CreateAllocaBPF(ts_struct_ty, "ts_struct");

    b_.CreateStore(b_.getInt64(0),
                   b_.CreateGEP(ts_struct_ty,
                                ts_struct,
                                { b_.getInt64(0), b_.getInt32(0) }));

    b_.CreateStore(b_.getInt64(0),
                   b_.CreateGEP(ts_struct_ty,
                                ts_struct,
                                { b_.getInt64(0), b_.getInt32(1) }));

    b_.CreateStore(epoch,
                   b_.CreateGEP(ts_struct_ty,
                                ts_struct,
                                { b_.getInt64(0), b_.getInt32(2) }));

    b_.CreateStore(ts_struct, ts_struct_ptr);

    b_.CreateStore(b_.getInt8(0), key_exists);

    b_.CreateBr(maybe_clear_block);

    // Step 3) If we do aggregation, check if we need to reset the bucket before
    //         updating it.
    b_.SetInsertPoint(maybe_clear_block);

    Value *ptr = b_.CreateLoad(PointerType::get(ts_struct_ty, 0),
                               ts_struct_ptr);

    Value *value_ptr = b_.CreateGEP(ts_struct_ty,
                                    ptr,
                                    { b_.getInt64(0), b_.getInt32(0) });
    Value *meta_ptr = b_.CreateGEP(ts_struct_ty,
                                   ptr,
                                   { b_.getInt64(0), b_.getInt32(1) });
    Value *epoch_ptr = b_.CreateGEP(ts_struct_ty,
                                    ptr,
                                    { b_.getInt64(0), b_.getInt32(2) });

    if (tseries_args.agg != TSeriesAggFunc::none) {
      Value *old_epoch = b_.CreateLoad(b_.getInt64Ty(), epoch_ptr);
      BasicBlock *clear_block = BasicBlock::Create(module_->getContext(),
                                                   "clear",
                                                   parent);

      b_.CreateCondBr(b_.CreateICmpNE(old_epoch, epoch, "new_epoch"),
                      clear_block,
                      merge_block);

      b_.SetInsertPoint(clear_block);

      // Clear the current bucket's value and metadata if it's a new epoch.
      b_.CreateStore(b_.getInt64(0), value_ptr);
      b_.CreateStore(b_.getInt64(0), meta_ptr);
    }

    // Step 4) Update the current bucket
    b_.CreateBr(merge_block);

    b_.SetInsertPoint(merge_block);

    auto &value_arg = call.vargs.at(2);
    ScopedExpr scoped_expr = visit(value_arg);
    // promote int to 64-bit
    Value *cast = b_.CreateIntCast(scoped_expr.value(),
                                   b_.getInt64Ty(),
                                   value_arg.type().IsSigned());

    // Update the value and metadata.
    switch (tseries_args.agg) {
      case TSeriesAggFunc::avg:
        b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), meta_ptr),
                                    b_.getInt64(1)),
                       meta_ptr);
        [[fallthrough]];
      case TSeriesAggFunc::sum:
        b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(), value_ptr),
                                    cast),
                       value_ptr);
        break;
      case TSeriesAggFunc::max:
      case TSeriesAggFunc::min:
        b_.CreateMinMax(cast,
                        value_ptr,
                        meta_ptr,
                        tseries_args.agg == TSeriesAggFunc::max,
                        value_arg.type().IsSigned());
        break;
      case TSeriesAggFunc::none:
        b_.CreateStore(cast, value_ptr);
        b_.CreateStore(now, meta_ptr);
        break;
      default:
        LOG(BUG) << "disallowed type \"" << tseries_args.agg << "\"";
    }

    b_.CreateStore(epoch, epoch_ptr);

    b_.CreateCondBr(b_.CreateICmpNE(b_.CreateLoad(b_.getInt8Ty(), key_exists),
                                    b_.getInt8(1),
                                    "needs_update"),
                    update_block,
                    exit_block);

    b_.SetInsertPoint(update_block);

    b_.CreateMapUpdateElem(map.ident, scoped_key.value(), ptr, call.loc);

    b_.CreateBr(exit_block);

    b_.SetInsertPoint(exit_block);

    b_.CreateLifetimeEnd(ts_struct);
    b_.CreateLifetimeEnd(ts_struct_ptr);
    b_.CreateLifetimeEnd(key_exists);

    return ScopedExpr();
  } else if (call.func == "delete") {
    auto &map = *call.vargs.at(0).as<Map>();
    auto scoped_key = getMapKey(map, call.vargs.at(1));

    if (!is_bpf_map_clearable(map_types_[map.ident])) {
      // store zero instead of calling bpf_map_delete_elem()
      auto *val = b_.CreateWriteMapValueAllocation(map.value_type,
                                                   map.ident + "_zero",
                                                   call.loc);
      b_.CreateStore(Constant::getNullValue(b_.GetType(map.value_type)), val);
      b_.CreateMapUpdateElem(map.ident, scoped_key.value(), val, call.loc);
      // Assume delete for these kinds of maps always succeeds.
      Value *expr = b_.getInt8(1);
      return ScopedExpr(expr);
    } else {
      Value *ret = b_.CreateMapDeleteElem(
          map, scoped_key.value(), call.ret_val_discarded, call.loc);
      Value *expr = b_.CreateICmpEQ(ret, b_.getInt64(0), "delete_ret");
      return ScopedExpr(expr);
    }
  } else if (call.func == "has_key") {
    auto &arg = call.vargs.at(0);
    auto &map = *arg.as<Map>();
    auto scoped_key = getMapKey(map, call.vargs.at(1));

    CallInst *lookup = b_.CreateMapLookup(map, scoped_key.value());
    Value *expr = b_.CreateICmpNE(b_.CreateIntCast(lookup, b_.getPtrTy(), true),
                                  b_.GetNull(),
                                  "has_key");
    return ScopedExpr(expr);
  } else if (call.func == "str") {
    const auto max_strlen = bpftrace_.config_->max_strlen;
    // Largest read we'll allow = our global string buffer size
    Value *strlen = b_.getInt64(max_strlen);
    if (call.vargs.size() > 1) {
      auto scoped_arg = visit(call.vargs.at(1));
      Value *proposed_strlen = scoped_arg.value();

      // integer comparison: unsigned less-than-or-equal-to
      CmpInst::Predicate P = CmpInst::ICMP_ULE;
      // check whether proposed_strlen is less-than-or-equal-to maximum
      Value *Cmp = b_.CreateICmp(P, proposed_strlen, strlen, "str.min.cmp");
      // select proposed_strlen if it's sufficiently low, otherwise choose
      // maximum
      strlen = b_.CreateSelect(Cmp, proposed_strlen, strlen, "str.min.select");
    }

    // Note that the successful copying of the string will always include the
    // NULL byte, so we explicitly poison the string value up front. This
    // allows the conversion to know when the string has been truncated. We
    // have added an extra byte to the kernel copy to account for this.
    // Anything copied out of this will be copied as a str[N] type that may
    // omit the NUL byte (which indicates that it has been truncated).
    uint64_t padding = 0;
    Value *readlen = strlen;
    if (max_strlen < 1024) {
      padding = 1;
      readlen = b_.CreateAdd(readlen, b_.getInt64(padding));
    }
    Value *buf = b_.CreateGetStrAllocation("str", call.loc, padding);
    b_.CreateMemsetBPF(buf, b_.getInt8(0xff), max_strlen + padding);
    auto &arg0 = call.vargs.front();
    auto scoped_expr = visit(call.vargs.front());
    b_.CreateProbeReadStr(
        buf, readlen, scoped_expr.value(), arg0.type().GetAS(), call.loc);

    if (dyn_cast<AllocaInst>(buf))
      return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
    return ScopedExpr(buf);
  } else if (call.func == "buf") {
    const auto max_strlen = bpftrace_.config_->max_strlen;
    // Subtract out metadata headroom
    uint64_t fixed_buffer_length = max_strlen - sizeof(AsyncEvent::Buf);
    Value *max_length = b_.getInt64(fixed_buffer_length);
    Value *length;

    if (call.vargs.size() > 1) {
      auto &arg = call.vargs.at(1);
      auto scoped_expr = visit(&arg);

      Value *proposed_length = scoped_expr.value();
      if (arg.type().GetSize() != 8)
        proposed_length = b_.CreateZExt(proposed_length, max_length->getType());
      Value *cmp = b_.CreateICmp(
          CmpInst::ICMP_ULE, proposed_length, max_length, "length.cmp");
      length = b_.CreateSelect(
          cmp, proposed_length, max_length, "length.select");

      auto *literal_length = arg.as<Integer>();
      if (literal_length)
        fixed_buffer_length = literal_length->value;
    } else {
      auto &arg = call.vargs.at(0);
      fixed_buffer_length = arg.type().GetNumElements() *
                            arg.type().GetElementTy()->GetSize();
      length = b_.getInt32(fixed_buffer_length);
    }

    Value *buf = b_.CreateGetStrAllocation("buf", call.loc);
    auto elements = AsyncEvent::Buf().asLLVMType(b_, fixed_buffer_length);
    std::ostringstream dynamic_sized_struct_name;
    dynamic_sized_struct_name << "buffer_" << fixed_buffer_length << "_t";
    StructType *buf_struct = b_.GetStructType(dynamic_sized_struct_name.str(),
                                              elements,
                                              true);

    Value *buf_len_offset = b_.CreateGEP(buf_struct,
                                         buf,
                                         { b_.getInt32(0), b_.getInt32(0) });
    length = b_.CreateIntCast(length, buf_struct->getElementType(0), false);
    b_.CreateStore(length, buf_len_offset);

    Value *buf_data_offset = b_.CreateGEP(buf_struct,
                                          buf,
                                          { b_.getInt32(0), b_.getInt32(1) });
    b_.CreateMemsetBPF(buf_data_offset, b_.getInt8(0), fixed_buffer_length);

    auto scoped_expr = visit(call.vargs.front());
    auto &arg0 = call.vargs.front();
    b_.CreateProbeRead(buf_data_offset,
                       length,
                       scoped_expr.value(),
                       find_addrspace_stack(arg0.type()),
                       call.loc);

    if (dyn_cast<AllocaInst>(buf))
      return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
    return ScopedExpr(buf);
  } else if (call.func == "path") {
    Value *buf = b_.CreateGetStrAllocation("path", call.loc);
    const auto max_size = bpftrace_.config_->max_strlen;
    b_.CreateMemsetBPF(buf, b_.getInt8(0), max_size);
    Value *sz;
    if (call.vargs.size() > 1) {
      auto scoped_arg = visit(call.vargs.at(1));
      Value *pr_sz = b_.CreateIntCast(scoped_arg.value(),
                                      b_.getInt32Ty(),
                                      false);
      Value *max_sz = b_.getInt32(max_size);
      Value *cmp = b_.CreateICmp(
          CmpInst::ICMP_ULE, pr_sz, max_sz, "path.size.cmp");
      sz = b_.CreateSelect(cmp, pr_sz, max_sz, "path.size.select");
    } else {
      sz = b_.getInt32(max_size);
    }

    auto scoped_arg = visit(call.vargs.front());
    Value *value = scoped_arg.value();
    b_.CreatePath(buf,
                  b_.CreateCast(value->getType()->isPointerTy()
                                    ? Instruction::BitCast
                                    : Instruction::IntToPtr,
                                value,
                                b_.getPtrTy()),
                  sz,
                  call.loc);

    if (dyn_cast<AllocaInst>(buf))
      return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
    return ScopedExpr(buf);
  } else if (call.func == "kaddr") {
    uint64_t addr;
    auto name = call.vargs.at(0).as<String>()->value;
    addr = bpftrace_.resolve_kname(name);
    if (!addr)
      call.addError() << "Failed to resolve kernel symbol: " << name;
    return ScopedExpr(b_.getInt64(addr));
  } else if (call.func == "percpu_kaddr") {
    auto name = call.vargs.at(0).as<String>()->value;
    auto *var = DeclareKernelVar(name);
    Value *percpu_ptr;
    if (call.vargs.size() == 1) {
      percpu_ptr = b_.CreateThisCpuPtr(var, call.loc);
    } else {
      auto scoped_cpu = visit(call.vargs.at(1));
      percpu_ptr = b_.CreatePerCpuPtr(var, scoped_cpu.value(), call.loc);
    }
    return ScopedExpr(b_.CreatePtrToInt(percpu_ptr, b_.getInt64Ty()));
  } else if (call.func == "uaddr") {
    auto name = call.vargs.at(0).as<String>()->value;
    struct symbol sym = {};
    int err = bpftrace_.resolve_uname(name,
                                      &sym,
                                      current_attach_point_->target);
    if (err < 0 || sym.address == 0)
      call.addError() << "Could not resolve symbol: "
                      << current_attach_point_->target << ":" << name;
    return ScopedExpr(b_.getInt64(sym.address));
  } else if (call.func == "cgroupid") {
    uint64_t cgroupid;
    auto path = call.vargs.at(0).as<String>()->value;
    cgroupid = util::resolve_cgroupid(path);
    return ScopedExpr(b_.getInt64(cgroupid));
  } else if (call.func == "join") {
    createJoinCall(call, async_ids_.join());
    return ScopedExpr();
  } else if (call.func == "ksym") {
    // We want to just pass through from the child node.
    return visit(call.vargs.front());
  } else if (call.func == "usym") {
    auto scoped_arg = visit(call.vargs.front());
    return ScopedExpr(
        b_.CreateUSym(scoped_arg.value(), get_probe_id(), call.loc),
        std::move(scoped_arg));
  } else if (call.func == "ntop") {
    // struct {
    //   int af_type;
    //   union {
    //     char[4] inet4;
    //     char[16] inet6;
    //   }
    // }
    //}
    std::vector<llvm::Type *> elements = { b_.getInt64Ty(),
                                           ArrayType::get(b_.getInt8Ty(), 16) };
    StructType *inet_struct = b_.GetStructType("inet", elements, false);

    AllocaInst *buf = b_.CreateAllocaBPF(inet_struct, "inet");

    Value *af_offset = b_.CreateGEP(inet_struct,
                                    buf,
                                    { b_.getInt64(0), b_.getInt32(0) });
    Value *af_type;

    size_t inet_index = 0;
    if (call.vargs.size() == 1) {
      auto &inet = call.vargs.at(0);
      if (inet.type().IsIntegerTy() || inet.type().GetSize() == 4) {
        af_type = b_.getInt64(AF_INET);
      } else {
        af_type = b_.getInt64(AF_INET6);
      }
    } else {
      inet_index = 1;
      auto scoped_arg = visit(call.vargs.at(0));
      af_type = b_.CreateIntCast(scoped_arg.value(), b_.getInt64Ty(), true);
    }
    b_.CreateStore(af_type, af_offset);

    Value *inet_offset = b_.CreateGEP(inet_struct,
                                      buf,
                                      { b_.getInt32(0), b_.getInt32(1) });
    b_.CreateMemsetBPF(inet_offset, b_.getInt8(0), 16);

    auto &inet = call.vargs.at(inet_index);
    auto scoped_inet = visit(inet);
    if (inet.type().IsArrayTy() || inet.type().IsStringTy()) {
      b_.CreateProbeRead(static_cast<AllocaInst *>(inet_offset),
                         inet.type(),
                         scoped_inet.value(),
                         call.loc);
    } else {
      b_.CreateStore(
          b_.CreateIntCast(scoped_inet.value(), b_.getInt32Ty(), false),
          inet_offset);
    }

    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "pton") {
    auto af_type = AF_INET;
    int addr_size = 4;
    std::string addr = call.vargs.at(0).as<String>()->value;
    if (addr.find(":") != std::string::npos) {
      af_type = AF_INET6;
      addr_size = 16;
    }

    llvm::Type *array_t = ArrayType::get(b_.getInt8Ty(), addr_size);
    AllocaInst *buf;
    if (af_type == AF_INET6) {
      buf = b_.CreateAllocaBPF(array_t, "addr6");
    } else {
      buf = b_.CreateAllocaBPF(array_t, "addr4");
    }

    std::vector<char> dst(addr_size);
    Value *octet;
    auto ret = inet_pton(af_type, addr.c_str(), dst.data());
    if (ret != 1) {
      call.addError() << "inet_pton() call returns " << std::to_string(ret);
    }
    for (int i = 0; i < addr_size; i++) {
      octet = b_.getInt8(dst[i]);
      b_.CreateStore(
          octet,
          b_.CreateGEP(array_t, buf, { b_.getInt64(0), b_.getInt64(i) }));
    }

    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "reg") {
    auto reg_name = call.vargs.at(0).as<String>()->value;
    auto offset = arch::Host::register_to_pt_regs_offset(reg_name);
    if (!offset) {
      call.addError() << "register " << reg_name
                      << " not available on architecture "
                      << arch::Host::Machine;
      return ScopedExpr(b_.getInt64(0));
    }

    return ScopedExpr(b_.CreateRegisterRead(ctx_,
                                            offset.value(),
                                            call.func + "_" + reg_name));
  } else if (call.func == "printf") {
    // We overload printf call for iterator probe's seq_printf helper.
    if (!inside_subprog_ &&
        probetype(current_attach_point_->provider) == ProbeType::iter) {
      auto nargs = call.vargs.size() - 1;

      int ptr_size = sizeof(unsigned long);
      int data_size = 0;

      // create buffer to store the argument expression values
      SizedType data_type = CreateArray(nargs, CreateUInt64());
      AllocaInst *data = b_.CreateAllocaBPFInit(data_type, "data");

      std::vector<ScopedExpr> scoped_args;
      scoped_args.reserve(call.vargs.size());
      for (size_t i = 1; i < call.vargs.size(); i++) {
        // process argument expression
        Expression &arg = call.vargs.at(i);
        auto scoped_arg = visit(&arg);
        Value *value = scoped_arg.value();

        // and store it to data area
        Value *offset = b_.CreateGEP(b_.GetType(data_type),
                                     data,
                                     { b_.getInt64(0), b_.getInt32(i - 1) });
        b_.CreateStore(value, offset);

        // keep the expression alive, so it's still there
        // for following seq_printf call
        scoped_args.emplace_back(std::move(scoped_arg));
        data_size += ptr_size;
      }

      // pick the current format string
      auto print_id = async_ids_.bpf_print();
      auto *fmt = createFmtString(print_id);
      const auto &s = bpftrace_.resources.bpf_print_fmts.at(print_id).str();

      // and finally the seq_printf call
      b_.CreateSeqPrintf(ctx_,
                         b_.CreateIntToPtr(fmt, b_.getPtrTy()),
                         b_.getInt32(s.size() + 1),
                         data,
                         b_.getInt32(data_size),
                         call.loc);
      return ScopedExpr();

    } else {
      auto async_id = async_ids_.printf();
      createFormatStringCall(call,
                             async_id,
                             std::get<1>(
                                 bpftrace_.resources.printf_args[async_id]),
                             "printf",
                             async_action::AsyncAction::printf);
      return ScopedExpr();
    }
  } else if (call.func == "errorf") {
    auto async_id = async_ids_.printf();
    createFormatStringCall(call,
                           async_id,
                           std::get<1>(
                               bpftrace_.resources.printf_args[async_id]),
                           "errorf",
                           async_action::AsyncAction::printf);
    return ScopedExpr();
  } else if (call.func == "debugf") {
    auto print_id = async_ids_.bpf_print();
    auto *fmt = createFmtString(print_id);
    const auto &s = bpftrace_.resources.bpf_print_fmts.at(print_id).str();

    std::vector<Value *> values;
    std::vector<ScopedExpr> exprs;
    for (size_t i = 1; i < call.vargs.size(); i++) {
      Expression &arg = call.vargs.at(i);
      auto scoped_expr = visit(arg);
      values.push_back(scoped_expr.value());
      exprs.emplace_back(std::move(scoped_expr));
    }

    b_.CreateTracePrintk(b_.CreateIntToPtr(fmt, b_.getPtrTy()),
                         b_.getInt32(s.size() + 1),
                         values,
                         call.loc);
    return ScopedExpr();
  } else if (call.func == "system") {
    auto async_id = async_ids_.system();
    createFormatStringCall(call,
                           async_id,
                           std::get<1>(
                               bpftrace_.resources.system_args[async_id]),
                           "system",
                           async_action::AsyncAction::syscall);
    return ScopedExpr();
  } else if (call.func == "cat") {
    auto async_id = async_ids_.cat();
    createFormatStringCall(call,
                           async_id,
                           std::get<1>(bpftrace_.resources.cat_args[async_id]),
                           "cat",
                           async_action::AsyncAction::cat);
    return ScopedExpr();
  } else if (call.func == "exit") {
    auto elements = AsyncEvent::Exit().asLLVMType(b_);
    StructType *exit_struct = b_.GetStructType("exit_t", elements, true);
    AllocaInst *buf = b_.CreateAllocaBPF(exit_struct, "exit");
    size_t struct_size = datalayout().getTypeAllocSize(exit_struct);

    // Fill in exit struct.
    b_.CreateStore(
        b_.getInt64(static_cast<int64_t>(async_action::AsyncAction::exit)),
        b_.CreateGEP(exit_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));

    Value *code = b_.getInt8(0);
    if (call.vargs.size() == 1) {
      auto scoped_expr = visit(call.vargs.at(0));
      code = scoped_expr.value();
    }
    b_.CreateStore(
        code,
        b_.CreateGEP(exit_struct, buf, { b_.getInt64(0), b_.getInt32(1) }));

    b_.CreateOutput(buf, struct_size, call.loc);
    b_.CreateLifetimeEnd(buf);

    createRet();

    // create an unreachable basic block for all the "dead instructions" that
    // may come after exit(). If we don't, LLVM will emit the instructions
    // leading to a `unreachable insn` warning from the verifier
    BasicBlock *deadcode = BasicBlock::Create(module_->getContext(),
                                              "deadcode",
                                              b_.GetInsertBlock()->getParent());
    b_.SetInsertPoint(deadcode);
    return ScopedExpr();
  } else if (call.func == "print") {
    if (call.vargs.at(0).is<Map>()) {
      createPrintMapCall(call);
    } else {
      createPrintNonMapCall(call, async_ids_.non_map_print());
    }
    return ScopedExpr();
  } else if (call.func == "cgroup_path") {
    auto elements = AsyncEvent::CgroupPath().asLLVMType(b_);
    StructType *cgroup_path_struct = b_.GetStructType(call.func + "_t",
                                                      elements,
                                                      true);
    AllocaInst *buf = b_.CreateAllocaBPF(cgroup_path_struct,
                                         call.func + "_args");

    // Store cgroup path event id
    b_.CreateStore(b_.GetIntSameSize(async_ids_.cgroup_path(), elements.at(0)),
                   b_.CreateGEP(cgroup_path_struct,
                                buf,
                                { b_.getInt64(0), b_.getInt32(0) }));

    // Store cgroup id
    auto &arg = call.vargs.at(0);
    auto scoped_expr = visit(arg);
    b_.CreateStore(scoped_expr.value(),
                   b_.CreateGEP(cgroup_path_struct,
                                buf,
                                { b_.getInt64(0), b_.getInt32(1) }));

    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "clear" || call.func == "zero") {
    auto elements = AsyncEvent::MapEvent().asLLVMType(b_);
    StructType *event_struct = b_.GetStructType(call.func + "_t",
                                                elements,
                                                true);

    auto &arg = call.vargs.at(0);
    auto &map = *arg.as<Map>();

    AllocaInst *buf = b_.CreateAllocaBPF(event_struct,
                                         call.func + "_" + map.ident);

    auto *aa_ptr = b_.CreateGEP(event_struct,
                                buf,
                                { b_.getInt64(0), b_.getInt32(0) });
    if (call.func == "clear")
      b_.CreateStore(b_.GetIntSameSize(static_cast<int64_t>(
                                           async_action::AsyncAction::clear),
                                       elements.at(0)),
                     aa_ptr);
    else
      b_.CreateStore(b_.GetIntSameSize(static_cast<int64_t>(
                                           async_action::AsyncAction::zero),
                                       elements.at(0)),
                     aa_ptr);

    int id = bpftrace_.resources.maps_info.at(map.ident).id;
    if (id == -1) {
      LOG(BUG) << "map id for map \"" << map.ident << "\" not found";
    }
    auto *ident_ptr = b_.CreateGEP(event_struct,
                                   buf,
                                   { b_.getInt64(0), b_.getInt32(1) });
    b_.CreateStore(b_.GetIntSameSize(id, elements.at(1)), ident_ptr);

    b_.CreateOutput(buf, getStructSize(event_struct), call.loc);
    return ScopedExpr(buf, [this, buf] { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "len") {
    if (call.vargs.at(0).type().IsStack()) {
      auto &arg = call.vargs.at(0);
      auto scoped_arg = visit(arg);

      auto *stack_key_struct = b_.GetStackStructType(arg.type().IsUstackTy());
      Value *nr_stack_frames = b_.CreateGEP(stack_key_struct,
                                            scoped_arg.value(),
                                            { b_.getInt64(0), b_.getInt32(1) });
      return ScopedExpr(
          b_.CreateIntCast(b_.CreateLoad(b_.getInt64Ty(), nr_stack_frames),
                           b_.getInt64Ty(),
                           false));
    } else /* call.vargs.at(0)->is_map */ {
      auto &arg = call.vargs.at(0);
      auto &map = *arg.as<Map>();

      // This is defined only for non-scalar maps.
      if (bpftrace_.feature_->has_kernel_func(Kfunc::bpf_map_sum_elem_count)) {
        return ScopedExpr(CreateKernelFuncCall(Kfunc::bpf_map_sum_elem_count,
                                               { b_.GetMapVar(map.ident) },
                                               "len",
                                               call));
      } else {
        if (!map_len_func_)
          map_len_func_ = createMapLenCallback();

        return ScopedExpr(
            b_.CreateForEachMapElem(map, map_len_func_, nullptr, call.loc));
      }
    }
  } else if (call.func == "time") {
    auto elements = AsyncEvent::Time().asLLVMType(b_);
    StructType *time_struct = b_.GetStructType(call.func + "_t",
                                               elements,
                                               true);

    AllocaInst *buf = b_.CreateAllocaBPF(time_struct, call.func + "_t");

    b_.CreateStore(
        b_.GetIntSameSize(static_cast<int64_t>(async_action::AsyncAction::time),
                          elements.at(0)),
        b_.CreateGEP(time_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));

    b_.CreateStore(
        b_.GetIntSameSize(async_ids_.time(), elements.at(1)),
        b_.CreateGEP(time_struct, buf, { b_.getInt64(0), b_.getInt32(1) }));

    b_.CreateOutput(buf, getStructSize(time_struct), call.loc);
    return ScopedExpr(buf, [this, buf] { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "strftime") {
    auto elements = AsyncEvent::Strftime().asLLVMType(b_);
    StructType *strftime_struct = b_.GetStructType(call.func + "_t",
                                                   elements,
                                                   true);

    AllocaInst *buf = b_.CreateAllocaBPF(strftime_struct, call.func + "_args");
    b_.CreateStore(
        b_.GetIntSameSize(async_ids_.strftime(), elements.at(0)),
        b_.CreateGEP(strftime_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));
    b_.CreateStore(
        b_.GetIntSameSize(static_cast<std::underlying_type_t<TimestampMode>>(
                              call.return_type.ts_mode),
                          elements.at(1)),
        b_.CreateGEP(strftime_struct, buf, { b_.getInt64(0), b_.getInt32(1) }));
    auto &arg = call.vargs.at(1);
    auto scoped_expr = visit(arg);
    b_.CreateStore(
        scoped_expr.value(),
        b_.CreateGEP(strftime_struct, buf, { b_.getInt64(0), b_.getInt32(2) }));
    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "kstack" || call.func == "ustack") {
    return kstack_ustack(call.func, call.return_type.stack_type, call.loc);
  } else if (call.func == "signal") {
    // long bpf_send_signal(u32 sig)
    auto &arg = call.vargs.at(0);
    if (arg.type().IsStringTy()) {
      auto signame = arg.as<String>()->value;
      int sigid = signal_name_to_num(signame);
      // Should be caught in semantic analyser
      if (sigid < 1) {
        LOG(BUG) << "Invalid signal ID for \"" << signame << "\"";
      }
      b_.CreateSignal(b_.getInt32(sigid), call.loc);
      return ScopedExpr();
    }
    auto scoped_arg = visit(arg);
    Value *sig_number = b_.CreateIntCast(scoped_arg.value(),
                                         b_.getInt32Ty(),
                                         arg.type().IsSigned());
    b_.CreateSignal(sig_number, call.loc);
    return ScopedExpr();
  } else if (call.func == "strerror") {
    return visit(call.vargs.front());
  } else if (call.func == "strncmp") {
    auto &left_arg = call.vargs.at(0);
    auto &right_arg = call.vargs.at(1);
    auto size_opt = call.vargs.at(2).as<Integer>()->value;
    uint64_t size = std::min(
        { size_opt, left_arg.type().GetSize(), right_arg.type().GetSize() });

    auto left_string = visit(&left_arg);
    auto right_string = visit(&right_arg);

    return ScopedExpr(b_.CreateStrncmp(
        left_string.value(), right_string.value(), size, false));
  } else if (call.func == "strcontains") {
    auto &left_arg = call.vargs.at(0);
    auto &right_arg = call.vargs.at(1);

    auto left_string = visit(left_arg);
    auto right_string = visit(right_arg);

    return ScopedExpr(b_.CreateStrcontains(left_string.value(),
                                           left_arg.type().GetSize(),
                                           right_string.value(),
                                           right_arg.type().GetSize()));
  } else if (call.func == "override") {
    // long bpf_override(struct pt_regs *regs, u64 rc)
    // returns: 0
    auto &arg = call.vargs.at(0);
    auto scoped_arg = visit(arg);
    auto *expr = b_.CreateIntCast(scoped_arg.value(),
                                  b_.getInt64Ty(),
                                  arg.type().IsSigned());
    b_.CreateOverrideReturn(ctx_, expr);
    return ScopedExpr();
  } else if (call.func == "kptr" || call.func == "uptr") {
    return visit(call.vargs.at(0));
  } else if (call.func == "macaddr") {
    // MAC addresses are presented as char[6]
    AllocaInst *buf = b_.CreateAllocaBPFInit(call.return_type, "macaddr");
    auto &macaddr = call.vargs.front();
    auto scoped_arg = visit(macaddr);

    if (inBpfMemory(macaddr.type()))
      b_.CreateMemcpyBPF(buf, scoped_arg.value(), macaddr.type().GetSize());
    else
      b_.CreateProbeRead(buf, macaddr.type(), scoped_arg.value(), call.loc);

    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "unwatch") {
    auto scoped_addr = visit(call.vargs.at(0));

    auto elements = AsyncEvent::WatchpointUnwatch().asLLVMType(b_);
    StructType *unwatch_struct = b_.GetStructType("unwatch_t", elements, true);
    AllocaInst *buf = b_.CreateAllocaBPF(unwatch_struct, "unwatch");
    size_t struct_size = datalayout().getTypeAllocSize(unwatch_struct);

    b_.CreateStore(
        b_.getInt64(
            static_cast<int64_t>(async_action::AsyncAction::watchpoint_detach)),
        b_.CreateGEP(unwatch_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));
    b_.CreateStore(
        b_.CreateIntCast(scoped_addr.value(),
                         b_.getInt64Ty(),
                         false /* unsigned */),
        b_.CreateGEP(unwatch_struct, buf, { b_.getInt64(0), b_.getInt32(1) }));
    b_.CreateOutput(buf, struct_size, call.loc);
    return ScopedExpr(buf, [this, buf] { b_.CreateLifetimeEnd(buf); });
  } else if (call.func == "bswap") {
    auto &arg = call.vargs.at(0);
    auto scoped_arg = visit(arg);

    assert(arg.type().IsIntegerTy());
    if (arg.type().GetSize() > 1) {
      llvm::Type *arg_type = b_.GetType(arg.type());
#if LLVM_VERSION_MAJOR >= 20
      llvm::Function *swap_fun = Intrinsic::getOrInsertDeclaration(
          module_.get(), Intrinsic::bswap, { arg_type });
#else
      llvm::Function *swap_fun = Intrinsic::getDeclaration(module_.get(),
                                                           Intrinsic::bswap,
                                                           { arg_type });
#endif

      return ScopedExpr(b_.CreateCall(swap_fun, { scoped_arg.value() }),
                        std::move(scoped_arg));
    }
    return scoped_arg;
  } else if (call.func == "skboutput") {
    auto elements = AsyncEvent::SkbOutput().asLLVMType(b_);
    StructType *hdr_t = b_.GetStructType("hdr_t", elements, false);
    AllocaInst *data = b_.CreateAllocaBPF(hdr_t, "hdr");

    // The extra 0 here ensures the type of addr_offset will be int64
    Value *aid_addr = b_.CreateGEP(hdr_t,
                                   data,
                                   { b_.getInt64(0), b_.getInt32(0) });
    Value *id_addr = b_.CreateGEP(hdr_t,
                                  data,
                                  { b_.getInt64(0), b_.getInt32(1) });
    Value *time_addr = b_.CreateGEP(hdr_t,
                                    data,
                                    { b_.getInt64(0), b_.getInt32(2) });

    b_.CreateStore(b_.getInt64(static_cast<int64_t>(
                       async_action::AsyncAction::skboutput)),
                   aid_addr);
    b_.CreateStore(b_.getInt64(async_ids_.skb_output()), id_addr);
    b_.CreateStore(b_.CreateGetNs(TimestampMode::boot, call.loc), time_addr);

    auto scoped_skb = visit(call.vargs.at(1));
    auto scoped_arg_len = visit(call.vargs.at(2));
    Value *len = b_.CreateIntCast(scoped_arg_len.value(),
                                  b_.getInt64Ty(),
                                  false);
    Value *ret = b_.CreateSkbOutput(
        scoped_skb.value(), len, data, getStructSize(hdr_t));
    return ScopedExpr(ret);
  } else if (call.func == "nsecs") {
    if (call.return_type.ts_mode == TimestampMode::sw_tai) {
      return ScopedExpr(createGetNsSwTAI(call.loc));
    } else {
      return ScopedExpr(b_.CreateGetNs(call.return_type.ts_mode, call.loc));
    }
  } else if (call.func == "pid") {
    bool force_init = shouldForceInitPidNs(call.vargs);

    return ScopedExpr(b_.CreateGetPid(call.loc, force_init));
  } else if (call.func == "tid") {
    bool force_init = shouldForceInitPidNs(call.vargs);

    return ScopedExpr(b_.CreateGetTid(call.loc, force_init));
  } else if (call.func == "socket_cookie") {
    auto scoped_arg = visit(call.vargs.at(0));

    return ScopedExpr(b_.CreateGetSocketCookie(scoped_arg.value(), call.loc));
  } else {
    auto *func = extern_funcs_[call.func];
    if (!func) {
      // If we don't know about this function for codegen, then it is very
      // likely something that will be linked in from the standard library.
      // Assume that the semantic analyser has provided the types correctly,
      // and set everything up for success.
      llvm::Type *result_type = b_.GetType(call.return_type);
      SmallVector<llvm::Type *> arg_types;
      for (const auto &expr : call.vargs) {
        arg_types.push_back(b_.GetType(expr.type()));
      }
      FunctionType *function_type = FunctionType::get(result_type,
                                                      arg_types,
                                                      false);
      func = llvm::Function::Create(function_type,
                                    llvm::Function::ExternalLinkage,
                                    call.func,
                                    module_.get());
      func->addFnAttr(Attribute::AlwaysInline);
      func->addFnAttr(Attribute::NoUnwind);
      func->setDSOLocal(true);

      // Add noundef attribute to each argument.
      for (auto &arg : func->args()) {
        arg.addAttr(Attribute::NoUndef);
      }
      extern_funcs_[call.func] = func;
    }

    std::vector<ScopedExpr> args;
    SmallVector<llvm::Value *> arg_values;
    for (auto &expr : call.vargs) {
      args.emplace_back(visit(expr));
      arg_values.push_back(args.back().value());
    }

    auto *inst = b_.CreateCall(func, arg_values, call.func);
    return ScopedExpr(inst, [this, inst, &call] {
      // We set the debug location on the call instructions only after the
      // scoped expression is not longer used. Otherwise the instruction emitter
      // seems to use this location for everything, which results in problems.
      inst->setDebugLoc(
          debug_.createDebugLocation(llvm_ctx_, scope_, call.loc));
    });
  }
}

llvm::Value *CodegenLLVM::createGetNsSwTAI(const Location &loc)
{
  if (!bpftrace_.delta_taitime_.has_value())
    LOG(BUG) << "delta_taitime_ should have been checked in semantic analysis";
  uint64_t delta = (bpftrace_.delta_taitime_->tv_sec * 1e9) +
                   bpftrace_.delta_taitime_->tv_nsec;
  Value *ns = b_.CreateGetNs(TimestampMode::boot, loc);
  return b_.CreateAdd(ns, b_.getInt64(delta));
}

ScopedExpr CodegenLLVM::visit(Sizeof &szof)
{
  if (std::holds_alternative<SizedType>(szof.record)) {
    return ScopedExpr(b_.getInt64(std::get<SizedType>(szof.record).GetSize()));
  }
  return ScopedExpr(
      b_.getInt64(std::get<Expression>(szof.record).type().GetSize()));
}

ScopedExpr CodegenLLVM::visit(Offsetof &offof)
{
  ssize_t offset = 0;
  SizedType record;
  if (std::holds_alternative<SizedType>(offof.record)) {
    record = std::get<SizedType>(offof.record);
  } else {
    record = std::get<Expression>(offof.record).type();
  }
  for (const auto &field : offof.field) {
    offset += record.GetField(field).offset;
    record = record.GetField(field).type;
  }
  return ScopedExpr(b_.getInt64(offset));
}

ScopedExpr CodegenLLVM::visit([[maybe_unused]] Map &map)
{
  // This is not currently used in code generation. Code is generated either
  // via `MapAccess` for reads or via `AssignMapStatement` for writes.
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(Variable &var)
{
  // Arrays and structs are not memcopied for local variables
  if (needMemcpy(var.var_type) &&
      !(var.var_type.IsArrayTy() || var.var_type.IsRecordTy())) {
    return ScopedExpr(getVariable(var.ident).value);
  } else {
    auto &var_llvm = getVariable(var.ident);
    return ScopedExpr(b_.CreateLoad(var_llvm.type, var_llvm.value));
  }
}

ScopedExpr CodegenLLVM::binop_string(Binop &binop)
{
  if (binop.op != Operator::EQ && binop.op != Operator::NE) {
    LOG(BUG) << "missing codegen to string operator \"" << opstr(binop) << "\"";
  }

  // strcmp returns 0 when strings are equal
  bool inverse = binop.op == Operator::EQ;

  auto left_string = visit(binop.left);
  auto right_string = visit(binop.right);

  size_t len = std::min(binop.left.type().GetSize(),
                        binop.right.type().GetSize());
  return ScopedExpr(b_.CreateStrncmp(
      left_string.value(), right_string.value(), len, inverse));
}

ScopedExpr CodegenLLVM::binop_integer_array(Binop &binop)
{
  assert(binop.op == Operator::EQ || binop.op == Operator::NE);

  // integer array compare returns 0 when arrays are equal
  bool inverse = binop.op == Operator::EQ;

  auto scoped_left = visit(binop.left);
  auto scoped_right = visit(binop.right);
  Value *left_array_val = scoped_left.value();
  Value *right_array_val = scoped_right.value();
  const auto &left_array_ty = binop.left.type();
  const auto &right_array_ty = binop.right.type();

  assert(left_array_ty.GetNumElements() == right_array_ty.GetNumElements());
  assert(left_array_ty.GetElementTy()->GetSize() ==
         right_array_ty.GetElementTy()->GetSize());

  return ScopedExpr(b_.CreateIntegerArrayCmp(left_array_val,
                                             right_array_val,
                                             left_array_ty,
                                             right_array_ty,
                                             inverse,
                                             binop.loc,
                                             createLoopMetadata()));
}

ScopedExpr CodegenLLVM::binop_buf(Binop &binop)
{
  if (binop.op != Operator::EQ && binop.op != Operator::NE) {
    LOG(BUG) << "missing codegen to buffer operator \"" << opstr(binop) << "\"";
  }

  // strcmp returns 0 when strings are equal
  bool inverse = binop.op == Operator::EQ;

  auto scoped_left = visit(binop.left);
  auto scoped_right = visit(binop.right);
  Value *left_string = scoped_left.value();
  Value *right_string = scoped_right.value();

  size_t len = std::min(binop.left.type().GetSize(),
                        binop.right.type().GetSize());
  return ScopedExpr(b_.CreateStrncmp(left_string, right_string, len, inverse));
}

ScopedExpr CodegenLLVM::binop_int(Binop &binop)
{
  auto scoped_left = visit(binop.left);
  auto scoped_right = visit(binop.right);
  Value *lhs = scoped_left.value();
  Value *rhs = scoped_right.value();

  // If left or right is PositionalParameter, that means the syntax is:
  //   str($1 + num) or str(num + $1)
  // The positional params returns a pointer to a buffer, and the buffer should
  // live until str() is accepted. Extend the lifetime of the buffer by moving
  // these into the deletion scoped, where they will run once the value is
  // consumed.
  auto del = [l = std::move(scoped_left), r = std::move(scoped_right)] {};

  bool lsign = binop.left.type().IsSigned();
  bool rsign = binop.right.type().IsSigned();
  bool do_signed = lsign && rsign;

  // Promote operands if necessary
  auto size = std::max(binop.left.type().GetSize(),
                       binop.right.type().GetSize());
  lhs = b_.CreateIntCast(lhs, b_.getIntNTy(size * 8), lsign);
  rhs = b_.CreateIntCast(rhs, b_.getIntNTy(size * 8), rsign);

  switch (binop.op) {
    case Operator::EQ:
      return ScopedExpr(b_.CreateICmpEQ(lhs, rhs), std::move(del));
    case Operator::NE:
      return ScopedExpr(b_.CreateICmpNE(lhs, rhs), std::move(del));
    case Operator::LE:
      return ScopedExpr(do_signed ? b_.CreateICmpSLE(lhs, rhs)
                                  : b_.CreateICmpULE(lhs, rhs),
                        std::move(del));
    case Operator::GE:
      return ScopedExpr(do_signed ? b_.CreateICmpSGE(lhs, rhs)
                                  : b_.CreateICmpUGE(lhs, rhs),
                        std::move(del));
    case Operator::LT:
      return ScopedExpr(do_signed ? b_.CreateICmpSLT(lhs, rhs)
                                  : b_.CreateICmpULT(lhs, rhs),
                        std::move(del));
    case Operator::GT:
      return ScopedExpr(do_signed ? b_.CreateICmpSGT(lhs, rhs)
                                  : b_.CreateICmpUGT(lhs, rhs),
                        std::move(del));
    case Operator::LEFT:
      return ScopedExpr(b_.CreateShl(lhs, rhs), std::move(del));
    case Operator::RIGHT:
      return ScopedExpr(b_.CreateLShr(lhs, rhs), std::move(del));
    case Operator::PLUS:
      return ScopedExpr(b_.CreateAdd(lhs, rhs), std::move(del));
    case Operator::MINUS:
      return ScopedExpr(b_.CreateSub(lhs, rhs), std::move(del));
    case Operator::MUL:
      return ScopedExpr(b_.CreateMul(lhs, rhs), std::move(del));
    case Operator::DIV:
    case Operator::MOD: {
      // Always do an unsigned modulo operation here even if `do_signed`
      // is true. bpf instruction set does not support signed division.
      // We already warn in the semantic analyser that signed modulo can
      // lead to undefined behavior (because we will treat it as unsigned).
      return ScopedExpr(b_.CreateCheckedBinop(binop, lhs, rhs), std::move(del));
    }
    case Operator::BAND:
      return ScopedExpr(b_.CreateAnd(lhs, rhs), std::move(del));
    case Operator::BOR:
      return ScopedExpr(b_.CreateOr(lhs, rhs), std::move(del));
    case Operator::BXOR:
      return ScopedExpr(b_.CreateXor(lhs, rhs), std::move(del));
    default:
      LOG(BUG) << "\"" << opstr(binop) << "\" was handled earlier";
      __builtin_unreachable();
  }
}

ScopedExpr CodegenLLVM::binop_ptr(Binop &binop)
{
  auto compare = false;
  auto arith = false;

  // Do what C does
  switch (binop.op) {
    case Operator::EQ:
    case Operator::NE:
    case Operator::LE:
    case Operator::GE:
    case Operator::LT:
    case Operator::GT:
      compare = true;
      break;
    case Operator::LEFT:
    case Operator::RIGHT:
    case Operator::MOD:
    case Operator::BAND:
    case Operator::BOR:
    case Operator::BXOR:
    case Operator::MUL:
    case Operator::DIV:
      LOG(BUG) << "binop_ptr: op not implemented for type\"" << opstr(binop)
               << "\"";
      break;
    case Operator::PLUS:
    case Operator::MINUS:
      arith = true;
      break;
    default:
      LOG(BUG) << "binop_ptr invalid op \"" << opstr(binop) << "\"";
  }

  auto scoped_left = visit(binop.left);
  auto scoped_right = visit(binop.right);
  Value *lhs = scoped_left.value();
  Value *rhs = scoped_right.value();

  // note: the semantic phase blocks invalid combinations
  if (compare) {
    switch (binop.op) {
      case Operator::EQ:
        return ScopedExpr(b_.CreateICmpEQ(lhs, rhs));
      case Operator::NE:
        return ScopedExpr(b_.CreateICmpNE(lhs, rhs));
      case Operator::LE: {
        return ScopedExpr(b_.CreateICmpULE(lhs, rhs));
      }
      case Operator::GE: {
        return ScopedExpr(b_.CreateICmpUGE(lhs, rhs));
      }
      case Operator::LT: {
        return ScopedExpr(b_.CreateICmpULT(lhs, rhs));
      }
      case Operator::GT: {
        return ScopedExpr(b_.CreateICmpUGT(lhs, rhs));
      }
      default:
        LOG(BUG) << "invalid op \"" << opstr(binop) << "\"";
        __builtin_unreachable();
    }
  } else if (arith) {
    // Cannot use GEP here as LLVM doesn't know its a pointer
    bool leftptr = binop.left.type().IsPtrTy();
    const auto &ptr_ty = leftptr ? binop.left.type() : binop.right.type();
    const auto &other_ty = leftptr ? binop.right.type() : binop.left.type();
    Value *ptr_expr = leftptr ? lhs : rhs;
    Value *other_expr = leftptr ? rhs : lhs;

    if (other_ty.IsIntTy() && other_ty.GetSize() != 8)
      other_expr = b_.CreateZExt(other_expr, b_.getInt64Ty());
    Value *expr = b_.CreatePtrOffset(*ptr_ty.GetPointeeTy(),
                                     other_expr,
                                     ptr_ty.GetAS());
    if (binop.op == Operator::PLUS)
      return ScopedExpr(b_.CreateAdd(ptr_expr, expr));
    else
      return ScopedExpr(b_.CreateSub(ptr_expr, expr));
  } else {
    LOG(BUG) << "unknown op \"" << opstr(binop) << "\"";
    __builtin_unreachable();
  }
}

ScopedExpr CodegenLLVM::visit(Binop &binop)
{
  // Handle && and || separately so short circuiting works
  if (binop.op == Operator::LAND) {
    return createLogicalAnd(binop);
  } else if (binop.op == Operator::LOR) {
    return createLogicalOr(binop);
  }

  const SizedType &type = binop.left.type();
  if (binop.left.type().IsPtrTy() || binop.right.type().IsPtrTy()) {
    return binop_ptr(binop);
  } else if (type.IsStringTy()) {
    return binop_string(binop);
  } else if (type.IsBufferTy()) {
    return binop_buf(binop);
  } else if (type.IsArrayTy() && type.GetElementTy()->IsIntegerTy()) {
    return binop_integer_array(binop);
  } else {
    return binop_int(binop);
  }
}

ScopedExpr CodegenLLVM::unop_int(Unop &unop)
{
  const SizedType &type = unop.expr.type();
  switch (unop.op) {
    case Operator::LNOT: {
      ScopedExpr scoped_expr = visit(unop.expr);
      auto *ty = scoped_expr.value()->getType();
      Value *zero_value = Constant::getNullValue(ty);
      Value *expr = b_.CreateICmpEQ(scoped_expr.value(), zero_value);
      // CreateICmpEQ() returns 1-bit integer
      // Cast it to the same type of the operand
      // Use unsigned extension, otherwise !0 becomes -1
      return ScopedExpr(b_.CreateIntCast(expr, ty, false));
    }
    case Operator::BNOT: {
      ScopedExpr scoped_expr = visit(unop.expr);
      return ScopedExpr(b_.CreateNot(scoped_expr.value()));
    }
    case Operator::MINUS: {
      ScopedExpr scoped_expr = visit(unop.expr);
      return ScopedExpr(b_.CreateNeg(scoped_expr.value()));
    }
    case Operator::INCREMENT:
    case Operator::DECREMENT: {
      return createIncDec(unop);
    }
    case Operator::MUL: {
      // When dereferencing a 32-bit integer, only read in 32-bits, etc.
      ScopedExpr scoped_expr = visit(unop.expr);
      auto dst_type = SizedType(type.GetTy(), type.GetSize());
      AllocaInst *dst = b_.CreateAllocaBPF(dst_type, "deref");
      b_.CreateProbeRead(dst, type, scoped_expr.value(), unop.loc);
      Value *value = b_.CreateIntCast(b_.CreateLoad(b_.GetType(dst_type), dst),
                                      b_.getInt64Ty(),
                                      type.IsSigned());
      b_.CreateLifetimeEnd(dst);
      return ScopedExpr(value);
    }
    default:
      LOG(BUG) << "unop_int: invalid op \"" << opstr(unop) << "\"";
      __builtin_unreachable();
  }
}

ScopedExpr CodegenLLVM::unop_ptr(Unop &unop)
{
  const SizedType &type = unop.expr.type();
  switch (unop.op) {
    case Operator::MUL: {
      ScopedExpr scoped_expr = visit(unop.expr);
      if (unop.result_type.IsIntegerTy() || unop.result_type.IsPtrTy()) {
        const auto *et = type.GetPointeeTy();
        AllocaInst *dst = b_.CreateAllocaBPF(*et, "deref");
        b_.CreateProbeRead(
            dst, *et, scoped_expr.value(), unop.loc, type.GetAS());
        Value *value = b_.CreateLoad(b_.GetType(*et), dst);
        b_.CreateLifetimeEnd(dst);
        return ScopedExpr(value);
      }
      return scoped_expr; // Pass as is.
    }
    case Operator::INCREMENT:
    case Operator::DECREMENT:
      return createIncDec(unop);
    default:
      return visit(unop.expr);
  }
}

ScopedExpr CodegenLLVM::visit(Unop &unop)
{
  const SizedType &type = unop.expr.type();
  if (type.IsIntegerTy() || type.IsBoolTy()) {
    return unop_int(unop);
  } else if (type.IsPtrTy() || type.IsCtxAccess()) // allow dereferencing args
  {
    return unop_ptr(unop);
  } else {
    LOG(BUG) << "invalid type (" << type << ") passed to unary operator \""
             << opstr(unop) << "\"";
    __builtin_unreachable();
  }
}

ScopedExpr CodegenLLVM::visit(Ternary &ternary)
{
  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *left_block = BasicBlock::Create(module_->getContext(),
                                              "left",
                                              parent);
  BasicBlock *right_block = BasicBlock::Create(module_->getContext(),
                                               "right",
                                               parent);
  BasicBlock *done = BasicBlock::Create(module_->getContext(), "done", parent);

  // ordering of all the following statements is important
  Value *buf = nullptr;
  if (ternary.result_type.IsStringTy()) {
    buf = b_.CreateGetStrAllocation("buf", ternary.loc);
    const auto max_strlen = bpftrace_.config_->max_strlen;
    b_.CreateMemsetBPF(buf, b_.getInt8(0), max_strlen);
  } else if (!ternary.result_type.IsIntTy() &&
             !ternary.result_type.IsNoneTy()) {
    buf = b_.CreateAllocaBPF(ternary.result_type);
    b_.CreateMemsetBPF(buf, b_.getInt8(0), ternary.result_type.GetSize());
  }

  auto scoped_expr = visit(ternary.cond);
  Value *cond = scoped_expr.value();
  Value *zero_value = Constant::getNullValue(cond->getType());
  b_.CreateCondBr(b_.CreateICmpNE(cond, zero_value, "true_cond"),
                  left_block,
                  right_block);

  if (ternary.result_type.IsIntTy()) {
    // fetch selected integer via CreateStore
    b_.SetInsertPoint(left_block);
    auto scoped_left = visit(ternary.left);
    auto *left_expr = b_.CreateIntCast(scoped_left.value(),
                                       b_.GetType(ternary.result_type),
                                       ternary.result_type.IsSigned());
    b_.CreateBr(done);

    b_.SetInsertPoint(right_block);
    auto scoped_right = visit(ternary.right);
    auto *right_expr = b_.CreateIntCast(scoped_right.value(),
                                        b_.GetType(ternary.result_type),
                                        ternary.result_type.IsSigned());
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
    auto *phi = b_.CreatePHI(b_.GetType(ternary.result_type), 2, "result");
    phi->addIncoming(left_expr, left_block);
    phi->addIncoming(right_expr, right_block);
    return ScopedExpr(phi);
  } else if (ternary.result_type.IsNoneTy()) {
    // Type::none
    b_.SetInsertPoint(left_block);
    visit(ternary.left);
    b_.CreateBr(done);
    b_.SetInsertPoint(right_block);
    visit(ternary.right);
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
    return ScopedExpr();
  } else {
    b_.SetInsertPoint(left_block);
    auto scoped_left = visit(ternary.left);
    if (ternary.result_type.IsTupleTy()) {
      createTupleCopy(
          ternary.left.type(), ternary.result_type, buf, scoped_left.value());
    } else if (needMemcpy(ternary.result_type)) {
      b_.CreateMemcpyBPF(buf,
                         scoped_left.value(),
                         ternary.result_type.GetSize());
    } else {
      b_.CreateStore(scoped_left.value(), buf);
    }
    b_.CreateBr(done);

    b_.SetInsertPoint(right_block);
    auto scoped_right = visit(ternary.right);
    if (ternary.result_type.IsTupleTy()) {
      createTupleCopy(
          ternary.right.type(), ternary.result_type, buf, scoped_right.value());
    } else if (needMemcpy(ternary.result_type)) {
      b_.CreateMemcpyBPF(buf,
                         scoped_right.value(),
                         ternary.result_type.GetSize());
    } else {
      b_.CreateStore(scoped_right.value(), buf);
    }
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
    if (dyn_cast<AllocaInst>(buf))
      return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
    return ScopedExpr(buf);
  }
}

ScopedExpr CodegenLLVM::visit(FieldAccess &acc)
{
  SizedType type = acc.expr.type();
  AddrSpace addrspace = acc.expr.type().GetAS();
  auto scoped_arg = visit(acc.expr);

  assert(type.IsRecordTy());
  bool is_ctx = type.IsCtxAccess();
  bool is_tparg = type.is_tparg;
  bool is_internal = type.is_internal;
  bool is_funcarg = type.is_funcarg;

  if (type.is_funcarg) {
    auto probe_type = probetype(current_attach_point_->provider);
    if (probe_type == ProbeType::fentry || probe_type == ProbeType::fexit ||
        probe_type == ProbeType::rawtracepoint)
      return ScopedExpr(b_.CreateKFuncArg(ctx_, acc.field_type, acc.field),
                        std::move(scoped_arg));
    else if (probe_type == ProbeType::uprobe) {
      llvm::Type *args_type = b_.UprobeArgsType(type);
      return readDatastructElemFromStack(std::move(scoped_arg),
                                         b_.getInt32(
                                             acc.field_type.funcarg_idx),
                                         args_type,
                                         acc.field_type);
    }
  }

  std::string cast_type = is_tparg ? TracepointFormatParser::get_struct_name(
                                         *current_attach_point_)
                                   : type.GetName();

  // This overwrites the stored type!
  type = CreateRecord(cast_type, bpftrace_.structs.Lookup(cast_type));
  if (is_ctx)
    type.MarkCtxAccess();
  type.is_tparg = is_tparg;
  type.is_internal = is_internal;
  type.is_funcarg = is_funcarg;
  // Restore the addrspace info
  // struct MyStruct { const int* a; };  $s = (struct MyStruct *)arg0;  $s->a
  type.SetAS(addrspace);

  const auto &field = type.GetField(acc.field);

  if (inBpfMemory(type)) {
    return readDatastructElemFromStack(
        std::move(scoped_arg), b_.getInt64(field.offset), type, field.type);
  } else {
    // Structs may contain two kinds of fields that must be handled separately
    // (bitfields and _data_loc)
    if (field.type.IsIntTy() &&
        (field.bitfield.has_value() || field.is_data_loc)) {
      if (field.bitfield.has_value()) {
        Value *raw;
        auto *field_type = b_.GetType(field.type);
        if (type.IsCtxAccess()) {
          // The offset is specified in absolute terms here; and the load
          // will implicitly convert to the intended field_type.
          Value *src = b_.CreateSafeGEP(b_.getPtrTy(),
                                        scoped_arg.value(),
                                        b_.getInt64(field.offset));
          raw = b_.CreateLoad(field_type, src, true);
        } else {
          // Since `src` is treated as a offset for a constructed probe read,
          // we are not constrained in the same way.
          Value *src = b_.CreateAdd(scoped_arg.value(),
                                    b_.getInt64(field.offset));
          AllocaInst *dst = b_.CreateAllocaBPF(field.type,
                                               type.GetName() + "." +
                                                   acc.field);
          // memset so verifier doesn't complain about reading uninitialized
          // stack
          b_.CreateMemsetBPF(dst, b_.getInt8(0), field.type.GetSize());
          b_.CreateProbeRead(dst,
                             b_.getInt32(field.bitfield->read_bytes),
                             src,
                             type.GetAS(),
                             acc.loc);
          raw = b_.CreateLoad(field_type, dst);
          b_.CreateLifetimeEnd(dst);
        }
        size_t rshiftbits;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        rshiftbits = field.bitfield->access_rshift;
#else
        rshiftbits = (field.type.GetSize() - field.bitfield->read_bytes) * 8;
        rshiftbits += field.bitfield->access_rshift;
#endif
        Value *shifted = b_.CreateLShr(raw, rshiftbits);
        Value *masked = b_.CreateAnd(shifted, field.bitfield->mask);
        return ScopedExpr(masked);
      } else {
        // `is_data_loc` should only be set if field access is on `args` which
        // has to be a ctx access
        assert(type.IsCtxAccess());
        // Parser needs to have rewritten field to be a u64
        assert(field.type.IsIntTy());
        assert(field.type.GetIntBitWidth() == 64);

        // Top 2 bytes are length (which we'll ignore). Bottom two bytes are
        // offset which we add to the start of the tracepoint struct. We need
        // to wrap the context here in a special way to treat it as the
        // expected pointer type for all versions.
        Value *value = b_.CreateLoad(b_.getInt32Ty(),
                                     b_.CreateSafeGEP(b_.getInt32Ty(),
                                                      ctx_,
                                                      b_.getInt64(field.offset /
                                                                  4)));
        value = b_.CreateIntCast(value, b_.getInt64Ty(), false);
        value = b_.CreateAnd(value, b_.getInt64(0xFFFF));
        value = b_.CreateSafeGEP(b_.getInt8Ty(), ctx_, value);
        return ScopedExpr(value);
      }
    } else {
      return probereadDatastructElem(std::move(scoped_arg),
                                     b_.getInt64(field.offset),
                                     type,
                                     field.type,
                                     acc.loc,
                                     type.GetName() + "." + acc.field);
    }
  }
}

ScopedExpr CodegenLLVM::visit(ArrayAccess &arr)
{
  // Only allow direct reads if the element is also marked as a BTF type; this
  // is specifically because the semantic analyzer has marked these cases to
  // avoid copying through two pointers.
  SizedType type = arr.expr.type();

  // We can allow the lifetime of the index to expire by the time the array
  // expression is complete, but we must preserve the lifetime of the
  // expression since the `readDatstructureElemFromStack` method might end up
  // returning a pointer to live memory produced by the expression.
  auto scoped_expr = visit(arr.expr);
  auto scoped_index = visit(arr.indexpr);

  if (inBpfMemory(arr.element_type) && !type.IsPtrTy())
    return readDatastructElemFromStack(
        std::move(scoped_expr), scoped_index.value(), type, arr.element_type);
  else {
    Value *array = scoped_expr.value();
    if (array->getType()->isPointerTy()) {
      scoped_expr = ScopedExpr(b_.CreatePtrToInt(array, b_.getInt64Ty()),
                               std::move(scoped_expr));
    }

    Value *index = b_.CreateIntCast(scoped_index.value(),
                                    b_.getInt64Ty(),
                                    type.IsSigned());
    Value *offset = b_.CreatePtrOffset(arr.element_type, index, type.GetAS());

    return probereadDatastructElem(std::move(scoped_expr),
                                   offset,
                                   type,
                                   arr.element_type,
                                   arr.loc,
                                   "array_access");
  }
}

ScopedExpr CodegenLLVM::visit(TupleAccess &acc)
{
  const SizedType &type = acc.expr.type();
  auto scoped_arg = visit(acc.expr);
  assert(type.IsTupleTy());

  Value *src = b_.CreateGEP(b_.GetType(type),
                            scoped_arg.value(),
                            { b_.getInt32(0), b_.getInt32(acc.index) });
  SizedType &elem_type = type.GetFields()[acc.index].type;

  if (shouldBeInBpfMemoryAlready(elem_type)) {
    // Extend lifetime of source buffer
    return ScopedExpr(src, std::move(scoped_arg));
  } else {
    // Lifetime is not extended, it is freed after the load
    return ScopedExpr(b_.CreateLoad(b_.GetType(elem_type), src));
  }
}

ScopedExpr CodegenLLVM::visit(MapAccess &acc)
{
  if (named_param_defaults_.defaults.contains(acc.map->ident)) {
    if (acc.map->value_type.IsStringTy()) {
      const auto max_strlen = bpftrace_.config_->max_strlen;
      Value *np_alloc = b_.CreateGetStrAllocation(acc.map->ident, acc.loc);
      b_.CreateMemsetBPF(np_alloc, b_.getInt8(0), max_strlen);
      auto sized_type = bpftrace_.resources.global_vars.get_sized_type(
          acc.map->ident, bpftrace_.resources, *bpftrace_.config_);
      b_.CreateMemcpyBPF(np_alloc,
                         module_->getGlobalVariable(acc.map->ident),
                         sized_type.GetSize());

      return ScopedExpr(np_alloc,
                        [this, np_alloc]() { b_.CreateLifetimeEnd(np_alloc); });
    }

    return ScopedExpr(b_.CreateLoad(acc.map->value_type.IsBoolTy()
                                        ? b_.getInt8Ty()
                                        : b_.getInt64Ty(),
                                    module_->getGlobalVariable(acc.map->ident),
                                    acc.map->ident));
  }

  auto scoped_key = getMapKey(*acc.map, acc.key);

  auto map_info = bpftrace_.resources.maps_info.find(acc.map->ident);
  if (map_info == bpftrace_.resources.maps_info.end()) {
    LOG(BUG) << "map name: \"" << acc.map->ident << "\" not found";
  }

  const auto &val_type = map_info->second.value_type;
  Value *value;
  if (canAggPerCpuMapElems(map_info->second.bpf_type, val_type)) {
    value = b_.CreatePerCpuMapAggElems(
        *acc.map, scoped_key.value(), val_type, acc.loc);
  } else {
    value = b_.CreateMapLookupElem(*acc.map, scoped_key.value(), acc.loc);
  }

  return ScopedExpr(value, [this, value] {
    if (dyn_cast<AllocaInst>(value))
      b_.CreateLifetimeEnd(value);
  });
}

ScopedExpr CodegenLLVM::visit(Cast &cast)
{
  auto scoped_expr = visit(cast.expr);
  if (cast.cast_type.IsIntTy()) {
    auto *int_ty = b_.GetType(cast.cast_type);
    if (cast.expr.type().IsArrayTy()) {
      // we need to read the array into the integer
      Value *array = scoped_expr.value();
      if (cast.expr.type().is_internal || cast.expr.type().IsCtxAccess()) {
        // array is on the stack - just cast the pointer
        if (array->getType()->isIntegerTy())
          array = b_.CreateIntToPtr(array, b_.getPtrTy());
      } else {
        // array is in memory - need to proberead
        auto *buf = b_.CreateAllocaBPF(cast.cast_type);
        b_.CreateProbeRead(
            buf, cast.cast_type, array, cast.loc, cast.expr.type().GetAS());
        array = buf;
      }
      return ScopedExpr(b_.CreateLoad(int_ty, array, true));
    } else {
      return ScopedExpr(
          b_.CreateIntCast(scoped_expr.value(),
                           b_.getIntNTy(cast.cast_type.GetIntBitWidth()),
                           cast.cast_type.IsSigned(),
                           "cast"));
    }
  } else if (cast.cast_type.IsArrayTy() && cast.expr.type().IsIntTy()) {
    // We need to store the cast integer on stack and reinterpret the pointer to
    // it to an array pointer.
    auto *v = b_.CreateAllocaBPF(scoped_expr.value()->getType());
    b_.CreateStore(scoped_expr.value(), v);
    return ScopedExpr(v, [this, v] { b_.CreateLifetimeEnd(v); });
  } else if (cast.cast_type.IsBoolTy()) {
    if (cast.expr.type().IsStringTy()) {
      auto *first_char = b_.CreateGEP(b_.getInt8Ty(),
                                      scoped_expr.value(),
                                      { b_.getInt32(0) });
      Value *cond = b_.CreateICmpNE(b_.CreateLoad(b_.getInt8Ty(), first_char),
                                    b_.getInt8(0),
                                    "bool_cast");
      return ScopedExpr(cond);
    }
    Value *zero_value = Constant::getNullValue(scoped_expr.value()->getType());
    Value *cond = b_.CreateICmpNE(scoped_expr.value(), zero_value, "bool_cast");
    return ScopedExpr(cond);
  } else {
    // FIXME(amscanne): The existing behavior is to simply pass the existing
    // expression back up when it is neither an integer nor an array.
    return scoped_expr;
  }
}

void CodegenLLVM::compareStructure(SizedType &our_type, llvm::Type *llvm_type)
{
  // Validate that what we thought the struct looks like
  // and LLVM made of it are equal to avoid issues.
  //
  // As the size is used throughout the semantic phase for
  // sizing buffers and maps we have to abort if it doesn't
  // match.
  // But offset is only used for printing, so we can recover
  // from that by storing the correct offset.
  //
  size_t our_size = our_type.GetSize();
  size_t llvm_size = datalayout().getTypeAllocSize(llvm_type);

  if (llvm_size != our_size) {
    LOG(BUG) << "Struct size mismatch: expected: " << our_size
             << ", real: " << llvm_size;
  }

  const auto *layout = datalayout().getStructLayout(
      reinterpret_cast<llvm::StructType *>(llvm_type));

  for (ssize_t i = 0; i < our_type.GetFieldCount(); i++) {
    ssize_t llvm_offset = layout->getElementOffset(i);
    auto &field = our_type.GetField(i);
    ssize_t our_offset = field.offset;
    if (llvm_offset != our_offset) {
      LOG(DEBUG) << "Struct offset mismatch for: " << field.type << "(" << i
                 << ")" << ": (llvm) " << llvm_offset << " != " << our_offset;

      field.offset = llvm_offset;
    }
  }
}

// createTuple
//
// Constructs a tuple on the scratch buffer or stack from the provided values.
Value *CodegenLLVM::createTuple(
    const SizedType &tuple_type,
    const std::vector<std::pair<llvm::Value *, Location>> &vals,
    const std::string &name,
    const Location &loc)
{
  auto *tuple_ty = b_.GetType(tuple_type);
  size_t tuple_size = datalayout().getTypeAllocSize(tuple_ty);
  auto *buf = b_.CreateTupleAllocation(tuple_type, name, loc);
  b_.CreateMemsetBPF(buf, b_.getInt8(0), tuple_size);

  for (size_t i = 0; i < vals.size(); ++i) {
    auto [val, vloc] = vals[i];
    SizedType &type = tuple_type.GetField(i).type;

    Value *dst = b_.CreateGEP(tuple_ty,
                              buf,
                              { b_.getInt32(0), b_.getInt32(i) });

    if (inBpfMemory(type))
      b_.CreateMemcpyBPF(dst, val, type.GetSize());
    else if (type.IsArrayTy() || type.IsRecordTy())
      b_.CreateProbeRead(dst, type, val, vloc);
    else
      b_.CreateStore(val, dst);
  }
  return buf;
}

void CodegenLLVM::createTupleCopy(const SizedType &expr_type,
                                  const SizedType &var_type,
                                  Value *dst_val,
                                  Value *src_val)
{
  assert(expr_type.IsTupleTy() && var_type.IsTupleTy());
  auto *array_ty = ArrayType::get(b_.getInt8Ty(), expr_type.GetSize());
  auto *tuple_ty = b_.GetType(var_type);
  for (size_t i = 0; i < expr_type.GetFields().size(); ++i) {
    SizedType &t_type = expr_type.GetField(i).type;
    Value *offset_val = b_.CreateGEP(
        array_ty,
        src_val,
        { b_.getInt64(0), b_.getInt64(expr_type.GetField(i).offset) });
    Value *dst = b_.CreateGEP(tuple_ty,
                              dst_val,
                              { b_.getInt32(0), b_.getInt32(i) });
    if (t_type.IsTupleTy() && !t_type.IsSameSizeRecursive(var_type)) {
      createTupleCopy(t_type, var_type.GetField(i).type, dst, offset_val);
    } else if (t_type.IsIntTy() && t_type.GetSize() < 8) {
      // Integers are always stored as 64-bit in map keys
      b_.CreateStore(b_.CreateIntCast(b_.CreateLoad(b_.GetType(t_type),
                                                    offset_val),
                                      b_.getInt64Ty(),
                                      t_type.IsSigned()),
                     dst);
    } else {
      b_.CreateMemcpyBPF(dst, offset_val, t_type.GetSize());
    }
  }
}

ScopedExpr CodegenLLVM::visit(Tuple &tuple)
{
  llvm::Type *tuple_ty = b_.GetType(tuple.tuple_type);

  compareStructure(tuple.tuple_type, tuple_ty);

  std::vector<std::pair<llvm::Value *, Location>> vals;
  std::vector<ScopedExpr> scoped_exprs;
  vals.reserve(tuple.elems.size());

  for (auto &elem : tuple.elems) {
    auto scoped_expr = visit(elem);
    vals.emplace_back(scoped_expr.value(), tuple.loc);
    scoped_exprs.emplace_back(std::move(scoped_expr));
  }

  auto *buf = createTuple(tuple.tuple_type, vals, "tuple", tuple.loc);
  if (dyn_cast<AllocaInst>(buf))
    return ScopedExpr(buf, [this, buf]() { b_.CreateLifetimeEnd(buf); });
  return ScopedExpr(buf);
}

ScopedExpr CodegenLLVM::visit(ExprStatement &expr)
{
  return visit(expr.expr);
}

ScopedExpr CodegenLLVM::visit(AssignMapStatement &assignment)
{
  auto scoped_expr = visit(assignment.expr);
  auto scoped_key = getMapKey(*assignment.map, assignment.key);
  Value *expr = scoped_expr.value();

  const auto &map_type = assignment.map->type();
  const auto &expr_type = assignment.expr.type();
  const auto self_alloca = needAssignMapStatementAllocation(assignment);
  Value *value = self_alloca
                     ? b_.CreateWriteMapValueAllocation(map_type,
                                                        assignment.map->ident +
                                                            "_val",
                                                        assignment.loc)
                     : expr;
  if (shouldBeInBpfMemoryAlready(expr_type)) {
    if (!expr_type.IsSameSizeRecursive(map_type)) {
      b_.CreateMemsetBPF(value, b_.getInt8(0), map_type.GetSize());
      if (expr_type.IsTupleTy()) {
        createTupleCopy(expr_type, map_type, value, expr);
      } else if (expr_type.IsStringTy()) {
        b_.CreateMemcpyBPF(value, expr, expr_type.GetSize());
      } else {
        LOG(BUG) << "Type size mismatch. Map Type Size: " << map_type.GetSize()
                 << " Expression Type Size: " << expr_type.GetSize();
      }
    }
  } else if (map_type.IsRecordTy() || map_type.IsArrayTy()) {
    if (!expr_type.is_internal) {
      // expr currently contains a pointer to the struct or array
      // We now want to read the entire struct/array in so we can save it
      b_.CreateProbeRead(
          value, map_type, expr, assignment.loc, expr_type.GetAS());
    }
  } else {
    if (assignment.map->type().IsIntTy()) {
      // Integers are always stored as 64-bit in map values
      expr = b_.CreateIntCast(expr, b_.getInt64Ty(), map_type.IsSigned());
    }
    b_.CreateStore(expr, value);
  }
  b_.CreateMapUpdateElem(
      assignment.map->ident, scoped_key.value(), value, assignment.loc);
  if (self_alloca && dyn_cast<AllocaInst>(value))
    b_.CreateLifetimeEnd(value);
  return ScopedExpr();
}

void CodegenLLVM::maybeAllocVariable(const std::string &var_ident,
                                     const SizedType &var_type,
                                     const Location &loc)
{
  if (maybeGetVariable(var_ident) != nullptr) {
    // Already been allocated
    return;
  }

  SizedType alloca_type = var_type;
  // Arrays and structs need not to be copied when assigned to local variables
  // since they are treated as read-only - it is sufficient to assign
  // the pointer and do the memcpy/proberead later when necessary
  if (var_type.IsArrayTy() || var_type.IsRecordTy()) {
    const auto &pointee_type = var_type.IsArrayTy() ? *var_type.GetElementTy()
                                                    : var_type;
    alloca_type = CreatePointer(pointee_type, var_type.GetAS());
  }

  auto *val = b_.CreateVariableAllocationInit(alloca_type, var_ident, loc);
  variables_[scope_stack_.back()][var_ident] = VariableLLVM{
    .value = val, .type = b_.GetType(alloca_type)
  };
}

VariableLLVM *CodegenLLVM::maybeGetVariable(const std::string &var_ident)
{
  for (auto *scope : scope_stack_) {
    if (auto search_val = variables_[scope].find(var_ident);
        search_val != variables_[scope].end()) {
      return &search_val->second;
    }
  }
  return nullptr;
}

VariableLLVM &CodegenLLVM::getVariable(const std::string &var_ident)
{
  auto *variable = maybeGetVariable(var_ident);
  if (!variable) {
    LOG(BUG) << "Can't find variable: " << var_ident
             << " in this or outer scope";
  }
  return *variable;
}

ScopedExpr CodegenLLVM::visit(AssignVarStatement &assignment)
{
  Variable &var = *assignment.var();

  auto scoped_expr = visit(assignment.expr);

  // In order to assign a value to a variable, the expression has to actually
  // produce a value. Unfortunately, there are many expressions which currently
  // do not produce values (and are either valid only the context of a map
  // assignment, or are otherwise useful only in statements). Therefore, we try
  // to provide as much information as possible but generally consider this a
  // bug until it can be resolved.
  if (!scoped_expr.value()) {
    LOG(BUG) << "Expression produced no value for variable: " << var.ident;
    __builtin_unreachable();
  }

  maybeAllocVariable(var.ident, var.var_type, var.loc);

  if (var.var_type.IsArrayTy() || var.var_type.IsRecordTy()) {
    // For arrays and structs, only the pointer is stored. However, this means
    // that we cannot release the underlying memory for any of these types. We
    // just disarm the scoped expression, and therefore never free any of these
    // values; this is a bug that matches existing behavior.
    scoped_expr.disarm();
    b_.CreateStore(b_.CreatePtrToInt(scoped_expr.value(), b_.getInt64Ty()),
                   getVariable(var.ident).value);
  } else if (needMemcpy(var.var_type)) {
    auto *val = getVariable(var.ident).value;
    const auto &expr_type = assignment.expr.type();
    if (!expr_type.IsSameSizeRecursive(var.var_type)) {
      b_.CreateMemsetBPF(val, b_.getInt8(0), var.var_type.GetSize());
      if (var.var_type.IsTupleTy()) {
        createTupleCopy(expr_type, var.var_type, val, scoped_expr.value());
      } else {
        b_.CreateMemcpyBPF(val, scoped_expr.value(), expr_type.GetSize());
      }
    } else {
      b_.CreateMemcpyBPF(val, scoped_expr.value(), expr_type.GetSize());
    }
  } else {
    b_.CreateStore(scoped_expr.value(), getVariable(var.ident).value);
  }
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(VarDeclStatement &decl)
{
  Variable &var = *decl.var;
  if (var.var_type.IsNoneTy()) {
    // unused and has no type
    return ScopedExpr();
  }
  maybeAllocVariable(var.ident, var.var_type, var.loc);
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(If &if_node)
{
  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *if_true = BasicBlock::Create(module_->getContext(),
                                           "if_body",
                                           parent);
  BasicBlock *if_end = BasicBlock::Create(module_->getContext(),
                                          "if_end",
                                          parent);
  BasicBlock *if_else = nullptr;

  auto scoped_cond = visit(if_node.cond);
  auto *cond_expr = scoped_cond.value();
  Value *zero_value = Constant::getNullValue(cond_expr->getType());
  Value *cond = b_.CreateICmpNE(cond_expr, zero_value, "true_cond");

  // 3 possible flows:
  //
  // if condition is true
  //   parent -> if_body -> if_end
  //
  // if condition is false, no else
  //   parent -> if_end
  //
  // if condition is false, with else
  //   parent -> if_else -> if_end
  //
  if (!if_node.else_block->stmts.empty()) {
    // LLVM doesn't accept empty basic block, only create when needed
    if_else = BasicBlock::Create(module_->getContext(), "else_body", parent);
    b_.CreateCondBr(cond, if_true, if_else);
  } else {
    b_.CreateCondBr(cond, if_true, if_end);
  }

  b_.SetInsertPoint(if_true);
  auto scoped_del_if_block = visit(*if_node.if_block);

  b_.CreateBr(if_end);

  b_.SetInsertPoint(if_end);

  if (!if_node.else_block->stmts.empty()) {
    b_.SetInsertPoint(if_else);
    auto scoped_del_else_block = visit(*if_node.else_block);

    b_.CreateBr(if_end);
    b_.SetInsertPoint(if_end);
  }
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(Unroll &unroll)
{
  auto n = unroll.expr.as<Integer>()->value;
  for (uint64_t i = 0; i < n; i++) {
    // Make sure to save/restore async ID state b/c we could be processing
    // the same async calls multiple times.
    auto reset_ids = async_ids_.create_reset_ids();
    auto scoped_del = visit(unroll.block);

    if (i != n - 1)
      reset_ids();
  }
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(Jump &jump)
{
  switch (jump.ident) {
    case JumpType::RETURN:
      // return can be used outside of loops
      if (jump.return_value) {
        auto scoped_return = visit(jump.return_value);
        createRet(scoped_return.value());
      } else
        createRet();
      break;
    case JumpType::BREAK:
      b_.CreateBr(std::get<1>(loops_.back()));
      break;
    case JumpType::CONTINUE:
      b_.CreateBr(std::get<0>(loops_.back()));
      break;
    default:
      LOG(BUG) << "jump: invalid op \"" << opstr(jump) << "\"";
      __builtin_unreachable();
  }

  // LLVM doesn't like having instructions after an unconditional branch (segv)
  // This can be avoided by putting all instructions in a unreachable basicblock
  // which will be optimize out.
  //
  // e.g. in the case of `while (..) { $i++; break; $i++ }` the ir will be:
  //
  // while_body:
  //   ...
  //   br label %while_end
  //
  // while_end:
  //   ...
  //
  // unreach:
  //   $i++
  //   br label %while_cond
  //

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *unreach = BasicBlock::Create(module_->getContext(),
                                           "unreach",
                                           parent);
  b_.SetInsertPoint(unreach);
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(While &while_block)
{
  if (!loop_metadata_)
    loop_metadata_ = createLoopMetadata();

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *while_cond = BasicBlock::Create(module_->getContext(),
                                              "while_cond",
                                              parent);
  BasicBlock *while_body = BasicBlock::Create(module_->getContext(),
                                              "while_body",
                                              parent);
  BasicBlock *while_end = BasicBlock::Create(module_->getContext(),
                                             "while_end",
                                             parent);

  loops_.emplace_back(while_cond, while_end);

  b_.CreateBr(while_cond);

  b_.SetInsertPoint(while_cond);
  auto scoped_cond = visit(while_block.cond);
  auto *cond_expr = scoped_cond.value();
  Value *zero_value = Constant::getNullValue(cond_expr->getType());
  auto *cond = b_.CreateICmpNE(cond_expr, zero_value, "true_cond");
  Instruction *loop_hdr = b_.CreateCondBr(cond, while_body, while_end);
  loop_hdr->setMetadata(LLVMContext::MD_loop, loop_metadata_);

  b_.SetInsertPoint(while_body);
  auto scoped_block = visit(*while_block.block);
  b_.CreateBr(while_cond);

  b_.SetInsertPoint(while_end);
  loops_.pop_back();

  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(For &f)
{
  scope_stack_.push_back(&f);
  std::visit([&](auto *iter) { visit(f, *iter); }, f.iterable.value);
  scope_stack_.pop_back();
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(Predicate &pred)
{
  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *pred_false_block = BasicBlock::Create(module_->getContext(),
                                                    "pred_false",
                                                    parent);
  BasicBlock *pred_true_block = BasicBlock::Create(module_->getContext(),
                                                   "pred_true",
                                                   parent);

  auto scoped_expr = visit(pred.expr);

  // allow unop casts in predicates:
  auto *cast_value = b_.CreateIntCast(scoped_expr.value(),
                                      b_.getInt64Ty(),
                                      false);
  auto *cmp_value = b_.CreateICmpEQ(cast_value, b_.getInt64(0), "predcond");

  b_.CreateCondBr(cmp_value, pred_false_block, pred_true_block);
  b_.SetInsertPoint(pred_false_block);

  createRet();

  b_.SetInsertPoint(pred_true_block);

  return ScopedExpr(cmp_value);
}

ScopedExpr CodegenLLVM::visit(Block &block)
{
  scope_stack_.push_back(&block);
  visit(block.stmts);
  scope_stack_.pop_back();

  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(BlockExpr &block_expr)
{
  scope_stack_.push_back(&block_expr);
  visit(block_expr.stmts);
  ScopedExpr value = visit(block_expr.expr);
  scope_stack_.pop_back();

  return value;
}

void CodegenLLVM::generateProbe(Probe &probe,
                                const std::string &name,
                                FunctionType *func_type,
                                std::optional<int> usdt_location_index,
                                bool dummy)
{
  auto probe_type = probetype(current_attach_point_->provider);
  int index = current_attach_point_->index() ?: probe.index();
  auto func_name = util::get_function_name_for_probe(name,
                                                     index,
                                                     usdt_location_index);
  auto *func = llvm::Function::Create(
      func_type, llvm::Function::ExternalLinkage, func_name, module_.get());
  func->setSection(util::get_section_name(func_name));
  func->addFnAttr(Attribute::NoUnwind);
  scope_ = debug_.createProbeDebugInfo(*func);

  BasicBlock *entry = BasicBlock::Create(module_->getContext(), "entry", func);
  b_.SetInsertPoint(entry);

  // check: do the following 8 lines need to be in the wildcard loop?
  ctx_ = func->arg_begin();

  if (bpftrace_.need_recursion_check_) {
    b_.CreateCheckSetRecursion(current_attach_point_->loc,
                               getReturnValueForProbe(probe_type));
  }

  if (probe.pred)
    visit(*probe.pred);
  variables_.clear();
  auto scoped_block = visit(*probe.block);
  createRet();

  if (dummy) {
    func->eraseFromParent();
    return;
  }

  auto pt = probetype(current_attach_point_->provider);
  if ((pt == ProbeType::watchpoint || pt == ProbeType::asyncwatchpoint) &&
      !current_attach_point_->func.empty()) {
    auto ok = generateWatchpointSetupProbe(
        func_type, name, current_attach_point_->address, index, probe.loc);
    if (!ok) {
      probe.addError() << "unable to setup watchpoint: " << ok.takeError();
    }
  }
}

void CodegenLLVM::add_probe(AttachPoint &ap,
                            Probe &probe,
                            FunctionType *func_type)
{
  current_attach_point_ = &ap;
  probefull_ = ap.name();
  if (probetype(ap.provider) == ProbeType::usdt) {
    auto usdt = usdt_helper_.find(bpftrace_.pid(), ap.target, ap.ns, ap.func);
    if (!usdt.has_value()) {
      ap.addError() << "Failed to find usdt probe: " << probefull_;
    } else
      ap.usdt = *usdt;

    // A "unique" USDT probe can be present in a binary in multiple
    // locations. One case where this happens is if a function
    // containing a USDT probe is inlined into a caller. So we must
    // generate a new program for each instance. We _must_ regenerate
    // because argument locations may differ between instance locations
    // (eg arg0. may not be found in the same offset from the same
    // register in each location)
    auto reset_ids = async_ids_.create_reset_ids();
    current_usdt_location_index_ = 0;
    for (int i = 0; i < ap.usdt.num_locations; ++i) {
      reset_ids();

      generateProbe(probe, probefull_, func_type, i);
      bpftrace_.add_probe(ap,
                          probe,
                          expansions_.get_expansion(ap),
                          expansions_.get_expanded_funcs(ap),
                          i);
      current_usdt_location_index_++;
    }
  } else {
    generateProbe(probe, probefull_, func_type);
    bpftrace_.add_probe(ap,
                        probe,
                        expansions_.get_expansion(ap),
                        expansions_.get_expanded_funcs(ap));
  }
  current_attach_point_ = nullptr;
}

ScopedExpr CodegenLLVM::visit(Subprog &subprog)
{
  scope_stack_.push_back(&subprog);
  std::vector<llvm::Type *> arg_types;
  // First argument is for passing ctx pointer for output, rest are proper
  // arguments to the function
  arg_types.push_back(b_.getPtrTy());
  std::ranges::transform(subprog.args,

                         std::back_inserter(arg_types),
                         [this](SubprogArg *arg) {
                           return b_.GetType(arg->type);
                         });
  FunctionType *func_type = FunctionType::get(b_.GetType(subprog.return_type),
                                              arg_types,
                                              false);

  auto *func = llvm::Function::Create(
      func_type, llvm::Function::InternalLinkage, subprog.name, module_.get());
  BasicBlock *entry = BasicBlock::Create(module_->getContext(), "entry", func);
  b_.SetInsertPoint(entry);

  variables_.clear();
  ctx_ = func->arg_begin();
  inside_subprog_ = true;

  int arg_index = 0;
  for (SubprogArg *arg : subprog.args) {
    auto *alloca = b_.CreateAllocaBPF(b_.GetType(arg->type), arg->name);
    b_.CreateStore(func->getArg(arg_index + 1), alloca);
    variables_[scope_stack_.back()][arg->name] = VariableLLVM{
      .value = alloca, .type = alloca->getAllocatedType()
    };
    ++arg_index;
  }

  for (Statement &stmt : subprog.stmts)
    visit(stmt);
  if (subprog.return_type.IsVoidTy())
    createRet();

  FunctionPassManager fpm;
  FunctionAnalysisManager fam;
  llvm::PassBuilder pb;
  pb.registerFunctionAnalyses(fam);
  fpm.addPass(UnreachableBlockElimPass());
  fpm.run(*func, fam);
  scope_stack_.pop_back();

  return ScopedExpr();
}

void CodegenLLVM::createRet(Value *value)
{
  if (bpftrace_.need_recursion_check_) {
    b_.CreateUnSetRecursion(current_attach_point_->loc);
  }

  // If value is explicitly provided, use it
  if (value) {
    b_.CreateRet(value);
    return;
  } else if (inside_subprog_) {
    b_.CreateRetVoid();
    return;
  }

  int ret_val = getReturnValueForProbe(
      probetype(current_attach_point_->provider));
  b_.CreateRet(b_.getInt64(ret_val));
}

int CodegenLLVM::getReturnValueForProbe(ProbeType probe_type)
{
  // Fall back to default return value
  switch (probe_type) {
    case ProbeType::invalid:
      LOG(BUG) << "Returning from invalid probetype";
      return 0;
    case ProbeType::tracepoint:
      // Classic (ie. *not* raw) tracepoints have a kernel quirk stopping perf
      // subsystem from seeing a tracepoint event if BPF program returns 0.
      // This breaks perf in some situations and generally makes such BPF
      // programs bad citizens. Return 1 instead.
      return 1;
    case ProbeType::special:
    case ProbeType::benchmark:
    case ProbeType::kprobe:
    case ProbeType::kretprobe:
    case ProbeType::uprobe:
    case ProbeType::uretprobe:
    case ProbeType::usdt:
    case ProbeType::profile:
    case ProbeType::interval:
    case ProbeType::software:
    case ProbeType::hardware:
    case ProbeType::watchpoint:
    case ProbeType::asyncwatchpoint:
    case ProbeType::fentry:
    case ProbeType::fexit:
    case ProbeType::iter:
    case ProbeType::rawtracepoint:
      return 0;
  }
  LOG(BUG) << "Unknown probetype";
  return 0;
}

ScopedExpr CodegenLLVM::visit(Probe &probe)
{
  FunctionType *func_type = FunctionType::get(b_.getInt64Ty(),
                                              { b_.getPtrTy() }, // ctx
                                              false);

  // We begin by saving state that gets changed by the codegen pass, so we
  // can restore it for the next pass (printf_id_, time_id_).
  auto reset_ids = async_ids_.create_reset_ids();
  bool generated = false;
  for (auto *attach_point : probe.attach_points) {
    reset_ids();
    current_attach_point_ = attach_point;

    if (attach_point->index() == 0)
      attach_point->set_index(getNextIndexForProbe());

    add_probe(*attach_point, probe, func_type);
    generated = true;
  }
  if (!generated) {
    generateProbe(probe, "dummy", func_type, std::nullopt, true);
  }

  current_attach_point_ = nullptr;
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(Program &program)
{
  for (Subprog *subprog : program.functions)
    visit(subprog);
  for (Probe *probe : program.probes)
    visit(probe);
  return ScopedExpr();
}

int CodegenLLVM::getNextIndexForProbe()
{
  return next_probe_index_++;
}

ScopedExpr CodegenLLVM::getMapKey(Map &map, Expression &key_expr)
{
  const auto alloca_created_here = needMapKeyAllocation(map, key_expr);

  auto scoped_key_expr = visit(key_expr);
  const auto &key_type = map.key_type;
  // Allocation needs to be done after recursing via visit(key_expr) so that we
  // have the expression SSA value.
  Value *key = alloca_created_here
                   ? b_.CreateMapKeyAllocation(key_type,
                                               map.ident + "_key",
                                               map.loc)
                   : scoped_key_expr.value();
  if (inBpfMemory(key_expr.type())) {
    if (!key_expr.type().IsSameSizeRecursive(key_type)) {
      b_.CreateMemsetBPF(key, b_.getInt8(0), key_type.GetSize());
      if (key_expr.type().IsTupleTy()) {
        createTupleCopy(
            key_expr.type(), key_type, key, scoped_key_expr.value());
      } else if (key_expr.type().IsStringTy()) {
        b_.CreateMemcpyBPF(key,
                           scoped_key_expr.value(),
                           key_expr.type().GetSize());
      } else {
        LOG(BUG) << "Type size mismatch. Key Type Size: " << key_type.GetSize()
                 << " Expression Type Size: " << key_expr.type().GetSize();
      }
    } else {
      // Call-ee freed
    }
  } else if (map.key_type.IsIntTy()) {
    // Integers are always stored as 64-bit in map keys
    b_.CreateStore(b_.CreateIntCast(scoped_key_expr.value(),
                                    b_.getInt64Ty(),
                                    key_type.IsSigned()),
                   key);
  } else if (map.key_type.IsBoolTy()) {
    b_.CreateStore(
        b_.CreateIntCast(scoped_key_expr.value(), b_.getInt8Ty(), false), key);
  } else {
    if (key_expr.type().IsArrayTy() || key_expr.type().IsRecordTy()) {
      // We need to read the entire array/struct and save it
      b_.CreateProbeRead(
          key, key_expr.type(), scoped_key_expr.value(), map.loc);
    } else {
      b_.CreateStore(b_.CreateIntCast(scoped_key_expr.value(),
                                      b_.getInt64Ty(),
                                      key_expr.type().IsSigned()),
                     key);
    }
  }
  // Either way we hold on to the original key, to ensure that its lifetime
  // lasts as long as it may be accessed.
  if (alloca_created_here && dyn_cast<AllocaInst>(key)) {
    return ScopedExpr(key, [this, key, k = std::move(scoped_key_expr)] {
      b_.CreateLifetimeEnd(key);
    });
  }
  return ScopedExpr(key, std::move(scoped_key_expr));
}

ScopedExpr CodegenLLVM::getMultiMapKey(Map &map,
                                       Expression &key_expr,
                                       const std::vector<Value *> &extra_keys,
                                       const Location &loc)
{
  auto scoped_expr = visit(key_expr);

  size_t size = map.key_type.GetSize();
  for (auto *extra_key : extra_keys) {
    size += module_->getDataLayout().getTypeAllocSize(extra_key->getType());
  }

  // If key ever changes to not be allocated here, be sure to update getMapKey()
  // as well to take the new lifetime semantics into account.
  auto *key = b_.CreateMapKeyAllocation(CreateArray(size, CreateInt8()),
                                        map.ident + "_key",
                                        loc);
  auto *key_type = ArrayType::get(b_.getInt8Ty(), size);

  int offset = 0;
  bool aligned = true;
  // Construct a map key in the stack
  Value *offset_val = b_.CreateGEP(key_type,
                                   key,
                                   { b_.getInt64(0), b_.getInt64(offset) });
  size_t map_key_size = map.key_type.GetSize();
  size_t expr_size = key_expr.type().GetSize();

  if (inBpfMemory(key_expr.type())) {
    if (!key_expr.type().IsSameSizeRecursive(map.key_type)) {
      b_.CreateMemsetBPF(offset_val, b_.getInt8(0), map_key_size);
      if (key_expr.type().IsTupleTy()) {
        createTupleCopy(
            key_expr.type(), map.key_type, offset_val, scoped_expr.value());
      } else if (key_expr.type().IsStringTy()) {
        b_.CreateMemcpyBPF(offset_val, scoped_expr.value(), expr_size);
      } else {
        LOG(BUG) << "Type size mismatch. Key Type Size: "
                 << map.key_type.GetSize()
                 << " Expression Type Size: " << expr_size;
      }
    } else {
      b_.CreateMemcpyBPF(offset_val, scoped_expr.value(), expr_size);
    }
    if ((map_key_size % 8) != 0)
      aligned = false;
  } else {
    if (key_expr.type().IsArrayTy() || key_expr.type().IsRecordTy()) {
      // Read the array/struct into the key
      b_.CreateProbeRead(
          offset_val, key_expr.type(), scoped_expr.value(), map.loc);
      if ((map_key_size % 8) != 0)
        aligned = false;
    } else {
      // promote map key to 64-bit:
      Value *key_elem = b_.CreateIntCast(scoped_expr.value(),
                                         b_.getInt64Ty(),
                                         key_expr.type().IsSigned());
      if (aligned)
        b_.CreateStore(key_elem, offset_val);
      else
        b_.createAlignedStore(key_elem, offset_val, 1);
    }
  }
  offset += map_key_size;

  for (auto *extra_key : extra_keys) {
    Value *offset_val = b_.CreateGEP(key_type,
                                     key,
                                     { b_.getInt64(0), b_.getInt64(offset) });
    if (aligned)
      b_.CreateStore(extra_key, offset_val);
    else
      b_.createAlignedStore(extra_key, offset_val, 1);
    offset += module_->getDataLayout().getTypeAllocSize(extra_key->getType());
  }

  return ScopedExpr(key, [this, key] { b_.CreateLifetimeEnd(key); });
}

ScopedExpr CodegenLLVM::createLogicalAnd(Binop &binop)
{
  assert(binop.left.type().IsIntTy() || binop.left.type().IsPtrTy() ||
         binop.left.type().IsBoolTy());
  assert(binop.right.type().IsIntTy() || binop.right.type().IsPtrTy() ||
         binop.right.type().IsBoolTy());

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *lhs_true_block = BasicBlock::Create(module_->getContext(),
                                                  "&&_lhs_true",
                                                  parent);
  BasicBlock *true_block = BasicBlock::Create(module_->getContext(),
                                              "&&_true",
                                              parent);
  BasicBlock *false_block = BasicBlock::Create(module_->getContext(),
                                               "&&_false",
                                               parent);
  BasicBlock *merge_block = BasicBlock::Create(module_->getContext(),
                                               "&&_merge",
                                               parent);

  Value *result = b_.CreateAllocaBPF(b_.getInt8Ty(), "&&_result");

  ScopedExpr scoped_lhs = visit(binop.left);
  Value *lhs = scoped_lhs.value();
  Value *lhs_zero_value = Constant::getNullValue(lhs->getType());
  b_.CreateCondBr(b_.CreateICmpNE(lhs, lhs_zero_value, "lhs_true_cond"),
                  lhs_true_block,
                  false_block);

  b_.SetInsertPoint(lhs_true_block);

  ScopedExpr scoped_rhs = visit(binop.right);
  Value *rhs = scoped_rhs.value();
  Value *rhs_zero_value = Constant::getNullValue(rhs->getType());
  b_.CreateCondBr(b_.CreateICmpNE(rhs, rhs_zero_value, "rhs_true_cond"),
                  true_block,
                  false_block);

  b_.SetInsertPoint(true_block);
  b_.CreateStore(b_.getInt8(1), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(false_block);
  b_.CreateStore(b_.getInt8(0), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(merge_block);
  return ScopedExpr(b_.CreateLoad(b_.getInt8Ty(), result));
}

ScopedExpr CodegenLLVM::createLogicalOr(Binop &binop)
{
  assert(binop.left.type().IsIntTy() || binop.left.type().IsPtrTy() ||
         binop.left.type().IsBoolTy());
  assert(binop.right.type().IsIntTy() || binop.right.type().IsPtrTy() ||
         binop.right.type().IsBoolTy());

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *lhs_false_block = BasicBlock::Create(module_->getContext(),
                                                   "||_lhs_false",
                                                   parent);
  BasicBlock *false_block = BasicBlock::Create(module_->getContext(),
                                               "||_false",
                                               parent);
  BasicBlock *true_block = BasicBlock::Create(module_->getContext(),
                                              "||_true",
                                              parent);
  BasicBlock *merge_block = BasicBlock::Create(module_->getContext(),
                                               "||_merge",
                                               parent);

  Value *result = b_.CreateAllocaBPF(b_.getInt8Ty(), "||_result");

  ScopedExpr scoped_lhs = visit(binop.left);
  Value *lhs = scoped_lhs.value();
  Value *lhs_zero_value = Constant::getNullValue(lhs->getType());
  b_.CreateCondBr(b_.CreateICmpNE(lhs, lhs_zero_value, "lhs_true_cond"),
                  true_block,
                  lhs_false_block);

  b_.SetInsertPoint(lhs_false_block);

  ScopedExpr scoped_rhs = visit(binop.right);
  Value *rhs = scoped_rhs.value();
  Value *rhs_zero_value = Constant::getNullValue(rhs->getType());
  b_.CreateCondBr(b_.CreateICmpNE(rhs, rhs_zero_value, "rhs_true_cond"),
                  true_block,
                  false_block);

  b_.SetInsertPoint(false_block);
  b_.CreateStore(b_.getInt8(0), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(true_block);
  b_.CreateStore(b_.getInt8(1), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(merge_block);
  return ScopedExpr(b_.CreateLoad(b_.getInt8Ty(), result));
}

llvm::Function *CodegenLLVM::createLog2Function()
{
  auto ip = b_.saveIP();
  // Arguments: VAL (int64), K (0..5)
  // Maps each power of 2 into N = 2^K buckets, so we can build fine-grained
  // histograms with low runtime cost.
  //
  // Returns:
  //   0               for      VAL < 0
  //   1 + VAL         for 0 <= VAL < 2^K
  //   1 + concat(A,B) for      VAL >= 2^K,
  // where
  //   A is the position of the leftmost "1" in VAL, minus K
  //   B are the K bits following the leftmost "1" in VAL
  //
  // As an example, if VAL = 225 (0b11100001) and K = 2:
  // - the leftmost "1" in VAL is at position 8, so A is 8-2=6 (0b110)
  // - the following bits are "11" so B is 0b11
  // and the returned value is 1 + concat(0b110, 0b11) = 1 + 0b11011 = 28
  //
  // log2(int n, int k)
  // {
  //   if (n < 0) return 0;
  //   mask = (1ul << k) - 1;
  //   if (n <= mask) return n + 1;
  //   n0 = n;
  //   // find leftmost 1
  //   l = 0;
  //   for (int i = 5; i >= 0; i--) {
  //     threshold = 1ul << (1<<i)
  //     shift = (n >= threshold) << i;
  //     n >>= shift;
  //     l += shift;
  //   }
  //   l -= k;
  //   // mask K bits after leftmost 1
  //   x = (n0 >> l) & mask;
  //   return ((l + 1) << k) + x + 1;
  // }

  FunctionType *log2_func_type = FunctionType::get(
      b_.getInt64Ty(), { b_.getInt64Ty(), b_.getInt64Ty() }, false);
  auto *log2_func = llvm::Function::Create(
      log2_func_type, llvm::Function::InternalLinkage, "log2", module_.get());
  log2_func->addFnAttr(Attribute::AlwaysInline);
  log2_func->setSection("helpers");
  log2_func->addFnAttr(Attribute::NoUnwind);
  BasicBlock *entry = BasicBlock::Create(module_->getContext(),
                                         "entry",
                                         log2_func);
  b_.SetInsertPoint(entry);

  // storage for arguments
  Value *n_alloc = b_.CreateAllocaBPF(CreateUInt64());
  b_.CreateStore(log2_func->arg_begin(), n_alloc);
  Value *k_alloc = b_.CreateAllocaBPF(CreateUInt64());
  b_.CreateStore(log2_func->arg_begin() + 1, k_alloc);

  // test for less than zero
  BasicBlock *is_less_than_zero = BasicBlock::Create(module_->getContext(),
                                                     "hist.is_less_than_zero",
                                                     log2_func);
  BasicBlock *is_not_less_than_zero = BasicBlock::Create(
      module_->getContext(), "hist.is_not_less_than_zero", log2_func);

  Value *n = b_.CreateLoad(b_.getInt64Ty(), n_alloc);
  Value *zero = b_.getInt64(0);
  b_.CreateCondBr(b_.CreateICmpSLT(n, zero),
                  is_less_than_zero,
                  is_not_less_than_zero);

  b_.SetInsertPoint(is_less_than_zero);
  createRet(zero);

  b_.SetInsertPoint(is_not_less_than_zero);

  // first set of buckets (<= mask)
  Value *one = b_.getInt64(1);
  Value *k = b_.CreateLoad(b_.getInt64Ty(), k_alloc);
  Value *mask = b_.CreateSub(b_.CreateShl(one, k), one);

  BasicBlock *is_zero = BasicBlock::Create(module_->getContext(),
                                           "hist.is_zero",
                                           log2_func);
  BasicBlock *is_not_zero = BasicBlock::Create(module_->getContext(),
                                               "hist.is_not_zero",
                                               log2_func);
  b_.CreateCondBr(b_.CreateICmpULE(n, mask), is_zero, is_not_zero);

  b_.SetInsertPoint(is_zero);
  createRet(b_.CreateAdd(n, one));

  b_.SetInsertPoint(is_not_zero);

  // index of first bit set in n, 1 means bit 0, guaranteed to be >= k
  Value *l = zero;
  for (int i = 5; i >= 0; i--) {
    Value *threshold = b_.getInt64(1UL << (1UL << i));
    Value *is_ge = b_.CreateICmpSGE(n, threshold);
    // cast is important.
    is_ge = b_.CreateIntCast(is_ge, b_.getInt64Ty(), false);
    Value *shift = b_.CreateShl(is_ge, i);
    n = b_.CreateLShr(n, shift);
    l = b_.CreateAdd(l, shift);
  }

  // see algorithm for next steps:
  // subtract k, so we can move the next k bits of N to position 0
  l = b_.CreateSub(l, k);
  // now find the k bits in n after the first '1'
  Value *x = b_.CreateAnd(
      b_.CreateLShr(b_.CreateLoad(b_.getInt64Ty(), n_alloc), l), mask);

  Value *ret = b_.CreateAdd(l, one);
  ret = b_.CreateShl(ret, k); // make room for the extra slots
  ret = b_.CreateAdd(ret, x);
  ret = b_.CreateAdd(ret, one);
  createRet(ret);

  b_.restoreIP(ip);
  return module_->getFunction("log2");
}

llvm::Function *CodegenLLVM::createLinearFunction()
{
  auto ip = b_.saveIP();
  // lhist() returns a bucket index for the given value. The first and last
  //   bucket indexes are special: they are 0 for the less-than-range
  //   bucket, and index max_bucket+2 for the greater-than-range bucket.
  //   Indexes 1 to max_bucket+1 span the buckets in the range.
  //
  // int lhist(int value, int min, int max, int step)
  // {
  //   int result;
  //
  //   if (value < min)
  //     return 0;
  //   if (value > max)
  //     return 1 + (max - min) / step;
  //   result = 1 + (value - min) / step;
  //
  //   return result;
  // }

  // inlined function initialization
  FunctionType *linear_func_type = FunctionType::get(
      b_.getInt64Ty(),
      { b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty() },
      false);
  auto *linear_func = llvm::Function::Create(linear_func_type,
                                             llvm::Function::InternalLinkage,
                                             "linear",
                                             module_.get());
  linear_func->addFnAttr(Attribute::AlwaysInline);
  linear_func->setSection("helpers");
  linear_func->addFnAttr(Attribute::NoUnwind);
  BasicBlock *entry = BasicBlock::Create(module_->getContext(),
                                         "entry",
                                         linear_func);
  b_.SetInsertPoint(entry);

  // pull in arguments
  Value *value_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *min_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *max_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *step_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *result_alloc = b_.CreateAllocaBPF(CreateUInt64());

  b_.CreateStore(linear_func->arg_begin() + 0, value_alloc);
  b_.CreateStore(linear_func->arg_begin() + 1, min_alloc);
  b_.CreateStore(linear_func->arg_begin() + 2, max_alloc);
  b_.CreateStore(linear_func->arg_begin() + 3, step_alloc);

  Value *cmp = nullptr;

  // algorithm
  {
    Value *min = b_.CreateLoad(b_.getInt64Ty(), min_alloc);
    Value *val = b_.CreateLoad(b_.getInt64Ty(), value_alloc);
    cmp = b_.CreateICmpSLT(val, min);
  }
  BasicBlock *lt_min = BasicBlock::Create(module_->getContext(),
                                          "lhist.lt_min",
                                          linear_func);
  BasicBlock *ge_min = BasicBlock::Create(module_->getContext(),
                                          "lhist.ge_min",
                                          linear_func);
  b_.CreateCondBr(cmp, lt_min, ge_min);

  b_.SetInsertPoint(lt_min);
  createRet(b_.getInt64(0));

  b_.SetInsertPoint(ge_min);
  {
    Value *max = b_.CreateLoad(b_.getInt64Ty(), max_alloc);
    Value *val = b_.CreateLoad(b_.getInt64Ty(), value_alloc);
    cmp = b_.CreateICmpSGT(val, max);
  }
  BasicBlock *le_max = BasicBlock::Create(module_->getContext(),
                                          "lhist.le_max",
                                          linear_func);
  BasicBlock *gt_max = BasicBlock::Create(module_->getContext(),
                                          "lhist.gt_max",
                                          linear_func);
  b_.CreateCondBr(cmp, gt_max, le_max);

  b_.SetInsertPoint(gt_max);
  {
    Value *step = b_.CreateLoad(b_.getInt64Ty(), step_alloc);
    Value *min = b_.CreateLoad(b_.getInt64Ty(), min_alloc);
    Value *max = b_.CreateLoad(b_.getInt64Ty(), max_alloc);
    Value *div = b_.CreateUDiv(b_.CreateSub(max, min), step);
    b_.CreateStore(b_.CreateAdd(div, b_.getInt64(1)), result_alloc);
    createRet(b_.CreateLoad(b_.getInt64Ty(), result_alloc));
  }

  b_.SetInsertPoint(le_max);
  {
    Value *step = b_.CreateLoad(b_.getInt64Ty(), step_alloc);
    Value *min = b_.CreateLoad(b_.getInt64Ty(), min_alloc);
    Value *val = b_.CreateLoad(b_.getInt64Ty(), value_alloc);
    Value *div3 = b_.CreateUDiv(b_.CreateSub(val, min), step);
    b_.CreateStore(b_.CreateAdd(div3, b_.getInt64(1)), result_alloc);
    createRet(b_.CreateLoad(b_.getInt64Ty(), result_alloc));
  }

  b_.restoreIP(ip);
  return module_->getFunction("linear");
}

MDNode *CodegenLLVM::createLoopMetadata()
{
  // Create metadata to disable loop unrolling
  //
  // For legacy reasons, the first item of a loop metadata node must be
  // a self-reference. See https://llvm.org/docs/LangRef.html#llvm-loop
  MDNode *unroll_disable = MDNode::get(
      llvm_ctx_, MDString::get(llvm_ctx_, "llvm.loop.unroll.disable"));
  MDNode *loopid = MDNode::getDistinct(llvm_ctx_,
                                       { unroll_disable, unroll_disable });
  loopid->replaceOperandWith(0, loopid);

  return loopid;
}

void CodegenLLVM::createFormatStringCall(Call &call,
                                         int id,
                                         const std::vector<Field> &call_args,
                                         const std::string &call_name,
                                         async_action::AsyncAction async_action)
{
  std::vector<llvm::Type *> elements;
  for (const Field &arg : call_args) {
    llvm::Type *ty = b_.GetType(arg.type);
    elements.push_back(ty);
  }

  // perf event output has: uint64_t id, vargs
  // The id maps to bpftrace_.*_args_, and is a way to define the
  // types and offsets of each of the arguments, and share that between BPF and
  // user-space for printing.
  std::vector<llvm::Type *> ringbuf_elems = { b_.getInt64Ty() };
  StructType *fmt_struct = nullptr;
  if (!elements.empty()) {
    fmt_struct = StructType::create(elements, call_name + "_args_t", false);
    ringbuf_elems.push_back(fmt_struct);
  }
  StructType *ringbuf_struct = StructType::create(ringbuf_elems,
                                                  call_name + "_t",
                                                  false);

  int struct_size = datalayout().getTypeAllocSize(ringbuf_struct);
  Value *fmt_args = b_.CreateGetFmtStringArgsAllocation(ringbuf_struct,
                                                        call_name + "_args",
                                                        call.loc);
  // The struct is not packed so we need to memset it
  b_.CreateMemsetBPF(fmt_args, b_.getInt8(0), struct_size);

  Value *id_offset = b_.CreateGEP(ringbuf_struct,
                                  fmt_args,
                                  { b_.getInt32(0), b_.getInt32(0) });
  b_.CreateStore(b_.getInt64(id + static_cast<int>(async_action)), id_offset);
  Value *fmt_offset = nullptr;
  if (fmt_struct) {
    fmt_offset = b_.CreateGEP(ringbuf_struct,
                              fmt_args,
                              { b_.getInt32(0), b_.getInt32(1) });
  }

  for (size_t i = 1; i < call.vargs.size(); i++) {
    Expression &arg = call.vargs.at(i);
    auto scoped_arg = visit(arg);
    Value *offset = b_.CreateGEP(fmt_struct,
                                 fmt_offset,
                                 { b_.getInt32(0), b_.getInt32(i - 1) });
    if (needMemcpy(arg.type()))
      b_.CreateMemcpyBPF(offset, scoped_arg.value(), arg.type().GetSize());
    else if (arg.type().IsIntegerTy() && arg.type().GetSize() < 8)
      b_.CreateStore(b_.CreateIntCast(scoped_arg.value(),
                                      b_.getInt64Ty(),
                                      arg.type().IsSigned()),
                     offset);
    else
      b_.CreateStore(scoped_arg.value(), offset);
  }

  b_.CreateOutput(fmt_args, struct_size, call.loc);
  if (dyn_cast<AllocaInst>(fmt_args))
    b_.CreateLifetimeEnd(fmt_args);
}

Result<> CodegenLLVM::generateWatchpointSetupProbe(
    FunctionType *func_type,
    const std::string &expanded_probe_name,
    int arg_num,
    int index,
    const Location &loc)
{
  const auto &arguments = arch::Host::arguments();
  if (static_cast<size_t>(arg_num) >= arguments.size()) {
    return make_error<InternalError>("argument out of range");
  }
  auto offset = arch::Host::register_to_pt_regs_offset(arguments[arg_num]);
  if (!offset) {
    return make_error<InternalError>("invalid register");
  }

  auto func_name = util::get_function_name_for_watchpoint_setup(
      expanded_probe_name, index);
  auto *func = llvm::Function::Create(
      func_type, llvm::Function::ExternalLinkage, func_name, module_.get());
  func->setSection(util::get_section_name(func_name));
  func->addFnAttr(Attribute::NoUnwind);
  debug_.createProbeDebugInfo(*func);

  BasicBlock *entry = BasicBlock::Create(module_->getContext(), "entry", func);
  b_.SetInsertPoint(entry);

  // Send SIGSTOP to curtask
  if (!current_attach_point_->async)
    b_.CreateSignal(b_.getInt32(SIGSTOP), current_attach_point_->loc);

  // Pull out function argument
  Value *ctx = func->arg_begin();
  Value *addr = b_.CreateRegisterRead(ctx,
                                      offset.value(),
                                      "arg" + std::to_string(arg_num));

  // Tell userspace to setup the real watchpoint
  auto elements = AsyncEvent::Watchpoint().asLLVMType(b_);
  StructType *watchpoint_struct = b_.GetStructType("watchpoint_t",
                                                   elements,
                                                   true);
  AllocaInst *buf = b_.CreateAllocaBPF(watchpoint_struct, "watchpoint");
  size_t struct_size = datalayout().getTypeAllocSize(watchpoint_struct);

  // Fill in perf event struct
  b_.CreateStore(
      b_.getInt64(
          static_cast<int64_t>(async_action::AsyncAction::watchpoint_attach)),
      b_.CreateGEP(watchpoint_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));
  b_.CreateStore(
      b_.getInt64(async_ids_.watchpoint()),
      b_.CreateGEP(watchpoint_struct, buf, { b_.getInt64(0), b_.getInt32(1) }));
  b_.CreateStore(
      addr,
      b_.CreateGEP(watchpoint_struct, buf, { b_.getInt64(0), b_.getInt32(2) }));
  b_.CreateOutput(buf, struct_size, loc);
  b_.CreateLifetimeEnd(buf);

  createRet();
  return OK();
}

void CodegenLLVM::createPrintMapCall(Call &call)
{
  auto elements = AsyncEvent::Print().asLLVMType(b_);
  StructType *print_struct = b_.GetStructType(call.func + "_t", elements, true);

  auto &arg = call.vargs.at(0);
  auto &map = *arg.as<Map>();

  AllocaInst *buf = b_.CreateAllocaBPF(print_struct,
                                       call.func + "_" + map.ident);

  // store asyncactionid:
  b_.CreateStore(
      b_.getInt64(static_cast<int64_t>(async_action::AsyncAction::print)),
      b_.CreateGEP(print_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));

  int id = bpftrace_.resources.maps_info.at(map.ident).id;
  if (id == -1) {
    LOG(BUG) << "map id for map \"" << map.ident << "\" not found";
  }
  auto *ident_ptr = b_.CreateGEP(print_struct,
                                 buf,
                                 { b_.getInt64(0), b_.getInt32(1) });
  b_.CreateStore(b_.GetIntSameSize(id, elements.at(1)), ident_ptr);

  // top, div
  // first loops sets the arguments as passed by user. The second one zeros
  // the rest
  size_t arg_idx = 1;
  for (; arg_idx < call.vargs.size(); arg_idx++) {
    auto scoped_arg = visit(call.vargs.at(arg_idx));

    b_.CreateStore(
        b_.CreateIntCast(scoped_arg.value(), elements.at(arg_idx), false),
        b_.CreateGEP(print_struct,
                     buf,
                     { b_.getInt64(0), b_.getInt32(arg_idx + 1) }));
  }

  for (; arg_idx < 3; arg_idx++) {
    b_.CreateStore(b_.GetIntSameSize(0, elements.at(arg_idx)),
                   b_.CreateGEP(print_struct,
                                buf,
                                { b_.getInt64(0), b_.getInt32(arg_idx + 1) }));
  }

  b_.CreateOutput(buf, getStructSize(print_struct), call.loc);
  b_.CreateLifetimeEnd(buf);
}

void CodegenLLVM::createJoinCall(Call &call, int id)
{
  auto &arg0 = call.vargs.front();
  auto scoped_arg = visit(arg0);
  auto addrspace = arg0.type().GetAS();

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *failure_callback = BasicBlock::Create(module_->getContext(),
                                                    "failure_callback",
                                                    parent);
  Value *perfdata = b_.CreateGetJoinMap(failure_callback, call.loc);

  uint32_t content_size = bpftrace_.join_argnum_ * bpftrace_.join_argsize_;

  auto elements = AsyncEvent::Join().asLLVMType(b_, content_size);
  StructType *join_struct = b_.GetStructType("join_t", elements, true);

  Value *join_data = b_.CreateBitCast(perfdata,
                                      PointerType::get(join_struct, 0));

  b_.CreateStore(
      b_.getInt64(static_cast<int>(async_action::AsyncAction::join)),
      b_.CreateGEP(join_struct, join_data, { b_.getInt64(0), b_.getInt32(0) }));

  b_.CreateStore(
      b_.getInt64(id),
      b_.CreateGEP(join_struct, join_data, { b_.getInt64(0), b_.getInt32(1) }));

  Value *content_ptr = b_.CreateGEP(join_struct,
                                    join_data,
                                    { b_.getInt64(0), b_.getInt32(2) });

  SizedType elem_type = CreatePointer(CreateInt8(), addrspace);
  size_t ptr_width = b_.getPointerStorageTy(addrspace)->getIntegerBitWidth();
  assert(b_.GetType(elem_type) == b_.getInt64Ty());

  Value *value = scoped_arg.value();
  AllocaInst *arr = b_.CreateAllocaBPF(b_.getInt64Ty(), call.func + "_r0");

  for (unsigned int i = 0; i < bpftrace_.join_argnum_; i++) {
    if (i > 0) {
      value = b_.CreateAdd(value, b_.getInt64(ptr_width / 8));
    }

    b_.CreateProbeRead(arr, elem_type, value, call.loc);
    Value *str_offset = b_.getInt64(
        static_cast<uint64_t>(i) *
        static_cast<uint64_t>(bpftrace_.join_argsize_));
    Value *str_ptr = b_.CreateGEP(b_.getInt8Ty(), content_ptr, str_offset);

    b_.CreateProbeReadStr(str_ptr,
                          bpftrace_.join_argsize_,
                          b_.CreateLoad(b_.getInt64Ty(), arr),
                          addrspace,
                          call.loc);
  }
  size_t header_size = offsetof(AsyncEvent::Join, content); // action_id +
                                                            // join_id
  size_t total_size = header_size + content_size;
  b_.CreateOutput(perfdata, total_size, call.loc);

  b_.CreateBr(failure_callback);
  b_.SetInsertPoint(failure_callback);
}

void CodegenLLVM::createPrintNonMapCall(Call &call, int id)
{
  auto &arg = call.vargs.at(0);
  auto scoped_arg = visit(arg);
  Value *value = scoped_arg.value();

  auto elements = AsyncEvent::PrintNonMap().asLLVMType(b_,
                                                       arg.type().GetSize());
  std::ostringstream struct_name;
  struct_name << call.func << "_" << arg.type().GetTy() << "_"
              << arg.type().GetSize() << "_t";
  StructType *print_struct = b_.GetStructType(struct_name.str(),
                                              elements,
                                              true);
  Value *buf = b_.CreateGetFmtStringArgsAllocation(print_struct,
                                                   struct_name.str(),
                                                   call.loc);
  size_t struct_size = datalayout().getTypeAllocSize(print_struct);

  // Store asyncactionid:
  b_.CreateStore(
      b_.getInt64(
          static_cast<int64_t>(async_action::AsyncAction::print_non_map)),
      b_.CreateGEP(print_struct, buf, { b_.getInt64(0), b_.getInt32(0) }));

  // Store print id
  b_.CreateStore(
      b_.getInt64(id),
      b_.CreateGEP(print_struct, buf, { b_.getInt64(0), b_.getInt32(1) }));

  // Store content
  Value *content_offset = b_.CreateGEP(print_struct,
                                       buf,
                                       { b_.getInt32(0), b_.getInt32(2) });
  b_.CreateMemsetBPF(content_offset, b_.getInt8(0), arg.type().GetSize());
  if (needMemcpy(arg.type())) {
    if (inBpfMemory(arg.type()))
      b_.CreateMemcpyBPF(content_offset, value, arg.type().GetSize());
    else
      b_.CreateProbeRead(content_offset, arg.type(), value, call.loc);
  } else {
    b_.CreateStore(value, content_offset);
  }

  b_.CreateOutput(buf, struct_size, call.loc);
  if (dyn_cast<AllocaInst>(buf))
    b_.CreateLifetimeEnd(buf);
}

void CodegenLLVM::createMapDefinition(const std::string &name,
                                      libbpf::bpf_map_type map_type,
                                      uint64_t max_entries,
                                      const SizedType &key_type,
                                      const SizedType &value_type)
{
  DIType *di_key_type = debug_.GetMapKeyType(key_type, value_type, map_type);
  map_types_.emplace(name, map_type);
  auto var_name = bpf_map_name(name);
  auto *debuginfo = debug_.createMapEntry(
      var_name, map_type, max_entries, di_key_type, value_type);

  // It's sufficient that the global variable has the correct size (struct with
  // one pointer per field). The actual inner types are defined in debug info.
  SmallVector<llvm::Type *, 4> elems = { b_.getPtrTy(), b_.getPtrTy() };
  if (!value_type.IsNoneTy()) {
    elems.push_back(b_.getPtrTy());
    elems.push_back(b_.getPtrTy());
  }
  auto *type = StructType::create(elems, "struct map_internal_repr_t", false);

  auto *var = llvm::dyn_cast<GlobalVariable>(
      module_->getOrInsertGlobal(var_name, type));
  var->setInitializer(ConstantAggregateZero::get(type));
  var->setSection(".maps");
  var->setDSOLocal(true);
  var->addDebugInfo(debuginfo);
}

// Emit maps in libbpf format so that Clang can create BTF info for them which
// can be read and used by libbpf.
//
// Each map should be defined by a global variable of a struct type with the
// following fields:
// - "type"        map type (e.g. BPF_MAP_TYPE_HASH)
// - "max_entries" maximum number of entries
// - "key"         key type
// - "value"       value type
//
// "type" and "max_entries" are integers but they must be represented as
// pointers to an array of ints whose dimension defines the specified value.
//
// "key" and "value" are pointers to the corresponding types. Note that these
// are not used for the BPF_MAP_TYPE_RINGBUF map type.
//
// The most important part is to generate BTF with the above information. This
// is done by emitting DWARF which LLVM will convert into BTF. The LLVM type of
// the global variable itself is not important, it can simply be a struct with 4
// pointers.
//
// Note that LLVM will generate BTF which misses some information. This is
// normally set by libbpf's linker but since we load BTF directly, we must do
// the fixing ourselves, until we start loading BPF programs via bpf_object.
// See BpfBytecode::fixupBTF for details.
void CodegenLLVM::generate_maps(const RequiredResources &required_resources,
                                const CodegenResources &codegen_resources)
{
  // User-defined maps
  for (const auto &[name, info] : required_resources.maps_info) {
    const auto &val_type = info.value_type;
    const auto &key_type = info.key_type;
    createMapDefinition(
        name, info.bpf_type, info.max_entries, key_type, val_type);
  }

  // bpftrace internal maps

  uint16_t max_stack_limit = 0;
  for (const StackType &stack_type : codegen_resources.stackid_maps) {
    createMapDefinition(stack_type.name(),
                        libbpf::BPF_MAP_TYPE_LRU_HASH,
                        128 << 10,
                        CreateArray(16, CreateInt8()),
                        CreateArray(stack_type.limit, CreateUInt64()));
    max_stack_limit = std::max(stack_type.limit, max_stack_limit);
  }

  if (max_stack_limit > 0) {
    createMapDefinition(StackType::scratch_name(),
                        libbpf::BPF_MAP_TYPE_PERCPU_ARRAY,
                        1,
                        CreateUInt32(),
                        CreateArray(max_stack_limit, CreateUInt64()));
  }

  if (codegen_resources.needs_join_map) {
    auto value_size = offsetof(AsyncEvent::Join, content) +
                      (bpftrace_.join_argnum_ * bpftrace_.join_argsize_);
    SizedType value_type = CreateArray(value_size, CreateInt8());
    createMapDefinition(to_string(MapType::Join),
                        libbpf::BPF_MAP_TYPE_PERCPU_ARRAY,
                        1,
                        CreateUInt32(),
                        value_type);
  }

  if (codegen_resources.needs_elapsed_map) {
    createMapDefinition(to_string(MapType::Elapsed),
                        libbpf::BPF_MAP_TYPE_HASH,
                        1,
                        CreateUInt64(),
                        CreateUInt64());
  }

  if (bpftrace_.need_recursion_check_) {
    createMapDefinition(to_string(MapType::RecursionPrevention),
                        libbpf::BPF_MAP_TYPE_PERCPU_ARRAY,
                        1,
                        CreateUInt32(),
                        CreateUInt64());
  }

  if (required_resources.using_skboutput) {
    createMapDefinition(to_string(MapType::PerfEvent),
                        libbpf::BPF_MAP_TYPE_PERF_EVENT_ARRAY,
                        util::get_online_cpus().size(),
                        CreateInt32(),
                        CreateInt32());
  }

  auto entries = bpftrace_.config_->perf_rb_pages * 4096;
  createMapDefinition(to_string(MapType::Ringbuf),
                      libbpf::BPF_MAP_TYPE_RINGBUF,
                      entries,
                      CreateNone(),
                      CreateNone());
}

void CodegenLLVM::generate_global_vars(
    const RequiredResources &resources,
    const ::bpftrace::Config &bpftrace_config)
{
  for (const auto &[name, config] : resources.global_vars.global_var_map()) {
    auto sized_type = resources.global_vars.get_sized_type(name,
                                                           resources,
                                                           bpftrace_config);
    auto *var = llvm::dyn_cast<GlobalVariable>(
        module_->getOrInsertGlobal(name, b_.GetType(sized_type)));
    var->setInitializer(Constant::getNullValue(b_.GetType(sized_type)));
    var->setConstant(config.section == globalvars::RO_SECTION_NAME);
    var->setSection(config.section);
    var->setExternallyInitialized(true);
    var->setDSOLocal(true);
    var->addDebugInfo(debug_.createGlobalVariable(name, sized_type));
  }
}

// Read a single element from a compound data structure (i.e. an array or
// a struct) that has been pulled onto BPF stack.
// Params:
//   src_data   pointer to the entire data structure
//   index      index of the field to read
//   data_type  type of the structure
//   elem_type  type of the element
//   scoped_del scope deleter for the data structure
ScopedExpr CodegenLLVM::readDatastructElemFromStack(ScopedExpr &&scoped_src,
                                                    Value *index,
                                                    llvm::Type *data_type,
                                                    const SizedType &elem_type)
{
  // src_data should contain a pointer to the data structure, but it may be
  // internally represented as an integer and then we need to cast it
  Value *src_data = scoped_src.value();
  if (src_data->getType()->isIntegerTy())
    src_data = b_.CreateIntToPtr(src_data, b_.getPtrTy());

  Value *src = b_.CreateGEP(data_type, src_data, { b_.getInt32(0), index });

  if (elem_type.IsIntegerTy() || elem_type.IsPtrTy() || elem_type.IsBoolTy()) {
    // Load the correct type from src
    return ScopedExpr(
        b_.CreateDatastructElemLoad(elem_type, src, true, elem_type.GetAS()));
  } else {
    // The inner type is an aggregate - instead of copying it, just pass
    // the pointer and extend lifetime of the source data.
    return ScopedExpr(src, std::move(scoped_src));
  }
}

ScopedExpr CodegenLLVM::readDatastructElemFromStack(ScopedExpr &&scoped_src,
                                                    Value *index,
                                                    const SizedType &data_type,
                                                    const SizedType &elem_type)
{
  return readDatastructElemFromStack(
      std::move(scoped_src), index, b_.GetType(data_type), elem_type);
}

// Read a single element from a compound data structure (i.e. an array or
// a struct) that has not been yet pulled into BPF memory.
// Params:
//   scoped_src scoped expression pointing to the data structure
//   offset     offset of the requested element from the structure beginning
//   data_type  type of the data structure
//   elem_type  type of the requested element
//   loc        location of the element access (for proberead)
//   temp_name  name of a temporary variable, if the function creates any
ScopedExpr CodegenLLVM::probereadDatastructElem(ScopedExpr &&scoped_src,
                                                Value *offset,
                                                const SizedType &data_type,
                                                const SizedType &elem_type,
                                                const Location &loc,
                                                const std::string &temp_name)
{
  // We treat this access as a raw byte offset, but may then subsequently need
  // to cast the pointer to the expected value.
  Value *src = b_.CreateSafeGEP(b_.getInt8Ty(), scoped_src.value(), offset);

  if (elem_type.IsRecordTy() || elem_type.IsArrayTy()) {
    // For nested arrays and structs, just pass the pointer along and
    // dereference it later when necessary. We just need to extend lifetime
    // of the source pointer.
    return ScopedExpr(src, std::move(scoped_src));
  } else if (elem_type.IsStringTy() || elem_type.IsBufferTy()) {
    AllocaInst *dst = b_.CreateAllocaBPF(elem_type, temp_name);
    if (elem_type.IsStringTy() && data_type.is_internal) {
      if (src->getType()->isIntegerTy())
        src = b_.CreateIntToPtr(src, dst->getType());
      b_.CreateMemcpyBPF(dst, src, elem_type.GetSize());
    } else {
      b_.CreateProbeRead(dst, elem_type, src, loc, data_type.GetAS());
    }
    // dst is left as is, so we need to return and bound its lifetime to the
    // underlying expression. Since we've finished copying, we can end the
    // lifetime of the `scoped_src` argument.
    return ScopedExpr(dst, [this, dst]() { b_.CreateLifetimeEnd(dst); });
  } else {
    // Read data onto stack
    if (data_type.IsCtxAccess()) {
      // Types have already been suitably casted; just do the access.
      Value *expr = b_.CreateDatastructElemLoad(
          elem_type, src, true, data_type.GetAS());
      // check context access for iter probes (required by kernel)
      if (data_type.IsCtxAccess() &&
          probetype(current_attach_point_->provider) == ProbeType::iter) {
        llvm::Function *parent = b_.GetInsertBlock()->getParent();
        BasicBlock *pred_false_block = BasicBlock::Create(module_->getContext(),
                                                          "pred_false",
                                                          parent);
        BasicBlock *pred_true_block = BasicBlock::Create(module_->getContext(),
                                                         "pred_true",
                                                         parent);
        Value *cast = b_.CreateIntCast(expr, b_.getInt64Ty(), false);
        Value *cmp = b_.CreateICmpEQ(cast, b_.getInt64(0), "predcond");

        b_.CreateCondBr(cmp, pred_false_block, pred_true_block);
        b_.SetInsertPoint(pred_false_block);
        createRet();

        b_.SetInsertPoint(pred_true_block);
      }
      // Everything should be loaded by this point, so we can drop the lifetime
      // of `scoped_src`.
      return ScopedExpr(expr);

    } else {
      AllocaInst *dst = b_.CreateAllocaBPF(elem_type, temp_name);
      b_.CreateProbeRead(dst, elem_type, src, loc, data_type.GetAS());
      Value *expr = b_.CreateLoad(b_.GetType(elem_type), dst);
      // We have completely loaded from dst, and therefore can insert an end to
      // its lifetime directly.
      b_.CreateLifetimeEnd(dst);
      return ScopedExpr(expr);
    }
  }
}

ScopedExpr CodegenLLVM::createIncDec(Unop &unop)
{
  bool is_increment = unop.op == Operator::INCREMENT;
  const SizedType &type = unop.expr.type();
  uint64_t step = type.IsPtrTy() ? type.GetPointeeTy()->GetSize() : 1;

  if (auto *acc = unop.expr.as<MapAccess>()) {
    auto &map = *acc->map;
    auto scoped_key = getMapKey(map, acc->key);
    Value *oldval = b_.CreateMapLookupElem(map, scoped_key.value(), unop.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.value_type,
                                            map.ident + "_newval");
    if (is_increment)
      b_.CreateStore(b_.CreateAdd(oldval, b_.GetIntSameSize(step, oldval)),
                     newval);
    else
      b_.CreateStore(b_.CreateSub(oldval, b_.GetIntSameSize(step, oldval)),
                     newval);
    b_.CreateMapUpdateElem(map.ident, scoped_key.value(), newval, unop.loc);

    Value *value;
    if (unop.is_post_op)
      value = oldval;
    else
      value = b_.CreateLoad(b_.GetType(map.value_type), newval);
    b_.CreateLifetimeEnd(newval);
    return ScopedExpr(value);
  } else if (auto *var = unop.expr.as<Variable>()) {
    const auto &variable = getVariable(var->ident);
    Value *oldval = b_.CreateLoad(variable.type, variable.value);
    Value *newval;
    if (is_increment)
      newval = b_.CreateAdd(oldval, b_.GetIntSameSize(step, oldval));
    else
      newval = b_.CreateSub(oldval, b_.GetIntSameSize(step, oldval));
    b_.CreateStore(newval, variable.value);

    if (unop.is_post_op)
      return ScopedExpr(oldval);
    else
      return ScopedExpr(newval);
  } else {
    LOG(BUG) << "invalid expression passed to " << opstr(unop);
    __builtin_unreachable();
  }
}

llvm::Function *CodegenLLVM::createMurmurHash2Func()
{
  // The goal is to produce the following code:
  //
  // uint64_t murmur_hash_2(void *stack, uint8_t nr_stack_frames, uint64_t seed)
  // {
  //   const uint64_t m = 0xc6a4a7935bd1e995LLU;
  //   const int r = 47;
  //   uint64_t id = seed ^ (nr_stack_frames * m);
  //   int i = 0;

  //   while(i < nr_stack_frames) {
  //     uint64_t k = stack[i];
  //     k *= m;
  //     k ^= k >> r;
  //     k *= m;
  //     id ^= k;
  //     id *= m;
  //     ++i;
  //   }
  //   return id;
  // }
  //
  // https://github.com/aappleby/smhasher/blob/92cf3702fcfaadc84eb7bef59825a23e0cd84f56/src/MurmurHash2.cpp
  auto saved_ip = b_.saveIP();

  std::array<llvm::Type *, 3> args = { b_.getPtrTy(),
                                       b_.getInt8Ty(),
                                       b_.getInt64Ty() };
  FunctionType *callback_type = FunctionType::get(b_.getInt64Ty(), args, false);

  auto *callback = llvm::Function::Create(
      callback_type,
      llvm::Function::LinkageTypes::InternalLinkage,
      "murmur_hash_2",
      module_.get());
  callback->addFnAttr(Attribute::AlwaysInline);
  callback->setSection("helpers");
  callback->addFnAttr(Attribute::NoUnwind);

  auto *bb = BasicBlock::Create(module_->getContext(), "entry", callback);
  b_.SetInsertPoint(bb);

  AllocaInst *nr_stack_frames = b_.CreateAllocaBPF(b_.getInt8Ty(),
                                                   "nr_stack_frames_addr");
  AllocaInst *seed_addr = b_.CreateAllocaBPF(b_.getInt64Ty(), "seed_addr");

  AllocaInst *id = b_.CreateAllocaBPF(b_.getInt64Ty(), "id");
  AllocaInst *i = b_.CreateAllocaBPF(b_.getInt8Ty(), "i");
  AllocaInst *k = b_.CreateAllocaBPF(b_.getInt64Ty(), "k");

  Value *m = b_.getInt64(0xc6a4a7935bd1e995LLU);
  Value *r = b_.getInt64(47);

  Value *stack_addr = callback->getArg(0);

  b_.CreateStore(callback->getArg(1), nr_stack_frames);
  b_.CreateStore(callback->getArg(2), seed_addr);

  // uint64_t id = seed ^ (len * m);
  b_.CreateStore(
      b_.CreateXor(b_.CreateLoad(b_.getInt64Ty(), seed_addr),
                   b_.CreateMul(b_.CreateIntCast(b_.CreateLoad(b_.getInt8Ty(),
                                                               nr_stack_frames),
                                                 b_.getInt64Ty(),
                                                 false),
                                m)),
      id);

  // int i = 0;
  b_.CreateStore(b_.getInt8(0), i);

  llvm::Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *while_cond = BasicBlock::Create(module_->getContext(),
                                              "while_cond",
                                              parent);
  BasicBlock *while_body = BasicBlock::Create(module_->getContext(),
                                              "while_body",
                                              parent);
  BasicBlock *while_end = BasicBlock::Create(module_->getContext(),
                                             "while_end",
                                             parent);
  b_.CreateBr(while_cond);
  b_.SetInsertPoint(while_cond);
  auto *cond = b_.CreateICmp(CmpInst::ICMP_ULT,
                             b_.CreateLoad(b_.getInt8Ty(), i),
                             b_.CreateLoad(b_.getInt8Ty(), nr_stack_frames),
                             "length.cmp");
  b_.CreateCondBr(cond, while_body, while_end);

  b_.SetInsertPoint(while_body);

  // uint64_t k = stack[i];
  Value *stack_ptr = b_.CreateGEP(b_.getInt64Ty(),
                                  stack_addr,
                                  b_.CreateLoad(b_.getInt8Ty(), i));
  b_.CreateStore(b_.CreateLoad(b_.getInt64Ty(), stack_ptr), k);

  // k *= m;
  b_.CreateStore(b_.CreateMul(b_.CreateLoad(b_.getInt64Ty(), k), m), k);

  // // k ^= k >> r
  b_.CreateStore(b_.CreateXor(b_.CreateLoad(b_.getInt64Ty(), k),
                              b_.CreateLShr(b_.CreateLoad(b_.getInt64Ty(), k),
                                            r)),
                 k);

  // // k *= m
  b_.CreateStore(b_.CreateMul(b_.CreateLoad(b_.getInt64Ty(), k), m), k);

  // id ^= k
  b_.CreateStore(b_.CreateXor(b_.CreateLoad(b_.getInt64Ty(), id),
                              b_.CreateLoad(b_.getInt64Ty(), k)),
                 id);

  // id *= m
  b_.CreateStore(b_.CreateMul(b_.CreateLoad(b_.getInt64Ty(), id), m), id);

  // ++i
  b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt8Ty(), i), b_.getInt8(1)),
                 i);

  b_.CreateBr(while_cond);
  b_.SetInsertPoint(while_end);

  b_.CreateLifetimeEnd(nr_stack_frames);
  b_.CreateLifetimeEnd(seed_addr);
  b_.CreateLifetimeEnd(i);
  b_.CreateLifetimeEnd(k);

  // We reserve 0 for errors so if we happen to hash to 0 just set to 1.
  // This should reduce hash collisions as we now have to come across two stacks
  // that naturally hash to 1 AND 0.
  BasicBlock *if_zero = BasicBlock::Create(module_->getContext(),
                                           "if_zero",
                                           parent);
  BasicBlock *if_end = BasicBlock::Create(module_->getContext(),
                                          "if_end",
                                          parent);

  Value *zero_cond = b_.CreateICmpEQ(b_.CreateLoad(b_.getInt64Ty(), id),
                                     b_.getInt64(0),
                                     "zero_cond");

  b_.CreateCondBr(zero_cond, if_zero, if_end);
  b_.SetInsertPoint(if_zero);
  b_.CreateStore(b_.getInt64(1), id);

  b_.CreateBr(if_end);
  b_.SetInsertPoint(if_end);

  Value *ret = b_.CreateLoad(b_.getInt64Ty(), id);

  b_.CreateLifetimeEnd(id);

  b_.CreateRet(ret);

  b_.restoreIP(saved_ip);

  return callback;
}

llvm::Function *CodegenLLVM::createMapLenCallback()
{
  // The goal is to produce the following code:
  //
  // static int cb(struct map *map, void *key, void *value, void *ctx)
  // {
  //   return 0;
  // }
  auto saved_ip = b_.saveIP();

  std::array<llvm::Type *, 4> args = {
    b_.getPtrTy(), b_.getPtrTy(), b_.getPtrTy(), b_.getPtrTy()
  };

  FunctionType *callback_type = FunctionType::get(b_.getInt64Ty(), args, false);

  auto *callback = llvm::Function::Create(
      callback_type,
      llvm::Function::LinkageTypes::InternalLinkage,
      "map_len_cb",
      module_.get());

  callback->setDSOLocal(true);
  callback->setVisibility(llvm::GlobalValue::DefaultVisibility);
  callback->setSection(".text");
  callback->addFnAttr(Attribute::NoUnwind);

  Struct debug_args;
  debug_args.AddField("map", CreatePointer(CreateInt8()));
  debug_args.AddField("key", CreatePointer(CreateInt8()));
  debug_args.AddField("value", CreatePointer(CreateInt8()));
  debug_args.AddField("ctx", CreatePointer(CreateInt8()));
  debug_.createFunctionDebugInfo(*callback, CreateInt64(), debug_args);

  auto *bb = BasicBlock::Create(module_->getContext(), "", callback);
  b_.SetInsertPoint(bb);

  b_.CreateRet(b_.getInt64(0));

  b_.restoreIP(saved_ip);

  return callback;
}

std::pair<llvm::Type *, llvm::Value *> CodegenLLVM::createForContext(
    const For &f,
    std::vector<llvm::Type *> &&extra_fields)
{
  const auto &ctx_fields = f.ctx_type.GetFields();
  std::vector<llvm::Type *> ctx_field_types(ctx_fields.size(), b_.getPtrTy());

  // Add all the extra fields.
  std::ranges::move(extra_fields, std::back_inserter(ctx_field_types));

  // Pack pointers to variables into context struct for use in the callback. If
  // there are no fields, then the underlying codegen helper will simply pass
  // null as the context value. We should not allocate an empty struct.
  llvm::Type *ctx_t = nullptr;
  Value *ctx = nullptr;
  if (!ctx_field_types.empty()) {
    ctx_t = StructType::create(ctx_field_types, "ctx_t");
    ctx = b_.CreateAllocaBPF(ctx_t, "ctx");
    for (size_t i = 0; i < ctx_fields.size(); i++) {
      const auto &field = ctx_fields[i];
      auto *field_expr = getVariable(field.name).value;
      auto *ctx_field_ptr = b_.CreateSafeGEP(
          ctx_t, ctx, { b_.getInt64(0), b_.getInt32(i) }, "ctx." + field.name);
      b_.CreateStore(field_expr, ctx_field_ptr);
    }
  }

  return { ctx_t, ctx };
}

llvm::Function *CodegenLLVM::createForCallback(
    For &f,
    const std::string &name,
    ArrayRef<llvm::Type *> args,
    const Struct &debug_args,
    llvm::Type *ctx_t,
    std::function<llvm::Value *(llvm::Function *)> decl)
{
  auto saved_ip = b_.saveIP();

  // All callbacks in BPF will be generated with a standard integer return.
  FunctionType *callback_type = FunctionType::get(b_.getInt64Ty(), args, false);
  auto *callback = llvm::Function::Create(
      callback_type,
      llvm::Function::LinkageTypes::InternalLinkage,
      name,
      module_.get());
  callback->setDSOLocal(true);
  callback->setVisibility(llvm::GlobalValue::DefaultVisibility);
  callback->setSection(".text");
  callback->addFnAttr(Attribute::NoUnwind);

  // Add the debug information.
  debug_.createFunctionDebugInfo(*callback, CreateInt64(), debug_args);

  // Start our basic function block.
  auto *for_body = BasicBlock::Create(module_->getContext(),
                                      "for_body",
                                      callback);
  auto *for_continue = BasicBlock::Create(module_->getContext(),
                                          "for_continue",
                                          callback);
  auto *for_break = BasicBlock::Create(module_->getContext(),
                                       "for_break",
                                       callback);
  b_.SetInsertPoint(for_body);

  // Extract our context type and value. As noted, this requires that some
  // member of `debug_args` is named `ctx`.
  size_t ctx_index = 0;
  while (ctx_index < debug_args.fields.size()) {
    if (debug_args.fields[ctx_index].name == "ctx") {
      break;
    }
    ctx_index++;
  }
  assert(ctx_index < debug_args.fields.size());
  llvm::Value *ctx = callback->getArg(ctx_index);

  // Generate the variable declaration.
  variables_[scope_stack_.back()][f.decl->ident] = VariableLLVM{
    .value = decl(callback), .type = b_.GetType(f.decl->type())
  };

  // 1. Save original locations of variables which will form part of the
  //    callback context
  // 2. Replace variable expressions with those from the context
  const auto &ctx_fields = f.ctx_type.GetFields();
  std::unordered_map<std::string, Value *> orig_ctx_vars;
  for (size_t i = 0; i < ctx_fields.size(); i++) {
    const auto &field = ctx_fields[i];
    orig_ctx_vars[field.name] = getVariable(field.name).value;

    auto *ctx_field_ptr = b_.CreateGEP(
        ctx_t, ctx, { b_.getInt64(0), b_.getInt32(i) }, "ctx." + field.name);
    getVariable(field.name).value = b_.CreateLoad(b_.getPtrTy(),
                                                  ctx_field_ptr,
                                                  field.name);
  }

  // Generate code for the loop body.
  loops_.emplace_back(for_continue, for_break);
  visit(f.stmts);
  b_.CreateBr(for_continue);
  loops_.pop_back();
  b_.SetInsertPoint(for_continue);
  b_.CreateRet(b_.getInt64(0));
  b_.SetInsertPoint(for_break);
  b_.CreateRet(b_.getInt64(1));

  // Restore original non-context variables.
  for (const auto &[ident, expr] : orig_ctx_vars) {
    getVariable(ident).value = expr;
  }

  // Decl variable is not valid beyond this for loop.
  variables_[scope_stack_.back()].erase(f.decl->ident);

  b_.restoreIP(saved_ip);
  return callback;
}

ScopedExpr CodegenLLVM::visit(For &f, Range &range)
{
  // Evaluate our starting and endpoint values.
  auto start = visit(range.start);
  auto end = visit(range.end);
  Value *iters = b_.CreateBinOp(Instruction::Sub, end.value(), start.value());

  // Construct the context and callback with extra fields add to the context,
  // which track the starting value and the current value of the iteration.
  auto [ctx_t, ctx] = createForContext(f, { b_.getInt64Ty(), b_.getInt64Ty() });
  const auto sz = f.ctx_type.GetFields().size();
  b_.CreateStore(start.value(),
                 b_.CreateSafeGEP(ctx_t,
                                  ctx,
                                  { b_.getInt64(0), b_.getInt32(sz) },
                                  "ctx.start"));

  // Create a callback function suitable for passing to bpf_loop, for the form:
  //
  //   static int cb(uint64_t index, void *ctx)
  //   {
  //     $x = index+prefix;
  //     [stmts...]
  //   }
  std::array<llvm::Type *, 2> args = { b_.getInt64Ty(), b_.getPtrTy() };
  Struct debug_args;
  debug_args.AddField("index", CreateInt64());
  debug_args.AddField("ctx", CreatePointer(CreateInt8()));

  auto *cb = createForCallback(
      f, "loop_cb", args, debug_args, ctx_t, [&](llvm::Function *callback) {
        // See above. Prior to the callback, we push the starting value into the
        // context as an extra field, and reserve space for the current value
        // there. This must be set on each iteration, and provides a declaration
        // that points there.
        auto *ctx = callback->getArg(1);
        auto *start_field_ptr = b_.CreateSafeGEP(
            ctx_t, ctx, { b_.getInt64(0), b_.getInt32(sz) }, "start");
        auto *current_field_ptr = b_.CreateSafeGEP(
            ctx_t, ctx, { b_.getInt64(0), b_.getInt32(sz + 1) }, "current");

        // Add the current iteration count to our starting count, reseting the
        // value of the current variable in the context. The starting value is
        // not available to the user, simply the value of the current iteration.
        b_.CreateStore(b_.CreateAdd(b_.CreateLoad(b_.getInt64Ty(),
                                                  start_field_ptr),
                                    callback->getArg(0)),
                       current_field_ptr);
        return current_field_ptr;
      });

  // Execute the loop.
  b_.CreateForRange(iters, cb, ctx, f.loc);
  return ScopedExpr();
}

ScopedExpr CodegenLLVM::visit(For &f, Map &map)
{
  // Construct our context types.
  auto [ctx_t, ctx] = createForContext(f);

  // Create a callback function suitable for passing to bpf_for_each_map_elem,
  // of the form:
  //
  //   static int cb(struct map *map, void *key, void *value, void *ctx)
  //   {
  //     $decl = (key, value);
  //     [stmts...]
  //   }
  std::array<llvm::Type *, 4> args = {
    b_.getPtrTy(), b_.getPtrTy(), b_.getPtrTy(), b_.getPtrTy()
  };
  Struct debug_args;
  debug_args.AddField("map", CreatePointer(CreateInt8()));
  debug_args.AddField("key", CreatePointer(CreateInt8()));
  debug_args.AddField("value", CreatePointer(CreateInt8()));
  debug_args.AddField("ctx", CreatePointer(CreateInt8()));

  const std::string name = "map_for_each_cb";
  auto *cb = createForCallback(
      f, name, args, debug_args, ctx_t, [&](llvm::Function *callback) {
        auto &key_type = f.decl->type().GetField(0).type;
        Value *key = callback->getArg(1);
        if (!inBpfMemory(key_type)) {
          key = b_.CreateLoad(b_.GetType(key_type), key, "key");
        }

        auto map_info = bpftrace_.resources.maps_info.find(map.ident);
        if (map_info == bpftrace_.resources.maps_info.end()) {
          LOG(BUG) << "map name: \"" << map.ident << "\" not found";
        }

        auto &val_type = f.decl->type().GetField(1).type;
        Value *val = callback->getArg(2);

        const auto &map_val_type = map_info->second.value_type;
        if (canAggPerCpuMapElems(map_info->second.bpf_type, map_val_type)) {
          val = b_.CreatePerCpuMapAggElems(
              map, callback->getArg(1), map_val_type, f.loc);
        } else if (!inBpfMemory(val_type)) {
          val = b_.CreateLoad(b_.GetType(val_type), val, "val");
        }

        return createTuple(f.decl->type(),
                           { { key, f.decl->loc }, { val, f.decl->loc } },
                           f.decl->ident,
                           f.decl->loc);
      });

  // Invoke via the helper.
  b_.CreateForEachMapElem(map, cb, ctx, f.loc);
  return ScopedExpr();
}

bool CodegenLLVM::canAggPerCpuMapElems(const libbpf::bpf_map_type map_type,
                                       const SizedType &val_type)
{
  return val_type.IsCastableMapTy() &&
         (map_type == libbpf::BPF_MAP_TYPE_PERCPU_ARRAY ||
          map_type == libbpf::BPF_MAP_TYPE_PERCPU_HASH);
}

// BPF helpers that use fmt strings (bpf_trace_printk, bpf_seq_printf) expect
// the string passed in a data map. libbpf is able to create the map internally
// if an internal global constant string is used. This function creates the
// constant. Uses bpf_print_id_ to pick the correct format string from
// RequiredResources.
Value *CodegenLLVM::createFmtString(int print_id)
{
  const auto &s = bpftrace_.resources.bpf_print_fmts.at(print_id).str();
  auto *res = llvm::dyn_cast<GlobalVariable>(
      module_->getOrInsertGlobal("__fmt_" + std::to_string(print_id),
                                 ArrayType::get(b_.getInt8Ty(), s.size() + 1)));
  res->setConstant(true);
  res->setInitializer(ConstantDataArray::getString(module_->getContext(), s));
  res->setAlignment(MaybeAlign(1));
  res->setLinkage(llvm::GlobalValue::InternalLinkage);
  return res;
}

/// This should emit
///
///    declare !dbg !... extern_weak ... @func_name(...) section ".ksyms"
///
/// with proper debug info entry.
///
/// The function type is retrieved from kernel BTF.
///
/// If the function declaration is already in the module, just return it.
///
llvm::Function *CodegenLLVM::DeclareKernelFunc(Kfunc kfunc, Node &call)
{
  const std::string &func_name = kfunc_name(kfunc);
  if (auto *fun = module_->getFunction(func_name))
    return fun;

  std::string err;
  auto func_struct = bpftrace_.btf_->resolve_args(
      func_name, true, false, false, err);
  if (!func_struct) {
    call.addError() << "Unknown kernel function: " << func_name;
    return nullptr;
  }

  Struct debug_args;
  std::vector<llvm::Type *> args;
  for (auto &field : func_struct->fields) {
    if (field.name != RETVAL_FIELD_NAME) {
      args.push_back(b_.GetType(field.type, false));
      debug_args.AddField(field.name,
                          field.type,
                          field.offset,
                          field.bitfield,
                          field.is_data_loc);
    }
  }

  FunctionType *func_type = FunctionType::get(
      b_.GetType(func_struct->GetField(RETVAL_FIELD_NAME).type, false),
      args,
      false);

  auto *fun = llvm::Function::Create(func_type,
                                     llvm::GlobalValue::ExternalWeakLinkage,
                                     func_name,
                                     module_.get());
  fun->setSection(".ksyms");
  fun->setUnnamedAddr(GlobalValue::UnnamedAddr::Local);
  fun->addFnAttr(Attribute::NoUnwind);

  debug_.createFunctionDebugInfo(
      *fun, func_struct->GetField(RETVAL_FIELD_NAME).type, debug_args, true);

  return fun;
}

CallInst *CodegenLLVM::CreateKernelFuncCall(Kfunc kfunc,
                                            ArrayRef<Value *> args,
                                            const Twine &name,
                                            Node &call)
{
  auto *func = DeclareKernelFunc(kfunc, call);
  return b_.createCall(func->getFunctionType(), func, args, name);
}

/// This should emit
///
///    declare !dbg !... extern ... @var_name(...) section ".ksyms"
///
/// with proper debug info entry.
///
/// The function type is retrieved from kernel BTF.
///
/// If the function declaration is already in the module, just return it.
///
GlobalVariable *CodegenLLVM::DeclareKernelVar(const std::string &var_name)
{
  if (auto *sym = module_->getGlobalVariable(var_name))
    return sym;

  std::string err;
  auto type = bpftrace_.btf_->get_var_type(var_name);
  assert(!type.IsNoneTy()); // already checked in semantic analyser

  auto *var = llvm::dyn_cast<GlobalVariable>(
      module_->getOrInsertGlobal(var_name, b_.GetType(type)));
  var->setSection(".ksyms");
  var->setLinkage(llvm::GlobalValue::ExternalLinkage);

  auto *var_debug = debug_.createGlobalVariable(var_name, type);
  var->addDebugInfo(var_debug);

  return var;
}

std::unique_ptr<llvm::Module> CodegenLLVM::compile()
{
  CodegenResourceAnalyser analyser(*bpftrace_.config_);
  auto codegen_resources = analyser.analyse(*ast_.root);
  generate_maps(bpftrace_.resources, codegen_resources);
  generate_global_vars(bpftrace_.resources, *bpftrace_.config_);
  {
    visit(ast_.root);
  }
  debug_.finalize();
  return std::move(module_);
}

Pass CreateLLVMInitPass()
{
  LLVMInitializeBPFTargetInfo();
  LLVMInitializeBPFTarget();
  LLVMInitializeBPFTargetMC();
  LLVMInitializeBPFAsmPrinter();
  return Pass::create("llvm-init", [] { return CompileContext(); });
}

Pass CreateCompilePass(
    std::optional<std::reference_wrapper<USDTHelper>> &&usdt_helper)
{
  return Pass::create("compile",
                      [usdt_helper](ASTContext &ast,
                                    BPFtrace &bpftrace,
                                    CDefinitions &c_definitions,
                                    NamedParamDefaults &named_param_defaults,
                                    CompileContext &ctx,
                                    ExpansionResult &expansions) mutable {
                        USDTHelper default_usdt;
                        if (!usdt_helper) {
                          usdt_helper = std::ref(default_usdt);
                        }
                        CodegenLLVM llvm(ast,
                                         bpftrace,
                                         c_definitions,
                                         named_param_defaults,
                                         *ctx.context,
                                         usdt_helper->get(),
                                         expansions);
                        return CompiledModule(llvm.compile());
                      });
}

Pass CreateLinkBitcodePass()
{
  return Pass::create(
      "LinkBitcode", [](BitcodeModules &bm, CompiledModule &cm) -> Result<> {
        for (auto &mod : bm.modules) {
          // Make a copy of the module, to ensure this is not modifying the
          // original. The link step must consume the module below.
          auto copy = llvm::CloneModule(*mod);

          // Modify to ensure everything is inlined. Note that this is also
          // marking all these functions for below, which will adjust their
          // linkage.
          //
          // We also want to ensure that we remove any attributes that prevent
          // any subsequent inline (such as "OptimizeNone"), and suitably tag
          // these functions are "NoUnwind", like the rest. There is no
          // unwinding in BPF, and these attributes are just ensuring that.
          for (auto &fn : copy->functions()) {
            if (fn.isDSOLocal() && !fn.isIntrinsic()) {
              fn.removeFnAttr(Attribute::NoInline);
              fn.removeFnAttr(Attribute::OptimizeNone);
              fn.addFnAttr(Attribute::AlwaysInline);
              fn.addFnAttr(Attribute::NoUnwind);
            }
          }

          // Link into the original source module, consume the new one. This
          // function returns `false` on success.  Hopefully this path is
          // unlikely to cause errors, since it seems the information available
          // is sparse.
          auto err = Linker::linkModules(*cm.module,
                                         std::move(copy),
                                         Linker::LinkOnlyNeeded);
          if (err) {
            return make_error<LinkError>("error during LLVM linking", EINVAL);
          }
        }

        return OK();
      });
}

Pass CreateVerifyPass()
{
  return Pass::create("verify", [](ASTContext &ast, CompiledModule &cm) {
    std::stringstream ss;
    raw_os_ostream OS(ss);
    bool ret = llvm::verifyModule(*cm.module, &OS);
    OS.flush();
    if (ret) {
      ast.root->addError() << ss.str();
    }
  });
}

Pass CreateOptimizePass()
{
  return Pass::create("optimize", [](CompiledModule &cm) {
    PipelineTuningOptions pto;
    pto.LoopUnrolling = false;
    pto.LoopInterleaving = false;
    pto.LoopVectorization = false;
    pto.SLPVectorization = false;

    llvm::PassBuilder pb(getTargetMachine(), pto);

    // ModuleAnalysisManager must be destroyed first.
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    // Register all the basic analyses with the managers.
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(
        llvm::OptimizationLevel::O3);
    mpm.run(*cm.module, mam);
  });
}

Pass CreateDumpIRPass(std::ostream &out)
{
  return Pass::create("dump-ir", [&out](CompiledModule &cm) {
    raw_os_ostream os(out);
    cm.module->print(os, nullptr, false, true);
    os.flush();
    out.flush();
  });
}

Pass CreateObjectPass()
{
  return Pass::create("object", [](CompiledModule &cm) {
    SmallVector<char, 0> output;
    raw_svector_ostream os(output);

    legacy::PassManager PM;
#if LLVM_VERSION_MAJOR >= 18
    auto type = CodeGenFileType::ObjectFile;
#else
    auto type = llvm::CGFT_ObjectFile;
#endif
    if (getTargetMachine()->addPassesToEmitFile(PM, os, nullptr, type))
      LOG(BUG) << "Cannot emit a file of this type";
    PM.run(*cm.module);
    return BpfObject(output);
  });
}

Pass CreateDumpASMPass([[maybe_unused]] std::ostream &out)
{
  return Pass::create("dump-asm", [](BpfObject &bpf) {
    // Technically we could use LLVM APIs to do a proper disassemble on
    // the in-memory ELF file. But that is quite complex, as LLVM only
    // provides fairly low level APIs to do this.
    //
    // Since disassembly is a debugging tool, just shell out to llvm-objdump
    // to keep things simple.
    std::cout << "\nDisassembled bytecode\n";
    std::cout << "---------------------------\n";

    FILE *objdump = ::popen("llvm-objdump -d -", "w");
    if (!objdump) {
      LOG(ERROR) << "Failed to spawn llvm-objdump: " << strerror(errno);
      return;
    }

    if (::fwrite(bpf.data.data(), sizeof(char), bpf.data.size(), objdump) !=
        bpf.data.size()) {
      LOG(ERROR) << "Failed to write ELF to llvm-objdump";
      return;
    }

    if (auto rc = ::pclose(objdump)) {
      LOG(WARNING) << "llvm-objdump did not exit cleanly: status " << rc;
    }
  });
}

} // namespace bpftrace::ast
