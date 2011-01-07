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


lua_State *lua_main_state;
GList *filter_list;
GNode *processes_tree;
GHashTable *processes;
u_scheduler scheduler = {NULL};
static int iteration;
static double _last_load;
static double _last_percent;

double get_last_load() {
  return _last_load;
}

double get_last_percent() {
  return _last_percent;
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

  if(proc->lua_data) {
    luaL_unref(lua_main_state, LUA_REGISTRYINDEX, proc->lua_data);
  }
  g_hash_table_remove_all (proc->skip_filter);
  g_hash_table_unref(proc->skip_filter);
  g_node_destroy(proc->node);
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

  if(proc) {
    rv->pid = proc->tgid;
    U_PROC_SET_STATE(rv,UPROC_ALIVE);
    memcpy(&(rv->proc), proc, sizeof(proc_t));
  } else {
    U_PROC_SET_STATE(rv,UPROC_NEW);
  }

  return rv;
}

void processes_free_value(gpointer data) {
  u_proc *proc = data;
  U_PROC_SET_STATE(proc, UPROC_INVALID);
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
    if(proc->proc.ppid) {
      parent = proc_by_pid(proc->proc.ppid);
      g_assert(parent && parent->node);
      g_node_unlink(proc->node);
      g_node_append(parent->node, proc->node);
    }
  }


}

