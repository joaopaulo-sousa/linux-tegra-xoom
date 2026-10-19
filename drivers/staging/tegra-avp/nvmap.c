// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvmap for the NVIDIA (L4T) media libraries: the /dev/nvmap and /dev/knvmap
 * interface of the NVIDIA/Android 2.6.39 tree (drivers/video/tegra/nvmap),
 * reimplemented on top of the DMA API.
 *
 * The original manages carveouts, the Tegra20 GART and page pools. Here
 * every handle is one physically contiguous allocation from CMA, which is
 * what the AVP and the engines it drives need to see anyway: pinning a
 * handle just reports its address. Memory is write-combined for the CPU,
 * which makes the cache maintenance requests no-ops.
 *
 * Handles are named by a global id, which is also what a client calls the
 * handle in its ioctls (the original used the kernel pointer for both).
 * Another client gets a reference by id once the owner published it
 * (NVMAP_IOC_GET_ID), or always through /dev/knvmap.
 */
#include <linux/dma-direct.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include "nvmap.h"

struct nvmap_client;

struct nvmap_handle {
	struct kref kref;
	u32 id;
	size_t size;			/* page aligned */
	size_t orig_size;		/* as asked for */
	struct mutex lock;		/* allocation */
	bool alloc;
	unsigned int flags;
	void *vaddr;
	dma_addr_t dma;
	atomic_t pin;
	struct nvmap_client *owner;	/* compared only, may be gone */
	bool global;			/* the owner published the id */
};

/* What a client holds of a handle: one handle reference for all dupes */
struct nvmap_ref {
	struct nvmap_handle *h;
	unsigned int dupes;
	unsigned int pins;
};

struct nvmap_client {
	struct xarray refs;		/* id -> struct nvmap_ref */
	struct mutex lock;
	bool super;			/* /dev/knvmap: any id */
};

struct nvmap_vma_priv {
	refcount_t count;
	struct nvmap_handle *h;		/* bound by NVMAP_IOC_MMAP */
	unsigned long offs;
};

static struct platform_device *nvmap_pdev;
static DEFINE_XARRAY_ALLOC1(nvmap_handles);
static const struct vm_operations_struct nvmap_vma_ops;
static struct miscdevice nvmap_knvmap_dev;

static void nvmap_handle_release(struct kref *kref)
{
	struct nvmap_handle *h = container_of(kref, struct nvmap_handle, kref);

	xa_erase(&nvmap_handles, h->id);
	if (h->alloc)
		dma_free_wc(&nvmap_pdev->dev, h->size, h->vaddr, h->dma);
	kfree(h);
}

static void nvmap_handle_put(struct nvmap_handle *h)
{
	kref_put(&h->kref, nvmap_handle_release);
}

/* A reference to handle "id", which the client holds */
static struct nvmap_handle *nvmap_client_handle(struct nvmap_client *client,
						u32 id)
{
	struct nvmap_handle *h = NULL;
	struct nvmap_ref *ref;

	mutex_lock(&client->lock);
	ref = xa_load(&client->refs, id);
	if (ref) {
		h = ref->h;
		kref_get(&h->kref);
	}
	mutex_unlock(&client->lock);
	return h;
}

/* Give the client a reference to h, taking over the caller's one */
static int nvmap_client_add(struct nvmap_client *client, struct nvmap_handle *h)
{
	struct nvmap_ref *ref;
	int err = 0;

	mutex_lock(&client->lock);
	ref = xa_load(&client->refs, h->id);
	if (ref) {
		ref->dupes++;
		nvmap_handle_put(h);
		goto out;
	}

	/* on failure the caller keeps its reference */
	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref) {
		err = -ENOMEM;
		goto out;
	}
	ref->h = h;
	ref->dupes = 1;
	err = xa_err(xa_store(&client->refs, h->id, ref, GFP_KERNEL));
	if (err)
		kfree(ref);
