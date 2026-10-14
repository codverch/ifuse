/* Copyright 2024 Litz Lab
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/***************************************************************************************
 * File         : ft.cc
 * Author       : Mingsheng Xu, Yuanpeng Liao
 * Date         :
 * Description  : Fetch Target (FT) class implementation
 ***************************************************************************************/

#include "ft.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <unordered_set>

#include "globals/assert.h"
#include "globals/utils.h"

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#include "helios/heliosFusion.h"

extern "C" {
#include "bp/bp_targ_mech.h"
#include "bp/cbp_to_scarab.h"
#include "frontend/frontend.h"
}
#include "frontend/frontend_intf.h"
#include "isa/isa_macros.h"

#include "decoupled_frontend.h"
#include "op_pool.h"
#include "uop_cache.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_DECOUPLED_FE, ##args)

uint64_t FT_id_counter = 0;

// Set of currently-live FT objects, maintained ONLY when HELIOS pipeline flushes are enabled (so the
// baseline / flushes-off path carries zero overhead). A HELIOS mis-fusion flush is the only recovery
// that can fully squash a nested off-path FT graph (a normal branch recovery keeps the off-path FT's
// moved on-path prefix, which keeps that FT alive). When the whole graph is squashed, an op can be
// left with a parent_FT / parent_FT_off_path that a sibling op's ft_free_op already deleted -- the op
// was split out of that FT's ops vector by a later off-path split, so FT::~FT never nulled its
// back-pointer. ft_is_live() lets ft_free_op detect and skip such dangling FT pointers instead of
// dereferencing freed memory (use-after-free). See ft_free_op.
static std::unordered_set<const FT*> g_helios_live_fts;
static inline bool ft_is_live(const FT* ft) {
  return g_helios_live_fts.count(ft) != 0;
}

// HELIOS: flush-flagged (helios_flush_pending) mem uops of the macro currently being fetched, awaiting their
// deferred BP-state snapshot at the macro's eom uop (see the deferral comment in FT::predict_ft).
// Single list (not per-proc): only the main BP of the single modeled core takes these snapshots.
// Cleared on every recovery -- a recovery boundary is always macro-aligned, so a pending (youngest,
// still-being-fetched) macro is always fully squashed and its ops are about to be freed/re-fetched.
static std::vector<Op*> g_helios_pending_flush_snapshots;

void helios_ft_clear_pending_flush_snapshots(void) {
  g_helios_pending_flush_snapshots.clear();
}

/* HELIOS flush squash sweep.
 *
 * A mis-fusion flush squashes ON-PATH ops (op_num > recovery_op_num). Their teardown is distributed
 * across the pipeline-slot recover passes (FTQ, ft_op_buffer, decode/uop-queue/idq/map latches, node
 * window), each of which calls ft_free_op on the squashed ops it happens to hold -- and ops are only
 * actually freed when ft_free_op runs on their FT's LAST op. An FT whose last op sits in NO recovered
 * slot (e.g. mid-construction FTs hit by the flush, or FTs whose tail ops were consumed into
 * structures that recover by trimming rather than freeing) leaks wholesale: its squashed ops stay
 * op_pool_valid with stale state/queue_entry_id and live wake-up-list entries. A surviving producer
 * (the KEPT mis-fused load re-executing after the flush) then wakes such a zombie into a freed/
 * reallocated issue-queue entry (issue_queue.cc wakeup assert; adjacent macros can share unique_num --
 * it only advances at icache serve -- so the wake-list unique_num staleness guard cannot catch a
 * still-alive zombie).
 *
 * Fix: after all recover passes, sweep every live FT (registry maintained when flushes are on) whose
 * trailing op is squashed and drive the EXISTING ft_free_op teardown on its last op -- this reuses the
 * mid-FT kept-prefix trim, the off-path-sibling delete, and the symmetric-ownership logic unchanged.
 * This is ONLY correct for a helios flush: its continuation is re-fetched from the memtrace replay
 * ring as brand-new ops (the stale saved_recovery_ft is discarded), so no live FT legitimately holds
 * re-servable ops younger than the boundary. A normal branch recovery DOES re-serve such ops (from
 * saved_recovery_ft), so the sweep must never run for it -- the caller gates on helios_flush. */
