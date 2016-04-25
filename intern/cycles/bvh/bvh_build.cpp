/*
 * Adapted from code copyright 2009-2010 NVIDIA Corporation
 * Modifications Copyright 2011, Blender Foundation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bvh_binning.h"
#include "bvh_build.h"
#include "bvh_node.h"
#include "bvh_params.h"
#include "bvh_split.h"

#include "mesh.h"
#include "object.h"
#include "scene.h"
#include "curves.h"

#include "util_debug.h"
#include "util_foreach.h"
#include "util_logging.h"
#include "util_progress.h"
#include "util_stack_allocator.h"
#include "util_simd.h"
#include "util_time.h"

CCL_NAMESPACE_BEGIN

/* BVH Build Task */

class BVHBuildTask : public Task {
public:
	BVHBuildTask(BVHBuild *build,
	             InnerNode *node,
	             int child,
	             const BVHObjectBinning& range,
	             int level)
	: range_(range)
	{
		run = function_bind(&BVHBuild::thread_build_node,
		                    build,
		                    node,
		                    child,
		                    &range_,
		                    level);
	}
private:
	BVHObjectBinning range_;
};

class BVHSpatialSplitBuildTask : public Task {
public:
	BVHSpatialSplitBuildTask(BVHBuild *build,
	                         InnerNode *node,
	                         int child,
	                         const BVHRange& range,
	                         const vector<BVHReference>& references,
	                         int level)
	: range_(range),
	  references_(references.begin() + range.start(),
	              references.begin() + range.end())
	{
		range_.set_start(0);
		run = function_bind(&BVHBuild::thread_build_spatial_split_node,
		                    build,
		                    node,
		                    child,
		                    &range_,
		                    &references_,
		                    level,
		                    _1);
	}
private:
	BVHRange range_;
	vector<BVHReference> references_;
};

/* Constructor / Destructor */

BVHBuild::BVHBuild(const vector<Object*>& objects_,
                   array<int>& prim_type_,
                   array<int>& prim_index_,
                   array<int>& prim_object_,
                   const BVHParams& params_,
                   Progress& progress_)
 : objects(objects_),
   prim_type(prim_type_),
   prim_index(prim_index_),
   prim_object(prim_object_),
   params(params_),
   progress(progress_),
   progress_start_time(0.0)
{
	spatial_min_overlap = 0.0f;
}

BVHBuild::~BVHBuild()
{
}

/* Adding References */

void BVHBuild::add_reference_mesh(BoundBox& root, BoundBox& center, Mesh *mesh, int i)
{
	Attribute *attr_mP = NULL;
	
	if(mesh->has_motion_blur())
		attr_mP = mesh->attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);

	for(uint j = 0; j < mesh->triangles.size(); j++) {
		Mesh::Triangle t = mesh->triangles[j];
		BoundBox bounds = BoundBox::empty;
		PrimitiveType type = PRIMITIVE_TRIANGLE;

		t.bounds_grow(&mesh->verts[0], bounds);

		/* motion triangles */
		if(attr_mP) {
			size_t mesh_size = mesh->verts.size();
			size_t steps = mesh->motion_steps - 1;
			float3 *vert_steps = attr_mP->data_float3();

			for(size_t i = 0; i < steps; i++)
				t.bounds_grow(vert_steps + i*mesh_size, bounds);

			type = PRIMITIVE_MOTION_TRIANGLE;
		}

		if(bounds.valid()) {
			references.push_back(BVHReference(bounds, j, i, type));
			root.grow(bounds);
			center.grow(bounds.center2());
		}
	}

	Attribute *curve_attr_mP = NULL;

	if(mesh->has_motion_blur())
		curve_attr_mP = mesh->curve_attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);

	for(uint j = 0; j < mesh->curves.size(); j++) {
		Mesh::Curve curve = mesh->curves[j];
		PrimitiveType type = PRIMITIVE_CURVE;

		for(int k = 0; k < curve.num_keys - 1; k++) {
			BoundBox bounds = BoundBox::empty;
			curve.bounds_grow(k, &mesh->curve_keys[0], bounds);

			/* motion curve */
			if(curve_attr_mP) {
				size_t mesh_size = mesh->curve_keys.size();
				size_t steps = mesh->motion_steps - 1;
				float4 *key_steps = curve_attr_mP->data_float4();

				for(size_t i = 0; i < steps; i++)
					curve.bounds_grow(k, key_steps + i*mesh_size, bounds);

				type = PRIMITIVE_MOTION_CURVE;
			}

			if(bounds.valid()) {
				int packed_type = PRIMITIVE_PACK_SEGMENT(type, k);
				
				references.push_back(BVHReference(bounds, j, i, packed_type));
				root.grow(bounds);
				center.grow(bounds.center2());
			}
		}
	}
}

