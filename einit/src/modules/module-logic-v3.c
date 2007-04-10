/*
 *  module-logic-v4.c
 *  einit
 *
 *  Created by Magnus Deininger on 09/04/2007.
 *  Copyright 2006, 2007 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2006, 2007, Magnus Deininger
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
    * Neither the name of the project nor the names of its contributors may be
	  used to endorse or promote products derived from this software without
	  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <einit/config.h>
#include <einit/module-logic.h>
#include <einit/module.h>
#include <einit/tree.h>
#include <pthread.h>
#include <string.h>
#include <einit/bitch.h>
#include <einit-modules/ipc.h>
#include <einit-modules/configuration.h>

#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>
#endif

#define MAX_SPAWNS 10

int _einit_module_logic_v3_configure (struct lmodule *);

#if defined(_EINIT_MODULE) || defined(_EINIT_MODULE_HEADER)
const struct smodule _einit_module_logic_v3_self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = 0,
 .options   = 0,
 .name      = "Module Logic Core (V3)",
 .rid       = "module-logic-v3",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 },
 .configure = _einit_module_logic_v3_configure
};

module_register(_einit_module_logic_v3_self);

#endif

void module_logic_ipc_event_handler (struct einit_event *);
void module_logic_einit_event_handler (struct einit_event *);
double __mod_get_plan_progress_f (struct mloadplan *);
char initdone = 0;
char mod_isbroken (char *service);
char mod_mark (char *service, char task);
struct group_data *mod_group_get_data (char *group);

/* new functions: */
char mod_examine_group (char *);
void mod_examine_module (struct lmodule *);
void mod_examine (char *);

void mod_commit_and_wait (char **, char **);

struct module_taskblock
  current = { NULL, NULL, NULL },
  target_state = { NULL, NULL, NULL };

struct stree *module_logics_service_list = NULL; // value is a (struct lmodule **)
struct stree *module_logics_group_data = NULL;

pthread_mutex_t
  ml_tb_current_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_tb_target_state_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_service_list_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_group_data_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_unresolved_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_currently_provided_mutex = PTHREAD_MUTEX_INITIALIZER,
  ml_service_update_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t
  ml_cond_service_update = PTHREAD_COND_INITIALIZER;

char **unresolved_services = NULL;
char **broken_services = NULL;

struct group_data {
 char **members;
 uint32_t options;
};

#define MOD_PLAN_GROUP_SEQ_ANY     0x00000001
#define MOD_PLAN_GROUP_SEQ_ALL     0x00000002
#define MOD_PLAN_GROUP_SEQ_ANY_IOP 0x00000004
#define MOD_PLAN_GROUP_SEQ_MOST    0x00000008

#define MARK_BROKEN                0x01
#define MARK_UNRESOLVED            0x02

/* module header functions */

int _einit_module_logic_v3_cleanup (struct lmodule *this) {
 function_unregister ("module-logic-get-plan-progress", 1, __mod_get_plan_progress_f);

 event_ignore (EVENT_SUBSYSTEM_EINIT, module_logic_einit_event_handler);
 event_ignore (EVENT_SUBSYSTEM_IPC, module_logic_ipc_event_handler);

 return 0;
}

int _einit_module_logic_v3_configure (struct lmodule *this) {
 module_init(this);

 thismodule->cleanup = _einit_module_logic_v3_cleanup;

 event_listen (EVENT_SUBSYSTEM_IPC, module_logic_ipc_event_handler);
 event_listen (EVENT_SUBSYSTEM_EINIT, module_logic_einit_event_handler);

 function_register ("module-logic-get-plan-progress", 1, __mod_get_plan_progress_f);

 return 0;
}

/* end module header */
/* start common functions with v3 */

char mod_isbroken (char *service) {
 int retval = 0;

 emutex_lock (&ml_unresolved_mutex);

 retval = inset ((const void **)broken_services, (void *)service, SET_TYPE_STRING) ||
   inset ((const void **)unresolved_services, (void *)service, SET_TYPE_STRING);

 emutex_unlock (&ml_unresolved_mutex);

 return retval;
}

struct group_data *mod_group_get_data (char *group) {
 struct group_data *ret = NULL;

 emutex_lock (&ml_group_data_mutex);

 struct stree *cur = module_logics_group_data ? streefind (module_logics_group_data, group, TREE_FIND_FIRST) : NULL;
 if (cur) { ret = (struct group_data *)cur->value; }
 else {
  char *tnodeid = emalloc (strlen (group)+17);
  struct cfgnode *gnode = NULL;

  memcpy (tnodeid, "services-alias-", 16);
  strcat (tnodeid, group);

  ret = ecalloc (1, sizeof (struct group_data));

  if ((gnode = cfg_getnode (tnodeid, NULL)) && gnode->arbattrs) {
   ssize_t r = 0;

   for (r = 0; gnode->arbattrs[r]; r+=2) {
    if (strmatch (gnode->arbattrs[r], "group")) {
     ret->members = str2set (':', gnode->arbattrs[r+1]);
    } else if (strmatch (gnode->arbattrs[r], "seq")) {
     if (strmatch (gnode->arbattrs[r+1], "any"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_ANY;
     else if (strmatch (gnode->arbattrs[r+1], "all"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_ALL;
     else if (strmatch (gnode->arbattrs[r+1], "any-iop"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_ANY_IOP;
     else if (strmatch (gnode->arbattrs[r+1], "most"))
      ret->options |=  MOD_PLAN_GROUP_SEQ_MOST;
    }
   }
  }
  free (tnodeid);

  if (!ret->members || !ret->options) {
   free (ret);
   ret = NULL;
  } else {
   module_logics_group_data = streeadd (module_logics_group_data, group, (void *)ret, SET_NOALLOC, (void *)ret);
  }
 }

 emutex_unlock (&ml_group_data_mutex);

 return ret;
}

void cross_taskblock (struct module_taskblock *source, struct module_taskblock *target) {
 if (source->enable) {
  char **tmp = (char **)setcombine ((const void **)target->enable, (const void **)source->enable, SET_TYPE_STRING);
  if (target->enable) free (target->enable);
  target->enable = tmp;

  tmp = (char **)setslice ((const void **)target->disable, (const void **)source->enable, SET_TYPE_STRING);
  if (target->disable) free (target->disable);
  target->disable = tmp;
 }
 if (source->critical) {
  char **tmp = (char **)setcombine ((const void **)target->critical, (const void **)source->critical, SET_TYPE_STRING);
  if (target->critical) free (target->critical);
  target->critical = tmp;
 }

 if (source->disable) {
  char **tmp = (char **)setcombine ((const void **)target->disable, (const void **)source->disable, SET_TYPE_STRING);
  if (target->disable) free (target->disable);
  target->disable = tmp;

  tmp = (char **)setslice ((const void **)target->enable, (const void **)source->disable, SET_TYPE_STRING);
  if (target->enable) free (target->enable);
  target->enable = tmp;

  tmp = (char **)setslice ((const void **)target->critical, (const void **)source->disable, SET_TYPE_STRING);
  if (target->critical) free (target->critical);
  target->critical = tmp;
 }
}

struct mloadplan *mod_plan (struct mloadplan *plan, char **atoms, unsigned int task, struct cfgnode *mode) {
 char disable_all_but_feedback = 0, disable_all = 0, auto_add_feedback = 0;

 if (!plan) {
  plan = emalloc (sizeof (struct mloadplan));
  memset (plan, 0, sizeof (struct mloadplan));
 }

 if (mode) {
  char **base = NULL;
  uint32_t xi = 0;
  char **enable   = str2set (':', cfg_getstring ("enable/services", mode));
  char **disable  = str2set (':', cfg_getstring ("disable/services", mode));
  char **critical = str2set (':', cfg_getstring ("enable/critical", mode));
  struct cfgnode *node = cfg_getnode ("auto-add-feedback", mode);

  if (node)
   auto_add_feedback = node->flag;

  if (!enable)
   enable  = str2set (':', cfg_getstring ("enable/mod", mode));
  if (!disable)
   disable = str2set (':', cfg_getstring ("disable/mod", mode));

  if (mode->arbattrs) for (; mode->arbattrs[xi]; xi+=2) {
   if (strmatch(mode->arbattrs[xi], "base")) {
    base = str2set (':', mode->arbattrs[xi+1]);
   }
  }

  if (enable) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.enable, (const void **)enable, SET_TYPE_STRING);
   if (plan->changes.enable) free (plan->changes.enable);
   plan->changes.enable = tmp;
  }
  if (disable) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.disable, (const void **)disable, SET_TYPE_STRING);
   if (plan->changes.disable) free (plan->changes.disable);
   plan->changes.disable = tmp;
  }
  if (critical) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.critical, (const void **)critical, SET_TYPE_STRING);
   if (plan->changes.critical) free (plan->changes.critical);
   plan->changes.critical = tmp;
  }

  if (auto_add_feedback) {
   if (plan->changes.enable) {
    uint32_t y = 0;

    for (; plan->changes.enable[y]; y++) {
     struct stree *st = NULL;

     emutex_lock (&ml_service_list_mutex);

     if ((st = streefind (module_logics_service_list, plan->changes.enable[y], TREE_FIND_FIRST))) {
      struct lmodule **lmod = st->value;

      if (lmod[0] && lmod[0]->module && lmod[0]->module->mode & EINIT_MOD_FEEDBACK)
       goto have_feedback;
     }

     emutex_unlock (&ml_service_list_mutex);
    }
   }

   plan->changes.enable = (char **)setadd ((void **)plan->changes.enable, (void *)"feedback", SET_TYPE_STRING);
  }
  have_feedback:

  if (base) {
   int y = 0;
   struct cfgnode *cno;
   while (base[y]) {
    if (!inset ((const void **)plan->used_modes, (void *)base[y], SET_TYPE_STRING)) {
     cno = cfg_findnode (base[y], EI_NODETYPE_MODE, NULL);
     if (cno) {
      plan = mod_plan (plan, NULL, 0, cno);
     }
    }

    y++;
   }

   free (base);
  }

  if (mode->id) {
   plan->used_modes = (char **)setadd ((void **)plan->used_modes, mode->id, SET_TYPE_STRING);
  }

  plan->mode = mode;
 } else {
  if (task & MOD_ENABLE) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.enable, (const void **)atoms, SET_TYPE_STRING);
   if (plan->changes.enable) free (plan->changes.enable);
   plan->changes.enable = tmp;
  }
  if (task & MOD_DISABLE) {
   char **tmp = (char **)setcombine ((const void **)plan->changes.disable, (const void **)atoms, SET_TYPE_STRING);
   if (plan->changes.disable) free (plan->changes.disable);
   plan->changes.disable = tmp;
  }
 }

 disable_all = inset ((const void **)plan->changes.disable, (void *)"all", SET_TYPE_STRING);
 disable_all_but_feedback = inset ((const void **)plan->changes.disable, (void *)"all-but-feedback", SET_TYPE_STRING);

 if (disable_all || disable_all_but_feedback) {
  struct stree *cur;
  ssize_t i = 0;
  char **tmpy = service_usage_query_cr (SERVICE_GET_ALL_PROVIDED, NULL, NULL);

  emutex_lock (&ml_service_list_mutex);
  emutex_lock (&ml_tb_target_state_mutex);
//  char **tmpx = (char **)setcombine ((const void **)plan->changes.enable, (const void **)target_state.enable, SET_TYPE_STRING);
  char **tmpx = (char **)setcombine ((const void **)tmpy, (const void **)target_state.enable, SET_TYPE_STRING);

  emutex_unlock (&ml_tb_target_state_mutex);

  char **tmp = (char **)setcombine ((const void **)tmpx, (const void **)plan->changes.disable, SET_TYPE_STRING);

  free (tmpx);
  free (tmpy);

  if (plan->changes.disable) {
   free (plan->changes.disable);
   plan->changes.disable = NULL;
  }

  if (tmp) {
   for (; tmp[i]; i++) {
    char add = 1;

    if (inset ((const void **)plan->changes.disable, (void *)tmp[i], SET_TYPE_STRING)) {
     add = 0;
    } else if ((disable_all && strmatch(tmp[i], "all")) ||
               (disable_all_but_feedback && strmatch(tmp[i], "all-but-feedback"))) {
     add = 0;
    } else if (module_logics_service_list && (cur = streefind (module_logics_service_list, tmp[i], TREE_FIND_FIRST))) {
     struct lmodule **lm = (struct lmodule **)cur->value;
     if (lm) {
      ssize_t y = 0;
      for (; lm[y]; y++) {
       if (disable_all_but_feedback && (lm[y]->module->mode & EINIT_MOD_FEEDBACK)) {
        add = 0;

        break;
       }
      }
     }
    } else if (!service_usage_query (SERVICE_IS_PROVIDED, NULL, tmp[i])) {
     add = 0;
    }

    if (add) {
     plan->changes.disable = (char **)setadd((void **)plan->changes.disable, (void *)tmp[i], SET_TYPE_STRING);
    }
   }

   free (tmp);
  }

  emutex_unlock (&ml_service_list_mutex);
 }

 return plan;
}

