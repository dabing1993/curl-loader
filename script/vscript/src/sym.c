/*
*
* 2007 Copyright (c) 
* Michael Moser,  <moser.michael@gmail.com>                 
* All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include "sym.h"
#include "asm.h"
#include "xlib.h"
#include "syntax.h"
#include <string.h>


/*
  Function name - hash_compare
 
  Description - callback function, used by symbol table hash to compare string keys in entries;
				entries are assumed to be of type STR2AST.				
 
  Input -       
 
  Return Code/Output 0 keys are equal.
 */
static int hash_compare(VBUCKETHASH_Entry *entry, void * key, size_t key_length)
{
	STR2AST *lhs;
	
	V_UNUSED(key_length);

	lhs = (STR2AST *) entry;

	if (strcmp(lhs->name,key) == 0) {
		return 0;
	}
	return 1;
}

int SYMBOLS_init(VBUCKETHASH  *hash)
{
	if (VBUCKETHASH_init_multimap(					
					0,
					hash, 
					10, 
					0,
					hash_compare)) {
		return -1;
	}
	return 0;
}

int SYMBOLS_define(VBUCKETHASH  *hash, const char *key, struct tagAST_BASE *entry)
{
	STR2AST *hentry;

	hentry = (STR2AST *) malloc(sizeof(STR2AST));
	if (!hentry) {
		return -1;
	}
	hentry->name = key;
	hentry->ast  = entry;
	
	return VBUCKETHASH_insert( hash, &hentry->entry, (char *) hentry->name, VHASH_STRING );
}


/*
  Function name - SYMBOLS_find_function_call
 
  Description - 
  
		for a argument AST node of function call (argument fcall), find a matching
		function definition (i.e. function of same name and matching argument list)
		and return FUNC_DECL_WITH_SAME_SIGNATURE

		in other cases we have a near match
			- function exists but with different signature, return FUNC_DECL_WITH_DIFF_SIGNATURE
			- entry with given name exists, but is not a function, return NOT_FUNC_DECL
			
		if no matches are found then we retry to search for a matching function in extension library.
		TODO: what to do with near misses in extension library ?
 
  Input - fcall - function cal ast node
 
  Return Code/Output status code explained in description	
		found - the matching function definition if we return FUNC_DECL_WITH_SAME_SIGNATURE
 */
SYM_SCOPE_RET SYMBOLS_find_function_call(VBUCKETHASH  *hash, struct tagAST_FUNCTION_CALL *fcall, struct tagAST_FUNCTION_DECL **found, struct tagAST_BASE **err )
{
	STR2AST *hentry;
	SYM_SCOPE_RET ret = NOTHING_DECL;

	*err = 0;

	hentry = (STR2AST *) VBUCKETHASH_find(hash, (void *) fcall->name, VHASH_STRING );
	if (hentry) {
		do {
			*err = hentry->ast;

			if (hentry->ast->type != S_FUN_DECL) {
				return NOT_FUNC_DECL;
			}

			ret = FUNC_DECL_WITH_DIFF_SIGNATURE; 

			/*TODO: check if expression type of function call matches expression of function parameters
			  if yes then return FUNC_DECL_WITH_SAME_SIGNATURE and set found retval
			 */
			if (! AST_compare_fcall(fcall,(AST_FUNCTION_DECL *) hentry->ast)) {
				*found = (AST_FUNCTION_DECL *) hentry->ast;
				ret = FUNC_DECL_WITH_SAME_SIGNATURE;
				break;
			}
		

			hentry = (STR2AST *) VBUCKETHASH_find_next( hash, &hentry->entry, fcall->name, VHASH_STRING );
		
		} while( hentry );	
	}
	return ret;

}

/*
  Function name - SYMBOLS_find_function_def
 
  Description -
		searches global scope for symbol with name that match argument function declaration (argument fdef)
		if one of the entries is a function that matches argument list of fdef then we have a match
		and return FUNC_DECL_WITH_SAME_SIGNATURE

		in other cases we have a near match
			- function exists but with different signature, return FUNC_DECL_WITH_DIFF_SIGNATURE
			- entry with given name exists, but is not a function, return NOT_FUNC_DECL
			
		if no matches are found then we retry to search for a matching function in extension library.
		TODO: what to do with near misses in extension library ?
 
  Input -  fdef - AST node  for function declaration      
 
  Return Code/Output status code explained in description
					 err - item that caused near miss.
 */

