/*
 * Copyright (c) 2013, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_G1_G1GCPHASETIMES_HPP
#define SHARE_VM_GC_G1_G1GCPHASETIMES_HPP

#include "gc/shared/referenceProcessorPhaseTimes.hpp"
#include "gc/shared/weakProcessorPhaseTimes.hpp"
#include "logging/logLevel.hpp"
#include "memory/allocation.hpp"
#include "utilities/macros.hpp"

class LineBuffer;
class G1ParScanThreadState;
class STWGCTimer;

template <class T> class WorkerDataArray;

class G1GCPhaseTimes : public CHeapObj<mtGC> {
  uint _max_gc_threads;
  jlong _gc_start_counter;
  double _gc_pause_time_ms;

 public:
  enum GCParPhases {
    GCWorkerStart,
    ExtRootScan,
    ThreadRoots,
    StringTableRoots,
    UniverseRoots,
    JNIRoots,
    ObjectSynchronizerRoots,
    ManagementRoots,
    SystemDictionaryRoots,
    CLDGRoots,
    JVMTIRoots,
    CMRefRoots,
    WaitForStrongCLD,
    WeakCLDRoots,
    SATBFiltering,
    UpdateRS,
    ScanHCC,
    ScanRS,
    OptScanRS,
    CodeRoots,
#if INCLUDE_AOT
    AOTCodeRoots,
#endif
    ObjCopy,
    OptObjCopy,
    Termination,
    Other,
    GCWorkerTotal,
    GCWorkerEnd,
    StringDedupQueueFixup,
    StringDedupTableFixup,
    RedirtyCards,
    YoungFreeCSet,
    NonYoungFreeCSet,
    GCParPhasesSentinel
  };

  enum GCScanRSWorkItems {
    ScanRSScannedCards,
    ScanRSClaimedCards,
    ScanRSSkippedCards
  };

  enum GCUpdateRSWorkItems {
    UpdateRSProcessedBuffers,
    UpdateRSScannedCards,
    UpdateRSSkippedCards
  };

  enum GCOptCSetWorkItems {
      OptCSetScannedCards,
      OptCSetClaimedCards,
      OptCSetSkippedCards,
      OptCSetUsedMemory
  };

 private:
  // Markers for grouping the phases in the GCPhases enum above
  static const int GCMainParPhasesLast = GCWorkerEnd;
  static const int StringDedupPhasesFirst = StringDedupQueueFixup;
  static const int StringDedupPhasesLast = StringDedupTableFixup;

  WorkerDataArray<double>* _gc_par_phases[GCParPhasesSentinel];

  WorkerDataArray<size_t>* _update_rs_processed_buffers;
  WorkerDataArray<size_t>* _update_rs_scanned_cards;
  WorkerDataArray<size_t>* _update_rs_skipped_cards;

  WorkerDataArray<size_t>* _scan_rs_scanned_cards;
  WorkerDataArray<size_t>* _scan_rs_claimed_cards;
  WorkerDataArray<size_t>* _scan_rs_skipped_cards;

  WorkerDataArray<size_t>* _opt_cset_scanned_cards;
  WorkerDataArray<size_t>* _opt_cset_claimed_cards;
  WorkerDataArray<size_t>* _opt_cset_skipped_cards;
  WorkerDataArray<size_t>* _opt_cset_used_memory;

  WorkerDataArray<size_t>* _termination_attempts;

  WorkerDataArray<size_t>* _redirtied_cards;

  double _cur_collection_par_time_ms;
  double _cur_optional_evac_ms;
  double _cur_collection_code_root_fixup_time_ms;
  double _cur_strong_code_root_purge_time_ms;

  double _cur_evac_fail_recalc_used;
  double _cur_evac_fail_remove_self_forwards;

  double _cur_string_dedup_fixup_time_ms;

  double _cur_prepare_tlab_time_ms;
  double _cur_resize_tlab_time_ms;

  double _cur_derived_pointer_table_update_time_ms;

  double _cur_clear_ct_time_ms;
  double _cur_expand_heap_time_ms;
  double _cur_ref_proc_time_ms;

  double _cur_collection_start_sec;
  double _root_region_scan_wait_time_ms;

  double _external_accounted_time_ms;

  double _recorded_clear_claimed_marks_time_ms;

  double _recorded_young_cset_choice_time_ms;
  double _recorded_non_young_cset_choice_time_ms;

  double _recorded_redirty_logged_cards_time_ms;

  double _recorded_preserve_cm_referents_time_ms;

  double _recorded_merge_pss_time_ms;

  double _recorded_start_new_cset_time_ms;

  double _recorded_total_free_cset_time_ms;

  double _recorded_serial_free_cset_time_ms;

  double _cur_fast_reclaim_humongous_time_ms;
  double _cur_fast_reclaim_humongous_register_time_ms;
  size_t _cur_fast_reclaim_humongous_total;
  size_t _cur_fast_reclaim_humongous_candidates;
  size_t _cur_fast_reclaim_humongous_reclaimed;

  double _cur_verify_before_time_ms;
  double _cur_verify_after_time_ms;

  ReferenceProcessorPhaseTimes _ref_phase_times;
  WeakProcessorPhaseTimes _weak_phase_times;

  double worker_time(GCParPhases phase, uint worker);
  void note_gc_end();
  void reset();

  template <class T>
  void details(T* phase, const char* indent) const;

  void log_phase(WorkerDataArray<double>* phase, uint indent, outputStream* out, bool print_sum) const;
  void debug_phase(WorkerDataArray<double>* phase) const;
  void trace_phase(WorkerDataArray<double>* phase, bool print_sum = true) const;

  void info_time(const char* name, double value) const;
  void debug_time(const char* name, double value) const;
  // This will print logs for both 'gc+phases' and 'gc+phases+ref'.
  void debug_time_for_reference(const char* name, double value) const;
  void trace_time(const char* name, double value) const;
  void trace_count(const char* name, size_t value) const;

  double print_pre_evacuate_collection_set() const;
  double print_evacuate_collection_set() const;
  double print_evacuate_optional_collection_set() const;
  double print_post_evacuate_collection_set() const;
  void print_other(double accounted_ms) const;

 public:
  G1GCPhaseTimes(STWGCTimer* gc_timer, uint max_gc_threads);
  void note_gc_start();
  void print();

  // record the time a phase took in seconds
  void record_time_secs(GCParPhases phase, uint worker_i, double secs);

  // add a number of seconds to a phase
  void add_time_secs(GCParPhases phase, uint worker_i, double secs);

  void record_or_add_time_secs(GCParPhases phase, uint worker_i, double secs);

  void record_or_add_objcopy_time_secs(uint worker_i, double secs);

  void record_thread_work_item(GCParPhases phase, uint worker_i, size_t count, uint index = 0);

  void record_or_add_thread_work_item(GCParPhases phase, uint worker_i, size_t count, uint index = 0);

  // return the average time for a phase in milliseconds
  double average_time_ms(GCParPhases phase);

  size_t sum_thread_work_items(GCParPhases phase, uint index = 0);

 public:

  void record_prepare_tlab_time_ms(double ms) {
    _cur_prepare_tlab_time_ms = ms;
  }

  void record_resize_tlab_time_ms(double ms) {
    _cur_resize_tlab_time_ms = ms;
  }

  void record_derived_pointer_table_update_time(double ms) {
    _cur_derived_pointer_table_update_time_ms = ms;
  }

  void record_clear_ct_time(double ms) {
    _cur_clear_ct_time_ms = ms;
  }

  void record_expand_heap_time(double ms) {
    _cur_expand_heap_time_ms = ms;
  }

  void record_par_time(double ms) {
    _cur_collection_par_time_ms = ms;
  }

  void record_optional_evacuation(double ms) {
    _cur_optional_evac_ms = ms;
  }

  void record_code_root_fixup_time(double ms) {
    _cur_collection_code_root_fixup_time_ms = ms;
  }

  void record_strong_code_root_purge_time(double ms) {
    _cur_strong_code_root_purge_time_ms = ms;
  }

  void record_evac_fail_recalc_used_time(double ms) {
    _cur_evac_fail_recalc_used = ms;
  }

  void record_evac_fail_remove_self_forwards(double ms) {
    _cur_evac_fail_remove_self_forwards = ms;
  }

  void record_string_dedup_fixup_time(double ms) {
    _cur_string_dedup_fixup_time_ms = ms;
  }

  void record_ref_proc_time(double ms) {
    _cur_ref_proc_time_ms = ms;
  }

  void record_root_region_scan_wait_time(double time_ms) {
    _root_region_scan_wait_time_ms = time_ms;
  }

  void record_total_free_cset_time_ms(double time_ms) {
    _recorded_total_free_cset_time_ms = time_ms;
  }

  void record_serial_free_cset_time_ms(double time_ms) {
    _recorded_serial_free_cset_time_ms = time_ms;
  }

  void record_fast_reclaim_humongous_stats(double time_ms, size_t total, size_t candidates) {
    _cur_fast_reclaim_humongous_register_time_ms = time_ms;
    _cur_fast_reclaim_humongous_total = total;
    _cur_fast_reclaim_humongous_candidates = candidates;
  }

  void record_fast_reclaim_humongous_time_ms(double value, size_t reclaimed) {
    _cur_fast_reclaim_humongous_time_ms = value;
    _cur_fast_reclaim_humongous_reclaimed = reclaimed;
  }

  void record_young_cset_choice_time_ms(double time_ms) {
    _recorded_young_cset_choice_time_ms = time_ms;
  }

  void record_non_young_cset_choice_time_ms(double time_ms) {
    _recorded_non_young_cset_choice_time_ms = time_ms;
  }

  void record_redirty_logged_cards_time_ms(double time_ms) {
    _recorded_redirty_logged_cards_time_ms = time_ms;
  }

  void record_preserve_cm_referents_time_ms(double time_ms) {
    _recorded_preserve_cm_referents_time_ms = time_ms;
  }

  void record_merge_pss_time_ms(double time_ms) {
    _recorded_merge_pss_time_ms = time_ms;
  }

  void record_start_new_cset_time_ms(double time_ms) {
    _recorded_start_new_cset_time_ms = time_ms;
  }

  void record_cur_collection_start_sec(double time_ms) {
    _cur_collection_start_sec = time_ms;
  }

  void record_verify_before_time_ms(double time_ms) {
    _cur_verify_before_time_ms = time_ms;
  }

  void record_verify_after_time_ms(double time_ms) {
    _cur_verify_after_time_ms = time_ms;
  }

  void inc_external_accounted_time_ms(double time_ms) {
    _external_accounted_time_ms += time_ms;
  }

  void record_clear_claimed_marks_time_ms(double recorded_clear_claimed_marks_time_ms) {
    _recorded_clear_claimed_marks_time_ms = recorded_clear_claimed_marks_time_ms;
  }

  double cur_collection_start_sec() {
    return _cur_collection_start_sec;
  }

  double cur_collection_par_time_ms() {
    return _cur_collection_par_time_ms;
  }

  double cur_clear_ct_time_ms() {
    return _cur_clear_ct_time_ms;
  }

  double cur_expand_heap_time_ms() {
    return _cur_expand_heap_time_ms;
  }

  double root_region_scan_wait_time_ms() {
    return _root_region_scan_wait_time_ms;
  }

  double young_cset_choice_time_ms() {
    return _recorded_young_cset_choice_time_ms;
  }

  double total_free_cset_time_ms() {
    return _recorded_total_free_cset_time_ms;
  }

  double non_young_cset_choice_time_ms() {
    return _recorded_non_young_cset_choice_time_ms;
  }

  double fast_reclaim_humongous_time_ms() {
    return _cur_fast_reclaim_humongous_time_ms;
  }

  ReferenceProcessorPhaseTimes* ref_phase_times() { return &_ref_phase_times; }

  WeakProcessorPhaseTimes* weak_phase_times() { return &_weak_phase_times; }
};

class G1EvacPhaseWithTrimTimeTracker : public StackObj {
  G1ParScanThreadState* _pss;
  Ticks _start;

  Tickspan& _total_time;
  Tickspan& _trim_time;

  bool _stopped;
public:
  G1EvacPhaseWithTrimTimeTracker(G1ParScanThreadState* pss, Tickspan& total_time, Tickspan& trim_time);
  ~G1EvacPhaseWithTrimTimeTracker();

  void stop();
};

class G1GCParPhaseTimesTracker : public CHeapObj<mtGC> {
protected:
  Ticks _start_time;
  G1GCPhaseTimes::GCParPhases _phase;
  G1GCPhaseTimes* _phase_times;
  uint _worker_id;
public:
  G1GCParPhaseTimesTracker(G1GCPhaseTimes* phase_times, G1GCPhaseTimes::GCParPhases phase, uint worker_id);
  virtual ~G1GCParPhaseTimesTracker();
};

class G1EvacPhaseTimesTracker : public G1GCParPhaseTimesTracker {
  Tickspan _total_time;
  Tickspan _trim_time;

  G1EvacPhaseWithTrimTimeTracker _trim_tracker;
public:
  G1EvacPhaseTimesTracker(G1GCPhaseTimes* phase_times, G1ParScanThreadState* pss, G1GCPhaseTimes::GCParPhases phase, uint worker_id);
  virtual ~G1EvacPhaseTimesTracker();
};

#endif // SHARE_VM_GC_G1_G1GCPHASETIMES_HPP