void BVHBuild::add_reference_object(BoundBox& root, BoundBox& center, Object *ob, int i)
{
	references.push_back(BVHReference(ob->bounds, -1, i, 0));
	root.grow(ob->bounds);
	center.grow(ob->bounds.center2());
}

static size_t count_curve_segments(Mesh *mesh)
{
	size_t num = 0, num_curves = mesh->curves.size();

	for(size_t i = 0; i < num_curves; i++)
		num += mesh->curves[i].num_keys - 1;
	
	return num;
}

void BVHBuild::add_references(BVHRange& root)
{
	/* reserve space for references */
	size_t num_alloc_references = 0;

	foreach(Object *ob, objects) {
		if(params.top_level) {
			if(!ob->mesh->is_instanced()) {
				num_alloc_references += ob->mesh->triangles.size();
				num_alloc_references += count_curve_segments(ob->mesh);
			}
			else
				num_alloc_references++;
		}
		else {
			num_alloc_references += ob->mesh->triangles.size();
			num_alloc_references += count_curve_segments(ob->mesh);
		}
	}

	references.reserve(num_alloc_references);

	/* add references from objects */
	BoundBox bounds = BoundBox::empty, center = BoundBox::empty;
	int i = 0;

	foreach(Object *ob, objects) {
		if(params.top_level) {
			if(!ob->mesh->is_instanced())
				add_reference_mesh(bounds, center, ob->mesh, i);
			else
				add_reference_object(bounds, center, ob, i);
		}
		else
			add_reference_mesh(bounds, center, ob->mesh, i);

		i++;

		if(progress.get_cancel()) return;
	}

	/* happens mostly on empty meshes */
	if(!bounds.valid())
		bounds.grow(make_float3(0.0f, 0.0f, 0.0f));

	root = BVHRange(bounds, center, 0, references.size());
}

/* Build */