void helios_sweep_squashed_fts(Counter recovery_op_num) {
  ASSERT(0, bp_recovery_info->helios_flush);
  // Snapshot: ft_free_op can delete this FT and/or sibling FTs from the registry while we sweep.
  // (The registry stores const FT*; the sweep mutates, so cast back -- all registered FTs are
  // non-const objects registered from FT::FT.)
  std::vector<FT*> live;
  live.reserve(g_helios_live_fts.size());
  for (const FT* cft : g_helios_live_fts)
    live.push_back(const_cast<FT*>(cft));
  for (FT* ft : live) {
    while (ft_is_live(ft)) {
      std::vector<Op*>& ops = ft->ops;
      // Drop already-freed trailing husks (an op freed via a sibling FT's teardown stays in this
      // vector with op_pool_valid=0) so the tail check below sees a real op.
      while (!ops.empty() && !ops.back()->op_pool_valid)
        ops.pop_back();
      if (ops.empty()) {
        free_ft(ft);  // empty husk
        break;
      }
      Op* last = ops.back();
      if (!last->off_path && last->op_num <= recovery_op_num) {
        // FT ends in a kept op; nothing younger survives in it (ops are op_num-ordered).
        if (ft->op_pos > ops.size())
          ft->op_pos = ops.size();
        break;
      }
      if (!last->parent_FT && !last->parent_FT_off_path) {
        // Fully detached orphan still referenced by this vector: free it directly.
        ops.pop_back();
        free_op(last);
        continue;
      }
      if (!last->parent_FT) {
        // Home FT already torn down; the op is held alive solely by an off-path sibling claim
        // (ft_free_op asserts parent_FT, so resolve the claim here).
        if (!ft_is_live(last->parent_FT_off_path)) {
          last->parent_FT_off_path = nullptr;  // stale claim; next iteration frees it as an orphan
          continue;
        }
        if (last->parent_FT_off_path == ft) {
          free_ft(ft);  // we ARE the owning sibling; release_ops frees ops with no other live owner
          break;
        }
        ops.pop_back();  // owned by a different live sibling; drop this non-owning reference
        continue;
      }
      const size_t prev_n = ops.size();
      ft_free_op(last);
      if (ft_is_live(ft) && ft->ops.size() == prev_n && ft->ops.back() == last) {
        // No progress: `last` is owned (claim pointers) by other FTs; drop this FT's non-owning
        // reference and let the owners free it.
        ft->ops.pop_back();
      }
    }
  }
}

static inline const Bp_Pred_Info* ft_active_or_main_bp_pred_info(const Op* op) {
  return op->bp_pred_info ? op->bp_pred_info : &op->bp_pred_main;
}

/* FT member functions */
FT::~FT() {
  release_ops();
}

void FT::release_ops() {
  if (HELIOS_DO_FUSION) {
    g_helios_live_fts.erase(this);
    decoupled_fe_helios_ft_deleted(proc_id, this);  // null any DFE pointer to this FT
  }
  for (auto ft_op : ops) {
    // HELIOS flush: a squashed home FT and its extracted off_path_ft (created by extract_off_path_ft)
    // share on-path ops -- the same Op* sits in BOTH ops vectors. A flush squashes both and they are
    // freed via their respective last ops, which ft_free_op can process in EITHER order (pipeline-slot
    // order, not op_num order). The baseline path below assumes the off_path_ft is always destroyed
    // first (so parent_FT_off_path == this when set); that ordering does not hold under a flush. Use
    // symmetric ownership: detach this FT's claim(s) and free the op only once no OTHER live FT still
    // owns it -- the still-live sibling frees it when it is torn down. This reduces to the baseline
    // behavior whenever there is no live cross-owner, and is gated so flushes-off stays byte-identical.
    if (HELIOS_DO_FUSION) {
      if (ft_op->parent_FT == this)
        ft_op->parent_FT = nullptr;
      if (ft_op->parent_FT_off_path == this)
        ft_op->parent_FT_off_path = nullptr;
      bool other_live = (ft_op->parent_FT && ft_is_live(ft_op->parent_FT)) ||
                        (ft_op->parent_FT_off_path && ft_is_live(ft_op->parent_FT_off_path));
      if (!other_live && ft_op->op_pool_valid) {
        ft_op->parent_FT = nullptr;
        ft_op->parent_FT_off_path = nullptr;
        free_op(ft_op);
      }
      continue;
    }
    if (!ft_op->parent_FT_off_path || ft_op->off_path) {
      ft_op->parent_FT = nullptr;
      free_op(ft_op);
    }
    if (ft_op->parent_FT_off_path) {
      ASSERT(proc_id, !ft_op->off_path);
      ASSERT(proc_id, ft_op->parent_FT_off_path == this);
      ft_op->parent_FT_off_path = nullptr;
    }
  }
  ops.clear();
}

FT::FT(uns _proc_id, uns _bp_id) {
  reinit(_proc_id, _bp_id);
}

void FT::reinit(uns _proc_id, uns _bp_id) {
  /* Reset by assigning a default-constructed FT, so every member -- including any added later --
     is reset without this function having to name it. The two things that must survive recycling
     are named explicitly: the ops vector (kept for its capacity; release_ops() already emptied it)
     and the pool link. */
  std::vector<Op*> kept_ops = std::move(ops);
  FT* kept_pool_next = pool_next;

  *this = FT{};

  ops = std::move(kept_ops);
  pool_next = kept_pool_next;
  proc_id = _proc_id;
  bp_id = _bp_id;
  ft_info.dynamic_info.FT_id = FT_id_counter++;
  if (HELIOS_DO_FUSION)
    g_helios_live_fts.insert(this);
}

static FT* ft_free_list = nullptr;

FT* alloc_ft(uns proc_id, uns bp_id) {
  FT* ft = ft_free_list;
  if (!ft)
    return new FT(proc_id, bp_id);
  ft_free_list = ft->pool_next;
  ft->reinit(proc_id, bp_id);
  return ft;
}

void free_ft(FT* ft) {
  ASSERT(0, ft);
  ASSERT(ft->proc_id, ft->bp_id || !ft->ops.empty());
  if (HELIOS_DO_FUSION) {
    // A recycled FT would make a stale pointer look live to ft_is_live().
    delete ft;
    return;
  }
  ft->release_ops();
  ft->pool_next = ft_free_list;
  ft_free_list = ft;
}

bool FT::can_fetch_op() {
  return op_pos < ops.size();
}

