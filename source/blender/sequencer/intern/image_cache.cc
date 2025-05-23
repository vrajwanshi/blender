/* SPDX-FileCopyrightText: 2010 Peter Schlaile <peter [at] schlaile [dot] de>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstddef>
#include <ctime>
#include <memory.h>

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_userdef_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_ghash.h"
#include "BLI_math_base.h"
#include "BLI_mempool.h"
#include "BLI_threads.h"

#include "BKE_main.hh"

#include "SEQ_prefetch.hh"
#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_time.hh"

#include "disk_cache.hh"
#include "image_cache.hh"
#include "prefetch.hh"

/**
 * Sequencer Cache Design Notes
 * ============================
 *
 * Function:
 * All images created during rendering are added to cache, even if the cache is already full.
 * This is because:
 * - One image may be needed multiple times during rendering.
 * - Keeping the last rendered frame allows us for faster re-render when user edits strip in stack.
 * - We can decide if we keep frame only when it's completely rendered. Otherwise we risk having
 *   "holes" in the cache, which can be annoying.
 *
 * If the cache is full all entries for pending frame will have is_temp_cache set.
 *
 * Linking: We use links to reduce number of iterations over entries needed to manage cache.
 * Entries are linked in order as they are put into cache.
 * Only permanent (is_temp_cache = 0) cache entries are linked.
 * Putting #SEQ_CACHE_STORE_FINAL_OUT will reset linking
 *
 * Only entire frame can be freed to release resources for new entries (recycling).
 * Once again, this is to reduce number of iterations, but also more controllable than removing
 * entries one by one in reverse order to their creation.
 *
 * User can exclude caching of some images. Such entries will have is_temp_cache set.
 */

namespace blender::seq {

struct SeqCache {
  Main *bmain;
  GHash *hash;
  ThreadMutex iterator_mutex;
  BLI_mempool *keys_pool;
  BLI_mempool *items_pool;
  SeqCacheKey *last_key;
  SeqDiskCache *disk_cache;
};

struct SeqCacheItem {
  SeqCache *cache_owner;
  ImBuf *ibuf;
};

static ThreadMutex cache_create_lock = BLI_MUTEX_INITIALIZER;

static bool seq_cmp_render_data(const RenderData *a, const RenderData *b)
{
  return ((a->preview_render_size != b->preview_render_size) || (a->rectx != b->rectx) ||
          (a->recty != b->recty) || (a->bmain != b->bmain) || (a->scene != b->scene) ||
          (a->motion_blur_shutter != b->motion_blur_shutter) ||
          (a->motion_blur_samples != b->motion_blur_samples) ||
          (a->scene->r.views_format != b->scene->r.views_format) || (a->view_id != b->view_id));
}

static uint seq_hash_render_data(const RenderData *a)
{
  uint rval = a->rectx + a->recty;

  rval ^= a->preview_render_size;
  rval ^= intptr_t(a->bmain) << 6;
  rval ^= intptr_t(a->scene) << 6;
  rval ^= int(a->motion_blur_shutter * 100.0f) << 10;
  rval ^= a->motion_blur_samples << 16;
  rval ^= ((a->scene->r.views_format * 2) + a->view_id) << 24;

  return rval;
}

static uint seq_cache_hashhash(const void *key_)
{
  const SeqCacheKey *key = static_cast<const SeqCacheKey *>(key_);
  uint rval = seq_hash_render_data(&key->context);

  rval ^= *(const uint *)&key->frame_index;
  rval += key->type;
  rval ^= intptr_t(key->strip) << 6;

  return rval;
}

static bool seq_cache_hashcmp(const void *a_, const void *b_)
{
  const SeqCacheKey *a = static_cast<const SeqCacheKey *>(a_);
  const SeqCacheKey *b = static_cast<const SeqCacheKey *>(b_);

  return ((a->strip != b->strip) || (a->frame_index != b->frame_index) || (a->type != b->type) ||
          seq_cmp_render_data(&a->context, &b->context));
}

static float seq_cache_timeline_frame_to_frame_index(const Scene *scene,
                                                     const Strip *strip,
                                                     const float timeline_frame,
                                                     const int type)
{
  /* With raw images, map timeline_frame to strip input media frame range. This means that static
   * images or extended frame range of movies will only generate one cache entry. No special
   * treatment in converting frame index to timeline_frame is needed. */
  bool is_effect = strip->type & STRIP_TYPE_EFFECT;
  if (!is_effect && type == SEQ_CACHE_STORE_RAW) {
    return give_frame_index(scene, strip, timeline_frame);
  }

  return timeline_frame - time_start_frame_get(strip);
}

static int seq_cache_key_timeline_frame_get(const SeqCacheKey *key)
{
  return key->frame_index + time_start_frame_get(key->strip);
}

static SeqCache *seq_cache_get_from_scene(Scene *scene)
{
  if (scene && scene->ed && scene->ed->cache) {
    return scene->ed->cache;
  }

  return nullptr;
}

static void seq_cache_lock(Scene *scene)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);

  if (cache) {
    BLI_mutex_lock(&cache->iterator_mutex);
  }
}