unsigned int mod_plan_commit (struct mloadplan *plan) {
 struct einit_event *fb = evinit (EVE_FEEDBACK_PLAN_STATUS);

 if (!plan) return 0;

// do some extra work if the plan was derived from a mode
 if (plan->mode) {
  char *cmdt;
  cmode = plan->mode;

  struct einit_event eex = evstaticinit (EVE_SWITCHING_MODE);
  eex.para = (void *)plan->mode;
  event_emit (&eex, EINIT_EVENT_FLAG_BROADCAST);
  evstaticdestroy (eex);

  if ((cmdt = cfg_getstring ("before-switch/emit-event", cmode))) {
   struct einit_event ee = evstaticinit (event_string_to_code(cmdt));
   event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
   evstaticdestroy (ee);
  }

  if ((cmdt = cfg_getstring ("before-switch/ipc", cmode))) {
   char **cmdts = str2set (';', cmdt);
   uint32_t in = 0;

   if (cmdts) {
    for (; cmdts[in]; in++)
     ipc_process(cmdts[in], stderr);

    free (cmdts);
   }
  }
 }

 fb->task = MOD_SCHEDULER_PLAN_COMMIT_START;
 fb->para = (void *)plan;
 status_update (fb);

 emutex_lock (&ml_tb_target_state_mutex);

 cross_taskblock (&(plan->changes), &target_state);

 emutex_lock (&ml_tb_current_mutex);

 cross_taskblock (&target_state, &current);

 uint32_t i = 0;

 if (current.enable) {
  char **tmp = NULL;
  for (i = 0; current.enable[i]; i++) {
   if (!service_usage_query (SERVICE_IS_PROVIDED, NULL, current.enable[i])) {
    tmp = (char **)setadd ((void **)tmp, (void *)current.enable[i], SET_TYPE_STRING);
   }
  }
  free (current.enable);
  current.enable = tmp;
 }
 if (current.disable) {
  char **tmp = NULL;
  for (i = 0; current.disable[i]; i++) {
   if (service_usage_query (SERVICE_IS_PROVIDED, NULL, current.disable[i])) {
    tmp = (char **)setadd ((void **)tmp, (void *)current.disable[i], SET_TYPE_STRING);
   }
  }
  free (current.disable);
  current.disable = tmp;
 }

 emutex_unlock (&ml_tb_current_mutex);
 emutex_unlock (&ml_tb_target_state_mutex);

 mod_commit_and_wait (plan->changes.enable, plan->changes.disable);

 fb->task = MOD_SCHEDULER_PLAN_COMMIT_FINISH;
 status_update (fb);

// do some more extra work if the plan was derived from a mode
 if (plan->mode) {
  char *cmdt;
  cmode = plan->mode;
  amode = plan->mode;

  struct einit_event eex = evstaticinit (EVE_MODE_SWITCHED);
  eex.para = (void *)plan->mode;
  event_emit (&eex, EINIT_EVENT_FLAG_BROADCAST);
  evstaticdestroy (eex);

  if (amode->id) {
   struct einit_event eema = evstaticinit (EVE_PLAN_UPDATE);
   eema.string = estrdup(amode->id);
   eema.para   = (void *)amode;
   event_emit (&eema, EINIT_EVENT_FLAG_BROADCAST);
   free (eema.string);
   evstaticdestroy (eema);
  }

  if ((cmdt = cfg_getstring ("after-switch/emit-event", amode))) {
   struct einit_event ee = evstaticinit (event_string_to_code(cmdt));
   event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
   evstaticdestroy (ee);
  }

  if ((cmdt = cfg_getstring ("after-switch/ipc", amode))) {
   char **cmdts = str2set (';', cmdt);
   uint32_t in = 0;

   if (cmdts) {
    for (; cmdts[in]; in++) {
     ipc_process(cmdts[in], stderr);
    }
    free (cmdts);
   }
  }
 }

 evdestroy (fb);

 if (plan->mode) return 0; // always return "OK" if it's based on a mode

 return 0;
}

int mod_plan_free (struct mloadplan *plan) {
 if (plan) {
  if (plan->changes.enable) free (plan->changes.enable);
  if (plan->changes.disable) free (plan->changes.disable);
  if (plan->changes.critical) free (plan->changes.critical);

  if (plan->used_modes) free (plan->used_modes);

  free (plan);
 }
 return 0;
}