Op* FT::fetch_op() {
  ASSERT(proc_id, can_fetch_op());
  Op* op = ops[op_pos];
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst);
  op_pos++;

  DEBUG(proc_id, "Fetch op from FT fetch_addr0x:%llx off_path:%i op_num:%llu\n", op->inst->addr, op->off_path,
        op->op_num);
  return op;
}

void FT::add_op(Op* op) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst);
  if (!ops.empty()) {
    ASSERT(proc_id, ops.back());
    ASSERT(proc_id, ops.back()->op_pool_valid);
    ASSERT(proc_id, ops.back()->inst);
    if (op->bom) {
      // assert consecutivity
      DEBUG(proc_id, "back addr + size %llx fetch addr %llx\n", ops.back()->inst->addr + ops.back()->inst->inst_size,
            op->inst->addr);
      ASSERT(proc_id, ops.back()->inst->addr + ops.back()->inst->inst_size == op->inst->addr);
    } else {
      // assert all uops of the same inst share the same addr
      ASSERT(proc_id, ops.back()->inst->addr == op->inst->addr);
    }
  }
  op->parent_FT = this;

  // Group this uop into its dynamic macro instance: a new instance starts at the bom; the macro's
  // consecutively-added uops all attach to it. Released when its eom uop is freed (see free_op).
  if (op->bom)
    building_dyn_inst = alloc_dyn_inst();
  ASSERT(proc_id, building_dyn_inst);
  dyn_inst_attach(building_dyn_inst, op);

  ops.emplace_back(op);
}

void FT::trim_unread_tail(const std::function<bool(Op*)>& should_remove) {
  ASSERT(proc_id, op_pos <= ops.size());
  while (ops.size() > op_pos) {
    Op* op = ops.back();
    if (!should_remove(op))
      break;
    ops.pop_back();
    op->parent_FT = nullptr;
    op->parent_FT_off_path = nullptr;
    free_op(op);
  }
}

/* remove some pre-built ops after recovery in execution-driven (PIN) mode.
 * in PIN execution-driven mode, after recovery, some ops may have been already saved as
 * recovery FT so we delete those ops and refetch them to rebuild the FT.
 */
void FT::remove_op_after_exec_recover() {
  ASSERT(proc_id, FRONTEND == FE_PIN_EXEC_DRIVEN);
  trim_unread_tail([](Op*) { return true; });
  ASSERT(proc_id, get_end_reason() == FT_NOT_ENDED || get_end_reason() == FT_TAKEN_BRANCH);
}

FT_Event FT::build(std::function<bool(uns8, uns8)> can_fetch_op_fn, std::function<bool(uns8, uns8, Op*)> fetch_op_fn,
                   bool off_path, bool conf_off_path, std::function<uint64_t()> get_next_op_id_fn) {
  FT_Event event = FT_EVENT_NONE;
  do {
    if (!can_fetch_op_fn(proc_id, bp_id)) {
      std::cout << "Warning could not fetch inst from frontend" << std::endl;
      free_ft(this);
      return FT_EVENT_BUILD_FAIL;
    }
    Op* op = alloc_op(proc_id);
    fetch_op_fn(proc_id, bp_id, op);
    op->off_path = off_path;
    op->conf_off_path = conf_off_path;
    collect_op_stats(op);
    op->op_num = get_next_op_id_fn();
    op->bp_pred_main.pred_npc = op->oracle_info.npc;
    op->bp_pred_main.pred = op->oracle_info.dir;  // for prebuilt, pred is same as dir
    add_op(op);
    if (off_path) {
      bp_predict_btb(g_bp_data, op);
      if (bp_l0_enabled())
        predict_op_ft_event(op, BP_PRED_L0);
      event = predict_op_ft_event(op, BP_PRED_MAIN);
      op_select_bp_pred_info(op, BP_PRED_MAIN);
      bp_btb_post_bp_predict(g_bp_data, op);  // for next BTB access
    }
    if (op->inst->fake_inst == 1) {
      ft_info.dynamic_info.contains_fake_nop = TRUE;
    }
    if ((event == FT_EVENT_MISPREDICT || event == FT_EVENT_FETCH_BARRIER) && off_path) {
      generate_ft_info();
      return event;
    }
    STAT_EVENT(proc_id, FTQ_FETCHED_INS_ONPATH + off_path);
  } while (get_end_reason() == FT_NOT_ENDED);

  generate_ft_info();

  return event;
}

