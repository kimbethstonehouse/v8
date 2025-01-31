// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/maglev/maglev-code-generator.h"

#include "src/codegen/code-desc.h"
#include "src/codegen/safepoint-table.h"
#include "src/maglev/maglev-code-gen-state.h"
#include "src/maglev/maglev-compilation-data.h"
#include "src/maglev/maglev-graph-labeller.h"
#include "src/maglev/maglev-graph-printer.h"
#include "src/maglev/maglev-graph-processor.h"
#include "src/maglev/maglev-graph.h"
#include "src/maglev/maglev-ir.h"

namespace v8 {
namespace internal {

namespace maglev {

#define __ masm()->

namespace {

class MaglevCodeGeneratingNodeProcessor {
 public:
  static constexpr bool kNeedsCheckpointStates = true;

  explicit MaglevCodeGeneratingNodeProcessor(MaglevCodeGenState* code_gen_state)
      : code_gen_state_(code_gen_state) {}

  void PreProcessGraph(MaglevCompilationUnit*, Graph* graph) {
    if (FLAG_maglev_break_on_entry) {
      __ int3();
    }

    __ EnterFrame(StackFrame::BASELINE);

    // Save arguments in frame.
    // TODO(leszeks): Consider eliding this frame if we don't make any calls
    // that could clobber these registers.
    __ Push(kContextRegister);
    __ Push(kJSFunctionRegister);              // Callee's JS function.
    __ Push(kJavaScriptCallArgCountRegister);  // Actual argument count.

    // Extend rsp by the size of the frame.
    code_gen_state_->SetVregSlots(graph->stack_slots());
    __ subq(rsp, Immediate(code_gen_state_->vreg_slots() * kSystemPointerSize));

    // Initialize stack slots.
    // TODO(jgruber): Update logic once the register allocator is further along.
    {
      ASM_CODE_COMMENT_STRING(masm(), "Initializing stack slots");
      __ Move(rax, Immediate(0));
      __ Move(rcx, Immediate(code_gen_state_->vreg_slots()));
      __ leaq(rdi, GetStackSlot(code_gen_state_->vreg_slots() - 1));
      __ repstosq();
    }

    // We don't emit proper safepoint data yet; instead, define a single
    // safepoint at the end of the code object, with all-tagged stack slots.
    // TODO(jgruber): Real safepoint handling.
    SafepointTableBuilder::Safepoint safepoint =
        safepoint_table_builder()->DefineSafepoint(masm());
    for (int i = 0; i < code_gen_state_->vreg_slots(); i++) {
      safepoint.DefineTaggedStackSlot(GetSafepointIndexForStackSlot(i));
    }
  }

  void PostProcessGraph(MaglevCompilationUnit*, Graph* graph) {
    code_gen_state_->EmitDeferredCode();
  }

  void PreProcessBasicBlock(MaglevCompilationUnit*, BasicBlock* block) {
    if (FLAG_code_comments) {
      std::stringstream ss;
      ss << "-- Block b" << graph_labeller()->BlockId(block);
      __ RecordComment(ss.str());
    }

    __ bind(block->label());
  }

