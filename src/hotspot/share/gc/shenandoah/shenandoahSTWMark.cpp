/*
 * Copyright (c) 2021, 2022, Red Hat, Inc. All rights reserved.
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



#include "gc/shared/strongRootsScope.hpp"
#ifdef SVM
#include "gc/shenandoah/shenandoahRuntimeCode.hpp"
#endif
#include "gc/shared/taskTerminator.hpp"
#include "gc/shared/workerThread.hpp"
#include "gc/shenandoah/shenandoahClosures.inline.hpp"
#include "gc/shenandoah/shenandoahGeneration.hpp"
#include "gc/shenandoah/shenandoahGenerationType.hpp"
#include "gc/shenandoah/shenandoahMark.inline.hpp"
#include "gc/shenandoah/shenandoahReferenceProcessor.hpp"
#include "gc/shenandoah/shenandoahRootProcessor.inline.hpp"
#include "gc/shenandoah/shenandoahSTWMark.hpp"
#include "gc/shenandoah/shenandoahVerifier.hpp"

namespace svm_gc {

class ShenandoahSTWMarkTask : public WorkerTask {
private:
  ShenandoahSTWMark* const _mark;
  bool _code_cache;

public:
  ShenandoahSTWMarkTask(ShenandoahSTWMark* mark, bool code_cache = false);
  void work(uint worker_id);
};

ShenandoahSTWMarkTask::ShenandoahSTWMarkTask(ShenandoahSTWMark* mark, bool code_cache) :
  WorkerTask("Shenandoah STW mark"),
  _mark(mark), _code_cache(code_cache) {
}

void ShenandoahSTWMarkTask::work(uint worker_id) {
  ShenandoahParallelWorkerSession worker_session(worker_id);
#ifdef SVM
  if (_code_cache) {
    _mark->mark_code_roots(worker_id);
  } else
#endif
  {
    _mark->mark_roots(worker_id);
  }
  _mark->finish_mark(worker_id);
}

ShenandoahSTWMark::ShenandoahSTWMark(ShenandoahGeneration* generation, bool full_gc) :
  ShenandoahMark(generation),
  _root_scanner(full_gc ? ShenandoahPhaseTimings::full_gc_mark : ShenandoahPhaseTimings::degen_gc_stw_mark),
  _terminator(ShenandoahHeap::heap()->workers()->active_workers(), task_queues()),
  _full_gc(full_gc) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Must be at a Shenandoah safepoint");
}

void ShenandoahSTWMark::mark() {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();

#ifndef SVM
  // Arm all nmethods. Even though this is STW mark, some marking code
  // piggybacks on nmethod barriers for special instances.
  ShenandoahCodeRoots::arm_nmethods_for_mark();
#endif // !SVM

  // Weak reference processing
  assert(ShenandoahHeap::heap()->gc_generation() == _generation, "Marking unexpected generation");
  ShenandoahReferenceProcessor* rp = _generation->ref_processor();
  shenandoah_assert_generations_reconciled();
  rp->reset_thread_locals();
  rp->set_soft_reference_policy(heap->soft_ref_policy()->should_clear_all_soft_refs());

  // Init mark, do not expect forwarded pointers in roots
  if (ShenandoahVerify) {
    assert(Thread::current()->is_VM_thread(), "Must be");
    heap->verifier()->verify_roots_no_forwarded();
  }

  start_mark();

  uint nworkers = heap->workers()->active_workers();
  task_queues()->reserve(nworkers);

  TASKQUEUE_STATS_ONLY(task_queues()->reset_taskqueue_stats());

  {
    // Mark
    if (_generation->is_young()) {
      // But only scan the remembered set for young generation.
      _generation->scan_remembered_set(false /* is_concurrent */);
    }

    StrongRootsScope scope(nworkers);
    ShenandoahSTWMarkTask task(this);
    heap->workers()->run_task(&task);

    assert(task_queues()->is_empty(), "Should be empty");
  }

