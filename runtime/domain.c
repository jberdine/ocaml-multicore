/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                 Stephen Dolan, University of Cambridge                 */
/*                                                                        */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "caml/alloc.h"
#include "caml/callback.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/eventlog.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/finalise.h"
#include "caml/gc_ctrl.h"
#include "caml/globroots.h"
#include "caml/major_gc.h"
#include "caml/minor_gc.h"
#include "caml/memory.h"
#include "caml/osdeps.h"
#include "caml/platform.h"
#include "caml/shared_heap.h"
#include "caml/signals.h"
#include "caml/startup.h"
#include "caml/sync.h"
#include "caml/weak.h"

#define BT_IN_BLOCKING_SECTION 0
#define BT_ENTERING_OCAML 1
#define BT_TERMINATE 2
#define BT_INIT 3

/* Since we support both heavyweight OS threads and lightweight
   userspace threads, the word "thread" is ambiguous. This file deals
   with OS-level threads, called "domains".
*/

/* control of STW interrupts */
struct interruptor {
  atomic_uintnat* interrupt_word;
  caml_plat_mutex lock;
  caml_plat_cond cond;

  int running;
  int terminating;
  /* unlike the domain ID, this ID number is not reused */
  uintnat unique_id;

  atomic_uintnat interrupt_pending;
};

struct dom_internal {
  /* readonly fields, initialised and never modified */
  int id;
  caml_domain_state* state;
  struct interruptor interruptor;

  /* backup thread */
  int backup_thread_running;
  pthread_t backup_thread;
  atomic_uintnat backup_thread_msg;
  caml_plat_mutex domain_lock;
  caml_plat_cond domain_cond;

  /* readonly */
  uintnat tls_area;
  uintnat tls_area_end;
  uintnat minor_heap_area;
  uintnat minor_heap_area_end;
};
typedef struct dom_internal dom_internal;


static struct {
  atomic_uintnat domains_still_running;
  atomic_uintnat num_domains_still_processing;
  void (*callback)(caml_domain_state*, void*, int participating_count, caml_domain_state** others_participating);
  void* data;
  void (*enter_spin_callback)(caml_domain_state*, void*);
  void* enter_spin_data;

  /* barrier state */
  int num_domains;
  atomic_uintnat barrier;

  caml_domain_state* participating[Max_domains];
} stw_request = {
  ATOMIC_UINTNAT_INIT(0),
  ATOMIC_UINTNAT_INIT(0),
  NULL,
  NULL,
  NULL,
  NULL,
  0,
  ATOMIC_UINTNAT_INIT(0),
  { 0 },
};

static caml_plat_mutex all_domains_lock = CAML_PLAT_MUTEX_INITIALIZER;
static caml_plat_cond all_domains_cond = CAML_PLAT_COND_INITIALIZER(&all_domains_lock);
static atomic_uintnat /* dom_internal* */ stw_leader = 0;
static struct dom_internal all_domains[Max_domains];

CAMLexport atomic_uintnat caml_num_domains_running;

CAMLexport uintnat caml_minor_heaps_base;
CAMLexport uintnat caml_minor_heaps_end;
CAMLexport uintnat caml_tls_areas_base;
static __thread dom_internal* domain_self;

static int64_t startup_timestamp;

static uintnat handle_incoming(struct interruptor* s);

#ifdef __APPLE__
/* OSX has issues with dynamic loading + exported TLS.
    This is slower but works */
CAMLexport pthread_key_t caml_domain_state_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void caml_make_domain_state_key ()
{
  (void) pthread_key_create (&caml_domain_state_key, NULL);
}

void caml_init_domain_state_key ()
{
  pthread_once(&key_once, caml_make_domain_state_key);
}

#else
CAMLexport __thread caml_domain_state* Caml_state;
#endif

/* Interrupt functions */
static const uintnat INTERRUPT_MAGIC = (uintnat)(-1);

Caml_inline void interrupt_domain(struct interruptor* s) {
  atomic_store_rel(s->interrupt_word, INTERRUPT_MAGIC);
}

int caml_incoming_interrupts_queued()
{
  return atomic_load_acq(&domain_self->interruptor.interrupt_pending);
}

/* must NOT be called with s->lock held */
static void stw_handler(caml_domain_state* domain);
static uintnat handle_incoming(struct interruptor* s)
{
  uintnat handled = atomic_load_acq(&s->interrupt_pending);
  Assert (s->running);
  if (handled) {
    atomic_store_rel(&s->interrupt_pending, 0);

    stw_handler(domain_self->state);
  }
  return handled;
}

static void handle_incoming_otherwise_relax (struct interruptor* self)
{
  if (!handle_incoming(self))
    cpu_relax();
}

void caml_handle_incoming_interrupts()
{
  handle_incoming(&domain_self->interruptor);
}

int caml_send_interrupt(struct interruptor* target)
{
  /* we have blocked new domains joining as the STW is in progress
   * a target domain can not go from 'not running' to 'running'
   */
  if (!target->running) return 0;

  caml_plat_lock(&target->lock);
  if (!target->running) {
    caml_plat_unlock(&target->lock);
    return 0;
  }

  /* signal that there is an interrupt pending */
  Assert(!atomic_load_acq(&target->interrupt_pending));
  atomic_store_rel(&target->interrupt_pending, 1);

  /* Signal the condition variable, in case the target is
     itself waiting for an interrupt to be processed elsewhere */
  caml_plat_broadcast(&target->cond); // OPT before/after unlock? elide?
  caml_plat_unlock(&target->lock);

  interrupt_domain(target);

  return 1;
}

