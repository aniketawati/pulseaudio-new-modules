/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <string.h>
#include <pulsecore/core.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/core-util.h>

#include "module-appsurfer-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("When a sink/source is removed, try to move their streams to the default sink/source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    NULL,
};

struct userdata {
    pa_hook_slot
	*new_sink_input_slot,
	*sink_input_move_fail_slot;

};


static pa_hook_result_t new_sink_input_callback(pa_core *c, pa_sink_input *i, void* userdata) {    
    uint32_t idx;
    char *pid;
    char *module_name;
    pa_sink *target;
    pa_module *null_sink;
    pa_assert(c);
    pa_assert(i);

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;
    pid = pa_proplist_gets(i->proplist,PA_PROP_APPLICATION_PROCESS_ID);        
    PA_IDXSET_FOREACH(target, c->sinks, idx) {        

        if(pa_streq(pid,target->name))
        {
            if (pa_sink_input_move_to(i, target, FALSE) < 0)
                pa_log_info("Failed to move sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), target->name);
            else
                pa_log_info("Successfully moved sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), target->name);
            return PA_HOOK_OK;
        }
    }
    module_name = pa_xmalloc(strlen(pid)+11);    
    sprintf(module_name,"sink_name=%s",pid);
    null_sink = pa_module_load(c,'module-null-sink',(const char*) module_name);
    if(null_sink)
    {
        if (pa_sink_input_move_to(i, null_sink, FALSE) < 0)
                pa_log_info("Failed to move sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), null_sink->name);
            else
                pa_log_info("Successfully moved sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), null_sink->name);
            return PA_HOOK_OK;
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_move_fail_hook_callback(pa_core *c, pa_sink_input *i, void *userdata) {
    uint32_t *idx;
    pa_sink *target;
    char *pid;
    pa_assert(c);
    pa_assert(i);

    /* There's no point in doing anything if the core is shut down anyway */
    if (c->state == PA_CORE_SHUTDOWN)
        return PA_HOOK_OK;

    pid = pa_proplist_gets(i->proplist,PA_PROP_APPLICATION_PROCESS_ID);        
    PA_IDXSET_FOREACH(target, c->sinks, idx) {        

        if(pa_streq(pid,target->name))
        {
            if (pa_sink_input_finish_move(i, target, FALSE) < 0) {
                pa_log_info("Failed to move sink input %u \"%s\" to %s.", i->index,
                        pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), target->name);
                return PA_HOOK_OK;

            } else {
                pa_log_info("Successfully moved sink input %u \"%s\" to %s.", i->index,
                    pa_strnull(pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)), target->name);
                return PA_HOOK_STOP;
            }  
        }
    }
}

int pa__init(pa_module*m) {
    pa_modargs *ma;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        return -1;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);

    /* A little bit later than module-stream-restore, module-intended-roles... */
    u->new_sink_input_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW], PA_HOOK_EARLY, (pa_hook_cb_t) new_sink_input_callback, u);

    u->sink_input_move_fail_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FAIL], PA_HOOK_LATE+20, (pa_hook_cb_t) sink_input_move_fail_hook_callback, u);

    pa_modargs_free(ma);
    return 0;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->new_sink_input_slot)
        pa_hook_slot_free(u->new_sink_input_slot);

    if (u->sink_input_move_fail_slot)
        pa_hook_slot_free(u->sink_input_move_fail_slot);

    pa_xfree(u);
}
