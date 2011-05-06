/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
* $Id$
*
* Copyright (C) 2011 Thomas Martitz
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
* KIND, either express or implied.
*
****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include "buflib.h"
#include "new_apis.h"

#define BUFLIB_BUFFER_SIZE (10<<10)
static char buflib_buffer[BUFLIB_BUFFER_SIZE];
static struct buflib_context ctx;
#define assert(x) do { if (!(x)) exit(1); } while(0)

int main(int argc, char **argv)
{
	buflib_init(&ctx, buflib_buffer, BUFLIB_BUFFER_SIZE);

    int id = buflib_alloc_ex(&ctx, 512, "foo");
    int id2 = buflib_alloc_ex(&ctx, 1024, "bar");
    int id3 = buflib_alloc_ex(&ctx, 8<<10, "8K");

    assert(id > 0 && id2 > 0 && id3 > 0);

    #define STR "<TEST>"
    strncpy(buflib_get_data(&ctx, id3), STR, sizeof STR);
    if (id > 0)
    {
        buflib_print_allocs(&ctx);
        buflib_free(&ctx, id);
        buflib_print_allocs(&ctx);
        buflib_free(&ctx, id2);
        buflib_print_allocs(&ctx);

        id = buflib_alloc_ex(&ctx, 512, "should compact");
        if (id <= 0) printf("compacting alloc failed");

        buflib_print_allocs(&ctx);

        printf("id I: %p\n", buflib_get_data(&ctx, id3));
        id2 = buflib_alloc_ex(&ctx, 3<<10, "should fail");
        printf("id II: %p\n", buflib_get_data(&ctx, id3));
        if (id2 <= 0) printf("failing alloc failed\n");
        else buflib_free(&ctx, id2);

        if (id > 0)
            buflib_free(&ctx, id);

        printf("Check string: \"%s\"\n", buflib_get_data(&ctx, id3));
        buflib_print_allocs(&ctx);
    }

	return 0;
}
