#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <fcntl.h>
#include <fstream>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <sstream>
#include <sys/mman.h>

#if LLVM_VERSION_MAJOR <= 16
#include <clang/Basic/DebugInfoOptions.h>
#endif

#include "arch/arch.h"
#include "ast/ast.h"
#include "ast/passes/clang_build.h"
#include "ast/passes/codegen_llvm.h"
#include "ast/passes/resolve_imports.h"
#include "bpftrace.h"
#include "stdlib/stdlib.h"
#include "util/result.h"

namespace bpftrace::ast {

char ClangBuildError::ID;

void ClangBuildError::log(llvm::raw_ostream &OS) const
{
  OS << msg_;
}

namespace {

class MemFds {
public:
  MemFds(int fd) : fd_(fd), path_("/dev/fd/" + std::to_string(fd)) {};
  MemFds(MemFds &&other) : fd_(other.fd_), path_(other.path_)
  {
    other.fd_ = -1;
  }
  MemFds(const MemFds &other) = delete;
  ~MemFds()
  {
    close_fd();
  }

  const std::string &write_file()
  {
    return path_;
  }

  std::string read_all()
  {
    // Seek to the beginning of the file, and read the entire contents
    // into a single string that can be returned. We stat the file up
    // front in order to avoid excessive chunking.
    struct stat st;
    if (fstat(fd_, &st) < 0) {
      return "";
    }
    std::string result;
    result.resize(st.st_size);
    if (read(fd_, result.data(), st.st_size) < 0) {
      return "";
    }
    return result;
  }

