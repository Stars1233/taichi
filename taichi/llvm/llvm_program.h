#pragma once
#include "taichi/system/snode_tree_buffer_manager.h"
#include "taichi/inc/constants.h"
#include "taichi/program/compile_config.h"
#include "taichi/common/logging.h"
#include "taichi/llvm/llvm_context.h"
#include "taichi/runtime/runtime.h"
#include "taichi/system/threading.h"
#include "llvm/IR/Module.h"
#include "taichi/struct/struct.h"
#include "taichi/struct/struct_llvm.h"
#include "taichi/program/snode_expr_utils.h"
#include "taichi/system/memory_pool.h"

#include <memory>

namespace taichi {
namespace lang {
class StructCompiler;

class LlvmProgramImpl {
 public:
  void *llvm_runtime{nullptr};
  std::unique_ptr<TaichiLLVMContext> llvm_context_host{nullptr};
  std::unique_ptr<TaichiLLVMContext> llvm_context_device{nullptr};
  std::unique_ptr<SNodeTreeBufferManager> snode_tree_buffer_manager{nullptr};
  std::unique_ptr<Runtime> runtime_mem_info{nullptr};
  std::unique_ptr<ThreadPool> thread_pool{nullptr};
  void *preallocated_device_buffer{
      nullptr};  // TODO: move this to memory allocator
  CompileConfig config;

  LlvmProgramImpl(CompileConfig &config);

  void maybe_initialize_cuda_llvm_context();

  TaichiLLVMContext *get_llvm_context(Arch arch) {
    if (arch_is_cpu(arch)) {
      return llvm_context_host.get();
    } else {
      return llvm_context_device.get();
    }
  }

  std::unique_ptr<llvm::Module> clone_struct_compiler_initial_context(
      const std::vector<std::unique_ptr<SNodeTree>> &snode_trees_,
      TaichiLLVMContext *tlctx);

  /**
   * Initializes the SNodes for LLVM based backends.
   */
  void initialize_llvm_runtime_snodes(const SNodeTree *tree,
                                      StructCompiler *scomp,
                                      uint64 *result_buffer);

  void materialize_snode_tree(
      SNodeTree *tree,
      std::vector<std::unique_ptr<SNodeTree>> &snode_trees_,
      std::unordered_map<int, SNode *> &snodes,
      SNodeGlobalVarExprMap &snode_to_glb_var_exprs_,
      uint64 *result_buffer);

  /**
   * Sets the attributes of the Exprs that are backed by SNodes.
   */
  void materialize_snode_expr_attributes(
      SNodeGlobalVarExprMap &snode_to_glb_var_exprs_);

  uint64 fetch_result_uint64(int i, uint64 *);

  template <typename T>
  T fetch_result(int i, uint64 *result_buffer) {
    return taichi_union_cast_with_different_sizes<T>(
        fetch_result_uint64(i, result_buffer));
  }

  template <typename T, typename... Args>
  T runtime_query(const std::string &key, uint64 *result_buffer, Args... args) {
    TI_ASSERT(arch_uses_llvm(config.arch));

    TaichiLLVMContext *tlctx = nullptr;
    if (llvm_context_device) {
      tlctx = llvm_context_device.get();
    } else {
      tlctx = llvm_context_host.get();
    }

    auto runtime = tlctx->runtime_jit_module;
    runtime->call<void *, Args...>("runtime_" + key, llvm_runtime,
                                   std::forward<Args>(args)...);
    return taichi_union_cast_with_different_sizes<T>(fetch_result_uint64(
        taichi_result_buffer_runtime_query_id, result_buffer));
  }

  /**
   * Initializes the runtime system for LLVM based backends.
   */
  void initialize_llvm_runtime_system(MemoryPool *memory_pool,
                                      KernelProfilerBase *profiler,
                                      uint64 **result_buffer);

  std::size_t get_snode_num_dynamically_allocated(SNode *snode,
                                                  uint64 *result_buffer);

  void print_list_manager_info(void *list_manager, uint64 *result_buffer);
  void print_memory_profiler_info(
      std::vector<std::unique_ptr<SNodeTree>> &snode_trees_,
      uint64 *result_buffer);

  void device_synchronize();

 private:
};
}  // namespace lang
}  // namespace taichi
