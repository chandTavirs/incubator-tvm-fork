/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Modified by contributors from Intel Labs

/*!
 * \file inject_virtual_thread.cc
 */
#include <tvm/runtime/registry.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/expr.h>
#include <tvm/tir/stmt_functor.h>
#include <tvm/tir/transform.h>

#include <unordered_set>

#include "ir_utils.h"

namespace tvm {
namespace tir {

// If expression is touched by var.
class ExprTouched final : public StmtExprVisitor {
 public:
  explicit ExprTouched(const std::unordered_set<const VarNode*>& touched, bool check_write)
      : touched_var_(touched), check_write_(check_write) {}

  void VisitExpr(const PrimExpr& n) final {
    // early stopping
    if (expr_touched_ && !check_write_) return;
    StmtExprVisitor::VisitExpr(n);
  }
  void VisitStmt(const Stmt& n) final {
    // early stopping
    if (expr_touched_ && !check_write_) return;
    StmtExprVisitor::VisitStmt(n);
  }
  void VisitExpr_(const LoadNode* op) final {
    HandleUseVar(op->buffer_var.get());
    StmtExprVisitor::VisitExpr_(op);
  }
  void VisitExpr_(const VarNode* op) final { HandleUseVar(op); }
  void VisitExpr_(const CallNode* op) final {
    if (op->op.same_as(builtin::tvm_access_ptr())) {
      const auto* rw_mask = op->args[4].as<IntImmNode>();
      const VarNode* buffer_var = op->args[1].as<VarNode>();
      ICHECK(buffer_var);
      ICHECK(rw_mask);
      // read
      if (rw_mask->value & 1) {
        HandleUseVar(buffer_var);
      }
      if (rw_mask->value & 2) {
        HandleWriteVar(buffer_var);
      }
      this->VisitExpr(op->args[2]);
    } else {
      StmtExprVisitor::VisitExpr_(op);
    }
  }
  void HandleUseVar(const VarNode* var) {
    auto it = touched_var_.find(var);
    if (it != touched_var_.end()) {
      expr_touched_ = true;
    }
    // rember the used vars
    // in case the var get touched later in a loop.
    if (!expr_touched_) {
      used_vars_.push_back(var);
    }
  }
  void HandleWriteVar(const VarNode* var) { write_vars_.push_back(var); }
  // the fields.
  bool expr_touched_{false};
  std::vector<const VarNode*> used_vars_;
  std::vector<const VarNode*> write_vars_;
  const std::unordered_set<const VarNode*>& touched_var_;
  bool check_write_;
};

// Analyze if the buffers are invariant to value of var
class VarTouchedAnalysis : public StmtVisitor {
 public:
  void VisitStmt_(const LetStmtNode* op) final {
    ExprTouched tc(touched_var_, false);
    tc(op->value);
    Record(op->var.get(), tc);
    this->VisitStmt(op->body);
  }
  void VisitStmt_(const StoreNode* op) final {
    ExprTouched tc(touched_var_, false);
    tc(op->value);
    tc(op->index);
    Record(op->buffer_var.get(), tc);
  }
  void VisitStmt_(const ForNode* op) final {
    ExprTouched tc(touched_var_, false);
    tc(op->min);
    tc(op->extent);
    Record(op->loop_var.get(), tc);
    this->VisitStmt(op->body);
  }
  // external function call
  void VisitStmt_(const EvaluateNode* op) final {
    ExprTouched tc(touched_var_, true);
    tc(op->value);
    for (const VarNode* var : tc.write_vars_) {
      Record(var, tc);
    }
  }
  void VisitStmt_(const AllocateNode* op) final {
    ExprTouched tc(touched_var_, false);
    for (size_t i = 0; i < op->extents.size(); ++i) {
      tc(op->extents[i]);
    }
    tc.VisitExpr(op->condition);
    Record(op->buffer_var.get(), tc);
    this->VisitStmt(op->body);
  }
  void Record(const VarNode* var,
              const ExprTouched& tc) {
    if (touched_var_.count(var)) return;
    if (tc.expr_touched_) {
      touched_var_.insert(var);
    } else {
      for (const VarNode* r : tc.used_vars_) {
        if (r != var) {
          affect_[r].push_back(var);
        }
      }
    }
  }

