// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdio.h>

#include <acpica/acpi.h>
#include <magenta/process.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/port.h>
#include <magenta/syscalls/iommu.h>

#include "ec.h"
#include "pci.h"
#include "powerbtn.h"
#include "processor.h"
#include "resource_tree.h"

#define ACPI_MAX_INIT_TABLES 32

static ACPI_STATUS set_apic_irq_mode(void);
static ACPI_STATUS init(void);
static mx_status_t find_iommus(void);

mx_handle_t root_resource_handle;

int main(int argc, char** argv) {
    root_resource_handle = mx_get_startup_handle(PA_HND(PA_USER0, 0));
    if (root_resource_handle <= 0) {
        printf("Failed to find root resource handle\n");
        return 1;
    }

    // Get handle from devmgr to serve as the ACPI root handle
    mx_handle_t acpi_root = mx_get_startup_handle(PA_HND(PA_USER1, 0));
    if (acpi_root <= 0) {
        printf("Failed to find acpi root handle\n");
        return 1;
    }

    ACPI_STATUS status = init();
    if (status != MX_OK) {
        printf("Failed to initialize ACPI\n");
        return 3;
    }
    printf("Initialized ACPI\n");

    mx_handle_t port;
    mx_status_t mx_status = mx_port_create(0, &port);
    if (mx_status != MX_OK) {
        printf("Failed to construct resource port\n");
        return 4;
    }

    ec_init();

    mx_status = install_powerbtn_handlers();
    if (mx_status != MX_OK) {
        printf("Failed to install powerbtn handler\n");
    }

    mx_status = find_iommus();
    if (mx_status != MX_OK) {
        printf("Failed to publish iommus\n");
    }

    mx_status = pci_report_current_resources(root_resource_handle);
    if (mx_status != MX_OK) {
        printf("WARNING: ACPI failed to report all current resources!\n");
    }

    return begin_processing(acpi_root);
}

static ACPI_STATUS init(void) {
    // This sequence is described in section 10.1.2.1 (Full ACPICA Initialization)
    // of the ACPICA developer's reference.
    ACPI_STATUS status = AcpiInitializeSubsystem();
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI\n");
        return status;
    }

    status = AcpiInitializeTables(NULL, ACPI_MAX_INIT_TABLES, FALSE);
    if (status == AE_NOT_FOUND) {
        printf("WARNING: could not find ACPI tables\n");
        return status;
    } else if (status == AE_NO_MEMORY) {
        printf("WARNING: could not initialize ACPI tables\n");
        return status;
    } else if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI tables for unknown reason\n");
        return status;
    }

    status = AcpiLoadTables();
    if (status != AE_OK) {
        printf("WARNING: could not load ACPI tables: %d\n", status);
        return status;
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not enable ACPI\n");
        return status;
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI objects\n");
        return status;
    }

    status = set_apic_irq_mode();
    if (status == AE_NOT_FOUND) {
        printf("WARNING: Could not find ACPI IRQ mode switch\n");
    } else if (status != AE_OK) {
        printf("Failed to set APIC IRQ mode\n");
        return status;
    }

    // TODO(teisenbe): Maybe back out of ACPI mode on failure, but we rely on
    // ACPI for some critical things right now, so failure will likely prevent
    // successful boot anyway.
    return AE_OK;
}

/* @brief Switch interrupts to APIC model (controls IRQ routing) */
static ACPI_STATUS set_apic_irq_mode(void) {
    ACPI_OBJECT selector = {
        .Integer.Type = ACPI_TYPE_INTEGER,
        .Integer.Value = 1, // 1 means APIC mode according to ACPI v5 5.8.1
    };
    ACPI_OBJECT_LIST params = {
        .Count = 1,
        .Pointer = &selector,
    };
    return AcpiEvaluateObject(NULL, (char*)"\\_PIC", &params, NULL);
}

