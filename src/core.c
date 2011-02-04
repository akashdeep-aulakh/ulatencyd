/*
    Copyright 2010,2011 ulatencyd developers

    This file is part of ulatencyd.

    ulatencyd is free software: you can redistribute it and/or modify it under 
    the terms of the GNU General Public License as published by the 
    Free Software Foundation, either version 3 of the License, 
    or (at your option) any later version.

    ulatencyd is distributed in the hope that it will be useful, 
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License 
    along with ulatencyd. If not, see http://www.gnu.org/licenses/.
*/

#include "config.h"
#include "ulatency.h"

#include "proc/procps.h"
#include "proc/sysinfo.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <unistd.h>

#ifdef ENABLE_DBUS
#include <dbus/dbus-glib.h>

DBusGConnection *U_dbus_connection;
#endif

lua_State *lua_main_state;
GList *filter_list;
GNode *processes_tree;
GHashTable *processes;
u_scheduler scheduler = {NULL};
static int iteration;
static double _last_load;
static double _last_percent;
// flag list of system wide flags
GList *system_flags;
int    system_flags_changed;


double get_last_load() {
  return _last_load;
}

double get_last_percent() {
  return _last_percent;
}

guint get_plugin_id() {
  static guint last = USER_ACTIVE_AGENT_MODULE;
  return ++last;
}


/*************************************************************
 * u_proc code
 ************************************************************/

void filter_block_free(gpointer fb) {
  free(fb);
}

void u_head_free(gpointer fb) {
  DEC_REF(fb);
}


void u_proc_free(void *ptr) {
  u_proc *proc = ptr;
  GNode *nparent, *cur;
  u_proc *proc_tmp;

  g_assert(proc->ref == 0);

  g_free(proc->cmdfile);
  g_free(proc->exe);
  g_free(proc->cmdline_match);
  if(proc->environ)
      g_hash_table_unref(proc->environ);
  if(proc->cmdline)
      g_ptr_array_unref(proc->cmdline);

  if(proc->lua_data) {
    luaL_unref(lua_main_state, LUA_REGISTRYINDEX, proc->lua_data);
  }
  //g_hash_table_remove_all (proc->skip_filter);
  //g_hash_table_unref(proc->skip_filter);
  g_hash_table_destroy (proc->skip_filter);

  if(g_node_n_children(proc->node)) {
    // the process which dies has some children. we have to move children
    // to a new parent. Try the parent of the dead process first
    if(proc->node->parent) {
      nparent = proc->node->parent;
    } else {
      proc_tmp = proc_by_pid(1);
      if(proc_tmp && proc_tmp->node) {
        nparent = proc_tmp->node;
      } else {
        // this should not happen, but we have to attach the node somewhere
        // this could happen if the netlink messages arrive befor a fill update
        g_warning("attach child from dead process to root tree");
        nparent = processes_tree;
      }
    }
    g_assert(nparent != proc->node);
    g_node_unlink(proc->node);
    while((cur = g_node_first_child(proc->node)) != NULL) {
      g_node_unlink(cur);
      g_node_append(nparent, cur);
    }
  }
  g_assert(g_node_n_children(proc->node) == 0);
  g_node_destroy(proc->node);
  freesupgrp(&(proc->proc));
  freeproc_light(&(proc->proc));
  free(proc);
}


u_proc* u_proc_new(proc_t *proc) {
  u_proc *rv;

  rv = g_new0(u_proc, 1);

  rv->free_fnk = u_proc_free;
  rv->ref = 1;
  rv->skip_filter = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, filter_block_free);

  rv->flags = NULL;
  rv->changed = TRUE;
  rv->node = g_node_new(rv);

  if(proc) {
    rv->pid = proc->tgid;
    U_PROC_SET_STATE(rv,UPROC_ALIVE);
    memcpy(&(rv->proc), proc, sizeof(proc_t));
  } else {
    U_PROC_SET_STATE(rv,UPROC_NEW);
  }

  return rv;
}

/**
 * u_proc_ensure:
 * @proc: a #u_proc
 * @what: what variable should exist
 * @update: force update
 *
 * Ensures a varible is filled. If update is true, the variable is updated
 * to recent values for sure
 *
 * Returns: @success
 */
