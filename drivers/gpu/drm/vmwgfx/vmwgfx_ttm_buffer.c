// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "vmwgfx_bo.h"
#include "vmwgfx_drv.h"
#include <drm/ttm/ttm_placement.h>

static const struct ttm_place vram_placement_flags = {
	.fpfn = 0,
	.lpfn = 0,
	.mem_type = TTM_PL_VRAM,
	.flags = 0
};

static const struct ttm_place sys_placement_flags = {
	.fpfn = 0,
	.lpfn = 0,
	.mem_type = TTM_PL_SYSTEM,
	.flags = 0
};

static const struct ttm_place gmr_placement_flags = {
	.fpfn = 0,
	.lpfn = 0,
	.mem_type = VMW_PL_GMR,
	.flags = 0
};

struct ttm_placement vmw_vram_placement = {
	.num_placement = 1,
	.placement = &vram_placement_flags,
	.num_busy_placement = 1,
	.busy_placement = &vram_placement_flags
};

static const struct ttm_place vram_gmr_placement_flags[] = {
	{
		.fpfn = 0,
		.lpfn = 0,
		.mem_type = TTM_PL_VRAM,
		.flags = 0
	}, {
		.fpfn = 0,
		.lpfn = 0,
		.mem_type = VMW_PL_GMR,
		.flags = 0
	}
};

struct ttm_placement vmw_vram_gmr_placement = {
	.num_placement = 2,
	.placement = vram_gmr_placement_flags,
	.num_busy_placement = 1,
	.busy_placement = &gmr_placement_flags
};

struct ttm_placement vmw_sys_placement = {
	.num_placement = 1,
	.placement = &sys_placement_flags,
	.num_busy_placement = 1,
	.busy_placement = &sys_placement_flags
};

const size_t vmw_tt_size = sizeof(struct vmw_ttm_tt);

/**
 * __vmw_piter_non_sg_next: Helper functions to advance
 * a struct vmw_piter iterator.
 *
 * @viter: Pointer to the iterator.
 *
 * These functions return false if past the end of the list,
 * true otherwise. Functions are selected depending on the current
 * DMA mapping mode.
 */
static bool __vmw_piter_non_sg_next(struct vmw_piter *viter)
{
	return ++(viter->i) < viter->num_pages;
}

static bool __vmw_piter_sg_next(struct vmw_piter *viter)
{
	bool ret = __vmw_piter_non_sg_next(viter);

	return __sg_page_iter_dma_next(&viter->iter) && ret;
}


static dma_addr_t __vmw_piter_dma_addr(struct vmw_piter *viter)
{
	return viter->addrs[viter->i];
}

static dma_addr_t __vmw_piter_sg_addr(struct vmw_piter *viter)
{
	return sg_page_iter_dma_address(&viter->iter);
}


/**
 * vmw_piter_start - Initialize a struct vmw_piter.
 *
 * @viter: Pointer to the iterator to initialize
 * @vsgt: Pointer to a struct vmw_sg_table to initialize from
 * @p_offset: Pointer offset used to update current array position
 *
 * Note that we're following the convention of __sg_page_iter_start, so that
 * the iterator doesn't point to a valid page after initialization; it has
 * to be advanced one step first.
 */
void vmw_piter_start(struct vmw_piter *viter, const struct vmw_sg_table *vsgt,
		     unsigned long p_offset)
{
	viter->i = p_offset - 1;
	viter->num_pages = vsgt->num_pages;
	viter->pages = vsgt->pages;
	switch (vsgt->mode) {
	case vmw_dma_alloc_coherent:
		viter->next = &__vmw_piter_non_sg_next;
		viter->dma_address = &__vmw_piter_dma_addr;
		viter->addrs = vsgt->addrs;
		break;
	case vmw_dma_map_populate:
	case vmw_dma_map_bind:
		viter->next = &__vmw_piter_sg_next;
		viter->dma_address = &__vmw_piter_sg_addr;
		__sg_page_iter_start(&viter->iter.base, vsgt->sgt->sgl,
				     vsgt->sgt->orig_nents, p_offset);
		break;
	default:
		BUG();
	}
}

