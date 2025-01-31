// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_MAGLEV_MAGLEV_CODE_GEN_STATE_H_
#define V8_MAGLEV_MAGLEV_CODE_GEN_STATE_H_

#include "src/codegen/assembler.h"
#include "src/codegen/label.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/safepoint-table.h"
#include "src/common/globals.h"
#include "src/compiler/backend/instruction.h"
#include "src/compiler/js-heap-broker.h"
#include "src/maglev/maglev-compilation-data.h"
#include "src/maglev/maglev-ir.h"

namespace v8 {
namespace internal {
namespace maglev {

class MaglevCodeGenState {
 public:
  class DeferredCodeInfo {
   public:
    virtual void Generate(MaglevCodeGenState* code_gen_state,
                          Label* return_label) = 0;
    Label deferred_code_label;
    Label return_label;
  };

  MaglevCodeGenState(MaglevCompilationUnit* compilation_unit,
                     SafepointTableBuilder* safepoint_table_builder)
      : compilation_unit_(compilation_unit),
        safepoint_table_builder_(safepoint_table_builder),
        masm_(isolate(), CodeObjectRequired::kNo) {}

  void SetVregSlots(int slots) { vreg_slots_ = slots; }

  void PushDeferredCode(DeferredCodeInfo* deferred_code) {
    deferred_code_.push_back(deferred_code);
  }
  void EmitDeferredCode() {
    for (auto& deferred_code : deferred_code_) {
      masm()->RecordComment("-- Deferred block");
      masm()->bind(&deferred_code->deferred_code_label);
      deferred_code->Generate(this, &deferred_code->return_label);
      masm()->int3();
    }
  }

  compiler::NativeContextRef native_context() const {
    return broker()->target_native_context();
  }
  Isolate* isolate() const { return compilation_unit_->isolate(); }
  int parameter_count() const { return compilation_unit_->parameter_count(); }
  int register_count() const { return compilation_unit_->register_count(); }
  const compiler::BytecodeAnalysis& bytecode_analysis() const {
    return compilation_unit_->bytecode_analysis;
  }
  compiler::JSHeapBroker* broker() const { return compilation_unit_->broker(); }
  const compiler::BytecodeArrayRef& bytecode() const {
    return compilation_unit_->bytecode;
  }
  MaglevGraphLabeller* graph_labeller() const {
    return compilation_unit_->graph_labeller();
  }
  MacroAssembler* masm() { return &masm_; }
  int vreg_slots() const { return vreg_slots_; }
  SafepointTableBuilder* safepoint_table_builder() const {
    return safepoint_table_builder_;
  }
  MaglevCompilationUnit* compilation_unit() const { return compilation_unit_; }

 private:
  MaglevCompilationUnit* const compilation_unit_;
  SafepointTableBuilder* const safepoint_table_builder_;

  MacroAssembler masm_;
  std::vector<DeferredCodeInfo*> deferred_code_;
  int vreg_slots_ = 0;
};

// Some helpers for codegen.
// TODO(leszeks): consider moving this to a separate header.

inline MemOperand GetStackSlot(int index) {
  return MemOperand(rbp, StandardFrameConstants::kExpressionsOffset -
                             index * kSystemPointerSize);
}

inline MemOperand GetStackSlot(const compiler::AllocatedOperand& operand) {
  return GetStackSlot(operand.index());
}

inline Register ToRegister(const compiler::InstructionOperand& operand) {
  return compiler::AllocatedOperand::cast(operand).GetRegister();
}

inline Register ToRegister(const ValueLocation& location) {
  return ToRegister(location.operand());
}

inline MemOperand ToMemOperand(const compiler::InstructionOperand& operand) {
  return GetStackSlot(compiler::AllocatedOperand::cast(operand));
}

inline MemOperand ToMemOperand(const ValueLocation& location) {
  return ToMemOperand(location.operand());
}

inline int GetSafepointIndexForStackSlot(int i) {
  // Safepoint tables also contain slots for all fixed frame slots (both
  // above and below the fp).
  return StandardFrameConstants::kFixedSlotCount + i;
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8

#endif  // V8_MAGLEV_MAGLEV_CODE_GEN_STATE_H_