static void seq_cache_unlock(Scene *scene)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);

  if (cache) {
    BLI_mutex_unlock(&cache->iterator_mutex);
  }
}

static size_t seq_cache_get_mem_total()
{
  return size_t(U.memcachelimit) * 1024 * 1024;
}

static void seq_cache_keyfree(void *val)
{
  SeqCacheKey *key = static_cast<SeqCacheKey *>(val);
  BLI_mempool_free(key->cache_owner->keys_pool, key);
}

static void seq_cache_valfree(void *val)
{
  SeqCacheItem *item = (SeqCacheItem *)val;

  if (item->ibuf) {
    IMB_freeImBuf(item->ibuf);
  }

  BLI_mempool_free(item->cache_owner->items_pool, item);
}

static int get_stored_types_flag(Scene *scene, SeqCacheKey *key)
{
  int flag;
  if (key->strip->cache_flag & SEQ_CACHE_OVERRIDE) {
    flag = key->strip->cache_flag;
  }
  else {
    flag = scene->ed->cache_flag;
  }

  /* SEQ_CACHE_STORE_FINAL_OUT can not be overridden by strip cache */
  flag |= (scene->ed->cache_flag & SEQ_CACHE_STORE_FINAL_OUT);

  return flag;
}

static void seq_cache_put_ex(Scene *scene, SeqCacheKey *key, ImBuf *ibuf)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  SeqCacheItem *item;
  item = static_cast<SeqCacheItem *>(BLI_mempool_alloc(cache->items_pool));
  item->cache_owner = cache;
  item->ibuf = ibuf;

  const int stored_types_flag = get_stored_types_flag(scene, key);

  /* Item stored for later use. */
  if (stored_types_flag & key->type) {
    key->is_temp_cache = false;
    key->link_prev = cache->last_key;
  }

  BLI_assert(!BLI_ghash_haskey(cache->hash, key));
  BLI_ghash_insert(cache->hash, key, item);
  IMB_refImBuf(ibuf);

  /* Store pointer to last cached key. */
  SeqCacheKey *temp_last_key = cache->last_key;
  cache->last_key = key;

  /* Set last_key's reference to this key so we can look up chain backwards.
   * Item is already put in cache, so cache->last_key points to current key.
   */
  if (!key->is_temp_cache && temp_last_key) {
    temp_last_key->link_next = cache->last_key;
  }

  /* Reset linking. */
  if (key->type == SEQ_CACHE_STORE_FINAL_OUT) {
    cache->last_key = nullptr;
  }
}

static ImBuf *seq_cache_get_ex(SeqCache *cache, SeqCacheKey *key)
{
  SeqCacheItem *item = static_cast<SeqCacheItem *>(BLI_ghash_lookup(cache->hash, key));

  if (item && item->ibuf) {
    IMB_refImBuf(item->ibuf);

    return item->ibuf;
  }

  return nullptr;
}

