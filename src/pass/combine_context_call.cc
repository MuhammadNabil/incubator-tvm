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

/*!
 *  Combine calls into context related function into one.
 *
 * \file combine_context_call.cc
 */
#include <tvm/ir.h>
#include <tvm/ir_functor_ext.h>
#include <tvm/ir_pass.h>
#include <map>

namespace tvm {
namespace ir {

// Calculate the statistics of packed function.
// These information are needed during codegen.
class ContextCallCombiner final : public StmtExprMutator {
 public:
  struct CompareExpr {
    bool operator()(const Expr& lhs, const Expr& rhs) const {
      return Compare(lhs, rhs) < 0;
    }
  };

  Expr VisitExpr_(const Call* op) final {
    if (op->is_intrinsic(intrinsic::tvm_thread_context)) {
      CHECK_EQ(op->args.size(), 1U);
      Expr ctx = op->args[0];
      auto it  = ctx_map_.find(ctx);
      if (it != ctx_map_.end()) {
        return it->second;
      } else {
        CHECK(ctx.dtype().is_handle());
        std::string name;
        if (const Call* call = ctx.as<Call>()) {
          name = call->name + "_cache";
        } else {
          name = "ctx_cache_";
        }
        Var ctx_var(name, ctx.dtype());
        ctx_map_[ctx] = ctx_var;
        return std::move(ctx_var);
      }
    } else {
      return StmtExprMutator::VisitExpr_(op);
    }
  }

  Stmt VisitStmt_(const AttrStmt* op) final {
    if (op->attr_key == attr::thread_extent ||
        op->attr_key == attr::coproc_uop_scope) {
      // Map of comparison expression to variable
      std::map<Expr, Var, CompareExpr> temp;
      std::swap(temp, ctx_map_);
      Stmt stmt = StmtExprMutator::VisitStmt_(op);
      std::swap(temp, ctx_map_);
      return BuildContext(temp, stmt);
    } else {
      return StmtExprMutator::VisitStmt_(op);
    }
  }

  Stmt VisitStmt_(const For* op) final {
    if (op->for_type == ForType::Parallel) {
      // Map of comparison expression to variable
      std::map<Expr, Var, CompareExpr> temp;
      std::swap(temp, ctx_map_);
      Stmt stmt = StmtExprMutator::VisitStmt_(op);
      std::swap(temp, ctx_map_);
      return BuildContext(temp, stmt);
    } else {
      return StmtExprMutator::VisitStmt_(op);
    }
  }

  Stmt Combine(Stmt stmt) {
    return BuildContext(ctx_map_, this->VisitStmt(stmt));
  }

 private:
  static Stmt BuildContext(const std::map<Expr, Var, CompareExpr>& cmap,
                           Stmt body) {
    for (const auto& kv : cmap) {
      body = LetStmt::make(kv.second, kv.first, body);
    }
    return body;
  }
  // Map of comparison expression to variable
  std::map<Expr, Var, CompareExpr> ctx_map_;
};

LoweredFunc CombineContextCall(LoweredFunc f) {
  auto n = make_object<LoweredFuncNode>(*f.operator->());
  n->body = ContextCallCombiner().Combine(n->body);
  return LoweredFunc(n);
}

}  // namespace ir
}  // namespace tvm