static int detect_changed(proc_t *old, proc_t *new) {
  // detects changes of main paramenters
  if(old->euid != new->euid || old->session != new->session ||
     old->egid != new->egid)
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


int update_processes_run(PROCTAB *proctab, int full) {
  proc_t buf;
  u_proc *proc;
  u_proc *parent;
  gboolean full_update = FALSE;
  static int run = 0;
  int removed;
  int rv = 0;
  
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
    U_PROC_UNSET_STATE(proc, UPROC_ALIVE);


    if(!proc->node) {
      proc->node = g_node_new(proc);
      if(proc->proc.ppid) {
        parent = g_hash_table_lookup(processes, GUINT_TO_POINTER(proc->proc.ppid));
        // the parent should exist. in case it is missing we have to run a full
        // tree rebuild then
        if(parent) {
          g_node_append(parent->node, proc->node);
        } else {
          full_update = TRUE;
        }
      } else {
        g_node_append(processes_tree, proc->node);
      }
    }
    rv++;
    //g_list_foreach(filter_list, filter_run_for_proc, &buf);
    //freesupgrp(&buf);
  }

  if(full) {
    removed = g_hash_table_foreach_remove(processes, 
                                          processes_is_last_changed,
                                          &run);
    if(full_update) {
      rebuild_tree();
    }
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

int process_update_pid(int pid) {
  pid_t pids [2] = { pid, 0 };
  int rv;
  PROCTAB *proctab;
  proctab = openproc(OPENPROC_FLAGS | PROC_PID, pids);
  rv = update_processes_run(proctab, FALSE);
  closeproc(proctab);
  return rv;
}

int process_new(int pid) {
  u_proc *proc;
  // if the process is already dead we can exit
  if(!process_update_pid(pid))
    return 0;
  proc = proc_by_pid(pid);
  g_assert(proc);
  filter_for_proc(proc);
  scheduler_run_one(proc);
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
  if(!g_list_find(proc->flags, flag)) {
    proc->flags = g_list_insert(proc->flags, flag, 0);
    INC_REF(flag);
    }
  proc->changed = 1;
}

int u_flag_del(u_proc *proc, u_flag *flag) {
  if(g_list_index(proc->flags, flag) != -1) {
    DEC_REF(flag);
  }
  proc->flags = g_list_remove(proc->flags, flag);
  proc->changed = 1;
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


int u_flag_clear_source(u_proc *proc, void *source) {
  GList *item;
  u_flag *flg;
  while((item = g_list_find_custom(proc->flags, source, u_flag_match_source)) != NULL) {
    flg = (u_flag *)item->data;
    proc->flags = g_list_remove_link (proc->flags, item);
    DEC_REF(item->data);
    item->data = NULL;
    g_list_free(item);
  }
  proc->changed = 1;
}


int u_flag_clear_name(u_proc *proc, const char *name) {
  GList *item;
  u_flag *flg;
  while((item = g_list_find_custom(proc->flags, name, u_flag_match_name)) != NULL) {
    flg = (u_flag *)item->data;
    proc->flags = g_list_remove_link (proc->flags, item);
    DEC_REF(item->data);
    item->data = NULL;
    g_list_free(item);
  }
  proc->changed = 1;
}



int u_flag_clear_all(u_proc *proc) {
  GList *item;
  u_flag *flg;
  while((item = g_list_first(proc->flags)) != NULL) {
    flg = (u_flag *)item->data;
    proc->flags = g_list_remove_link (proc->flags, item);
    DEC_REF(item->data);
    item->data = NULL;
    g_list_free(item);
  }
  g_list_free(proc->flags);
  proc->changed = 1;
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
    time (&ttime);
    if(flt_block->skip)
      return 0;
    if(flt_block->timeout > ttime)
      return 0;
  }

  if(flt->check) {
    // if return 0 the real callback will be skipped
    if(!flt->check(proc, flt))
      return;
  }

  rv = flt->callback(proc, flt);

  if(rv == 0)
    return rv;

  if(!flt_block) {
    flt_block = malloc(sizeof(struct filter_block));
    g_hash_table_insert(proc->skip_filter, GUINT_TO_POINTER(flt), flt_block);
  }

  flt_block->pid = proc->proc.tgid;

  timeout = FILTER_TIMEOUT(rv);
  flags = FILTER_FLAGS(rv);

  if(timeout) {
    if(!ttime)
      time (&ttime);
    flt_block->timeout = ttime + abs(timeout);
  } else if(rv == FILTER_STOP) {
    flt_block->skip = TRUE;
  }

  return rv;
}

static GNode *blocked_parent;

gboolean filter_run_for_node(GNode *node, gpointer data) {
  GNode *tmp;
  int rv;
  u_filter *uf = data;
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
    //printf("cur %p\n", cur);
    flt = cur->data;
    blocked_parent = NULL;
    printf("children %d %d\n", g_node_n_children(processes_tree), g_node_n_nodes (processes_tree,G_TRAVERSE_ALL ));
    g_node_traverse(processes_tree, G_PRE_ORDER,G_TRAVERSE_ALL, -1, 
                    filter_run_for_node, flt);
    cur = g_list_next(cur);
  }
  blocked_parent = NULL;
}

static void update_caches() {
  double a, b;
  
  loadavg(&_last_load, &a, &b);
  _last_percent = (_last_load / (double)smp_num_cpus);

}


int iterate(gpointer ignored) {
  GTimer *timer = g_timer_new();
  gdouble last, current;
  gulong dump;

  g_timer_start(timer);
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
  return TRUE;
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



/***************************************************************************
 * rules and modules handling
 **************************************************************************/

int load_rule_directory(char *path, char *load_pattern) {
  DIR             *dip;
  struct dirent   *dit;
  char rpath[PATH_MAX+1];

  g_message("load rule directory: %s", path);

  if ((dip = opendir(path)) == NULL)
  {
    perror("opendir");
    return 0;
  }

  if(load_pattern)
    g_message("load pattern: %s", load_pattern);

  while ((dit = readdir(dip)) != NULL)
  {
    if(fnmatch("*.lua", dit->d_name, 0))
      continue;
    if(load_pattern && (fnmatch(load_pattern, dit->d_name, 0) != 0)) {
      g_debug("skip rule: %s", dit->d_name);
      continue;
    }

    snprintf(rpath, PATH_MAX, "%s/%s", path, dit->d_name);
    load_lua_rule_file(lua_main_state, rpath);
  }
  free(dip);
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
  g_free(disabled);
  free(dip);
}

int core_init() {
  // load config
  iteration = 1;
  filter_list = NULL;
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

  // FIXME path
  if(load_lua_rule_file(lua_main_state, "src/core.lua"))
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load core library");
}

void core_unload() {
  lua_gc (lua_main_state, LUA_GCCOLLECT, 0);
}