static void seq_cache_key_unlink(SeqCacheKey *key)
{
  if (key->link_next) {
    BLI_assert(key == key->link_next->link_prev);
    key->link_next->link_prev = key->link_prev;
  }
  if (key->link_prev) {
    BLI_assert(key == key->link_prev->link_next);
    key->link_prev->link_next = key->link_next;
  }
}

/* Choose a key out of 2 candidates(leftmost and rightmost items)
 * to recycle based on currently used strategy */
static SeqCacheKey *seq_cache_choose_key(Scene *scene, SeqCacheKey *lkey, SeqCacheKey *rkey)
{
  SeqCacheKey *finalkey = nullptr;
  const int lkey_tml_frame = seq_cache_key_timeline_frame_get(lkey);
  const int rkey_tml_frame = seq_cache_key_timeline_frame_get(rkey);

  /* Ideally, cache would not need to check the state of prefetching task
   * that is tricky to do however, because prefetch would need to know,
   * if a key, that is about to be created would be removed by itself.
   *
   * This can happen because only FINAL_OUT item insertion will trigger recycling
   * but that is also the point, where prefetch can be suspended.
   *
   * We could use temp cache as a shield and later make it a non-temporary entry,
   * but it is not worth of increasing system complexity.
   */
  if (scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE && seq_prefetch_job_is_running(scene)) {
    int pfjob_start, pfjob_end;
    seq_prefetch_get_time_range(scene, &pfjob_start, &pfjob_end);

    if (lkey) {
      if (lkey_tml_frame < pfjob_start || lkey_tml_frame > pfjob_end) {
        return lkey;
      }
    }

    if (rkey) {
      if (rkey_tml_frame < pfjob_start || rkey_tml_frame > pfjob_end) {
        return rkey;
      }
    }

    return nullptr;
  }

  if (rkey && lkey) {
    if (lkey_tml_frame > rkey_tml_frame) {
      SeqCacheKey *swapkey = lkey;
      lkey = rkey;
      rkey = swapkey;
    }

    int l_diff = scene->r.cfra - lkey_tml_frame;
    int r_diff = rkey_tml_frame - scene->r.cfra;

    if (l_diff > r_diff) {
      finalkey = lkey;
    }
    else {
      finalkey = rkey;
    }
  }
  else {
    if (lkey) {
      finalkey = lkey;
    }
    else {
      finalkey = rkey;
    }
  }
  return finalkey;
}

static void seq_cache_recycle_linked(Scene *scene, SeqCacheKey *base)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return;
  }

  SeqCacheKey *next = base->link_next;

  while (base) {
    if (!BLI_ghash_haskey(cache->hash, base)) {
      break; /* Key has already been removed from cache. */
    }

    SeqCacheKey *prev = base->link_prev;
    if (prev != nullptr && prev->link_next != base) {
      /* Key has been removed and replaced and doesn't belong to this chain anymore. */
      base->link_prev = nullptr;
      break;
    }

    seq_cache_key_unlink(base);
    BLI_ghash_remove(cache->hash, base, seq_cache_keyfree, seq_cache_valfree);
    BLI_assert(base != cache->last_key);
    base = prev;
  }

  base = next;
  while (base) {
    if (!BLI_ghash_haskey(cache->hash, base)) {
      break; /* Key has already been removed from cache. */
    }

    next = base->link_next;
    if (next != nullptr && next->link_prev != base) {
      /* Key has been removed and replaced and doesn't belong to this chain anymore. */
      base->link_next = nullptr;
      break;
    }

    seq_cache_key_unlink(base);
    BLI_ghash_remove(cache->hash, base, seq_cache_keyfree, seq_cache_valfree);
    BLI_assert(base != cache->last_key);
    base = next;
  }
}