int u_proc_ensure(u_proc *proc, enum ENSURE_WHAT what, int update) {
  if(what == ENVIRONMENT) {
      if(update && proc->environ) {
          g_hash_table_unref(proc->environ);
          proc->environ = NULL;
      }
      if(!proc->environ)
          proc->environ = u_read_env_hash (proc->pid);
      return (proc->environ != NULL);
  } else if(what == CMDLINE) {
      if(update && proc->cmdline) {
          g_ptr_array_unref(proc->cmdline);
          proc->cmdline = NULL;
      }
      if(!proc->cmdline) {
          int i;
          gchar *tmp, *tmp2;

          g_free(proc->cmdline_match);
          proc->cmdline_match = NULL;

          GString *match = g_string_new("");
          proc->cmdline = u_read_0file (proc->pid, "cmdline");
          // update cmd
          if(proc->cmdline) {
              for(i = 0; i < proc->cmdline->len; i++) {
                  if(i)
                      match = g_string_append_c(match, ' ');
                  match = g_string_append(match, g_ptr_array_index(proc->cmdline, i));
              }
              proc->cmdline_match = g_string_free(match, FALSE);
              // empty command line, for kernel threads for example
              if(!proc->cmdline->len)
                return TRUE;
              if(proc->cmdfile) {
                g_free(proc->cmdfile);
                proc->cmdfile = NULL;
              }
              tmp = g_ptr_array_index(proc->cmdline, 0);
              if(tmp) {
                  tmp2 = g_strrstr_len(tmp, -1, "/");
                  if(tmp2 == NULL) {
                    proc->cmdfile = g_strdup(tmp);
                  } else if((tmp2+1-tmp) < strlen(tmp)) {
                    proc->cmdfile = g_strdup(tmp2+1);
                  }
              }
              return TRUE;
          } else {
              return FALSE;
          }
      }
      return (proc->cmdline != NULL);
  } else if(what == EXE) {
      char buf[PATH_MAX+1];
      ssize_t out;
      char *path;
      if(update && proc->exe) {
          g_free(proc->exe);
          proc->exe = NULL;
      }
      if(!proc->exe) {
        path = g_strdup_printf ("/proc/%u/exe", (guint)proc->pid);
        out = readlink(path, (char *)&buf, PATH_MAX);
        if(out > 0) {
            proc->exe = g_strndup((char *)&buf, out);
        }
        g_free(path);
      }
      return TRUE;
  }
  return FALSE;
}

void processes_free_value(gpointer data) {
  // called when a process is freed from the process list
  // this means that the process is not valid anymore and is
  // marked as such
  u_proc *proc = data;
  U_PROC_SET_STATE(proc, UPROC_INVALID);
  if(proc->node)
    g_node_unlink(proc->node);
  DEC_REF(proc);
}

// rebuild the node tree from content of the hash table
void rebuild_tree() {
//  processes_tree
  GHashTableIter iter;
  gpointer key, value;
  u_proc *proc, *parent;

  // clear root node
  g_node_destroy(processes_tree);
  processes_tree = g_node_new(NULL);

  // create nodes first
  g_hash_table_iter_init (&iter, processes);
  while (g_hash_table_iter_next (&iter, &key, &value)) 
  {
    proc = (u_proc *)value;
    proc->node = g_node_new(proc);
    g_node_append(processes_tree, proc->node);
  }

  // now we can lookup the parents and attach the node to the parent
  g_hash_table_iter_init (&iter, processes);
  while (g_hash_table_iter_next (&iter, &key, &value)) 
  {
    proc = (u_proc *)value;
    g_assert(proc->proc.ppid != proc->pid);
    if(proc->proc.ppid) {
      parent = proc_by_pid(proc->proc.ppid);
      // this should't happen, but under fork stress init may not have
      // collected so the parent does not exist.
      if(!parent) {
        g_warning("pid: %d parent %d missing. attaching to pid 1", proc->pid, proc->proc.ppid);
        parent = proc_by_pid(1);
      }

      g_assert(parent != proc);
      g_assert(parent && parent->node);
      g_node_unlink(proc->node);
      g_node_append(parent->node, proc->node);
    } else {
      g_node_unlink(proc->node);
      g_node_append(processes_tree, proc->node);
    }
  }


}