/**
 * vmw_ttm_unmap_from_dma - unmap  device addresses previsouly mapped for
 * TTM pages
 *
 * @vmw_tt: Pointer to a struct vmw_ttm_backend
 *
 * Used to free dma mappings previously mapped by vmw_ttm_map_for_dma.
 */
static void vmw_ttm_unmap_from_dma(struct vmw_ttm_tt *vmw_tt)
{
	struct device *dev = vmw_tt->dev_priv->drm.dev;

	dma_unmap_sgtable(dev, &vmw_tt->sgt, DMA_BIDIRECTIONAL, 0);
	vmw_tt->sgt.nents = vmw_tt->sgt.orig_nents;
}

/**
 * vmw_ttm_map_for_dma - map TTM pages to get device addresses
 *
 * @vmw_tt: Pointer to a struct vmw_ttm_backend
 *
 * This function is used to get device addresses from the kernel DMA layer.
 * However, it's violating the DMA API in that when this operation has been
 * performed, it's illegal for the CPU to write to the pages without first
 * unmapping the DMA mappings, or calling dma_sync_sg_for_cpu(). It is
 * therefore only legal to call this function if we know that the function
 * dma_sync_sg_for_cpu() is a NOP, and dma_sync_sg_for_device() is at most
 * a CPU write buffer flush.
 */
static int vmw_ttm_map_for_dma(struct vmw_ttm_tt *vmw_tt)
{
	struct device *dev = vmw_tt->dev_priv->drm.dev;

	return dma_map_sgtable(dev, &vmw_tt->sgt, DMA_BIDIRECTIONAL, 0);
}

/**
 * vmw_ttm_map_dma - Make sure TTM pages are visible to the device
 *
 * @vmw_tt: Pointer to a struct vmw_ttm_tt
 *
 * Select the correct function for and make sure the TTM pages are
 * visible to the device. Allocate storage for the device mappings.
 * If a mapping has already been performed, indicated by the storage
 * pointer being non NULL, the function returns success.
 */
static int vmw_ttm_map_dma(struct vmw_ttm_tt *vmw_tt)
{
	struct vmw_private *dev_priv = vmw_tt->dev_priv;
	struct vmw_sg_table *vsgt = &vmw_tt->vsgt;
	int ret = 0;

	if (vmw_tt->mapped)
		return 0;

	vsgt->mode = dev_priv->map_mode;
	vsgt->pages = vmw_tt->dma_ttm.pages;
	vsgt->num_pages = vmw_tt->dma_ttm.num_pages;
	vsgt->addrs = vmw_tt->dma_ttm.dma_address;
	vsgt->sgt = NULL;

	switch (dev_priv->map_mode) {
	case vmw_dma_map_bind:
	case vmw_dma_map_populate:
		if (vmw_tt->dma_ttm.page_flags  & TTM_TT_FLAG_EXTERNAL) {
			vsgt->sgt = vmw_tt->dma_ttm.sg;
		} else {
			vsgt->sgt = &vmw_tt->sgt;
			ret = sg_alloc_table_from_pages_segment(&vmw_tt->sgt,
				vsgt->pages, vsgt->num_pages, 0,
				(unsigned long)vsgt->num_pages << PAGE_SHIFT,
				dma_get_max_seg_size(dev_priv->drm.dev),
				GFP_KERNEL);
			if (ret)
				goto out_sg_alloc_fail;
		}

		ret = vmw_ttm_map_for_dma(vmw_tt);
		if (unlikely(ret != 0))
			goto out_map_fail;

		break;
	default:
		break;
	}

	vmw_tt->mapped = true;
	return 0;

out_map_fail:
	drm_warn(&dev_priv->drm, "VSG table map failed!");
	sg_free_table(vsgt->sgt);
	vsgt->sgt = NULL;
out_sg_alloc_fail:
	return ret;
}