static void caml_wait_interrupt_serviced(struct interruptor* target)
{
  int i;

  /* Often, interrupt handlers are fast, so spin for a bit before waiting */
  for (i=0; i<1000; i++) {
    if (!atomic_load_acq(&target->interrupt_pending)) {
      return;
    }
    cpu_relax();
  }

  {
    SPIN_WAIT {
      if (!atomic_load_acq(&target->interrupt_pending))
        return;
      cpu_relax();
    }
  }
}

asize_t caml_norm_minor_heap_size (intnat wsize)
{
  asize_t bs, max;
  if (wsize < Minor_heap_min) wsize = Minor_heap_min;
  bs = caml_mem_round_up_pages(Bsize_wsize (wsize));

  max = Bsize_wsize(Minor_heap_max);

  if (bs > max) bs = max;

  return Wsize_bsize(bs);
}

int caml_reallocate_minor_heap(asize_t wsize)
{
  caml_domain_state* domain_state = Caml_state;
  Assert(domain_state->young_ptr == domain_state->young_end);

  /* free old minor heap.
     instead of unmapping the heap, we decommit it, so there's
     no race whereby other code could attempt to reuse the memory. */
  caml_mem_decommit((void*)domain_self->minor_heap_area,
                    domain_self->minor_heap_area_end - domain_self->minor_heap_area);

  wsize = caml_norm_minor_heap_size(wsize);

  if (!caml_mem_commit((void*)domain_self->minor_heap_area, Bsize_wsize(wsize))) {
    return -1;
  }

#ifdef DEBUG
  {
    uintnat* p = (uintnat*)domain_self->minor_heap_area;
    for (; p < (uintnat*)(domain_self->minor_heap_area + Bsize_wsize(wsize)); p++)
      *p = Debug_uninit_align;
  }
#endif

  domain_state->minor_heap_wsz = wsize;

  domain_state->young_start = (char*)domain_self->minor_heap_area;
  domain_state->young_end = (char*)(domain_self->minor_heap_area + Bsize_wsize(wsize));
  domain_state->young_limit = (uintnat) domain_state->young_start;
  domain_state->young_ptr = domain_state->young_end;
  return 0;
}

/* must be run on the domain's thread */
static void create_domain(uintnat initial_minor_heap_wsize) {
  int i;
  dom_internal* d = 0;
  Assert (domain_self == 0);

  caml_plat_lock(&all_domains_lock);

  /* wait until any in-progress STW sections end */
  while (atomic_load_acq(&stw_leader)) caml_plat_wait(&all_domains_cond);

  for (i = 0; i < Max_domains && !d; i++) {
    struct interruptor* s = &all_domains[i].interruptor;
    caml_plat_lock(&s->lock);
    if (!s->running) {
      d = &all_domains[i];
      if (!s->interrupt_word) {
        caml_domain_state* domain_state;
        atomic_uintnat* young_limit;
        /* never been started before, so set up minor heap */
        if (!caml_mem_commit((void*)d->tls_area, (d->tls_area_end - d->tls_area))) {
          /* give up now: if we couldn't get memory for this domain, we're
             unlikely to have better luck with any other */
          d = 0;
          caml_plat_unlock(&s->lock);
          break;
        }
      	domain_state = (caml_domain_state*)(d->tls_area);
        young_limit = (atomic_uintnat*)&domain_state->young_limit;
        s->interrupt_word = young_limit;
        atomic_store_rel(young_limit, (uintnat)domain_state->young_start);
      }
      Assert(!s->interrupt_pending);
      s->running = 1;
      atomic_fetch_add(&caml_num_domains_running, 1);
    }
    caml_plat_unlock(&s->lock);
  }
  if (d) {
    caml_domain_state* domain_state;
    domain_self = d;
    SET_Caml_state((void*)(d->tls_area));
    domain_state = (caml_domain_state*)(d->tls_area);
    caml_plat_lock(&d->domain_lock);

    domain_state->id = d->id;
    domain_state->unique_id = d->interruptor.unique_id;
    d->state = domain_state;
    Assert(!d->interruptor.interrupt_pending);

    if (caml_init_signal_stack() < 0) {
      goto init_signal_stack_failure;
    }

    domain_state->young_start = domain_state->young_end =
      domain_state->young_ptr = 0;
    domain_state->minor_tables = caml_alloc_minor_tables();
    if(domain_state->minor_tables == NULL) {
      goto alloc_minor_tables_failure;
    }

    d->state->shared_heap = caml_init_shared_heap();
    if(d->state->shared_heap == NULL) {
      goto init_shared_heap_failure;
    }

    if (caml_init_major_gc(domain_state) < 0) {
      goto init_major_gc_failure;
    }

    if(caml_reallocate_minor_heap(initial_minor_heap_wsize) < 0) {
      goto reallocate_minor_heap_failure;
    }

    domain_state->dls_root = Val_unit;
    caml_register_generational_global_root(&domain_state->dls_root);

    domain_state->unique_token_root = caml_alloc_shr_noexc(Abstract_tag, Val_unit);
    if(domain_state->unique_token_root == (value)NULL) {
      goto create_unique_token_failure;
    }

    caml_register_generational_global_root(&domain_state->unique_token_root);

    domain_state->stack_cache = caml_alloc_stack_cache();
    if(domain_state->stack_cache == NULL) {
      goto create_stack_cache_failure;
    }

    domain_state->current_stack =
        caml_alloc_main_stack(Stack_size / sizeof(value));
    if(domain_state->current_stack == NULL) {
      goto alloc_main_stack_failure;
    }

    domain_state->backtrace_buffer = NULL;
    domain_state->backtrace_last_exn = Val_unit;
    caml_register_generational_global_root(&domain_state->backtrace_last_exn);
#ifndef NATIVE_CODE
    domain_state->external_raise = NULL;
    domain_state->trap_sp_off = 1;
#endif


#if defined(NAKED_POINTERS_CHECKER) && !defined(_WIN32)
    domain_state->checking_pointer_pc = NULL;
#endif

    goto domain_init_complete;

  caml_free_stack(domain_state->current_stack);
alloc_main_stack_failure:
create_stack_cache_failure:
  caml_remove_generational_global_root(&domain_state->unique_token_root);
  caml_remove_generational_global_root(&domain_state->dls_root);
create_unique_token_failure:
reallocate_minor_heap_failure:
  caml_teardown_major_gc();
init_major_gc_failure:
  caml_teardown_shared_heap(d->state->shared_heap);
init_shared_heap_failure:
  caml_free_minor_tables(domain_state->minor_tables);
  domain_state->minor_tables = NULL;
alloc_minor_tables_failure:
  caml_free_signal_stack();
init_signal_stack_failure:
  domain_self = NULL;

  }
domain_init_complete:
  caml_plat_unlock(&all_domains_lock);
}

