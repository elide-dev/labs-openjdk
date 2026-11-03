/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 */

#include "code/codeCache.hpp"
#include "code/nmethod.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shenandoah/shenandoahRuntimeCode.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "svmImageHeap.hpp"
#include "svmIsolateThread.hpp"
#include "oops/oop.inline.hpp"

namespace svm_gc {

bool ShenandoahCodeIsAliveClosure::do_object_b(oop obj) {
  if (obj == nullptr) return false;
  if (SVMImageHeap::is_image_heap_or_metaspace_object(obj)) return true;
  obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  return !heap->is_in_active_generation(obj) || heap->marking_context()->is_marked(obj);
}

class ShenandoahConditionalMarkCodeCacheClosure : public NMethodClosure {
  OopClosure* _mark_cl;
  BoolObjectClosure* _is_alive;
  bool _remark;
 public:
  ShenandoahConditionalMarkCodeCacheClosure(OopClosure* mark, BoolObjectClosure* is_alive, bool remark) :
    _mark_cl(mark), _is_alive(is_alive), _remark(remark) {}
  void do_nmethod(nmethod* nm) override;
};

void ShenandoahConditionalMarkCodeCacheClosure::do_nmethod(nmethod* nm) {
  if (nm->oops_do_try_claim()) {
    if (nm->are_all_objects_in_image_heap()) {
      return;
    }

    if (_is_alive == nullptr) {
      nm->oops_do(_mark_cl);
      return;
    }
    oop tether = nm->tether();
    if (tether != nullptr && !_is_alive->do_object_b(tether)) {
      nmethod::NMethodState state = nm->state();
      if (state == nmethod::state_removed_from_code_cache) {
        if (_remark) {
          nm->oops_do(_mark_cl);
        } else {
          nm->oops_do_object_fields(_mark_cl);
        }
        nm->set_state(nmethod::state_pending_free);
        return;
      }

      if (state == nmethod::state_non_entrant || (TreatRuntimeCodeInfoReferencesAsWeak && state == nmethod::state_code_constants_live && nm->has_weak_reference_to_unreachable_object(_is_alive))) {
        if (_remark) {
          nm->oops_do(_mark_cl);
        } else {
          nm->oops_do_object_fields(_mark_cl);
        }
        nm->set_state(nmethod::state_pending_removal_from_code_cache);
        return;
      }
    }

    nm->oops_do(_mark_cl);
  }
}

void ShenandoahRuntimeCode::mark(OopClosure* mark, BoolObjectClosure* is_alive, bool remark) {
  ShenandoahConditionalMarkCodeCacheClosure code(mark, is_alive, remark);
  CodeCache::nmethods_do(&code);
}

void ShenandoahRuntimeCode::clean() {
  if (SVMGlobalData::_clean_runtime_code_cache != nullptr) {
    // In particular, Java history logging must not enqueue through a stale
    // MARKING bit after SATB queues have been deactivated by finish_mark().
    ShenandoahHeap::heap()->propagate_gc_state_to_all_threads();
    SVMGlobalData::_clean_runtime_code_cache(CompressedOops::base(), IsolateThread::current());
  }
}

} // namespace svm_gc
