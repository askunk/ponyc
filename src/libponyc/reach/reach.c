#include "reach.h"
#include "../codegen/genname.h"
#include "../pass/expr.h"
#include "../type/assemble.h"
#include "../type/lookup.h"
#include "../type/reify.h"
#include "../type/subtype.h"
#include "../../libponyrt/ds/stack.h"
#include "../../libponyrt/mem/pool.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

DECLARE_STACK(reachable_method_stack, reachable_method_stack_t,
  reachable_method_t);
DEFINE_STACK(reachable_method_stack, reachable_method_stack_t,
  reachable_method_t);

static reachable_type_t* add_type(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type, pass_opt_t* opt);

static void reachable_method(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type, const char* name,
  ast_t* typeargs, pass_opt_t* opt);

static void reachable_expr(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* ast, pass_opt_t* opt);

static size_t reachable_method_hash(reachable_method_t* m)
{
  return ponyint_hash_ptr(m->name);
}

static bool reachable_method_cmp(reachable_method_t* a, reachable_method_t* b)
{
  return a->name == b->name;
}

static void reachable_method_free(reachable_method_t* m)
{
  ast_free(m->typeargs);
  ast_free(m->r_fun);

  if(m->param_count > 0)
  {
    ponyint_pool_free_size(m->param_count * sizeof(reachable_type_t*),
      m->params);
  }

  POOL_FREE(reachable_method_t, m);
}

DEFINE_HASHMAP(reachable_methods, reachable_methods_t, reachable_method_t,
  reachable_method_hash, reachable_method_cmp, ponyint_pool_alloc_size,
  ponyint_pool_free_size, reachable_method_free);

static size_t reachable_method_name_hash(reachable_method_name_t* m)
{
  return ponyint_hash_ptr(m->name);
}

static bool reachable_method_name_cmp(reachable_method_name_t* a,
  reachable_method_name_t* b)
{
  return a->name == b->name;
}

static void reachable_method_name_free(reachable_method_name_t* m)
{
  reachable_methods_destroy(&m->r_methods);
  POOL_FREE(reachable_method_name_t, m);
}

DEFINE_HASHMAP(reachable_method_names, reachable_method_names_t,
  reachable_method_name_t, reachable_method_name_hash,
  reachable_method_name_cmp, ponyint_pool_alloc_size, ponyint_pool_free_size,
  reachable_method_name_free);

static size_t reachable_type_hash(reachable_type_t* t)
{
  return ponyint_hash_ptr(t->name);
}

static bool reachable_type_cmp(reachable_type_t* a, reachable_type_t* b)
{
  return a->name == b->name;
}

static void reachable_type_free(reachable_type_t* t)
{
  ast_free(t->ast);
  reachable_method_names_destroy(&t->methods);
  reachable_type_cache_destroy(&t->subtypes);

  if(t->field_count > 0)
  {
    for(uint32_t i = 0; i < t->field_count; i++)
      ast_free_unattached(t->fields[i].ast);

    free(t->fields);
    t->field_count = 0;
    t->fields = NULL;
  }

  POOL_FREE(reachable_type_t, t);
}

DEFINE_HASHMAP(reachable_types, reachable_types_t, reachable_type_t,
  reachable_type_hash, reachable_type_cmp, ponyint_pool_alloc_size,
  ponyint_pool_free_size, reachable_type_free);

DEFINE_HASHMAP(reachable_type_cache, reachable_type_cache_t, reachable_type_t,
  reachable_type_hash, reachable_type_cmp, ponyint_pool_alloc_size,
  ponyint_pool_free_size, NULL);

static reachable_method_t* reach_method_instance(reachable_method_name_t* n,
  const char* name)
{
  reachable_method_t k;
  k.name = name;
  return reachable_methods_get(&n->r_methods, &k);
}

