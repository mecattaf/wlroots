#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include "types/wlr_scene.h"
#include "util/signal.h"
#include "util/time.h"

#define HIGHLIGHT_DAMAGE_FADEOUT_TIME 250

static struct wlr_scene_tree *scene_tree_from_node(struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_TREE);
	return (struct wlr_scene_tree *)node;
}

static struct wlr_scene_rect *scene_rect_from_node(
		struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_RECT);
	return (struct wlr_scene_rect *)node;
}

struct wlr_scene_buffer *wlr_scene_buffer_from_node(
		struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_BUFFER);
	return (struct wlr_scene_buffer *)node;
}

struct wlr_scene *scene_node_get_root(struct wlr_scene_node *node) {
	struct wlr_scene_tree *tree;
	if (node->type == WLR_SCENE_NODE_TREE) {
		tree = scene_tree_from_node(node);
	} else {
		tree = node->parent;
	}

	while (tree->node.parent != NULL) {
		tree = tree->node.parent;
	}
	return (struct wlr_scene *)tree;
}

static void scene_node_init(struct wlr_scene_node *node,
		enum wlr_scene_node_type type, struct wlr_scene_tree *parent) {
	memset(node, 0, sizeof(*node));
	node->type = type;
	node->parent = parent;
	node->enabled = true;

	wl_list_init(&node->link);

	wl_signal_init(&node->events.destroy);

	if (parent != NULL) {
		wl_list_insert(parent->children.prev, &node->link);
	}

	wlr_addon_set_init(&node->addons);
}

static void scene_node_damage_whole(struct wlr_scene_node *node);

struct highlight_region {
	pixman_region32_t region;
	struct timespec when;
	struct wl_list link;
};

static void highlight_region_destroy(struct highlight_region *damage) {
	wl_list_remove(&damage->link);
	pixman_region32_fini(&damage->region);
	free(damage);
}

void wlr_scene_node_destroy(struct wlr_scene_node *node) {
	if (node == NULL) {
		return;
	}

	scene_node_damage_whole(node);

	// We want to call the destroy listeners before we do anything else
	// in case the destroy signal would like to remove children before they
	// are recursively destroyed.
	wlr_signal_emit_safe(&node->events.destroy, NULL);

	struct wlr_scene *scene = scene_node_get_root(node);
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		uint64_t active = scene_buffer->active_outputs;
		if (active) {
			struct wlr_scene_output *scene_output;
			wl_list_for_each(scene_output, &scene->outputs, link) {
				if (active & (1ull << scene_output->index)) {
					wlr_signal_emit_safe(&scene_buffer->events.output_leave,
						scene_output);
				}
			}
		}

		wlr_texture_destroy(scene_buffer->texture);
		wlr_buffer_unlock(scene_buffer->buffer);
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);

		if (scene_tree == &scene->tree) {
			assert(!node->parent);
			struct wlr_scene_output *scene_output, *scene_output_tmp;
			wl_list_for_each_safe(scene_output, scene_output_tmp, &scene->outputs, link) {
				wlr_scene_output_destroy(scene_output);
			}

			struct highlight_region *damage, *tmp_damage;
			wl_list_for_each_safe(damage, tmp_damage, &scene->damage_highlight_regions, link) {
				highlight_region_destroy(damage);
			}

			wl_list_remove(&scene->presentation_destroy.link);
		} else {
			assert(node->parent);
		}

		struct wlr_scene_node *child, *child_tmp;
		wl_list_for_each_safe(child, child_tmp,
				&scene_tree->children, link) {
			wlr_scene_node_destroy(child);
		}
	}

	wlr_addon_set_finish(&node->addons);
	wl_list_remove(&node->link);
	free(node);
}

static void scene_tree_init(struct wlr_scene_tree *tree,
		struct wlr_scene_tree *parent) {
	memset(tree, 0, sizeof(*tree));
	scene_node_init(&tree->node, WLR_SCENE_NODE_TREE, parent);
	wl_list_init(&tree->children);
}

struct wlr_scene *wlr_scene_create(void) {
	struct wlr_scene *scene = calloc(1, sizeof(struct wlr_scene));
	if (scene == NULL) {
		return NULL;
	}

	scene_tree_init(&scene->tree, NULL);

	wl_list_init(&scene->outputs);
	wl_list_init(&scene->presentation_destroy.link);
	wl_list_init(&scene->damage_highlight_regions);

	char *debug_damage = getenv("WLR_SCENE_DEBUG_DAMAGE");
	if (debug_damage) {
		wlr_log(WLR_INFO, "Loading WLR_SCENE_DEBUG_DAMAGE option: %s", debug_damage);
	}

	if (!debug_damage || strcmp(debug_damage, "none") == 0) {
		scene->debug_damage_option = WLR_SCENE_DEBUG_DAMAGE_NONE;
	} else if (strcmp(debug_damage, "rerender") == 0) {
		scene->debug_damage_option = WLR_SCENE_DEBUG_DAMAGE_RERENDER;
	} else if (strcmp(debug_damage, "highlight") == 0) {
		scene->debug_damage_option = WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT;
	} else {
		wlr_log(WLR_ERROR, "Unknown WLR_SCENE_DEBUG_DAMAGE option: %s", debug_damage);
		scene->debug_damage_option = WLR_SCENE_DEBUG_DAMAGE_NONE;
	}