out:
	mutex_unlock(&client->lock);
	return err;
}

static void nvmap_ref_drop(struct nvmap_ref *ref)
{
	atomic_sub(ref->pins, &ref->h->pin);
	nvmap_handle_put(ref->h);
	kfree(ref);
}

static int nvmap_ioctl_create(struct nvmap_client *client, unsigned int cmd,
			      void __user *arg)
{
	struct nvmap_create_handle op;
	struct nvmap_handle *h;
	int err;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	if (cmd == NVMAP_IOC_CREATE) {
		if (!op.size)
			return -EINVAL;
		h = kzalloc(sizeof(*h), GFP_KERNEL);
		if (!h)
			return -ENOMEM;
		kref_init(&h->kref);
		mutex_init(&h->lock);
		h->size = PAGE_ALIGN(op.size);
		h->orig_size = op.size;
		h->owner = client;
		err = xa_alloc(&nvmap_handles, &h->id, h, xa_limit_32b, GFP_KERNEL);
		if (err) {
			kfree(h);
			return err;
		}
	} else {
		/* NVMAP_IOC_FROM_ID */
		xa_lock(&nvmap_handles);
		h = xa_load(&nvmap_handles, op.id);
		if (h && !kref_get_unless_zero(&h->kref))
			h = NULL;
		xa_unlock(&nvmap_handles);
		if (!h)
			return -EPERM;
		if (!h->global && !client->super && h->owner != client) {
			nvmap_handle_put(h);
			return -EPERM;
		}
	}

	op.handle = h->id;
	err = nvmap_client_add(client, h);
	if (err) {
		nvmap_handle_put(h);
		return err;
	}

	return copy_to_user(arg, &op, sizeof(op)) ? -EFAULT : 0;
}

static int nvmap_ioctl_free(struct nvmap_client *client, u32 id)
{
	struct nvmap_ref *ref;

	mutex_lock(&client->lock);
	ref = xa_load(&client->refs, id);
	if (ref && !--ref->dupes) {
		xa_erase(&client->refs, id);
		nvmap_ref_drop(ref);
	}
	mutex_unlock(&client->lock);
	return 0;
}

static int nvmap_ioctl_alloc(struct nvmap_client *client, void __user *arg)
{
	struct nvmap_alloc_handle op;
	struct nvmap_handle *h;
	int err = 0;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;
	if (op.align & (op.align - 1))
		return -EINVAL;
	/* every heap is contiguous memory here, but IRAM is not offered */
	if (!(op.heap_mask & ~NVMAP_HEAP_CARVEOUT_IRAM))
		return -ENOMEM;

	h = nvmap_client_handle(client, op.handle);
	if (!h)
		return -EPERM;

	mutex_lock(&h->lock);
	if (!h->alloc) {
		/* CMA hands out blocks aligned to their order, which covers
		 * the alignments asked for (page to a few hundred KiB) */
		if (op.align > (PAGE_SIZE << get_order(h->size)))
			dev_warn(&nvmap_pdev->dev, "alignment %u for %zu bytes\n",
				 op.align, h->size);
		h->vaddr = dma_alloc_wc(&nvmap_pdev->dev, h->size, &h->dma,
					GFP_KERNEL);
		if (h->vaddr) {
			h->flags = op.flags;
			h->alloc = true;
		} else {
			err = -ENOMEM;
		}
	}
	mutex_unlock(&h->lock);

	nvmap_handle_put(h);
	return err;
}

static int nvmap_ioctl_getid(struct nvmap_client *client, void __user *arg)
{
	struct nvmap_create_handle op;
	struct nvmap_handle *h;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	h = nvmap_client_handle(client, op.handle);
	if (!h)
		return -EPERM;
	if (h->owner == client)
		h->global = true;
	op.id = h->id;
	nvmap_handle_put(h);

	return copy_to_user(arg, &op, sizeof(op)) ? -EFAULT : 0;
}