static int detect_changed(proc_t *old, proc_t *new) {
  // detects changes of main paramenters
  if(old->euid != new->euid || old->session != new->session ||
     old->egid != new->egid || old->pgrp != new->pgrp)
     return 1;
  return 0;
}

static gboolean processes_is_last_changed(gpointer key, gpointer value,
                                         gpointer user_data) {
  u_proc *proc = (u_proc *)value;
  int last_changed = *(int *)user_data;

  return (proc->last_update != last_changed);

}

int process_remove(u_proc *proc) {
  return g_hash_table_remove(processes, GUINT_TO_POINTER(proc->pid));
}

int process_remove_by_pid(int pid) {
  return g_hash_table_remove(processes, GUINT_TO_POINTER(pid));
}

static void clear_process_changed() {
  GHashTableIter iter;
  gpointer ikey, value;
  u_proc *proc;

  g_hash_table_iter_init (&iter, processes);
  while (g_hash_table_iter_next (&iter, &ikey, &value)) 
  {
    proc = (u_proc *)value;
    proc->changed = FALSE;
  }
  return;
}

// copy the fake value of a parent pid to the child until the real value
// of the child changes from the parent
#define fake_var_fix(FAKE, ORG) \
  if(proc->FAKE && (proc-> FAKE##_old != proc->proc.ORG)) { \
    /* when real value was set, the fake value disapears. */ \
    proc-> FAKE = 0; \
    proc->FAKE##_old = 0; \
    proc->changed = 1; \
  } else if(parent-> FAKE && \
            parent->proc.ORG == proc->proc.ORG && \
            parent-> FAKE != proc->FAKE) { \
    proc-> FAKE = parent->FAKE; \
    proc->FAKE##_old = proc->proc.ORG; \
    proc->changed = 1; \
  }

void process_workarrounds(u_proc *proc, u_proc *parent) {
  // do various workaround jobs here...
  fake_var_fix(fake_pgrp, pgrp);
  fake_var_fix(fake_session, session);
}

#undef fake_var_fix

int update_processes_run(PROCTAB *proctab, int full) {
  proc_t buf;
  u_proc *proc;
  u_proc *parent;
  time_t timeout = time(NULL);
  gboolean full_update = FALSE;
  static int run = 0;
  int removed;
  int rv = 0;
  int i;
  GList *updated = NULL;
  
  if(full)
    run++;

  if(!proctab) {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't open /proc");
    return 1;
  }

  while(readproc(proctab, &buf)){
    proc = proc_by_pid(buf.tgid);
    if(proc) {
      // free all changable allocated buffers
      freesupgrp(&(proc->proc));
      freeproc_light(&(proc->proc));
    } else {
      proc = u_proc_new(&buf);
      g_hash_table_insert(processes, GUINT_TO_POINTER(proc->pid), proc);
    }
    // detect change of important parameters that will cause a reschedule
    proc->changed = proc->changed | detect_changed(&(proc->proc), &buf);

    if(full)
      proc->last_update = run;
    // we can simply steal the pointer of the current allocated buffer
    memcpy(&(proc->proc), &buf, sizeof(proc_t));
    U_PROC_UNSET_STATE(proc, UPROC_NEW);
    U_PROC_SET_STATE(proc, UPROC_ALIVE);

    u_flag_clear_timeout(proc, timeout);
    updated = g_list_append(updated, proc);

    rv++;
    //g_list_foreach(filter_list, filter_run_for_proc, &buf);
    //freesupgrp(&buf);
  }

  // we update the parent links after all processes are updated
  for(i = 0; i < rv; i++) {
    proc = g_list_nth_data(updated, i);

    if(proc->proc.ppid && proc->proc.ppid != proc->pid) {
      parent = g_hash_table_lookup(processes, GUINT_TO_POINTER(proc->proc.ppid));
      // the parent should exist. in case it is missing we have to run a full
      // tree rebuild then
      if(parent && parent->node) {
        // current parent is not what it should be
        if(proc->node->parent != parent->node) {
          g_node_unlink(proc->node);
          g_node_append(parent->node, proc->node);
        }
        process_workarrounds(proc, parent);
      } else {
        full_update = TRUE;
      }
    } else {
      // this is kinda bad. it is ok for kernel processes and init
      if(proc->node->parent != processes_tree) {
        if(!G_NODE_IS_ROOT(proc->node))
          g_node_unlink(proc->node);
        g_node_append(processes_tree, proc->node);
      }
    }
  }
  // remove old processes

  g_list_free(updated);

  if(full) {
    removed = g_hash_table_foreach_remove(processes, 
                                          processes_is_last_changed,
                                          &run);
  }
  if(full_update) {
    rebuild_tree();
  }
  return rv;

}

int process_update_all() {
  int rv;
  PROCTAB *proctab;
  proctab = openproc(OPENPROC_FLAGS);
  rv = update_processes_run(proctab, TRUE);
  closeproc(proctab);
  return rv;
}

/* 
  updates a list of pids.
  pids must be a array of pid_t values followed by 0
*/
int process_update_pids(pid_t pids[]) {
  int rv;
  PROCTAB *proctab;
  proctab = openproc(OPENPROC_FLAGS | PROC_PID, pids);
  rv = update_processes_run(proctab, FALSE);
  closeproc(proctab);
  return rv;

}

int process_update_pid(int pid) {
  pid_t pids [2] = { pid, 0 };
  return process_update_pids(pids);
}


int process_new(int pid, int noupdate) {
  u_proc *proc;
  // if the process is already dead we can exit
  if(noupdate && proc_by_pid(pid))
      return 0;
  if(!process_update_pid(pid))
    return 0;
  proc = proc_by_pid(pid);
  if(!proc)
    return 0;
  filter_for_proc(proc);
  scheduler_run_one(proc);
  return 1;
}

int process_new_list(GArray *list, int noupdate) {
  u_proc *proc;
  int i, j = 0;
  pid_t *pids = (pid_t *)malloc((list->len+1)*sizeof(pid_t));
  //int pid_t = malloc(sizeof(pid_t)*(list->len+1));
  for(; i < list->len; i++) {
    if(!proc_by_pid(g_array_index(list,pid_t,i))) {
      pids[j] = g_array_index(list,pid_t,i);
      j++;
    }
  }
  pids[j] = 0;
  // if the process is already dead we can exit
  process_update_pids(pids);
  for(i=0; i < list->len; i++) {
    proc = proc_by_pid(g_array_index(list,pid_t,i));
    if(!proc)
      continue;
    filter_for_proc(proc);
    scheduler_run_one(proc);
  }
  free(pids);
  return TRUE;
}

int process_run_one(u_proc *proc, int update) {
  if(update)
    process_update_pid(proc->pid);
  filter_for_proc(proc);
  scheduler_run_one(proc);
  return TRUE;
}


void u_flag_free(void *ptr) {
  u_flag *flag = ptr;

  g_assert(flag->ref == 0);

  if(flag->name)
    free(flag->name);
  free(flag);
}

u_flag *u_flag_new(u_filter *source, const char *name) {
  u_flag *rv;
  
  rv = malloc(sizeof(u_flag));
  memset(rv, 0, sizeof(u_flag));
  
  rv->free_fnk = u_flag_free;
  rv->ref = 1;
  rv->source = source;

  if(name) {
    rv->name = g_strdup(name);
  }

  return rv;
}

int u_flag_add(u_proc *proc, u_flag *flag) {
  if(proc) {
    if(!g_list_find(proc->flags, flag)) {
      proc->flags = g_list_insert(proc->flags, flag, 0);
      INC_REF(flag);
    }
    proc->changed = 1;
  } else {
    if(!g_list_find(system_flags, flag)) {
      system_flags = g_list_insert(system_flags, flag, 0);
      INC_REF(flag);
    }
  }
  return TRUE;
}

int u_flag_del(u_proc *proc, u_flag *flag) {
  int rv = 0;
  if(proc) {
    if(g_list_index(proc->flags, flag) != -1) {
      DEC_REF(flag);
    }
    proc->flags = g_list_remove(proc->flags, flag);
    proc->changed = 1;
    return 1;
  } else {
    if(g_list_index(system_flags, flag) != -1) {
      DEC_REF(flag);
      rv++;
    }
    system_flags = g_list_remove(system_flags, flag);
  }
  return rv;
}

static gint u_flag_match_source(gconstpointer a, gconstpointer match) {
  u_flag *flg = (u_flag *)a;

  if(flg->source == match)
    return 0;

  return -1;
}

static int u_flag_match_name(gconstpointer a, gconstpointer name) {
  u_flag *flg = (u_flag *)a;

  return strcmp(flg->name, (char *)name);
}

static int u_flag_match_timeout(gconstpointer a, gconstpointer time) {
  u_flag *flg = (u_flag *)a;
  time_t t = GPOINTER_TO_UINT(time);
  if(flg->timeout == 0)
    return TRUE;
  return (flg->timeout > t);
}


#define CLEAR_BUILD(NAME, ARG, CMP) \
int NAME (u_proc *proc, ARG ) { \
  GList *item; \
  int rv = 0; \
  while((item = CMP ) != NULL) { \
    if(proc) { \
      proc->flags = g_list_remove_link (proc->flags, item); \
      DEC_REF(item->data); \
      item->data = NULL; \
      proc->changed = 1; \
      rv++; \
      g_list_free(item); \
    } else { \
      system_flags = g_list_remove_link (system_flags, item); \
      DEC_REF(item->data); \
      item->data = NULL; \
      system_flags_changed = 1; \
      rv ++; \
      g_list_free(item); \
    } \
  } \
  return rv; \
} 

CLEAR_BUILD(u_flag_clear_source, void *var, g_list_find_custom(proc ? proc->flags : system_flags, var, u_flag_match_source))

CLEAR_BUILD(u_flag_clear_name, const char *name, g_list_find_custom(proc ? proc->flags : system_flags, name, u_flag_match_name))

CLEAR_BUILD(u_flag_clear_timeout, time_t tm, g_list_find_custom(proc ? proc->flags : system_flags, GUINT_TO_POINTER(tm), u_flag_match_timeout))

int u_flag_clear_all(u_proc *proc) {
  GList *item;
  int rv = 0;
  if(proc) {
    while((item = g_list_first(proc->flags)) != NULL) {
      proc->flags = g_list_remove_link (proc->flags, item);
      DEC_REF(item->data);
      item->data = NULL;
      proc->changed = 1;
      rv++;
      g_list_free(item);
    }
    g_list_free(proc->flags);
  } else {
    while((item = g_list_first(system_flags)) != NULL) {
      system_flags = g_list_remove_link(system_flags, item);
      DEC_REF(item->data);
      item->data = NULL;
      g_list_free(item);
      rv++;
      system_flags_changed = 1;
    }
    g_list_free(system_flags);
  }
  return rv;
}


/*************************************************************
 * filter code
 ************************************************************/

void u_filter_free(void *ptr) {
  // FIXME
}

u_filter* filter_new() {
  u_filter *rv = malloc(sizeof(u_filter));
  memset(rv, 0, sizeof(u_filter));
  rv->free_fnk = u_filter_free;
  return rv;
}

void filter_register(u_filter *filter) {
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "register new filter: %s", filter->name ? filter->name : "unknown");
  filter_list = g_list_append(filter_list, filter);
}