  void close_fd()
  {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_ = -1;
  std::string path_;
};

Result<MemFds> create_memfd()
{
  int fd = memfd_create("memfd", MFD_CLOEXEC);
  if (fd < 0) {
    return make_error<ClangBuildError>("failed to create memfd");
  }
  return MemFds(fd);
}

} // namespace

static Result<> build(CompileContext &ctx,
                      const std::string &name,
                      LoadedObject &obj,
                      const llvm::MemoryBufferRef &vmlinux_h,
                      Imports &imports,
                      BitcodeModules &result)
{
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> vfs(
      new llvm::vfs::InMemoryFileSystem());
  vfs->addFile(name,
               0,
               llvm::MemoryBuffer::getMemBufferCopy(obj.data(), "main"));

  const std::string asm_dir = "include/asm/" + arch::Host::asm_arch() + "/";
  for (const auto &[name, other] : stdlib::Stdlib::files) {
    // If this include file is an arch-specific assembly file, then we
    // skip if it does not match the current architecture. If it *does*
    // match the current architecture, then we remap it as the `asm`
    // directory (without the arch prefix).
    if (name.starts_with("include/asm/")) {
      if (!name.starts_with(asm_dir)) {
        continue; // Not our architecture.
      }
      // Replace the arch-specific path with just the asm path.
      std::string nn = "include/asm/" + name.substr(asm_dir.size());
      vfs->addFile(nn, 0, llvm::MemoryBuffer::getMemBufferCopy(other, nn));
    } else {
      vfs->addFile(name, 0, llvm::MemoryBuffer::getMemBufferCopy(other, name));
    }
  }
  for (auto &[name, other] : imports.c_headers) {
    vfs->addFile(name,
                 0,
                 llvm::MemoryBuffer::getMemBufferCopy(other.data(), name));
  }
  vfs->addFileNoOwn("include/vmlinux.h", 0, vmlinux_h);

  // Create the diagnostic options and client. We emit the error to
  // a string, which we can then capture and associate with the import.
  std::string errstr;
  llvm::raw_string_ostream err(errstr);
  auto diagOpts = llvm::makeIntrusiveRefCnt<clang::DiagnosticOptions>();
  auto diags = std::make_unique<clang::DiagnosticsEngine>(
      llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(),
      diagOpts,
      new clang::TextDiagnosticPrinter(err, diagOpts.get()));

  // We create a temporary memfd that we can use to store the output,
  // since the ClangDriver API is framed in terms of filenames. Perhaps
  // we could use the internals here, but that carries other risks.
  auto memfd = create_memfd();
  if (!memfd) {
    return memfd.takeError();
  }

  // Create the compiler invocation. Note that the `-O2` introduces some passes
  // that seem to be load-bearing with respect to generating useful debug
  // information, for some reason. The generated module will be linked and
  // optimized again regardless, but it is better safe than sorry.
  std::vector<const char *> args;
  args.push_back("-O2");
  args.push_back("-Iinclude");
  for (const auto &s : arch::Host::c_defs()) {
    args.push_back("-D");
    args.push_back(s.c_str());
  }
  args.push_back("-o");
  args.push_back(memfd->write_file().c_str());
  args.push_back(name.c_str());

  // Configure the instance. We want to read the source file named
  // by `name` above, enable debug information and optimization.
  auto inv = std::make_shared<clang::CompilerInvocation>();
  clang::CompilerInvocation::CreateFromArgs(*inv,
                                            llvm::ArrayRef<const char *>(args),
                                            *diags);
  inv->getTargetOpts().Triple = "bpf";
#if LLVM_VERSION_MAJOR <= 16
  inv->getCodeGenOpts().setDebugInfo(clang::codegenoptions::FullDebugInfo);
#else
  inv->getCodeGenOpts().setDebugInfo(llvm::codegenoptions::FullDebugInfo);
#endif
  inv->getCodeGenOpts().DebugColumnInfo = true;

  clang::CompilerInstance ci;
  ci.setInvocation(inv);
  ci.setDiagnostics(diags.release());
  ci.setFileManager(new clang::FileManager(clang::FileSystemOptions(), vfs));
  ci.createSourceManager(ci.getFileManager());

  // Generate the object file, which should include the required BTF
  // debug information. This also generates the module as a
  // side-effect, which is what we actually extract for linking.
  std::unique_ptr<clang::CodeGenAction> action =
      std::make_unique<clang::EmitObjAction>(ctx.context.get());
  if (!ci.ExecuteAction(*action)) {
    // This is likely a build failure, we can surface this directly
    // into the user context. We first highlight the location of the
    // original import, then include the C message as a "hint".
    auto &e = obj.node.addError();
    e << "failed to build";
    e.addHint() << errstr;
    return OK();
  }
  if (!errstr.empty()) {
    // If the compilation didn't fail, then these weren't errors but we
    // can surface them as compilation warnings.
    auto &e = obj.node.addWarning();
    e << "found external warnings";
    e.addHint() << errstr;
  }
  std::unique_ptr<llvm::Module> mod = action->takeModule();
  if (!mod) {
    // This is an internal error, not suitable to surface as a user
    // diagnostic. Surface it directly as an error in the pipeline.
    return make_error<ClangBuildError>("failed to generate module");
  }
  result.modules.emplace_back(std::move(mod));
  result.objects.emplace_back(memfd->read_all());
  return OK();
}

ast::Pass CreateClangBuildPass()
{
  return ast::Pass::create(
      "ClangBuilder",
      [](BPFtrace &bpftrace,
         CompileContext &ctx,
         ast::Imports &imports) -> Result<BitcodeModules> {
        BitcodeModules result;

        // Nothing to do? Return directly.
        if (imports.c_sources.empty()) {
          return result;
        }

        // Construct our kernel headers. This is a rather expensive operation,
        // so we ensure that we do this only once for all files.
        std::string vmlinux_h = bpftrace.btf_->c_def();

        // For each of the source files in the imports, we
        // build it and turn it into a bitcode file.
        for (auto &[name, obj] : imports.c_sources) {
          auto ok = build(ctx,
                          name,
                          obj,
                          llvm::MemoryBufferRef(llvm::StringRef(vmlinux_h),
                                                "vmlinux.h"),
                          imports,
                          result);
          if (!ok) {
            return ok.takeError();
          }
        }

        return result;
      });
}

} // namespace bpftrace::ast