// will extract ops from 0 to index and form a new FT as off-path FT,
// the original FT have op_pos moved and modify ft_info to truncated version
// returns off_path and original FT
/***************************************************************************************
 * redirect_to_off_path() Cases Documentation
 *
 * This function handles branch misprediction by transitioning to off-path execution.
 * The behavior depends on where the misprediction occurs and the current (off path) FT state.
 *
 *
 * Code Flow:
 * 1. Extract on-path op from current FT at misprediction index as the current (off path) FT
 * 2. Set up recovery FT (either use trailing_ft or build new one)
 * 3. Transition to off-path state and redirect frontend
 * 4. Continue building off-path FT if needed (cases 2)
 *
 * - split_last:      Misprediction occurred at the last operation of the FT
 * - ft_ended:        splitted front part FT was already terminated before misprediction
 * - generate off FT: Need to building/padding the current off-path FT
 * - trailing_ft:     Remaining on-path operations after misprediction point
 *
 *
 * +------+------------+----------+-----------------+----------------------------------+
 * | Case | Split Last | FT Ended | Generate Off FT | Description                      |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  1   |    Yes     |   Yes    |       No        | Mispred branch at end of line    |
 * |      |            |          |                 | - Misprediction at last op       |
 * |      |            |          |                 | - FT already ended               |
 * |      |            |          |                 | - Build New recovery FT          |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  2   |    Yes     |   No     |      Yes        | Last op mispred not-taken        |
 * |      |            |          |                 | - Misprediction at last op       |
 * |      |            |          |                 | - FT not ended (pred not-taken)  |
 * |      |            |          |                 | - Need to pad/continue off-path  |
 * |      |            |          |                 | - Build New recovery FT          |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  3   |    No      |   Yes    |      Yes        | Mispred in middle as taken       |
 * |      |            |          |                 | - Misprediction in middle of FT  |
 * |      |            |          |                 | - FT ends (pred taken branch)    |
 * |      |            |          |                 | - needs new off-path FT          |
 * |      |            |          |                 | - No need to pad off-path        |
 * |      |            |          |                 | - Use trailing_ft for recovery   |
 * +------+------------+----------+-----------------+----------------------------------+
 * |  4   |    No      |   No     |      Yes        | Mispred in middle not taken (btb)|
 * |      |            |          |                 | - btb miss result in mispred     |
 * |      |            |          |                 | - Misprediction in middle of FT  |
 * |      |            |          |                 | - FT not end                     |
 * |      |            |          |                 | - Need to pad off-path           |
 * |      |            |          |                 | - Use trailing_ft for recovery   |
 * +------+------------+----------+-----------------+----------------------------------+
 ***************************************************************************************/

std::pair<FT*, FT*> FT::extract_off_path_ft(uns split_index) {
  uns index_uns = static_cast<uns>(split_index);
  ASSERT(proc_id, index_uns < ops.size() && index_uns >= 0);
  ASSERT(proc_id, get_is_prebuilt());

  // if split at the last op and FT already ended, no need to create new FT, just update end condition
  if (split_index == ops.size() - 1 && get_end_reason() != FT_NOT_ENDED) {
    generate_ft_info();
    return {this, nullptr};
  }
  // Initialize off-path FT that will contain off-path ops after split position
  FT* off_path_ft = alloc_ft(proc_id, bp_id);

  bool has_trailing_ops = (index_uns + 1 < ops.size());

  for (uns i = op_pos; i <= split_index; i++) {
    off_path_ft->ops.push_back(ops[i]);
    ops[i]->parent_FT_off_path = off_path_ft;
  }

  ASSERT(proc_id, off_path_ft->ops.size() == index_uns + 1 - op_pos);
  op_pos = index_uns + 1;
  if (has_trailing_ops) {
    // Inline move_over_ft logic
    ASSERT(0, (index_uns + 1 <= ops.size() - 1 && ops.size() - 1 < ops.size() && index_uns + 1 >= 0));

    generate_ft_info();

    ASSERT(proc_id, ft_info.static_info.start && ft_info.static_info.length && ops.size());
    ASSERT(proc_id, ops.size() == (ops.size() - ops.size()) + ops.size());
    ASSERT(proc_id, ft_info.dynamic_info.first_op_off_path == 0);
    ASSERT(proc_id, ft_info.dynamic_info.ended_by != FT_NOT_ENDED);
    ASSERT(proc_id, get_end_reason() != FT_NOT_ENDED);
  } else {
    off_path_ft->is_prebuilt = 1;
    this->is_prebuilt = 0;
  }

  ASSERT(proc_id, this->is_prebuilt || off_path_ft->is_prebuilt);
  ASSERT(proc_id, !(this->is_prebuilt && off_path_ft->is_prebuilt));

  off_path_ft->generate_ft_info();

  return {off_path_ft, this};
}

