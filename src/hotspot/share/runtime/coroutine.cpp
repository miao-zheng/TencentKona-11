/*
 * Copyright 2001-2010 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 */

#include "precompiled.hpp"
#if INCLUDE_KONA_FIBER
#include "runtime/coroutine.hpp"
#include "runtime/execution_unit.hpp"
#ifdef TARGET_ARCH_x86
# include "vmreg_x86.inline.hpp"
#endif
#ifdef TARGET_ARCH_aarch64
# include "vmreg_aarch64.inline.hpp"
#endif
#include "services/threadService.hpp"
#include "gc/cms/concurrentMarkSweepThread.hpp"
#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/parallel/pcTasks.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "oops/method.inline.hpp"
#if INCLUDE_ZGC
#include "gc/z/zBarrierSet.inline.hpp"
#endif

JavaThread* Coroutine::_main_thread = NULL;
Method* Coroutine::_continuation_start = NULL;
Coroutine::ConcCoroStage Coroutine::_conc_stage = Coroutine::_Uninitialized;
int Coroutine::_conc_claim_parity = 1;

ContBucket* ContContainer::_buckets= NULL;

Mutex* ContReservedStack::_lock = NULL;
GrowableArray<address>* ContReservedStack::free_array = NULL;
ContPreMappedStack* ContReservedStack::current_pre_mapped_stack = NULL;
uintx ContReservedStack::stack_size = 0;
int ContReservedStack::free_array_uncommit_index = 0;
Method* Coroutine::_try_compensate_method = NULL;
Method* Coroutine::_update_active_count_method = NULL;

JavaThreadState Coroutine::update_thread_state(Thread *Self, JavaThreadState new_jts) {
  JavaThreadState old_jts = ((JavaThread *)Self)->thread_state();
  ThreadStateTransition::transition((JavaThread *)Self, old_jts, new_jts);

  return old_jts;
}

void Coroutine::call_forkjoinpool_method(Thread* Self, Method* target_method, JavaCallArguments* args, JavaValue* result) {
  JavaThreadState saved_jts = update_thread_state(Self, _thread_in_vm);

  JavaCalls::call(result, methodHandle(target_method), args, (JavaThread *)Self);

  update_thread_state(Self, saved_jts);
}

int Coroutine::try_compensate(Thread* Self) {
  if (!YieldWithMonitor || _try_compensate_method == NULL) {
    return -1;
  }

  JavaCallArguments args; // No arguments
  JavaValue result(T_INT);
  call_forkjoinpool_method(Self, _try_compensate_method, &args, &result);

  return result.get_jint();
}

void Coroutine::update_active_count(Thread* Self) {
  if (!YieldWithMonitor || _update_active_count_method == NULL) {
    return;
  }

  JavaCallArguments args; // No arguments
  JavaValue result(T_VOID);

  call_forkjoinpool_method(Self, _update_active_count_method, &args, &result);
}

void Coroutine::init_forkjoinpool_method(Method** init_method, Symbol* method_name, Symbol* signature) {
  guarantee(*init_method == NULL, "java call method already initialized");

  CallInfo callinfo;
  LinkInfo link_info(SystemDictionary::java_util_concurrent_ForkJoinPool_klass(), method_name, signature);
  LinkResolver::resolve_static_call(callinfo, link_info, true, Thread::current());
  methodHandle method = callinfo.selected_method();
  assert(method.not_null(), "should have thrown exception");

  *init_method = method();
  guarantee(*init_method != NULL, "java call method not resolveds");
}

void ContReservedStack::init() {
  _lock = new Mutex(Mutex::leaf, "InitializedStack", false, Monitor::_safepoint_check_never);

  free_array = new (ResourceObj::C_HEAP, mtCoroutine)GrowableArray<address>(CONT_RESERVED_PHYSICAL_MEM_MAX, true);
  stack_size = align_up(DefaultCoroutineStackSize +
               JavaThread::stack_shadow_zone_size() +
               JavaThread::stack_reserved_zone_size() +
               JavaThread::stack_yellow_zone_size() +
               JavaThread::stack_red_zone_size()
               , os::vm_page_size());
}

bool ContReservedStack::add_pre_mapped_stack() {
  uintx alloc_real_stack_size = stack_size * CONT_PREMAPPED_STACK_NUM;
  uintx reserved_size = align_up(alloc_real_stack_size, os::vm_allocation_granularity());

  ContPreMappedStack* node = new ContPreMappedStack(reserved_size, current_pre_mapped_stack);
  if (node == NULL) {
    return false;
  }

  if (!node->initialize_virtual_space(alloc_real_stack_size)) {
    delete node;
    return false;
  }

  current_pre_mapped_stack = node;
  MemTracker::record_virtual_memory_type((address)node->get_base_address() - reserved_size, mtCoroutineStack);
  return true;
}

void ContReservedStack::insert_stack(address node) {
  MutexLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
  free_array->append(node);

  /*if (free_array->length() - free_array_uncommit_index > CONT_RESERVED_PHYSICAL_MEM_MAX) {
    address target = free_array->at(free_array_uncommit_index);
    os::free_heap_physical_memory((char *)(target - ContReservedStack::stack_size), ContReservedStack::stack_size);
    free_array_uncommit_index++;
  }*/
}

address ContReservedStack::get_stack_from_free_array() {
  MutexLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
  if (free_array->is_empty()) {
    return NULL;
  }

  address stack_base = free_array->pop();
  //if (free_array->length() <= free_array_uncommit_index) {
    /* The node which is ahead of uncommit index has no physical memory */
  //  free_array_uncommit_index = free_array->length();
  //}
  return stack_base;
}