// Walks the given unit's scopes and appends them to the given descriptor.
// |max_scopes| is the maximum number of scopes |desc| can hold, including ones
// already in it.  |num_scopes_found| is the number of scopes found on |unit|, even if
// they wouldn't all fit in |desc|.
static mx_status_t append_scopes(ACPI_DMAR_HARDWARE_UNIT* unit, mx_iommu_desc_intel_t* desc,
                                 size_t max_scopes, size_t* num_scopes_found) {
    size_t num_scopes = 0;
    uintptr_t scope;
    const uintptr_t addr = (uintptr_t)unit;
    for (scope = addr + 16; scope < addr + unit->Header.Length; ) {
        ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
        printf("  DMAR Scope: %u, bus %u\n", s->EntryType, s->Bus);
        for (ssize_t i = 0; i < (s->Length - 6) / 2; ++i) {
            uint16_t v = *(uint16_t*)(scope + 6 + 2 * i);
            printf("    Path %ld: %02x.%02x\n", i, v & 0xffu, (uint16_t)(v >> 8));
        }
        scope += s->Length;

        // Count the scopes we care about
        switch (s->EntryType) {
            case ACPI_DMAR_SCOPE_TYPE_ENDPOINT:
            case ACPI_DMAR_SCOPE_TYPE_BRIDGE:
                num_scopes++;
                break;
        }
    }

    assert(!desc || unit->Segment == desc->pci_segment);

    if (num_scopes_found) {
        *num_scopes_found = num_scopes;
    }
    if (!desc) {
        return MX_OK;
    }

    if (desc->num_scopes + num_scopes > max_scopes) {
        return MX_ERR_BUFFER_TOO_SMALL;
    }

    size_t scope_index = desc->num_scopes;
    for (scope = addr + 16; scope < addr + unit->Header.Length && scope_index < max_scopes;
         ++scope_index) {

        ACPI_DMAR_DEVICE_SCOPE* s = (ACPI_DMAR_DEVICE_SCOPE*)scope;
        mx_iommu_desc_intel_scope_t* scope_desc = &desc->scopes[scope_index];

        switch (s->EntryType) {
            case ACPI_DMAR_SCOPE_TYPE_ENDPOINT:
                scope_desc->type = MX_IOMMU_INTEL_SCOPE_ENDPOINT;
                break;
            case ACPI_DMAR_SCOPE_TYPE_BRIDGE:
                scope_desc->type = MX_IOMMU_INTEL_SCOPE_BRIDGE;
                break;
            default:
                // Skip this scope, since it's not a type we care about.
                --scope_index;
                continue;
        }
        scope_desc->start_bus = s->Bus;
        // TOOD: Check this fits
        scope_desc->num_hops = (s->Length - 6) / 2;
        for (ssize_t i = 0; i < (s->Length - 6) / 2; ++i) {
            uint16_t v = *(uint16_t*)(scope + 6 + 2 * i);
            const uint8_t dev = v >> 8;
            const uint8_t func = v & 0x7;
            scope_desc->dev_func[i] = (dev << 3) | func;
        }
        scope += s->Length;
    }
    desc->num_scopes = scope_index;

    return MX_OK;
}

static mx_status_t create_whole_segment_iommu_desc(ACPI_TABLE_DMAR* table,
                                                   ACPI_DMAR_HARDWARE_UNIT* unit,
                                                   mx_iommu_desc_intel_t** desc_out,
                                                   size_t* desc_len_out) {
    assert(unit->Flags & ACPI_DMAR_INCLUDE_ALL);

    // The VT-d spec requires that whole-segment hardware units appear in the
    // DMAR table after all other hardware units on their segment.  Search those
    // entries for scopes to specify as excluded from this descriptor.

    size_t num_scopes = 0;
    size_t num_scopes_on_unit;

    const uintptr_t records_start = ((uintptr_t)table) + sizeof(*table);
    const uintptr_t records_end = (uintptr_t)unit;

    uintptr_t addr;
    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;
                if (rec->Segment != unit->Segment) {
                    break;
                }
                mx_status_t status = append_scopes(rec, NULL, 0, &num_scopes_on_unit);
                if (status != MX_OK) {
                    return status;
                }
                num_scopes += num_scopes_on_unit;
            }
        }
        addr += record_hdr->Length;
    }

    const size_t desc_len = sizeof(mx_iommu_desc_intel_t) +
            sizeof(mx_iommu_desc_intel_scope_t) * num_scopes;
    mx_iommu_desc_intel_t* desc = malloc(desc_len);
    if (!desc) {
        return MX_ERR_NO_MEMORY;
    }
    desc->register_base = unit->Address;
    desc->pci_segment = unit->Segment;
    desc->whole_segment = true;
    desc->num_scopes = 0;

    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;
                if (rec->Segment != unit->Segment) {
                    break;
                }
                mx_status_t status = append_scopes(unit, desc, num_scopes, NULL);
                if (status != MX_OK) {
                    free(desc);
                    return status;
                }
            }
        }
        addr += record_hdr->Length;
    }

    *desc_out = desc;
    *desc_len_out = desc_len;
    return MX_OK;
}

