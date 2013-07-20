/*
 * lwan - simple web server
 * Copyright (c) 2013 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#include "lwan.h"
#include "lwan-cache.h"
#include "hash.h"

enum {
  FLOATING = 1<<0
};

struct cache_t {
  struct {
    CreateEntryCallback create_entry;
    DestroyEntryCallback destroy_entry;
    void *context;
  } cb;
  struct {
    struct hash *table;
    pthread_rwlock_t lock;
  } hash;
  struct {
    struct list_head list;
    pthread_rwlock_t lock;
  } queue;
  struct {
    time_t time_to_live;
  } settings;
  struct {
    unsigned hits;
    unsigned misses;
    unsigned evicted;
  } stats;
  bool shutting_down : 1;
};

static bool cache_pruner_job(void *data);

struct cache_t *cache_create(CreateEntryCallback create_entry_cb,
      DestroyEntryCallback destroy_entry_cb,
      void *cb_context,
      time_t time_to_live)
{
  struct cache_t *cache;

  assert(create_entry_cb);
  assert(destroy_entry_cb);
  assert(time_to_live > 0);

  cache = calloc(1, sizeof(*cache));
  if (!cache)
    return NULL;

  cache->hash.table = hash_str_new(free, NULL);
  if (!cache->hash.table)
    goto error_no_hash;

  if (pthread_rwlock_init(&cache->hash.lock, NULL))
    goto error_no_hash_lock;
  if (pthread_rwlock_init(&cache->queue.lock, NULL))
    goto error_no_queue_lock;

  cache->cb.create_entry = create_entry_cb;
  cache->cb.destroy_entry = destroy_entry_cb;
  cache->cb.context = cb_context;

  cache->settings.time_to_live = time_to_live;

  list_head_init(&cache->queue.list);

  lwan_job_add(cache_pruner_job, cache);

  return cache;

error_no_queue_lock:
  pthread_rwlock_destroy(&cache->hash.lock);
error_no_hash_lock:
  hash_free(cache->hash.table);
error_no_hash:
  free(cache);

  return NULL;
}

void cache_destroy(struct cache_t *cache)
{
  assert(cache);

  lwan_job_del(cache_pruner_job, cache);
  cache->shutting_down = true;
  cache_pruner_job(cache);
  pthread_rwlock_destroy(&cache->hash.lock);
  pthread_rwlock_destroy(&cache->queue.lock);
  hash_free(cache->hash.table);
  free(cache);
}

struct cache_entry_t *cache_get_and_ref_entry(struct cache_t *cache,
      const char *key, int *error)
{
  struct cache_entry_t *entry;

  assert(cache);

  *error = 0;

  if (UNLIKELY(pthread_rwlock_tryrdlock(&cache->hash.lock) == EBUSY)) {
    *error = EWOULDBLOCK;
    return NULL;
  }
  entry = hash_find(cache->hash.table, key);
  if (LIKELY(entry)) {
    ATOMIC_INC(entry->refs);
    pthread_rwlock_unlock(&cache->hash.lock);
    ATOMIC_INC(cache->stats.hits);
    return entry;
  }
  pthread_rwlock_unlock(&cache->hash.lock);

  ATOMIC_INC(cache->stats.misses);

  entry = cache->cb.create_entry(key, cache->cb.context);
  if (!entry)
    return NULL;

  memset(entry, 0, sizeof(*entry));
  entry->key = strdup(key);

try_adding_again:
  pthread_rwlock_wrlock(&cache->hash.lock);
  switch (hash_add_unique(cache->hash.table, entry->key, entry)) {
  case -EEXIST: {
      struct cache_entry_t *tmp_entry;

      pthread_rwlock_unlock(&cache->hash.lock);

      pthread_rwlock_rdlock(&cache->hash.lock);
      tmp_entry = hash_find(cache->hash.table, key);
      if (tmp_entry) {
        cache->cb.destroy_entry(entry, cache->cb.context);
        entry = tmp_entry;
        goto end;
      }

      pthread_rwlock_unlock(&cache->hash.lock);
      goto try_adding_again;
    }
  default:
    entry->flags = FLOATING;
    entry->time_to_die = time(NULL);
    entry->refs = 1;
    goto unlock_and_return;
  case 0:
    entry->time_to_die = time(NULL) + cache->settings.time_to_live;
    pthread_rwlock_wrlock(&cache->queue.lock);
    list_add_tail(&cache->queue.list, &entry->entries);
    pthread_rwlock_unlock(&cache->queue.lock);
  }

end:
  ATOMIC_INC(entry->refs);
unlock_and_return:
  pthread_rwlock_unlock(&cache->hash.lock);

  return entry;
}

void cache_entry_unref(struct cache_t *cache, struct cache_entry_t *entry)
{
  assert(entry);

  if (!ATOMIC_DEC(entry->refs) && entry->flags & FLOATING)
    cache->cb.destroy_entry(entry, cache->cb.context);
}

static bool cache_pruner_job(void *data)
{
  struct cache_t *cache = data;
  struct cache_entry_t *node, *next;
  time_t now;
  bool had_job = false;
  bool shutting_down = cache->shutting_down;
  unsigned evicted = 0;

  if (pthread_rwlock_trywrlock(&cache->queue.lock) < 0) {
    lwan_status_perror("pthread_rwlock_trywrlock");
    return false;
  }
  if (pthread_rwlock_wrlock(&cache->hash.lock) < 0) {
    lwan_status_perror("pthread_rwlock_wrlock");
    goto unlock_queue_lock;
  }

  now = time(NULL);
  list_for_each_safe(&cache->queue.list, node, next, entries) {
    char *key = node->key;

    if (LIKELY(!shutting_down) && now > node->time_to_die)
      break;

    list_del(&node->entries);

    if (!ATOMIC_READ(node->refs))
      cache->cb.destroy_entry(node, cache->cb.context);
    else
      __sync_and_and_fetch(&node->flags, FLOATING);

    /*
     * FIXME: Find a way to avoid this hash lookup, as we already have
     *        a reference to the item itself.
     */
    hash_del(cache->hash.table, key);
    evicted++;
    had_job = true;
  }

  if (pthread_rwlock_unlock(&cache->hash.lock) < 0)
    lwan_status_perror("pthread_rwlock_unlock");
unlock_queue_lock:
  if (pthread_rwlock_unlock(&cache->queue.lock) < 0)
    lwan_status_perror("pthread_rwlock_unlock");

  ATOMIC_AAF(&cache->stats.evicted, evicted);

  return had_job;
}

void cache_get_stats(struct cache_t *cache, unsigned *hits,
      unsigned *misses, unsigned *evicted)
{
  assert(cache);
  if (hits)
    *hits = cache->stats.hits;
  if (misses)
    *misses = cache->stats.misses;
  if (evicted)
    *evicted = cache->stats.evicted;
}

struct cache_entry_t *
cache_coro_get_and_ref_entry(struct cache_t *cache, coro_t *coro,
      const char *key)
{
    struct cache_entry_t *ce;
    int error;

    while (true) {
        ce = cache_get_and_ref_entry(cache, key, &error);
        if (LIKELY(ce)) {
            /*
             * This is deferred here so that, if the coroutine is killed
             * after it has been yielded, this cache entry is properly
             * freed.
             */
            coro_defer2(coro, CORO_DEFER2(cache_entry_unref), cache, ce);
            break;
        }

        /*
         * If the cache would block while reading its hash table, yield and
         * try again. On any other error, just return NULL.
         */
        if (UNLIKELY(error != EWOULDBLOCK))
            break;

        coro_yield(coro, 1);
    }

    return ce;
}