bool ContReservedStack::pre_mapped_stack_is_full() {
  if (current_pre_mapped_stack->allocated_num >= CONT_PREMAPPED_STACK_NUM) {
    return true;
  }

  return false;
}

address ContReservedStack::acquire_stack() {
  address result = current_pre_mapped_stack->get_base_address() - current_pre_mapped_stack->allocated_num * stack_size;
  current_pre_mapped_stack->allocated_num++;

  return result;
}

address ContReservedStack::get_stack_from_pre_mapped() {
  address stack_base;
  {
    MutexLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
    if ((current_pre_mapped_stack == NULL) || pre_mapped_stack_is_full()) {
      if (!add_pre_mapped_stack()) {
        return NULL;
      }
    }

    stack_base = acquire_stack();
  }

  /* guard reserved, yellow page and red page of virtual space */
  if (os::uses_stack_guard_pages()) {
    address low_addr = stack_base - ContReservedStack::stack_size;
    size_t len = JavaThread::stack_guard_zone_size();
    assert(is_aligned(low_addr, os::vm_page_size()), "Stack base should be the start of a page");
    assert(is_aligned(len, os::vm_page_size()), "Stack size should be a multiple of page size");

    if (os::guard_memory((char *) low_addr, len)) {
      //_stack_guard_state = stack_guard_enabled;
    } else {
      log_warning(os, thread)("Attempt to protect stack guard pages failed ("
        PTR_FORMAT "-" PTR_FORMAT ").", p2i(low_addr), p2i(low_addr + len));
      if (os::uncommit_memory((char *) low_addr, len)) {
        log_warning(os, thread)("Attempt to deallocate stack guard pages failed.");
      }
    }
  }
  return stack_base;
}

address ContReservedStack::get_stack() {
  address stack_base = ContReservedStack::get_stack_from_free_array();
  if (stack_base != NULL) {
    return stack_base;
  }

  return ContReservedStack::get_stack_from_pre_mapped();
}

bool ContPreMappedStack::initialize_virtual_space(intptr_t real_stack_size) {
  if (!_virtual_space.initialize(_reserved_space, real_stack_size)) {
    _reserved_space.release();
    return false;
  }

  return true;
}

ContBucket::ContBucket() : _lock(Mutex::leaf + 1, "ContBucket", false, Monitor::_safepoint_check_never) {
  _head = NULL;
  _count = 0;

  // This initial value ==> never claimed.
  _oops_do_parity = 0;
}

void ContBucket::insert(Coroutine* cont) {
  cont->insert_into_list(_head);
  _count++;
}

void ContBucket::remove(Coroutine* cont) {
  cont->remove_from_list(_head);
  _count--;
  assert(_count >= 0, "illegal count");
}

// GC Support
bool ContBucket::claim_oops_do_par_case(int strong_roots_parity) {
  jint cont_bucket_parity = _oops_do_parity;
  if (cont_bucket_parity != strong_roots_parity) {
    jint res = Atomic::cmpxchg((jint)strong_roots_parity, &_oops_do_parity, cont_bucket_parity);
    if (res == cont_bucket_parity) {
      return true;
    } else {
      guarantee(res == strong_roots_parity, "Or else what?");
    }
  }
  return false;
}

// Used by ParallelScavenge
void ContBucket::create_cont_bucket_roots_tasks(GCTaskQueue* q) {
  for (size_t i = 0; i < CONT_CONTAINER_SIZE; i++) {
    q->enqueue(new ContBucketRootsTask((int)i));
  }
}

// Used by Parallel Old
void ContBucket::create_cont_bucket_roots_marking_tasks(GCTaskQueue* q) {
  for (size_t i = 0; i < CONT_CONTAINER_SIZE; i++) {
    q->enqueue(new ContBucketRootsMarkingTask((int)i));
  }
}

#define ALL_BUCKET_CONTS(OPR)      \
  {                                \
    Coroutine* head = _head;       \
    if (head != NULL) {            \
      Coroutine* current = head;   \
      do {                         \
        current->OPR;              \
        current = current->next(); \
      } while (current != head);   \
    }                              \
  }

void ContBucket::frames_do(void f(frame*, const RegisterMap*)) {
  ALL_BUCKET_CONTS(frames_do(f));
}

void ContBucket::oops_do(OopClosure* f, CodeBlobClosure* cf) {
  ALL_BUCKET_CONTS(oops_do(f, cf));
}

void ContBucket::nmethods_do(CodeBlobClosure* cf) {
  ALL_BUCKET_CONTS(nmethods_do(cf));
}

void ContBucket::metadata_do(void f(Metadata*)) {
  ALL_BUCKET_CONTS(metadata_do(f));
}

void ContBucket::print_stack_on(outputStream* st) {
  ALL_BUCKET_CONTS(print_stack_on(st));
}

void ContContainer::init() {
  assert(is_power_of_2(CONT_CONTAINER_SIZE), "Must be a power of two");
  _buckets = new ContBucket[CONT_CONTAINER_SIZE];
}

ContBucket* ContContainer::bucket(size_t i) {
  return &(_buckets[i]);
}

size_t ContContainer::hash_code(Coroutine* cont) {
  return ((uintptr_t)cont >> CONT_MASK_SHIFT) & CONT_MASK;
}

void ContContainer::insert(Coroutine* cont) {
  size_t index = hash_code(cont);
  guarantee(index < CONT_CONTAINER_SIZE, "Must in the range from 0 to CONT_CONTAINER_SIZE - 1");
  {
    ContBucket* bucket = ContContainer::bucket(index);
    MutexLockerEx ml(bucket->lock(), Mutex::_no_safepoint_check_flag);
    bucket->insert(cont);
    if (log_is_enabled(Trace, coroutine)) {
      ResourceMark rm;
      Log(coroutine) log;
      log.trace("[insert] cont: %p, index: %d, count : %d", cont, (int)index, bucket->count());
    }
  }
}