	return scene;
}

struct wlr_scene_tree *wlr_scene_tree_create(struct wlr_scene_tree *parent) {
	assert(parent);

	struct wlr_scene_tree *tree = calloc(1, sizeof(struct wlr_scene_tree));
	if (tree == NULL) {
		return NULL;
	}

	scene_tree_init(tree, parent);
	return tree;
}

static void scene_node_get_size(struct wlr_scene_node *node, int *lx, int *ly);

// This function must be called whenever the coordinates/dimensions of a scene
// buffer or scene output change. It is not necessary to call when a scene
// buffer's node is enabled/disabled or obscured by other nodes.
static void scene_buffer_update_outputs(struct wlr_scene_buffer *scene_buffer,
		int lx, int ly, struct wlr_scene *scene,
		struct wlr_scene_output *ignore) {
	struct wlr_box buffer_box = { .x = lx, .y = ly };
	scene_node_get_size(&scene_buffer->node, &buffer_box.width, &buffer_box.height);

	int largest_overlap = 0;
	scene_buffer->primary_output = NULL;

	uint64_t active_outputs = 0;

	// let's update the outputs in two steps:
	//  - the primary outputs
	//  - the enter/leave signals
	// This ensures that the enter/leave signals can rely on the primary output
	// to have a reasonable value. Otherwise, they may get a value that's in
	// the middle of a calculation.
	struct wlr_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		if (scene_output == ignore) {
			continue;
		}

		struct wlr_box output_box = {
			.x = scene_output->x,
			.y = scene_output->y,
		};
		wlr_output_effective_resolution(scene_output->output,
			&output_box.width, &output_box.height);

		struct wlr_box intersection;
		bool intersects = wlr_box_intersection(&intersection, &buffer_box, &output_box);

		if (intersects) {
			int overlap = intersection.width * intersection.height;
			if (overlap > largest_overlap) {
				largest_overlap = overlap;
				scene_buffer->primary_output = scene_output;
			}

			active_outputs |= 1ull << scene_output->index;
		}
	}

	uint64_t old_active = scene_buffer->active_outputs;
	scene_buffer->active_outputs = active_outputs;

	wl_list_for_each(scene_output, &scene->outputs, link) {
		uint64_t mask = 1ull << scene_output->index;
		bool intersects = active_outputs & mask;
		bool intersects_before = old_active & mask;

		if (intersects && !intersects_before) {
			wlr_signal_emit_safe(&scene_buffer->events.output_enter, scene_output);
		} else if (!intersects && intersects_before) {
			wlr_signal_emit_safe(&scene_buffer->events.output_leave, scene_output);
		}
	}
}

static void _scene_node_update_outputs(struct wlr_scene_node *node,
		int lx, int ly, struct wlr_scene *scene,
		struct wlr_scene_output *ignore) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);
		scene_buffer_update_outputs(scene_buffer, lx, ly, scene, ignore);
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			_scene_node_update_outputs(child, lx + child->x,
				ly + child->y, scene, ignore);
		}
	}
}

static void scene_node_update_outputs(struct wlr_scene_node *node,
		struct wlr_scene_output *ignore) {
	struct wlr_scene *scene = scene_node_get_root(node);
	int lx, ly;
	wlr_scene_node_coords(node, &lx, &ly);
	_scene_node_update_outputs(node, lx, ly, scene, ignore);
}

struct wlr_scene_rect *wlr_scene_rect_create(struct wlr_scene_tree *parent,
		int width, int height, const float color[static 4]) {
	struct wlr_scene_rect *scene_rect =
		calloc(1, sizeof(struct wlr_scene_rect));
	if (scene_rect == NULL) {
		return NULL;
	}
	assert(parent);
	scene_node_init(&scene_rect->node, WLR_SCENE_NODE_RECT, parent);

	scene_rect->width = width;
	scene_rect->height = height;
	memcpy(scene_rect->color, color, sizeof(scene_rect->color));

	scene_node_damage_whole(&scene_rect->node);

	return scene_rect;
}

void wlr_scene_rect_set_size(struct wlr_scene_rect *rect, int width, int height) {
	if (rect->width == width && rect->height == height) {
		return;
	}

	scene_node_damage_whole(&rect->node);
	rect->width = width;
	rect->height = height;
	scene_node_damage_whole(&rect->node);
}

void wlr_scene_rect_set_color(struct wlr_scene_rect *rect, const float color[static 4]) {
	if (memcmp(rect->color, color, sizeof(rect->color)) == 0) {
		return;
	}

	memcpy(rect->color, color, sizeof(rect->color));
	scene_node_damage_whole(&rect->node);
}

