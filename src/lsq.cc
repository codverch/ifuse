/*
 * Copyright 2025 University of California Santa Cruz
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
 * File         : lsq.cc
 * Author       : Litz Lab
 * Date         : 7/2025
 * Description  : Load/Store Queue
 ***************************************************************************************/

#include "lsq.h"

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "bp/bp.h"

#include "exec_ports.h"
#include "node_stage.h"

#include "prefetcher/rfp.param.h"
#include "statistics.h"
}

#include <deque>
#include <vector>

/**************************************************************************************/
/* Definition */

struct LSQ_Entry {
  Op* op = nullptr;
  Counter op_num = 0;
  Counter unique_num = 0;
  Flag off_path = 0;
  Mem_Type mem_type = {};
  /* RFP-only fields. An RFP FIFO entry reuses LSQ_Entry and points at its load's Op. */
  Addr rfp_predicted_line_addr = 0;
  uns16 rfp_prfid = 0;
  Counter rfp_launch_cycle = 0;

  LSQ_Entry() {}
  LSQ_Entry(Op* mem_op)
      : op(mem_op),
        op_num(mem_op->op_num),
        unique_num(mem_op->unique_num),
        off_path(mem_op->off_path),
        mem_type(mem_op->uop->mem_type) {}
  LSQ_Entry(Op* owner_op, Addr predicted_line_addr, uns16 prfid, Counter launch_cycle)
      : op(owner_op),
        op_num(owner_op->op_num),
        unique_num(owner_op->unique_num),
        off_path(owner_op->off_path),
        mem_type(MEM_LD),
        rfp_predicted_line_addr(predicted_line_addr),
        rfp_prfid(prfid),
        rfp_launch_cycle(launch_cycle) {}
};

class LSQ {
 private:
  uns8 proc_id = 0;
  Mem_Type mem_type = {};
  size_t entry_num = 0;
  /* Dedicated RFP FIFO capacity (separate from demand entry_num). Only the load queue has one. */
  size_t rfp_entry_num = 0;

  std::deque<LSQ_Entry> entries;
  /* RFP FIFO, oldest first. Filled at rename (rfp.c:rfp_prefetch_launch_prf) and drained by
     dcache_stage.c:dcache_process_lsq_rfp_queue. Requests never occupy demand entries. */
  std::deque<LSQ_Entry> rfp_fifo_entries;

 public:
  void init(uns8 proc_id, Mem_Type mem_type, size_t entry_num);
  void allocate(Op* mem_op);
  Flag allocate_rfp(Op* owner_op, Addr predicted_line_addr, uns16 prfid, Counter launch_cycle);
  void free(Op* mem_op);
  bool available();
  void recover(Counter flush_op_num);
  void drop_rfp_upto(Counter op_num);
  Flag peek_rfp(Lsq_Rfp_Req* out_req) const;
  void pop_rfp();

  LSQ(){};
  const std::deque<LSQ_Entry>& get_entries() const { return entries; }
};

/* Initialize the LSQ */
void LSQ::init(const uns8 proc_id, const Mem_Type mem_type, const size_t entry_num) {
  this->proc_id = proc_id;
  this->mem_type = mem_type;
  this->entry_num = entry_num;
  this->rfp_entry_num = (mem_type == MEM_LD) ? RFP_LSQ_FIFO_SIZE : 0;
  entries.clear();
  rfp_fifo_entries.clear();
}

/* Allocate an entry for a load operation */
void LSQ::allocate(Op* mem_op) {
  ASSERT(proc_id, entries.size() < entry_num);
  ASSERT(proc_id, mem_op->uop->mem_type == this->mem_type);

  entries.emplace_back(mem_op);
}

/* Allocate an entry in the RFP FIFO for an RFP prefetch request */
Flag LSQ::allocate_rfp(Op* owner_op, Addr predicted_line_addr, uns16 prfid, Counter launch_cycle) {
  if (!owner_op || owner_op->uop->mem_type != MEM_LD)
    return FALSE;
  
  if (rfp_fifo_entries.size() >= rfp_entry_num) {
    STAT_EVENT(proc_id, RFP_LSQ_FIFO_FULL);
    return FALSE;
  }
  
  rfp_fifo_entries.emplace_back(owner_op, predicted_line_addr, prfid, launch_cycle);
  return TRUE;
}

/* Remove an entry from the LSQ. Searches by op instead of popping the front, as on hpca2027-rfp. */
void LSQ::free(Op* mem_op) {
  ASSERT(proc_id, !entries.empty());
  ASSERT(proc_id, mem_op->uop->mem_type == this->mem_type);
  ASSERT(proc_id, !mem_op->off_path);

  for (auto it = entries.begin(); it != entries.end(); ++it) {
    if (it->op_num == mem_op->op_num && it->unique_num == mem_op->unique_num) {
      entries.erase(it);
      return;
    }
  }
  ASSERT(proc_id, FALSE);
}

/* Check if the LSQ is available for a load operation */
bool LSQ::available() {
  ASSERT(proc_id, entries.size() <= entry_num);
  if (entries.size() == entry_num) {
    return false;
  }

  return true;
}