int filter_run_for_proc(gpointer data, gpointer user_data) {
  u_proc *proc = data;
  u_filter *flt = user_data;
  struct filter_block *flt_block =NULL;
  int rv = 0;
  time_t ttime = 0;
  int timeout, flags;

  //printf("filter for proc %p\n", flt);

  g_assert(data);

  flt_block = (struct filter_block *)g_hash_table_lookup(proc->skip_filter, GUINT_TO_POINTER(flt));

  //g_hash_table_lookup
  if(flt_block) {
    if(flt_block->skip)
      return 0;
    time (&ttime);
    if(flt_block->timeout > ttime)
      return 0;
  }

  if(flt->check) {
    // if return 0 the real callback will be skipped
    if(!flt->check(proc, flt))
      return 0;
  }

  rv = flt->callback(proc, flt);

  if(rv == 0)
    return rv;

  if(!flt_block) {
    flt_block = malloc(sizeof(struct filter_block));
    memset(flt_block, 0, sizeof(struct filter_block));
    g_hash_table_insert(proc->skip_filter, GUINT_TO_POINTER(flt), flt_block);
  }

  flt_block->pid = proc->proc.tgid;

  timeout = FILTER_TIMEOUT(rv);
  flags = FILTER_FLAGS(rv);

  if(timeout) {
    if(!ttime)
      time (&ttime);
    flt_block->timeout = ttime + abs(timeout);
  } else if(flags == FILTER_STOP) {
    flt_block->skip = TRUE;
  }

  return rv;
}