static reachable_method_t* add_rmethod(reachable_method_stack_t** s,
  reachable_type_t* t, reachable_method_name_t* n,
  token_id cap, ast_t* typeargs, pass_opt_t* opt)
{
  const char* name = genname_fun(cap, n->name, typeargs);
  reachable_method_t* m = reach_method_instance(n, name);

  if(m != NULL)
    return m;

  m = POOL_ALLOC(reachable_method_t);
  memset(m, 0, sizeof(reachable_method_t));
  m->name = name;
  m->full_name = genname_funlong(t->name, name);
  m->cap = cap;
  m->typeargs = ast_dup(typeargs);
  m->vtable_index = (uint32_t)-1;

  ast_t* r_ast = set_cap_and_ephemeral(t->ast, cap, TK_NONE);
  ast_t* fun = lookup(NULL, NULL, r_ast, n->name);
  ast_free_unattached(r_ast);

  if(typeargs != NULL)
  {
    // Reify the method with its typeargs, if it has any.
    AST_GET_CHILDREN(fun, cap, id, typeparams, params, result, can_error,
      body);

    ast_t* r_fun = reify(fun, typeparams, typeargs, opt);
    ast_free_unattached(fun);
    fun = r_fun;
  }

  m->r_fun = fun;
  reachable_methods_put(&n->r_methods, m);

  // Put on a stack of reachable methods to trace.
  *s = reachable_method_stack_push(*s, m);
  return m;
}

static void add_method(reachable_method_stack_t** s,
  reachable_type_t* t, const char* name, ast_t* typeargs, pass_opt_t* opt)
{
  reachable_method_name_t* n = reach_method_name(t, name);

  if(n == NULL)
  {
    n = POOL_ALLOC(reachable_method_name_t);
    n->name = name;
    reachable_methods_init(&n->r_methods, 0);
    reachable_method_names_put(&t->methods, n);

    ast_t* fun = lookup(NULL, NULL, t->ast, name);
    n->cap = ast_id(ast_child(fun));
    ast_free_unattached(fun);
  }

  reachable_method_t* m = add_rmethod(s, t, n, n->cap, typeargs, opt);

  // TODO: if it doesn't use this-> in a constructor, we could reuse the
  // function, which means always reuse in a fun tag
  if((n->cap == TK_BOX) || (n->cap == TK_TAG))
  {
    bool subordinate = n->cap == TK_TAG;
    reachable_method_t* m2;

    if(t->underlying != TK_PRIMITIVE)
    {
      m2 = add_rmethod(s, t, n, TK_REF, typeargs, opt);

      if(subordinate)
      {
        m2->intrinsic = true;
        m->subordinate = m2;
        m = m2;
      }
    }

    m2 = add_rmethod(s, t, n, TK_VAL, typeargs, opt);

    if(subordinate)
    {
      m2->intrinsic = true;
      m->subordinate = m2;
      m = m2;
    }

    if(n->cap == TK_TAG)
    {
      m2 = add_rmethod(s, t, n, TK_BOX, typeargs, opt);
      m2->intrinsic = true;
      m->subordinate = m2;
      m = m2;
    }
  }

  // Add to subtypes if we're an interface or trait.
  if(ast_id(t->ast) != TK_NOMINAL)
    return;

  ast_t* def = (ast_t*)ast_data(t->ast);

  switch(ast_id(def))
  {
    case TK_INTERFACE:
    case TK_TRAIT:
    {
      size_t i = HASHMAP_BEGIN;
      reachable_type_t* t2;

      while((t2 = reachable_type_cache_next(&t->subtypes, &i)) != NULL)
        add_method(s, t2, name, typeargs, opt);

      break;
    }

    default: {}
  }
}

static void add_methods_to_type(reachable_method_stack_t** s,
  reachable_type_t* from, reachable_type_t* to, pass_opt_t* opt)
{
  size_t i = HASHMAP_BEGIN;
  reachable_method_name_t* n;

  while((n = reachable_method_names_next(&from->methods, &i)) != NULL)
  {
    size_t j = HASHMAP_BEGIN;
    reachable_method_t* m;

    while((m = reachable_methods_next(&n->r_methods, &j)) != NULL)
      add_method(s, to, n->name, m->typeargs, opt);
  }
}

static void add_types_to_trait(reachable_method_stack_t** s,
  reachable_types_t* r, reachable_type_t* t, pass_opt_t* opt)
{
  size_t i = HASHMAP_BEGIN;
  reachable_type_t* t2;

  ast_t* def = (ast_t*)ast_data(t->ast);
  bool interface = ast_id(def) == TK_INTERFACE;

  while((t2 = reachable_types_next(r, &i)) != NULL)
  {
    if(ast_id(t2->ast) != TK_NOMINAL)
      continue;

    ast_t* def2 = (ast_t*)ast_data(t2->ast);

    switch(ast_id(def2))
    {
      case TK_INTERFACE:
      {
        // Use the same typeid.
        if(interface && is_eqtype(t->ast, t2->ast, NULL, opt))
          t->type_id = t2->type_id;
        break;
      }

      case TK_PRIMITIVE:
      case TK_CLASS:
      case TK_ACTOR:
        if(is_subtype(t2->ast, t->ast, NULL, opt))
        {
          reachable_type_cache_put(&t->subtypes, t2);
          reachable_type_cache_put(&t2->subtypes, t);
          add_methods_to_type(s, t, t2, opt);
        }
        break;

      default: {}
    }
  }
}