  template <typename NodeT>
  void Process(NodeT* node, const ProcessingState& state) {
    if (FLAG_code_comments) {
      std::stringstream ss;
      ss << "--   " << graph_labeller()->NodeId(node) << ": "
         << PrintNode(graph_labeller(), node);
      __ RecordComment(ss.str());
    }

    // Emit Phi moves before visiting the control node.
    if (std::is_base_of<UnconditionalControlNode, NodeT>::value) {
      BasicBlock* target =
          node->template Cast<UnconditionalControlNode>()->target();
      if (target->has_state()) {
        int predecessor_id = state.block()->predecessor_id();
        __ RecordComment("--   Register merge gap moves:");
        for (int index = 0; index < kAllocatableGeneralRegisterCount; ++index) {
          RegisterMerge* merge;
          if (LoadMergeState(target->state()->register_state()[index],
                             &merge)) {
            compiler::AllocatedOperand source = merge->operand(predecessor_id);
            Register reg = MapIndexToRegister(index);

            if (FLAG_code_comments) {
              std::stringstream ss;
              ss << "--   * " << source << " → " << reg;
              __ RecordComment(ss.str());
            }

            // TODO(leszeks): Implement parallel moves.
            if (source.IsStackSlot()) {
              __ movq(reg, GetStackSlot(source));
            } else {
              __ movq(reg, ToRegister(source));
            }
          }
        }
        if (target->has_phi()) {
          __ RecordComment("--   Phi gap moves:");
          Phi::List* phis = target->phis();
          for (Phi* phi : *phis) {
            compiler::AllocatedOperand source =
                compiler::AllocatedOperand::cast(
                    phi->input(state.block()->predecessor_id()).operand());
            compiler::AllocatedOperand target =
                compiler::AllocatedOperand::cast(phi->result().operand());
            if (FLAG_code_comments) {
              std::stringstream ss;
              ss << "--   * " << source << " → " << target << " (n"
                 << graph_labeller()->NodeId(phi) << ")";
              __ RecordComment(ss.str());
            }
            if (source.IsRegister()) {
              Register source_reg = ToRegister(source);
              if (target.IsRegister()) {
                __ movq(ToRegister(target), source_reg);
              } else {
                __ movq(GetStackSlot(target), source_reg);
              }
            } else {
              if (target.IsRegister()) {
                __ movq(ToRegister(target), GetStackSlot(source));
              } else {
                __ movq(kScratchRegister, GetStackSlot(source));
                __ movq(GetStackSlot(target), kScratchRegister);
              }
            }
          }
        }
      } else {
        __ RecordComment("--   Target has no state, must be a fallthrough");
      }
    }

    node->GenerateCode(code_gen_state_, state);

    if (std::is_base_of<ValueNode, NodeT>::value) {
      ValueNode* value_node = node->template Cast<ValueNode>();
      if (value_node->is_spilled()) {
        if (FLAG_code_comments) __ RecordComment("--   Spill:");
        compiler::AllocatedOperand source =
            compiler::AllocatedOperand::cast(value_node->result().operand());
        // We shouldn't spill nodes which already output to the stack.
        DCHECK(!source.IsStackSlot());
        __ movq(GetStackSlot(value_node->spill_slot()), ToRegister(source));
      }
    }
  }

  Isolate* isolate() const { return code_gen_state_->isolate(); }
  MacroAssembler* masm() const { return code_gen_state_->masm(); }
  MaglevGraphLabeller* graph_labeller() const {
    return code_gen_state_->graph_labeller();
  }
  SafepointTableBuilder* safepoint_table_builder() const {
    return code_gen_state_->safepoint_table_builder();
  }

 private:
  MaglevCodeGenState* code_gen_state_;
};

}  // namespace

class MaglevCodeGeneratorImpl final {
 public:
  static Handle<Code> Generate(MaglevCompilationUnit* compilation_unit,
                               Graph* graph) {
    return MaglevCodeGeneratorImpl(compilation_unit, graph).Generate();
  }

 private:
  MaglevCodeGeneratorImpl(MaglevCompilationUnit* compilation_unit, Graph* graph)
      : safepoint_table_builder_(compilation_unit->zone()),
        code_gen_state_(compilation_unit, safepoint_table_builder()),
        processor_(compilation_unit, &code_gen_state_),
        graph_(graph) {}

  Handle<Code> Generate() {
    EmitCode();
    EmitMetadata();
    return BuildCodeObject();
  }

  void EmitCode() { processor_.ProcessGraph(graph_); }

  void EmitMetadata() {
    // Final alignment before starting on the metadata section.
    masm()->Align(Code::kMetadataAlignment);

    safepoint_table_builder()->Emit(masm(),
                                    stack_slot_count_with_fixed_frame());
  }

  Handle<Code> BuildCodeObject() {
    CodeDesc desc;
    static constexpr int kNoHandlerTableOffset = 0;
    masm()->GetCode(isolate(), &desc, safepoint_table_builder(),
                    kNoHandlerTableOffset);
    return Factory::CodeBuilder{isolate(), desc, CodeKind::MAGLEV}
        .set_stack_slots(stack_slot_count_with_fixed_frame())
        .Build();
  }

  int stack_slot_count() const { return code_gen_state_.vreg_slots(); }
  int stack_slot_count_with_fixed_frame() const {
    return stack_slot_count() + StandardFrameConstants::kFixedSlotCount;
  }

  Isolate* isolate() const {
    return code_gen_state_.compilation_unit()->isolate();
  }
  MacroAssembler* masm() { return code_gen_state_.masm(); }
  SafepointTableBuilder* safepoint_table_builder() {
    return &safepoint_table_builder_;
  }

  SafepointTableBuilder safepoint_table_builder_;
  MaglevCodeGenState code_gen_state_;
  GraphProcessor<MaglevCodeGeneratingNodeProcessor> processor_;
  Graph* const graph_;
};

// static
Handle<Code> MaglevCodeGenerator::Generate(
    MaglevCompilationUnit* compilation_unit, Graph* graph) {
  return MaglevCodeGeneratorImpl::Generate(compilation_unit, graph);
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8