struct wlr_scene_buffer *wlr_scene_buffer_create(struct wlr_scene_tree *parent,
		struct wlr_buffer *buffer) {
	struct wlr_scene_buffer *scene_buffer = calloc(1, sizeof(*scene_buffer));
	if (scene_buffer == NULL) {
		return NULL;
	}
	assert(parent);
	scene_node_init(&scene_buffer->node, WLR_SCENE_NODE_BUFFER, parent);

	if (buffer) {
		scene_buffer->buffer = wlr_buffer_lock(buffer);
	}

	wl_signal_init(&scene_buffer->events.output_enter);
	wl_signal_init(&scene_buffer->events.output_leave);
	wl_signal_init(&scene_buffer->events.output_present);
	wl_signal_init(&scene_buffer->events.frame_done);

	scene_node_damage_whole(&scene_buffer->node);

	scene_node_update_outputs(&scene_buffer->node, NULL);

	return scene_buffer;
}

void wlr_scene_buffer_set_buffer_with_damage(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer, pixman_region32_t *damage) {
	// specifying a region for a NULL buffer doesn't make sense. We need to know
	// about the buffer to scale the buffer local coordinates down to scene
	// coordinates. 
	assert(buffer || !damage);

	if (buffer != scene_buffer->buffer) {
		if (!damage) {
			scene_node_damage_whole(&scene_buffer->node);
		}

		wlr_texture_destroy(scene_buffer->texture);
		scene_buffer->texture = NULL;
		wlr_buffer_unlock(scene_buffer->buffer);

		if (buffer) {
			scene_buffer->buffer = wlr_buffer_lock(buffer);
		} else {
			scene_buffer->buffer = NULL;
		}

		scene_node_update_outputs(&scene_buffer->node, NULL);

		if (!damage) {
			scene_node_damage_whole(&scene_buffer->node);
		}
	}

	if (!damage) {
		return;
	}

	int lx, ly;
	if (!wlr_scene_node_coords(&scene_buffer->node, &lx, &ly)) {
		return;
	}

	struct wlr_fbox box = scene_buffer->src_box;
	if (wlr_fbox_empty(&box)) {
		box.x = 0;
		box.y = 0;

		if (scene_buffer->transform & WL_OUTPUT_TRANSFORM_90) {
			box.width = buffer->height;
			box.height = buffer->width;
		} else {
			box.width = buffer->width;
			box.height = buffer->height;
		}
	}

	double scale_x, scale_y;
	if (scene_buffer->dst_width || scene_buffer->dst_height) {
		scale_x = scene_buffer->dst_width / box.width;
		scale_y = scene_buffer->dst_height / box.height;
	} else {
		scale_x = buffer->width / box.width;
		scale_y = buffer->height / box.height;
	}

	pixman_region32_t trans_damage;
	pixman_region32_init(&trans_damage);
	wlr_region_transform(&trans_damage, damage,
		scene_buffer->transform, buffer->width, buffer->height);
	pixman_region32_intersect_rect(&trans_damage, &trans_damage,
		box.x, box.y, box.width, box.height);

	struct wlr_scene *scene = scene_node_get_root(&scene_buffer->node);
	struct wlr_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		float output_scale = scene_output->output->scale;
		pixman_region32_t output_damage;
		pixman_region32_init(&output_damage);
		wlr_region_scale_xy(&output_damage, &trans_damage,
			output_scale * scale_x, output_scale * scale_y);
		pixman_region32_translate(&output_damage,
			(lx - scene_output->x) * output_scale, (ly - scene_output->y) * output_scale);
		wlr_output_damage_add(scene_output->damage, &output_damage);
		pixman_region32_fini(&output_damage);
	}

	pixman_region32_fini(&trans_damage);
}

void wlr_scene_buffer_set_buffer(struct wlr_scene_buffer *scene_buffer,
		struct wlr_buffer *buffer)  {
	wlr_scene_buffer_set_buffer_with_damage(scene_buffer, buffer, NULL);
}

void wlr_scene_buffer_set_source_box(struct wlr_scene_buffer *scene_buffer,
		const struct wlr_fbox *box) {
	struct wlr_fbox *cur = &scene_buffer->src_box;
	if ((wlr_fbox_empty(box) && wlr_fbox_empty(cur)) ||
			(box != NULL && memcmp(cur, box, sizeof(*box)) == 0)) {
		return;
	}

	if (box != NULL) {
		memcpy(cur, box, sizeof(*box));
	} else {
		memset(cur, 0, sizeof(*cur));
	}

	scene_node_damage_whole(&scene_buffer->node);
}