CAMLexport void caml_reset_domain_lock(void)
{
  dom_internal* self = domain_self;
  // This is only used to reset the domain_lock state on fork.
  caml_plat_mutex_init(&self->domain_lock);
  caml_plat_cond_init(&self->domain_cond, &self->domain_lock);

  return;
}

void caml_init_domains(uintnat minor_heap_wsz) {
  int i;
  uintnat size;
  uintnat tls_size;
  uintnat tls_areas_size;
  void* heaps_base;
  void* tls_base;

  /* sanity check configuration */
  if (caml_mem_round_up_pages(Bsize_wsize(Minor_heap_max)) != Bsize_wsize(Minor_heap_max))
    caml_fatal_error("Minor_heap_max misconfigured for this platform");

  /* reserve memory space for minor heaps and tls_areas */
  size = (uintnat)Bsize_wsize(Minor_heap_max) * Max_domains;
  tls_size = caml_mem_round_up_pages(sizeof(caml_domain_state));
  tls_areas_size = tls_size * Max_domains;

  heaps_base = caml_mem_map(size, size, 1 /* reserve_only */);
  tls_base = caml_mem_map(tls_areas_size, tls_areas_size, 1 /* reserve_only */);
  if (!heaps_base || !tls_base) caml_raise_out_of_memory();

  caml_minor_heaps_base = (uintnat) heaps_base;
  caml_minor_heaps_end = (uintnat) heaps_base + size;
  caml_tls_areas_base = (uintnat) tls_base;

  for (i = 0; i < Max_domains; i++) {
    struct dom_internal* dom = &all_domains[i];
    uintnat domain_minor_heap_base;
    uintnat domain_tls_base;

    dom->id = i;

    dom->interruptor.interrupt_word = 0;
    caml_plat_mutex_init(&dom->interruptor.lock);
    caml_plat_cond_init(&dom->interruptor.cond,
                        &dom->interruptor.lock);
    dom->interruptor.running = 0;
    dom->interruptor.terminating = 0;
    dom->interruptor.unique_id = i;
    dom->interruptor.interrupt_pending = 0;

    caml_plat_mutex_init(&dom->domain_lock);
    caml_plat_cond_init(&dom->domain_cond, &dom->domain_lock);
    dom->backup_thread_running = 0;
    dom->backup_thread_msg = BT_INIT;

    domain_minor_heap_base = caml_minor_heaps_base +
      (uintnat)Bsize_wsize(Minor_heap_max) * (uintnat)i;
    domain_tls_base = caml_tls_areas_base + tls_size * (uintnat)i;
    dom->tls_area = domain_tls_base;
    dom->tls_area_end = domain_tls_base + tls_size;
    dom->minor_heap_area = domain_minor_heap_base;
    dom->minor_heap_area_end = domain_minor_heap_base + Bsize_wsize(Minor_heap_max);
  }


  create_domain(minor_heap_wsz);
  if (!domain_self) caml_fatal_error("Failed to create main domain");

  caml_init_signal_handling();
  startup_timestamp = caml_time_counter();

  CAML_EVENTLOG_INIT();
}

void caml_init_domain_self(int domain_id) {
  Assert (domain_id >= 0 && domain_id < Max_domains);
  domain_self = &all_domains[domain_id];
  SET_Caml_state(domain_self->state);
}

