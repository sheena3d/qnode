#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ldb.h"
#include "ldb_util.h"

#define LDB_MAX_INPUT 200
#define LDB_MAX_PARAM 5

static const char* lua_debugger_tag = "__ldb_debugger";

typedef struct input_t {
  char    buffer[LDB_MAX_INPUT][LDB_MAX_PARAM];
  int     num;
} input_t;

static void single_step(ldb_t *ldb, int step);
static void enable_line_hook(lua_State *state, int enable);
static void line_hook(lua_State *state, lua_Debug *ar);
//static void func_hook(lua_State *state, lua_Debug *ar);
static int  get_input(char *buff, int size);
static void set_prompt();
static int  split_input(const char *buff, input_t *input);
static int  search_local_var(lua_State *state, lua_Debug *ar,
                             const char* var);

static int  search_global_var(lua_State *state, lua_Debug *ar,
                              const char* var);

static void print_var(lua_State *state, int si, int depth);
static void print_table_var(lua_State *state, int si, int depth);
static void print_string_var(lua_State *state, int si, int depth);
static void dump_stack(lua_State *state, int depth, int verbose); 

static int help_handler(lua_State *state,  lua_Debug *ar, input_t *input);
static int quit_handler(lua_State *state,  lua_Debug *ar, input_t *input);
static int print_handler(lua_State *state, lua_Debug *ar, input_t *input);
static int backtrace_handler(lua_State *state, lua_Debug *ar, input_t *input);
static int list_handler(lua_State *state, lua_Debug *ar, input_t *input);

typedef int (*handler_t)(lua_State *state, lua_Debug *ar, input_t *input);

typedef struct ldb_command_t {
  const char* name;
  const char* help;
  handler_t   handler;
} ldb_command_t;

ldb_command_t commands[] = {
  {"help",  "h(help): print help info",     &help_handler},
  {"h",     NULL,                           &help_handler},

  {"quit",  NULL,                           &quit_handler},
  {"q",     "q(quit): quit ldb",            &quit_handler},

  {"p",     "p <varname>: print var value", &print_handler},

  {"bt",    "bt: print backtrace info",     &backtrace_handler},

  {"list",  "l(list): list file source",    &list_handler},
  {"l",     NULL,                           &list_handler},

  {NULL,    NULL,                           NULL},
};

ldb_t*
ldb_new(lua_State *state) {
  int    i;
  ldb_t *ldb;

  ldb = (ldb_t*)malloc(sizeof(ldb_t));
  if (ldb == NULL) {
    return NULL;
  }
  ldb->step = 0;
  ldb->call_depth = 0;

  lua_pushstring(state, lua_debugger_tag);
  lua_pushlightuserdata(state, ldb);
  lua_settable(state, LUA_REGISTRYINDEX);

  for (i = 0; i < MAX_FILE_BUCKET; ++i) {
    ldb->files[i] = NULL;
  }
  return ldb;
}

void
ldb_destroy(ldb_t *ldb) {
  int         i;
  ldb_file_t *file, *next;

  for (i = 0; i < MAX_FILE_BUCKET; ++i) {
    file = ldb->files[i];
    while (file) {
      next = file->next;
      ldb_file_free(file);
      file = next;
    }
  }
  free(ldb);
}

void
ldb_attach(ldb_t *ldb, lua_State *state) {
  lua_pushlightuserdata(state, state);
  lua_pushlightuserdata(state, ldb);
  lua_settable(state, LUA_REGISTRYINDEX);
}

void
ldb_step_in(lua_State *state, int step) {
  ldb_t *ldb;

  lua_pushlightuserdata(state, state);
  lua_gettable(state, LUA_REGISTRYINDEX);
  ldb = (ldb_t*)lua_touserdata(state, -1);
  if (ldb == NULL) {
    return;
  }
  ldb->state = state;
  if (ldb->step == 0) {
    single_step(ldb, 1);
  }

  ldb->call_depth = -1;
}

static void
single_step(ldb_t *ldb, int step) {
  if (step) {
    enable_line_hook(ldb->state, 1);
  }

  ldb->step = step;
}

static void
enable_line_hook(lua_State *state, int enable) {
  int mask;
  
  mask = lua_gethookmask(state);
  if (enable) {
    lua_sethook(state, line_hook, mask | LUA_MASKLINE, 0); 
  } else {
    lua_sethook(state, line_hook, mask & ~LUA_MASKLINE, 0); 
  }
}

static int
get_input(char *buff, int size) {
  int len = read(STDIN_FILENO, buff, size);
  if(len > 0) { 
    buff[len - 1] = '\0';
    return len - 1;
  }   
  return -1;
}