double __mod_get_plan_progress_f (struct mloadplan *plan) {
 if (plan) {
  return 0.0;
 } else {
  double all = 0, left = 0;
  emutex_lock (&ml_tb_target_state_mutex);
  if (target_state.enable) all += setcount ((const void **)target_state.enable);
  if (target_state.disable) all += setcount ((const void **)target_state.disable);
  emutex_unlock (&ml_tb_target_state_mutex);

  emutex_lock (&ml_tb_current_mutex);
  if (current.enable) left += setcount ((const void **)current.enable);
  if (current.disable) left += setcount ((const void **)current.disable);
  emutex_unlock (&ml_tb_current_mutex);

  return 1.0 - (double)(left / all);
 }
}

void mod_sort_service_list_items_by_preference() {
 struct stree *cur;

 emutex_lock (&ml_service_list_mutex);

 cur = module_logics_service_list;

 while (cur) {
  struct lmodule **lm = (struct lmodule **)cur->value;

  if (lm) {
   /* order modules that should be enabled according to the user's preference */
   uint32_t mpx, mpy, mpz = 0;
   char *pnode = NULL, **preference = NULL;

   /* first make sure all modules marked as "deprecated" are last */
   for (mpx = 0; lm[mpx]; mpx++); mpx--;
   for (mpy = 0; mpy < mpx; mpy++) {
    if (lm[mpy]->module && (lm[mpy]->module->options & EINIT_MOD_DEPRECATED)) {
     struct lmodule *t = lm[mpx];
     lm[mpx] = lm[mpy];
     lm[mpy] = t;
     mpx--;
    }
   }

   /* now to the sorting bit... */
   pnode = emalloc (strlen (cur->key)+18);
   pnode[0] = 0;
   strcat (pnode, "services-prefer-");
   strcat (pnode, cur->key);

   if ((preference = str2set (':', cfg_getstring (pnode, NULL)))) {
    debugx ("applying module preferences for service %s", cur->key);

    for (mpx = 0; preference[mpx]; mpx++) {
     for (mpy = 0; lm[mpy]; mpy++) {
      if (lm[mpy]->module && lm[mpy]->module->rid && strmatch(lm[mpy]->module->rid, preference[mpx])) {
       struct lmodule *tm = lm[mpy];

       lm[mpy] = lm[mpz];
       lm[mpz] = tm;

       mpz++;
      }
     }
    }
    free (preference);
   }

   free (pnode);
  }

  cur = streenext(cur);
 }

 emutex_unlock (&ml_service_list_mutex);
}

int mod_switchmode (char *mode) {
 if (!mode) return -1;
 struct cfgnode *cur = cfg_findnode (mode, EI_NODETYPE_MODE, NULL);
 struct mloadplan *plan = NULL;

 if (!cur) {
  notice (1, "scheduler: scheduled mode not defined, aborting");
  return -1;
 }

 plan = mod_plan (NULL, NULL, 0, cur);
 if (!plan) {
  notice (1, "scheduler: scheduled mode defined but nothing to be done");
 } else {
  pthread_t th;
  mod_plan_commit (plan);
  /* make it so that the erase operation will not disturb the flow of the program */
  ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_plan_free, (void *)plan);
 }

 return 0;
}

int mod_modaction (char **argv) {
 int argc = setcount ((const void **)argv), ret = 1;
 int32_t task = 0;
 struct mloadplan *plan;
 if (!argv || (argc != 2)) return -1;

 if (strmatch (argv[1], "enable")) task = MOD_ENABLE;
 else if (strmatch (argv[1], "disable")) task = MOD_DISABLE;
 else {
  struct lmodule **tm = NULL;
  uint32_t r = 0;

  emutex_lock (&ml_service_list_mutex);
  if (module_logics_service_list) {
   struct stree *cur = streefind (module_logics_service_list, argv[0], TREE_FIND_FIRST);
   if (cur) {
    tm = cur->value;
   }
  }

  emutex_unlock (&ml_service_list_mutex);

  ret = 1;
  if (tm) {
   if (strmatch (argv[1], "status")) {
    for (; tm[r]; r++) {
     if (tm[r]->status & STATUS_WORKING) {
      ret = 2;
      break;
     }
     if (tm[r]->status & STATUS_ENABLED) {
      ret = 0;
      break;
     }
    }
   } else {
    for (; tm[r]; r++) {
     int retx = mod (MOD_CUSTOM, tm[r], argv[1]);

     if (retx == STATUS_OK)
      ret = 0;
    }
   }
  }

  return ret;
 }

 argv[1] = NULL;

 if ((plan = mod_plan (NULL, argv, task, NULL))) {
  pthread_t th;

  ret = mod_plan_commit (plan);

  ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_plan_free, (void *)plan);
 }

// free (argv[0]);
// free (argv);

 return ret;
}

void module_logic_einit_event_handler(struct einit_event *ev) {
 if ((ev->type == EVE_UPDATE_CONFIGURATION) && !initdone) {
  initdone = 1;

  function_register("module-logic-get-plan-progress", 1, (void (*)(void *))__mod_get_plan_progress_f);
 } else if (ev->type == EVE_MODULE_LIST_UPDATE) {
  /* update list with services */
  struct stree *new_service_list = NULL;
  struct lmodule *cur = ev->para;

  emutex_lock (&ml_service_list_mutex);

  while (cur) {
   if (cur->si && cur->si->provides) {
    ssize_t i = 0;

    for (; cur->si->provides[i]; i++) {
     struct stree *slnode = new_service_list ?
       streefind (new_service_list, cur->si->provides[i], TREE_FIND_FIRST) :
       NULL;
     struct lnode **curval = (struct lnode **) (slnode ? slnode->value : NULL);

     curval = (struct lnode **)setadd ((void **)curval, cur, SET_NOALLOC);

     if (slnode) {
      slnode->value = curval;
      slnode->luggage = curval;
     } else {
      new_service_list = streeadd (new_service_list, cur->si->provides[i], (void *)curval, SET_NOALLOC, (void *)curval);
     }
    }
   }
   cur = cur->next;
  }

  if (module_logics_service_list) streefree (module_logics_service_list);
  module_logics_service_list = new_service_list;

  emutex_unlock (&ml_service_list_mutex);

  mod_sort_service_list_items_by_preference();

  emutex_lock (&ml_unresolved_mutex);

  if (unresolved_services) {
   free (unresolved_services);
   unresolved_services = NULL;
  }
  if (broken_services) {
   free (broken_services);
   broken_services = NULL;
  }

  emutex_unlock (&ml_unresolved_mutex);
  emutex_lock (&ml_group_data_mutex);

  if (module_logics_group_data) {
   streefree (module_logics_group_data);
   module_logics_group_data = NULL;
  }

  emutex_unlock (&ml_group_data_mutex);
 } else if ((ev->type == EVE_SERVICE_UPDATE) && (!(ev->status & STATUS_WORKING))) {
/* something's done now, update our lists */
  mod_examine_module ((struct lmodule *)ev->para);
 } else switch (ev->type) {
  case EVE_SWITCH_MODE:
   if (!ev->string) return;
   else {
    if (ev->para) {
     struct einit_event ee = evstaticinit(EVENT_FEEDBACK_REGISTER_FD);
     ee.para = ev->para;
     event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
     evstaticdestroy(ee);
    }

    mod_switchmode (ev->string);

    if (ev->para) {
     struct einit_event ee = evstaticinit(EVENT_FEEDBACK_UNREGISTER_FD);
     ee.para = ev->para;
     event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
     evstaticdestroy(ee);
    }
   }
   return;
  case EVE_CHANGE_SERVICE_STATUS:
   if (!ev->set) return;
   else {
    if (ev->para) {
     struct einit_event ee = evstaticinit(EVENT_FEEDBACK_REGISTER_FD);
     ee.para = ev->para;
     event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
     evstaticdestroy(ee);
    }

    if (mod_modaction ((char **)ev->set)) {
     ev->integer = 1;
    }

    if (ev->para) {
     struct einit_event ee = evstaticinit(EVENT_FEEDBACK_UNREGISTER_FD);
     ee.para = ev->para;
     event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
     evstaticdestroy(ee);
    }
   }
   return;
 }
}