BVHNode* BVHBuild::run()
{
	BVHRange root;

	/* add references */
	add_references(root);

	if(progress.get_cancel())
		return NULL;

	/* init spatial splits */
	if(params.top_level) {
		/* NOTE: Technically it is supported by the builder but it's not really
		 * optimized for speed yet and not really clear yet if it has measurable
		 * improvement on render time. Needs some extra investigation before
		 * enabling spatial split for top level BVH.
		 */
		params.use_spatial_split = false;
	}

	spatial_min_overlap = root.bounds().safe_area() * params.spatial_split_alpha;
	if(params.use_spatial_split) {
		/* NOTE: The API here tries to be as much ready for multi-threaded build
		 * as possible, but at the same time it tries not to introduce any
		 * changes in behavior for until all refactoring needed for threading is
		 * finished.
		 *
		 * So we currently allocate single storage for now, which is only used by
		 * the only thread working on the spatial BVH build.
		 */
		spatial_storage.resize(TaskScheduler::num_threads() + 1);
		size_t num_bins = max(root.size(), (int)BVHParams::NUM_SPATIAL_BINS) - 1;
		foreach(BVHSpatialStorage &storage, spatial_storage) {
			storage.right_bounds.clear();
		}
		spatial_storage[0].right_bounds.resize(num_bins);
	}
	spatial_free_index = 0;

	/* init progress updates */
	double build_start_time;
	build_start_time = progress_start_time = time_dt();
	progress_count = 0;
	progress_total = references.size();
	progress_original_total = progress_total;

	prim_type.resize(references.size());
	prim_index.resize(references.size());
	prim_object.resize(references.size());

	/* build recursively */
	BVHNode *rootnode;

	if(params.use_spatial_split) {
		/* Perform multithreaded spatial split build. */
		rootnode = build_node(root, &references, 0, 0);
		task_pool.wait_work();
	}
	else {
		/* Perform multithreaded binning build. */
		BVHObjectBinning rootbin(root, (references.size())? &references[0]: NULL);
		rootnode = build_node(rootbin, 0);
		task_pool.wait_work();
	}

	/* delete if we canceled */
	if(rootnode) {
		if(progress.get_cancel()) {
			rootnode->deleteSubtree();
			rootnode = NULL;
			VLOG(1) << "BVH build cancelled.";
		}
		else {
			/*rotate(rootnode, 4, 5);*/
			rootnode->update_visibility();
		}
		if(rootnode != NULL) {
			VLOG(1) << "BVH build statistics:\n"
			        << "  Build time: " << time_dt() - build_start_time << "\n"
			        << "  Total number of nodes: "
			        << rootnode->getSubtreeSize(BVH_STAT_NODE_COUNT) << "\n"
			        << "  Number of inner nodes: "
			        << rootnode->getSubtreeSize(BVH_STAT_INNER_COUNT)  << "\n"
			        << "  Number of leaf nodes: "
			        << rootnode->getSubtreeSize(BVH_STAT_LEAF_COUNT)  << "\n"
			        << "  Allocation slop factor: "
			               << ((prim_type.capacity() != 0)
			                       ? (float)prim_type.size() / prim_type.capacity()
			                       : 1.0f) << "\n";
		}
	}


	return rootnode;
}

void BVHBuild::progress_update()
{
	if(time_dt() - progress_start_time < 0.25)
		return;
	
	double progress_start = (double)progress_count/(double)progress_total;
	double duplicates = (double)(progress_total - progress_original_total)/(double)progress_total;

	string msg = string_printf("Building BVH %.0f%%, duplicates %.0f%%",
	                           progress_start * 100.0, duplicates * 100.0);

	progress.set_substatus(msg);
	progress_start_time = time_dt(); 
}

void BVHBuild::thread_build_node(InnerNode *inner,
                                 int child,
                                 BVHObjectBinning *range,
                                 int level)
{
	if(progress.get_cancel())
		return;

	/* build nodes */
	BVHNode *node = build_node(*range, level);

	/* set child in inner node */
	inner->children[child] = node;

	/* update progress */
	if(range->size() < THREAD_TASK_SIZE) {
		/*rotate(node, INT_MAX, 5);*/

		thread_scoped_lock lock(build_mutex);

		progress_count += range->size();
		progress_update();
	}
}

void BVHBuild::thread_build_spatial_split_node(InnerNode *inner,
                                               int child,
                                               BVHRange *range,
                                               vector<BVHReference> *references,
                                               int level,
                                               int thread_id)
{
	if(progress.get_cancel()) {
		return;
	}

	/* build nodes */
	BVHNode *node = build_node(*range, references, level, thread_id);

	/* set child in inner node */
	inner->children[child] = node;
}

bool BVHBuild::range_within_max_leaf_size(const BVHRange& range,
                                          const vector<BVHReference>& references) const
{
	size_t size = range.size();
	size_t max_leaf_size = max(params.max_triangle_leaf_size, params.max_curve_leaf_size);

	if(size > max_leaf_size)
		return false;

	size_t num_triangles = 0;
	size_t num_curves = 0;
	size_t num_motion_curves = 0;

	for(int i = 0; i < size; i++) {
		const BVHReference& ref = references[range.start() + i];

		if(ref.prim_type() & PRIMITIVE_CURVE)
			num_curves++;
		if(ref.prim_type() & PRIMITIVE_MOTION_CURVE)
			num_motion_curves++;
		else if(ref.prim_type() & PRIMITIVE_ALL_TRIANGLE)
			num_triangles++;
	}

	return (num_triangles < params.max_triangle_leaf_size) &&
	       (num_curves < params.max_curve_leaf_size) &&
	       (num_motion_curves < params.max_curve_leaf_size);
}