void wlr_scene_buffer_set_dest_size(struct wlr_scene_buffer *scene_buffer,
		int width, int height) {
	if (scene_buffer->dst_width == width && scene_buffer->dst_height == height) {
		return;
	}

	scene_node_damage_whole(&scene_buffer->node);
	scene_buffer->dst_width = width;
	scene_buffer->dst_height = height;
	scene_node_damage_whole(&scene_buffer->node);

	scene_node_update_outputs(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_set_transform(struct wlr_scene_buffer *scene_buffer,
		enum wl_output_transform transform) {
	if (scene_buffer->transform == transform) {
		return;
	}

	scene_node_damage_whole(&scene_buffer->node);
	scene_buffer->transform = transform;
	scene_node_damage_whole(&scene_buffer->node);

	scene_node_update_outputs(&scene_buffer->node, NULL);
}

void wlr_scene_buffer_send_frame_done(struct wlr_scene_buffer *scene_buffer,
		struct timespec *now) {
	wlr_signal_emit_safe(&scene_buffer->events.frame_done, now);
}

static struct wlr_texture *scene_buffer_get_texture(
		struct wlr_scene_buffer *scene_buffer, struct wlr_renderer *renderer) {
	struct wlr_client_buffer *client_buffer =
		wlr_client_buffer_get(scene_buffer->buffer);
	if (client_buffer != NULL) {
		return client_buffer->texture;
	}

	if (scene_buffer->texture != NULL) {
		return scene_buffer->texture;
	}

	scene_buffer->texture =
		wlr_texture_from_buffer(renderer, scene_buffer->buffer);
	return scene_buffer->texture;
}

static void scene_node_get_size(struct wlr_scene_node *node,
		int *width, int *height) {
	*width = 0;
	*height = 0;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		return;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = scene_rect_from_node(node);
		*width = scene_rect->width;
		*height = scene_rect->height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
			*width = scene_buffer->dst_width;
			*height = scene_buffer->dst_height;
		} else if (scene_buffer->buffer) {
			if (scene_buffer->transform & WL_OUTPUT_TRANSFORM_90) {
				*height = scene_buffer->buffer->width;
				*width = scene_buffer->buffer->height;
			} else {
				*width = scene_buffer->buffer->width;
				*height = scene_buffer->buffer->height;
			}
		}
		break;
	}
}

static int scale_length(int length, int offset, float scale) {
	return round((offset + length) * scale) - round(offset * scale);
}

static void scale_box(struct wlr_box *box, float scale) {
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height, box->y, scale);
	box->x = round(box->x * scale);
	box->y = round(box->y * scale);
}

static void _scene_node_damage_whole(struct wlr_scene_node *node,
		struct wlr_scene *scene, int lx, int ly) {
	if (!node->enabled) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			_scene_node_damage_whole(child, scene,
				lx + child->x, ly + child->y);
		}
	}

	int width, height;
	scene_node_get_size(node, &width, &height);

	struct wlr_scene_output *scene_output;
	wl_list_for_each(scene_output, &scene->outputs, link) {
		struct wlr_box box = {
			.x = lx - scene_output->x,
			.y = ly - scene_output->y,
			.width = width,
			.height = height,
		};

		scale_box(&box, scene_output->output->scale);

		wlr_output_damage_add_box(scene_output->damage, &box);
	}
}

static void scene_node_damage_whole(struct wlr_scene_node *node) {
	struct wlr_scene *scene = scene_node_get_root(node);
	if (wl_list_empty(&scene->outputs)) {
		return;
	}

	int lx, ly;
	if (!wlr_scene_node_coords(node, &lx, &ly)) {
		return;
	}

	_scene_node_damage_whole(node, scene, lx, ly);
}

void wlr_scene_node_set_enabled(struct wlr_scene_node *node, bool enabled) {
	if (node->enabled == enabled) {
		return;
	}

	// One of these damage_whole() calls will short-circuit and be a no-op
	scene_node_damage_whole(node);
	node->enabled = enabled;
	scene_node_damage_whole(node);
}

void wlr_scene_node_set_position(struct wlr_scene_node *node, int x, int y) {
	if (node->x == x && node->y == y) {
		return;
	}

	scene_node_damage_whole(node);
	node->x = x;
	node->y = y;
	scene_node_damage_whole(node);

	scene_node_update_outputs(node, NULL);
}

void wlr_scene_node_place_above(struct wlr_scene_node *node,
		struct wlr_scene_node *sibling) {
	assert(node != sibling);
	assert(node->parent == sibling->parent);

	if (node->link.prev == &sibling->link) {
		return;
	}

	wl_list_remove(&node->link);
	wl_list_insert(&sibling->link, &node->link);

	scene_node_damage_whole(node);
	scene_node_damage_whole(sibling);
}

void wlr_scene_node_place_below(struct wlr_scene_node *node,
		struct wlr_scene_node *sibling) {
	assert(node != sibling);
	assert(node->parent == sibling->parent);

	if (node->link.next == &sibling->link) {
		return;
	}

	wl_list_remove(&node->link);
	wl_list_insert(sibling->link.prev, &node->link);

	scene_node_damage_whole(node);
	scene_node_damage_whole(sibling);
}

void wlr_scene_node_raise_to_top(struct wlr_scene_node *node) {
	struct wlr_scene_node *current_top = wl_container_of(
		node->parent->children.prev, current_top, link);
	if (node == current_top) {
		return;
	}
	wlr_scene_node_place_above(node, current_top);
}

void wlr_scene_node_lower_to_bottom(struct wlr_scene_node *node) {
	struct wlr_scene_node *current_bottom = wl_container_of(
		node->parent->children.next, current_bottom, link);
	if (node == current_bottom) {
		return;
	}
	wlr_scene_node_place_below(node, current_bottom);
}

void wlr_scene_node_reparent(struct wlr_scene_node *node,
		struct wlr_scene_tree *new_parent) {
	assert(new_parent != NULL);

	if (node->parent == new_parent) {
		return;
	}

	/* Ensure that a node cannot become its own ancestor */
	for (struct wlr_scene_tree *ancestor = new_parent; ancestor != NULL;
			ancestor = ancestor->node.parent) {
		assert(&ancestor->node != node);
	}

	scene_node_damage_whole(node);

	wl_list_remove(&node->link);
	node->parent = new_parent;
	wl_list_insert(new_parent->children.prev, &node->link);

	scene_node_damage_whole(node);

	scene_node_update_outputs(node, NULL);
}