enum domain_status { Dom_starting, Dom_started, Dom_failed };

struct domain_ml_values {
  value callback;
  value mutex;
};

static void init_domain_ml_values(struct domain_ml_values* ml_values, value callback, value mutex) {
  ml_values->callback = callback;
  ml_values->mutex = mutex;
  caml_register_generational_global_root(&ml_values->callback);
  caml_register_generational_global_root(&ml_values->mutex);
}

static void free_domain_ml_values(struct domain_ml_values* ml_values) {
  caml_remove_generational_global_root(&ml_values->callback);
  caml_remove_generational_global_root(&ml_values->mutex);
  caml_stat_free(ml_values);
}

struct domain_startup_params {
  struct interruptor* parent;
  enum domain_status status;
  struct domain_ml_values* ml_values;
  dom_internal* newdom;
  uintnat unique_id;
};

static void* backup_thread_func(void* v)
{
  dom_internal* di = (dom_internal*)v;
  uintnat msg;
  struct interruptor* s = &di->interruptor;

  domain_self = di;
  SET_Caml_state((void*)(di->tls_area));

  CAML_EVENTLOG_IS_BACKUP_THREAD();

  /* TODO: how does the backup thread interact with the eventlog infra?
   * caml_ev_tag_self_as_backup_thread(); */

  msg = atomic_load_acq (&di->backup_thread_msg);
  while (msg != BT_TERMINATE) {
    Assert (msg <= BT_TERMINATE);
    switch (msg) {
      case BT_IN_BLOCKING_SECTION:
        /* Handle interrupts on behalf of the main thread:
         *  - must hold domain_lock to handle interrupts
         *  - need to guarantee no blocking so that backup thread
         *    can be signalled from caml_leave_blocking_section
         */
        if (caml_incoming_interrupts_queued()) {
          if (caml_plat_try_lock(&di->domain_lock)) {
            caml_handle_incoming_interrupts();
            caml_plat_unlock(&di->domain_lock);
          }
        }
        /* Wait safely if there is nothing to do.
         * Will be woken from caml_leave_blocking_section
         */
        caml_plat_lock(&s->lock);
        msg = atomic_load_acq (&di->backup_thread_msg);
        if (msg == BT_IN_BLOCKING_SECTION &&
            !caml_incoming_interrupts_queued())
          caml_plat_wait(&s->cond);
        caml_plat_unlock(&s->lock);
        break;
      case BT_ENTERING_OCAML:
        /* Main thread wants to enter OCaml
         * Will be woken from caml_enter_blocking_section
         * or domain_terminate
         */
        caml_plat_lock(&di->domain_lock);
        msg = atomic_load_acq (&di->backup_thread_msg);
        if (msg == BT_ENTERING_OCAML)
          caml_plat_wait(&di->domain_cond);
        caml_plat_unlock(&di->domain_lock);
        break;
      default:
        cpu_relax();
        break;
    };
    msg = atomic_load_acq (&di->backup_thread_msg);
  }

  /* doing terminate */
  atomic_store_rel(&di->backup_thread_msg, BT_INIT);

  return 0;
}

static void install_backup_thread (dom_internal* di)
{
  int err;

  if (di->backup_thread_running == 0) {
    Assert (di->backup_thread_msg == BT_INIT ||     /* Using fresh domain */
            di->backup_thread_msg == BT_TERMINATE); /* Reusing domain */

    while (atomic_load_acq(&di->backup_thread_msg) != BT_INIT) {
      /* Give a chance for backup thread on this domain to terminate */
      caml_plat_unlock (&di->domain_lock);
      cpu_relax ();
      caml_plat_lock (&di->domain_lock);
    }

    atomic_store_rel(&di->backup_thread_msg, BT_ENTERING_OCAML);
    err = pthread_create (&di->backup_thread, 0, backup_thread_func, (void*)di);
    if (err)
      caml_failwith("failed to create domain backup thread");
    di->backup_thread_running = 1;
    pthread_detach(di->backup_thread);
  }
}

static void caml_domain_stop_default(void)
{
  return;
}

static void caml_domain_start_default(void)
{
  return;
}

CAMLexport void (*caml_domain_start_hook)(void) =
   caml_domain_start_default;

CAMLexport void (*caml_domain_stop_hook)(void) =
   caml_domain_stop_default;

static void domain_terminate();

static void* domain_thread_func(void* v)
{
  sync_mutex terminate_mutex = NULL;
  struct domain_startup_params* p = v;
  struct domain_ml_values *ml_values = p->ml_values;

  create_domain(caml_params->init_minor_heap_wsz);
  p->newdom = domain_self;

  caml_plat_lock(&p->parent->lock);
  if (domain_self) {
    /* this domain is part of STW sections, so can read ml_values */
    terminate_mutex = Mutex_val(ml_values->mutex);
    sync_mutex_lock(terminate_mutex);
    p->status = Dom_started;
    p->unique_id = domain_self->interruptor.unique_id;
  } else {
    p->status = Dom_failed;
  }
  caml_plat_broadcast(&p->parent->cond);
  caml_plat_unlock(&p->parent->lock);
  /* cannot access p below here */

  if (domain_self) {
    install_backup_thread(domain_self);
    caml_gc_log("Domain starting (unique_id = %"ARCH_INTNAT_PRINTF_FORMAT"u)",
                domain_self->interruptor.unique_id);
    caml_domain_start_hook();
    caml_callback(ml_values->callback, Val_unit);
    domain_terminate();
    /* joining domains will lock/unlock the terminate_mutex
      so this unlock will release them */
    sync_mutex_unlock(terminate_mutex);
    free_domain_ml_values(ml_values);
  } else {
    caml_gc_log("Failed to create domain");
  }
  return 0;
}