void module_logic_ipc_event_handler (struct einit_event *ev) {
 if (ev->set && ev->set[0] && ev->set[1] && ev->para) {
  if (strmatch (ev->set[0], "examine") && strmatch (ev->set[1], "configuration")) {
   struct cfgnode *cfgn = cfg_findnode ("mode-enable", 0, NULL);
   char **modes = NULL;

   while (cfgn) {
    if (cfgn->arbattrs && cfgn->mode && cfgn->mode->id && (!modes || !inset ((const void **)modes, (const void *)cfgn->mode->id, SET_TYPE_STRING))) {
     uint32_t i = 0;
     modes = (char **)setadd ((void **)modes, (void *)cfgn->mode->id, SET_TYPE_STRING);

     for (i = 0; cfgn->arbattrs[i]; i+=2) {
      if (strmatch(cfgn->arbattrs[i], "services")) {
       char **tmps = str2set (':', cfgn->arbattrs[i+1]);

       if (tmps) {
        uint32_t i = 0;

        emutex_lock(&ml_service_list_mutex);

        for (; tmps[i]; i++) {
         if (!streefind (module_logics_service_list, tmps[i], TREE_FIND_FIRST) && !mod_group_get_data(tmps[i])) {
          eprintf ((FILE *)ev->para, " * mode \"%s\": service \"%s\" referenced but not found\n", cfgn->mode->id, tmps[i]);
          ev->task++;
         }
        }

        emutex_unlock(&ml_service_list_mutex);

        free (tmps);
       }
       break;
      }
     }
    }

    cfgn = cfg_findnode ("mode-enable", 0, cfgn);
   }

   ev->flag = 1;
  } else if (strmatch (ev->set[0], "list")) {
   if (strmatch (ev->set[1], "services")) {
    struct stree *modes = NULL;
    struct stree *cur = NULL;
    struct cfgnode *cfgn = cfg_findnode ("mode-enable", 0, NULL);

    while (cfgn) {
     if (cfgn->arbattrs && cfgn->mode && cfgn->mode->id && (!modes || !streefind (modes, cfgn->mode->id, TREE_FIND_FIRST))) {
      uint32_t i = 0;
      for (i = 0; cfgn->arbattrs[i]; i+=2) {
       if (strmatch(cfgn->arbattrs[i], "services")) {
        char **tmps = str2set (':', cfgn->arbattrs[i+1]);

        modes = streeadd (modes, cfgn->mode->id, tmps, SET_NOALLOC, tmps);

        break;
       }
      }
     }

     cfgn = cfg_findnode ("mode-enable", 0, cfgn);
    }

    emutex_lock(&ml_service_list_mutex);

    cur = module_logics_service_list;

    while (cur) {
     char **inmodes = NULL;
     struct stree *mcur = modes;

     while (mcur) {
      if (inset ((const void **)mcur->value, (void *)cur->key, SET_TYPE_STRING)) {
       inmodes = (char **)setadd((void **)inmodes, (void *)mcur->key, SET_TYPE_STRING);
      }

      mcur = streenext(mcur);
     }

     if (inmodes) {
      char *modestr;
      if (ev->status & EIPC_OUTPUT_XML) {
       modestr = set2str (':', (const char **)inmodes);
       eprintf ((FILE *)ev->para, " <service id=\"%s\" used-in=\"%s\">\n", cur->key, modestr);
      } else {
       modestr = set2str (' ', (const char **)inmodes);
       eprintf ((FILE *)ev->para, (ev->status & EIPC_OUTPUT_ANSI) ?
                                "\e[1mservice \"%s\" (%s)\n\e[0m" :
                                  "service \"%s\" (%s)\n",
                                cur->key, modestr);
      }
      free (modestr);
      free (inmodes);
     } else if (!(ev->status & EIPC_ONLY_RELEVANT)) {
      if (ev->status & EIPC_OUTPUT_XML) {
       eprintf ((FILE *)ev->para, " <service id=\"%s\">\n", cur->key);
      } else {
       eprintf ((FILE *)ev->para, (ev->status & EIPC_OUTPUT_ANSI) ?
                                "\e[1mservice \"%s\" (not in any mode)\e[0m\n" :
                                  "service \"%s\" (not in any mode)\n",
                                cur->key);
      }
     }

     if (inmodes || (!(ev->status & EIPC_ONLY_RELEVANT))) {
      if (ev->status & EIPC_OUTPUT_XML) {
       if (cur->value) {
        struct lmodule **xs = cur->value;
        uint32_t u = 0;
        for (u = 0; xs[u]; u++) {
         eprintf ((FILE *)ev->para, "  <module id=\"%s\" name=\"%s\" />\n",
                   xs[u]->module && xs[u]->module->rid ? xs[u]->module->rid : "unknown",
                   xs[u]->module && xs[u]->module->name ? xs[u]->module->name : "unknown");
        }
       }

       eputs (" </service>\n", (FILE*)ev->para);
      } else {
       if (cur->value) {
        struct lmodule **xs = cur->value;
        uint32_t u = 0;
        for (u = 0; xs[u]; u++) {
         eprintf ((FILE *)ev->para, (ev->status & EIPC_OUTPUT_ANSI) ?
           ((xs[u]->module && (xs[u]->module->options & EINIT_MOD_DEPRECATED)) ?
                                  " \e[31m- \e[0mcandidate \"%s\" (%s)\n" :
                                  " \e[33m* \e[0mcandidate \"%s\" (%s)\n") :
             " * candidate \"%s\" (%s)\n",
           xs[u]->module && xs[u]->module->rid ? xs[u]->module->rid : "unknown",
           xs[u]->module && xs[u]->module->name ? xs[u]->module->name : "unknown");
        }
       }
      }
     }

     cur = streenext (cur);
    }

    emutex_unlock(&ml_service_list_mutex);

    ev->flag = 1;
   }
#ifdef DEBUG
   else if (strmatch (ev->set[1], "control-blocks")) {
    emutex_lock (&ml_tb_target_state_mutex);

    if (target_state.enable) {
     char *r = set2str (' ', (const char **)target_state.enable);
     if (r) {
      eprintf ((FILE *)ev->para, "target_state.enable = { %s }\n", r);
      free (r);
     }
    }
    if (target_state.disable) {
     char *r = set2str (' ', (const char **)target_state.disable);
     if (r) {
      eprintf ((FILE *)ev->para, "target_state.disable = { %s }\n", r);
      free (r);
     }
    }
    if (target_state.critical) {
     char *r = set2str (' ', (const char **)target_state.critical);
     if (r) {
      eprintf ((FILE *)ev->para, "target_state.critical = { %s }\n", r);
      free (r);
     }
    }

    emutex_unlock (&ml_tb_target_state_mutex);
    emutex_lock (&ml_tb_current_mutex);

    if (current.enable) {
     char *r = set2str (' ', (const char **)current.enable);
     if (r) {
      eprintf ((FILE *)ev->para, "current.enable = { %s }\n", r);
      free (r);
     }
    }
    if (current.disable) {
     char *r = set2str (' ', (const char **)current.disable);
     if (r) {
      eprintf ((FILE *)ev->para, "current.disable = { %s }\n", r);
      free (r);
     }
    }
    if (current.critical) {
     char *r = set2str (' ', (const char **)current.critical);
     if (r) {
      eprintf ((FILE *)ev->para, "current.critical = { %s }\n", r);
      free (r);
     }
    }

    emutex_unlock (&ml_tb_current_mutex);

    ev->flag = 1;
   }
#endif
  }
 }
}

/* end common functions */

/* start new functions */

int mod_gettask (char * service);

pthread_mutex_t
 ml_examine_mutex = PTHREAD_MUTEX_INITIALIZER,
 ml_chain_examine = PTHREAD_MUTEX_INITIALIZER,
 ml_workthreads_mutex = PTHREAD_MUTEX_INITIALIZER,
 ml_commits_mutex = PTHREAD_MUTEX_INITIALIZER;

struct stree *module_logics_chain_examine = NULL; // value is a (char **)
struct stree *module_logics_chain_examine_reverse = NULL;
char **currently_provided;
signed char mod_flatten_current_tb_group(char *serv, char task);

uint32_t ml_workthreads = 0;
uint32_t ml_commits = 0;

void mod_workthreads_dec () {
 emutex_lock (&ml_workthreads_mutex);
 ml_workthreads--;
 if (!ml_workthreads) {
  emutex_unlock (&ml_workthreads_mutex);

#ifdef _POSIX_PRIORITY_SCHEDULING
  sched_yield();
#endif

  pthread_cond_broadcast (&ml_cond_service_update);
 } else {
  emutex_unlock (&ml_workthreads_mutex);
 }
}

void mod_workthreads_inc () {
 emutex_lock (&ml_commits_mutex);
 ml_workthreads++;
 emutex_unlock (&ml_commits_mutex);
}

