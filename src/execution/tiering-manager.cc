// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/tiering-manager.h"

#include "src/base/platform/platform.h"
#include "src/baseline/baseline-batch-compiler.h"
#include "src/baseline/baseline.h"
#include "src/codegen/assembler.h"
#include "src/codegen/compilation-cache.h"
#include "src/codegen/compiler.h"
#include "src/codegen/pending-optimization-table.h"
#include "src/diagnostics/code-tracer.h"
#include "src/execution/execution.h"
#include "src/execution/frames-inl.h"
#include "src/handles/global-handles.h"
#include "src/init/bootstrapper.h"
#include "src/interpreter/interpreter.h"
#include "src/tracing/trace-event.h"

namespace v8 {
namespace internal {

// Maximum size in bytes of generate code for a function to allow OSR.
static const int kOSRBytecodeSizeAllowanceBase = 119;
static const int kOSRBytecodeSizeAllowancePerTick = 44;

#define OPTIMIZATION_REASON_LIST(V)   \
  V(DoNotOptimize, "do not optimize") \
  V(HotAndStable, "hot and stable")   \
  V(SmallFunction, "small function")

enum class OptimizationReason : uint8_t {
#define OPTIMIZATION_REASON_CONSTANTS(Constant, message) k##Constant,
  OPTIMIZATION_REASON_LIST(OPTIMIZATION_REASON_CONSTANTS)
#undef OPTIMIZATION_REASON_CONSTANTS
};

char const* OptimizationReasonToString(OptimizationReason reason) {
  static char const* reasons[] = {
#define OPTIMIZATION_REASON_TEXTS(Constant, message) message,
      OPTIMIZATION_REASON_LIST(OPTIMIZATION_REASON_TEXTS)
#undef OPTIMIZATION_REASON_TEXTS
  };
  size_t const index = static_cast<size_t>(reason);
  DCHECK_LT(index, arraysize(reasons));
  return reasons[index];
}

#undef OPTIMIZATION_REASON_LIST

std::ostream& operator<<(std::ostream& os, OptimizationReason reason) {
  return os << OptimizationReasonToString(reason);
}

namespace {

void TraceInOptimizationQueue(JSFunction function) {
  if (FLAG_trace_opt_verbose) {
    PrintF("[function ");
    function.PrintName();
    PrintF(" is already in optimization queue]\n");
  }
}

void TraceHeuristicOptimizationDisallowed(JSFunction function) {
  if (FLAG_trace_opt_verbose) {
    PrintF("[function ");
    function.PrintName();
    PrintF(" has been marked manually for optimization]\n");
  }
}

void TraceRecompile(JSFunction function, OptimizationReason reason,
                    CodeKind code_kind, Isolate* isolate) {
  if (FLAG_trace_opt) {
    CodeTracer::Scope scope(isolate->GetCodeTracer());
    PrintF(scope.file(), "[marking ");
    function.ShortPrint(scope.file());
    PrintF(scope.file(), " for optimized recompilation, reason: %s",
           OptimizationReasonToString(reason));
    PrintF(scope.file(), "]\n");
  }
}

}  // namespace

void TieringManager::Optimize(JSFunction function, OptimizationReason reason,
                              CodeKind code_kind) {
  DCHECK_NE(reason, OptimizationReason::kDoNotOptimize);
  TraceRecompile(function, reason, code_kind, isolate_);
  function.MarkForOptimization(ConcurrencyMode::kConcurrent);
}

void TieringManager::AttemptOnStackReplacement(UnoptimizedFrame* frame,
                                               int loop_nesting_levels) {
  JSFunction function = frame->function();
  SharedFunctionInfo shared = function.shared();
  if (!FLAG_use_osr || !shared.IsUserJavaScript()) {
    return;
  }

  // If the code is not optimizable, don't try OSR.
  if (shared.optimization_disabled()) return;

  // We're using on-stack replacement: Store new loop nesting level in
  // BytecodeArray header so that certain back edges in any interpreter frame
  // for this bytecode will trigger on-stack replacement for that frame.
  if (FLAG_trace_osr) {
    CodeTracer::Scope scope(isolate_->GetCodeTracer());
    PrintF(scope.file(), "[OSR - arming back edges in ");
    function.PrintName(scope.file());
    PrintF(scope.file(), "]\n");
  }

  DCHECK(frame->is_unoptimized());
  int level = frame->GetBytecodeArray().osr_loop_nesting_level();
  frame->GetBytecodeArray().set_osr_loop_nesting_level(std::min(
      {level + loop_nesting_levels, AbstractCode::kMaxLoopNestingMarker}));
}

void TieringManager::MaybeOptimizeFrame(JSFunction function,
                                        JavaScriptFrame* frame,
                                        CodeKind code_kind) {
  if (function.IsInOptimizationQueue()) {
    TraceInOptimizationQueue(function);
    return;
  }

  if (FLAG_testing_d8_test_runner &&
      !PendingOptimizationTable::IsHeuristicOptimizationAllowed(isolate_,
                                                                function)) {
    TraceHeuristicOptimizationDisallowed(function);
    return;
  }

  if (function.shared().optimization_disabled()) return;

  // Note: We currently do not trigger OSR compilation from TP code.
  if (frame->is_unoptimized()) {
    if (FLAG_always_osr) {
      AttemptOnStackReplacement(UnoptimizedFrame::cast(frame),
                                AbstractCode::kMaxLoopNestingMarker);
      // Fall through and do a normal optimized compile as well.
    } else if (MaybeOSR(function, UnoptimizedFrame::cast(frame))) {
      return;
    }
  }

  OptimizationReason reason = ShouldOptimize(
      function, function.shared().GetBytecodeArray(isolate_), frame);

  if (reason != OptimizationReason::kDoNotOptimize) {
    Optimize(function, reason, code_kind);
  }
}

bool TieringManager::MaybeOSR(JSFunction function, UnoptimizedFrame* frame) {
  int ticks = function.feedback_vector().profiler_ticks();
  if (function.IsMarkedForOptimization() ||
      function.IsMarkedForConcurrentOptimization() ||
      function.HasAvailableOptimizedCode()) {
    int64_t allowance = kOSRBytecodeSizeAllowanceBase +
                        ticks * kOSRBytecodeSizeAllowancePerTick;
    if (function.shared().GetBytecodeArray(isolate_).length() <= allowance) {
      AttemptOnStackReplacement(frame);
    }
    return true;
  }
  return false;
}

namespace {

bool ShouldOptimizeAsSmallFunction(int bytecode_size, bool any_ic_changed) {
  return !any_ic_changed &&
         bytecode_size < FLAG_max_bytecode_size_for_early_opt;
}

}  // namespace

OptimizationReason TieringManager::ShouldOptimize(JSFunction function,
                                                  BytecodeArray bytecode,
                                                  JavaScriptFrame* frame) {
  if (function.ActiveTierIsTurbofan()) {
    return OptimizationReason::kDoNotOptimize;
  }
  // If function's SFI has OSR cache, once enter loop range of OSR cache, set
  // OSR loop nesting level for matching condition of OSR (loop_depth <
  // osr_level), soon later OSR will be triggered when executing bytecode
  // JumpLoop which is entry of the OSR cache, then hit the OSR cache.
  if (V8_UNLIKELY(function.shared().osr_code_cache_state() > kNotCached) &&
      frame->is_unoptimized()) {
    int current_offset =
        static_cast<UnoptimizedFrame*>(frame)->GetBytecodeOffset();
    OSROptimizedCodeCache cache =
        function.context().native_context().GetOSROptimizedCodeCache();
    std::vector<int> bytecode_offsets =
        cache.GetBytecodeOffsetsFromSFI(function.shared());
    interpreter::BytecodeArrayIterator iterator(
        Handle<BytecodeArray>(bytecode, isolate_));
    for (int jump_offset : bytecode_offsets) {
      iterator.SetOffset(jump_offset);
      int jump_target_offset = iterator.GetJumpTargetOffset();
      if (jump_offset >= current_offset &&
          current_offset >= jump_target_offset) {
        bytecode.set_osr_loop_nesting_level(iterator.GetImmediateOperand(1) +
                                            1);
        return OptimizationReason::kHotAndStable;
      }
    }
  }
  const int ticks = function.feedback_vector().profiler_ticks();
  const int ticks_for_optimization =
      FLAG_ticks_before_optimization +
      (bytecode.length() / FLAG_bytecode_size_allowance_per_tick);
  if (ticks >= ticks_for_optimization) {
    return OptimizationReason::kHotAndStable;
  } else if (ShouldOptimizeAsSmallFunction(bytecode.length(),
                                           any_ic_changed_)) {
    // If no IC was patched since the last tick and this function is very
    // small, optimistically optimize it now.
    return OptimizationReason::kSmallFunction;
  } else if (FLAG_trace_opt_verbose) {
    PrintF("[not yet optimizing ");
    function.PrintName();
    PrintF(", not enough ticks: %d/%d and ", ticks, ticks_for_optimization);
    if (any_ic_changed_) {
      PrintF("ICs changed]\n");
    } else {
      PrintF(" too large for small function optimization: %d/%d]\n",
             bytecode.length(), FLAG_max_bytecode_size_for_early_opt);
    }
  }
  return OptimizationReason::kDoNotOptimize;
}

TieringManager::OnInterruptTickScope::OnInterruptTickScope(
    TieringManager* profiler)
    : profiler_(profiler) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.MarkCandidatesForOptimization");
}

TieringManager::OnInterruptTickScope::~OnInterruptTickScope() {
  profiler_->any_ic_changed_ = false;
}

void TieringManager::OnInterruptTick(Handle<JSFunction> function) {
  IsCompiledScope is_compiled_scope(
      function->shared().is_compiled_scope(isolate_));

  // Remember whether the function had a vector at this point. This is relevant
  // later since the configuration 'Ignition without a vector' can be
  // considered a tier on its own. We begin tiering up to tiers higher than
  // Sparkplug only when reaching this point *with* a feedback vector.
  const bool had_feedback_vector = function->has_feedback_vector();

  // Ensure that the feedback vector has been allocated, and reset the
  // interrupt budget in preparation for the next tick.
  if (had_feedback_vector) {
    function->SetInterruptBudget();
  } else {
    JSFunction::EnsureFeedbackVector(function, &is_compiled_scope);
    DCHECK(is_compiled_scope.is_compiled());
    // Also initialize the invocation count here. This is only really needed for
    // OSR. When we OSR functions with lazy feedback allocation we want to have
    // a non zero invocation count so we can inline functions.
    function->feedback_vector().set_invocation_count(1, kRelaxedStore);
  }

  DCHECK(function->has_feedback_vector());
  DCHECK(function->shared().is_compiled());
  DCHECK(function->shared().HasBytecodeArray());

  // TODO(jgruber): Consider integrating this into a linear tiering system
  // controlled by OptimizationMarker in which the order is always
  // Ignition-Sparkplug-Turbofan, and only a single tierup is requested at
  // once.
  // It's unclear whether this is possible and/or makes sense - for example,
  // batching compilation can introduce arbitrary latency between the SP
  // compile request and fulfillment, which doesn't work with strictly linear
  // tiering.
  if (CanCompileWithBaseline(isolate_, function->shared()) &&
      !function->ActiveTierIsBaseline()) {
    if (FLAG_baseline_batch_compilation) {
      isolate_->baseline_batch_compiler()->EnqueueFunction(function);
    } else {
      IsCompiledScope is_compiled_scope(
          function->shared().is_compiled_scope(isolate_));
      Compiler::CompileBaseline(isolate_, function, Compiler::CLEAR_EXCEPTION,
                                &is_compiled_scope);
    }
  }

  // We only tier up beyond sparkplug if we already had a feedback vector.
  if (!had_feedback_vector) return;

  // Don't tier up if Turbofan is disabled.
  // TODO(jgruber): Update this for a multi-tier world.
  if (V8_UNLIKELY(!isolate_->use_optimizer())) return;

  // --- We've decided to proceed for now. ---

  DisallowGarbageCollection no_gc;
  OnInterruptTickScope scope(this);
  JSFunction function_obj = *function;

  function_obj.feedback_vector().SaturatingIncrementProfilerTicks();

  JavaScriptFrameIterator it(isolate_);
  DCHECK(it.frame()->is_unoptimized());
  const CodeKind code_kind = function_obj.GetActiveTier().value();
  MaybeOptimizeFrame(function_obj, it.frame(), code_kind);
}

}  // namespace internal
}  // namespace v8