static int nvmap_ioctl_param(struct nvmap_client *client, void __user *arg)
{
	struct nvmap_handle_param op;
	struct nvmap_handle *h;
	int err = 0;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;

	h = nvmap_client_handle(client, op.handle);
	if (!h)
		return -EINVAL;

	switch (op.param) {
	case NVMAP_HANDLE_PARAM_SIZE:
		op.result = h->orig_size;
		break;
	case NVMAP_HANDLE_PARAM_ALIGNMENT:
		op.result = h->alloc ? PAGE_SIZE : 0;
		break;
	case NVMAP_HANDLE_PARAM_BASE:
		op.result = h->alloc && atomic_read(&h->pin) ? h->dma : -1ul;
		break;
	case NVMAP_HANDLE_PARAM_HEAP:
		op.result = h->alloc ? NVMAP_HEAP_CARVEOUT_GENERIC : 0;
		break;
	default:
		err = -EINVAL;
	}
	nvmap_handle_put(h);

	if (!err && copy_to_user(arg, &op, sizeof(op)))
		err = -EFAULT;
	return err;
}

/* Pin or unpin "count" of the client's handles; pinning reports addresses */
static int nvmap_pin_ids(struct nvmap_client *client, bool pin, u32 *ids,
			 unsigned int count, unsigned long *addrs)
{
	unsigned int i;
	int err = 0;

	mutex_lock(&client->lock);
	for (i = 0; i < count; i++) {
		struct nvmap_ref *ref = xa_load(&client->refs, ids[i]);

		if (!ref || (pin && !ref->h->alloc)) {
			err = -EPERM;
			break;
		}
		if (pin) {
			ref->pins++;
			atomic_inc(&ref->h->pin);
			addrs[i] = ref->h->dma;
		} else if (ref->pins) {
			ref->pins--;
			atomic_dec(&ref->h->pin);
		}
	}
	/* all or nothing */
	while (err && pin && i--) {
		struct nvmap_ref *ref = xa_load(&client->refs, ids[i]);

		ref->pins--;
		atomic_dec(&ref->h->pin);
	}
	mutex_unlock(&client->lock);
	return err;
}

static int nvmap_ioctl_pinop(struct nvmap_client *client, bool pin,
			     struct nvmap_pin_handle __user *arg)
{
	struct nvmap_pin_handle op;
	u32 on_stack[16], *ids = on_stack;
	unsigned long out_stack[16], *addrs = out_stack;
	int err;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;
	if (!op.count || op.count > 4096)
		return -EINVAL;

	if (op.count > ARRAY_SIZE(on_stack)) {
		ids = kmalloc_array(op.count, sizeof(*ids), GFP_KERNEL);
		addrs = kmalloc_array(op.count, sizeof(*addrs), GFP_KERNEL);
		if (!ids || !addrs) {
			err = -ENOMEM;
			goto out;
		}
	}

	/* one handle is passed by value, several as an array */
	if (op.count == 1) {
		ids[0] = op.handles;
	} else if (copy_from_user(ids, (void __user *)op.handles,
				  op.count * sizeof(*ids))) {
		err = -EFAULT;
		goto out;
	}

	err = nvmap_pin_ids(client, pin, ids, op.count, addrs);
	if (err || !pin)
		goto out;

	if (op.count == 1) {
		err = put_user(addrs[0], &arg->addr);
	} else if (op.addr) {
		err = copy_to_user((void __user *)op.addr, addrs,
				   op.count * sizeof(*addrs)) ? -EFAULT : 0;
	}
	if (err)
		nvmap_pin_ids(client, false, ids, op.count, addrs);
out:
	if (ids != on_stack)
		kfree(ids);
	if (addrs != out_stack)
		kfree(addrs);
	return err;
}