/* multithreaded binning builder */
BVHNode* BVHBuild::build_node(const BVHObjectBinning& range, int level)
{
	size_t size = range.size();
	float leafSAH = params.sah_primitive_cost * range.leafSAH;
	float splitSAH = params.sah_node_cost * range.bounds().half_area() + params.sah_primitive_cost * range.splitSAH;

	/* have at least one inner node on top level, for performance and correct
	 * visibility tests, since object instances do not check visibility flag */
	if(!(range.size() > 0 && params.top_level && level == 0)) {
		/* make leaf node when threshold reached or SAH tells us */
		if((params.small_enough_for_leaf(size, level)) ||
		   (range_within_max_leaf_size(range, references) && leafSAH < splitSAH))
		{
			return create_leaf_node(range, references);
		}
	}

	/* perform split */
	BVHObjectBinning left, right;
	range.split(&references[0], left, right);

	/* create inner node. */
	InnerNode *inner;

	if(range.size() < THREAD_TASK_SIZE) {
		/* local build */
		BVHNode *leftnode = build_node(left, level + 1);
		BVHNode *rightnode = build_node(right, level + 1);

		inner = new InnerNode(range.bounds(), leftnode, rightnode);
	}
	else {
		/* threaded build */
		inner = new InnerNode(range.bounds());

		task_pool.push(new BVHBuildTask(this, inner, 0, left, level + 1), true);
		task_pool.push(new BVHBuildTask(this, inner, 1, right, level + 1), true);
	}

	return inner;
}

/* multithreaded spatial split builder */
BVHNode* BVHBuild::build_node(const BVHRange& range,
                              vector<BVHReference> *references,
                              int level,
                              int thread_id)
{
	/* Update progress.
	 *
	 * TODO(sergey): Currently it matches old behavior, but we can move it to the
	 * task thread (which will mimic non=split builder) and save some CPU ticks
	 * on checking cancel status.
	 */
	progress_update();
	if(progress.get_cancel()) {
		return NULL;
	}

	/* Small enough or too deep => create leaf. */
	if(!(range.size() > 0 && params.top_level && level == 0)) {
		if(params.small_enough_for_leaf(range.size(), level)) {
			progress_count += range.size();
			return create_leaf_node(range, *references);
		}
	}

	/* Perform splitting test. */
	BVHSpatialStorage *storage = &spatial_storage[thread_id];
	BVHMixedSplit split(this, storage, range, references, level);

	if(!(range.size() > 0 && params.top_level && level == 0)) {
		if(split.no_split) {
			progress_count += range.size();
			return create_leaf_node(range, *references);
		}
	}

	/* Do split. */
	BVHRange left, right;
	split.split(this, left, right, range);

	progress_total += left.size() + right.size() - range.size();

	/* Create inner node. */
	InnerNode *inner;

	if(range.size() < THREAD_TASK_SIZE) {
		/* Local build. */

		/* Build left node. */
		vector<BVHReference> copy(references->begin() + right.start(),
		                          references->begin() + right.end());
		right.set_start(0);

		BVHNode *leftnode = build_node(left, references, level + 1, thread_id);

		/* Build right node. */
		BVHNode *rightnode = build_node(right, &copy, level + 1, thread_id);

		inner = new InnerNode(range.bounds(), leftnode, rightnode);
	}
	else {
		/* Threaded build. */
		inner = new InnerNode(range.bounds());
		task_pool.push(new BVHSpatialSplitBuildTask(this,
		                                            inner,
		                                            0,
		                                            left,
		                                            *references,
		                                            level + 1),
		               true);
		task_pool.push(new BVHSpatialSplitBuildTask(this,
		                                            inner,
		                                            1,
		                                            right,
		                                            *references,
		                                            level + 1),
		               true);
	}

	return inner;
}

/* Create Nodes */

