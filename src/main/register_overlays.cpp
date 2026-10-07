// Registers N64Recomp's section/function table (RecompiledFuncs/recomp_overlays.inl) with librecomp,
// which uses it to resolve indirect calls (get_function / LOOKUP_FUNC).
#include "../../RecompiledFuncs/recomp_overlays.inl"

#include "librecomp/overlays.hpp"

#ifndef ARRLEN
#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace wg3d {
    void register_overlays() {
        recomp::overlays::overlay_section_table_data_t sections{
            .code_sections = section_table,
            .num_code_sections = ARRLEN(section_table),
            .total_num_sections = num_sections,
        };
        recomp::overlays::overlays_by_index_t overlays{
            .table = overlay_sections_by_index,
            .len = ARRLEN(overlay_sections_by_index),
        };
        recomp::overlays::register_overlays(sections, overlays);
    }
}
