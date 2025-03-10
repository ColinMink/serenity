/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Types.h>
#include <Kernel/Arch/Processor.h>
#include <Kernel/Arch/x86/CPUID.h>
#include <Kernel/Arch/x86/ProcessorInfo.h>

namespace Kernel {

ProcessorInfo::ProcessorInfo(Processor const& processor)
    : m_cpuid(query_processor_cpuid())
    , m_brand(query_processor_brand())
    , m_features(processor.features_string())
{
    CPUID cpuid(1);
    m_stepping = cpuid.eax() & 0xf;
    u32 model = (cpuid.eax() >> 4) & 0xf;
    u32 family = (cpuid.eax() >> 8) & 0xf;
    m_type = (cpuid.eax() >> 12) & 0x3;
    u32 extended_model = (cpuid.eax() >> 16) & 0xf;
    u32 extended_family = (cpuid.eax() >> 20) & 0xff;
    if (family == 15) {
        m_display_family = family + extended_family;
        m_display_model = model + (extended_model << 4);
    } else if (family == 6) {
        m_display_family = family;
        m_display_model = model + (extended_model << 4);
    } else {
        m_display_family = family;
        m_display_model = model;
    }
}

NonnullOwnPtr<KString> ProcessorInfo::query_processor_cpuid()
{
    CPUID cpuid(0);
    StringBuilder builder;
    auto emit_u32 = [&](u32 value) {
        builder.appendff("{:c}{:c}{:c}{:c}",
            value & 0xff,
            (value >> 8) & 0xff,
            (value >> 16) & 0xff,
            (value >> 24) & 0xff);
    };
    emit_u32(cpuid.ebx());
    emit_u32(cpuid.edx());
    emit_u32(cpuid.ecx());
    return KString::must_create(builder.string_view());
}

NonnullOwnPtr<KString> ProcessorInfo::query_processor_brand()
{
    u32 max_extended_leaf = CPUID(0x80000000).eax();

    if (max_extended_leaf < 0x80000004)
        return KString::must_create({});

    alignas(u32) char buffer[48];
    u32* bufptr = reinterpret_cast<u32*>(buffer);
    auto copy_brand_string_part_to_buffer = [&](u32 i) {
        CPUID cpuid(0x80000002 + i);
        *bufptr++ = cpuid.eax();
        *bufptr++ = cpuid.ebx();
        *bufptr++ = cpuid.ecx();
        *bufptr++ = cpuid.edx();
    };
    copy_brand_string_part_to_buffer(0);
    copy_brand_string_part_to_buffer(1);
    copy_brand_string_part_to_buffer(2);
    return KString::must_create(buffer);
}

}