static SeqCacheKey *seq_cache_get_item_for_removal(Scene *scene)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  SeqCacheKey *finalkey = nullptr;
  /* Leftmost key. */
  SeqCacheKey *lkey = nullptr;
  /* Rightmost key. */
  SeqCacheKey *rkey = nullptr;
  SeqCacheKey *key = nullptr;

  GHashIterator gh_iter;
  BLI_ghashIterator_init(&gh_iter, cache->hash);
  int total_count = 0;

  while (!BLI_ghashIterator_done(&gh_iter)) {
    key = static_cast<SeqCacheKey *>(BLI_ghashIterator_getKey(&gh_iter));
    SeqCacheItem *item = static_cast<SeqCacheItem *>(BLI_ghashIterator_getValue(&gh_iter));
    BLI_ghashIterator_step(&gh_iter);
    BLI_assert(key->cache_owner == cache);

    /* This shouldn't happen, but better be safe than sorry. */
    if (!item->ibuf) {
      seq_cache_recycle_linked(scene, key);
      /* Can not continue iterating after linked remove. */
      BLI_ghashIterator_init(&gh_iter, cache->hash);
      continue;
    }

    if (key->is_temp_cache || key->link_next != nullptr) {
      continue;
    }

    total_count++;

    if (lkey) {
      if (seq_cache_key_timeline_frame_get(key) < seq_cache_key_timeline_frame_get(lkey)) {
        lkey = key;
      }
    }
    else {
      lkey = key;
    }
    if (rkey) {
      if (seq_cache_key_timeline_frame_get(key) > seq_cache_key_timeline_frame_get(rkey)) {
        rkey = key;
      }
    }
    else {
      rkey = key;
    }
  }
  (void)total_count; /* Quiet set-but-unused warning (may be removed). */

  finalkey = seq_cache_choose_key(scene, lkey, rkey);

  return finalkey;
}

bool seq_cache_recycle_item(Scene *scene)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return false;
  }

  seq_cache_lock(scene);

  while (seq_cache_is_full()) {
    SeqCacheKey *finalkey = seq_cache_get_item_for_removal(scene);

    if (finalkey) {
      seq_cache_recycle_linked(scene, finalkey);
    }
    else {
      seq_cache_unlock(scene);
      return false;
    }
  }
  seq_cache_unlock(scene);
  return true;
}

static void seq_cache_set_temp_cache_linked(Scene *scene, SeqCacheKey *base)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);

  if (!cache || !base) {
    return;
  }

  SeqCacheKey *next = base->link_next;

  while (base) {
    SeqCacheKey *prev = base->link_prev;
    base->is_temp_cache = true;
    base = prev;
  }

  base = next;
  while (base) {
    next = base->link_next;
    base->is_temp_cache = true;
    base = next;
  }
}

static void seq_cache_create(Main *bmain, Scene *scene)
{
  BLI_mutex_lock(&cache_create_lock);
  if (scene->ed->cache == nullptr) {
    SeqCache *cache = MEM_callocN<SeqCache>("SeqCache");
    cache->keys_pool = BLI_mempool_create(sizeof(SeqCacheKey), 0, 64, BLI_MEMPOOL_NOP);
    cache->items_pool = BLI_mempool_create(sizeof(SeqCacheItem), 0, 64, BLI_MEMPOOL_NOP);
    cache->hash = BLI_ghash_new(seq_cache_hashhash, seq_cache_hashcmp, "SeqCache hash");
    cache->last_key = nullptr;
    cache->bmain = bmain;
    BLI_mutex_init(&cache->iterator_mutex);
    scene->ed->cache = cache;

    if (scene->ed->disk_cache_timestamp == 0) {
      scene->ed->disk_cache_timestamp = time(nullptr);
    }
  }
  BLI_mutex_unlock(&cache_create_lock);
}

static void seq_cache_populate_key(SeqCacheKey *key,
                                   const RenderData *context,
                                   Strip *strip,
                                   const float timeline_frame,
                                   const int type)
{
  key->cache_owner = seq_cache_get_from_scene(context->scene);
  key->strip = strip;
  key->context = *context;
  key->frame_index = seq_cache_timeline_frame_to_frame_index(
      context->scene, strip, timeline_frame, type);
  key->type = type;
  key->link_prev = nullptr;
  key->link_next = nullptr;
  key->is_temp_cache = true;
  key->task_id = context->task_id;
}