static int
split_input(const char *buff, input_t *input) {
   const char *p, *save;
   int i;

   save = p = buff;
   save = NULL;
   i = 0;
   while (p && *p) {
     if (*p == '\n') {
       break;
     }

     if (isspace(*p)) {
       if (save) {
         if (i > LDB_MAX_PARAM) {
           ldb_output("param %s more than %d", buff, LDB_MAX_PARAM);
           return -1;
         }
         strncpy(input->buffer[i++], save, p - save);
         save = NULL;
       }
     } else {
       if (save == NULL) {
         save = p;
       }
     }

     ++p;
   }
   if (i > LDB_MAX_PARAM) {
     ldb_output("param %s more than %d", buff, LDB_MAX_PARAM);
     return -1;
   }
   strcpy(input->buffer[i++], save);
   input->num = i;

   /*
   ldb_output("input: ");
   int j = 0;
   for (j = 0; j < i; ++j) {
     ldb_output("%s +", input[j]);
   }
   ldb_output("\n");
   */

   return 0;
}

static void
line_hook(lua_State *state, lua_Debug *ar) {
  input_t input;;

  if(!lua_getstack(state, 0, ar)) { 
    ldb_output("[LUA_DEBUG]lua_getstack fail\n");
    return;
  }

  //if(lua_getinfo(state, "lnS", ar)) {
  if(lua_getinfo(state, "lnSu", ar)) {
    set_prompt();
  } else {
    ldb_output("[LUA_DEBUG]lua_getinfo fail\n");
    return;
  }

   char buff[LDB_MAX_INPUT];
   int ret, i;
   while (get_input(&buff[0], sizeof(buff)) > 0) {
     if (split_input(&(buff[0]), &input) < 0) {
       set_prompt();
       continue;
     }

     ret = 0;
     for (i = 0; commands[i].handler != NULL; ++i) {
       if (strcmp(input.buffer[0], commands[i].name) == 0) {
         ret = (*commands[i].handler)(state, ar, &input);
         break;
       }
     }
     if (commands[i].name == NULL) {
       ldb_output("bad command: %s, ldb_output h for help\n", buff);
     }
     //input.clear();
     if (ret < 0) {
       break;
     }
     set_prompt();
   }
}

/*
static void
func_hook(lua_State *state, lua_Debug *ar) {
}
*/

static void
set_prompt() {   
  ldb_output("(ldb) ");
}  

static int
help_handler(lua_State *state, lua_Debug *ar, input_t *input) {
  int i;

  ldb_output("Lua debugger written by Lichuang(2013)\ncmd:\n");
  for (i = 0; commands[i].name != NULL; ++i) {
    if (commands[i].help) {
      ldb_output("\t%s\n", commands[i].help);
    }
  }
  return 0;
}

static int
quit_handler(lua_State *state, lua_Debug *ar, input_t *input) {
  //ldb_output( "Continue...\n" );
  enable_line_hook(state, 0);

  return -1;
} 

static int
print_handler(lua_State *state, lua_Debug *ar, input_t *input) {
  if (input->num < 2) {
    ldb_output("usage: p <varname>\n");
    return 0;
  }

  if (search_local_var(state, ar, input->buffer[1])) {
    ldb_output("local %s =", input->buffer[1]);
    print_var(state, -1, -1);
    lua_pop(state, 1);
    ldb_output("\n");
  } else if (search_global_var(state, ar, input->buffer[1])) {
    ldb_output("global %s =", input->buffer[1]);
    print_var(state, -1, -1);
    lua_pop(state, 1);
    ldb_output("\n");
  } else {
    ldb_output("not found var %s\n",  input->buffer[1]);
  }

  return 0;
}

static int
search_local_var(lua_State *state, lua_Debug *ar, const char* var) {
  int         i;
  const char *name;

  for(i = 1; (name = lua_getlocal(state, ar, i)) != NULL; i++) {
    if(strcmp(var,name) == 0) {
      return i;
    }    
    // not match, pop out the var's value
    lua_pop(state, 1); 
  }
  return 0;
}

static int
search_global_var(lua_State *state, lua_Debug *ar, const char* var) {
  lua_getglobal(state, var);

  if(lua_type(state, -1 ) == LUA_TNIL) {
    lua_pop(state, 1);
    return 0;
  }   

  return 1;
}

static void
print_table_var(lua_State *state, int si, int depth) {
  int pos_si = si > 0 ? si : (si - 1);
  ldb_output("{");
  int top = lua_gettop(state);
  lua_pushnil(state);
  int empty = 1;
  while(lua_next(state, pos_si ) !=0) {
    if(empty) {
      ldb_output("\n");
      empty = 0;
    }

    int i;
    for(i = 0; i < depth; i++) {
      ldb_output("\t");
    }

    ldb_output( "[" );
    print_var(state, -2, -1);
    ldb_output( "] = " );
    if(depth > 5) {
      ldb_output("{...}");
    } else {
      print_var(state, -1, depth + 1);
    }
    lua_pop(state, 1);
    ldb_output(",\n");
  }

  if (empty) {
    ldb_output(" }");
  } else {
    int i;
    for (i = 0; i < depth - 1; i++) {
      ldb_output("\t");
    }
    ldb_output("}");
  }
  lua_settop(state, top);
}