bool wlr_scene_node_coords(struct wlr_scene_node *node,
		int *lx_ptr, int *ly_ptr) {
	assert(node);

	int lx = 0, ly = 0;
	bool enabled = true;
	while (true) {
		lx += node->x;
		ly += node->y;
		enabled = enabled && node->enabled;
		if (node->parent == NULL) {
			break;
		}

		node = &node->parent->node;
	}

	*lx_ptr = lx;
	*ly_ptr = ly;
	return enabled;
}

static void scene_node_for_each_scene_buffer(struct wlr_scene_node *node,
		int lx, int ly, wlr_scene_buffer_iterator_func_t user_iterator,
		void *user_data) {
	if (!node->enabled) {
		return;
	}

	lx += node->x;
	ly += node->y;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		user_iterator(scene_buffer, lx, ly, user_data);
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_for_each_scene_buffer(child, lx, ly, user_iterator, user_data);
		}
	}
}

void wlr_scene_node_for_each_buffer(struct wlr_scene_node *node,
		wlr_scene_buffer_iterator_func_t user_iterator, void *user_data) {
	scene_node_for_each_scene_buffer(node, 0, 0, user_iterator, user_data);
}

struct wlr_scene_node *wlr_scene_node_at(struct wlr_scene_node *node,
		double lx, double ly, double *nx, double *ny) {
	if (!node->enabled) {
		return NULL;
	}

	// TODO: optimize by storing a bounding box in each node?
	lx -= node->x;
	ly -= node->y;

	bool intersects = false;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE:;
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each_reverse(child, &scene_tree->children, link) {
			struct wlr_scene_node *node =
				wlr_scene_node_at(child, lx, ly, nx, ny);
			if (node != NULL) {
				return node;
			}
		}
		break;
	case WLR_SCENE_NODE_RECT:;
		int width, height;
		scene_node_get_size(node, &width, &height);
		intersects = lx >= 0 && lx < width && ly >= 0 && ly < height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

		if (scene_buffer->point_accepts_input) {
			intersects = scene_buffer->point_accepts_input(scene_buffer, lx, ly);
		} else {
			int width, height;
			scene_node_get_size(node, &width, &height);
			intersects = lx >= 0 && lx < width && ly >= 0 && ly < height;
		}
		break;
	}

	if (intersects) {
		if (nx != NULL) {
			*nx = lx;
		}
		if (ny != NULL) {
			*ny = ly;
		}
		return node;
	}

	return NULL;
}

static void scissor_output(struct wlr_output *output, pixman_box32_t *rect) {
	struct wlr_renderer *renderer = output->renderer;
	assert(renderer);

	struct wlr_box box = {
		.x = rect->x1,
		.y = rect->y1,
		.width = rect->x2 - rect->x1,
		.height = rect->y2 - rect->y1,
	};

	int ow, oh;
	wlr_output_transformed_resolution(output, &ow, &oh);

	enum wl_output_transform transform =
		wlr_output_transform_invert(output->transform);
	wlr_box_transform(&box, &box, transform, ow, oh);

	wlr_renderer_scissor(renderer, &box);
}

static void render_rect(struct wlr_output *output,
		pixman_region32_t *output_damage, const float color[static 4],
		const struct wlr_box *box, const float matrix[static 9]) {
	struct wlr_renderer *renderer = output->renderer;
	assert(renderer);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_init_rect(&damage, box->x, box->y, box->width, box->height);
	pixman_region32_intersect(&damage, &damage, output_damage);

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&damage, &nrects);
	for (int i = 0; i < nrects; ++i) {
		scissor_output(output, &rects[i]);
		wlr_render_rect(renderer, box, color, matrix);
	}

	pixman_region32_fini(&damage);
}

static void render_texture(struct wlr_output *output,
		pixman_region32_t *output_damage, struct wlr_texture *texture,
		const struct wlr_fbox *src_box, const struct wlr_box *dst_box,
		const float matrix[static 9]) {
	struct wlr_renderer *renderer = output->renderer;
	assert(renderer);

	struct wlr_fbox default_src_box = {0};
	if (wlr_fbox_empty(src_box)) {
		default_src_box.width = texture->width;
		default_src_box.height = texture->height;
		src_box = &default_src_box;
	}

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_init_rect(&damage, dst_box->x, dst_box->y,
		dst_box->width, dst_box->height);
	pixman_region32_intersect(&damage, &damage, output_damage);

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&damage, &nrects);
	for (int i = 0; i < nrects; ++i) {
		scissor_output(output, &rects[i]);
		wlr_render_subtexture_with_matrix(renderer, texture, src_box, matrix, 1.0);
	}

	pixman_region32_fini(&damage);
}

struct render_data {
	struct wlr_scene_output *scene_output;
	pixman_region32_t *damage;
};

