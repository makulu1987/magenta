// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <dev/iommu.h>
#include <magenta/syscalls/iommu.h>
#include <mxtl/ref_ptr.h>

class IntelIommu {
public:
    static status_t Create(const mx_iommu_desc_intel_t* desc, uint32_t desc_len,
                           mxtl::RefPtr<Iommu>* out);
};