  std::unordered_set<const VarNode*> TouchedVar(const Stmt& stmt, const VarNode* var) {
    touched_var_.insert(var);
    this->VisitStmt(stmt);
    // do a DFS to push affect around dependency.
    std::vector<const VarNode*> pending(touched_var_.begin(), touched_var_.end());
    while (!pending.empty()) {
      const VarNode* v = pending.back();
      pending.pop_back();
      for (const VarNode* r : affect_[v]) {
        if (!touched_var_.count(r)) {
          touched_var_.insert(r);
          pending.push_back(r);
        }
      }
    }
    return std::move(touched_var_);
  }

 private:
  // Whether variable is touched by the thread variable.
  std::unordered_set<const VarNode*> touched_var_;
  // x -> all the buffers x read from
  std::unordered_map<const VarNode*, std::vector<const VarNode*> > affect_;
};

// Class as a expression visitor for VTA pass
// Helps in identifying the EvaluateNode to be deleted
class DetectDthread : public ExprVisitor {
  public:
    void VisitExpr(const PrimExpr &e) final {
      if (has_dthread_) return;
      ExprVisitor::VisitExpr(e);
    }

    void VisitExpr_(const CallNode* op) final {
      if (has_dthread_) return;
      if (!check_load_ && op->op.same_as(builtin::call_extern()) && op->args[0].as<StringImmNode>()->value == "VTALoadBuffer2D") {
        check_load_ = true;
      }
      ExprVisitor::VisitExpr_(op);  
    }

    void VisitExpr_(const VarNode* op) final {
      if (check_load_ && op->name_hint == "dthread") {
        has_dthread_ = true; 
        return;
      }
    }

    // Flag to check the presence of dthread
    bool has_dthread_{false};
    // Flag to check VTALoadBuffer2D call
    bool check_load_{false};
};


// Inject virtual thread loop
// rewrite the buffer access pattern when necessary.
class VTInjector : public StmtExprMutator {
 public:
  // constructor
  VTInjector(Var var, int num_threads, const std::unordered_set<const VarNode*>& touched_var,
             bool allow_share)
      : var_(var), num_threads_(num_threads),
        touched_var_(touched_var), allow_share_(allow_share) {
    // In VTA IR, touched_var_ will only have dthread
    if (touched_var_.size() == 1) {
      is_vta_pass_ = true;
    }
  }
  // Inject VTLoop when needed.
  Stmt VisitStmt(const Stmt& s) final {
    // Stmt parser run
    if (stmt_parser_) {
      return StmtExprMutator::VisitStmt(s);
    }
    ICHECK(!visit_touched_var_);
    auto stmt = StmtExprMutator::VisitStmt(s);
    if (visit_touched_var_ || trigger_base_inject_) {
      if (!vt_loop_injected_) {
        return InjectVTLoop(stmt, false);
      }
      visit_touched_var_ = false;
      trigger_base_inject_ = false;
    }
    return stmt;
  }
  // Variable
  PrimExpr VisitExpr_(const VarNode* op) final {
    if (stmt_parser_) {
      return GetRef<PrimExpr>(op);
    }
    // Get mutator variable for tvm IR pass
    if (track_loadnode_ && !has_index_mutator_) {
      index_mutator_ = GetRef<Var>(op);
      has_index_mutator_ = true;
    }
    ICHECK(!alloc_remap_.count(op)) << "Buffer address may get rewritten in virtual thread";
    if (touched_var_.count(op)) {
      visit_touched_var_ = true;
    }
    return GetRef<PrimExpr>(op);
  }

  PrimExpr RewriteIndexDoubleBuff(PrimExpr index, PrimExpr alloc_extent) const {
    return index + indexmod(index_mutator_, num_threads_) * alloc_extent;
  }