void ContContainer::remove(Coroutine* cont) {
  size_t index = hash_code(cont);
  guarantee(index < CONT_CONTAINER_SIZE, "Must in the range from 0 to CONT_CONTAINER_SIZE - 1");
  {
    ContBucket* bucket = ContContainer::bucket(index);
    MutexLockerEx ml(bucket->lock(), Mutex::_no_safepoint_check_flag);
    bucket->remove(cont);
    if (log_is_enabled(Trace, coroutine)) {
      ResourceMark rm;
      Log(coroutine) log;
      log.trace("[remove] cont: %p, index: %d, count : %d", cont, (int)index, bucket->count());
    }
  }
}

#define ALL_BUCKETS_DO(OPR)                                     \
  {                                                             \
    for (size_t i = 0; i < CONT_CONTAINER_SIZE; i++) {          \
      ContBucket* bucket = ContContainer::bucket(i);            \
      MutexLockerEx ml(bucket->lock(), Mutex::_no_safepoint_check_flag); \
      bucket->OPR;                                              \
    }                                                           \
  }

void ContContainer::frames_do(void f(frame*, const RegisterMap*)) {
  ALL_BUCKETS_DO(frames_do(f));
}

void ContContainer::oops_do(OopClosure* f, CodeBlobClosure* cf) {
  ALL_BUCKETS_DO(oops_do(f, cf));
}

void ContContainer::nmethods_do(CodeBlobClosure* cf) {
  ALL_BUCKETS_DO(nmethods_do(cf));
}

void ContContainer::metadata_do(void f(Metadata*)) {
  ALL_BUCKETS_DO(metadata_do(f));
}

void ContContainer::print_stack_on(outputStream* st) {
  ALL_BUCKETS_DO(print_stack_on(st));
}

void Coroutine::add_stack_frame(void* frames, int* depth, javaVFrame* jvf) {
  StackFrameInfo* frame = new StackFrameInfo(jvf, false);
  ((GrowableArray<StackFrameInfo*>*)frames)->append(frame);
  (*depth)++;
}

#if defined(LINUX) || defined(_ALLBSD_SOURCE) || defined(_WINDOWS)
void coroutine_start(void* dummy, const void* coroutineObjAddr) {
#if !defined(AMD64) && !defined(AARCH64)
  fatal("Corotuine not supported on current platform");
#endif
  JavaThread* thread = JavaThread::current();
  thread->set_thread_state(_thread_in_vm);
  // passing raw object address form stub to C method
  // normally oop is OopDesc*, can use raw object directly
  // in fastdebug mode, oop is "class oop", raw object addrss is stored in class oop structure
#ifdef CHECK_UNHANDLED_OOPS
  oop coroutineObj = oop(coroutineObjAddr);
#else
  oop coroutineObj = (oop)coroutineObjAddr;
#endif
  JavaCalls::call_continuation_start(coroutineObj, thread);
  ShouldNotReachHere();
}
#endif

void Coroutine::TerminateCoroutine(Coroutine* coro) {
  JavaThread* thread = coro->thread();
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(coroutine) log;
    log.trace("[Co]: TerminateCoroutine %p in thread %s(%p)", coro, coro->thread()->name(), coro->thread());
  }
  guarantee(thread == JavaThread::current(), "thread not match");

  {
    ContContainer::remove(coro);
    if (thread->coroutine_cache_size() < MaxFreeCoroutinesCacheSize) {
      coro->insert_into_list(thread->coroutine_cache());
      thread->coroutine_cache_size() ++;
    } else {
      delete coro;
    }
  }
}

void Coroutine::TerminateCoroutineObj(jobject coroutine) {
  oop old_oop = JNIHandles::resolve(coroutine);
  Coroutine* coro = (Coroutine*)java_lang_Continuation::data(old_oop);
  assert(coro != NULL, "NULL old coroutine in switchToAndTerminate");
  java_lang_Continuation::set_data(old_oop, 0);
  if (!coro->is_thread_coroutine()) {
    coro->_continuation = NULL;
  }
  TerminateCoroutine(coro);
}

void Coroutine::Initialize() {
  guarantee(_continuation_start == NULL, "continuation start already initialized");
  Klass* klass = SystemDictionary::continuation_klass();
  Symbol* method_name = vmSymbols::cont_start_method_name();
  Symbol* signature = vmSymbols::void_method_signature();
  LinkInfo link_info(klass, method_name, signature);
  methodHandle method = LinkResolver::linktime_resolve_virtual_method_or_null(link_info);
  _continuation_start = method();
  guarantee(_continuation_start != NULL, "continuation start not resolveds");

  if (YieldWithMonitor) {
    init_forkjoinpool_method(&_try_compensate_method, 
      vmSymbols::tryCompensate_name(), vmSymbols::void_int_signature());
    init_forkjoinpool_method(&_update_active_count_method,
      vmSymbols::updateActiveCount_name(), vmSymbols::void_method_signature());
  }
}

void Coroutine::start_concurrent(ConcCoroStage stage) {
  guarantee(SafepointSynchronize::is_at_safepoint(), "should be at safepoint");
  assert(_conc_claim_parity >= 1 && _conc_claim_parity <= 2, "unexpected _conc_claim_parity");
  _conc_stage = stage;
  _conc_claim_parity++;
  if (_conc_claim_parity == 3) {
    _conc_claim_parity = 1;
  }
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(gc) log;
    log.trace("[Coroutine::start_concurrent] stage %d, parity %d", stage, _conc_claim_parity);
  }
}