static void render_node_iterator(struct wlr_scene_node *node,
		int x, int y, void *_data) {
	struct render_data *data = _data;
	struct wlr_scene_output *scene_output = data->scene_output;
	struct wlr_output *output = scene_output->output;
	pixman_region32_t *output_damage = data->damage;

	struct wlr_box dst_box = {
		.x = x,
		.y = y,
	};
	scene_node_get_size(node, &dst_box.width, &dst_box.height);
	scale_box(&dst_box, output->scale);

	struct wlr_texture *texture;
	float matrix[9];
	enum wl_output_transform transform;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		/* Root or tree node has nothing to render itself */
		break;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = scene_rect_from_node(node);

		render_rect(output, output_damage, scene_rect->color, &dst_box,
			output->transform_matrix);
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (!scene_buffer->buffer) {
			return;
		}

		struct wlr_renderer *renderer = output->renderer;
		texture = scene_buffer_get_texture(scene_buffer, renderer);
		if (texture == NULL) {
			return;
		}

		transform = wlr_output_transform_invert(scene_buffer->transform);
		wlr_matrix_project_box(matrix, &dst_box, transform, 0.0,
			output->transform_matrix);

		render_texture(output, output_damage, texture, &scene_buffer->src_box,
			&dst_box, matrix);

		wlr_signal_emit_safe(&scene_buffer->events.output_present, scene_output);
		break;
	}
}

static void scene_node_for_each_node(struct wlr_scene_node *node,
		int lx, int ly, wlr_scene_node_iterator_func_t user_iterator,
		void *user_data) {
	if (!node->enabled) {
		return;
	}

	lx += node->x;
	ly += node->y;

	user_iterator(node, lx, ly, user_data);

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_for_each_node(child, lx, ly, user_iterator, user_data);
		}
	}
}

static void scene_handle_presentation_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_scene *scene =
		wl_container_of(listener, scene, presentation_destroy);
	wl_list_remove(&scene->presentation_destroy.link);
	wl_list_init(&scene->presentation_destroy.link);
	scene->presentation = NULL;
}

void wlr_scene_set_presentation(struct wlr_scene *scene,
		struct wlr_presentation *presentation) {
	assert(scene->presentation == NULL);
	scene->presentation = presentation;
	scene->presentation_destroy.notify = scene_handle_presentation_destroy;
	wl_signal_add(&presentation->events.destroy, &scene->presentation_destroy);
}

static void scene_output_handle_destroy(struct wlr_addon *addon) {
	struct wlr_scene_output *scene_output =
		wl_container_of(addon, scene_output, addon);
	wlr_scene_output_destroy(scene_output);
}

static const struct wlr_addon_interface output_addon_impl = {
	.name = "wlr_scene_output",
	.destroy = scene_output_handle_destroy,
};

static void scene_output_handle_commit(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_commit);
	struct wlr_output_event_commit *event = data;

	if (event->committed & (WLR_OUTPUT_STATE_MODE |
			WLR_OUTPUT_STATE_TRANSFORM |
			WLR_OUTPUT_STATE_SCALE)) {
		scene_node_update_outputs(&scene_output->scene->tree.node, NULL);
	}
}

static void scene_output_handle_mode(struct wl_listener *listener, void *data) {
	struct wlr_scene_output *scene_output = wl_container_of(listener,
		scene_output, output_mode);
	scene_node_update_outputs(&scene_output->scene->tree.node, NULL);
}

struct wlr_scene_output *wlr_scene_output_create(struct wlr_scene *scene,
		struct wlr_output *output) {
	struct wlr_scene_output *scene_output = calloc(1, sizeof(*scene_output));
	if (scene_output == NULL) {
		return NULL;
	}

	scene_output->damage = wlr_output_damage_create(output);
	if (scene_output->damage == NULL) {
		free(scene_output);
		return NULL;
	}

	scene_output->output = output;
	scene_output->scene = scene;
	wlr_addon_init(&scene_output->addon, &output->addons, scene, &output_addon_impl);

	int prev_output_index = -1;
	struct wl_list *prev_output_link = &scene->outputs;

	struct wlr_scene_output *current_output;
	wl_list_for_each(current_output, &scene->outputs, link) {
		if (prev_output_index + 1 != current_output->index) {
			break;
		}

		prev_output_index = current_output->index;
		prev_output_link = &current_output->link;
	}

	scene_output->index = prev_output_index + 1;
	assert(scene_output->index < 64);
	wl_list_insert(prev_output_link, &scene_output->link);

	wl_signal_init(&scene_output->events.destroy);

	scene_output->output_commit.notify = scene_output_handle_commit;
	wl_signal_add(&output->events.commit, &scene_output->output_commit);

	scene_output->output_mode.notify = scene_output_handle_mode;
	wl_signal_add(&output->events.mode, &scene_output->output_mode);

	wlr_output_damage_add_whole(scene_output->damage);
	scene_node_update_outputs(&scene->tree.node, NULL);

	return scene_output;
}

void wlr_scene_output_destroy(struct wlr_scene_output *scene_output) {
	if (scene_output == NULL) {
		return;
	}

	wlr_signal_emit_safe(&scene_output->events.destroy, NULL);

	scene_node_update_outputs(&scene_output->scene->tree.node, scene_output);

	wlr_addon_finish(&scene_output->addon);
	wl_list_remove(&scene_output->link);
	wl_list_remove(&scene_output->output_commit.link);
	wl_list_remove(&scene_output->output_mode.link);

	free(scene_output);
}