/* Recover the LSQ from a flush */
void LSQ::recover(Counter flush_op_num) {
  while (!entries.empty()) {
    auto& back_entry = entries.back();

    // Stop when reaching on-path ops earlier than the branch
    if (back_entry.op_num < flush_op_num) {
      break;
    }

    // Free younger demand entries from the back.
    ASSERT(proc_id, !entries.empty());
    ASSERT(proc_id, back_entry.op->off_path);
    ASSERT(proc_id, entries.back().op_num == back_entry.op->op_num);
    ASSERT(proc_id, back_entry.op->uop->mem_type == this->mem_type);
    entries.pop_back();
  }

  /* Scan the RFP FIFO and remove entries that are older than the flush operation */
  for (auto it = rfp_fifo_entries.begin(); it != rfp_fifo_entries.end();) {
    if (it->op_num >= flush_op_num)
      it = rfp_fifo_entries.erase(it);
    else
      ++it;
  }
}

/* Drop RFP entries up to a given op number. Called at load commit, since a retired load no longer
   needs its prefetch. */
void LSQ::drop_rfp_upto(Counter op_num) {
  for (auto it = rfp_fifo_entries.begin(); it != rfp_fifo_entries.end();) {
    if (it->op_num <= op_num)
      it = rfp_fifo_entries.erase(it);
    else
      ++it;
  }
}

/* Peek at the oldest RFP prefetch request in the FIFO */
Flag LSQ::peek_rfp(Lsq_Rfp_Req* out_req) const {
  if (!out_req || rfp_fifo_entries.empty())
    return FALSE;
  const auto& entry = rfp_fifo_entries.front();
  out_req->owner_op = entry.op;
  out_req->owner_unique = entry.unique_num;
  out_req->owner_op_num = entry.op_num;
  out_req->predicted_line_addr = entry.rfp_predicted_line_addr;
  out_req->prfid = entry.rfp_prfid;
  out_req->launch_cycle = entry.rfp_launch_cycle;
  return TRUE;
}

/* Pop the oldest RFP prefetch request from the FIFO */
void LSQ::pop_rfp() {
  if (!rfp_fifo_entries.empty())
    rfp_fifo_entries.pop_front();
}

/**************************************************************************************/

class LSQ_Unit {
 private:
  uns8 proc_id = 0;
  LSQ load_queue;
  LSQ store_queue;

 public:
  LSQ_Unit(uns8 proc_id);
  const LSQ* get_queue(Mem_Type mem_type) const;

  void init(uns8 proc_id);
  Flag available(Mem_Type mem_type);
  void dispatch(Op* mem_op);
  void recover(Counter flush_op_num);
  void commit(Op* mem_op);
  Flag rfp_enqueue(Op* owner_op, Addr predicted_line_addr, uns16 prfid, Counter launch_cycle);
  Flag rfp_peek(Lsq_Rfp_Req* out_req) const;
  void rfp_pop();
  Flag older_store_overlaps(Counter younger_op_num, Addr addr, uns size) const;
};

LSQ_Unit::LSQ_Unit(uns8 proc_id) {
  this->init(proc_id);
}

const LSQ* LSQ_Unit::get_queue(Mem_Type mem_type) const {
  switch (mem_type) {
    case MEM_LD:
      return &load_queue;

    case MEM_ST:
      return &store_queue;

    default:
      ASSERT(this->proc_id, FALSE);
      break;
  }

  return nullptr;
}

void LSQ_Unit::init(uns8 proc_id) {
  load_queue.init(proc_id, MEM_LD, LOAD_QUEUE_ENTRY_NUM);
  store_queue.init(proc_id, MEM_ST, STORE_QUEUE_ENTRY_NUM);
}

Flag LSQ_Unit::available(Mem_Type mem_type) {
  switch (mem_type) {
    case MEM_LD:
      return static_cast<Flag>(load_queue.available());

    case MEM_ST:
      return static_cast<Flag>(store_queue.available());

    default:
      ASSERT(this->proc_id, FALSE);
      break;
  }

  return FALSE;
}

void LSQ_Unit::dispatch(Op* mem_op) {
  switch (mem_op->uop->mem_type) {
    case MEM_LD:
      load_queue.allocate(mem_op);
      break;

    case MEM_ST:
      store_queue.allocate(mem_op);
      break;

    default:
      ASSERT(this->proc_id, FALSE);
      break;
  }
}

void LSQ_Unit::recover(Counter flush_op_num) {
  load_queue.recover(flush_op_num);
  store_queue.recover(flush_op_num);
}

void LSQ_Unit::commit(Op* mem_op) {
  switch (mem_op->uop->mem_type) {
    case MEM_LD:
      load_queue.free(mem_op);
      load_queue.drop_rfp_upto(mem_op->op_num);
      break;

    case MEM_ST:
      store_queue.free(mem_op);
      break;

    default:
      ASSERT(mem_op->proc_id, FALSE);
      break;
  }
}