void mod_commits_dec () {
 char clean_broken = 0;
 emutex_lock (&ml_unresolved_mutex);
 if (broken_services) {
  struct einit_event ee = evstaticinit(EVENT_FEEDBACK_BROKEN_SERVICES);
  ee.set = (void **)broken_services;

  event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
  evstaticdestroy (ee);

  free (broken_services);
  broken_services = NULL;
 }
 if (unresolved_services) {
  struct einit_event ee = evstaticinit(EVENT_FEEDBACK_UNRESOLVED_SERVICES);
  ee.set = (void **)unresolved_services;

  event_emit (&ee, EINIT_EVENT_FLAG_BROADCAST);
  evstaticdestroy (ee);

  free (unresolved_services);
  unresolved_services = NULL;
 }
 emutex_unlock (&ml_unresolved_mutex);

 emutex_lock (&ml_commits_mutex);
 ml_commits--;
 clean_broken = (ml_commits == 0);
 emutex_unlock (&ml_commits_mutex);

 if (clean_broken) {
  emutex_lock (&ml_unresolved_mutex);
  if (unresolved_services) {
   free (unresolved_services);
   unresolved_services = NULL;
  }
  if (broken_services) {
   free (broken_services);
   broken_services = NULL;
  }
  emutex_unlock (&ml_unresolved_mutex);
 }
}

void mod_commits_inc () {
 emutex_lock (&ml_workthreads_mutex);
 ml_commits++;
 emutex_unlock (&ml_workthreads_mutex);
}

void mod_defer_until (char *service, char *after) {
 struct stree *xn = NULL;
 emutex_lock(&ml_chain_examine);

 if ((xn = streefind (module_logics_chain_examine, after, TREE_FIND_FIRST))) {
  if (!inset ((const void **)xn->value, service, SET_TYPE_STRING)) {
   char **n = (char **)setadd ((void **)xn->value, service, SET_TYPE_STRING);

   xn->value = (void *)n;
   xn->luggage = (void *)n;
  }
 } else {
  char **n = (char **)setadd ((void **)NULL, service, SET_TYPE_STRING);

  module_logics_chain_examine =
   streeadd(module_logics_chain_examine, after, n, SET_NOALLOC, n);
 }

 if ((xn = streefind (module_logics_chain_examine_reverse, service, TREE_FIND_FIRST))) {
  if (!inset ((const void **)xn->value, after, SET_TYPE_STRING)) {
   char **n = (char **)setadd ((void **)xn->value, after, SET_TYPE_STRING);

   xn->value = (void *)n;
   xn->luggage = (void *)n;
  }
 } else {
  char **n = (char **)setadd ((void **)NULL, after, SET_TYPE_STRING);

  module_logics_chain_examine_reverse =
   streeadd(module_logics_chain_examine_reverse, service, n, SET_NOALLOC, n);
 }

 emutex_unlock(&ml_chain_examine);

#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);
}

void mod_remove_defer (char *service) {
 struct stree *xn = NULL;
 emutex_lock(&ml_chain_examine);

 if ((xn = streefind (module_logics_chain_examine_reverse, service, TREE_FIND_FIRST))) {
  uint32_t i = 0;

  if (xn->value) {
   for (; ((char **)xn->value)[i]; i++) {
    struct stree *yn = streefind (module_logics_chain_examine, ((char **)xn->value)[i], TREE_FIND_FIRST);

    if (yn) {
     yn->value = (void *)strsetdel ((char **)yn->value, ((char **)xn->value)[i]);

     if (!yn->value) {
      module_logics_chain_examine = streedel (yn);
     } else {
      yn->luggage = yn->value;
     }
    }
   }
  }

  module_logics_chain_examine_reverse = streedel (xn);
 }

 emutex_unlock(&ml_chain_examine);
}

void mod_decrease_deferred_by (char *service) {
 struct stree *xn = NULL;
 char **do_examine = NULL;

 emutex_lock(&ml_chain_examine);

 if ((xn = streefind (module_logics_chain_examine, service, TREE_FIND_FIRST))) {
  uint32_t i = 0;

  if (xn->value) {
   for (; ((char **)xn->value)[i]; i++) {
    struct stree *yn = streefind (module_logics_chain_examine_reverse, ((char **)xn->value)[i], TREE_FIND_FIRST);

    if (yn) {
     yn->value = (void *)strsetdel ((char **)yn->value, ((char **)xn->value)[i]);
     yn->luggage = yn->value;

     if (!yn->value) {
      do_examine = (char **)setadd ((void **)do_examine, yn->key, SET_TYPE_STRING);

      module_logics_chain_examine_reverse = streedel (yn);
     }
    }
   }
  }

  module_logics_chain_examine = streedel (xn);
 }

 emutex_unlock(&ml_chain_examine);

 if (do_examine) {
  uint32_t i = 0;

  for (; do_examine[i]; i++) {
   mod_examine (do_examine[i]);
  }
  free (do_examine);
 }
}

char mod_isdeferred (char *service) {
 char ret = 0;

 emutex_lock(&ml_chain_examine);

 struct stree *r =
  streefind (module_logics_chain_examine_reverse, service, TREE_FIND_FIRST);

#if 0
 if (r) {
  char **deferrees = r->value;
  uint32_t i = 0;

  for (; deferrees[i]; i++) {
   eprintf (stderr, " -- %s: deferred by %s\n", service, deferrees[i]);
  }
 }
#endif

 emutex_unlock(&ml_chain_examine);

 ret = r != NULL;

 return ret;
}

char mod_mark (char *service, char task) {
 char retval = 0;

 emutex_lock (&ml_unresolved_mutex);

 if ((task & MARK_BROKEN) && !inset ((const void **)broken_services, (void *)service, SET_TYPE_STRING)) {
  broken_services = (char **)setadd ((void **)broken_services, (void *)service, SET_TYPE_STRING);
 }
 if ((task & MARK_UNRESOLVED) && !inset ((const void **)unresolved_services, (void *)service, SET_TYPE_STRING)) {
  unresolved_services = (char **)setadd ((void **)unresolved_services, (void *)service, SET_TYPE_STRING);
 }

 emutex_unlock (&ml_unresolved_mutex);

 mod_remove_defer (service);

#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);

 return retval;
}

char mod_isprovided(char *service) {
 char ret = 0;

 emutex_lock (&ml_currently_provided_mutex);

 ret = inset ((const void **)currently_provided, (const void *)service, SET_TYPE_STRING);

 emutex_unlock (&ml_currently_provided_mutex);

 return ret;
}

void mod_queue_enable (char *service) {
 emutex_lock (&ml_tb_current_mutex);

 current.enable = (char **)setadd ((void **)current.enable, (const void *)service, SET_TYPE_STRING);
 current.disable = strsetdel ((char **)current.disable, (void *)service);

 emutex_unlock (&ml_tb_current_mutex);

 mod_examine (service);
}

void mod_queue_disable (char *service) {
 emutex_lock (&ml_tb_current_mutex);

 current.disable = (char **)setadd ((void **)current.disable, (const void *)service, SET_TYPE_STRING);
 current.enable = strsetdel ((char **)current.enable, (void *)service);

 emutex_unlock (&ml_tb_current_mutex);

 mod_examine (service);
}

signed char mod_flatten_current_tb_group(char *serv, char task) {
 struct group_data *gd = mod_group_get_data (serv);

 if (gd) {
  uint32_t changes = 0;
  char *service = estrdup (serv);

  if (!gd->members || !gd->members[0])
   return -1;

  if (gd->options & (MOD_PLAN_GROUP_SEQ_ANY | MOD_PLAN_GROUP_SEQ_ANY_IOP)) {
   uint32_t i = 0;

   for (; gd->members[i]; i++) {
    if (((task & MOD_ENABLE) && mod_isprovided (gd->members[i])) ||
          ((task & MOD_DISABLE) && !mod_isprovided (gd->members[i]))) {
     free (service);
     return 0;
    }

    if (mod_isbroken (gd->members[i])) {
     continue;
    }

    if (!inset ((const void **)(task & MOD_ENABLE ? current.enable : current.disable), gd->members[i], SET_TYPE_STRING)) {
     changes++;

     if (task & MOD_ENABLE) {
      current.enable = (char **)setadd ((void **)current.enable, (const void *)gd->members[i], SET_TYPE_STRING);
     } else {
      current.disable = (char **)setadd ((void **)current.disable, (const void *)gd->members[i], SET_TYPE_STRING);
     }

     mod_defer_until(service, gd->members[i]);

     free (service);
     return 1;
    }
   }

   mod_mark (service, MARK_BROKEN);
  } else { // MOD_PLAN_GROUP_SEQ_ALL | MOD_PLAN_GROUP_SEQ_MOST
   uint32_t i = 0, bc = 0;

   for (; gd->members[i]; i++) {
    if (((task & MOD_ENABLE) && mod_isprovided (gd->members[i])) ||
        ((task & MOD_DISABLE) && !mod_isprovided (gd->members[i]))) {
//     eprintf (stderr, "%s: skipping %s (already in proper state)\n", service, gd->members[i]);

     continue;
    }

    if (mod_isbroken (gd->members[i])) {
//     eprintf (stderr, "%s: skipping %s (broken)\n", service, gd->members[i]);

     bc++;
     continue;
    }

    if (!inset ((const void **)(task & MOD_ENABLE ? current.enable : current.disable), gd->members[i], SET_TYPE_STRING)) {
     changes++;

//     eprintf (stderr, "%s: deferring after %s\n", service, gd->members[i]);

     mod_defer_until(service, gd->members[i]);

     if (task & MOD_ENABLE) {
      current.enable = (char **)setadd ((void **)current.enable, (const void *)gd->members[i], SET_TYPE_STRING);
     } else {
      current.disable = (char **)setadd ((void **)current.disable, (const void *)gd->members[i], SET_TYPE_STRING);
     }
    }
   }

   if (bc) {
    if (bc == i) {
     notice (5, "group %s broken!\n", service);

     mod_mark (service, MARK_BROKEN);
    } else if (gd->options & MOD_PLAN_GROUP_SEQ_ALL) {
     notice (5, "group %s broken!\n", service);

     mod_mark (service, MARK_BROKEN);
    }
   }
  }

  free (service);
  return changes != 0;
 }

 return -1;
}