  PrimExpr RewriteIndex(PrimExpr index, PrimExpr alloc_extent) const {
    return index + var_ * alloc_extent;
  }
  // Load
  PrimExpr VisitExpr_(const LoadNode* op) final {
    if (stmt_parser_) {
      return StmtExprMutator::VisitExpr_(op);
    }
    if (track_untouched_ && !is_vta_pass_) {
      track_loadnode_ = true;
    }
    PrimExpr expr = StmtExprMutator::VisitExpr_(op);
    op = expr.as<LoadNode>();
    if (touched_var_.count(op->buffer_var.get()) && is_vta_pass_) {
      visit_touched_var_ = true;
    }
    auto it = alloc_remap_.find(op->buffer_var.get());
    if (it != alloc_remap_.end()) {
      // For tvm IR pass, modified access for LoadNode
      if (var_->name_hint == "dthread" && 
          untouched_buffer_var_->name_hint == op->buffer_var->name_hint) {
        return Load(op->dtype, op->buffer_var, RewriteIndexDoubleBuff(op->index, it->second), op->predicate);
      }
      else {
        return Load(op->dtype, op->buffer_var, RewriteIndex(op->index, it->second), op->predicate);
      }
    } else {
      return expr;
    }
  }
  // Expression.
  PrimExpr VisitExpr_(const CallNode* op) final {
    if (stmt_parser_) {
      return StmtExprMutator::VisitExpr_(op);
    }
    // For VTA IR pass, track loadbuffer calls
    if (op->op.same_as(builtin::call_extern()) && op->args[0].as<StringImmNode>()->value == "VTALoadBuffer2D") {
      loadbuffer_tracked_ = true;
      PrimExpr dest = op->args.operator[](op->args.size()-1);
      // Don't track for acc buffer load (type 3)
      if (dest.as<IntImmNode>()->value == 3) {
        loadbuffer_tracked_ = false;
      }
    }
    if (op->op.same_as(builtin::tvm_access_ptr())) {
      ICHECK_EQ(op->args.size(), 5U);
      DataType dtype = op->args[0].dtype();
      const VarNode* buffer = op->args[1].as<VarNode>();
      auto it = alloc_remap_.find(buffer);
      if (it == alloc_remap_.end()) return StmtExprMutator::VisitExpr_(op);
      visit_touched_var_ = true;
      PrimExpr offset = this->VisitExpr(op->args[2]);
      PrimExpr extent = this->VisitExpr(op->args[3]);
      PrimExpr stride = it->second / make_const(offset.dtype(), dtype.lanes());
      // For VTA IR pass modify access of CallNode
      if (var_->name_hint == "dthread" && !visit_touched_var_ &&
          loadbuffer_tracked_) {
        offset = stride * indexmod(fornode_var_, 2) + offset;
        skip_buffer_name_ = buffer->name_hint;
        loadbuffer_tracked_ = false;
        found_fornode_var_ = true;
        skip_vta_dthread_ = true;
      }
      else if (var_->name_hint == "dthread" && 
               buffer->name_hint == skip_buffer_name_) {
        offset = stride * indexmod(fornode_var_, 2) + offset;
      }
      else {
        offset = stride * var_ + offset;
      }
      return Call(op->dtype, op->op, {op->args[0], op->args[1], offset, extent, op->args[4]});
    } else if (op->op.same_as(builtin::tvm_context_id())) {
      return allow_share_ ? GetRef<PrimExpr>(op) : var_;
    } else {
      return StmtExprMutator::VisitExpr_(op);
    }
  }
  Stmt VisitStmt_(const EvaluateNode* op) final {
    // Detect if dthread present in EvaluateNode
    // Then create a NoOp node to avoid redundant loads
    if (stmt_parser_) {
      DetectDthread det;
      det(op->value);
      if (det.check_load_ && !det.has_dthread_){
        return Evaluate(0);
      }
      else {
        return StmtExprMutator::VisitStmt_(op);
      }
    }
    
    if (loadbuffer_tracked_) {
      loadbuffer_tracked_ = false;
    }
    trigger_base_inject_ = !allow_share_;
    return StmtExprMutator::VisitStmt_(op);
  }
  // Store
  Stmt VisitStmt_(const StoreNode* op) final {
    if (stmt_parser_) {
      return StmtExprMutator::VisitStmt_(op);
    }
    if (!touched_var_.count(op->buffer_var.get()) && !is_vta_pass_) {
      track_untouched_ = true;
    }
    Stmt stmt = StmtExprMutator::VisitStmt_(op);
    op = stmt.as<StoreNode>();
    if (touched_var_.count(op->buffer_var.get()) && !is_vta_pass_) {
      visit_touched_var_ = true;
    }
    trigger_base_inject_ = !allow_share_;
    auto it = alloc_remap_.find(op->buffer_var.get());
    if (var_->name_hint == "dthread" && has_index_mutator_ && !is_vta_pass_) {
      untouched_buffer_var_ = op->buffer_var;
    }
    if (it != alloc_remap_.end()) {
      return Store(op->buffer_var, op->value, RewriteIndex(op->index, it->second), op->predicate);
    } else {
      return stmt;
    }
  }
  // Attribute
  Stmt VisitStmt_(const AttrStmtNode* op) final {
    PrimExpr value = this->VisitExpr(op->value);
    if (visit_touched_var_ && !vt_loop_injected_) {
      return InjectVTLoop(GetRef<Stmt>(op), true);
    } else if (!allow_share_ && !vt_loop_injected_ &&
               (op->attr_key == attr::coproc_uop_scope || op->attr_key == attr::coproc_scope)) {
      return InjectVTLoop(GetRef<Stmt>(op), true);
    } else {
      Stmt body = this->VisitStmt(op->body);
      if (value.same_as(op->value) && body.same_as(op->body)) {
        return GetRef<Stmt>(op);
      } else {
        return AttrStmt(op->node, op->attr_key, value, body);
      }
    }
  }
  // LetStmt
  Stmt VisitStmt_(const LetStmtNode* op) final {
    PrimExpr value = this->VisitExpr(op->value);
    if (visit_touched_var_ && !vt_loop_injected_) {
      return InjectVTLoop(GetRef<Stmt>(op), true);
    }
    visit_touched_var_ = false;
    Stmt body = this->VisitStmt(op->body);
    if (value.same_as(op->value) &&
        body.same_as(op->body)) {
      return GetRef<Stmt>(op);
    } else {
      return LetStmt(op->var, value, body);
    }
  }
  // For
  Stmt VisitStmt_(const ForNode* op) final {
    // Track ForNode var for mutator during VTA pass
    if (!found_fornode_var_) {
      if (((std::string)(op->loop_var->name_hint)).find("init") == std::string::npos) {
        fornode_var_ = op->loop_var;
      }
    }
    ICHECK(is_zero(op->min));
    PrimExpr extent = this->VisitExpr(op->extent);
    if (visit_touched_var_ && !vt_loop_injected_) {
      Stmt stmt = InjectVTLoop(GetRef<Stmt>(op), true);
      ++max_loop_depth_;
      return stmt;
    }
    visit_touched_var_ = false;
    Stmt body = this->VisitStmt(op->body);
    ++max_loop_depth_;
    if (extent.same_as(op->extent) && body.same_as(op->body)) {
      return GetRef<Stmt>(op);
    } else {
      auto n = CopyOnWrite(op);
      n->extent = std::move(extent);
      n->body = std::move(body);
      return Stmt(n);
    }
  }
  // IfThenElse
  Stmt VisitStmt_(const IfThenElseNode* op) final {
    PrimExpr condition = this->VisitExpr(op->condition);
    if (visit_touched_var_ && !vt_loop_injected_) {
      return InjectVTLoop(GetRef<Stmt>(op), true);
    }
    visit_touched_var_ = false;
    ICHECK_EQ(max_loop_depth_, 0);
    Stmt then_case = this->VisitStmt(op->then_case);
    Stmt else_case;
    if (op->else_case.defined()) {
      int temp = max_loop_depth_;
      max_loop_depth_ = 0;
      else_case = this->VisitStmt(op->else_case);
      max_loop_depth_ = std::max(temp, max_loop_depth_);
    }
    if (condition.same_as(op->condition) && then_case.same_as(op->then_case) &&
        else_case.same_as(op->else_case)) {
      return GetRef<Stmt>(op);
    } else {
      return IfThenElse(condition, then_case, else_case);
    }
  }