void Coroutine::end_concurrent() {
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(gc) log;
    log.trace("[Coroutine::end_concurrent]");
  }
}

class ConcCoroutineCodeBlobClosure : public CodeBlobToOopClosure {
private:
  BarrierSetNMethod* _bs;

public:
  ConcCoroutineCodeBlobClosure(OopClosure* cl) :
    CodeBlobToOopClosure(cl, true /* fix_relocations */),
    _bs(BarrierSet::barrier_set()->barrier_set_nmethod()) {}

  virtual void do_code_blob(CodeBlob* cb) {
    nmethod* const nm = cb->as_nmethod_or_null();
    if (nm != NULL) {
      _bs->nmethod_entry_barrier(nm);
    }
  }
};

void Coroutine::concurrent_task_run(OopClosure* f, int* claim) {
  ConcCoroutineCodeBlobClosure cf(f);
  while (true) {
    int res = Atomic::add(1, claim);
    int cur = res - 1;
    if (cur >= (int)CONT_CONTAINER_SIZE) {
      break;
    }
    ContBucket* bucket = ContContainer::bucket(cur);
    MutexLockerEx ml(bucket->lock(), Mutex::_no_safepoint_check_flag);
    Coroutine* head = bucket->head();
    if (head == NULL) {
      continue;
    }
    Coroutine* current = head;
    do {
      if (current->conc_claim(true)) {
        ResourceMark rm;
        current->oops_do(f, ClassUnloading ? &cf : NULL);
        bool res = current->conc_claim(false);
        guarantee(res == true, "must success release");
      }
      current = current->next();
    } while (current != head);
  }
}

void Coroutine::cont_metadata_do(void f(Metadata*)) {
  if (_continuation_start != NULL) {
    f(_continuation_start);
  }
}

Coroutine::Coroutine() {
  _has_javacall = false;
  _continuation = NULL;
#if defined(_WINDOWS)
  _guaranteed_stack_bytes = 0;
#endif
#ifdef CHECK_UNHANDLED_OOPS
  _t = NULL;
#endif
}

Coroutine::~Coroutine() {
  if (_verify_state != NULL) {
    delete _verify_state; 
  } else {
    assert(VerifyCoroutineStateOnYield == false || _is_thread_coroutine,
      "VerifyCoroutineStateOnYield is on and _verify_state is NULL");
  }
  free_stack();
}

#if INCLUDE_ZGC
class ZConcurrentCoroutineRootsClosure : public OopClosure {
public:
  virtual void do_oop(oop* p) {
    ZBarrier::load_barrier_on_oop_field((volatile oop*)p);
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }
};

void Z_Coroutine_Concurrent_Do(Coroutine* coro) {
  ZConcurrentCoroutineRootsClosure cl;
  ConcCoroutineCodeBlobClosure code_cl(&cl);
  coro->oops_do(&cl, ClassUnloading ? &code_cl : NULL);
}

void Coroutine::Concurrent_Coroutine_slowpath(Coroutine* coro) {
  guarantee(!SafepointSynchronize::is_at_safepoint(), "should not at safepoint");
  int claim_index;
  bool can_claim = true;
  while ((claim_index = coro->_coro_claim) != _conc_claim_parity) {
    if (claim_index == 4) {
      // GC thread processing
      // adaptive counter and sleep?
      os::naked_yield();
      can_claim = false;
      continue;
    }
    assert(can_claim == true, "can_claim false means processing by GC thread");
    if(coro->conc_claim(false)) {
      if (_conc_stage == _ZConcurrent) {
        ResourceMark rm;
        Z_Coroutine_Concurrent_Do(coro);
      } else {
        guarantee(false, "unexpected stage");
      }
      return;
    }
  }
}
#endif

// check yield is from thread contianuation or to thread contianuation
// check resource is not still hold by contianuation when yield back to thread contianuation
void Coroutine::yield_verify(Coroutine* from, Coroutine* to, bool terminate) {
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(coroutine) log;
    log.trace("yield_verify from %p to %p", from, to);
  }
  JavaThread* thread = from->_thread;
  guarantee(thread->reserved_stack_activation() == thread->stack_base(), "reserved overflow should have been processed");
  // check before change, check is need when yield from none thread to others
  if (!from->is_thread_coroutine()) {
    // switch to other corotuine, compare status
    JNIHandleBlock* jni_handle_block = thread->active_handles();
    guarantee(from->_verify_state->saved_active_handles == jni_handle_block, "must same handle");
    guarantee(from->_verify_state->saved_active_handle_count == jni_handle_block->get_number_of_live_handles(), "must same count");
    guarantee(thread->monitor_chunks() == NULL, "not empty _monitor_chunks");
    if (terminate) {
      assert(thread->java_call_counter() == 1, "must be 1 when terminate");
    }
    if (from->_verify_state->saved_handle_area_hwm != thread->handle_area()->hwm()) {
      tty->print_cr("%p failed %p, %p", from, from->_verify_state->saved_handle_area_hwm, thread->handle_area()->hwm());
      guarantee(false, "handle area leak");
    }
    if (from->_verify_state->saved_resource_area_hwm != thread->resource_area()->hwm()) {
      tty->print_cr("%p failed %p, %p", from, from->_verify_state->saved_resource_area_hwm, thread->resource_area()->hwm());
      guarantee(false, "resource area leak");
    }
  }
  // save context if yield to none thread coroutine
  if (!to->is_thread_coroutine()) {
    // save snapshot
    guarantee(terminate == false, "switch from kernel to continnuation");
    JNIHandleBlock* jni_handle_block = thread->active_handles();
    to->_verify_state->saved_active_handles = jni_handle_block;
    to->_verify_state->saved_active_handle_count = jni_handle_block->get_number_of_live_handles();
    to->_verify_state->saved_resource_area_hwm = thread->resource_area()->hwm();
    to->_verify_state->saved_handle_area_hwm = thread->handle_area()->hwm();
  }