FT_Event FT::predict_op_ft_event(Op* op, Bp_Pred_Level pred_level) {
  Bp_Pred_Info* bp_pred_info = (pred_level == BP_PRED_L0) ? &op->bp_pred_l0 : &op->bp_pred_main;
  bool trace_mode = false;

#ifdef ENABLE_PT_MEMTRACE
  trace_mode |= (FRONTEND == FE_PT || FRONTEND == FE_MEMTRACE);
#endif
  if (op->uop->cf_type) {
    ASSERT(proc_id, op->eom);
    bp_predict_op(g_bp_data, op, op->parent_FT->bp_id, 1, op->inst->addr, pred_level);
    const Addr pc_plus_offset = ADDR_PLUS_OFFSET(op->inst->addr, op->inst->inst_size);

    DEBUG(proc_id,
          "[DFE%u] Predict CF fetch_addr:%llx true_npc:%llx pred_npc:%llx recover_at_fe:%i btb miss:%i taken:%i "
          "recover_at_decode:%i recover_at_exec:%i, bar_fetch:%i\n",
          bp_id, op->inst->addr, op->oracle_info.npc, bp_pred_info->pred_npc,
          bp_pred_info->recovery_point == RECOVER_AT_FE, btb_pred_miss(op->btb_pred_info), bp_pred_info->pred == TAKEN,
          bp_pred_info->recovery_point == RECOVER_AT_DECODE, bp_pred_info->recovery_point == RECOVER_AT_EXEC,
          op->uop->bar_type & BAR_FETCH);
    if ((op->uop->bar_type & BAR_FETCH) || IS_CALLSYS(op->uop)) {
      bp_pred_info->recovery_point = RECOVER_AT_NONE;
      STAT_EVENT(proc_id, op->off_path ? FTQ_SAW_BAR_FETCH_OFFPATH : FTQ_SAW_BAR_FETCH_ONPATH);
      return FT_EVENT_FETCH_BARRIER;
    }
    if (bp_pred_info->recovery_point != RECOVER_AT_NONE) {
      ASSERT(0,
             !(bp_pred_info->recovery_point == RECOVER_AT_DECODE && bp_pred_info->recovery_point == RECOVER_AT_EXEC));

      if (op->off_path) {
        bp_pred_info->recovery_point = RECOVER_AT_NONE;
      }
      return FT_EVENT_MISPREDICT;
    } else if (trace_mode && op->off_path && bp_pred_info->pred == TAKEN) {
      // in this case, not a misprediction pred is taken so oracle info should be taken
      // and this should be last op in the FT
      // no misprediction, just manually redirect
      if (pc_plus_offset != bp_pred_info->pred_npc)
        ASSERT(proc_id, op->oracle_info.dir == TAKEN);
      return FT_EVENT_OFFPATH_TAKEN_REDIRECT;
    }

  } else if (op->uop->bar_type & BAR_FETCH) {
    ASSERT(0, bp_pred_info->recovery_point == RECOVER_AT_NONE);
    return FT_EVENT_FETCH_BARRIER;
  }

  return FT_EVENT_NONE;
}

FT_PredictResult FT::predict_ft() {
  for (size_t idx = op_pos; idx < ops.size(); idx++) {
    Op* op = ops[idx];
    bp_predict_btb(g_bp_data, op);
    FT_Event event = FT_EVENT_NONE;
    if (bp_l0_enabled()) {
      INC_STAT_EVENT(proc_id, DFE_L0_ENABLED_PREDICTIONS, 1);

      const FT_Event l0_event = predict_op_ft_event(op, BP_PRED_L0);
      const Flag l0_wrong = op->bp_pred_l0.recovery_point == RECOVER_AT_FE;

      const FT_Event main_event = predict_op_ft_event(op, BP_PRED_MAIN);
      const Flag main_wrong =
          op->bp_pred_main.recovery_point == RECOVER_AT_DECODE || op->bp_pred_main.recovery_point == RECOVER_AT_EXEC;

      if (l0_wrong && !main_wrong) {
        STAT_EVENT(proc_id, DFE_L0_WRONG_MAIN_CORRECT);
        if (!op->off_path) {
          if (ended_by_exit()) {
            op_select_bp_pred_info(op, BP_PRED_MAIN);
            event = FT_EVENT_NONE;
          } else {
            op_select_bp_pred_info(op, BP_PRED_L0);

            const Counter fetch_cycle = op->bp_pred_main.bp_ready_cycle - BP_MAIN_LATENCY;
            const Flag l0_dir_wrong = op->bp_pred_l0.pred_orig != op->oracle_info.dir;
            const uns recovery_latency = l0_dir_wrong ? BP_MAIN_LATENCY : op->btb_pred_info->btb_pred_latency;
            ASSERT(proc_id, recovery_latency > BP_L0_LATENCY);
            const Counter recovery_cycle = fetch_cycle + recovery_latency - BP_L0_LATENCY;

            bp_sched_recovery(bp_recovery_info, op, recovery_cycle);
            event = l0_event;
          }
        } else {
          event = main_event;
        }
      } else if (l0_wrong && main_wrong) {
        STAT_EVENT(proc_id, DFE_L0_WRONG_MAIN_WRONG);
        op_select_bp_pred_info(op, BP_PRED_MAIN);
        event = main_event;
      } else if (!l0_wrong && main_wrong) {
        STAT_EVENT(proc_id, DFE_L0_CORRECT_MAIN_WRONG);
        op_select_bp_pred_info(op, BP_PRED_MAIN);
        event = main_event;
      } else {
        STAT_EVENT(proc_id, DFE_L0_CORRECT_MAIN_CORRECT);
        op_select_bp_pred_info(op, BP_PRED_MAIN);
        event = main_event;
      }
    } else {
      event = predict_op_ft_event(op, BP_PRED_MAIN);
      op_select_bp_pred_info(op, BP_PRED_MAIN);
    }
    bp_btb_post_bp_predict(g_bp_data, op);  // for next BTB access

    // HELIOS: Fetch hook for the main on-path stream: predict, validate, and fuse.
    // bp_pred_info (pred_global_hist) and op_num are both valid here.
    if (HELIOS_DO_FUSION && bp_id == MAIN_BP && !op->off_path) {
      heliosFetchHook(op);
      // HELIOS: a mis-fused (non-CF) load never goes through bp_predict_op, so its recovery_info
      // is never populated and the branch predictor is never rewound for its flush. Snapshot the BP
      // speculative state (heliosFetchHook has just set helios_flush_pending) so the exec-time flush can
      // restore the global/target history, CRS, and the TAGE speculative history -- otherwise
      // re-fetched younger ops predict against a doubly-advanced predictor.
      //
      // The snapshot is DEFERRED from the flushing uop to its macro's LAST (eom) uop: the flush keeps
      // the flushing op's ENTIRE macro (squash boundary = helios_macro_last_op_num) and re-steers to
      // the macro's true next PC, so the restore target is the state AFTER the whole kept macro was
      // predicted. When the macro ends in a CF uop (e.g. the implicit stack-pop load of a `ret`), a
      // snapshot taken at the load itself predates the kept CF's spec update and TAGE checkpoint:
      // restoring it (a) erases the KEPT CF's TAGE checkpoint (younger key), tripping the
      // RetireCheckpoint assert when that CF retires, and (b) loses the kept CF's history/CRS update,
      // skewing every replayed prediction. For a macro with no CF uop the BP state does not change
      // between the flushing uop and its eom, so deferral is behavior-identical there.
      if (op->helios_flush_pending)
        g_helios_pending_flush_snapshots.push_back(op);
      if (op->eom && !g_helios_pending_flush_snapshots.empty()) {
        // This op closes the pending ops' macro (pending ops are cleared on every recovery, so they
        // can only belong to the macro currently being fetched).
        ASSERT(proc_id, g_helios_pending_flush_snapshots.front()->helios_macro_last_op_num == op->op_num);
        for (Op* mem_op : g_helios_pending_flush_snapshots) {
          if (!mem_op->helios_flush_pending)
            continue;  // cancelled in the allocation queue (heliosAllocQueueHook)
          // HELIOS: stamp the flush re-steer target (read in bp.c::helios_sched_recovery) with the
          // macro's TRUE next dynamic PC. That PC is this eom uop's npc (= the trace's
          // instruction_next_addr); a non-eom mem uop's own npc is its macro's PC, so it must be
          // taken here at the eom and applied to every flush-flagged uop of the macro.
          mem_op->helios_macro_next_fetch_addr = op->oracle_info.npc;
          mem_op->recovery_info.pred_global_hist = g_bp_data->global_hist;
          mem_op->recovery_info.targ_hist = g_bp_data->targ_hist;
          mem_op->recovery_info.crs_next = g_bp_data->crs.next;
          mem_op->recovery_info.crs_tos = g_bp_data->crs.tos;
          mem_op->recovery_info.crs_depth = g_bp_data->crs.depth;
          if (BP_MECH == TAGE64K_BP)
            bp_helios_take_checkpoint_TAGE64K(g_bp_data->proc_id, bp_id, mem_op);
        }
        g_helios_pending_flush_snapshots.clear();
      }
    }

    if (op->uop->cf_type) {
      // Per-CF prediction event: now that main's bp_pred_info is finalized for
      // op (and main's bp_data has just been spec-updated), fire alt-DFE
      // _ON_PREDICTION dispatch. Single-op rollback in trigger_alt_with_rewind
      // works because main's state hasn't advanced past op yet. Off-path
      // predictions (FT::build with off_path=true) intentionally do not fire
      // this; alt _ON_PREDICTION semantics only cover main's on-path/recovery
      // predict_ft pass.
      if (bp_id == MAIN_BP)
        decoupled_fe_on_main_prediction(proc_id, op);
    }

    if (event == FT_EVENT_FETCH_BARRIER) {
      STAT_EVENT(proc_id, op->off_path ? FTQ_SAW_BAR_FETCH_OFFPATH : FTQ_SAW_BAR_FETCH_ONPATH);
    }

    if (event != FT_EVENT_NONE) {
      uint64_t return_idx = (event == FT_EVENT_MISPREDICT) ? (idx) : 0;
      Addr pred_addr = op->bp_pred_info->pred_npc;
      if (!ended_by_exit())
        return {return_idx, event, op, pred_addr};
    }
  }
  return {0, FT_EVENT_NONE, nullptr, 0};
}