static void add_traits_to_type(reachable_method_stack_t** s,
  reachable_types_t* r, reachable_type_t* t, pass_opt_t* opt)
{
  size_t i = HASHMAP_BEGIN;
  reachable_type_t* t2;

  while((t2 = reachable_types_next(r, &i)) != NULL)
  {
    if(ast_id(t2->ast) != TK_NOMINAL)
      continue;

    ast_t* def = (ast_t*)ast_data(t2->ast);

    switch(ast_id(def))
    {
      case TK_INTERFACE:
      case TK_TRAIT:
        if(is_subtype(t->ast, t2->ast, NULL, opt))
        {
          reachable_type_cache_put(&t->subtypes, t2);
          reachable_type_cache_put(&t2->subtypes, t);
          add_methods_to_type(s, t2, t, opt);
        }
        break;

      default: {}
    }
  }
}

static void add_fields(reachable_method_stack_t** s, reachable_types_t* r,
  reachable_type_t* t, uint32_t* next_type_id, pass_opt_t* opt)
{
  ast_t* def = (ast_t*)ast_data(t->ast);
  ast_t* typeargs = ast_childidx(t->ast, 2);
  ast_t* typeparams = ast_childidx(def, 1);
  ast_t* members = ast_childidx(def, 4);
  ast_t* member = ast_child(members);

  while(member != NULL)
  {
    switch(ast_id(member))
    {
      case TK_FVAR:
      case TK_FLET:
      case TK_EMBED:
      {
        t->field_count++;
        break;
      }

      default: {}
    }

    member = ast_sibling(member);
  }

  if(t->field_count == 0)
    return;

  t->fields = (reachable_field_t*)calloc(t->field_count,
    sizeof(reachable_field_t));
  member = ast_child(members);
  size_t index = 0;

  while(member != NULL)
  {
    switch(ast_id(member))
    {
      case TK_FVAR:
      case TK_FLET:
      case TK_EMBED:
      {
        ast_t* r_member = lookup(NULL, NULL, t->ast,
          ast_name(ast_child(member)));
        assert(r_member != NULL);

        AST_GET_CHILDREN(r_member, name, type, init);

        t->fields[index].embed = ast_id(member) == TK_EMBED;
        t->fields[index].ast = reify(ast_type(member), typeparams, typeargs,
          opt);
        ast_setpos(t->fields[index].ast, NULL, ast_line(name), ast_pos(name));
        t->fields[index].type = add_type(s, r, next_type_id, type, opt);

        if(r_member != member)
          ast_free_unattached(r_member);

        index++;
        break;
      }

      default: {}
    }

    member = ast_sibling(member);
  }
}

static void add_special(reachable_method_stack_t** s, reachable_type_t* t,
  ast_t* type, const char* special, pass_opt_t* opt)
{
  special = stringtab(special);
  ast_t* find = lookup_try(NULL, NULL, type, special);

  if(find != NULL)
  {
    add_method(s, t, special, NULL, opt);
    ast_free_unattached(find);
  }
}

static reachable_type_t* add_reachable_type(reachable_types_t* r, ast_t* type)
{
  reachable_type_t* t = POOL_ALLOC(reachable_type_t);
  memset(t, 0, sizeof(reachable_type_t));

  t->name = genname_type(type);
  t->ast = set_cap_and_ephemeral(type, TK_REF, TK_NONE);
  reachable_method_names_init(&t->methods, 0);
  reachable_type_cache_init(&t->subtypes, 0);
  reachable_types_put(r, t);

  return t;
}

static reachable_type_t* add_isect_or_union(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type, pass_opt_t* opt)
{
  reachable_type_t* t = reach_type(r, type);

  if(t != NULL)
    return t;

  t = add_reachable_type(r, type);
  t->type_id = ++(*next_type_id);

  ast_t* child = ast_child(type);

  while(child != NULL)
  {
    add_type(s, r, next_type_id, child, opt);
    child = ast_sibling(child);
  }

  return t;
}