SYM_SCOPE_RET SYMBOLS_find_function_def(VBUCKETHASH  *hash, struct tagAST_FUNCTION_DECL *fdef, struct tagAST_BASE **err)
{
	STR2AST *hentry;
	SYM_SCOPE_RET ret = NOTHING_DECL;

	if (err) {
		*err = 0;
	}

	hentry = (STR2AST *) VBUCKETHASH_find(hash, (void *) fdef->name, VHASH_STRING );
	if (hentry) {
		do {
			if (err) {
				*err = hentry->ast;
			}

			if (hentry->ast->type != S_FUN_DECL) {
				ret = NOT_FUNC_DECL;
				break;
			}

			ret = FUNC_DECL_WITH_DIFF_SIGNATURE;

			/* check if found function is of the same prototoype as argument */

			if (! AST_compare_signatures( fdef, (AST_FUNCTION_DECL *) hentry->ast )) {
				ret = FUNC_DECL_WITH_SAME_SIGNATURE;
				break;
			} 

			hentry = (STR2AST *) VBUCKETHASH_find_next( hash, &hentry->entry, fdef->name, VHASH_STRING );
		} while( hentry );	
	}
	return ret;
}

/* *** symbol table *** */


typedef struct tagREUSESYMBOL {
	SYM_LOCATION location;
	int	is_in_use;
} REUSESYMBOL;


/*
  Function name - SYM_SCOPE_push
 
  Description -
		create new symbol table for a new lexical scope 
		symbol tables are kept in a stack
 
  Input -       
 
  Return Code/Output 
 */
int SYM_SCOPE_push()
{
	SYM_SCOPE *sc = (SYM_SCOPE *) malloc(sizeof(SYM_SCOPE));
	
	if (!sc) {
		return -1;
	}

	sc->parent = vscript_ctx->current_scope;
	sc->fdecl = 0;
	sc->next_storage = 1; // location 0 is reserved for return value
	
	/* can have functions with the same name but different parameter lists - therefore multimap */
	if (SYMBOLS_init(&sc->hash)) {
		return -1;
	}
	sc->can_reuse_count = 0;


	/* array that allows to reuse values for temporary variables
	   (like those that are created for results of binary operators +, *, etc)
	 */
	if (VARR_init(0,
				  &sc->resuse_temp_array,
				  sizeof(REUSESYMBOL),
				  0)) {
		return -1;
	}
	
	/* update pointers to current / global scope */
	vscript_ctx->current_scope = sc;
	if (!vscript_ctx->global_scope) {
		vscript_ctx->global_scope = vscript_ctx->current_scope;
	}
		
	return 0;
}

int SYM_SCOPE_is_global_scope()
{
	return vscript_ctx->global_scope == vscript_ctx->current_scope;
}

/*
  Function name - SYM_SCOPE_pop
 
  Description -
		delete the current symbole scope, 
		when finished compiling a function we exit symbol scope.
 
  Input -       
 
  Return Code/Output 
 */
int SYM_SCOPE_pop()
{
	if (!vscript_ctx->current_scope) {
		return -1;
	}
	
	VBUCKETHASH_free(&vscript_ctx->current_scope->hash);

	VARR_free(&vscript_ctx->current_scope->resuse_temp_array);
	
	vscript_ctx->current_scope = vscript_ctx->current_scope->parent;
	if (!vscript_ctx->current_scope) {
		vscript_ctx->global_scope = 0;
	}
	return 0;

}

/*
  Function name - SYM_SCOPE_find_ex
 
  Description -
		find entry for given name (argument item) in given lexical scope (argument scope)
		when a symbol is not found in argument scope, we try to find it in parent
		scope, and so on.

		optionaly returns scope where we found entry. (find_scope)

  Input - scope - symbol scope that is initialy searched
		  item - string that we are looking 

 
  Return Code/Output 
		   returns symbol that matches argument name (argument item)
		   find_scope - (if argument find_scope is != 0) the matching scope 
 */
STR2AST *SYM_SCOPE_find_ex(SYM_SCOPE *scope, const char *item, SYM_SCOPE **find_scope)
{
	STR2AST *hentry;

	do {

		hentry = (STR2AST *) VBUCKETHASH_find(&scope->hash, (void *) item, VHASH_STRING );
		if (hentry) {
			if (find_scope) {
				*find_scope = scope;
			}

			return hentry;
		}
		scope = scope->parent;
	
	} while( scope );

	if (find_scope) {
		*find_scope = scope;
	}

	return 0;
}