static GNode *blocked_parent;

gboolean filter_run_for_node(GNode *node, gpointer data) {
  GNode *tmp;
  int rv;
  
  //u_filter *uf = data;
  //printf("rfn %s ;", uf->name);
  //printf("run for node\n");
  if(node == processes_tree)
    return FALSE;
  if(blocked_parent) {
    do {
      tmp = node->parent;
      if(!tmp)
        break;

      if(tmp == blocked_parent) {
        // we don't run filters on nodes those parent set the skip child flag
        return FALSE;
      } else if (tmp == processes_tree) {
        // we can unset the block, as we must have left all childs
        blocked_parent = NULL;
        break;
      }
    } while(TRUE);
  }
  rv = filter_run_for_proc(node->data, data);

  if(FILTER_FLAGS(rv) & FILTER_SKIP_CHILD) {
    blocked_parent = node;
  }
  return FALSE;
}

int scheduler_run() {
  // FIXME make scheduler more flexible
  if(scheduler.all) {
    return scheduler.all();
  } else {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "no scheduler.all set");
  }
  return 1;
}

int scheduler_run_one(u_proc *proc) {
  // FIXME make scheduler more flexible
  if(scheduler.one) {
    return scheduler.one(proc);
  }
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "no scheduler.one set");
  return 1;
}