/**
 * vmw_ttm_unmap_dma - Tear down any TTM page device mappings
 *
 * @vmw_tt: Pointer to a struct vmw_ttm_tt
 *
 * Tear down any previously set up device DMA mappings and free
 * any storage space allocated for them. If there are no mappings set up,
 * this function is a NOP.
 */
static void vmw_ttm_unmap_dma(struct vmw_ttm_tt *vmw_tt)
{
	struct vmw_private *dev_priv = vmw_tt->dev_priv;

	if (!vmw_tt->vsgt.sgt)
		return;

	switch (dev_priv->map_mode) {
	case vmw_dma_map_bind:
	case vmw_dma_map_populate:
		vmw_ttm_unmap_from_dma(vmw_tt);
		sg_free_table(vmw_tt->vsgt.sgt);
		vmw_tt->vsgt.sgt = NULL;
		break;
	default:
		break;
	}
	vmw_tt->mapped = false;
}

/**
 * vmw_bo_sg_table - Return a struct vmw_sg_table object for a
 * TTM buffer object
 *
 * @bo: Pointer to a struct ttm_buffer_object
 *
 * Returns a pointer to a struct vmw_sg_table object. The object should
 * not be freed after use.
 * Note that for the device addresses to be valid, the buffer object must
 * either be reserved or pinned.
 */
const struct vmw_sg_table *vmw_bo_sg_table(struct ttm_buffer_object *bo)
{
	struct vmw_ttm_tt *vmw_tt =
		container_of(bo->ttm, struct vmw_ttm_tt, dma_ttm);

	return &vmw_tt->vsgt;
}


static int vmw_ttm_bind(struct ttm_device *bdev,
			struct ttm_tt *ttm, struct ttm_resource *bo_mem)
{
	struct vmw_ttm_tt *vmw_be =
		container_of(ttm, struct vmw_ttm_tt, dma_ttm);
	int ret = 0;

	if (!bo_mem)
		return -EINVAL;

	if (vmw_be->bound)
		return 0;

	ret = vmw_ttm_map_dma(vmw_be);
	if (unlikely(ret != 0))
		return ret;

	vmw_be->gmr_id = bo_mem->start;
	vmw_be->mem_type = bo_mem->mem_type;

	switch (bo_mem->mem_type) {
	case VMW_PL_GMR:
		ret = vmw_gmr_bind(vmw_be->dev_priv, &vmw_be->vsgt,
				    ttm->num_pages, vmw_be->gmr_id);
		break;
	case VMW_PL_MOB:
		if (unlikely(vmw_be->mob == NULL)) {
			vmw_be->mob =
				vmw_mob_create(ttm->num_pages);
			if (unlikely(vmw_be->mob == NULL))
				return -ENOMEM;
		}

		ret = vmw_mob_bind(vmw_be->dev_priv, vmw_be->mob,
				    &vmw_be->vsgt, ttm->num_pages,
				    vmw_be->gmr_id);
		break;
	case VMW_PL_SYSTEM:
		/* Nothing to be done for a system bind */
		break;
	default:
		BUG();
	}
	vmw_be->bound = true;
	return ret;
}

static void vmw_ttm_unbind(struct ttm_device *bdev,
			   struct ttm_tt *ttm)
{
	struct vmw_ttm_tt *vmw_be =
		container_of(ttm, struct vmw_ttm_tt, dma_ttm);

	if (!vmw_be->bound)
		return;

	switch (vmw_be->mem_type) {
	case VMW_PL_GMR:
		vmw_gmr_unbind(vmw_be->dev_priv, vmw_be->gmr_id);
		break;
	case VMW_PL_MOB:
		vmw_mob_unbind(vmw_be->dev_priv, vmw_be->mob);
		break;
	case VMW_PL_SYSTEM:
		break;
	default:
		BUG();
	}

	if (vmw_be->dev_priv->map_mode == vmw_dma_map_bind)
		vmw_ttm_unmap_dma(vmw_be);
	vmw_be->bound = false;
}