static SeqCacheKey *seq_cache_allocate_key(SeqCache *cache,
                                           const RenderData *context,
                                           Strip *strip,
                                           const float timeline_frame,
                                           const int type)
{
  SeqCacheKey *key = static_cast<SeqCacheKey *>(BLI_mempool_alloc(cache->keys_pool));
  seq_cache_populate_key(key, context, strip, timeline_frame, type);
  return key;
}

/* ***************************** API ****************************** */

void seq_cache_free_temp_cache(Scene *scene, short id, int timeline_frame)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return;
  }

  seq_cache_lock(scene);

  GHashIterator gh_iter;
  BLI_ghashIterator_init(&gh_iter, cache->hash);
  while (!BLI_ghashIterator_done(&gh_iter)) {
    SeqCacheKey *key = static_cast<SeqCacheKey *>(BLI_ghashIterator_getKey(&gh_iter));
    BLI_ghashIterator_step(&gh_iter);
    BLI_assert(key->cache_owner == cache);

    if (key->is_temp_cache && key->task_id == id) {
      /* Use frame_index here to avoid freeing raw images if they are used for multiple frames. */
      float frame_index = seq_cache_timeline_frame_to_frame_index(
          scene, key->strip, timeline_frame, key->type);
      if (frame_index != key->frame_index ||
          timeline_frame > time_right_handle_frame_get(scene, key->strip) ||
          timeline_frame < time_left_handle_frame_get(scene, key->strip))
      {
        seq_cache_key_unlink(key);
        BLI_ghash_remove(cache->hash, key, seq_cache_keyfree, seq_cache_valfree);
        if (key == cache->last_key) {
          cache->last_key = nullptr;
        }
      }
    }
  }
  seq_cache_unlock(scene);
}

void seq_cache_destruct(Scene *scene)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return;
  }

  BLI_ghash_free(cache->hash, seq_cache_keyfree, seq_cache_valfree);
  BLI_mempool_destroy(cache->keys_pool);
  BLI_mempool_destroy(cache->items_pool);
  BLI_mutex_end(&cache->iterator_mutex);

  if (cache->disk_cache != nullptr) {
    seq_disk_cache_free(cache->disk_cache);
  }

  MEM_freeN(cache);
  scene->ed->cache = nullptr;
}

void cache_cleanup(Scene *scene)
{
  prefetch_stop(scene);

  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return;
  }

  seq_cache_lock(scene);

  GHashIterator gh_iter;
  BLI_ghashIterator_init(&gh_iter, cache->hash);
  while (!BLI_ghashIterator_done(&gh_iter)) {
    SeqCacheKey *key = static_cast<SeqCacheKey *>(BLI_ghashIterator_getKey(&gh_iter));
    BLI_assert(key->cache_owner == cache);

    BLI_ghashIterator_step(&gh_iter);

    /* NOTE: no need to call #seq_cache_key_unlink as all keys are removed. */
    BLI_ghash_remove(cache->hash, key, seq_cache_keyfree, seq_cache_valfree);
  }
  cache->last_key = nullptr;
  seq_cache_unlock(scene);
}