#if defined(_WINDOWS)
  guarantee(from->get_guaranteed_stack_bytes() == 0, "guaranteed stack bytes of old coroutine should be zero");
  guarantee(to->get_guaranteed_stack_bytes() == 0, "guaranteed stack bytes of target coroutine should be zero");
#endif
}

void Coroutine::print_stack_on(void* frames, int* depth) {
  if (!has_javacall() || state() != Coroutine::_onstack) {
    return;
  }
  print_stack_on(NULL, frames, depth);
}

void Coroutine::print_stack_on(outputStream* st) {
  if (!has_javacall()) {
    return;
  }
  if (state() == Coroutine::_onstack) {
    st->cr();
    st->print("   Coroutine: %p", this);
    if (is_thread_coroutine()) {
      st->print_cr("  [thread coroutine]");
    } else {
      print_VT_info(st);
      st->cr();
    }
    print_stack_on(st, NULL, NULL);
  }
}

Coroutine* Coroutine::create_thread_coroutine(JavaThread* thread) {
  Coroutine* coro = new Coroutine();
  coro->_state = _current;
  coro->_verify_state = NULL;
  coro->_is_thread_coroutine = true;
  coro->_thread = thread;
  coro->_coro_claim = _conc_claim_parity;
  coro->init_thread_stack(thread);
  coro->_has_javacall = true;
  coro->_t = thread;
#ifdef ASSERT
  coro->_java_call_counter = 0;
#endif
#if defined(_WINDOWS)
  coro->_last_SEH = NULL;
#endif
  ContContainer::insert(coro);
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(coroutine) log;
    log.trace("[Co]: CreateThreadCoroutine %p in thread %s(%p)", coro, thread->name(), thread);
  }
  return coro;
}

void Coroutine::reset_coroutine(Coroutine* coro) {
  coro->_has_javacall = false;
}

void Coroutine::init_coroutine(Coroutine* coro, JavaThread* thread) {
  intptr_t** d = (intptr_t**)coro->_stack_base;
  // 7 is async profiler's lookup slots count, avoid cross stack
  // boundary when using async profiler
  // must be odd number to keep frame pointer align to 16 bytes.
  for (int32_t i = 0; i < 7; i++) {
    *(--d) = NULL;
  }
#if defined TARGET_ARCH_aarch64
  // aarch64 pops 2 slots when doing coroutine switch
  // must keep frame pointer align to 16 bytes
  *(--d) = NULL;
#endif
  *(--d) = (intptr_t*)coroutine_start;
  *(--d) = NULL;

  coro->set_last_sp((address) d);

  coro->_state = _onstack;
  coro->_is_thread_coroutine = false;
  coro->_thread = thread;
  coro->_coro_claim = _conc_claim_parity;

#ifdef ASSERT
  coro->_java_call_counter = 0;
#endif
#if defined(_WINDOWS)
  coro->_last_SEH = NULL;
#endif
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(coroutine) log;
    log.trace("[Co]: CreateCoroutine %p in thread %s(%p)", coro, coro->thread()->name(), coro->thread());
  }
}

Coroutine* Coroutine::create_coroutine(JavaThread* thread, long stack_size) {
  assert(stack_size <= 0, "Can not specify stack size by users");

  Coroutine* coro = new Coroutine();
  if (VerifyCoroutineStateOnYield) {
    coro->_verify_state = new CoroutineVerify();
  } else {
    coro->_verify_state = NULL;
  }
  if (!coro->init_stack(thread)) {
    return NULL;
  }

  Coroutine::init_coroutine(coro, thread);
  return coro;
}

void Coroutine::frames_do(FrameClosure* fc) {
  if (_state == Coroutine::_onstack) {
    on_stack_frames_do(fc, _is_thread_coroutine);
  }
}

bool Coroutine::conc_claim(bool gc_thread) {
  int coro_parity = _coro_claim;
  if (coro_parity != _conc_claim_parity) {
    int new_value = gc_thread ? 4 : _conc_claim_parity;
    int res = Atomic::cmpxchg(new_value, &_coro_claim, coro_parity);
    if (res == coro_parity) {
      return true;
    } else {
      if (gc_thread) {
        guarantee(res == _conc_claim_parity, "Or else what?");
      } else {
        guarantee(res == _conc_claim_parity || res == 4, "Or else what?");
      }
    }
  }
  return false;
}

class oops_do_Closure: public FrameClosure {
private:
  OopClosure* _f;
  CodeBlobClosure* _cf;

public:
  oops_do_Closure(OopClosure* f, CodeBlobClosure* cf): _f(f), _cf(cf) { }
  void frames_do(frame* fr, RegisterMap* map) { fr->oops_do(_f, _cf, map); }
};

void Coroutine::oops_do(OopClosure* f, CodeBlobClosure* cf) {
  if (is_thread_coroutine() == false) {
    f->do_oop(&_continuation);
  }
  if(state() != Coroutine::_onstack) {
    //tty->print_cr("oops_do on %p is skipped", this);
    return;
  }
  //tty->print_cr("oops_do on %p is performed", this);
  oops_do_Closure fc(f, cf);
  frames_do(&fc);
}

