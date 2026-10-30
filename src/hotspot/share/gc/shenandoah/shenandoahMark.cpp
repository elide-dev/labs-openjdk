/*
 * Copyright (c) 2021, 2026, Red Hat, Inc. All rights reserved.
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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



#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahMark.inline.hpp"
#include "gc/shenandoah/shenandoahReferenceProcessor.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"

#ifdef SVM
#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.hpp"
#include "runtime/threads.hpp"
#include "svmGlobalData.hpp"
#endif

namespace svm_gc {

#ifdef SVM
// Match Native Image's CodeInfo lifecycle, as G1ConditionalMarkCodeCacheClosure does.
// Decide weak liveness before marking code references. During SATB remark even
// invalidated code's references must be traced to preserve the snapshot.
class ShenandoahRuntimeCodeMarkClosure : public NMethodClosure {
  OopClosure* _mark;
  BoolObjectClosure* _is_alive;
  bool _remark;
  bool _unload;
public:
  ShenandoahRuntimeCodeMarkClosure(OopClosure* mark, BoolObjectClosure* is_alive, bool remark, bool unload) :
    _mark(mark), _is_alive(is_alive), _remark(remark), _unload(unload) {}

  void do_nmethod(nmethod* nm) override {
    if (nm->are_all_objects_in_image_heap()) {
      return;
    }
    oop tether = nm->tether();
    if (_unload && tether != nullptr && !_is_alive->do_object_b(tether)) {
      nmethod::NMethodState state = nm->state();
      if (state == nmethod::state_removed_from_code_cache) {
        if (_remark) {
          nm->oops_do(_mark);
        } else {
          nm->oops_do_object_fields(_mark);
        }
        nm->set_state(nmethod::state_pending_free);
        return;
      }
      if (state == nmethod::state_non_entrant ||
          (TreatRuntimeCodeInfoReferencesAsWeak && state == nmethod::state_code_constants_live && nm->has_weak_reference_to_unreachable_object(_is_alive))) {
        if (_remark) {
          nm->oops_do(_mark);
        } else {
          nm->oops_do_object_fields(_mark);
        }
        nm->set_state(nmethod::state_pending_removal_from_code_cache);
        return;
      }
    }
    nm->oops_do(_mark);
  }
};

class ShenandoahRuntimeCodeTetherClosure : public NMethodClosure {
  OopClosure* _mark;
public:
  explicit ShenandoahRuntimeCodeTetherClosure(OopClosure* mark) : _mark(mark) {}
  void do_nmethod(nmethod* nm) override { _mark->do_oop(nm->tether_addr()); }
};

class ShenandoahRuntimeCodeThreadClosure : public ThreadClosure {
  NMethodClosure* _code;
public:
  explicit ShenandoahRuntimeCodeThreadClosure(NMethodClosure* code) : _code(code) {}
  void do_thread(Thread* thread) override {
    if (thread->is_Java_thread()) {
      JavaThread::cast(thread)->nmethods_do(_code);
    }
  }
};

class ShenandoahRuntimeCodeMarkTask : public WorkerTask {
  ShenandoahMark* _mark;
  bool _remark;
public:
  ShenandoahRuntimeCodeMarkTask(ShenandoahMark* mark, bool remark) :
    WorkerTask("Shenandoah Native Image Code Roots"), _mark(mark), _remark(remark) {}
  void work(uint worker_id) override {
    if (worker_id == 0) {
      ShenandoahParallelWorkerSession session(worker_id);
      _mark->mark_runtime_code_work(_remark);
    }
  }
};

void ShenandoahMark::mark_runtime_code(bool remark) {
  if (SVMGlobalData::_fetch_code_infos != nullptr) {
    CodeInfosPerThread* infos = remark ? Threads::set_java_code_infos() : nullptr;
    ShenandoahRuntimeCodeMarkTask task(this, remark);
    // Preserve the GC worker count and the queue-set reservation used by mark_loop.
    // Only worker zero scans code metadata and drains the additional marking work.
    ShenandoahHeap::heap()->workers()->run_task(&task);
    if (remark) {
      Threads::free_java_code_infos(infos);
    }
  }
}

void ShenandoahMark::mark_runtime_code_work(bool remark) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahReferenceProcessor* rp = _generation->ref_processor();
  TaskTerminator terminator(1, task_queues());
  StringDedup::Requests requests;
  // Global and non-generational marking use the same whole-heap closure.
  // For a young cycle, code references are additional young roots; unloading
  // waits for a whole-heap cycle so that old-generation liveness is available.
  ShenandoahMarkRefsClosure<NON_GEN> all_cl(get_queue(0), rp, get_old_queue(0));
  ShenandoahMarkRefsClosure<YOUNG> young_cl(get_queue(0), rp, get_old_queue(0));
  ShenandoahMarkRefsClosure<OLD> old_cl(get_queue(0), rp, get_old_queue(0));
  OopClosure* cl = _generation->is_young() ? static_cast<OopClosure*>(&young_cl) :
                   _generation->is_old() ? static_cast<OopClosure*>(&old_cl) : static_cast<OopClosure*>(&all_cl);

  if (remark) {
    ShenandoahRuntimeCodeTetherClosure tethers(cl);
    ShenandoahRuntimeCodeThreadClosure threads(&tethers);
    Threads::threads_do(&threads);
    mark_loop(0, &terminator, rp, _generation->type(), false, NO_DEDUP, &requests);
    terminator.reset_for_reuse();
  }

  ShenandoahIsAliveClosure is_alive;
  ShenandoahRuntimeCodeMarkClosure code(cl, &is_alive, remark, !_generation->is_young() && !_generation->is_old());
  CodeCache::nmethods_do(&code);
  mark_loop(0, &terminator, rp, _generation->type(), false, NO_DEDUP, &requests);
}

void ShenandoahMark::clean_runtime_code() {
  if (SVMGlobalData::_clean_runtime_code_cache != nullptr) {
    // Java cleanup can write heap references. Publish the stopped marking state
    // before those stores can reach an already-deactivated SATB queue.
    ShenandoahHeap::heap()->propagate_gc_state_to_all_threads();
    SVMGlobalData::_clean_runtime_code_cache(CompressedOops::base(), IsolateThread::current());
  }
}
#endif

void ShenandoahMark::start_mark() {
  if (!CodeCache::is_gc_marking_cycle_active()) {
    CodeCache::on_gc_marking_cycle_start();
  }
}

void ShenandoahMark::end_mark() {
  // Unlike other GCs, we do not arm the nmethods
  // when marking terminates.
  if (!ShenandoahHeap::heap()->is_concurrent_old_mark_in_progress()) {
    CodeCache::on_gc_marking_cycle_finish();
  }
}

ShenandoahMark::ShenandoahMark(ShenandoahGeneration* generation) :
  _generation(generation),
  _task_queues(generation->task_queues()),
  _old_gen_task_queues(generation->old_gen_task_queues()) {
}

template <ShenandoahGenerationType GENERATION, bool CANCELLABLE, StringDedupMode STRING_DEDUP>
void ShenandoahMark::mark_loop_prework(uint w, TaskTerminator *t, ShenandoahReferenceProcessor *rp, StringDedup::Requests* const req, bool update_refs) {
  ShenandoahObjToScanQueue* q = get_queue(w);
  ShenandoahObjToScanQueue* old_q = get_old_queue(w);

  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  ShenandoahLiveData* ld = heap->get_liveness_cache(w);

  // TODO: We can clean up this if we figure out how to do templated oop closures that
  // play nice with specialized_oop_iterators.
  if (update_refs) {
    using Closure = ShenandoahMarkUpdateRefsClosure<GENERATION>;
    Closure cl(q, rp, old_q);
    mark_loop_work<Closure, GENERATION, CANCELLABLE, STRING_DEDUP>(&cl, ld, w, t, req);
  } else {
    using Closure = ShenandoahMarkRefsClosure<GENERATION>;
    Closure cl(q, rp, old_q);
    mark_loop_work<Closure, GENERATION, CANCELLABLE, STRING_DEDUP>(&cl, ld, w, t, req);
  }

  heap->flush_liveness_cache(w);
}

template<bool CANCELLABLE, StringDedupMode STRING_DEDUP>
void ShenandoahMark::mark_loop(uint worker_id, TaskTerminator* terminator, ShenandoahReferenceProcessor *rp,
                               ShenandoahGenerationType generation, StringDedup::Requests* const req) {
  bool update_refs = ShenandoahHeap::heap()->has_forwarded_objects();
  switch (generation) {
    case YOUNG:
      mark_loop_prework<YOUNG, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, update_refs);
      break;
    case OLD:
      // Old generation collection only performs marking, it should not update references.
      mark_loop_prework<OLD, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, false);
      break;
    case GLOBAL:
      mark_loop_prework<GLOBAL, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, update_refs);
      break;
    case NON_GEN:
      mark_loop_prework<NON_GEN, CANCELLABLE, STRING_DEDUP>(worker_id, terminator, rp, req, update_refs);
      break;
    default:
      ShouldNotReachHere();
      break;
  }
}

void ShenandoahMark::mark_loop(uint worker_id, TaskTerminator* terminator, ShenandoahReferenceProcessor *rp,
                               ShenandoahGenerationType generation, bool cancellable, StringDedupMode dedup_mode, StringDedup::Requests* const req) {
  if (cancellable) {
    switch(dedup_mode) {
      case NO_DEDUP:
        mark_loop<true, NO_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ENQUEUE_DEDUP:
        mark_loop<true, ENQUEUE_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ALWAYS_DEDUP:
        mark_loop<true, ALWAYS_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
    }
  } else {
    switch(dedup_mode) {
      case NO_DEDUP:
        mark_loop<false, NO_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ENQUEUE_DEDUP:
        mark_loop<false, ENQUEUE_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
      case ALWAYS_DEDUP:
        mark_loop<false, ALWAYS_DEDUP>(worker_id, terminator, rp, generation, req);
        break;
    }
  }
}

template <class T, ShenandoahGenerationType GENERATION, bool CANCELLABLE, StringDedupMode STRING_DEDUP>
void ShenandoahMark::mark_loop_work(T* cl, ShenandoahLiveData* live_data, uint worker_id, TaskTerminator *terminator, StringDedup::Requests* const req) {
  uintx stride = ShenandoahMarkLoopStride;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahObjToScanQueueSet* queues = task_queues();
  ShenandoahObjToScanQueue* q;
  ShenandoahMarkTask t;

  // Do not use active_generation() : we must use the gc_generation() set by
  // ShenandoahGCScope on the ControllerThread's stack; no safepoint may
  // intervene to update active_generation, so we can't
  // shenandoah_assert_generations_reconciled() here.
  assert(heap->gc_generation()->type() == GENERATION, "Sanity: %d != %d", heap->gc_generation()->type(), GENERATION);
  heap->gc_generation()->ref_processor()->set_mark_closure(worker_id, cl);

  /*
   * Process outstanding queues, if any.
   *
   * There can be more queues than workers. To deal with the imbalance, we claim
   * extra queues first. Since marking can push new tasks into the queue associated
   * with this worker id, we come back to process this queue in the normal loop.
   */
  assert(queues->get_reserved() == heap->workers()->active_workers(),
         "Need to reserve proper number of queues: reserved: %u, active: %u", queues->get_reserved(), heap->workers()->active_workers());

  q = queues->claim_next();
  while (q != nullptr) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }

    for (uint i = 0; i < stride; i++) {
      if (q->pop(t)) {
        do_task<T, GENERATION, STRING_DEDUP>(q, cl, live_data, req, &t, worker_id);
      } else {
        assert(q->is_empty(), "Must be empty");
        q = queues->claim_next();
        break;
      }
    }
  }
  q = get_queue(worker_id);
  ShenandoahObjToScanQueue* old_q = get_old_queue(worker_id);

  ShenandoahSATBBufferClosure<GENERATION> drain_satb(q, old_q);
  SATBMarkQueueSet& satb_mq_set = ShenandoahBarrierSet::satb_mark_queue_set();

  /*
   * Normal marking loop:
   */
  while (true) {
    if (CANCELLABLE && heap->check_cancelled_gc_and_yield()) {
      return;
    }
    while (satb_mq_set.completed_buffers_num() > 0) {
      satb_mq_set.apply_closure_to_completed_buffer(&drain_satb);
    }

    uint work = 0;
    for (uint i = 0; i < stride; i++) {
      if (q->pop(t) ||
          queues->steal(worker_id, t)) {
        do_task<T, GENERATION, STRING_DEDUP>(q, cl, live_data, req, &t, worker_id);
        work++;
      } else {
        break;
      }
    }

    if (work == 0) {
      // No work encountered in current stride, try to terminate.
      // Need to leave the STS here otherwise it might block safepoints.
      ShenandoahSuspendibleThreadSetLeaver stsl(CANCELLABLE);
      ShenandoahTerminatorTerminator tt(heap);
      if (terminator->offer_termination(&tt)) return;
    }
  }
}

} // namespace svm_gc