void filter_for_proc(u_proc *proc) {
  /* run all filters on one proc */
  GList *cur = g_list_first(filter_list);
  while(cur) {
    filter_run_for_proc(proc, cur->data);
    cur = g_list_next(cur);
  }

}


void filter_run() {
  u_filter *flt;
  //printf("run filter %p, %d\n", filter_list, g_list_length(filter_list));
  GList *cur = g_list_first(filter_list);
  while(cur) {
    flt = cur->data;
    blocked_parent = NULL;
    if(flt->precheck)
      if(!flt->precheck(flt)) {
        cur = g_list_next(cur);
        continue;
      }
    g_debug("run filter: %s", flt->name);
    //printf("children %d %d\n", g_node_n_children(processes_tree), g_node_n_nodes (processes_tree,G_TRAVERSE_ALL ));
    g_node_traverse(processes_tree, G_PRE_ORDER,G_TRAVERSE_ALL, -1, 
                    filter_run_for_node, flt);
    cur = g_list_next(cur);
    if(flt->postcheck) {
      flt->postcheck(flt);
    }
  }
  blocked_parent = NULL;
}

static void update_caches() {
  double a, b;
  
  loadavg(&_last_load, &a, &b);
  _last_percent = (_last_load / (double)smp_num_cpus);

}


int iterate(gpointer rv) {
  time_t timeout = time(NULL);
  GTimer *timer = g_timer_new();
  gdouble last, current;
  gulong dump;

  g_timer_start(timer);
  u_flag_clear_timeout(NULL, timeout);
  iteration += 1;
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "start iteration %d:", iteration);
  update_caches();
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "update processes:");
  last = g_timer_elapsed(timer, &dump);
  process_update_all();
  current = g_timer_elapsed(timer, &dump);
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "took %0.2F. run filter:", (current - last));
  last = current;
  filter_run();
  current = g_timer_elapsed(timer, &dump);
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "took %0.2F. schedule:", (current - last));
  last = current;
  scheduler_run();
  g_timer_stop(timer);
  current = g_timer_elapsed(timer, &dump);
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "took %0.2F. complete run %d took %0.2F", (current - last), iteration, current);
  clear_process_changed();
  system_flags_changed = 0;
  return GPOINTER_TO_INT(rv);
}

/***************************************************************************
 * scheduler stuff
 **************************************************************************/

int scheduler_set(u_scheduler *sched) {
  if(sched) {
    memcpy(&scheduler, sched, sizeof(u_scheduler));
  } else {
    memset(&scheduler, 0, sizeof(u_scheduler));
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "unset scheduler");
  }
  return 0;
}

u_scheduler *scheduler_get() {
  return &scheduler;
}



/***************************************************************************
 * rules and modules handling
 **************************************************************************/

