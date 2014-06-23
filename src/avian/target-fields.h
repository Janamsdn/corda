/* Copyright (c) 2008-2014, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#ifndef AVIAN_TARGET_FIELDS_H
#define AVIAN_TARGET_FIELDS_H


#ifdef TARGET_BYTES_PER_WORD
#  if (TARGET_BYTES_PER_WORD == 8)

#define TARGET_THREAD_EXCEPTION 80
#define TARGET_THREAD_EXCEPTIONSTACKADJUSTMENT 2264
#define TARGET_THREAD_EXCEPTIONOFFSET 2272
#define TARGET_THREAD_EXCEPTIONHANDLER 2280

#define TARGET_THREAD_IP 2224
#define TARGET_THREAD_STACK 2232
#define TARGET_THREAD_NEWSTACK 2240
#define TARGET_THREAD_SCRATCH 2248
#define TARGET_THREAD_CONTINUATION 2256
#define TARGET_THREAD_TAILADDRESS 2288
#define TARGET_THREAD_VIRTUALCALLTARGET 2296
#define TARGET_THREAD_VIRTUALCALLINDEX 2304
#define TARGET_THREAD_HEAPIMAGE 2312
#define TARGET_THREAD_CODEIMAGE 2320
#define TARGET_THREAD_THUNKTABLE 2328
#define TARGET_THREAD_STACKLIMIT 2376

#  elif (TARGET_BYTES_PER_WORD == 4)

#define TARGET_THREAD_EXCEPTION 44
#define TARGET_THREAD_EXCEPTIONSTACKADJUSTMENT 2168
#define TARGET_THREAD_EXCEPTIONOFFSET 2172
#define TARGET_THREAD_EXCEPTIONHANDLER 2176

#define TARGET_THREAD_IP 2148
#define TARGET_THREAD_STACK 2152
#define TARGET_THREAD_NEWSTACK 2156
#define TARGET_THREAD_SCRATCH 2160
#define TARGET_THREAD_CONTINUATION 2164
#define TARGET_THREAD_TAILADDRESS 2180
#define TARGET_THREAD_VIRTUALCALLTARGET 2184
#define TARGET_THREAD_VIRTUALCALLINDEX 2188
#define TARGET_THREAD_HEAPIMAGE 2192
#define TARGET_THREAD_CODEIMAGE 2196
#define TARGET_THREAD_THUNKTABLE 2200
#define TARGET_THREAD_STACKLIMIT 2224

#  else
#    error
#  endif
#else
#  error
#endif

#endif