struct tagSTR2AST *SYM_SCOPE_find(SYM_SCOPE *scope, const char *item)
{
	return SYM_SCOPE_find_ex(scope, item, 0);
}

/*
  Function name - SYM_SCOPE_undef
 
  Description - undefine a symbol in given scope
 
  Input -	scope - scope where we are undefining the name
			item - symbol with this name is undefined
		
 
  Return Code/Output 
 */
int SYM_SCOPE_undef(SYM_SCOPE *scope, const char *item)
{
	STR2AST *hentry;

	hentry = (STR2AST *) VBUCKETHASH_unlink( &scope->hash, (void *) item, VHASH_STRING );
	if (hentry) {
		free( hentry );
		return 0;
	}
	return -1;
}


/*
  Function name - SYM_SCOPE_find_function_def
 
  Description - find function declaration	
				first search in global scope 
				then search in symbol table of extension library object.
 
  Input -       
 
  Return Code/Output 
 */
SYM_SCOPE_RET SYM_SCOPE_find_function_def(AST_FUNCTION_DECL *fdef, AST_BASE **err)
{
   SYM_SCOPE_RET ret_global,ret_xlib;
   AST_BASE *ast_global,*ast_xlib;	   
      
   ret_global = SYMBOLS_find_function_def( &vscript_ctx->global_scope->hash, fdef, &ast_global);

   ret_xlib = SYMBOLS_find_function_def(&vscript_ctx->extension_library->hash, fdef, &ast_xlib);


   if (ret_xlib < ret_global) {
	  ret_global = ret_xlib;
	  ast_global = ast_xlib;

   }

   if (err) {
	   *err = ast_global;
   }
   return ret_global;

}

/*
  Function name - SYM_SCOPE_find_function_def
 
  Description - find function declaration that matches the argument function call.	
				first search in global scope 
				then search in symbol table of extension library object.
 
  Input -       
 
  Return Code/Output 
 */
SYM_SCOPE_RET SYM_SCOPE_find_function_call( AST_FUNCTION_CALL *fcall, AST_FUNCTION_DECL **found, AST_BASE **err )
{
   SYM_SCOPE_RET ret_global,ret_xlib;
   AST_FUNCTION_DECL *ast_global,*ast_xlib;
   AST_BASE *err_global, *err_xlib;

   ret_global = SYMBOLS_find_function_call( &vscript_ctx->global_scope->hash, fcall, &ast_global, &err_global);

   ret_xlib = SYMBOLS_find_function_call(&vscript_ctx->extension_library->hash, fcall, &ast_xlib, &err_xlib);

   if (ret_xlib < ret_global) {
	  ret_global = ret_xlib;
	  ast_global = ast_xlib;
	  err_global = err_xlib;

   }

   if (found) {
	   *found = ast_global;
   }
   if (err) {
	   *err = err_global;
   }
   return ret_global;
}


/* *** scoping rules life here *** */

/*
  Function name - SYM_SCOPE_is_defined_ex
 
  Description - check if argument AST node (argument entry) is defined in a relevant scope
				returns the exact scope where it is defined (optional ret value find_scope)

				dispatch on type of ast node.

				function declarations
					- check that this function is not already defined with same name and same arguments
				function call
					- check if a matching function is declared, if yes then return it via (find_entry)
				variable declaration
					- check that no variable with such a name is declared in current scope
				expression (if it is a variable reference)
					- check that refernce variable has been defined as matching type
								
  Input -       
 
  Return Code/Output  0 - a matching entry has been found
 */