FT_Info FT::get_ft_info() const {
  return ft_info;
}

std::vector<Op*>& FT::get_ops() {
  return ops;
}

Op* FT::get_last_op() const {
  return ops.back();
}
Op* FT::get_first_op() const {
  return ops.front();
}

Addr FT::get_start_addr() const {
  return ft_info.static_info.start;
}

bool FT::is_consecutive(const FT& previous_ft) const {
  ASSERT(0, previous_ft.get_last_op());
  Op* last_op = previous_ft.get_last_op();
  if (!last_op)
    return false;
  FT_Ended_By prev_end_type = previous_ft.get_ft_info().dynamic_info.ended_by;
  Addr start_addr = ft_info.static_info.start;
  const Bp_Pred_Info* bp_pred_info = ft_active_or_main_bp_pred_info(last_op);
  Addr pred_npc = bp_pred_info->pred_npc;
  Addr npc = last_op->oracle_info.npc;
  Addr end_addr = (FRONTEND == FE_PIN_EXEC_DRIVEN) ? ADDR_PLUS_OFFSET(last_op->inst->addr, last_op->inst->inst_size)
                                                   : last_op->inst->addr + last_op->inst->inst_size;
  bool matches = false;
  switch (prev_end_type) {
    case FT_TAKEN_BRANCH:
      // Next FT must start at the predicted or actual NPC.
      matches = (pred_npc == start_addr) || (npc == start_addr);
      break;
    case FT_BAR_FETCH:
      // Barrier-fetch allows either fall-through to end_addr or NPC.
      matches = (npc == start_addr) || (end_addr == start_addr);
      break;
    default:
      // Normal fall-through: next start must equal the end of last instruction.
      matches = (end_addr == start_addr);
      break;
  }
  return matches;
}