void seq_cache_cleanup_strip(Scene *scene,
                             Strip *strip,
                             Strip *strip_changed,
                             int invalidate_types,
                             bool force_strip_changed_range)
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return;
  }

  if (seq_disk_cache_is_enabled(cache->bmain) && cache->disk_cache != nullptr) {
    seq_disk_cache_invalidate(cache->disk_cache, scene, strip, strip_changed, invalidate_types);
  }

  seq_cache_lock(scene);

  const int range_start_strip_changed = time_left_handle_frame_get(scene, strip_changed);
  const int range_end_strip_changed = time_right_handle_frame_get(scene, strip_changed);

  int range_start = range_start_strip_changed;
  int range_end = range_end_strip_changed;

  if (!force_strip_changed_range) {
    const int range_start_strip = time_left_handle_frame_get(scene, strip);
    const int range_end_strip = time_right_handle_frame_get(scene, strip);

    range_start = max_ii(range_start, range_start_strip);
    range_end = min_ii(range_end, range_end_strip);
  }

  int invalidate_composite = invalidate_types & SEQ_CACHE_STORE_FINAL_OUT;
  int invalidate_source = invalidate_types & (SEQ_CACHE_STORE_RAW | SEQ_CACHE_STORE_PREPROCESSED |
                                              SEQ_CACHE_STORE_COMPOSITE);

  GHashIterator gh_iter;
  BLI_ghashIterator_init(&gh_iter, cache->hash);
  while (!BLI_ghashIterator_done(&gh_iter)) {
    SeqCacheKey *key = static_cast<SeqCacheKey *>(BLI_ghashIterator_getKey(&gh_iter));
    BLI_ghashIterator_step(&gh_iter);
    BLI_assert(key->cache_owner == cache);

    const int key_timeline_frame = seq_cache_key_timeline_frame_get(key);
    /* Clean all final and composite in intersection of strip and strip_changed. */
    if (key->type & invalidate_composite && key_timeline_frame >= range_start &&
        key_timeline_frame <= range_end)
    {
      seq_cache_key_unlink(key);
      BLI_ghash_remove(cache->hash, key, seq_cache_keyfree, seq_cache_valfree);
    }
    else if (key->type & invalidate_source && key->strip == strip &&
             key_timeline_frame >= time_left_handle_frame_get(scene, strip_changed) &&
             key_timeline_frame <= time_right_handle_frame_get(scene, strip_changed))
    {
      seq_cache_key_unlink(key);
      BLI_ghash_remove(cache->hash, key, seq_cache_keyfree, seq_cache_valfree);
    }
  }
  cache->last_key = nullptr;
  seq_cache_unlock(scene);
}

ImBuf *seq_cache_get(const RenderData *context, Strip *strip, float timeline_frame, int type)
{

  if (context->skip_cache || context->is_proxy_render || !strip) {
    return nullptr;
  }

  Scene *scene = context->scene;

  if (context->is_prefetch_render) {
    context = seq_prefetch_original_context_get(context);
    scene = context->scene;
    strip = seq_prefetch_original_strip_get(strip, scene);
  }

  if (!strip) {
    return nullptr;
  }

  if (!scene->ed->cache) {
    seq_cache_create(context->bmain, scene);
  }

  seq_cache_lock(scene);
  SeqCache *cache = seq_cache_get_from_scene(scene);
  ImBuf *ibuf = nullptr;
  SeqCacheKey key;

  /* Try RAM cache: */
  if (cache && strip) {
    seq_cache_populate_key(&key, context, strip, timeline_frame, type);
    ibuf = seq_cache_get_ex(cache, &key);
  }
  seq_cache_unlock(scene);

  if (ibuf) {
    return ibuf;
  }

  if (context->for_render) {
    return nullptr;
  }

  /* Try disk cache: */
  if (seq_disk_cache_is_enabled(context->bmain)) {
    if (cache->disk_cache == nullptr) {
      cache->disk_cache = seq_disk_cache_create(context->bmain, context->scene);
    }

    ibuf = seq_disk_cache_read_file(cache->disk_cache, &key);

    if (ibuf == nullptr) {
      return nullptr;
    }

    /* Store read image in RAM. Only recycle item for final type. */
    if (key.type != SEQ_CACHE_STORE_FINAL_OUT || seq_cache_recycle_item(scene)) {
      SeqCacheKey *new_key = seq_cache_allocate_key(cache, context, strip, timeline_frame, type);
      seq_cache_put_ex(scene, new_key, ibuf);
    }
  }

  return ibuf;
}