static int nvmap_ioctl_rw(struct nvmap_client *client, bool read,
			  struct nvmap_rw_handle __user *arg)
{
	struct nvmap_rw_handle op;
	struct nvmap_handle *h;
	size_t copied = 0;
	int err = 0;
	u32 i;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;
	if (!op.addr || !op.count || !op.elem_size)
		return -EINVAL;

	h = nvmap_client_handle(client, op.handle);
	if (!h)
		return -EPERM;
	if (!h->alloc ||
	    op.offset + (u64)(op.count - 1) * op.hmem_stride + op.elem_size > h->size) {
		err = -EFAULT;
		goto out;
	}

	for (i = 0; i < op.count; i++) {
		void *hp = h->vaddr + op.offset + (size_t)i * op.hmem_stride;
		void __user *up = (void __user *)(op.addr + (size_t)i * op.user_stride);

		if (read ? copy_to_user(up, hp, op.elem_size) :
			   copy_from_user(hp, up, op.elem_size)) {
			err = -EFAULT;
			break;
		}
		copied += op.elem_size;
	}
	if (put_user(copied, &arg->count))
		err = -EFAULT;
out:
	nvmap_handle_put(h);
	return err;
}

/* Bind a handle to a VMA made by mmap() of the device */
static int nvmap_ioctl_mmap(struct nvmap_client *client, void __user *arg)
{
	struct nvmap_map_caller op;
	struct nvmap_vma_priv *vpriv;
	struct vm_area_struct *vma;
	struct nvmap_handle *h;
	int err = 0;

	if (copy_from_user(&op, arg, sizeof(op)))
		return -EFAULT;
	if (op.offset & ~PAGE_MASK)
		return -EFAULT;

	h = nvmap_client_handle(client, op.handle);
	if (!h)
		return -EPERM;
	if ((u64)op.offset + op.length > h->size) {
		nvmap_handle_put(h);
		return -EADDRNOTAVAIL;
	}

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, op.addr);
	if (!vma || vma->vm_ops != &nvmap_vma_ops || vma->vm_start != op.addr ||
	    vma->vm_end - vma->vm_start != op.length) {
		err = -EPERM;
		goto out;
	}
	vpriv = vma->vm_private_data;
	if (vpriv->h) {
		err = vpriv->h == h ? 0 : -EADDRNOTAVAIL;
		goto out;
	}
	vpriv->h = h;		/* takes over the reference */
	vpriv->offs = op.offset;
	h = NULL;
out:
	mmap_read_unlock(current->mm);
	if (h)
		nvmap_handle_put(h);
	return err;
}

static long nvmap_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct nvmap_client *client = filp->private_data;
	void __user *uarg = (void __user *)arg;

	if (_IOC_TYPE(cmd) != NVMAP_IOC_MAGIC || _IOC_NR(cmd) > NVMAP_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case NVMAP_IOC_CREATE:
	case NVMAP_IOC_FROM_ID:
		return nvmap_ioctl_create(client, cmd, uarg);
	case NVMAP_IOC_CLAIM:
		return -ENODEV;		/* preserved handles: not supported */
	case NVMAP_IOC_ALLOC:
		return nvmap_ioctl_alloc(client, uarg);
	case NVMAP_IOC_FREE:
		return nvmap_ioctl_free(client, arg);
	case NVMAP_IOC_MMAP:
		return nvmap_ioctl_mmap(client, uarg);
	case NVMAP_IOC_WRITE:
	case NVMAP_IOC_READ:
		return nvmap_ioctl_rw(client, cmd == NVMAP_IOC_READ, uarg);
	case NVMAP_IOC_PARAM:
		return nvmap_ioctl_param(client, uarg);
	case NVMAP_IOC_PIN_MULT:
	case NVMAP_IOC_UNPIN_MULT:
		return nvmap_ioctl_pinop(client, cmd == NVMAP_IOC_PIN_MULT, uarg);
	case NVMAP_IOC_CACHE:
		return 0;		/* write-combined: nothing to maintain */
	case NVMAP_IOC_GET_ID:
		return nvmap_ioctl_getid(client, uarg);
	}
	return -ENOTTY;
}