static void vmw_ttm_destroy(struct ttm_device *bdev, struct ttm_tt *ttm)
{
	struct vmw_ttm_tt *vmw_be =
		container_of(ttm, struct vmw_ttm_tt, dma_ttm);

	vmw_ttm_unmap_dma(vmw_be);
	ttm_tt_fini(ttm);
	if (vmw_be->mob)
		vmw_mob_destroy(vmw_be->mob);

	kfree(vmw_be);
}


static int vmw_ttm_populate(struct ttm_device *bdev,
			    struct ttm_tt *ttm, struct ttm_operation_ctx *ctx)
{
	bool external = (ttm->page_flags & TTM_TT_FLAG_EXTERNAL) != 0;

	if (ttm_tt_is_populated(ttm))
		return 0;

	if (external && ttm->sg)
		return  drm_prime_sg_to_dma_addr_array(ttm->sg,
						       ttm->dma_address,
						       ttm->num_pages);

	return ttm_pool_alloc(&bdev->pool, ttm, ctx);
}

static void vmw_ttm_unpopulate(struct ttm_device *bdev,
			       struct ttm_tt *ttm)
{
	struct vmw_ttm_tt *vmw_tt = container_of(ttm, struct vmw_ttm_tt,
						 dma_ttm);
	bool external = (ttm->page_flags & TTM_TT_FLAG_EXTERNAL) != 0;

	if (external)
		return;

	vmw_ttm_unbind(bdev, ttm);

	if (vmw_tt->mob) {
		vmw_mob_destroy(vmw_tt->mob);
		vmw_tt->mob = NULL;
	}

	vmw_ttm_unmap_dma(vmw_tt);

	ttm_pool_free(&bdev->pool, ttm);
}

static struct ttm_tt *vmw_ttm_tt_create(struct ttm_buffer_object *bo,
					uint32_t page_flags)
{
	struct vmw_ttm_tt *vmw_be;
	int ret;
	bool external = bo->type == ttm_bo_type_sg;

	vmw_be = kzalloc(sizeof(*vmw_be), GFP_KERNEL);
	if (!vmw_be)
		return NULL;

	vmw_be->dev_priv = vmw_priv_from_ttm(bo->bdev);
	vmw_be->mob = NULL;

	if (external)
		page_flags |= TTM_TT_FLAG_EXTERNAL | TTM_TT_FLAG_EXTERNAL_MAPPABLE;

	if (vmw_be->dev_priv->map_mode == vmw_dma_alloc_coherent || external)
		ret = ttm_sg_tt_init(&vmw_be->dma_ttm, bo, page_flags,
				     ttm_cached);
	else
		ret = ttm_tt_init(&vmw_be->dma_ttm, bo, page_flags,
				  ttm_cached, 0);
	if (unlikely(ret != 0))
		goto out_no_init;

	return &vmw_be->dma_ttm;
out_no_init:
	kfree(vmw_be);
	return NULL;
}

static void vmw_evict_flags(struct ttm_buffer_object *bo,
		     struct ttm_placement *placement)
{
	*placement = vmw_sys_placement;
}