int SYM_SCOPE_is_defined_ex(AST_BASE *entry, AST_BASE ** find_entry, SYM_SCOPE **find_scope)
{
	STR2AST *hentry = 0;

	V_UNUSED(find_scope);
	
	switch(entry->type) {

	case S_FUN_DECL: {
		AST_FUNCTION_DECL *	fdecl = (AST_FUNCTION_DECL *) entry;
		AST_BASE *err;	
	
		switch(SYM_SCOPE_find_function_def( fdecl, &err ))
		{
		case FUNC_DECL_WITH_SAME_SIGNATURE:
			do_yyerror(fdecl->super.location, 
				 "Function %s is defined twice with the same name and parameters. first definition in row %d column %d",
				 fdecl->name,
				 err->location.first_line,
				 err->location.first_column);
			return -1;

		case FUNC_DECL_WITH_DIFF_SIGNATURE:	
			break;


		case NOT_FUNC_DECL:			
			 do_yyerror(fdecl->super.location, 
						"Can't define function  %s a %s of the same name is already defined at row %d column %d",
						 fdecl->name,
						 AST_BASE_get_display_name(err->type),
						 err->location.first_line,
						 err->location.first_column);
			 break;

		case NOTHING_DECL:
			break;
		}
		
	} 
	break;
	
	case S_FUN_CALL: {
		AST_FUNCTION_CALL *fcall = (AST_FUNCTION_CALL *) entry;
		AST_FUNCTION_DECL * found;
		AST_BASE *err;	

		switch(SYM_SCOPE_find_function_call( fcall, &found, &err )) {
			
		case FUNC_DECL_WITH_SAME_SIGNATURE:
			// bingo
			fcall->func_decl = found;
			if (find_entry) {
				*find_entry = (AST_BASE *) found;
			}
			return 0;

		case FUNC_DECL_WITH_DIFF_SIGNATURE:
			do_yyerror(fcall->super.location,
				"calling function %s that is defined with the same name but different parameters. function defined at row %d column %d",
				fcall->name, 
				err->location.first_line,
				err->location.first_column);
			return -1;
	
		case NOT_FUNC_DECL:
			do_yyerror(fcall->super.location, 
				"calling function %s that is defined as %s in row %d column %d",
				 fcall->name,
				 AST_BASE_get_display_name(err->type),
				 err->location.first_line,
				 err->location.first_column);
			return -1;
			
		case NOTHING_DECL:
			do_yyerror(fcall->super.location, 
						"calling function %s that is not defined",
						fcall->name);
			return -1;
		}
			
	}
	break;
	
	case S_VARDEF: {
		AST_VARDEF *vd = (AST_VARDEF *) entry;
		SYM_SCOPE *f_scope;

		hentry = SYM_SCOPE_find_ex(vscript_ctx->current_scope, vd->var_name, &f_scope);
		if (hentry != 0 && f_scope == vscript_ctx->current_scope) {							
			do_yyerror(vd->super.location, 
				 "Can't define variable %s, it is already defined as a %s, first definition at line %d column %d",
				 vd->var_name,
				 AST_BASE_get_display_name(hentry->ast->type),
				 hentry->ast->location.first_line,
				 hentry->ast->location.first_column);
			return -1;
		}
	}
	break;


	case S_EXPRESSION: {
		AST_EXPRESSION *expr = (AST_EXPRESSION *) entry;
		AST_VARDEF *exist;

		if (expr->exp_type == S_EXPR_HASH_REF ||
		    expr->exp_type == S_EXPR_ARRAY_REF ||
			expr->exp_type == S_EXRP_SCALAR_REF) {


			if ( (hentry = SYM_SCOPE_find( vscript_ctx->current_scope, expr->val.ref.lhs )) == 0) {
				do_yyerror(expr->super.location, 
						   "trying to get value of variable %s, but it has not been defined by either variable or function parameter",
						   expr->val.ref.lhs );
				return -1;
			}

			if (hentry->ast->type != S_VARDEF) {
				do_yyerror(expr->super.location, 
					"Variable %s is used as %s but has been defined as %s at row %d column %d",
					expr->val.ref.lhs,
					AST_BASE_get_expression_type_name(expr->exp_type),
					AST_BASE_get_display_name(hentry->ast->type),
					hentry->ast->location.first_line,
					hentry->ast->location.first_column);
				return -1;
			} 

			// error: 
			// when used with left hand side of assignment we can assign
			// scalar ref a hash value.
			// var @key,@val
			// key = (1, 2, 3)
			// also  key = val 
			// can be also left hand side.
			// 
			// what we can't do is
			// my @val
			// val{ kuku } - use a scalar or array as hash, or can't use hash or scalar as array.

			exist =  (AST_VARDEF *) hentry->ast;
			if (IS_EXPR_TYPE_REF(expr->exp_type) &&
				expr->exp_type != S_EXRP_SCALAR_REF &&
				expr->exp_type != exist->var_type  ) {

				do_yyerror(expr->super.location, 
						"Variable %s is used as %s but has been defined as %s",
						expr->val.ref.lhs,
						AST_BASE_get_expression_type_name(expr->exp_type),
						AST_BASE_get_var_type_name(exist->var_type));
						
				return -1;
			}
			expr->val.ref.var_def = exist;
			expr->value_type = exist->var_type;
		
		}	
		}
		break;
		
	case S_LABEL: {
		

	}
	break;
	
	default: 
		return -1;
	}

	if (find_entry) {
		*find_entry = hentry ? hentry->ast : 0;
	}

	return 0;
}