static void 
print_string_var(lua_State *state, int si, int depth) {
  ldb_output( "\"" );

  const char * val = lua_tostring(state, si);
  int vallen = lua_strlen(state, si);
  int i;
  const char spchar[] = "\"\t\n\r";
  for(i = 0; i < vallen; ) {
    if(val[i] == 0) {
      ldb_output("\\000");
      ++i;
    } else if (val[i] == '"') {
      ldb_output("\\\"");
      ++i;
    } else if(val[i] == '\\') {
      ldb_output("\\\\");
      ++i;
    } else if(val[i] == '\t') {
      ldb_output("\\t");
      ++i;
    } else if(val[i] == '\n') {
      ldb_output("\\n");
      ++i;
    } else if(val[i] == '\r') {
      ldb_output("\\r");
      ++i;
    } else {
      int splen = strcspn(val + i, spchar);

      ldb_output("%.*s", splen, val+i);
      i += splen;
    }
  }
  ldb_output("\"");
}

static void
dump_stack(lua_State *state, int depth, int verbose) {
  lua_Debug ldb; 
  int i;
  const char *name, *filename;
  /*
  int addr_len;
  char fn[4096];
  */

  for(i = depth; lua_getstack(state, i, &ldb) == 1; i++) {
    lua_getinfo(state, "Slnu", &ldb);
    name = ldb.name;
    if(!name) {
      name = "";
    }    
    filename = ldb.source;

    ldb_output("#%d: %s:'%s', '%s' line %d\n",
           i + 1 - depth, ldb.what, name,
           filename, ldb.currentline );
    /*
    addr_len = strlen(strlcpy( fn, search_path_, 4096)); 
    strlcpy(fn + addr_len, filename + 3, 4096-addr_len ); // @./
    */
    /*
    if(verbose) {
      if( ldb.source[0]=='@' && ldb.currentline!=-1 ) {
        const char * line = src_manager_->load_file_line( fn, ldb.currentline-1 );
        if( line ) {
          print( "%s\n", line );
        } else {
          print( "[no source available]\n" );
        }    
      } else {
        print( "[no source available]\n" );
      }
    }
    */
  }
}

static void
print_var(lua_State *state, int si, int depth) {
  switch(lua_type(state, si)) {
  case LUA_TNIL:
    ldb_output("(nil)");
    break;

  case LUA_TNUMBER:
    ldb_output("%f", lua_tonumber(state, si));
    break;

  case LUA_TBOOLEAN:
    ldb_output("%s", lua_toboolean(state, si) ? "true":"false");
    break;

  case LUA_TFUNCTION:
    {
      lua_CFunction func = lua_tocfunction(state, si);
      if( func != NULL ) {
        ldb_output("(C function)0x%p", func);
      } else {
        ldb_output("(function)");
      }
    }
    break;

  case LUA_TUSERDATA:
    ldb_output("(user data)0x%p", lua_touserdata(state, si));
    break;

  case LUA_TSTRING:
    print_string_var(state, si, depth);
    break;

  case LUA_TTABLE:
    print_table_var(state, si, depth);
    break;

  default:
    break;
  }
}

static int
backtrace_handler(lua_State *state, lua_Debug *ar, input_t *input) {
  dump_stack(state, 0, 0);

  return 0;
}

static int
list_handler(lua_State *state, lua_Debug *ar, input_t *input) {
  ldb_file_t *file;
  ldb_t      *ldb;
  int         i, j;

  lua_pushstring(state, lua_debugger_tag);
  lua_gettable(state, LUA_REGISTRYINDEX);
  ldb = (ldb_t*)lua_touserdata(state, -1);
  if (ldb == NULL) {
    return -1;
  }

  /* ignore `@` char */
  file = ldb_file_load(ldb, ar->source + 1);
  if (file == NULL) {
    return 0;
  }

  i = ar->currentline - 5;
  if (i < 0) {
    i = 0;
  }
  for (; i < ar->currentline; ++i) {
    ldb_output("%s:%d\t%s", file->name, i, file->lines[i]);
  }
  for (i = ar->currentline, j = 0; j < 6 && i < file->line; ++j, ++i) {
    ldb_output("%s:%d\t%s", file->name, i, file->lines[i]);
  }
  return 0;
}