static reachable_type_t* add_tuple(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type, pass_opt_t* opt)
{
  if(contains_dontcare(type))
    return NULL;

  reachable_type_t* t = reach_type(r, type);

  if(t != NULL)
    return t;

  t = add_reachable_type(r, type);
  t->type_id = ++(*next_type_id);

  t->field_count = (uint32_t)ast_childcount(t->ast);
  t->fields = (reachable_field_t*)calloc(t->field_count,
    sizeof(reachable_field_t));
  size_t index = 0;

  ast_t* child = ast_child(type);

  while(child != NULL)
  {
    t->fields[index].ast = ast_dup(child);
    t->fields[index].type = add_type(s, r, next_type_id, child, opt);;
    index++;

    child = ast_sibling(child);
  }

  return t;
}

static reachable_type_t* add_nominal(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type,
  pass_opt_t* opt)
{
  reachable_type_t* t = reach_type(r, type);

  if(t != NULL)
    return t;

  t = add_reachable_type(r, type);

  AST_GET_CHILDREN(type, pkg, id, typeparams);
  ast_t* typeparam = ast_child(typeparams);

  while(typeparam != NULL)
  {
    add_type(s, r, next_type_id, typeparam, opt);
    typeparam = ast_sibling(typeparam);
  }

  ast_t* def = (ast_t*)ast_data(type);

  switch(ast_id(def))
  {
    case TK_INTERFACE:
    case TK_TRAIT:
      add_types_to_trait(s, r, t, opt);
      break;

    case TK_PRIMITIVE:
      add_traits_to_type(s, r, t, opt);
      add_special(s, t, type, "_init", opt);
      add_special(s, t, type, "_final", opt);
      break;

    case TK_STRUCT:
    case TK_CLASS:
      add_traits_to_type(s, r, t, opt);
      add_special(s, t, type, "_final", opt);
      add_fields(s, r, t, next_type_id, opt);
      break;

    case TK_ACTOR:
      add_traits_to_type(s, r, t, opt);
      add_special(s, t, type, "_event_notify", opt);
      add_special(s, t, type, "_final", opt);
      add_fields(s, r, t, next_type_id, opt);
      break;

    default: {}
  }

  if(t->type_id == 0)
    t->type_id = ++(*next_type_id);

  return t;
}

static reachable_type_t* add_type(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type,
  pass_opt_t* opt)
{
  switch(ast_id(type))
  {
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
      return add_isect_or_union(s, r, next_type_id, type, opt);

    case TK_TUPLETYPE:
      return add_tuple(s, r, next_type_id, type, opt);

    case TK_NOMINAL:
      return add_nominal(s, r, next_type_id, type, opt);

    default:
      assert(0);
  }

  return NULL;
}

static void reachable_pattern(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* ast,
  pass_opt_t* opt)
{
  switch(ast_id(ast))
  {
    case TK_DONTCARE:
    case TK_NONE:
      break;

    case TK_MATCH_CAPTURE:
    {
      AST_GET_CHILDREN(ast, idseq, type);
      add_type(s, r, next_type_id, type, opt);
      break;
    }

    case TK_TUPLE:
    case TK_SEQ:
    {
      ast_t* child = ast_child(ast);

      while(child != NULL)
      {
        reachable_pattern(s, r, next_type_id, child, opt);
        child = ast_sibling(child);
      }
      break;
    }

    default:
    {
      reachable_method(s, r, next_type_id, ast_type(ast), stringtab("eq"),
        NULL, opt);
      reachable_expr(s, r, next_type_id, ast, opt);
      break;
    }
  }
}

static void reachable_fun(reachable_method_stack_t** s, reachable_types_t* r,
  uint32_t* next_type_id, ast_t* ast, pass_opt_t* opt)
{
  AST_GET_CHILDREN(ast, receiver, method);
  ast_t* typeargs = NULL;

  // Dig through function qualification.
  switch(ast_id(receiver))
  {
    case TK_NEWREF:
    case TK_NEWBEREF:
    case TK_BEREF:
    case TK_FUNREF:
      typeargs = method;
      AST_GET_CHILDREN_NO_DECL(receiver, receiver, method);
      break;

    default: {}
  }

  ast_t* type = ast_type(receiver);
  const char* method_name = ast_name(method);

  reachable_method(s, r, next_type_id, type, method_name, typeargs, opt);
}

static void reachable_addressof(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* ast, pass_opt_t* opt)
{
  ast_t* expr = ast_child(ast);

  switch(ast_id(expr))
  {
    case TK_FUNREF:
    case TK_BEREF:
      reachable_fun(s, r, next_type_id, expr, opt);
      break;

    default: {}
  }
}