struct wlr_scene_output *wlr_scene_get_scene_output(struct wlr_scene *scene,
		struct wlr_output *output) {
	struct wlr_addon *addon =
		wlr_addon_find(&output->addons, scene, &output_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct wlr_scene_output *scene_output =
		wl_container_of(addon, scene_output, addon);
	return scene_output;
}

void wlr_scene_output_set_position(struct wlr_scene_output *scene_output,
		int lx, int ly) {
	if (scene_output->x == lx && scene_output->y == ly) {
		return;
	}

	scene_output->x = lx;
	scene_output->y = ly;
	wlr_output_damage_add_whole(scene_output->damage);

	scene_node_update_outputs(&scene_output->scene->tree.node, NULL);
}

struct check_scanout_data {
	// in
	struct wlr_box viewport_box;
	// out
	struct wlr_scene_node *node;
	size_t n;
};

static void check_scanout_iterator(struct wlr_scene_node *node,
		int x, int y, void *_data) {
	struct check_scanout_data *data = _data;

	struct wlr_box node_box = { .x = x, .y = y };
	scene_node_get_size(node, &node_box.width, &node_box.height);

	struct wlr_box intersection;
	if (!wlr_box_intersection(&intersection, &data->viewport_box, &node_box)) {
		return;
	}

	data->n++;

	if (data->viewport_box.x == node_box.x &&
			data->viewport_box.y == node_box.y &&
			data->viewport_box.width == node_box.width &&
			data->viewport_box.height == node_box.height) {
		data->node = node;
	}
}

static bool scene_output_scanout(struct wlr_scene_output *scene_output) {
	if (scene_output->scene->debug_damage_option ==
			WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		// We don't want to enter direct scan out if we have highlight regions
		// enabled. Otherwise, we won't be able to render the damage regions.
		return false;
	}

	struct wlr_output *output = scene_output->output;

	struct wlr_box viewport_box = { .x = scene_output->x, .y = scene_output->y };
	wlr_output_effective_resolution(output,
		&viewport_box.width, &viewport_box.height);

	struct check_scanout_data check_scanout_data = {
		.viewport_box = viewport_box,
	};
	scene_node_for_each_node(&scene_output->scene->tree.node, 0, 0,
		check_scanout_iterator, &check_scanout_data);
	if (check_scanout_data.n != 1 || check_scanout_data.node == NULL) {
		return false;
	}

	struct wlr_scene_node *node = check_scanout_data.node;
	struct wlr_buffer *buffer;
	switch (node->type) {
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (scene_buffer->buffer == NULL ||
				!wlr_fbox_empty(&scene_buffer->src_box) ||
				scene_buffer->transform != output->transform) {
			return false;
		}
		buffer = scene_buffer->buffer;
		break;
	default:
		return false;
	}

	wlr_output_attach_buffer(output, buffer);
	if (!wlr_output_test(output)) {
		wlr_output_rollback(output);
		return false;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);
		wlr_signal_emit_safe(&scene_buffer->events.output_present, scene_output);
	}

	return wlr_output_commit(output);
}