CAMLprim value caml_domain_spawn(value callback, value mutex)
{
  CAMLparam2 (callback, mutex);
  struct domain_startup_params p;
  pthread_t th;
  int err;

  CAML_EV_BEGIN(EV_DOMAIN_SPAWN);
  p.parent = &domain_self->interruptor;
  p.status = Dom_starting;

  p.ml_values = (struct domain_ml_values*) caml_stat_alloc_noexc(sizeof(struct domain_ml_values));
  if (!p.ml_values) {
    caml_failwith("failed to create ml values for domain thread");
  }
  init_domain_ml_values(p.ml_values, callback, mutex);

  err = pthread_create(&th, 0, domain_thread_func, (void*)&p);
  if (err) {
    caml_failwith("failed to create domain thread");
  }

  /* while waiting for the child thread to startup
     we need to servicing any STW if they come in */
  caml_plat_lock(&domain_self->interruptor.lock);
  while (p.status == Dom_starting) {
    if (caml_incoming_interrupts_queued()) {
      caml_plat_unlock(&domain_self->interruptor.lock);
      handle_incoming(&domain_self->interruptor);
      caml_plat_lock(&domain_self->interruptor.lock);
    } else {
      caml_plat_wait(&domain_self->interruptor.cond);
    }
  }
  caml_plat_unlock(&domain_self->interruptor.lock);

  if (p.status == Dom_started) {
    /* successfully created a domain.
       p.ml_values is now owned by that domain */
    pthread_detach(th);
  } else {
    Assert (p.status == Dom_failed);
    /* failed */
    pthread_join(th, 0);
    free_domain_ml_values(p.ml_values);
    caml_failwith("failed to allocate domain");
  }
  install_backup_thread(domain_self);
  CAML_EV_END(EV_DOMAIN_SPAWN);
  CAMLreturn (Val_long(p.unique_id));
}

caml_domain_state* caml_owner_of_young_block(value v) {
  int heap_id;
  Assert(Is_young(v));
  heap_id = ((uintnat)v - caml_minor_heaps_base) / Bsize_wsize(Minor_heap_max);
  return all_domains[heap_id].state;
}

CAMLprim value caml_ml_domain_id(value unit)
{
  CAMLnoalloc;
  return Val_int(domain_self->interruptor.unique_id);
}

CAMLprim value caml_ml_domain_unique_token (value unit)
{
  CAMLnoalloc;
  return Caml_state->unique_token_root;
}

/* sense-reversing barrier */
#define BARRIER_SENSE_BIT 0x100000

barrier_status caml_global_barrier_begin()
{
  uintnat b = 1 + atomic_fetch_add(&stw_request.barrier, 1);
  return b;
}

int caml_global_barrier_is_final(barrier_status b)
{
  return ((b & ~BARRIER_SENSE_BIT) == stw_request.num_domains);
}

void caml_global_barrier_end(barrier_status b)
{
  uintnat sense = b & BARRIER_SENSE_BIT;
  if (caml_global_barrier_is_final(b)) {
    /* last domain into the barrier, flip sense */
    atomic_store_rel(&stw_request.barrier, sense ^ BARRIER_SENSE_BIT);
  } else {
    /* wait until another domain flips the sense */
    SPIN_WAIT {
      uintnat barrier = atomic_load_acq(&stw_request.barrier);
      if ((barrier & BARRIER_SENSE_BIT) != sense) break;
    }
  }
}

void caml_global_barrier()
{
  barrier_status b = caml_global_barrier_begin();
  caml_global_barrier_end(b);
}

int caml_global_barrier_num_domains()
{
  return stw_request.num_domains;
}

static void decrement_stw_domains_still_processing()
{
  /* we check if we are the last to leave a stw section
     if so, clear the stw_leader to allow the new stw sections to start.
   */
  intnat am_last = atomic_fetch_add(&stw_request.num_domains_still_processing, -1) == 1;

  if( am_last ) {
    /* release the STW lock to allow new STW sections */
    caml_plat_lock(&all_domains_lock);
    atomic_store_rel(&stw_leader, 0);
    caml_plat_broadcast(&all_domains_cond);
    caml_gc_log("clearing stw leader");
    caml_plat_unlock(&all_domains_lock);
  }
}