#ifdef SVM
  if (SVMGlobalData::_clean_runtime_code_cache != nullptr) {
    // Tethers on active stacks were traced by the first marking task. Decide the
    // lifetime of other code only after strong marking, then drain its references.
    NMethodMarkScope scope;
    _terminator.reset_for_reuse(nworkers);
    ShenandoahSTWMarkTask task(this, true);
    heap->workers()->run_task(&task);
  }
#endif

  _generation->set_mark_complete();
  end_mark();

#ifndef SVM
  // Mark is finished, can disarm the nmethods now.
  ShenandoahCodeRoots::disarm_nmethods();
#endif // !SVM

  assert(task_queues()->is_empty(), "Should be empty");
  TASKQUEUE_STATS_ONLY(task_queues()->print_and_reset_taskqueue_stats(""));
}

void ShenandoahSTWMark::mark_roots(uint worker_id) {
  assert(ShenandoahHeap::heap()->gc_generation() == _generation, "Marking unexpected generation");
  ShenandoahReferenceProcessor* rp = _generation->ref_processor();
  auto queue = task_queues()->queue(worker_id);
  switch (_generation->type()) {
    case NON_GEN: {
      ShenandoahMarkRefsClosure<NON_GEN> init_mark(queue, rp, nullptr);
      _root_scanner.roots_do(&init_mark, worker_id);
      break;
    }
    case GLOBAL: {
      ShenandoahMarkRefsClosure<GLOBAL> init_mark(queue, rp, nullptr);
      _root_scanner.roots_do(&init_mark, worker_id);
      break;
    }
    case YOUNG: {
      ShenandoahMarkRefsClosure<YOUNG> init_mark(queue, rp, nullptr);
      _root_scanner.roots_do(&init_mark, worker_id);
      break;
    }
    case OLD:
      // We never exclusively mark the old generation on a safepoint. This would be encompassed
      // by a 'global' collection. Note that both GLOBAL and NON_GEN mark the entire heap, but
      // the GLOBAL closure is specialized for the generational mode.
    default:
      ShouldNotReachHere();
  }
}

#ifdef SVM
void ShenandoahSTWMark::mark_code_roots(uint worker_id) {
  auto queue = task_queues()->queue(worker_id);
  ShenandoahReferenceProcessor* rp = _generation->ref_processor();
  ShenandoahCodeIsAliveClosure is_alive;
  switch (_generation->type()) {
    case NON_GEN: {
      ShenandoahMarkRefsClosure<NON_GEN> mark(queue, rp, nullptr);
      ShenandoahRuntimeCode::mark(&mark, &is_alive, false);
      break;
    }
    case GLOBAL: {
      ShenandoahMarkRefsClosure<GLOBAL> mark(queue, rp, nullptr);
      ShenandoahRuntimeCode::mark(&mark, &is_alive, false);
      break;
    }
    case YOUNG: {
      // Keep every code reference during young-only collections. Reclamation is
      // decided by global or old-generation marking.
      ShenandoahMarkRefsClosure<YOUNG> mark(queue, rp, get_old_queue(worker_id));
      ShenandoahRuntimeCode::mark(&mark, nullptr, false);
      break;
    }
    default:
      ShouldNotReachHere();
  }
}
#endif // SVM

void ShenandoahSTWMark::finish_mark(uint worker_id) {
  assert(ShenandoahHeap::heap()->gc_generation() == _generation, "Marking unexpected generation");
  ShenandoahPhaseTimings::Phase phase = _full_gc ? ShenandoahPhaseTimings::full_gc_mark : ShenandoahPhaseTimings::degen_gc_stw_mark;
  ShenandoahWorkerTimingsTracker timer(phase, ShenandoahPhaseTimings::ParallelMark, worker_id, true /* cumulative */);
  ShenandoahReferenceProcessor* rp = _generation->ref_processor();
  shenandoah_assert_generations_reconciled();
  StringDedup::Requests requests;

  mark_loop(worker_id, &_terminator, rp,
            _generation->type(), false /* not cancellable */,
            ShenandoahStringDedup::is_enabled() ? ALWAYS_DEDUP : NO_DEDUP, &requests);
}

} // namespace svm_gc