Flag LSQ_Unit::rfp_enqueue(Op* owner_op, Addr predicted_line_addr, uns16 prfid, Counter launch_cycle) {
  return load_queue.allocate_rfp(owner_op, predicted_line_addr, prfid, launch_cycle);
}

Flag LSQ_Unit::rfp_peek(Lsq_Rfp_Req* out_req) const {
  return load_queue.peek_rfp(out_req);
}

void LSQ_Unit::rfp_pop() {
  load_queue.pop_rfp();
}

/* ISCA'22 §3.2.1 — scan older in-flight stores (youngest-first) for address overlap. */
Flag LSQ_Unit::older_store_overlaps(Counter younger_op_num, Addr addr, uns size) const {
  uns st_size;
  Addr st_va;

  if (!addr || !size)
    return FALSE;

  for (const auto& entry : store_queue.get_entries()) {
    Op* st_op;

    if (entry.op_num >= younger_op_num)
      continue;

    st_op = entry.op;
    if (!st_op || st_op->off_path)
      continue;

    st_va = st_op->oracle_info.va;
    st_size = st_op->oracle_info.mem_size;
    if (!st_size)
      st_size = st_op->uop->mem_size;

    /* Store address not yet known — MD would make RFP wait (§3.2.1). */
    if (!st_va)
      return TRUE;

    if (BYTE_OVERLAP(st_va, st_size, addr, size))
      return TRUE;
  }

  return FALSE;
}

/**************************************************************************************/
/* Global Values */

static std::vector<LSQ_Unit> per_core_lsq_unit;
LSQ_Unit* lsq_unit = nullptr;

/**************************************************************************************/
/* External Methods */

void alloc_mem_lsq(uns num_cores) {
  if (!LSQ_ENABLE)
    return;

  for (uns ii = 0; ii < num_cores; ii++) {
    per_core_lsq_unit.push_back(LSQ_Unit(ii));
  }
}

void set_lsq(uns8 proc_id) {
  if (!LSQ_ENABLE)
    return;

  lsq_unit = &per_core_lsq_unit[proc_id];
}

void init_lsq(uns8 proc_id, const char* name) {
  if (!LSQ_ENABLE)
    return;

  lsq_unit->init(proc_id);
}

void recover_lsq() {
  if (!LSQ_ENABLE)
    return;

  lsq_unit->recover(bp_recovery_info->recovery_op_num);
}

/**************************************************************************************/

/*
  Called by:
  --- node_stage.c -> before a mem op is filled into ROB
  Desc:
  --- return TRUE if there is an available entry
*/
Flag lsq_available(Mem_Type mem_type) {
  if (!LSQ_ENABLE)
    return TRUE;

  return lsq_unit->available(mem_type);
}

/*
  Called by:
  --- node_stage.c -> after a mem op is filled into ROB
  Desc:
  --- allocate an load/store entry
*/
void lsq_dispatch(Op* mem_op) {
  if (!LSQ_ENABLE)
    return;

  ASSERT(mem_op->proc_id, mem_op->uop->mem_type);
  lsq_unit->dispatch(mem_op);
}

/*
  Called by:
  --- node_stage.c -> when a mem op is retired
  Desc:
  --- free the entry
*/
void lsq_commit(Op* mem_op) {
  if (!LSQ_ENABLE)
    return;

  ASSERT(mem_op->proc_id, mem_op->uop->mem_type);
  lsq_unit->commit(mem_op);
}

/**************************************************************************************/

Flag lsq_rfp_enqueue(uns8 proc_id, Op* owner_op, Addr predicted_line_addr, uns16 prfid, Counter launch_cycle) {
  if (!LSQ_ENABLE)
    return FALSE;
  if (!owner_op || proc_id >= per_core_lsq_unit.size())
    return FALSE;
  return per_core_lsq_unit[proc_id].rfp_enqueue(owner_op, predicted_line_addr, prfid, launch_cycle);
}

Flag lsq_rfp_peek(uns8 proc_id, Lsq_Rfp_Req* out_req) {
  if (!LSQ_ENABLE || proc_id >= per_core_lsq_unit.size())
    return FALSE;
  return per_core_lsq_unit[proc_id].rfp_peek(out_req);
}

void lsq_rfp_pop(uns8 proc_id) {
  if (!LSQ_ENABLE || proc_id >= per_core_lsq_unit.size())
    return;
  per_core_lsq_unit[proc_id].rfp_pop();
}

Flag lsq_older_store_overlaps(uns8 proc_id, Counter younger_op_num, Addr addr, uns size) {
  if (!LSQ_ENABLE || proc_id >= per_core_lsq_unit.size())
    return FALSE;
  return per_core_lsq_unit[proc_id].older_store_overlaps(younger_op_num, addr, size);
}

/**************************************************************************************/

int lsq_get_in_flight_load_num() {
  if (!LSQ_ENABLE)
    return 0;

  int in_flight_num = 0;
  const auto& load_entries = lsq_unit->get_queue(MEM_LD)->get_entries();
  for (const auto& entry : load_entries) {
    if (entry.op && entry.op->state >= OS_IN_RS)
      in_flight_num++;
  }

  return in_flight_num;
}