BVHNode *BVHBuild::create_object_leaf_nodes(const BVHReference *ref, int start, int num)
{
	if(num == 0) {
		BoundBox bounds = BoundBox::empty;
		return new LeafNode(bounds, 0, 0, 0);
	}
	else if(num == 1) {
		assert(start < prim_type.size());
		prim_type[start] = ref->prim_type();
		prim_index[start] = ref->prim_index();
		prim_object[start] = ref->prim_object();

		uint visibility = objects[ref->prim_object()]->visibility;
		return new LeafNode(ref->bounds(), visibility, start, start+1);
	}
	else {
		int mid = num/2;
		BVHNode *leaf0 = create_object_leaf_nodes(ref, start, mid); 
		BVHNode *leaf1 = create_object_leaf_nodes(ref+mid, start+mid, num-mid); 

		BoundBox bounds = BoundBox::empty;
		bounds.grow(leaf0->m_bounds);
		bounds.grow(leaf1->m_bounds);

		return new InnerNode(bounds, leaf0, leaf1);
	}
}

BVHNode* BVHBuild::create_leaf_node(const BVHRange& range,
                                    const vector<BVHReference>& references)
{
	/* This is a bit overallocating here (considering leaf size into account),
	 * but chunk-based re-allocation in vector makes it difficult to use small
	 * size of stack storage here. Some tweaks are possible tho.
	 *
	 * NOTES:
	 *  - If the size is too big, we'll have inefficient stack usage,
	 *    and lots of cache misses.
	 *  - If the size is too small, then we can run out of memory
	 *    allowed to be used by vector.
	 *    In practice it wouldn't mean crash, just allocator will fallback
	 *    to heap which is slower.
	 *  - Optimistic re-allocation in STL could jump us out of stack usage
	 *    because re-allocation happens in chunks and size of those chunks we
	 *    can not control.
	 */
	typedef StackAllocator<256, int> LeafStackAllocator;

	vector<int, LeafStackAllocator> p_type[PRIMITIVE_NUM_TOTAL];
	vector<int, LeafStackAllocator> p_index[PRIMITIVE_NUM_TOTAL];
	vector<int, LeafStackAllocator> p_object[PRIMITIVE_NUM_TOTAL];
	/* TODO(sergey): In theory we should be able to store references. */
	vector<BVHReference, LeafStackAllocator> object_references;

	uint visibility[PRIMITIVE_NUM_TOTAL] = {0};
	/* NOTE: Keep initializtion in sync with actual number of primitives. */
	BoundBox bounds[PRIMITIVE_NUM_TOTAL] = {BoundBox::empty,
	                                        BoundBox::empty,
	                                        BoundBox::empty,
	                                        BoundBox::empty};
	int ob_num = 0;
	int num_new_prims = 0;
	/* Fill in per-type type/index array. */
	for(int i = 0; i < range.size(); i++) {
		const BVHReference& ref = references[range.start() + i];
		if(ref.prim_index() != -1) {
			int type_index = bitscan(ref.prim_type() & PRIMITIVE_ALL);
			p_type[type_index].push_back(ref.prim_type());
			p_index[type_index].push_back(ref.prim_index());
			p_object[type_index].push_back(ref.prim_object());

			bounds[type_index].grow(ref.bounds());
			visibility[type_index] |= objects[ref.prim_object()]->visibility;
			++num_new_prims;
		}
		else {
			object_references.push_back(ref);
			++ob_num;
		}
	}

	/* Create leaf nodes for every existing primitive.
	 *
	 * Here we write primitive types, indices and objects to a temporary array.
	 * This way we keep all the heavy memory allocation code outside of the
	 * thread lock in the case of spatial split building.
	 *
	 * TODO(sergey): With some pointer trickery we can write directly to the
	 * destination buffers for the non-spatial split BVH.
	 */
	BVHNode *leaves[PRIMITIVE_NUM_TOTAL + 1] = {NULL};
	int num_leaves = 0;
	size_t start_index = 0;
	vector<int, LeafStackAllocator> local_prim_type,
	                                local_prim_index,
	                                local_prim_object;
	local_prim_type.resize(num_new_prims);
	local_prim_index.resize(num_new_prims);
	local_prim_object.resize(num_new_prims);
	for(int i = 0; i < PRIMITIVE_NUM_TOTAL; ++i) {
		int num = (int)p_type[i].size();
		if(num != 0) {
			assert(p_type[i].size() == p_index[i].size());
			assert(p_type[i].size() == p_object[i].size());
			for(int j = 0; j < num; ++j) {
				const int index = start_index + j;
				local_prim_type[index] = p_type[i][j];
				local_prim_index[index] = p_index[i][j];
				local_prim_object[index] = p_object[i][j];
			}
			leaves[num_leaves++] = new LeafNode(bounds[i],
			                                    visibility[i],
			                                    start_index,
			                                    start_index + num);
			start_index += num;
		}
	}
	/* Get size of new data to be copied to the packed arrays. */
	const int num_new_leaf_data = start_index;
	const size_t new_leaf_data_size = sizeof(int) * num_new_leaf_data;
	/* Copy actual data to the packed array. */
	if(params.use_spatial_split) {
		spatial_spin_lock.lock();
		/* We use first free index in the packed arrays and mode pointer to the
		 * end of the current range.
		 *
		 * This doesn't give deterministic packed arrays, but it shouldn't really
		 * matter because order of children in BVH is deterministic.
		 */
		start_index = spatial_free_index;
		spatial_free_index += range.size();

		/* Extend an array when needed. */
		const size_t range_end = start_index + range.size();
		if(prim_type.size() < range_end) {
			/* Avoid extra re-allocations by pre-allocating bigger array in an
			 * advance.
			 */
			if(range_end >= prim_type.capacity()) {
				float progress = (float)progress_count/(float)progress_total;
				float factor = (1.0f - progress);
				const size_t reserve = (size_t)(range_end + (float)range_end*factor);
				prim_type.reserve(reserve);
				prim_index.reserve(reserve);
				prim_object.reserve(reserve);
			}

			prim_type.resize(range_end);
			prim_index.resize(range_end);
			prim_object.resize(range_end);
		}
		spatial_spin_lock.unlock();

		/* Perform actual data copy. */
		if(new_leaf_data_size > 0) {
			memcpy(&prim_type[start_index], &local_prim_type[0], new_leaf_data_size);
			memcpy(&prim_index[start_index], &local_prim_index[0], new_leaf_data_size);
			memcpy(&prim_object[start_index], &local_prim_object[0], new_leaf_data_size);
		}
	}
	else {
		/* For the regular BVH builder we simply copy new data starting at the
		 * range start. This is totally thread-safe, all threads are living
		 * inside of their own range.
		 */
		start_index = range.start();
		if(new_leaf_data_size > 0) {
			memcpy(&prim_type[start_index], &local_prim_type[0], new_leaf_data_size);
			memcpy(&prim_index[start_index], &local_prim_index[0], new_leaf_data_size);
			memcpy(&prim_object[start_index], &local_prim_object[0], new_leaf_data_size);
		}
	}

	/* So far leaves were created with the zero-based index in an arrays,
	 * here we modify the indices to correspond to actual packed array start
	 * index.
	 */
	for(int i = 0; i < num_leaves; ++i) {
		LeafNode *leaf = (LeafNode *)leaves[i];
		leaf->m_lo += start_index;
		leaf->m_hi += start_index;
	}

	/* Create leaf node for object. */
	if(num_leaves == 0 || ob_num) {
		/* Only create object leaf nodes if there are objects or no other
		 * nodes created.
		 */
		const BVHReference *ref = (ob_num)? &object_references[0]: NULL;
		leaves[num_leaves] = create_object_leaf_nodes(ref,
		                                              start_index + num_new_leaf_data,
		                                              ob_num);
		++num_leaves;
	}

	if(num_leaves == 1) {
		/* Simplest case: single leaf, just return it.
		 * In all the rest cases we'll be creating intermediate inner node with
		 * an appropriate bounding box.
		 */
		return leaves[0];
	}
	else if(num_leaves == 2) {
		return new InnerNode(range.bounds(), leaves[0], leaves[1]);
	}
	else if(num_leaves == 3) {
		BoundBox inner_bounds = merge(leaves[1]->m_bounds, leaves[2]->m_bounds);
		BVHNode *inner = new InnerNode(inner_bounds, leaves[1], leaves[2]);
		return new InnerNode(range.bounds(), leaves[0], inner);
	} else {
		/* Shpuld be doing more branches if more primitive types added. */
		assert(num_leaves <= 5);
		BoundBox inner_bounds_a = merge(leaves[0]->m_bounds, leaves[1]->m_bounds);
		BoundBox inner_bounds_b = merge(leaves[2]->m_bounds, leaves[3]->m_bounds);
		BVHNode *inner_a = new InnerNode(inner_bounds_a, leaves[0], leaves[1]);
		BVHNode *inner_b = new InnerNode(inner_bounds_b, leaves[2], leaves[3]);
		BoundBox inner_bounds_c = merge(inner_a->m_bounds, inner_b->m_bounds);
		BVHNode *inner_c = new InnerNode(inner_bounds_c, inner_a, inner_b);
		if(num_leaves == 5) {
			return new InnerNode(range.bounds(), inner_c, leaves[4]);
		}
		return inner_c;
	}

#undef MAX_ITEMS_PER_LEAF
}