static void reachable_call(reachable_method_stack_t** s, reachable_types_t* r,
  uint32_t* next_type_id, ast_t* ast, pass_opt_t* opt)
{
  AST_GET_CHILDREN(ast, positional, named, postfix);
  reachable_fun(s, r, next_type_id, postfix, opt);
}

static void reachable_ffi(reachable_method_stack_t** s, reachable_types_t* r,
  uint32_t* next_type_id, ast_t* ast, pass_opt_t* opt)
{
  AST_GET_CHILDREN(ast, name, return_typeargs, args, namedargs, question);
  ast_t* decl = (ast_t*)ast_data(ast);

  if(decl != NULL)
  {
    AST_GET_CHILDREN(decl, decl_name, decl_ret_typeargs, params, named_params,
      decl_error);

    args = params;
    return_typeargs = decl_ret_typeargs;
  }

  ast_t* return_type = ast_child(return_typeargs);
  add_type(s, r, next_type_id, return_type, opt);

  ast_t* arg = ast_child(args);

  while(arg != NULL)
  {
    if(ast_id(arg) != TK_ELLIPSIS)
    {
      ast_t* type = ast_type(arg);

      if(type == NULL)
        type = ast_childidx(arg, 1);

      add_type(s, r, next_type_id, type, opt);
    }

    arg = ast_sibling(arg);
  }
}

static void reachable_expr(reachable_method_stack_t** s, reachable_types_t* r,
  uint32_t* next_type_id, ast_t* ast, pass_opt_t* opt)
{
  // If this is a method call, mark the method as reachable.
  switch(ast_id(ast))
  {
    case TK_TRUE:
    case TK_FALSE:
    case TK_INT:
    case TK_FLOAT:
    case TK_STRING:
    {
      ast_t* type = ast_type(ast);

      if(type != NULL)
        reachable_method(s, r, next_type_id, type, stringtab("create"), NULL,
          opt);
      break;
    }

    case TK_LET:
    case TK_VAR:
    case TK_TUPLE:
    {
      ast_t* type = ast_type(ast);
      add_type(s, r, next_type_id, type, opt);
      break;
    }

    case TK_CASE:
    {
      AST_GET_CHILDREN(ast, pattern, guard, body);
      reachable_pattern(s, r, next_type_id, pattern, opt);
      reachable_expr(s, r, next_type_id, guard, opt);
      reachable_expr(s, r, next_type_id, body, opt);
      break;
    }

    case TK_CALL:
      reachable_call(s, r, next_type_id, ast, opt);
      break;

    case TK_FFICALL:
      reachable_ffi(s, r, next_type_id, ast, opt);
      break;

    case TK_ADDRESS:
      reachable_addressof(s, r, next_type_id, ast, opt);
      break;

    case TK_IF:
    {
      AST_GET_CHILDREN(ast, cond, then_clause, else_clause);
      assert(ast_id(cond) == TK_SEQ);
      cond = ast_child(cond);

      ast_t* type = ast_type(ast);

      if(is_result_needed(ast) && !is_control_type(type))
        add_type(s, r, next_type_id, type, opt);

      if(ast_sibling(cond) == NULL)
      {
        if(ast_id(cond) == TK_TRUE)
        {
          reachable_expr(s, r, next_type_id, then_clause, opt);
          return;
        } else if(ast_id(cond) == TK_FALSE) {
          reachable_expr(s, r, next_type_id, else_clause, opt);
          return;
        }
      }
      break;
    }

    case TK_MATCH:
    case TK_WHILE:
    case TK_REPEAT:
    case TK_TRY:
    {
      ast_t* type = ast_type(ast);

      if(is_result_needed(ast) && !is_control_type(type))
        add_type(s, r, next_type_id, type, opt);

      break;
    }

    default: {}
  }

  // Traverse all child expressions looking for calls.
  ast_t* child = ast_child(ast);

  while(child != NULL)
  {
    reachable_expr(s, r, next_type_id, child, opt);
    child = ast_sibling(child);
  }
}

