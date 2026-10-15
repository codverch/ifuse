#include "ifuse_rename.h"

#include "ifuse_apt.h"
#include "../map.h"
#include "../map_rename.h"
#include "../globals/assert.h"
#include "../isa/isa.h"
#include "../isa/isa_macros.h"
#include "../statistics.h"
#include "ifuse_stats.h"

/**
 * I-Fuse speculative-register lifecycle:
 *
 * 1. LD1 reaches rename and allocates one physical GPR reserved for the
 *    predicted LD2 result. APT holds the register. It is not yet
 *    installed as LD2's architectural destination.
 * 2. When the matching LD2 reaches rename and fusion is correct, LD2
 *    takes LD1's reserved register as its destination
 *    (ifuse_take_ld2_dest_reg()) and allocates no register of its own.
 *    Dependents and reg_file_produce() use that register.
 * 3. After the handoff, the register's lifetime follows normal rename
 *    (flush and commit). The reserved register is no longer tracked as
 *    an I-Fuse "extra".
 * 4. If LD2 never arrives, validation fails, or APT cleanup removes the
 *    entry, the reserved register returns to the free list.
 */

static Flag ifuse_is_gp_arch_reg(int arch_reg_id) {
  return (arch_reg_id >= REG_RAX && arch_reg_id < REG_CS) ||
         (arch_reg_id >= REG_TMP0 && arch_reg_id <= REG_TMP4) ||
         (arch_reg_id >= REG_ZPS && arch_reg_id < REG_ZMM0);
}

/**
 * Allocates one physical GPR reserved for a predicted LD2 result.
 */
static int ifuse_alloc_ld2_physical_reg(Op* ld1_op) {
  struct reg_table* physical_reg_table =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
          ->reg_table[REG_TABLE_TYPE_PHYSICAL];

  if (physical_reg_table->free_list->reg_free_num == 0) {
    return REG_TABLE_REG_ID_INVALID;
  }

  struct reg_table_entry* entry =
      physical_reg_table->free_list->ops->alloc(physical_reg_table->free_list);

  /*
   * The register stays outside the arch map until LD2's rename installs
   * it as LD2's destination.
   */
  entry->ops->write(entry, ld1_op, REG_TABLE_REG_ID_INVALID);
  entry->num_refs++;

  ifuse_extra_reg_note_alloc();
  return entry->self_reg_id;
}

/**
 * Releases an unconsumed I-Fuse-reserved physical register back to the
 * pool.
 */
void ifuse_free_ld2_physical_reg(unsigned int ld2_physical_reg_id) {
  if (ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
    return;
  }

  struct reg_table* physical_reg_table =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
          ->reg_table[REG_TABLE_TYPE_PHYSICAL];
  struct reg_table_entry* entry =
      &physical_reg_table->entries[ld2_physical_reg_id];

  physical_reg_table->ops->free(physical_reg_table, entry);
  ifuse_extra_reg_note_free();
}

/**
 * Returns the physical register LD1 reserved for this LD2, to be used as
 * LD2's destination in place of a fresh allocation, or
 * REG_TABLE_REG_ID_INVALID if there is none. Called from
 * reg_file_write_dst() for each destination of each op. Only an LD2's
 * first general-purpose destination takes the reservation, because the
 * APT entry is consumed here.
 *
 * @param op The op being renamed.
 * @param arch_reg_id The destination's architectural register.
 * @return The reserved physical register, or REG_TABLE_REG_ID_INVALID.
 */
int ifuse_take_ld2_dest_reg(Op* op, int arch_reg_id) {
  if (op->ifuse_load_role != LOAD2 || !ifuse_is_gp_arch_reg(arch_reg_id)) {
    return REG_TABLE_REG_ID_INVALID;
  }

  unsigned int reserved_id = REG_TABLE_REG_ID_INVALID;
  if (!apt_take_ld2_physical_reg_id(op->inst_info->addr,
                                    (unsigned int)op->ifuse_partner_op_num,
                                    &reserved_id)) {
    return REG_TABLE_REG_ID_INVALID;
  }

  struct reg_table* physical_reg_table =
      map_data->reg_file[REG_FILE_REG_TYPE_GENERAL_PURPOSE]
          ->reg_table[REG_TABLE_TYPE_PHYSICAL];
  struct reg_table_entry* reserved = &physical_reg_table->entries[reserved_id];
  ASSERT(op->proc_id, reserved->reg_state != REG_TABLE_ENTRY_STATE_FREE);
  ASSERT(op->proc_id, reserved->num_refs > 0);

  // The register becomes LD2's ordinary destination. Normal flush and
  // commit manage it from here.
  reserved->ops->write(reserved, op, arch_reg_id);
  ifuse_extra_reg_note_free();
  return (int)reserved_id;
}

void ifuse_rename_op(Op* op) {
  if (op->ifuse_load_role == LOAD1) {
    int ld2_physical_reg_id = ifuse_alloc_ld2_physical_reg(op);

    /* APT owns the reserved register until LD2's rename or stale cleanup */
    if (ld2_physical_reg_id != REG_TABLE_REG_ID_INVALID &&
        !apt_set_ld2_physical_reg_id((unsigned int)op->op_num,
                                     ld2_physical_reg_id)) {
      ifuse_free_ld2_physical_reg(ld2_physical_reg_id);
    }
    return;
  }

  if (op->ifuse_load_role == LOAD2) {
    // ifuse_take_ld2_dest_reg() already gave LD2 the reserved register.
    // If LD2 had no general-purpose destination to take it, return the
    // reservation to the free list.
    unsigned int ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
    if (apt_take_ld2_physical_reg_id(op->inst_info->addr,
                                     (unsigned int)op->ifuse_partner_op_num,
                                     &ld2_physical_reg_id)) {
      ifuse_free_ld2_physical_reg(ld2_physical_reg_id);
    }
    op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
    return;
  }

  if (op->ifuse_load_role == PREDICTED_NOT_FUSED) {
    /*
     * Validation failed. LD2 keeps its ordinary destination, so free the
     * reservation.
     */
    apt_remove_entry_by_ld1_micro_op(op->inst_info->addr,
                                     (unsigned int)op->ifuse_partner_op_num);
  }
}

void ifuse_retire_op(Op* op) {
  if (op->ifuse_ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
    return;
  }

  /*
   * This point is reached only if a reserved register was still tracked
   * as an I-Fuse sidecar after rename. A successful fused handoff clears
   * this ID, so commit alone manages the destination register.
   */
  ifuse_free_ld2_physical_reg(op->ifuse_ld2_physical_reg_id);
  op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
}

void ifuse_recover_op(Op* op) {
  if (op->ifuse_ld2_physical_reg_id == REG_TABLE_REG_ID_INVALID) {
    return;
  }

  /*
   * This mirrors retire. A successful handoff clears this ID, and
   * flush_mispredict already frees dst_reg_id. Free the register here
   * only if it remained an unbound I-Fuse reservation.
   */
  ifuse_free_ld2_physical_reg(op->ifuse_ld2_physical_reg_id);
  op->ifuse_ld2_physical_reg_id = REG_TABLE_REG_ID_INVALID;
}