class nmethods_do_Closure: public FrameClosure {
private:
  CodeBlobClosure* _cf;
public:
  nmethods_do_Closure(CodeBlobClosure* cf): _cf(cf) { }
  void frames_do(frame* fr, RegisterMap* map) { fr->nmethods_do(_cf); }
};

void Coroutine::nmethods_do(CodeBlobClosure* cf) {
  nmethods_do_Closure fc(cf);
  frames_do(&fc);
}

class metadata_do_Closure: public FrameClosure {
private:
  void (*_f)(Metadata*);
public:
  metadata_do_Closure(void f(Metadata*)): _f(f) { }
  void frames_do(frame* fr, RegisterMap* map) { fr->metadata_do(_f); }
};

void Coroutine::metadata_do(void f(Metadata*)) {
	if(state() != Coroutine::_onstack) {
		return;
	}
  metadata_do_Closure fc(f);
  frames_do(&fc);
}

class frames_do_Closure: public FrameClosure {
private:
  void (*_f)(frame*, const RegisterMap*);
public:
  frames_do_Closure(void f(frame*, const RegisterMap*)): _f(f) { }
  void frames_do(frame* fr, RegisterMap* map) { _f(fr, map); }
};

void Coroutine::frames_do(void f(frame*, const RegisterMap* map)) {
  frames_do_Closure fc(f);
  frames_do(&fc);
}

bool Coroutine::is_disposable() {
  return false;
}


ObjectMonitor* Coroutine::current_pending_monitor() {
  // if coroutine is detached(_onstack), it doesn't pend on monitor
  // if coroutine is attached(_current), its pending monitor is thread's pending monitor
  if (_state == _onstack) {
    return NULL;
  } else {
    assert(_state == _current, "unexpected");
    return _thread->current_pending_monitor();
  }
}

oop Coroutine::current_park_blocker() {
  // get continuation_oop->virtualthread_oop->java_lang_Thread::park_blocker(virtualthread_oop)
  if (_is_thread_coroutine) {
    return _t->current_park_blocker();
  }
  if (_continuation == NULL
      || _continuation->klass() != SystemDictionary::VTcontinuation_klass()) {
    return NULL;
  }
  oop vt = java_lang_VTContinuation::VT(_continuation);
  if (vt != NULL &&
      JDK_Version::current().supports_thread_park_blocker()) {
    return java_lang_Thread::park_blocker(vt);
  }
  return NULL;
}

oop Coroutine::threadObj() const {
  if (_is_thread_coroutine) {
    return _t->threadObj();
  } else if (_continuation != NULL) {
    if (_continuation->klass() != SystemDictionary::VTcontinuation_klass()) {
      return NULL;
    }
    oop vt = java_lang_VTContinuation::VT(_continuation);
    return vt;
  }
  return NULL;
}

bool Coroutine::is_attaching_via_jni() const {
    if (_is_thread_coroutine) {
      return _t->is_attaching_via_jni();
    }

    return false;
}

const char* Coroutine::get_thread_name() const {
  if (_is_thread_coroutine) {
    return _t->get_thread_name();
  } else {
    return get_vt_name_string();
  }
}

const char* Coroutine::get_vt_name_string(char* buf, int buflen) const {
  const char* name_str;
  oop vt_obj = threadObj();
  if (vt_obj != NULL) {
    oop name = java_lang_Thread::name(vt_obj);
    assert(name != NULL, "vt must have default name");
    if (buf == NULL) {
      name_str = java_lang_String::as_utf8_string(name);
    } else {
      name_str = java_lang_String::as_utf8_string(name, buf, buflen);
    }
  } else {
    name_str = "unknown_vt";
  }
  assert(name_str != NULL, "unexpected NULL thread name");
  return name_str;
}

bool Coroutine::current_pending_monitor_is_from_java() {
  // pending on monitor, must be thread's _current coroutine
  if (_state == _onstack) {
    return true; // not in jni pending
  } else {
    assert(_state == _current, "unexpected");
    return _thread->current_pending_monitor_is_from_java();
  }
}

Coroutine* Coroutine::owning_coro_from_monitor_owner(address owner) {

  // NULL owner means not locked so we can skip the search
  if (owner == NULL) return NULL;

  {
    size_t i = ContContainer::hash_code((Coroutine*)owner);
    ContBucket* bucket = ContContainer::bucket(i);
    if (bucket->head() != NULL) {
      Coroutine* current = bucket->head();
      do {
        if (owner == (address)current) {
          return current;
        }
        current = current->next();
      } while (current != bucket->head());
    }
  }

  // Cannot assert on lack of success here since this function may be
  // used by code that is trying to report useful problem information
  // like deadlock detection.
  if (UseHeavyMonitors) return NULL;

  Coroutine* the_owner = NULL;
  ExecutionUnitsIterator jti(NULL);
  for (ExecutionType* jt = jti.first(); jt != NULL; jt = jti.next()) {
    if (jt->is_lock_owned(owner)) {
      return jt;
    }
  }
  return NULL;
}

void Coroutine::init_thread_stack(JavaThread* thread) {
  _stack_base = thread->stack_base();
  _stack_size = thread->stack_size();
  _stack_overflow_limit = thread->stack_overflow_limit();
  _shadow_zone_safe_limit = thread->shadow_zone_safe_limit();
  _last_sp = NULL;
}