bool seq_cache_put_if_possible(
    const RenderData *context, Strip *strip, float timeline_frame, int type, ImBuf *ibuf)
{
  Scene *scene = context->scene;

  if (context->is_prefetch_render) {
    context = seq_prefetch_original_context_get(context);
    scene = context->scene;
    strip = seq_prefetch_original_strip_get(strip, scene);
  }

  if (!strip) {
    return false;
  }

  if (seq_cache_recycle_item(scene)) {
    seq_cache_put(context, strip, timeline_frame, type, ibuf);
    return true;
  }

  if (scene->ed->cache) {
    seq_cache_set_temp_cache_linked(scene, scene->ed->cache->last_key);
    scene->ed->cache->last_key = nullptr;
  }

  return false;
}

void seq_cache_put(
    const RenderData *context, Strip *strip, float timeline_frame, int type, ImBuf *i)
{
  if (i == nullptr || context->skip_cache || context->is_proxy_render || !strip) {
    return;
  }

  Scene *scene = context->scene;

  if (context->is_prefetch_render) {
    context = seq_prefetch_original_context_get(context);
    scene = context->scene;
    strip = seq_prefetch_original_strip_get(strip, scene);
    BLI_assert(strip != nullptr);
  }

  /* Prevent reinserting, it breaks cache key linking. */
  ImBuf *test = seq_cache_get(context, strip, timeline_frame, type);
  if (test) {
    IMB_freeImBuf(test);
    return;
  }

  if (!scene->ed->cache) {
    seq_cache_create(context->bmain, scene);
  }

  seq_cache_lock(scene);
  SeqCache *cache = seq_cache_get_from_scene(scene);
  SeqCacheKey *key = seq_cache_allocate_key(cache, context, strip, timeline_frame, type);
  seq_cache_put_ex(scene, key, i);
  seq_cache_unlock(scene);

  if (context->for_render) {
    key->is_temp_cache = true;
  }

  if (!key->is_temp_cache) {
    if (seq_disk_cache_is_enabled(context->bmain)) {
      if (cache->disk_cache == nullptr) {
        seq_disk_cache_create(context->bmain, context->scene);
      }

      seq_disk_cache_write_file(cache->disk_cache, key, i);
      seq_disk_cache_enforce_limits(cache->disk_cache);
    }
  }
}

void cache_iterate(
    Scene *scene,
    void *userdata,
    bool callback_init(void *userdata, size_t item_count),
    bool callback_iter(void *userdata, Strip *strip, int timeline_frame, int cache_type))
{
  SeqCache *cache = seq_cache_get_from_scene(scene);
  if (!cache) {
    return;
  }

  seq_cache_lock(scene);
  bool interrupt = callback_init(userdata, BLI_ghash_len(cache->hash));

  GHashIterator gh_iter;
  BLI_ghashIterator_init(&gh_iter, cache->hash);

  while (!BLI_ghashIterator_done(&gh_iter) && !interrupt) {
    SeqCacheKey *key = static_cast<SeqCacheKey *>(BLI_ghashIterator_getKey(&gh_iter));
    BLI_ghashIterator_step(&gh_iter);
    BLI_assert(key->cache_owner == cache);
    int timeline_frame;
    if (key->type & SEQ_CACHE_STORE_FINAL_OUT) {
      timeline_frame = seq_cache_key_timeline_frame_get(key);
    }
    else {
      /* This is not a final cache image. The cached frame is relative to where the strip is
       * currently and where it was when it was cached. We can't use the timeline_frame, we need to
       * derive the timeline frame from key->frame_index.
       *
       * NOTE This will not work for RAW caches if they have retiming, strobing, or different
       * playback rate than the scene. Because it would take quite a bit of effort to properly
       * convert RAW frames like that to a timeline frame, we skip doing this as visualizing these
       * are a developer option that not many people will see.
       */
      timeline_frame = key->frame_index + time_start_frame_get(key->strip);
    }

    interrupt = callback_iter(userdata, key->strip, timeline_frame, key->type);
  }

  cache->last_key = nullptr;
  seq_cache_unlock(scene);
}

bool seq_cache_is_full()
{
  return seq_cache_get_mem_total() < MEM_get_memory_in_use();
}

}  // namespace blender::seq