int load_rule_directory(char *path, char *load_pattern, int fatal) {
  DIR             *dip;
  struct dirent   *dit;
  char rpath[PATH_MAX+1];
  gsize  disabled_len;
  int i;
  char **disabled;
  char *rule_name;

  disabled = g_key_file_get_string_list(config_data, CONFIG_CORE,
                                        "disabled_rules", &disabled_len, NULL);


  g_message("load rule directory: %s", path);

  if ((dip = opendir(path)) == NULL)
  {
    perror("opendir");
    return 1;
  }

  if(load_pattern)
    g_message("load pattern: %s", load_pattern);

  while ((dit = readdir(dip)) != NULL)
  {
    if(fnmatch("*.lua", dit->d_name, 0))
      continue;
    rule_name = g_strndup(dit->d_name,strlen(dit->d_name)-4);
    if(load_pattern && (fnmatch(load_pattern, dit->d_name, 0) != 0))
      goto skip;

    for(i = 0; i < disabled_len; i++) {
      if(!g_strcasecmp(disabled[i], rule_name))
        goto skip;
    }

    snprintf(rpath, PATH_MAX, "%s/%s", path, dit->d_name);
    if(load_lua_rule_file(lua_main_state, rpath) && fatal)
      abort();
    g_free(rule_name);
    continue;
skip:
    g_debug("skip rule: %s", dit->d_name);
    g_free(rule_name);
  }
  g_strfreev(disabled);
  free(dip);
  return 0;
}


int load_modules(char *modules_directory) {
  DIR             *dip;
  struct dirent   *dit;
  char rpath[PATH_MAX+1];
  char *minit_name, *module_name, *error;
  char **disabled;
  gsize  disabled_len, i;
  gboolean skip;
  void *handle;
  int (*minit)(void);

  if ((dip = opendir(modules_directory)) == NULL)
  {
    perror("opendir");
    return 0;
  }

  disabled = g_key_file_get_string_list(config_data, CONFIG_CORE,
                                        "disabled_modules", &disabled_len, NULL);

  while ((dit = readdir(dip)) != NULL)
  {
    skip = FALSE;
    if(fnmatch("*.so", dit->d_name, 0))
      continue;

    module_name = g_strndup(dit->d_name,strlen(dit->d_name)-3);

    for(i = 0; i < disabled_len; i++) {
      if(!g_strcasecmp(disabled[i], module_name)) {
        skip = TRUE;
        break;
      }
    }
    if(!skip) {
      snprintf(rpath, PATH_MAX, "%s/%s", modules_directory, dit->d_name);
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "load module %s", dit->d_name);

      handle = dlopen(rpath, RTLD_LAZY);
      if (!handle) {
        //fprintf(stderr, "%s\n", dlerror());
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load module %s", rpath);
      }
      dlerror();

      minit_name = g_strconcat(module_name, "_init", NULL);
      *(void **) (&minit) = dlsym(handle, minit_name);

      if ((error = dlerror()) != NULL)  {
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load module %s: %s", module_name, error);
      }

      if(minit())
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "module %s returned error", module_name);

      free(minit_name);
    } else
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "skip module %s", module_name);

    free(module_name);
  }
  g_strfreev(disabled);
  closedir(dip);
  return 1;
}

int luaopen_ulatency(lua_State *L);
int luaopen_bc(lua_State *L);

int u_dbus_setup();


int core_init() {
  // load config
  iteration = 1;
  filter_list = NULL;

  smp_num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

#ifdef ENABLE_DBUS
  if(!u_dbus_setup())
    g_warning("failed to setup dbus");
#endif


  processes_tree = g_node_new(NULL);
  processes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, 
                                    processes_free_value);

  // configure lua
  lua_main_state = luaL_newstate();
  luaL_openlibs(lua_main_state);
  luaopen_bc(lua_main_state);
  luaopen_ulatency(lua_main_state);
#ifdef LIBCGROUP
  luaopen_cgroup(lua_main_state);
#endif

  // FIXME make it configurable
  scheduler_set(&LUA_SCHEDULER);

  if(load_lua_rule_file(lua_main_state, QUOTEME(LUA_CORE_FILE)))
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load core library");

  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "core initialized");
  return 1;
}

void core_unload() {
  lua_gc (lua_main_state, LUA_GCCOLLECT, 0);
}

