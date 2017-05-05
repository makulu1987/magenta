// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stddef.h>

#include <vector>

// A TypeShape represents a type in a message. For example, given
//
//     struct tag {
//         int32 t;
//     }
//     struct vectors_of_vectors {
//         vector<vector<tag?>:3> tags;
//         array<handle<channel>>:8 channels;
//         vector<vector<uint32>>:5 ints;
//     }
//
// the typeshape corresponding to vectors_of_vectors is
//
//     TypeShape {
//         size_ = 64 // 16 + (8 * 4) + 16
//         alignment_ = 8 // The pointers and sizes of the vectors are 8 byte aligned.
//         allocations_ = {
//             Allocation { // The allocation for an unbounded vector of bounded vectors...
//                 typeshape_ = {
//                     size_ = 16
//                     alignment_ = 8
//                     allocations_ = {
//                         Allocation { // ...each of which is a pointer to a struct tag.
//                             typeshape_ = {
//                                 size_ = 8
//                                 alignment_ = 8
//                                 allocations_ = {
//                                     Allocation {
//                                         typeshape_ = {
//                                             size_ = 4
//                                             alignment_ = 4
//                                             allocations_ = {
//                                             }
//                                         }
//                                         bound_ = 1
//                                     }
//                                 }
//                             bound_ = 3
//                             }
//                         }
//                     }
//                 bound_ = SIZE_MAX
//             }
//             Allocation { // The allocation for a bounded vector of unbounded vectors...
//                 typeshape_ = {
//                     size_ = 16
//                     alignment_ = 8
//                     allocations_ = {
//                         Allocation { // ...each of which is a uint32.
//                             typeshape_ = {
//                                 size_ = 4
//                                 alignment_ = 4
//                                 allocations_ = {
//                                 }
//                             bound_ = SIZE_MAX
//                             }
//                         }
//                     }
//                 bound_ = 5
//             }
//         }
//     }

class Allocation;

class TypeShape {
public:
    TypeShape(size_t size, size_t alignment, std::vector<Allocation> allocations);
    TypeShape(size_t size, size_t alignment);
    TypeShape();

    size_t Size() const;
    size_t Alignment() const;
    const std::vector<Allocation>& Allocations() const;

    void AddAllocation(Allocation allocation);

private:
    size_t size_;
    size_t alignment_;
    std::vector<Allocation> allocations_;
};

// Represents an out-of-line allocation.
class Allocation {
public:
    explicit Allocation(TypeShape typeshape, size_t bound = std::numeric_limits<size_t>::max());
    Allocation();

    size_t Size() const;
    size_t Alignment() const;
    const std::vector<Allocation>& Allocations() const;
    size_t Bound() const;

private:
    TypeShape typeshape_;
    size_t bound_;
};