  // Seq
  Stmt VisitStmt_(const SeqStmtNode* op) final {

    // To avoid nbytes=8 in VTA runtime's UopKernelMap
    // Flattened the dthread loop
    // max_loop_depth > 0 as a result
    // Disabled this check
    // ICHECK_EQ(max_loop_depth_, 0);
    auto fmutate = [this](const Stmt& s) {
      int temp = max_loop_depth_;
      max_loop_depth_ = 0;
      Stmt ret = this->VisitStmt(s);
      max_loop_depth_ = std::max(max_loop_depth_, temp);
      return ret;
    };
    return StmtMutator::VisitSeqStmt_(op, false, fmutate);
  }
  // Allocate
  Stmt VisitStmt_(const AllocateNode* op) final {
    PrimExpr condition = this->VisitExpr(op->condition);
    if (visit_touched_var_ && !vt_loop_injected_) {
      return InjectVTLoop(GetRef<Stmt>(op), true);
    }

    bool changed = false;
    Array<PrimExpr> extents;
    for (size_t i = 0; i < op->extents.size(); i++) {
      PrimExpr new_ext = this->VisitExpr(op->extents[i]);
      if (visit_touched_var_ && !vt_loop_injected_) {
        return InjectVTLoop(GetRef<Stmt>(op), true);
      }
      if (!new_ext.same_as(op->extents[i])) changed = true;
      extents.push_back(new_ext);
    }
    visit_touched_var_ = false;

    Stmt body;
    // always rewrite if not allow sharing.
    if (touched_var_.count(op->buffer_var.get()) || !allow_share_) {
      // place v on highest dimension.
      PrimExpr stride = foldl([](PrimExpr a, PrimExpr b, Span span) { return mul(a, b, span); },
                              make_const(DataType::Int(32), 1), op->extents) *
                        op->dtype.lanes();
      Array<PrimExpr> other;
      other.push_back(make_const(op->extents[0].dtype(), num_threads_));
      for (PrimExpr e : extents) {
        other.push_back(e);
      }
      extents = other;
      changed = true;
      // mark this buffer get touched.
      alloc_remap_[op->buffer_var.get()] = stride;
      // Mutate the body.
      body = this->VisitStmt(op->body);
    } else {
      // Mutate the body.
      body = this->VisitStmt(op->body);
    }
    if (!changed && body.same_as(op->body) && condition.same_as(op->condition)) {
      return GetRef<Stmt>(op);
    } else {
      return Allocate(op->buffer_var, op->dtype, extents, condition, body);
    }
  }