static void caml_poll_gc_work();
static void stw_handler(caml_domain_state* domain)
{
  CAML_EV_BEGIN(EV_STW_HANDLER);
  CAML_EV_BEGIN(EV_STW_API_BARRIER);
  {
    SPIN_WAIT {
      if (atomic_load_acq(&stw_request.domains_still_running) == 0)
        break;

      if (stw_request.enter_spin_callback)
        stw_request.enter_spin_callback(domain, stw_request.enter_spin_data);
    }
  }
  CAML_EV_END(EV_STW_API_BARRIER);

  #ifdef DEBUG
  Caml_state->inside_stw_handler = 1;
  #endif
  stw_request.callback(domain, stw_request.data, stw_request.num_domains, stw_request.participating);
  #ifdef DEBUG
  Caml_state->inside_stw_handler = 0;
  #endif

  decrement_stw_domains_still_processing();

  CAML_EV_END(EV_STW_HANDLER);

  /* poll the GC to check for deferred work
     we do this here because blocking or waiting threads only execute
     the interrupt handler and do not poll for deferred work*/
  caml_poll_gc_work();
}


#ifdef DEBUG
int caml_domain_is_in_stw() {
  return Caml_state->inside_stw_handler;
}
#endif

int caml_try_run_on_all_domains_with_spin_work(
  void (*handler)(caml_domain_state*, void*, int, caml_domain_state**), void* data,
  void (*leader_setup)(caml_domain_state*),
  void (*enter_spin_callback)(caml_domain_state*, void*), void* enter_spin_data)
{
#ifdef DEBUG
  caml_domain_state* domain_state = Caml_state;
#endif
  int i;
  uintnat domains_participating = 0;

  caml_gc_log("requesting STW");

  // Don't take the lock if there's already a stw leader
  if (atomic_load_acq(&stw_leader)) {
    caml_handle_incoming_interrupts();
    return 0;
  }

  /* Try to take the lock by setting ourselves as the stw_leader.
     If it fails, handle interrupts (probably participating in
     an STW section) and return. */
  caml_plat_lock(&all_domains_lock);
  if (atomic_load_acq(&stw_leader)) {
    caml_plat_unlock(&all_domains_lock);
    caml_handle_incoming_interrupts();
    return 0;
  } else {
    atomic_store_rel(&stw_leader, (uintnat)domain_self);
  }
  caml_plat_unlock(&all_domains_lock);

  CAML_EV_BEGIN(EV_STW_LEADER);
  caml_gc_log("causing STW");

  /* setup all fields for this stw_request, must have those needed
     for domains waiting at the enter spin barrier */
  stw_request.enter_spin_callback = enter_spin_callback;
  stw_request.enter_spin_data = enter_spin_data;
  stw_request.callback = handler;
  stw_request.data = data;
  atomic_store_rel(&stw_request.barrier, 0);
  atomic_store_rel(&stw_request.domains_still_running, 1);

  if( leader_setup ) {
    leader_setup(domain_self->state);
  }

  /* Next, interrupt all domains, counting how many domains received
     the interrupt (i.e. are not terminated and are participating in
     this STW section). */
  {
    caml_domain_state** participating = stw_request.participating;

    for (i = 0; i < Max_domains; i++) {
      if (&all_domains[i] == domain_self) {
        participating[domains_participating] = domain_self->state;
        Assert(!domain_self->interruptor.interrupt_pending);
        domains_participating++;
        continue;
      }
      if (caml_send_interrupt(&all_domains[i].interruptor)) {
        participating[domains_participating] = all_domains[i].state;
        domains_participating++;
      }
    }

    Assert(domains_participating > 0);

    /* setup the domain_participating fields */
    stw_request.num_domains = domains_participating;
    atomic_store_rel(&stw_request.num_domains_still_processing,
                     domains_participating);

    for(i = 0; i < domains_participating ; i++) {
      int id = participating[i]->id;
      caml_wait_interrupt_serviced(&all_domains[id].interruptor);
    }
  }

  /* release from the enter barrier */
  atomic_store_rel(&stw_request.domains_still_running, 0);

  #ifdef DEBUG
  domain_state->inside_stw_handler = 1;
  #endif
  handler(domain_self->state, data, domains_participating, stw_request.participating);
  #ifdef DEBUG
  domain_state->inside_stw_handler = 0;
  #endif

  decrement_stw_domains_still_processing();

  CAML_EV_END(EV_STW_LEADER);

  return 1;
}

int caml_try_run_on_all_domains(void (*handler)(caml_domain_state*, void*, int, caml_domain_state**), void* data, void (*leader_setup)(caml_domain_state*))
{
  return caml_try_run_on_all_domains_with_spin_work(handler, data, leader_setup, 0, 0);
}

void caml_interrupt_self() {
  interrupt_domain(&domain_self->interruptor);
}

/* Arrange for a major GC slice to be performed on the current domain
   as soon as possible */
void caml_request_major_slice (void)
{
  Caml_state->requested_major_slice = 1;
  caml_interrupt_self();
}

/* Arrange for a minor GC to be performed on the current domain
   as soon as possible */
void caml_request_minor_gc (void)
{
  Caml_state->requested_minor_gc = 1;
  caml_interrupt_self();
}