signed char mod_flatten_current_tb_module(char *serv, char task) {
 emutex_lock (&ml_service_list_mutex);
 struct stree *xn = streefind (module_logics_service_list, serv, TREE_FIND_FIRST);

 if (xn && xn->value) {
  struct lmodule **lm = xn->value;
  uint32_t changes = 0;
  char *service = estrdup (serv);

  if (task & MOD_ENABLE) {
   struct lmodule *first = lm[0];
   char broken = 0;

   do {
    struct lmodule *rcurrent = lm[0];
    char rotate = 0;
    broken = 0;

    first = 0;
    if (rcurrent && rcurrent->si && rcurrent->si->requires) {
     uint32_t i = 0;

     for (; rcurrent->si->requires[i]; i++) {
      if (mod_isprovided (rcurrent->si->requires[i])) {
       continue;
      }

      if (mod_isbroken (rcurrent->si->requires[i])) {
       rotate = 1;
       broken = 1;

       break;
      }

      if (!inset ((const void **)current.enable, (const void *)rcurrent->si->requires[i], SET_TYPE_STRING)) {
       current.enable = (char **)setadd ((void **)current.enable, (const void *)rcurrent->si->requires[i], SET_TYPE_STRING);

       changes++;
      }

      broken = 0;
     }
    }

    if (rotate && lm[1]) {
     ssize_t rx = 1;

     for (; lm[rx]; rx++) {
      lm[rx-1] = lm[rx];
     }

     lm[rx-1] = rcurrent;
    }
   } while (broken && (lm[0] != first));

   if (broken) {
    mod_mark (service, MARK_BROKEN);
   }
  } else { /* disable... */
   uint32_t z = 0;

   for (; lm[z]; z++) {
    char **t = service_usage_query_cr (SERVICE_GET_SERVICES_THAT_USE, lm[z], NULL);

    if (t) {
     uint32_t i = 0;

     for (; t[i]; i++) {
      if (!mod_isprovided (t[i]) || mod_isbroken (t[i])) {
       continue;
      }

      if (!inset ((const void **)current.disable, (const void *)t[i], SET_TYPE_STRING)) {
       current.disable = (char **)setadd ((void **)current.disable, (const void *)t[i], SET_TYPE_STRING);

       changes++;
      }
     }
    }
   }
  }

  emutex_unlock (&ml_service_list_mutex);

  free (service);

  return changes != 0;
 }

 emutex_unlock (&ml_service_list_mutex);

 return -1;
}

void mod_flatten_current_tb () {
 emutex_lock (&ml_tb_current_mutex);

 repeat_ena:
 if (current.enable) {
  uint32_t i;

  for (i = 0; current.enable[i]; i++) {
   signed char t = 0;
   if (mod_isprovided (current.enable[i]) || mod_isbroken(current.enable[i])) {
    current.enable = (char **)setdel ((void **)current.enable, current.enable[i]);
    goto repeat_ena;
   }

   if (((t = mod_flatten_current_tb_group(current.enable[i], MOD_ENABLE)) == -1) &&
       ((t = mod_flatten_current_tb_module(current.enable[i], MOD_ENABLE)) == -1)) {
    notice (2, "can't resolve service %s\n", current.enable[i]);

    mod_mark (current.enable[i], MARK_UNRESOLVED);
   } else {
    if (t != 0) {
     goto repeat_ena;
    }
   }
  }
 }

 repeat_disa:
 if (current.disable) {
  uint32_t i;

  for (i = 0; current.disable[i]; i++) {
   signed char t = 0;
   if (!mod_isprovided (current.disable[i]) || mod_isbroken(current.disable[i])) {
    current.disable = (char **)setdel ((void **)current.disable, current.disable[i]);
    goto repeat_disa;
   }

   if (((t = mod_flatten_current_tb_group(current.disable[i], MOD_DISABLE)) == -1) &&
       ((t = mod_flatten_current_tb_module(current.disable[i], MOD_DISABLE)) == -1)) {
    notice (2, "can't resolve service %s\n", current.disable[i]);

    mod_mark (current.disable[i], MARK_UNRESOLVED);
   } else {
    if (t != 0) {
     goto repeat_disa;
    }
   }
  }
 }

 emutex_unlock (&ml_tb_current_mutex);
}

void mod_examine_module (struct lmodule *module) {
 if (!(module->status & STATUS_WORKING)) {
  if (module->si && module->si->provides) {
   uint32_t i = 0;
   if (module->status & STATUS_ENABLED) {
    emutex_lock (&ml_tb_current_mutex);
    current.enable = (char **)setslice_nc ((void **)current.enable, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_tb_current_mutex);

    emutex_lock (&ml_currently_provided_mutex);
    currently_provided = (char **)setcombine_nc ((void **)currently_provided, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_currently_provided_mutex);

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   } else if ((module->status & STATUS_DISABLED) || (module->status == STATUS_IDLE)) {
    emutex_lock (&ml_tb_current_mutex);
    current.disable = (char **)setslice_nc ((void **)current.disable, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_tb_current_mutex);

    emutex_lock (&ml_currently_provided_mutex);
    currently_provided = (char **)setslice_nc ((void **)currently_provided, (const void **)module->si->provides, SET_TYPE_STRING);
    emutex_unlock (&ml_currently_provided_mutex);

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   }

   for (; module->si->provides[i]; i++) {
    mod_examine (module->si->provides[i]);
   }
  }
 }
}

void mod_post_examine (char *service) {
 char **pex = NULL;
 struct stree *post_examine;

 emutex_lock (&ml_chain_examine);

 if ((post_examine = streefind (module_logics_chain_examine, service, TREE_FIND_FIRST))) {
  pex = (char **)setdup ((const void **)post_examine->value, SET_TYPE_STRING);
 }

 emutex_unlock (&ml_chain_examine);

 mod_decrease_deferred_by (service);

 if (pex) {
  uint32_t j = 0;

  for (; pex[j]; j++) {
   mod_examine (pex[j]);
  }

  free (pex);
 }
}

void mod_pre_examine (char *service) {
 char **pex = NULL;
 struct stree *post_examine;

 emutex_lock (&ml_chain_examine);

 if ((post_examine = streefind (module_logics_chain_examine_reverse, service, TREE_FIND_FIRST))) {
  pex = (char **)setdup ((const void **)post_examine->value, SET_TYPE_STRING);
 }

 emutex_unlock (&ml_chain_examine);

 if (pex) {
  uint32_t j = 0, broken = 0, done = 0;

  for (; pex[j]; j++) {
   if (mod_isbroken (pex[j])) {
    broken++;
   } else {
    int task = mod_gettask (service);

    if (!task ||
        ((task & MOD_ENABLE) && mod_isprovided (pex[j])) ||
        ((task & MOD_DISABLE) && !mod_isprovided (pex[j]))) {
     done++;

     mod_post_examine (pex[j]);
    }

    mod_examine (pex[j]);
   }
  }
  free (pex);

  if ((broken + done) == j) {
   mod_remove_defer (service);
   mod_examine (service);
  }
 }
}