  // inject vthread loop
  Stmt InjectVTLoop(Stmt stmt, bool before_mutation) {
    ICHECK(!vt_loop_injected_);
    // reset the flags
    visit_touched_var_ = false;
    trigger_base_inject_ = false;
    vt_loop_injected_ = true;
    if (before_mutation) {
      stmt = this->VisitStmt(stmt);
    }
    // reset the flags after processing.
    vt_loop_injected_ = false;
    visit_touched_var_ = false;
    // only unroll if number of vthreads are small
    // Disabled the first condition to avoid nbytes=8
    // in VTA runtime for dthread
    // if (max_loop_depth_ == 0 && num_threads_ < 16) {
    if (num_threads_ < 16) {
      // do unrolling if it is inside innermost content.
      Array<Stmt> seq;
      if (var_->name_hint == "dthread" && !inject_virtual_thread_) {
        inject_virtual_thread_ = true;
        track_untouched_ = false;
        track_loadnode_ = false;
        return stmt;
      }
      else {
        for (int i = 0; i < num_threads_; ++i) {
          if (skip_vta_dthread_) {
            if (!stmt_parser_) {
              seq.push_back(Substitute(stmt, {{var_, make_const(var_.dtype(), i)}}));
              stmt_parser_ = true;
            } 
            else {
              // Mutate the AST, by zeroing out EvaluateNode in VTA pass
              // stmt_parser_ = true bypasses the usual mutation
              Stmt mod = this->VisitStmt(stmt);
              seq.push_back(Substitute(mod, {{var_, make_const(var_.dtype(), i)}}));
            }
          }
          else {
            seq.push_back(Substitute(stmt, {{var_, make_const(var_.dtype(), i)}}));
          }
        }
        stmt_parser_ = false;
        return SeqStmt::Flatten(seq);
      }
    } else {
      // insert a for loop
      Var idx(var_->name_hint + ".s", var_->dtype);
      Map<Var, PrimExpr> values{{var_, idx}};
      stmt = Substitute(stmt, values);
      return For(idx, make_zero(idx.dtype()), make_const(idx.dtype(), num_threads_),
                 ForKind::kSerial, stmt);
    }
  }