static void caml_poll_gc_work()
{
  CAMLalloc_point_here;

  if (((uintnat)Caml_state->young_ptr - Bhsize_wosize(Max_young_wosize) <
       (uintnat)Caml_state->young_start) ||
      Caml_state->requested_minor_gc) {
    /* out of minor heap or collection forced */
    CAML_EV_BEGIN(EV_MINOR);
    Caml_state->requested_minor_gc = 0;
    caml_empty_minor_heaps_once();
    CAML_EV_END(EV_MINOR);

    /* FIXME: a domain will only ever call finalizers if its minor
      heap triggers the minor collection
      Care may be needed with finalizers running when the domain
      is waiting in a blocking section and serviced by the backup
      thread.
      */
    CAML_EV_BEGIN(EV_MINOR_FINALIZED);
    caml_final_do_calls();
    CAML_EV_END(EV_MINOR_FINALIZED);
  }

  if (Caml_state->requested_major_slice) {
    CAML_EV_BEGIN(EV_MAJOR);
    Caml_state->requested_major_slice = 0;
    caml_major_collection_slice(AUTO_TRIGGERED_MAJOR_SLICE);
    CAML_EV_END(EV_MAJOR);
  }
}

void caml_handle_gc_interrupt()
{
  atomic_uintnat* young_limit = domain_self->interruptor.interrupt_word;
  CAMLalloc_point_here;

  CAML_EV_BEGIN(EV_INTERRUPT_GC);
  if (atomic_load_acq(young_limit) == INTERRUPT_MAGIC) {
    /* interrupt */
    CAML_EV_BEGIN(EV_INTERRUPT_REMOTE);
    while (atomic_load_acq(young_limit) == INTERRUPT_MAGIC) {
      uintnat i = INTERRUPT_MAGIC;
      atomic_compare_exchange_strong(young_limit, &i, (uintnat)Caml_state->young_start);
    }
    caml_handle_incoming_interrupts();
    CAML_EV_END(EV_INTERRUPT_REMOTE);
  }

  caml_poll_gc_work();
  CAML_EV_END(EV_INTERRUPT_GC);
}

CAMLexport int caml_bt_is_in_blocking_section(void)
{
  dom_internal* self = domain_self;
  uintnat status = atomic_load_acq(&self->backup_thread_msg);
  if (status == BT_IN_BLOCKING_SECTION)
    return 1;
  else
    return 0;

}

CAMLexport intnat caml_domain_is_multicore ()
{
  dom_internal *self = domain_self;
  return (!caml_domain_alone() || self->backup_thread_running);
}

CAMLexport void caml_acquire_domain_lock(void)
{
  dom_internal* self = domain_self;
  caml_plat_lock(&self->domain_lock);
  return;
}

CAMLexport void caml_bt_enter_ocaml(void)
{
  dom_internal* self = domain_self;

  Assert(caml_domain_alone() || self->backup_thread_running);

  if (self->backup_thread_running) {
    atomic_store_rel(&self->backup_thread_msg, BT_ENTERING_OCAML);
  }

  return;
}

CAMLexport void caml_release_domain_lock(void)
{
  dom_internal* self = domain_self;
  caml_plat_unlock(&self->domain_lock);
  return;
}

CAMLexport void caml_bt_exit_ocaml(void)
{
  dom_internal* self = domain_self;

  Assert(caml_domain_alone() || self->backup_thread_running);

  if (self->backup_thread_running) {
    atomic_store_rel(&self->backup_thread_msg, BT_IN_BLOCKING_SECTION);
    /* Wakeup backup thread if it is sleeping */
    caml_plat_signal(&self->domain_cond);
  }


  return;
}

static void caml_enter_blocking_section_default(void)
{
  caml_bt_exit_ocaml();
  caml_release_domain_lock();
  return;
}

static void caml_leave_blocking_section_default(void)
{
  caml_bt_enter_ocaml();
  caml_acquire_domain_lock();
  return;
}


CAMLexport void (*caml_enter_blocking_section_hook)(void) =
   caml_enter_blocking_section_default;
CAMLexport void (*caml_leave_blocking_section_hook)(void) =
   caml_leave_blocking_section_default;

CAMLexport void caml_leave_blocking_section() {
  caml_leave_blocking_section_hook();
  caml_process_pending_signals();
}

CAMLexport void caml_enter_blocking_section() {

  caml_process_pending_signals();
  caml_enter_blocking_section_hook();
}

/* default handler for unix_fork, will be called by unix_fork. */
static void caml_atfork_default(void) {
  caml_reset_domain_lock();
  caml_acquire_domain_lock();
}

CAMLexport void (*caml_atfork_hook)(void) = caml_atfork_default;

void caml_print_stats () {
  struct gc_stats s;

  caml_gc_stat(Val_unit);
  caml_sample_gc_stats(&s);
  fprintf(stderr,"**** GC stats ****\n");
  fprintf(stderr, "Minor words:\t\t%"ARCH_INTNAT_PRINTF_FORMAT"u\n",
    (uintnat)s.minor_words);
  fprintf(stderr, "Promoted words:\t\t%"ARCH_INTNAT_PRINTF_FORMAT"u\n",
    (uintnat)s.promoted_words);
  fprintf(stderr, "Major words:\t\t%"ARCH_INTNAT_PRINTF_FORMAT"u\n",
    (uintnat)s.major_words);
  fprintf(stderr, "Minor collections:\t%"ARCH_INTNAT_PRINTF_FORMAT"u\n",
    (uintnat)s.minor_collections);
  fprintf(stderr, "Major collections:\t%"ARCH_INTNAT_PRINTF_FORMAT"u\n",
    Caml_state->stat_major_collections);
}