static void reachable_method(reachable_method_stack_t** s,
  reachable_types_t* r, uint32_t* next_type_id, ast_t* type,
  const char* name, ast_t* typeargs, pass_opt_t* opt)
{
  switch(ast_id(type))
  {
    case TK_NOMINAL:
    {
      reachable_type_t* t = add_type(s, r, next_type_id, type, opt);
      add_method(s, t, name, typeargs, opt);
      return;
    }

    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
    {
      reachable_type_t* t = add_type(s, r, next_type_id, type, opt);
      add_method(s, t, name, typeargs, opt);
      ast_t* child = ast_child(type);

      while(child != NULL)
      {
        ast_t* find = lookup_try(NULL, NULL, child, name);

        if(find != NULL)
        {
          reachable_method(s, r, next_type_id, child, name, typeargs, opt);
          ast_free_unattached(find);
        }

        child = ast_sibling(child);
      }

      return;
    }

    default: {}
  }

  assert(0);
}

static void handle_stack(reachable_method_stack_t* s, reachable_types_t* r,
  uint32_t* next_type_id, pass_opt_t* opt)
{
  while(s != NULL)
  {
    reachable_method_t* m;
    s = reachable_method_stack_pop(s, &m);

    AST_GET_CHILDREN(m->r_fun, cap, id, typeparams, params, result, can_error,
      body);

    m->param_count = ast_childcount(params);
    m->params = (reachable_type_t**)ponyint_pool_alloc_size(
      m->param_count * sizeof(reachable_type_t*));

    ast_t* param = ast_child(params);
    size_t i = 0;

    while(param != NULL)
    {
      AST_GET_CHILDREN(param, p_id, p_type);
      m->params[i++] = add_type(&s, r, next_type_id, p_type, opt);
      param = ast_sibling(param);
    }

    m->result = add_type(&s, r, next_type_id, result, opt);
    reachable_expr(&s, r, next_type_id, body, opt);
  }
}

reachable_types_t* reach_new()
{
  reachable_types_t* r = POOL_ALLOC(reachable_types_t);
  reachable_types_init(r, 64);
  return r;
}

void reach_free(reachable_types_t* r)
{
  if(r == NULL)
    return;

  reachable_types_destroy(r);
  POOL_FREE(reachable_types_t, r);
}

void reach(reachable_types_t* r, uint32_t* next_type_id, ast_t* type,
  const char* name, ast_t* typeargs, pass_opt_t* opt)
{
  reachable_method_stack_t* s = NULL;
  reachable_method(&s, r, next_type_id, type, name, typeargs, opt);
  handle_stack(s, r, next_type_id, opt);
}

reachable_type_t* reach_type(reachable_types_t* r, ast_t* type)
{
  reachable_type_t k;
  k.name = genname_type(type);
  return reachable_types_get(r, &k);
}

reachable_type_t* reach_type_name(reachable_types_t* r, const char* name)
{
  reachable_type_t k;
  k.name = stringtab(name);
  return reachable_types_get(r, &k);
}

reachable_method_t* reach_method(reachable_type_t* t, token_id cap,
  const char* name, ast_t* typeargs)
{
  reachable_method_name_t* n = reach_method_name(t, name);

  if(n == NULL)
    return NULL;

  if((n->cap == TK_BOX) || (n->cap == TK_TAG))
  {
    switch(cap)
    {
      case TK_REF:
      case TK_VAL:
      case TK_BOX:
        break;

      default:
        cap = n->cap;
    }
  } else {
    cap = n->cap;
  }

  name = genname_fun(cap, n->name, typeargs);
  return reach_method_instance(n, name);
}

reachable_method_name_t* reach_method_name(reachable_type_t* t,
  const char* name)
{
  reachable_method_name_t k;
  k.name = name;
  return reachable_method_names_get(&t->methods, &k);
}

uint32_t reach_vtable_index(reachable_type_t* t, const char* name)
{
  reachable_method_t* m = reach_method(t, TK_NONE, stringtab(name), NULL);

  if(m == NULL)
    return (uint32_t)-1;

  return m->vtable_index;
}

void reach_dump(reachable_types_t* r)
{
  printf("REACH\n");

  size_t i = HASHMAP_BEGIN;
  reachable_type_t* t;

  while((t = reachable_types_next(r, &i)) != NULL)
  {
    printf("  %s: %d\n", t->name, t->vtable_size);
    size_t j = HASHMAP_BEGIN;
    reachable_method_name_t* m;

    while((m = reachable_method_names_next(&t->methods, &j)) != NULL)
    {
      size_t k = HASHMAP_BEGIN;
      reachable_method_t* p;

      while((p = reachable_methods_next(&m->r_methods, &k)) != NULL)
        printf("    %s: %d\n", p->name, p->vtable_index);
    }
  }
}
