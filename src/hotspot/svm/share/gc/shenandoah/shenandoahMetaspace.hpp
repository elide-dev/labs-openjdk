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

#ifndef SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP
#define SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP

#include "oops/oop.hpp"
#include "svmMetaspace.hpp"


namespace svm_gc {

class InstanceKlass;
class Klass;

// Permanent, non-moving runtime metadata in a bounded prefix of the heap reservation.
// Allocation initializes the complete object before release-publishing the region top.
// Root scanning acquires that top and traces outgoing references. Reference referents
// remain subject to normal weak-reference processing. Class unloading is not supported.
class ShenandoahMetaspace : public SVMMetaspace {
 public:
  enum AllocationKind {
    DynamicHubAllocation = 0,
    ByteArrayAllocation = 1,
    IntArrayAllocation = 2,
    ObjectAllocation = 3,
    AllocationKindCount = 4
  };

 private:
  static uint _current_region;
  static uint _committed_regions;
  static size_t _used;
  static size_t _used_at_last_gc;
  static size_t _allocation_counts[AllocationKindCount];
  static size_t _allocation_sizes[AllocationKindCount];

  static oop allocate(Klass* klass, size_t word_size, int array_length, AllocationKind allocation_kind);
  static oop allocate_humongous(Klass* klass, size_t word_size, int array_length, AllocationKind allocation_kind);
  static oop initialize_object(HeapWord* memory, Klass* klass, size_t word_size, int array_length);
  static void record_allocation(size_t word_size, AllocationKind allocation_kind);
  static void commit_regions(uint end_region);

 public:
  void initialize();
  void update_used_at_last_gc() override;

  static oop allocate_instance(InstanceKlass* klass);
  static oop allocate_array(Klass* klass, int length, AllocationKind allocation_kind);

  bool is_in_allocated_memory(const void* address) const override;

  static size_t used() { return _used; }
  static size_t used_at_last_gc() { return _used_at_last_gc; }
  static size_t waste();
  static size_t allocation_count(AllocationKind allocation_kind) { return _allocation_counts[allocation_kind]; }
  static size_t allocation_size(AllocationKind allocation_kind) { return _allocation_sizes[allocation_kind]; }
};


} // namespace svm_gc

#endif // SVM_SHARE_GC_SHENANDOAH_SHENANDOAHMETASPACE_HPP
