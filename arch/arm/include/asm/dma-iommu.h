#ifndef ASMARM_DMA_IOMMU_H
#define ASMARM_DMA_IOMMU_H

#ifdef __KERNEL__

#include <linux/mm_types.h>
#include <linux/scatterlist.h>
#include <linux/dma-debug.h>
#include <linux/kmemcheck.h>

#include <asm/memory.h>

struct dma_iommu_mapping {
	/* iommu specific data */
	struct iommu_domain	*domain;

	void			*bitmap;
	size_t			bits;
	unsigned int		order;
	dma_addr_t		base;

	spinlock_t		lock;
	struct kref		kref;
};

struct dma_iommu_mapping *
arm_iommu_create_mapping(struct bus_type *bus, dma_addr_t base, size_t size,
			 int order);

void arm_iommu_release_mapping(struct dma_iommu_mapping *mapping);

int arm_iommu_attach_device(struct device *dev,
					struct dma_iommu_mapping *mapping);

dma_addr_t arm_iommu_alloc_iova(struct device *dev, dma_addr_t iova,
				size_t size);

void arm_iommu_free_iova(struct device *dev, dma_addr_t addr, size_t size);

size_t arm_iommu_iova_avail(struct device *dev);

size_t arm_iommu_iova_max_free(struct device *dev);

dma_addr_t arm_iommu_map_page_at(struct device *dev, struct page *page,
			 dma_addr_t addr, unsigned long offset, size_t size,
			 enum dma_data_direction dir, struct dma_attrs *attrs);

static inline dma_addr_t dma_map_page_at(struct device *d, struct page *p,
					 dma_addr_t a, size_t o, size_t s,
					 enum dma_data_direction r)
{
	return arm_iommu_map_page_at(d, p, a, o, s, r, 0);
}

void arm_iommu_unmap_page_at(struct device *dev, dma_addr_t handle,
			     size_t size, enum dma_data_direction dir,
			     struct dma_attrs *attrs);

static inline void dma_unmap_page_at(struct device *d, dma_addr_t a, size_t s,
				     enum dma_data_direction r)
{
	return arm_iommu_unmap_page_at(d, a, s, r, 0);
}


#endif /* __KERNEL__ */
#endif