static void handover_ephemerons(caml_domain_state* domain_state)
{
  if (domain_state->ephe_info->todo == 0 &&
      domain_state->ephe_info->live == 0)
    return;

  caml_add_to_orphaned_ephe_list(domain_state->ephe_info);
  if (domain_state->ephe_info->todo != 0) {
    caml_ephe_todo_list_emptied();
  }
  domain_state->ephe_info->live = 0;
  domain_state->ephe_info->todo = 0;
}

static void handover_finalisers(caml_domain_state* domain_state)
{
  struct caml_final_info* f = domain_state->final_info;

  if (f->todo_head != NULL || f->first.size != 0 || f->last.size != 0) {
    /* have some final structures */
    if (caml_gc_phase != Phase_sweep_and_mark_main) {
      /* Force a major GC to simplify constraints for
      * handing over ephemerons. */
      caml_gc_major(Val_unit);
    }
    caml_add_orphaned_finalisers (f);
    /* Create a dummy final info */
    domain_state->final_info = caml_alloc_final_info();
  }
  caml_final_domain_terminate(domain_state);
}

int caml_domain_is_terminating ()
{
  struct interruptor* s = &domain_self->interruptor;
  return s->terminating;
}

static void domain_terminate()
{
  caml_domain_state* domain_state = domain_self->state;
  struct interruptor* s = &domain_self->interruptor;
  int finished = 0;

  caml_gc_log("Domain terminating");
  s->terminating = 1;

  while (!finished) {
    caml_orphan_allocated_words();
    caml_finish_sweeping();

    caml_empty_minor_heaps_once();

    caml_finish_marking();
    handover_ephemerons(domain_state);
    handover_finalisers(domain_state);

    caml_plat_lock(&s->lock);

    /* The interaction of termination and major GC is quite subtle.
     *
     * At the end of the major GC, we decide the number of domains to mark and
     * sweep for the next cycle. If the following [handle_incoming] participates
     * in a major GC cycle, then we need to finish marking and sweeping again in
     * order to decrement the globals [num_domains_to_mark] and
     * [num_domains_to_sweep] (see major_gc.c). Luckily, if the following
     * [handle_incoming] does participate in a major GC cycle, then
     * [Caml_state->sweeping_done] will be set to 0 making conditional check to
     * fail, which forces this domain to finish marking and sweeping again.
     */

    if (!caml_incoming_interrupts_queued() &&
        Caml_state->marking_done &&
        Caml_state->sweeping_done) {

      finished = 1;
      s->terminating = 0;
      s->running = 0;
      s->unique_id += Max_domains;

      /* signal the interruptor condition variable
       * because the backup thread may be waiting on it
       */
      caml_plat_broadcast(&s->cond);
      Assert (domain_self->backup_thread_running);
      domain_self->backup_thread_running = 0;
    }
    caml_plat_unlock(&s->lock);
  }
  caml_sample_gc_collect(domain_state);
  caml_remove_generational_global_root(&domain_state->unique_token_root);
  caml_remove_generational_global_root(&domain_state->dls_root);
  caml_remove_generational_global_root(&domain_state->backtrace_last_exn);

  caml_stat_free(domain_state->final_info);
  // run the domain termination hook
  caml_domain_stop_hook();
  caml_stat_free(domain_state->ephe_info);
  caml_teardown_major_gc();
  CAML_EVENTLOG_TEARDOWN();
  caml_teardown_shared_heap(domain_state->shared_heap);
  domain_state->shared_heap = 0;
  caml_free_minor_tables(domain_state->minor_tables);
  domain_state->minor_tables = 0;
  caml_free_signal_stack();

  if(domain_state->current_stack != NULL) {
    caml_free_stack(domain_state->current_stack);
  }

  /* we shouldn't have any unserviced interrupts pending */
  Assert(!domain_self->interruptor.interrupt_pending);

  atomic_store_rel(&domain_self->backup_thread_msg, BT_TERMINATE);
  caml_plat_signal(&domain_self->domain_cond);
  caml_plat_unlock(&domain_self->domain_lock);

  caml_plat_assert_all_locks_unlocked();
  /* This is the last thing we do because we need to be able to rely
     on caml_domain_alone (which uses caml_num_domains_running) in at least
     the shared_heap lockfree fast paths */
  atomic_fetch_add(&caml_num_domains_running, -1);
}

CAMLprim int64_t caml_ml_domain_ticks_unboxed(value unused)
{
  return caml_time_counter() - startup_timestamp;
}

CAMLprim value caml_ml_domain_ticks(value unused)
{
  return caml_copy_int64(caml_ml_domain_ticks_unboxed(unused));
}

CAMLprim value caml_ml_domain_cpu_relax(value t)
{
  struct interruptor* self = &domain_self->interruptor;
  handle_incoming_otherwise_relax (self);
  return Val_unit;
}

CAMLprim value caml_domain_dls_set(value t)
{
  CAMLnoalloc;
  caml_modify_generational_global_root(&Caml_state->dls_root, t);
  return Val_unit;
}

CAMLprim value caml_domain_dls_get(value unused)
{
  CAMLnoalloc;
  return Caml_state->dls_root;
}