bool Coroutine::init_stack(JavaThread* thread) {
  _stack_base = ContReservedStack::get_stack();
  if (_stack_base == NULL) {
    return false;
  }
  _stack_size = ContReservedStack::stack_size;
  _stack_overflow_limit = _stack_base - _stack_size + MAX2(JavaThread::stack_guard_zone_size(), JavaThread::stack_shadow_zone_size());
  _shadow_zone_safe_limit = _stack_base - _stack_size + JavaThread::stack_guard_zone_size() + JavaThread::stack_shadow_zone_size();
  _shadow_zone_growth_watermark = _stack_base;
  _shadow_zone_growth_native_watermark = _stack_base;
  _last_sp = NULL;
  return true;
}

void Coroutine::free_stack() {
  if(!is_thread_coroutine()) {
    ContReservedStack::insert_stack(_stack_base);
  }
}

static const char* VirtualThreadStateNames[] = {
  "NEW",
  "STARTED",
  "RUNNABLE",
  "RUNNING",
  "PARKING",
  "PARKED",
  "PINNED"
};

static const char* virtual_thread_get_state_name(int state) {
  if (state >= 0 && state < (int)(sizeof(VirtualThreadStateNames) / sizeof(const char*))) {
    return VirtualThreadStateNames[state];
  } else if (state == 99) {
    return "TERMINATED";
  } else {
    return "ERROR STATE";
  }
}
// dump VirtualThread info if possible
// 1. check if continuation is java/lang/VirtualThread$VTContinuation
// 2. Get VT from continuation
// 3. Print VT name and state
void Coroutine::print_VT_info(outputStream* st) {
  if (is_thread_coroutine()) {
    ResourceMark rm;
    st->print_cr("thread coroutine: %s", _t->get_thread_name());
    return;
  }
  Klass* k = _continuation->klass();
  if (k != SystemDictionary::VTcontinuation_klass()) {
    return;
  }
  oop vt = java_lang_VTContinuation::VT(_continuation);
  guarantee(vt != NULL, "on stack VT is null");
  oop vt_name = java_lang_Thread::name(vt);
  int state = java_lang_VT::state(vt);
  if (vt_name == NULL) {
    st->print("\tVirtualThread => name: null, state %s",
      virtual_thread_get_state_name(state));
  } else {
    ResourceMark rm;
    st->print("\tVirtualThread => name: %s, state %s",
      java_lang_String::as_utf8_string(vt_name),
      virtual_thread_get_state_name(state));
  }
}

#if defined TARGET_ARCH_x86
#define FRAME_POINTER rbp
#elif defined TARGET_ARCH_aarch64
#define FRAME_POINTER rfp
#else
#error "Arch is not supported."
#endif

void Coroutine::print_stack_on(outputStream* st, void* frames, int* depth) {
  if (_last_sp == NULL) return;
  address pc = ((address*)_last_sp)[1];
  if (pc != (address)coroutine_start) {
    intptr_t * fp = ((intptr_t**)_last_sp)[0];
    intptr_t* sp = ((intptr_t*)_last_sp) + 2;

    RegisterMap _reg_map(_thread, true);
    _reg_map.set_location(FRAME_POINTER->as_VMReg(), (address)_last_sp);
    _reg_map.set_include_argument_oops(false);
    frame f(sp, fp, pc);
    vframe* start_vf = NULL;
    for (vframe* vf = vframe::new_vframe(&f, &_reg_map, _thread); vf; vf = vf->sender()) {
      if (vf->is_java_frame()) {
        start_vf = javaVFrame::cast(vf);
        break;
      }
    }
    int count = 0;
    for (vframe* f = start_vf; f; f = f->sender()) {
      if (f->is_java_frame()) {
        javaVFrame* jvf = javaVFrame::cast(f);
        if (st != NULL) {
          java_lang_Throwable::print_stack_element(st, jvf->method(), jvf->bci());

          // Print out lock information
          if (JavaMonitorsInStackTrace) {
            jvf->print_lock_info_on(st, count, this);
          }
        } else {
          add_stack_frame(frames, depth, jvf);
        }
      }
      else {
        // Ignore non-Java frames
      }
      // Bail-out case for too deep stacks
      count++;
      if (MaxJavaStackTraceDepth == count) return;
    }
  }
}

void Coroutine::on_stack_frames_do(FrameClosure* fc, bool isThreadCoroutine) {
  assert(_last_sp != NULL, "CoroutineStack with NULL last_sp");

  // optimization to skip coroutine not started yet, check if return address is coroutine_start
  // fp is only valid for call from interperter, from compiled code fp register is not gurantee valid
  // JIT method utilize sp and oop map for oops iteration.
  address pc = ((address*)_last_sp)[1];
  intptr_t* fp = ((intptr_t**)_last_sp)[0];
  if (pc != (address)coroutine_start) {
    intptr_t* sp = ((intptr_t*)_last_sp) + 2;
    frame fr(sp, fp, pc);
    StackFrameStream fst(_thread, fr);
    fst.register_map()->set_location(FRAME_POINTER->as_VMReg(), (address)_last_sp);
    fst.register_map()->set_include_argument_oops(false);
    for(; !fst.is_done(); fst.next()) {
      fc->frames_do(fst.current(), fst.register_map());
    }
  } else {
    guarantee(!isThreadCoroutine, "thread conrotuine with coroutine_start as return address");
    guarantee(fp == NULL, "conrotuine fp not in init status");
  }
}

JVM_ENTRY(jint, CONT_isPinned0(JNIEnv* env, jclass klass, long data)) {
  JavaThread* thread = JavaThread::thread_from_jni_environment(env);
  if (thread->locksAcquired != 0) {
    return CONT_PIN_MONITOR;
  }
  if (thread->contJniFrames != 0) {
    return CONT_PIN_JNI;
  }
  return 0;
}
JVM_END