static mx_status_t create_non_whole_segment_iommu_desc(ACPI_DMAR_HARDWARE_UNIT* unit,
                                                       mx_iommu_desc_intel_t** desc_out,
                                                       size_t* desc_len_out) {
    assert((unit->Flags & ACPI_DMAR_INCLUDE_ALL) == 0);

    size_t num_scopes;
    mx_status_t mx_status = append_scopes(unit, NULL, 0, &num_scopes);
    if (mx_status != MX_OK) {
        return mx_status;
    }

    const size_t desc_len = sizeof(mx_iommu_desc_intel_t) +
            sizeof(mx_iommu_desc_intel_scope_t) * num_scopes;
    mx_iommu_desc_intel_t* desc = malloc(desc_len);
    if (!desc) {
        return MX_ERR_NO_MEMORY;
    }
    desc->register_base = unit->Address;
    desc->pci_segment = unit->Segment;
    desc->whole_segment = false;
    desc->num_scopes = 0;
    mx_status = append_scopes(unit, desc, num_scopes, NULL);
    if (mx_status != MX_OK) {
        free(desc);
        return mx_status;
    }

    *desc_out = desc;
    *desc_len_out = desc_len;
    return MX_OK;
}

mx_status_t find_iommus(void) {
    ACPI_TABLE_HEADER* table = NULL;
    ACPI_STATUS status = AcpiGetTable((char*)ACPI_SIG_DMAR, 1, &table);
    if (status != AE_OK) {
        printf("could not find DMAR\n");
        return MX_ERR_NOT_FOUND;
    }
    ACPI_TABLE_DMAR* dmar = (ACPI_TABLE_DMAR*)table;
    const uintptr_t records_start = ((uintptr_t)dmar) + sizeof(*dmar);
    const uintptr_t records_end = ((uintptr_t)dmar) + dmar->Header.Length;
    if (records_start >= records_end) {
        printf("DMAR wraps around address space\n");
        return MX_ERR_IO_DATA_INTEGRITY;
    }
    // Shouldn't be too many records
    if (dmar->Header.Length > 4096) {
        printf("DMAR suspiciously long: %u\n", dmar->Header.Length);
        return MX_ERR_IO_DATA_INTEGRITY;
    }

    uintptr_t addr;
    for (addr = records_start; addr < records_end;) {
        ACPI_DMAR_HEADER* record_hdr = (ACPI_DMAR_HEADER*)addr;
        printf("DMAR record: %d\n", record_hdr->Type);
        switch (record_hdr->Type) {
            case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
                ACPI_DMAR_HARDWARE_UNIT* rec = (ACPI_DMAR_HARDWARE_UNIT*)record_hdr;

                printf("DMAR Hardware Unit: %u %#llx %#x\n", rec->Segment, rec->Address, rec->Flags);
                const bool whole_segment = rec->Flags & ACPI_DMAR_INCLUDE_ALL;

                mx_iommu_desc_intel_t* desc;
                size_t desc_len;
                mx_status_t mx_status;
                if (whole_segment) {
                    mx_status = create_whole_segment_iommu_desc(dmar, rec, &desc, &desc_len);
                } else {
                    mx_status = create_non_whole_segment_iommu_desc(rec, &desc, &desc_len);
                }
                if (mx_status != MX_OK) {
                    printf("Failed to create iommu desc: %d\n", mx_status);
                    return mx_status;
                }

                mx_handle_t iommu_handle;
                mx_status = mx_iommu_create(root_resource_handle, MX_IOMMU_TYPE_INTEL,
                                            desc, desc_len, &iommu_handle);
                free(desc);
                if (mx_status != MX_OK) {
                    printf("Failed to create iommu: %d\n", mx_status);
                    return mx_status;
                }
                // TODO(teisenbe): Do something with these handles
                //mx_handle_close(iommu_handle);
                break;
            }
        }

        addr += record_hdr->Length;
    }
    if (addr != records_end) {
        return MX_ERR_IO_DATA_INTEGRITY;
    }

    return MX_OK;
}
