/*
 * Copyright (C) 2021, 2023, THL A29 Limited, a Tencent company. All rights reserved.
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
 */

#ifndef SHARE_RUNTIME_EXECUTION_UNIT_HPP
#define SHARE_RUNTIME_EXECUTION_UNIT_HPP

#include "utilities/macros.hpp"
class ThreadsList;
#if INCLUDE_KONA_FIBER
class Coroutine;
typedef Coroutine ExecutionType;
#else
class JavaThread;
typedef JavaThread ExecutionType;
#endif

class ExecutionUnit: AllStatic {
public:
  static ExecutionType* get_execution_unit(oop threadObj);
  static ExecutionType* owning_thread_from_monitor_owner(ThreadsList* t_list, address owner);
};

class ExecutionUnitsIterator {
private:
#if INCLUDE_KONA_FIBER
  ExecutionType* _cur;
  size_t         _cur_bucket_index;
#else
  ThreadsList*   _list;
  uint           _index;
#endif
public:
  ExecutionUnitsIterator(ThreadsList* t_list);
  ExecutionType* first();
  ExecutionType* next();
};
#endif // SHARE_RUNTIME_EXECUTION_UNIT_HPP