static void nvmap_vma_open(struct vm_area_struct *vma)
{
	struct nvmap_vma_priv *vpriv = vma->vm_private_data;

	refcount_inc(&vpriv->count);
}

static void nvmap_vma_close(struct vm_area_struct *vma)
{
	struct nvmap_vma_priv *vpriv = vma->vm_private_data;

	if (refcount_dec_and_test(&vpriv->count)) {
		if (vpriv->h)
			nvmap_handle_put(vpriv->h);
		kfree(vpriv);
	}
}

static vm_fault_t nvmap_vma_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct nvmap_vma_priv *vpriv = vma->vm_private_data;
	unsigned long offs;

	if (!vpriv->h)
		return VM_FAULT_SIGBUS;
	offs = (vmf->address - vma->vm_start) + vpriv->offs;
	if (offs >= vpriv->h->size)
		return VM_FAULT_SIGBUS;

	return vmf_insert_pfn(vma, vmf->address & PAGE_MASK,
			      PHYS_PFN(dma_to_phys(&nvmap_pdev->dev, vpriv->h->dma) + offs));
}

static const struct vm_operations_struct nvmap_vma_ops = {
	.open	= nvmap_vma_open,
	.close	= nvmap_vma_close,
	.fault	= nvmap_vma_fault,
};

static int nvmap_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct nvmap_vma_priv *vpriv;

	vpriv = kzalloc(sizeof(*vpriv), GFP_KERNEL);
	if (!vpriv)
		return -ENOMEM;
	refcount_set(&vpriv->count, 1);

	vma->vm_private_data = vpriv;
	vma->vm_ops = &nvmap_vma_ops;
	vm_flags_set(vma, VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return 0;
}

static int nvmap_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *misc = filp->private_data;
	struct nvmap_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	xa_init(&client->refs);
	mutex_init(&client->lock);
	client->super = misc == &nvmap_knvmap_dev;

	filp->private_data = client;
	return nonseekable_open(inode, filp);
}

static int nvmap_release(struct inode *inode, struct file *filp)
{
	struct nvmap_client *client = filp->private_data;
	struct nvmap_ref *ref;
	unsigned long id;

	xa_for_each(&client->refs, id, ref)
		nvmap_ref_drop(ref);
	xa_destroy(&client->refs);
	kfree(client);
	return 0;
}

static const struct file_operations nvmap_fops = {
	.owner		= THIS_MODULE,
	.open		= nvmap_open,
	.release	= nvmap_release,
	.unlocked_ioctl	= nvmap_ioctl,
	.mmap		= nvmap_mmap,
};

static struct miscdevice nvmap_user_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "nvmap",
	.fops	= &nvmap_fops,
	.mode	= 0666,
};

static struct miscdevice nvmap_knvmap_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "knvmap",
	.fops	= &nvmap_fops,
	.mode	= 0600,
};

int tegra_nvmap_init(void)
{
	int err;

	nvmap_pdev = platform_device_register_simple("tegra-nvmap", -1, NULL, 0);
	if (IS_ERR(nvmap_pdev))
		return PTR_ERR(nvmap_pdev);
	err = dma_coerce_mask_and_coherent(&nvmap_pdev->dev, DMA_BIT_MASK(32));
	if (err)
		goto err_pdev;

	err = misc_register(&nvmap_user_dev);
	if (err)
		goto err_pdev;
	err = misc_register(&nvmap_knvmap_dev);
	if (err)
		goto err_user;
	return 0;

err_user:
	misc_deregister(&nvmap_user_dev);
err_pdev:
	platform_device_unregister(nvmap_pdev);
	return err;
}

void tegra_nvmap_exit(void)
{
	misc_deregister(&nvmap_knvmap_dev);
	misc_deregister(&nvmap_user_dev);
	platform_device_unregister(nvmap_pdev);
}