static int vmw_ttm_io_mem_reserve(struct ttm_device *bdev, struct ttm_resource *mem)
{
	struct vmw_private *dev_priv = vmw_priv_from_ttm(bdev);

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
	case VMW_PL_SYSTEM:
	case VMW_PL_GMR:
	case VMW_PL_MOB:
		return 0;
	case TTM_PL_VRAM:
		mem->bus.offset = (mem->start << PAGE_SHIFT) +
			dev_priv->vram_start;
		mem->bus.is_iomem = true;
		mem->bus.caching = ttm_cached;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/**
 * vmw_move_notify - TTM move_notify_callback
 *
 * @bo: The TTM buffer object about to move.
 * @old_mem: The old memory where we move from
 * @new_mem: The struct ttm_resource indicating to what memory
 *       region the move is taking place.
 *
 * Calls move_notify for all subsystems needing it.
 * (currently only resources).
 */
static void vmw_move_notify(struct ttm_buffer_object *bo,
			    struct ttm_resource *old_mem,
			    struct ttm_resource *new_mem)
{
	vmw_bo_move_notify(bo, new_mem);
	vmw_query_move_notify(bo, old_mem, new_mem);
}


/**
 * vmw_swap_notify - TTM move_notify_callback
 *
 * @bo: The TTM buffer object about to be swapped out.
 */
static void vmw_swap_notify(struct ttm_buffer_object *bo)
{
	vmw_bo_swap_notify(bo);
	(void) ttm_bo_wait(bo, false, false);
}

static bool vmw_memtype_is_system(uint32_t mem_type)
{
	return mem_type == TTM_PL_SYSTEM || mem_type == VMW_PL_SYSTEM;
}

static int vmw_move(struct ttm_buffer_object *bo,
		    bool evict,
		    struct ttm_operation_ctx *ctx,
		    struct ttm_resource *new_mem,
		    struct ttm_place *hop)
{
	struct ttm_resource_manager *new_man;
	struct ttm_resource_manager *old_man = NULL;
	int ret = 0;

	new_man = ttm_manager_type(bo->bdev, new_mem->mem_type);
	if (bo->resource)
		old_man = ttm_manager_type(bo->bdev, bo->resource->mem_type);

	if (new_man->use_tt && !vmw_memtype_is_system(new_mem->mem_type)) {
		ret = vmw_ttm_bind(bo->bdev, bo->ttm, new_mem);
		if (ret)
			return ret;
	}

	if (!bo->resource || (bo->resource->mem_type == TTM_PL_SYSTEM &&
			      bo->ttm == NULL)) {
		ttm_bo_move_null(bo, new_mem);
		return 0;
	}

	vmw_move_notify(bo, bo->resource, new_mem);

	if (old_man && old_man->use_tt && new_man->use_tt) {
		if (vmw_memtype_is_system(bo->resource->mem_type)) {
			ttm_bo_move_null(bo, new_mem);
			return 0;
		}
		ret = ttm_bo_wait_ctx(bo, ctx);
		if (ret)
			goto fail;

		vmw_ttm_unbind(bo->bdev, bo->ttm);
		ttm_resource_free(bo, &bo->resource);
		ttm_bo_assign_mem(bo, new_mem);
		return 0;
	} else {
		ret = ttm_bo_move_memcpy(bo, ctx, new_mem);
		if (ret)
			goto fail;
	}
	return 0;
fail:
	vmw_move_notify(bo, new_mem, bo->resource);
	return ret;
}

struct ttm_device_funcs vmw_bo_driver = {
	.ttm_tt_create = &vmw_ttm_tt_create,
	.ttm_tt_populate = &vmw_ttm_populate,
	.ttm_tt_unpopulate = &vmw_ttm_unpopulate,
	.ttm_tt_destroy = &vmw_ttm_destroy,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = vmw_evict_flags,
	.move = vmw_move,
	.swap_notify = vmw_swap_notify,
	.io_mem_reserve = &vmw_ttm_io_mem_reserve,
};

int vmw_bo_create_and_populate(struct vmw_private *dev_priv,
			       size_t bo_size, u32 domain,
			       struct vmw_bo **bo_p)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false
	};
	struct vmw_bo *vbo;
	int ret;
	struct vmw_bo_params bo_params = {
		.domain = domain,
		.busy_domain = domain,
		.bo_type = ttm_bo_type_kernel,
		.size = bo_size,
		.pin = true,
		.keep_resv = true,
	};

	ret = vmw_bo_create(dev_priv, &bo_params, &vbo);
	if (unlikely(ret != 0))
		return ret;

	ret = vmw_ttm_populate(vbo->tbo.bdev, vbo->tbo.ttm, &ctx);
	if (likely(ret == 0)) {
		struct vmw_ttm_tt *vmw_tt =
			container_of(vbo->tbo.ttm, struct vmw_ttm_tt, dma_ttm);
		ret = vmw_ttm_map_dma(vmw_tt);
	}

	ttm_bo_unreserve(&vbo->tbo);

	if (likely(ret == 0))
		*bo_p = vbo;
	return ret;
}
