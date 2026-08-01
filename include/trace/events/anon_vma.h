#undef TRACE_SYSTEM
#define TRACE_SYSTEM anon_vma

#if !defined(_TRACE_ANON_VMA_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ANON_VMA_H

#include <linux/tracepoint.h>
#include <linux/mm_types.h>
#include <linux/rmap.h>


TRACE_EVENT(anon_vma_fork,
	TP_PROTO(struct vm_area_struct *vma, struct anon_vma *anon_vma,
		 unsigned long base_vm_offset, unsigned long end_vm_offset),

	TP_ARGS(vma, anon_vma, base_vm_offset, end_vm_offset),

	TP_STRUCT__entry(
		__field(struct vm_area_struct *, vma)
		__field(struct anon_vma *, anon_vma)
		__field(unsigned long, base_vm_offset)
		__field(unsigned long, end_vm_offset)
	),

	TP_fast_assign(
		__entry->vma = vma;
		__entry->anon_vma = anon_vma;
		__entry->base_vm_offset = base_vm_offset;
		__entry->end_vm_offset = end_vm_offset;
	),

	TP_printk("vma=%p anon_vma=%p base_vm_offset=%lx end_vm_offset=%lx",
		  __entry->vma, __entry->anon_vma,
		  __entry->base_vm_offset, __entry->end_vm_offset)
);

TRACE_EVENT(anon_vma_mirror_parent,
	TP_PROTO(struct vm_area_struct *vma, struct anon_vma *anon_vma,
		 struct anon_vma *orig, unsigned long base_vm_offset,
		 unsigned long end_vm_offset),

	TP_ARGS(vma, anon_vma, orig, base_vm_offset, end_vm_offset),

	TP_STRUCT__entry(
		__field(struct vm_area_struct *, vma)
		__field(struct anon_vma *, anon_vma)
		__field(struct anon_vma *, orig)
		__field(unsigned long, base_vm_offset)
		__field(unsigned long, end_vm_offset)
	),

	TP_fast_assign(
		__entry->vma = vma;
		__entry->anon_vma = anon_vma;
		__entry->orig = orig;
		__entry->base_vm_offset = base_vm_offset;
		__entry->end_vm_offset = end_vm_offset;
	),

	TP_printk("vma=%p anon_vma=%p orig=%p base_vm_offset=%lx end_vm_offset=%lx",
		  __entry->vma, __entry->anon_vma, __entry->orig,
		  __entry->base_vm_offset, __entry->end_vm_offset)
);
#endif /* _TRACE_ANON_VMA_H */

/* This part must be outside protection */
#include <trace/define_trace.h>