JVM_ENTRY(jlong, CONT_createContinuation(JNIEnv* env, jclass klass, long stackSize)) {
  if (stackSize < 0) {
    guarantee(thread->current_coroutine()->is_thread_coroutine(), "current coroutine is not thread coroutine");
    if (log_is_enabled(Trace, coroutine)) {
      ResourceMark rm; 
      Log(coroutine) log;
      log.trace("CONT_createContinuation: reuse main thread continuation %p", thread->current_coroutine());
    }
    return (jlong)thread->current_coroutine();
  }

  // illegal arguments is checked in library side
  // 0 means default stack size
  // -1 means no stack, this is continuation for kernel thread
  // Stack is cached in thread local now, later will be cached in bucket sized list
  // now cache is not cosiderering different size, for example:
  // 1. DefaultCoroutineStackSize is 256K
  // 2. if user allocate stack as 8k and freed, 8K stack will be in cache too and reused as default size
  // This will be solved later with global coroutine cache
  Coroutine* coro = NULL;
  if (stackSize == 0) {
    if (thread->coroutine_cache_size() > 0) {
      coro = thread->coroutine_cache();
      coro->remove_from_list(thread->coroutine_cache());
      thread->coroutine_cache_size()--;
      Coroutine::reset_coroutine(coro);
      Coroutine::init_coroutine(coro, thread);
    }
  }
  if (coro == NULL) {
    coro = Coroutine::create_coroutine(thread, stackSize);
    if (coro == NULL) {
      HandleMark mark(thread);
      THROW_0(vmSymbols::java_lang_OutOfMemoryError());
    }
  }
  ContContainer::insert(coro);
  if (log_is_enabled(Trace, coroutine)) {
    ResourceMark rm;
    Log(coroutine) log;
    log.trace("CONT_createContinuation: create continuation %p", coro);
  }
  return (jlong)coro;
}
JVM_END

JVM_ENTRY(jint, CONT_switchTo(JNIEnv* env, jclass klass, jobject target, jobject current)) {
  ShouldNotReachHere();
  return 0;
}
JVM_END

JVM_ENTRY(void, CONT_switchToAndTerminate(JNIEnv* env, jclass klass, jobject target, jobject current)) {
  Coroutine::TerminateCoroutineObj(current);
}
JVM_END

JVM_ENTRY(jobjectArray, CONT_dumpStackTrace(JNIEnv *env, jclass klass, jobject cont))
  oop contOop = JNIHandles::resolve(cont);
  Coroutine* coro = (Coroutine*)java_lang_Continuation::data(contOop);
  VirtualThreadStackTrace* res = new VirtualThreadStackTrace(coro);
  // if coro is NULL, array is empty
  res->dump_stack();

  Handle stacktraces = res->allocate_fill_stack_trace_element_array(thread);
  return (jobjectArray)JNIHandles::make_local(env, stacktraces());
JVM_END

#define CC (char*)  /*cast a literal from (const char*)*/
#define FN_PTR(f) CAST_FROM_FN_PTR(void*, &f)
#define JLSTR  "Ljava/lang/String;"
#define JLSTE  "Ljava/lang/StackTraceElement;"
#define JLCONT "Ljava/lang/Continuation;"

static JNINativeMethod CONT_methods[] = {
  {CC"isPinned0",                 CC"(J)I", FN_PTR(CONT_isPinned0)},
  {CC"createContinuation",        CC"(J)J", FN_PTR(CONT_createContinuation)},
  {CC"switchTo",                  CC"(" JLCONT JLCONT ")I", FN_PTR(CONT_switchTo)},
  {CC"switchToAndTerminate",      CC"(" JLCONT JLCONT ")V", FN_PTR(CONT_switchToAndTerminate)},
  {CC"dumpStackTrace",            CC"(" JLCONT ")[" JLSTE, FN_PTR(CONT_dumpStackTrace)},
};

static const int switchToIndex = 2;
static const int switchToAndTerminateIndex = 3;

static void initializeForceWrapper(JNIEnv *env, jclass cls, JavaThread* thread, int index) {
  jmethodID id = env->GetStaticMethodID(cls, CONT_methods[index].name, CONT_methods[index].signature);
  {
    ThreadInVMfromNative tivfn(thread);
    methodHandle method(Method::resolve_jmethod_id(id));
    AdapterHandlerLibrary::create_native_wrapper(method);
    // switch method doesn't have real implemenation, when JVMTI is on and JavaThread::interp_only_mode is true
    // it crashes when execute registered native method, as empty or incorrect.
    // set i2i as point to i2c entry, force it execute native wrapper
    method->set_interpreter_entry(method->from_interpreted_entry());
  }
}

void CONT_RegisterNativeMethods(JNIEnv *env, jclass cls, JavaThread* thread) {
    if (UseKonaFiber == false) {
      fatal("UseKonaFiber is off");
    }
    assert(thread->is_Java_thread(), "");
    {
      ThreadToNativeFromVM trans((JavaThread*)thread);
      int status = env->RegisterNatives(cls, CONT_methods, sizeof(CONT_methods)/sizeof(JNINativeMethod));
      guarantee(status == JNI_OK && !env->ExceptionOccurred(), "register java.lang.Continuation natives");
#ifdef ASSERT
      if (FLAG_IS_DEFAULT(VerifyCoroutineStateOnYield)) {
        FLAG_SET_DEFAULT(VerifyCoroutineStateOnYield, true);
      }
#endif
      initializeForceWrapper(env, cls, thread, switchToIndex);
      initializeForceWrapper(env, cls, thread, switchToAndTerminateIndex);
    }
    Coroutine::Initialize();
}
#endif// INCLUDE_KONA_FIBER