FT_Ended_By FT::get_end_reason() const {
  if (ops.empty()) {
    return FT_NOT_ENDED;
  }

  Op* op = ops.back();  // Get the last op
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst);
  if (op->eom) {
    const Bp_Pred_Info* bp_pred_info = ft_active_or_main_bp_pred_info(op);
    uns offset = ADDR_PLUS_OFFSET(op->inst->addr, op->inst->inst_size) - ROUND_DOWN(op->inst->addr, ICACHE_LINE_SIZE);
    bool end_of_icache_line = offset >= ICACHE_LINE_SIZE;
    bool cf_taken = (op->uop->cf_type && bp_pred_info->pred == TAKEN);
    bool bar_fetch = IS_CALLSYS(op->uop) || op->uop->bar_type & BAR_FETCH;

    if (op->exit) {
      return FT_APP_EXIT;
    } else if (bar_fetch) {
      return FT_BAR_FETCH;
    } else if (cf_taken) {
      return FT_TAKEN_BRANCH;
    } else if (end_of_icache_line) {
      return FT_ICACHE_LINE_BOUNDARY;
    }
  }

  return FT_NOT_ENDED;
}

void FT::generate_ft_info() {
  // first op to be read at op_pos
  auto op = ops[op_pos];
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->op_pool_valid);
  ASSERT(proc_id, op->inst);
  ASSERT(proc_id, get_last_op());
  ASSERT(proc_id, get_last_op()->op_pool_valid);
  ASSERT(proc_id, get_last_op()->inst);
  ASSERT(proc_id, op->bom && get_last_op()->eom);

  ft_info.static_info.start = op->inst->addr;
  ft_info.dynamic_info.first_op_off_path = op->off_path;
  ft_info.dynamic_info.ended_by = get_end_reason();
  ft_info.static_info.n_uops = ops.size() - op_pos;
  ft_info.static_info.length = ops.back()->inst->addr + ops.back()->inst->inst_size - ft_info.static_info.start;
  ASSERT(proc_id, ft_info.static_info.start && ft_info.static_info.length && ft_info.static_info.n_uops);
  STAT_EVENT(proc_id, POWER_BTB_READ);
}

void FT::clear_recovery_info() {
  for (auto op : ops) {
    op->bp_pred_info->recovery_point = RECOVER_AT_NONE;
  }
}

// generate_uop_cache_data was moved to uop_cache.cc as
// generate_uop_cache_data_from_FT(FT*, std::vector<Uop_Cache_Data>&)
// to keep FT class focused on FT semantics. Implementation lives in
// uop_cache.cc and is declared a friend of FT (see ft.h).

/* FT wrappers */
bool ft_can_fetch_op(FT* ft) {
  return ft->can_fetch_op();
}

Op* ft_fetch_op(FT* ft) {
  return ft->fetch_op();
}

FT_Info ft_get_ft_info(FT* ft) {
  return ft->get_ft_info();
}

static bool ft_op_recovery_addr_is_consecutive(Op* op, Addr next_start) {
  ASSERT(0, op);

  const Bp_Pred_Info* bp_pred_info = ft_active_or_main_bp_pred_info(op);
  Addr pred_npc = bp_pred_info->pred_npc;
  Addr npc = op->oracle_info.npc;
  Addr end_addr = (FRONTEND == FE_PIN_EXEC_DRIVEN) ? ADDR_PLUS_OFFSET(op->inst->addr, op->inst->inst_size)
                                                   : op->inst->addr + op->inst->inst_size;

  return (pred_npc == next_start) || (npc == next_start) || (end_addr == next_start);
}

bool ft_recovery_addr_is_consecutive(FT* ft, Addr next_start) {
  ASSERT(0, ft);
  return ft_op_recovery_addr_is_consecutive(ft->get_last_op(), next_start);
}

void assert_ft_after_recovery(uns8 proc_id, Op* op, Addr recovery_fetch_addr) {
  ASSERT(proc_id, op);
  ASSERT(proc_id, op->parent_FT);
  const bool consecutive = ft_op_recovery_addr_is_consecutive(op, recovery_fetch_addr);
  ASSERT(proc_id, consecutive);
  ASSERT(proc_id, IS_FLUSHING_OP(op));
  ASSERT(proc_id, op->eom);
}