char mod_disable_users (struct lmodule *module) {
 if (!service_usage_query(SERVICE_NOT_IN_USE, module, NULL)) {
  ssize_t i = 0;
  char **need = NULL;
  char **t = service_usage_query_cr (SERVICE_GET_SERVICES_THAT_USE, module, NULL);
  char retval = 1;

  if (t) {
   for (; t[i]; i++) {
    if (mod_isbroken (t[i])) {
     if (need) free (need);
     return 0;
    } else {
     emutex_lock (&ml_tb_current_mutex);

     if (!inset ((const void **)current.disable, (void *)t[i], SET_TYPE_STRING)) {
      retval = 2;
      need = (char **)setadd ((void **)need, t[i], SET_TYPE_STRING);

      if (module->si && module->si->provides) {
       uint32_t y = 0;
       for (; module->si->provides[y]; y++) {
        mod_defer_until (module->si->provides[y], t[i]);
       }
      }
     }

     emutex_unlock (&ml_tb_current_mutex);
    }
   }

   if (retval == 2) {
    emutex_lock (&ml_tb_current_mutex);

    char **tmp = (char **)setcombine ((const void **)current.disable, (const void **)need, SET_TYPE_STRING);
    if (current.disable) free (current.disable);
    current.disable = tmp;

    emutex_unlock (&ml_tb_current_mutex);

    for (i = 0; need[i]; i++) {
     mod_examine (need[i]);
    }
   }
  }

  return retval;
 }

 return 0;
}

char mod_enable_requirements (struct lmodule *module) {
 if (!service_usage_query(SERVICE_REQUIREMENTS_MET, module, NULL)) {
  char retval = 1;
  if (module->si && module->si->requires) {
   ssize_t i = 0;
   char **need = NULL;

   for (; module->si->requires[i]; i++) {
    if (mod_isbroken (module->si->requires[i])) {
     if (need) free (need);
     return 0;
    } else if (!service_usage_query (SERVICE_IS_PROVIDED, NULL, module->si->requires[i])) {
     emutex_lock (&ml_tb_current_mutex);

     if (!inset ((const void **)current.enable, (void *)module->si->requires[i], SET_TYPE_STRING)) {
      retval = 2;
      need = (char **)setadd ((void **)need, module->si->requires[i], SET_TYPE_STRING);

      if (module->si && module->si->provides) {
       uint32_t y = 0;
       for (; module->si->provides[y]; y++) {
        mod_defer_until (module->si->provides[y], module->si->requires[i]);
       }
      }
     }

     emutex_unlock (&ml_tb_current_mutex);
    }
   }

   if (retval == 2) {
    emutex_lock (&ml_tb_current_mutex);

    char **tmp = (char **)setcombine ((const void **)current.enable, (const void **)need, SET_TYPE_STRING);
    if (current.enable) free (current.enable);
    current.enable = tmp;

    emutex_unlock (&ml_tb_current_mutex);

    for (i = 0; need[i]; i++) {
     mod_examine (need[i]);
    }
   }
  }

  return retval;
 }

 return 0;
}

void mod_apply_enable (struct stree *des) {
 mod_workthreads_inc();
 if (des) {
  struct lmodule **lm = (struct lmodule **)des->value;

  if (lm && lm[0]) {
   struct lmodule *first = lm[0];

   do {
    struct lmodule *current = lm[0];

    if ((lm[0]->status & STATUS_ENABLED) || mod_enable_requirements (current)) {
     mod_workthreads_dec();
     return;
    }

    mod (MOD_ENABLE, current, NULL);

/* check module status or return value to find out if it's appropriate for the task */
    if (lm[0]->status & STATUS_ENABLED) {
     mod_workthreads_dec();
     return;
    }

/* next module */
    emutex_lock (&ml_service_list_mutex);

/* make sure there's not been a different thread that did what we want to do */
    if ((lm[0] == current) && lm[1]) {
     ssize_t rx = 1;

     notice (10, "service %s: done with module %s, rotating the list", des->key, (current->module && current->module->rid ? current->module->rid : "unknown"));

     for (; lm[rx]; rx++) {
      lm[rx-1] = lm[rx];
     }

     lm[rx-1] = current;
    }

    emutex_unlock (&ml_service_list_mutex);
   } while (lm[0] != first);
/* if we tried to enable something and end up here, it means we did a complete
   round-trip and nothing worked */

   emutex_lock (&ml_tb_current_mutex);
   current.enable = strsetdel(current.enable, des->key);
   emutex_unlock (&ml_tb_current_mutex);

/* mark service broken if stuff went completely wrong */
   notice (2, "ran out of options for service %s (enable), marking as broken", des->key);

   mod_mark (des->key, MARK_BROKEN);
  }

  mod_workthreads_dec();
  return;
 }

 mod_workthreads_dec();
 return;
}

void mod_apply_disable (struct stree *des) {
 mod_workthreads_inc();
 if (des) {
  struct lmodule **lm = (struct lmodule **)des->value;

  if (lm && lm[0]) {
   struct lmodule *first = lm[0];
   int any_ok = 0;

   do {
    struct lmodule *current = lm[0];

    if ((lm[0]->status & STATUS_DISABLED) || (lm[0]->status == STATUS_IDLE)) {
     goto skip_module;
    }

    if (mod_disable_users (current)) {
     mod_workthreads_dec();
     return;
    }

    mod (MOD_DISABLE, current, NULL);

    /* check module status or return value to find out if it's appropriate for the task */
    if ((lm[0]->status & STATUS_DISABLED) || (lm[0]->status == STATUS_IDLE)) {
     any_ok = 1;
    }

    skip_module:
/* next module */
    emutex_lock (&ml_service_list_mutex);

/* make sure there's not been a different thread that did what we want to do */
    if ((lm[0] == current) && lm[1]) {
     ssize_t rx = 1;

     notice (10, "service %s: done with module %s, rotating the list", des->key, (current->module && current->module->rid ? current->module->rid : "unknown"));

     for (; lm[rx]; rx++) {
      lm[rx-1] = lm[rx];
     }

     lm[rx-1] = current;
    }

    emutex_unlock (&ml_service_list_mutex);
   } while (lm[0] != first);
/* if we tried to enable something and end up here, it means we did a complete
   round-trip and nothing worked */

   emutex_lock (&ml_tb_current_mutex);
   current.disable = strsetdel(current.disable, des->key);
   emutex_unlock (&ml_tb_current_mutex);

   if (any_ok) {
    mod_workthreads_dec();
    return;
   }

/* mark service broken if stuff went completely wrong */
   notice (2, "ran out of options for service %s (disable), marking as broken", des->key);

   mod_mark (des->key, MARK_BROKEN);
  }

  mod_workthreads_dec();
  return;
 }

 mod_workthreads_dec();
 return;
}

int mod_gettask (char * service) {
 int task = 0;

 emutex_lock (&ml_tb_current_mutex);
 if (inset ((const void **)current.disable, service, SET_TYPE_STRING))
  task = MOD_DISABLE;
 else if (inset ((const void **)current.enable, service, SET_TYPE_STRING))
  task = MOD_ENABLE;
 emutex_unlock (&ml_tb_current_mutex);

 return task;
}

char mod_examine_group (char *groupname) {
 struct group_data *gd = mod_group_get_data (groupname);
 if (!gd) return 0;
 char post_examine = 0;

 emutex_lock (&ml_group_data_mutex);

 if (gd->members) {
  int task = mod_gettask (groupname);

  notice (2, "group %s: examining members", groupname);

  ssize_t x = 0, mem = setcount ((const void **)gd->members), failed = 0, on = 0;
  struct lmodule **providers = NULL;
  char group_failed = 0, group_ok = 0;

  for (; gd->members[x]; x++) {
   if (mod_isbroken (gd->members[x])) {
    failed++;
   } else {
    struct stree *serv = NULL;

    emutex_lock (&ml_service_list_mutex);

    if (module_logics_service_list && (serv = streefind(module_logics_service_list, gd->members[x], TREE_FIND_FIRST))) {
     struct lmodule **lm = (struct lmodule **)serv->value;

     if (lm) {
      ssize_t y = 0;

      for (; lm[y]; y++) {
       if (lm[y]->status & STATUS_ENABLED) {
        providers = (struct lmodule **)setadd ((void **)providers, (void *)lm[y], SET_NOALLOC);
        on++;

        break;
       }
      }
     }
    }
   }

   emutex_unlock (&ml_service_list_mutex);
  }

  if (!on) {
   if (mod_isprovided (groupname)) {
    emutex_lock (&ml_currently_provided_mutex);
    currently_provided = (char **)strsetdel ((char **)currently_provided, (char *)groupname);
    emutex_unlock (&ml_currently_provided_mutex);

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   }

   if (task & MOD_DISABLE) {
    post_examine = 1;
   }
  }

  if (task & MOD_ENABLE) {
   if (providers) {
    if (gd->options & (MOD_PLAN_GROUP_SEQ_ANY | MOD_PLAN_GROUP_SEQ_ANY_IOP)) {
     if (on > 0) {
      group_ok = 1;
     } else if (failed >= mem) {
      group_failed = 1;
     }
    } else if (gd->options & MOD_PLAN_GROUP_SEQ_MOST) {
     if (on && ((on + failed) >= mem)) {
      group_ok = 1;
     } else if (failed >= mem) {
      group_failed = 1;
     }
    } else if (gd->options & MOD_PLAN_GROUP_SEQ_ALL) {
     if (on >= mem) {
      group_ok = 1;
     } else if (failed) {
      group_failed = 1;
     }
    } else {
     notice (2, "marking group %s broken (bad group type)", groupname);

     mod_mark (groupname, MARK_BROKEN);
    }
   }

   if (group_ok) {
    notice (2, "marking group %s up", groupname);

    service_usage_query_group (SERVICE_SET_GROUP_PROVIDERS, (struct lmodule *)providers, groupname);

    if (!mod_isprovided (groupname)) {
     emutex_lock (&ml_currently_provided_mutex);
     currently_provided = (char **)setadd ((void **)currently_provided, (void *)groupname, SET_TYPE_STRING);
     emutex_unlock (&ml_currently_provided_mutex);
    }

    post_examine = 1;

#ifdef _POSIX_PRIORITY_SCHEDULING
    sched_yield();
#endif

    pthread_cond_broadcast (&ml_cond_service_update);
   } else if (group_failed) {
    notice (2, "marking group %s broken (group requirements failed)", groupname);

    mod_mark (groupname, MARK_BROKEN);
   } else {
/* just to make sure everything will actually be enabled/disabled */
    emutex_unlock (&ml_group_data_mutex);
    mod_flatten_current_tb_group(groupname, task);
    emutex_lock (&ml_group_data_mutex);
   }
  } else { /* mod_disable */
/* just to make sure everything will actually be enabled/disabled */
   emutex_unlock (&ml_group_data_mutex);
   mod_flatten_current_tb_group(groupname, task);
   emutex_lock (&ml_group_data_mutex);
  }

  if (providers) free (providers);
 }

 emutex_unlock (&ml_group_data_mutex);

 if (post_examine) {
  mod_post_examine (groupname);
 }
 return 1;
}