int SYM_SCOPE_is_defined(AST_BASE *entry)
{
	return SYM_SCOPE_is_defined_ex(entry, 0, 0);
}

/*
  Function name - SYM_SCOPE_define
 
  Description - define an AST node by inserting it into symbol table of relevant scope
	switch on type of AST node
		- for function declarations
			enter into global scope
		- for variable definition, labels.
			enter into current sope

  Input -  entry - ast node
 
  Return Code/Output 
 */
int SYM_SCOPE_define(AST_BASE *entry)
{
	VBUCKETHASH *hash;
	char *key;

	if (SYM_SCOPE_is_defined(entry)) {
		return -1;
	}
	if (entry->type == S_FUN_DECL) {
		hash = &vscript_ctx->global_scope->hash;
		key = ((AST_FUNCTION_DECL *) entry)->name;
		
	} else if (entry->type == S_VARDEF) {
		hash = &vscript_ctx->current_scope->hash;
		key = ((AST_VARDEF *) entry)->var_name;
	} else if (entry->type == S_LABEL) {
		hash = &vscript_ctx->current_scope->hash;
		key = ((AST_LABEL *) entry)->label_name;	
	}


	return SYMBOLS_define(hash, key, entry);
}

/* *** reuse of local variables *** */



static SYM_LOCATION SYM_SCOPE_make_new_location_ex(SYM_SCOPE *scope, int *is_reused)
{
	SYM_LOCATION location;

	if (scope->can_reuse_count) {
		REUSESYMBOL *ret = 0;
		size_t i, idx = (size_t) -1;

		for(i = 0;i < VARR_size( &scope->resuse_temp_array ); i++ ) {
			REUSESYMBOL *r = (REUSESYMBOL *) VARR_at( &scope->resuse_temp_array, i );
			if (!r->is_in_use) {

				if (ret) {
					if (ret->location > r->location) {
						ret = r;
						idx = i;
					}
				} else {
					ret = r;
					idx = i;
				}
			}

		}

		*is_reused = 1;
		ret->is_in_use = 1;
		scope->can_reuse_count --;

		location = ret->location;
		VARR_delete_at( &scope->resuse_temp_array, idx );

//fprintf(stderr,"REUSE: 0x%x\n",  ret->location);
		return location;
	}

	* is_reused = 0;

	location = scope->next_storage ++;
	if (!scope->parent) {
//fprintf(stderr,"NEW_LOCATION_GLOBAL: 0x%x\n",  location);
		return ASM_MAX_GLOBAL_VAR - location;
	}

//fprintf(stderr,"NEW_LOCATION_STACK: 0x%x\n",  location);

	return location;
}

/*
   return location for next variable, depending on current scope 
 */
SYM_LOCATION SYM_SCOPE_make_new_location(SYM_SCOPE *scope)
{
	int reuse;
	return SYM_SCOPE_make_new_location_ex(scope,&reuse);
}


/*
   return location for next variable, depending on current scope 
   mark this location as possible for reuse, will be reused after we call SYM_SCOPE_release_locale
 */
SYM_LOCATION SYM_SCOPE_make_new_location_reusable(SYM_SCOPE *scope)
{
	int reuse;
	SYM_LOCATION ret;


	
	ret = SYM_SCOPE_make_new_location_ex(scope,&reuse);
	if (!reuse) {
		REUSESYMBOL r;

		r.is_in_use = 1;
		r.location = ret;

		VARR_push_back( &scope->resuse_temp_array, &r, sizeof(REUSESYMBOL) );
	}


	return ret;
}

/*
   attempt to release a temporary variable
 */
int SYM_SCOPE_release_location(SYM_SCOPE *scope, SYM_LOCATION location)
{
	size_t i;

	if (ASM_IS_CONSTANT(location)) {
		return -1;
	}

	for(i = 0;i < VARR_size( &scope->resuse_temp_array ); i++ ) {
		REUSESYMBOL *r = (REUSESYMBOL *) VARR_at( &scope->resuse_temp_array, i );
		if (r->is_in_use && r->location == location) {
			r->is_in_use = 0;

			scope->can_reuse_count ++;

			return 0;
		}
	}
	return -1;
}

int SYM_SCOPE_is_location_on_stack( SYM_SCOPE *global_scope, SYM_LOCATION location)
{
	return ASM_IS_STACK(location,global_scope->next_storage);
		
		//location < (SYM_LOCATION) (ASM_MAX_GLOBAL_VAR - global_scope->next_storage);
}