 private:
  // vthread variable
  Var var_;
  // the threads/lanes
  int num_threads_;
  // whethe the loop is already injected.
  bool vt_loop_injected_{false};
  // whether current expression get touched.
  bool visit_touched_var_{false};
  // Trigger base stmt
  bool trigger_base_inject_{false};
  // the counter of loops in after mutation.
  int max_loop_depth_{0};
  // The variables that get touched.
  const std::unordered_set<const VarNode*>& touched_var_;
  // Whether allow shareding.
  bool allow_share_;
  // The allocations that get touched -> extent
  std::unordered_map<const VarNode*, PrimExpr> alloc_remap_;

  // Enhancements for double buffering improvements
  // Track if a StoreNode is untouched by thread var
  bool track_untouched_{false};
  // Track a LoadNode for untouched StoreNode
  bool track_loadnode_{false};
  // Store double buffering mutator variable
  bool has_index_mutator_{false};
  // Switch for virtual thread injection and skip
  bool inject_virtual_thread_{true};
  // Intermediate variables
  Var index_mutator_, untouched_buffer_var_;
  // VTA-specific variable to store mutator
  Var fornode_var_;
  // Check if ForNode var is used
  bool found_fornode_var_{false};
  // Check if loadbuffer is tracked
  bool loadbuffer_tracked_{false};
  // Flag to indicate statement parsing for VTA pass
  bool stmt_parser_{false};
  // Flag to skip virtual thread injection in VTA pass
  bool skip_vta_dthread_{false};
  // String to store buffer name to be skipped
  std::string skip_buffer_name_{""};
  // Flag to check if VTA IR pass
  bool is_vta_pass_{false};
  
};

class VirtualThreadInjector : public StmtMutator {
 public:
  Stmt VisitStmt_(const AttrStmtNode* op) final {
    Stmt stmt = StmtMutator::VisitStmt_(op);
    op = stmt.as<AttrStmtNode>();
    if (op->attr_key == attr::virtual_thread) {
      IterVar iv = Downcast<IterVar>(op->node);
      bool allow_share = iv->thread_tag == "vthread";
      int nthread = static_cast<int>(op->value.as<IntImmNode>()->value);
      VarTouchedAnalysis vs;
      auto touched = vs.TouchedVar(op->body, iv->var.get());
      VTInjector injecter(iv->var, nthread, touched, allow_share);
      return injecter(op->body);
    } else {
      return stmt;
    }
  }

  Stmt VisitStmt_(const ProducerStoreNode* op) final {
    LOG(FATAL) << "Need to call StorageFlatten first";
    return GetRef<Stmt>(op);
  }
};

Stmt InjectVirtualThread(Stmt stmt) {
  stmt = VirtualThreadInjector()(std::move(stmt));
  return ConvertSSA(std::move(stmt));
}

namespace transform {

Pass InjectVirtualThread() {
  auto pass_func = [](PrimFunc f, IRModule m, PassContext ctx) {
    auto* n = f.CopyOnWrite();
    n->body = ConvertSSA(VirtualThreadInjector()(std::move(n->body)));
    return f;
  };
  return CreatePrimFuncPass(pass_func, 0, "tir.InjectVirtualThread", {});
}

TVM_REGISTER_GLOBAL("tir.transform.InjectVirtualThread").set_body_typed(InjectVirtualThread);

}  // namespace transform

}  // namespace tir
}  // namespace tvm