void mod_examine (char *service) {
 if (mod_isbroken (service)) {
  notice (2, "service %s marked as being broken", service);

/* make sure this is not still queued for something */
  int task = mod_gettask (service);

  if (task) {
   emutex_lock (&ml_tb_current_mutex);
   if (task & MOD_ENABLE) {
    current.enable = strsetdel (current.enable, service);
   } else if (task & MOD_DISABLE) {
    current.disable = strsetdel (current.disable, service);
   }
   emutex_unlock (&ml_tb_current_mutex);
  }

  mod_post_examine(service);
  return;
 } else if (mod_isdeferred (service)) {
  mod_pre_examine(service);
  notice (2, "service %s still marked as deferred", service);

  return;
 } else if (mod_examine_group (service)) {
  return;
 } else {
  int task = mod_gettask (service);

  if (!task ||
      ((task & MOD_ENABLE) && mod_isprovided (service)) ||
      ((task & MOD_DISABLE) && !mod_isprovided (service))) {
   mod_post_examine (service);
   return;
  }

/*  eprintf (stderr, " ** examining service %s (%s).\n", service,
                   task & MOD_ENABLE ? "enable" : "disable");*/

  emutex_lock (&ml_service_list_mutex);
  struct stree *v = streefind (module_logics_service_list, service, TREE_FIND_FIRST);
  struct lmodule **lm = v ? v->value : NULL;
  emutex_unlock (&ml_service_list_mutex);

  if (lm && lm[0]) {
   pthread_t th;
   char **before = NULL, **after = NULL, **against = NULL;

   if (lm[0]->si && (lm[0]->si->before || lm[0]->si->after)) {
    if (task & MOD_ENABLE) {
     before = lm[0]->si->before;
     after = lm[0]->si->after;
     against = current.enable;
    } else if (task & MOD_DISABLE) {
     before = lm[0]->si->after;
     after = lm[0]->si->before;
     against = current.disable;
    }
   }

/* "loose" service ordering */
   if (before) {
    uint32_t i = 0;

    for (; before[i]; i++) {
     char **d;
     emutex_lock (&ml_tb_current_mutex);
     if ((d = inset_pattern ((const void **)against, before[i], SET_TYPE_STRING))) {
      uint32_t y = 0;
      emutex_unlock (&ml_tb_current_mutex);

      for (; d[y]; y++) {
       mod_defer_until (d[y], service);
      }

      free (d);
     } else
      emutex_unlock (&ml_tb_current_mutex);
    }
   }
   if (after) {
    char hd = 0;
    uint32_t i = 0;

    for (; after[i]; i++) {
     char **d;
     emutex_lock (&ml_tb_current_mutex);
     if ((d = inset_pattern ((const void **)against, after[i], SET_TYPE_STRING))) {
      uint32_t y = 0;
      emutex_unlock (&ml_tb_current_mutex);

      for (; d[y]; y++) {
       mod_defer_until (service, d[y]);
      }

      hd = 1;

      free (d);
     } else
      emutex_unlock (&ml_tb_current_mutex);
    }

    if (hd) {
     return;
    }
   }
   if (task & MOD_ENABLE) {
    if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_apply_enable, v)) {
     mod_apply_enable(v);
    }
   } else {
    if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))mod_apply_disable, v)) {
     mod_apply_disable(v);
    }
   }
  } else {
   notice (2, "cannot resolve service %s.", service);

   mod_mark (service, MARK_UNRESOLVED);
  }

  return;
 }
}

void workthread_examine (char *service) {
 mod_workthreads_inc();

 mod_examine (service);
 free (service);

 mod_workthreads_dec();
}

void mod_spawn_workthreads () {
 emutex_lock (&ml_tb_current_mutex);

 if (current.enable) {
  uint32_t i = 0;

  for (; current.enable[i]; i++) {
   char *sc = estrdup (current.enable[i]);
   pthread_t th;

   if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))workthread_examine, sc)) {
    emutex_unlock (&ml_tb_current_mutex);
    workthread_examine (sc);
    emutex_lock (&ml_tb_current_mutex);
   }
  }
 }

 if (current.disable) {
  uint32_t i = 0;

  for (; current.disable[i]; i++) {
   char *sc = estrdup (current.disable[i]);
   pthread_t th;

   if (ethread_create (&th, &thread_attribute_detached, (void *(*)(void *))workthread_examine, sc)) {
    emutex_unlock (&ml_tb_current_mutex);
    workthread_examine (sc);
    emutex_lock (&ml_tb_current_mutex);
   }
  }
 }

 emutex_unlock (&ml_tb_current_mutex);
}

void mod_commit_and_wait (char **en, char **dis) {
 int remainder;
 char spawns = 0;

 mod_commits_inc();

#ifdef _POSIX_PRIORITY_SCHEDULING
 sched_yield();
#endif

 pthread_cond_broadcast (&ml_cond_service_update);

 mod_flatten_current_tb();

 mod_spawn_workthreads ();

 do {
  remainder = 0;
  char spawn = 0;

  if (en) {
   uint32_t i = 0;

   for (; en[i]; i++) {
    if (!mod_isbroken (en[i]) && !mod_isprovided(en[i])) {
//        eprintf (stderr, "not yet provided: %s\n", en[i]);

     remainder++;

     emutex_lock (&ml_tb_current_mutex);
     if (!inset ((const void **)current.enable, en[i], SET_TYPE_STRING)) {
      emutex_unlock (&ml_tb_current_mutex);

      notice (2, "something must've gone wrong with service %s...", en[i]);

      remainder--;
     } else
      emutex_unlock (&ml_tb_current_mutex);
    }
   }
  }

  if (dis) {
   uint32_t i = 0;

   for (; dis[i]; i++) {
    if (!mod_isbroken (dis[i]) && mod_isprovided(dis[i])) {
//     eprintf (stderr, "still provided: %s\n", dis[i]);

     remainder++;

     emutex_lock (&ml_tb_current_mutex);
     if (!inset ((const void **)current.disable, dis[i], SET_TYPE_STRING)) {
      emutex_unlock (&ml_tb_current_mutex);

      notice (2, "something must've gone wrong with service %s...", dis[i]);

      remainder--;
     } else
      emutex_unlock (&ml_tb_current_mutex);
    }
   }
  }

  if (remainder <= 0) {
   notice (5, "plan finished.");

   mod_commits_dec();
   return;
  }

  notice (4, "still need %i services\n", remainder);

  emutex_lock (&ml_workthreads_mutex);
  spawn = ml_workthreads == 0;
  emutex_unlock (&ml_workthreads_mutex);

  if (spawn) {
/* lockup protection... */
   if (spawns > MAX_SPAWNS) {
    notice (1, "too many respawns: giving up on plan requirements.");

    mod_commits_dec();
    return;
   }
   mod_spawn_workthreads ();
   spawns++;
  } else {
   notice (4, "%i workthreads left\n", ml_workthreads);
  }

  pthread_cond_wait (&ml_cond_service_update, &ml_service_update_mutex);
 } while (remainder);

 mod_commits_dec();
}

/* end new functions */