/* Tree Rotations */

void BVHBuild::rotate(BVHNode *node, int max_depth, int iterations)
{
	/* in tested scenes, this resulted in slightly slower raytracing, so disabled
	 * it for now. could be implementation bug, or depend on the scene */
	if(node)
		for(int i = 0; i < iterations; i++)
			rotate(node, max_depth);
}

void BVHBuild::rotate(BVHNode *node, int max_depth)
{
	/* nothing to rotate if we reached a leaf node. */
	if(node->is_leaf() || max_depth < 0)
		return;
	
	InnerNode *parent = (InnerNode*)node;

	/* rotate all children first */
	for(size_t c = 0; c < 2; c++)
		rotate(parent->children[c], max_depth-1);

	/* compute current area of all children */
	BoundBox bounds0 = parent->children[0]->m_bounds;
	BoundBox bounds1 = parent->children[1]->m_bounds;

	float area0 = bounds0.half_area();
	float area1 = bounds1.half_area();
	float4 child_area = make_float4(area0, area1, 0.0f, 0.0f);

	/* find best rotation. we pick a target child of a first child, and swap
	 * this with an other child. we perform the best such swap. */
	float best_cost = FLT_MAX;
	int best_child = -1, best_target = -1, best_other = -1;

	for(size_t c = 0; c < 2; c++) {
		/* ignore leaf nodes as we cannot descent into */
		if(parent->children[c]->is_leaf())
			continue;

		InnerNode *child = (InnerNode*)parent->children[c];
		BoundBox& other = (c == 0)? bounds1: bounds0;

		/* transpose child bounds */
		BoundBox target0 = child->children[0]->m_bounds;
		BoundBox target1 = child->children[1]->m_bounds;

		/* compute cost for both possible swaps */
		float cost0 = merge(other, target1).half_area() - child_area[c];
		float cost1 = merge(target0, other).half_area() - child_area[c];

		if(min(cost0,cost1) < best_cost) {
			best_child = (int)c;
			best_other = (int)(1-c);

			if(cost0 < cost1) {
				best_cost = cost0;
				best_target = 0;
			}
			else {
				best_cost = cost0;
				best_target = 1;
			}
		}
	}

	/* if we did not find a swap that improves the SAH then do nothing */
	if(best_cost >= 0)
		return;

	assert(best_child == 0 || best_child == 1);
	assert(best_target != -1);

	/* perform the best found tree rotation */
	InnerNode *child = (InnerNode*)parent->children[best_child];

	swap(parent->children[best_other], child->children[best_target]);
	child->m_bounds = merge(child->children[0]->m_bounds, child->children[1]->m_bounds);
}

CCL_NAMESPACE_END