bool wlr_scene_output_commit(struct wlr_scene_output *scene_output) {
	struct wlr_output *output = scene_output->output;
	enum wlr_scene_debug_damage_option debug_damage =
		scene_output->scene->debug_damage_option;

	struct wlr_renderer *renderer = output->renderer;
	assert(renderer != NULL);

	bool scanout = scene_output_scanout(scene_output);
	if (scanout != scene_output->prev_scanout) {
		wlr_log(WLR_DEBUG, "Direct scan-out %s",
			scanout ? "enabled" : "disabled");
		// When exiting direct scan-out, damage everything
		wlr_output_damage_add_whole(scene_output->damage);
	}
	scene_output->prev_scanout = scanout;
	if (scanout) {
		return true;
	}

	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_RERENDER) {
		wlr_output_damage_add_whole(scene_output->damage);
	}

	struct timespec now;
	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		struct wl_list *regions = &scene_output->scene->damage_highlight_regions;
		clock_gettime(CLOCK_MONOTONIC, &now);

		// add the current frame's damage if there is damage
		if (pixman_region32_not_empty(&scene_output->damage->current)) {
			struct highlight_region *current_damage =
				calloc(1, sizeof(*current_damage));
			if (current_damage) {
				pixman_region32_init(&current_damage->region);
				pixman_region32_copy(&current_damage->region,
					&scene_output->damage->current);
				memcpy(&current_damage->when, &now, sizeof(now));
				wl_list_insert(regions, &current_damage->link);
			}
		}

		pixman_region32_t acc_damage;
		pixman_region32_init(&acc_damage);
		struct highlight_region *damage, *tmp_damage;
		wl_list_for_each_safe(damage, tmp_damage, regions, link) {
			// remove overlaping damage regions
			pixman_region32_subtract(&damage->region, &damage->region, &acc_damage);
			pixman_region32_union(&acc_damage, &acc_damage, &damage->region);

			// if this damage is too old or has nothing in it, get rid of it
			struct timespec time_diff;
			timespec_sub(&time_diff, &now, &damage->when);
			if (timespec_to_msec(&time_diff) >= HIGHLIGHT_DAMAGE_FADEOUT_TIME ||
					!pixman_region32_not_empty(&damage->region)) {
				highlight_region_destroy(damage);
			}
		}

		wlr_output_damage_add(scene_output->damage, &acc_damage);
		pixman_region32_fini(&acc_damage);
	}

	bool needs_frame;
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	if (!wlr_output_damage_attach_render(scene_output->damage,
			&needs_frame, &damage)) {
		pixman_region32_fini(&damage);
		return false;
	}

	if (!needs_frame) {
		pixman_region32_fini(&damage);
		wlr_output_rollback(output);
		return true;
	}

	wlr_renderer_begin(renderer, output->width, output->height);

	int nrects;
	pixman_box32_t *rects = pixman_region32_rectangles(&damage, &nrects);
	for (int i = 0; i < nrects; ++i) {
		scissor_output(output, &rects[i]);
		wlr_renderer_clear(renderer, (float[4]){ 0.0, 0.0, 0.0, 1.0 });
	}

	struct render_data data = {
		.scene_output = scene_output,
		.damage = &damage,
	};
	scene_node_for_each_node(&scene_output->scene->tree.node,
		-scene_output->x, -scene_output->y,
		render_node_iterator, &data);
	wlr_renderer_scissor(renderer, NULL);

	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		struct highlight_region *damage;
		wl_list_for_each(damage, &scene_output->scene->damage_highlight_regions, link) {
			struct timespec time_diff;
			timespec_sub(&time_diff, &now, &damage->when);
			int64_t time_diff_ms = timespec_to_msec(&time_diff);
			float alpha = 1.0 - (double)time_diff_ms / HIGHLIGHT_DAMAGE_FADEOUT_TIME;

			int nrects;
			pixman_box32_t *rects = pixman_region32_rectangles(&damage->region, &nrects);
			for (int i = 0; i < nrects; ++i) {
				struct wlr_box box = {
					.x = rects[i].x1,
					.y = rects[i].y1,
					.width = rects[i].x2 - rects[i].x1,
					.height = rects[i].y2 - rects[i].y1,
				};

				float color[4] = { alpha * .5, 0.0, 0.0, alpha * .5 };
				wlr_render_rect(renderer, &box, color, output->transform_matrix);
			}
		}
	}

	wlr_output_render_software_cursors(output, &damage);

	wlr_renderer_end(renderer);
	pixman_region32_fini(&damage);

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

	enum wl_output_transform transform =
		wlr_output_transform_invert(output->transform);

	pixman_region32_t frame_damage;
	pixman_region32_init(&frame_damage);
	wlr_region_transform(&frame_damage, &scene_output->damage->current,
		transform, tr_width, tr_height);
	wlr_output_set_damage(output, &frame_damage);
	pixman_region32_fini(&frame_damage);

	bool success = wlr_output_commit(output);

	if (debug_damage == WLR_SCENE_DEBUG_DAMAGE_HIGHLIGHT &&
			!wl_list_empty(&scene_output->scene->damage_highlight_regions)) {
		wlr_output_schedule_frame(scene_output->output);
	}

	return success;
}

static void scene_node_send_frame_done(struct wlr_scene_node *node,
		struct wlr_scene_output *scene_output, struct timespec *now) {
	if (!node->enabled) {
		return;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);

		if (scene_buffer->primary_output == scene_output) {
			wlr_scene_buffer_send_frame_done(scene_buffer, now);
		}
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_send_frame_done(child, scene_output, now);
		}
	}
}

void wlr_scene_output_send_frame_done(struct wlr_scene_output *scene_output,
		struct timespec *now) {
	scene_node_send_frame_done(&scene_output->scene->tree.node,
		scene_output, now);
}

static void scene_output_for_each_scene_buffer(const struct wlr_box *output_box,
		struct wlr_scene_node *node, int lx, int ly,
		wlr_scene_buffer_iterator_func_t user_iterator, void *user_data) {
	if (!node->enabled) {
		return;
	}

	lx += node->x;
	ly += node->y;

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_box node_box = { .x = lx, .y = ly };
		scene_node_get_size(node, &node_box.width, &node_box.height);

		struct wlr_box intersection;
		if (wlr_box_intersection(&intersection, output_box, &node_box)) {
			struct wlr_scene_buffer *scene_buffer =
				wlr_scene_buffer_from_node(node);
			user_iterator(scene_buffer, lx, ly, user_data);
		}
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *scene_tree = scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_output_for_each_scene_buffer(output_box, child, lx, ly,
				user_iterator, user_data);
		}
	}
}

void wlr_scene_output_for_each_buffer(struct wlr_scene_output *scene_output,
		wlr_scene_buffer_iterator_func_t iterator, void *user_data) {
	struct wlr_box box = { .x = scene_output->x, .y = scene_output->y };
	wlr_output_effective_resolution(scene_output->output,
		&box.width, &box.height);
	scene_output_for_each_scene_buffer(&box, &scene_output->scene->tree.node, 0, 0,
		iterator, user_data);
}
