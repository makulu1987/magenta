// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "type_shape.h"

#include <assert.h>

Allocation::Allocation(TypeShape typeshape, size_t bound) :
    typeshape_(typeshape), bound_(bound) { }

Allocation::Allocation() : Allocation(TypeShape()) {}

size_t Allocation::Size() const { return typeshape_.Size(); }

size_t Allocation::Alignment() const { return typeshape_.Alignment(); }

const std::vector<Allocation>& Allocation::Allocations() const { return typeshape_.Allocations(); }

size_t Allocation::Bound() const { return bound_; }

TypeShape::TypeShape(size_t size, size_t alignment, std::vector<Allocation> allocations) :
    size_(size),
    alignment_(alignment),
    allocations_(std::move(allocations)) {
    // Must be a small power of 2.
    assert(alignment_ == 1 || alignment_ == 2 || alignment_ == 4 || alignment_ == 8);
}

TypeShape::TypeShape(size_t size, size_t alignment) : TypeShape(size, alignment, std::vector<Allocation>()) {}

TypeShape::TypeShape() : size_(0u), alignment_(1u) {}

size_t TypeShape::Size() const { return size_; }

size_t TypeShape::Alignment() const { return alignment_; }

const std::vector<Allocation>& TypeShape::Allocations() const { return allocations_; }

void TypeShape::AddAllocation(Allocation allocation) {
    allocations_.push_back(allocation);
}