/* retire and flush, free all ops in a FT when last op is freed */
void ft_free_op(Op* op) {
  // HELIOS flush: a shared on-path op (present in both a home FT and its off_path_ft) can be freed
  // when the sibling FT is torn down, yet still sit in a pipeline-stage slot whose recover pass then
  // calls ft_free_op on it. Skip the already-freed op -- its FTs are handled by the sibling teardown.
  // Gated on an in-progress flush recovery so the baseline path is untouched.
  if (HELIOS_DO_FUSION && bp_recovery_info->helios_flush && !op->op_pool_valid)
    return;
  ASSERT(0, op->parent_FT);

  // HELIOS mis-fusion flush: this is the only recovery that can fully squash a nested off-path FT
  // graph, which can leave `op` referencing a parent_FT / parent_FT_off_path that a sibling op's
  // ft_free_op already deleted (op was split out of that FT's ops vector by a later off-path split,
  // so FT::~FT never nulled its back-pointer). Dereferencing get_last_op() on a freed FT is a
  // use-after-free, so detect stale (already-freed) FT pointers via the liveness set and handle them
  // safely. Only active under HELIOS flushes; the baseline path is untouched.
  if (HELIOS_DO_FUSION && bp_recovery_info->helios_flush) {
    if (op->parent_FT_off_path && !ft_is_live(op->parent_FT_off_path))
      op->parent_FT_off_path = nullptr;  // off-path FT already torn down; ignore the stale pointer
    if (!ft_is_live(op->parent_FT) && !(op->parent_FT_off_path && ft_is_live(op->parent_FT_off_path))) {
      // The op's home FT is already gone AND no live off_path_ft sibling still owns it; the op is
      // orphaned. Free it directly instead of dereferencing the freed FT. (When a live sibling exists
      // the op is NOT orphaned -- the sibling's last-op delete below frees it, so fall through.)
      // op_pool_valid guards against a double free.
      if (op->op_pool_valid) {
        op->parent_FT = nullptr;
        free_op(op);
      }
      return;
    }
  }

  if (op->parent_FT_off_path && op->parent_FT_off_path->get_last_op() == op)
    free_ft(op->parent_FT_off_path);
  // HELIOS flush: the off_path_ft teardown above may have freed this shared op (symmetric ownership in
  // FT::release_ops frees an op once its last live owner is gone). If so, stop -- the parent_FT branch
  // below would dereference a freed op / a now-dangling parent_FT.
  if (HELIOS_DO_FUSION && bp_recovery_info->helios_flush && !op->op_pool_valid)
    return;
  if (!op->parent_FT_off_path && op->parent_FT->get_last_op() == op) {
    FT* ft = op->parent_FT;
    std::vector<Op*>& ft_ops = ft->get_ops();

    // HELIOS on-path flush: the squash boundary (recovery_op_num = the mis-fused load's macro last
    // uop) can fall MID-FT, because a load is not a taken-branch FT boundary the way a normal
    // branch recovery is. The squashed tail (op_num > recovery_op_num) is being freed here via the
    // FT's last op, but the SAME FT also holds KEPT ops (op_num <= recovery_op_num) that must
    // survive and retire normally. Deleting the whole FT would free those kept ops too (FT::~FT
    // frees every on-path op), leaving dangling seq_op_list / backend references that the op pool
    // later recycles into younger ops -> corruption. Trim only the squashed tail and keep the FT
    // alive with its kept ops (mirrors the mixed on/off-path cleanup below, keyed on op_num).
    if (HELIOS_DO_FUSION && bp_recovery_info->helios_flush && !ft_ops.empty() &&
        ft_ops.front()->op_num <= bp_recovery_info->recovery_op_num &&
        ft_ops.back()->op_num > bp_recovery_info->recovery_op_num) {
      while (!ft_ops.empty() && ft_ops.back()->op_num > bp_recovery_info->recovery_op_num) {
        Op* tail = ft_ops.back();
        ft_ops.pop_back();
        // A squashed tail op may also be a divergence point that owns a (still-live) off-path FT;
        // tear that down if tail is its last op so it is not left dangling. Then free the op once.
        if (tail->parent_FT_off_path && ft_is_live(tail->parent_FT_off_path) &&
            tail->parent_FT_off_path->get_last_op() == tail)
          free_ft(tail->parent_FT_off_path);
        if (tail->op_pool_valid) {
          tail->parent_FT = nullptr;
          free_op(tail);
        }
      }
      ft->op_pos = 0;
      ft->generate_ft_info();
      ft->op_pos = ft_ops.size();
      return;
    }

    Flag has_on_path = FALSE;
    Flag has_off_path = FALSE;
    for (Op* ft_op : ft_ops) {
      has_off_path |= ft_op->off_path;
      has_on_path |= !ft_op->off_path;
      if (has_on_path && has_off_path)
        break;
    }

    // Mixed-path FT: drop all off-path ops so recovery sees a pure on-path FT.
    if (has_on_path && has_off_path) {
      DEBUG(op->proc_id, "[DFE%u] ft_free_op mixed FT cleanup: ft_id:%llu trigger_op:%llu total_ops:%zu\n",
            ft->get_bp_id(), (unsigned long long)ft->get_ft_info().dynamic_info.FT_id, (unsigned long long)op->op_num,
            ft_ops.size());
      while (!ft_ops.empty() && ft_ops.back()->off_path) {
        Op* tail = ft_ops.back();
        DEBUG(op->proc_id, "[DFE%u] ft_free_op removing off-path op_num:%llu addr:0x%llx\n", ft->get_bp_id(),
              (unsigned long long)tail->op_num, (unsigned long long)tail->inst->addr);
        ft_ops.pop_back();
        free_op(tail);
      }
      // Regenerate ft_info to reflect the on-path-only ops, then mark all as fetched.
      ft->op_pos = 0;
      ft->generate_ft_info();
      ft->op_pos = ft_ops.size();
      return;
    }

    // Uniform-path FT (all on-path or all off-path): recycle the FT as before.
    free_ft(ft);
  }
}

/* use set to avoid duplicates and keep PC order */
std::set<Addr> FT::get_pcs() {
  std::set<Addr> pc_set;
  for (auto op : ops) {
    if (op && op->inst) {
      pc_set.insert(op->inst->addr);
    }
  }
  return pc_set;
}
