/*
 * plv8.cc : PL/v8 handler routines.
 */
#include "plv8.h"
#include <new>

extern "C" {
#define delete		delete_
#define namespace	namespace_
#define	typeid		typeid_
#define	typename	typename_
#define	using		using_

#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#undef delete
#undef namespace
#undef typeid
#undef typename
#undef using

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);

Datum	plv8_call_handler(PG_FUNCTION_ARGS) throw();
Datum	plv8_call_validator(PG_FUNCTION_ARGS) throw();

void _PG_init(void);

#if PG_VERSION_NUM >= 90000
PG_FUNCTION_INFO_V1(plv8_inline_handler);
Datum	plv8_inline_handler(PG_FUNCTION_ARGS) throw();
#endif
} // extern "C"

using namespace v8;

typedef struct plv8_proc_cache
{
	Oid						fn_oid;

	Persistent<Script>		script;
	char					proname[NAMEDATALEN];
	char				   *prosrc;

	TransactionId			fn_xmin;
	ItemPointerData			fn_tid;

	int						nargs;
	bool					retset;		/* true if SRF */
	Oid						rettype;
	Oid						argtypes[FUNC_MAX_ARGS];
} plv8_proc_cache;

/*
 * The function and context are created at the first invocation.  Their
 * lifetime is same as plv8_poc, but they are not palloc'ed memory,
 * so we need to clear them at the end of transaction.
 */
typedef struct plv8_exec_env
{
	Persistent<Function>	function;
	Persistent<Context>		context;
	struct plv8_exec_env   *next;
} plv8_exec_env;

/*
 * We cannot cache plv8_type inter executions because it has FmgrInfo fields.
 * So, we cache rettype and argtype in fn_extra only during one execution.
 */
typedef struct plv8_proc
{
	plv8_proc_cache		   *cache;
	plv8_exec_env		   *xenv;
	plv8_type				rettype;
	plv8_type				argtypes[FUNC_MAX_ARGS];
} plv8_proc;

static HTAB *plv8_proc_cache_hash = NULL;

static plv8_exec_env		   *exec_env_head = NULL;
/*
 * The separate context feature is currently experimental.  One benchmark shows
 * creating a new Context takes 1 ms or so, which may or may not be fine
 * depending on use cases.  It is much, much longer than a typical hash_search.
 */
static bool plv8_use_separate_context = false;

/*
 * lower_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc(Oid fn_oid, MemoryContext fn_mcxt, bool validate, char ***argnames) throw();
static void plv8_xact_cb(XactEvent event, void *arg);

/*
 * CamelCaseFunctions are C++ functions.
 * They could raise errors with C++ throw statements, or never throw exceptions.
 */
static plv8_exec_env *CreateExecEnv(Handle<Script> script);
static plv8_proc *Compile(Oid fn_oid, MemoryContext fn_mcxt, bool validate, bool is_trigger);
static Local<Script> CompileScript(const char *proname, int proarglen,
					const char *proargs[], const char *prosrc,
					bool is_trigger, bool retset);
static Datum CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv);
static Persistent<Context> GetGlobalContext() throw();
static Persistent<ObjectTemplate> GetGlobalObjectTemplate() throw();

void
_PG_init(void)
{
	DefineCustomBoolVariable("plv8.use_separate_context",
							 gettext_noop("ON to use separate context for each function execution."),
							 gettext_noop("Enabling this may increase execution overhead."),
							 &plv8_use_separate_context,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	RegisterXactCallback(plv8_xact_cb, NULL);
}

static void
plv8_xact_cb(XactEvent event, void *arg)
{
	plv8_exec_env	   *env = exec_env_head;

	while (env)
	{
		if (!env->function.IsEmpty())
		{
			env->function.Dispose();
			env->function.Clear();
		}
		if (!env->context.IsEmpty() &&
				GetGlobalContext() != env->context)
		{
			env->context.Dispose();
			env->context.Clear();
		}
		env = env->next;
		/*
		 * Each item was allocated in TopTransactionContext, so
		 * it will be freed eventually.
		 */
	}
	exec_env_head = NULL;
}

static inline plv8_exec_env *
plv8_new_exec_env()
{
	plv8_exec_env	   *xenv = (plv8_exec_env *)
		MemoryContextAllocZero(TopTransactionContext, sizeof(plv8_exec_env));

	new(&xenv->context) Persistent<Context>();
	new(&xenv->function) Persistent<Function>();

	/*
	 * Add it to the list, which will be freed in the end of top transaction.
	 */
	xenv->next = exec_env_head;
	exec_env_head = xenv;

	return xenv;
}

Datum
plv8_call_handler(PG_FUNCTION_ARGS) throw()
{
	Oid		fn_oid = fcinfo->flinfo->fn_oid;
	bool	is_trigger = CALLED_AS_TRIGGER(fcinfo);

	try
	{
		HandleScope	handle_scope;

		if (!fcinfo->flinfo->fn_extra)
			fcinfo->flinfo->fn_extra = Compile(fn_oid, fcinfo->flinfo->fn_mcxt, false, is_trigger);

		plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
		plv8_proc_cache *cache = proc->cache;

		if (is_trigger)
			return CallTrigger(fcinfo, proc->xenv);
		else if (cache->retset)
			return CallSRFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
		else
			return CallFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

#if PG_VERSION_NUM >= 90000
Datum
plv8_inline_handler(PG_FUNCTION_ARGS) throw()
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));

	Assert(IsA(codeblock, InlineCodeBlock));

	try
	{
		HandleScope			handle_scope;

		Handle<Script>		script = CompileScript(NULL, 0, NULL,
										codeblock->source_text, false, false);
		plv8_exec_env	   *xenv = CreateExecEnv(script);
		return CallFunction(fcinfo, xenv, 0, NULL, NULL);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}
#endif

/*
 * DoCall -- Call a JS function with SPI support.
 *
 * This function could throw C++ exceptions, but must not throw PG exceptions.
 */
static Local<v8::Value>
DoCall(Handle<Function> fn, Handle<Object> context,
	int nargs, Handle<v8::Value> args[])
{
	TryCatch		try_catch;

	if (SPI_connect() != SPI_OK_CONNECT)
		throw js_error(_("could not connect to SPI manager"));
	Local<v8::Value> result = fn->Call(context, nargs, args);
	int	status = SPI_finish();

	if (result.IsEmpty())
		throw js_error(try_catch);

	if (status < 0)
		throw js_error(FormatSPIStatus(status));

	return result;
}

static Datum
CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	Handle<Context>		context = xenv->context;
	Context::Scope		context_scope(context);
	Handle<v8::Value>	args[FUNC_MAX_ARGS];

	for (int i = 0; i < nargs; i++)
		args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);
	Local<v8::Value> result =
		DoCall(xenv->function, context->Global(), nargs, args);

	if (rettype)
		return ToDatum(result, &fcinfo->isnull, rettype);
	else
		PG_RETURN_VOID();
}

static Tuplestorestate *
CreateTupleStore(PG_FUNCTION_ARGS, TupleDesc *tupdesc)
{
	Tuplestorestate	   *tupstore;

	PG_TRY();
	{
		ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		MemoryContext	per_query_ctx;
		MemoryContext	oldcontext;

		/* check to see if caller supports us returning a tuplestore */
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		if (!(rsinfo->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not " \
							"allowed in this context")));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("SETOF a scalar type is not implemented yet")));

		per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		tupstore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->returnMode = SFRM_Materialize;
		rsinfo->setResult = tupstore;
		rsinfo->setDesc = *tupdesc;

		MemoryContextSwitchTo(oldcontext);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return tupstore;
}

static Datum
CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;

	tupstore = CreateTupleStore(fcinfo, &tupdesc);

	Handle<Context>		context = xenv->context;
	Context::Scope		context_scope(context);
	Converter			conv(tupdesc);
	Handle<v8::Value>	args[FUNC_MAX_ARGS + 1];
	Handle<Object>		plv8obj = Handle<Object>::Cast(
			context->Global()->Get(String::NewSymbol("plv8")));

	for (int i = 0; i < nargs; i++)
		args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);

	// Add "yield" function as a hidden argument.
	// TODO: yield should be passed as a global variable.
	args[nargs] = CreateYieldFunction(&conv, tupstore);

	/*
	 * Not safe for recursive calls.  Revisit when we introduce SQL function call.
	 */
	plv8obj->Set(String::NewSymbol("return_next"), CreateYieldFunction(&conv, tupstore));
	Handle<v8::Value> result =
		DoCall(xenv->function, context->Global(), nargs + 1, args);
	plv8obj->Delete(String::NewSymbol("return_next"));

	if (result->IsUndefined())
	{
		// no additinal values
	}
	else if (result->IsArray())
	{
		Handle<Array> array = Handle<Array>::Cast(result);
		// return an array of records.
		int	length = array->Length();
		for (int i = 0; i < length; i++)
			conv.ToDatum(array->Get(i), tupstore);
	}
	else if (result->IsObject())
	{
		// return a record
		conv.ToDatum(result, tupstore);
	}
	else
	{
		throw js_error("SETOF function cannot return scalar value");
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

static Datum
CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv)
{
	// trigger arguments are:
	//	0: NEW
	//	1: OLD
	//	2: TG_NAME
	//	3: TG_WHEN
	//	4: TG_LEVEL
	//	5: TG_OP
	//	6: TG_RELID
	//	7: TG_TABLE_NAME
	//	8: TG_TABLE_SCHEMA
	//	9: TG_ARGV
	TriggerData		   *trig = (TriggerData *) fcinfo->context;
	Relation			rel = trig->tg_relation;
	TriggerEvent		event = trig->tg_event;
	Handle<v8::Value>	args[10];
	Datum				result = (Datum) 0;

	Handle<Context>		context = xenv->context;
	Context::Scope		context_scope(context);

	if (TRIGGER_FIRED_FOR_ROW(event))
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);

		if (TRIGGER_FIRED_BY_INSERT(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_trigtuple);
			// OLD
			args[1] = Undefined();
		}
		else if (TRIGGER_FIRED_BY_DELETE(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = Undefined();
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
		else if (TRIGGER_FIRED_BY_UPDATE(event))
		{
			result = PointerGetDatum(trig->tg_newtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_newtuple);
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
	}
	else
	{
		args[0] = args[1] = Undefined();
	}

	// 2: TG_NAME
	args[2] = ToString(trig->tg_trigger->tgname);

	// 3: TG_WHEN
	if (TRIGGER_FIRED_BEFORE(event))
		args[3] = String::New("BEFORE");
	else
		args[3] = String::New("AFTER");

	// 4: TG_LEVEL
	if (TRIGGER_FIRED_FOR_ROW(event))
		args[4] = String::New("ROW");
	else
		args[4] = String::New("STATEMENT");

	// 5: TG_OP
	if (TRIGGER_FIRED_BY_INSERT(event))
		args[5] = String::New("INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(event))
		args[5] = String::New("DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(event))
		args[5] = String::New("UPDATE");
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(event))
		args[5] = String::New("TRUNCATE");
#endif
	else
		args[5] = String::New("?");

	// 6: TG_RELID
	args[6] = Uint32::New(RelationGetRelid(rel));

	// 7: TG_TABLE_NAME
	args[7] = ToString(RelationGetRelationName(rel));

	// 8: TG_TABLE_SCHEMA
	args[8] = ToString(get_namespace_name(RelationGetNamespace(rel)));

	// 9: TG_ARGV
	Handle<Array> tgargs = Array::New(trig->tg_trigger->tgnargs);
	for (int i = 0; i < trig->tg_trigger->tgnargs; i++)
		tgargs->Set(i, ToString(trig->tg_trigger->tgargs[i]));
	args[9] = tgargs;

	Handle<v8::Value> newtup =
		DoCall(xenv->function, context->Global(), lengthof(args), args);

	// TODO: replace NEW tuple if modified.

	return result;
}

Datum
plv8_call_validator(PG_FUNCTION_ARGS) throw()
{
	Oid				fn_oid = PG_GETARG_OID(0);
	HeapTuple		tuple;
	Form_pg_proc	proc;
	char			functyptype;
	bool			is_trigger = false;

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, or VOID */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			is_trigger = true;
		else if (proc->prorettype != RECORDOID &&
			proc->prorettype != VOIDOID)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/v8 functions cannot return type %s",
						format_type_be(proc->prorettype))));
	}

	ReleaseSysCache(tuple);

	try
	{
		(void) Compile(fn_oid, fcinfo->flinfo->fn_mcxt, true, is_trigger);
		/* the result of a validator is ignored */
		PG_RETURN_VOID();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

static plv8_proc *
plv8_get_proc(Oid fn_oid, MemoryContext fn_mcxt, bool validate, char ***argnames) throw()
{
	HeapTuple			procTup;
	plv8_proc_cache	   *cache;
	bool				found;
	bool				isnull;
	Datum				prosrc;
	Oid				   *argtypes;
	char			   *argmodes;
	Oid					rettype;
	MemoryContext		oldcontext;

	if (plv8_proc_cache_hash == NULL)
	{
		HASHCTL    hash_ctl = { 0 };

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(plv8_proc_cache);
		hash_ctl.hash = oid_hash;
		plv8_proc_cache_hash = hash_create("PLv8 Procedures", 32,
									 &hash_ctl, HASH_ELEM | HASH_FUNCTION);
	}

	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	cache = (plv8_proc_cache *) hash_search(plv8_proc_cache_hash, &fn_oid, HASH_ENTER, &found);

	if (found)
	{
		bool    uptodate;

		uptodate = (!cache->script.IsEmpty() &&
			cache->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			ItemPointerEquals(&cache->fn_tid, &procTup->t_self));

		if (!uptodate)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->script.Dispose();
			cache->script.Clear();
		}
		else
		{
			ReleaseSysCache(procTup);
		}
	}
	else
	{
		new(&cache->script) Persistent<Script>();
		cache->prosrc = NULL;
	}

	if (cache->script.IsEmpty())
	{
		Form_pg_proc	procStruct;

		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		prosrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		cache->retset = procStruct->proretset;
		cache->rettype = procStruct->prorettype;

		strlcpy(cache->proname, NameStr(procStruct->proname), NAMEDATALEN);
		cache->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		cache->fn_tid = procTup->t_self;

		int nargs = get_func_arg_info(procTup, &argtypes, argnames, &argmodes);

		if (validate)
		{
			/* Disallow pseudotypes in arguments (either IN or OUT) */
			for (int i = 0; i < nargs; i++)
			{
				if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO)
					ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/v8 functions cannot accept type %s",
								format_type_be(argtypes[i]))));
			}
		}

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		cache->prosrc = TextDatumGetCString(prosrc);
		MemoryContextSwitchTo(oldcontext);

		ReleaseSysCache(procTup);

		int	inargs = 0;
		for (int i = 0; i < nargs; i++)
		{
			Oid		argtype = argtypes[i];
			char	argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			switch (argmode)
			{
			case PROARGMODE_IN:
			case PROARGMODE_INOUT:
			case PROARGMODE_VARIADIC:
				break;
			default:
				continue;
			}

			if (*argnames)
				(*argnames)[inargs] = (*argnames)[i];
			cache->argtypes[inargs] = argtype;
			inargs++;
		}
		cache->nargs = inargs;
	}

	plv8_proc *proc = (plv8_proc *) MemoryContextAllocZero(fn_mcxt,
		offsetof(plv8_proc, argtypes) + sizeof(plv8_type) * cache->nargs);

	proc->cache = cache;
	for (int i = 0; i < cache->nargs; i++)
		plv8_fill_type(&proc->argtypes[i], cache->argtypes[i], fn_mcxt);
	plv8_fill_type(&proc->rettype, cache->rettype, fn_mcxt);

	return proc;
}

static plv8_exec_env *
CreateExecEnv(Handle<Script> script)
{
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope;

	PG_TRY();
	{
		xenv = plv8_new_exec_env();
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (plv8_use_separate_context)
		xenv->context = Context::New(NULL,
									 GetGlobalObjectTemplate());
	else
		xenv->context = GetGlobalContext();

	Context::Scope		scope(xenv->context);
	TryCatch			try_catch;
	Local<v8::Value>	result = script->Run();

	if (result.IsEmpty())
		throw js_error(try_catch);

	xenv->function =
		Persistent<Function>::New(Local<Function>::Cast(result));

	return xenv;
}

static plv8_proc *
Compile(Oid fn_oid, MemoryContext fn_mcxt, bool validate, bool is_trigger)
{
	plv8_proc  *proc;
	char	  **argnames;

	PG_TRY();
	{
		proc = plv8_get_proc(fn_oid, fn_mcxt, validate, &argnames);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	HandleScope		handle_scope;
	plv8_proc_cache *cache = proc->cache;

	if (cache->script.IsEmpty())
		cache->script = Persistent<Script>::New(CompileScript(
						cache->proname,
						cache->nargs,
						(const char **) argnames,
						cache->prosrc,
						is_trigger,
						cache->retset));
	proc->xenv = CreateExecEnv(cache->script);

	return proc;
}

static Local<Script>
CompileScript(
	const char *proname,
	int proarglen,
	const char *proargs[],
	const char *prosrc,
	bool is_trigger,
	bool retset)
{
	HandleScope		handle_scope;
	StringInfoData  src;
	Handle<Context>	global_context = GetGlobalContext();

	initStringInfo(&src);

	/*
	 *  function _(<arg1, ...>){
	 *    <prosrc>
	 *  }
	 *  _
	 */
	appendStringInfo(&src, "function _(");
	if (is_trigger)
	{
		if (proarglen != 0)
			throw js_error("trigger function cannot have arguments");
		// trigger function has special arguments.
		appendStringInfo(&src,
			"NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
			"TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
	}
	else
	{
		for (int i = 0; i < proarglen; i++)
		{
			if (i > 0)
				appendStringInfoChar(&src, ',');
			if (proargs && proargs[i])
				appendStringInfoString(&src, proargs[i]);
			else
				appendStringInfo(&src, "$%d", i + 1);	// unnamed argument to $N
		}

		// Add "yield" function as a hidden argument.
		if (retset)
		{
			if (proarglen > 0)
				appendStringInfoChar(&src, ',');
			appendStringInfoString(&src, "yield");
		}
	}
	appendStringInfo(&src, "){\n%s\n};\n_", prosrc);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined();
	Local<String> source = ToString(src.data, src.len);
	pfree(src.data);

	Context::Scope	context_scope(global_context);
	TryCatch		try_catch;
	Local<Script>	script = Script::New(source, name);

	if (script.IsEmpty())
		throw js_error(try_catch);

	return handle_scope.Close(script);
}

/*
 * NOTICE: the returned buffer could be an internal static buffer.
 */
const char *
FormatSPIStatus(int status) throw()
{
	static char	private_buf[1024];

	if (status > 0)
		return "OK";

	switch (status)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
		case SPI_ERROR_TRANSACTION:
			return _("current transaction is aborted, "
					 "commands ignored until end of transaction block");
		case SPI_ERROR_CURSOR:
			return "SPI_ERROR_CURSOR";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		default:
			snprintf(private_buf, sizeof(private_buf),
				_("SPI_ERROR: %d"), status);
			return private_buf;
	}
}

Handle<v8::Value>
ThrowError(const char *message) throw()
{
	return ThrowException(Exception::Error(String::New(message)));
}

static Persistent<Context>
GetGlobalContext() throw()
{
	static Persistent<Context>	global_context;

	if (global_context.IsEmpty())
	{
		HandleScope				handle_scope;
		Handle<ObjectTemplate>	global = GetGlobalObjectTemplate();

		global_context = Context::New(NULL, global);
	}

	return global_context;
}

static Persistent<ObjectTemplate>
GetGlobalObjectTemplate() throw()
{
	static Persistent<ObjectTemplate>	global;

	if (global.IsEmpty())
	{
		HandleScope				handle_scope;

		global = Persistent<ObjectTemplate>::New(ObjectTemplate::New());
		// built-in function print(elevel, ...)
		global->Set(String::NewSymbol("print"),
					FunctionTemplate::New(Print));
		global->Set(String::NewSymbol("DEBUG5"), Int32::New(DEBUG5));
		global->Set(String::NewSymbol("DEBUG4"), Int32::New(DEBUG4));
		global->Set(String::NewSymbol("DEBUG3"), Int32::New(DEBUG3));
		global->Set(String::NewSymbol("DEBUG2"), Int32::New(DEBUG2));
		global->Set(String::NewSymbol("DEBUG1"), Int32::New(DEBUG1));
		global->Set(String::NewSymbol("DEBUG"), Int32::New(DEBUG5));
		global->Set(String::NewSymbol("LOG"), Int32::New(LOG));
		global->Set(String::NewSymbol("INFO"), Int32::New(INFO));
		global->Set(String::NewSymbol("NOTICE"), Int32::New(NOTICE));
		global->Set(String::NewSymbol("WARNING"), Int32::New(WARNING));
		global->Set(String::NewSymbol("ERROR"), Int32::New(ERROR));
		// ERROR or higher severity levels are not allowed. Use "throw" instead.

		// built-in SPI access

		global->Set(String::NewSymbol("executeSql"),
					FunctionTemplate::New(ExecuteSql));

		global->Set(String::NewSymbol("createPlan"),
					FunctionTemplate::New(CreatePlan));
		
		global->Set(String::NewSymbol("executePlan"),
					FunctionTemplate::New(ExecutePlan));
		
		global->Set(String::NewSymbol("freePlan"),
					FunctionTemplate::New(FreePlan));
		
		global->Set(String::NewSymbol("createCursor"),
					FunctionTemplate::New(CreateCursor));
		
		global->Set(String::NewSymbol("fetchCursor"),
					FunctionTemplate::New(FetchCursor));
		
		global->Set(String::NewSymbol("closeCursor"),
					FunctionTemplate::New(CloseCursor));

		global->Set(String::NewSymbol("subtransaction"),
					FunctionTemplate::New(Subtransaction));


		Handle<ObjectTemplate>	plv8 = ObjectTemplate::New();

		SetupPlv8Functions(plv8);

		global->Set(String::NewSymbol("plv8"), plv8);
	}

	return global;
}

Converter::Converter(TupleDesc tupdesc) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts)
{
	for (int c = 0; c < tupdesc->natts; c++)
	{
		m_colnames[c] = ToString(NameStr(tupdesc->attrs[c]->attname));
		PG_TRY();
		{
			plv8_fill_type(&m_coltypes[c], m_tupdesc->attrs[c]->atttypid, NULL);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();
	}
}

// TODO: use prototype instead of per tuple fields to reduce
// memory consumption.
Local<Object>
Converter::ToValue(HeapTuple tuple)
{
	Local<Object>	obj = Object::New();

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Datum		datum;
		bool		isnull;

		datum = heap_getattr(tuple, c + 1, m_tupdesc, &isnull);
		obj->Set(m_colnames[c], ::ToValue(datum, isnull, &m_coltypes[c]));
	}

	return obj;
}

Datum
Converter::ToDatum(Handle<v8::Value> value, Tuplestorestate *tupstore)
{
	Datum			result;
	TryCatch		try_catch;

	Handle<Object>	obj = Handle<Object>::Cast(value);
	if (obj.IsEmpty())
		throw js_error(try_catch);

	/*
	 * Use vector<char> instead of vector<bool> because <bool> version is
	 * s specialized and different from bool[].
	 */
	Datum  *values = (Datum *) palloc(sizeof(Datum) * m_tupdesc->natts);
	bool   *nulls = (bool *) palloc(sizeof(bool) * m_tupdesc->natts);

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Handle<v8::Value> attr = obj->Get(m_colnames[c]);
		if (attr.IsEmpty() || attr->IsUndefined() || attr->IsNull())
			nulls[c] = true;
		else
			values[c] = ::ToDatum(attr, &nulls[c], &m_coltypes[c]);
	}

	if (tupstore)
	{
		tuplestore_putvalues(tupstore, m_tupdesc, values, nulls);
		result = (Datum) 0;
	}
	else
	{
		result = HeapTupleGetDatum(heap_form_tuple(m_tupdesc, values, nulls));
	}

	pfree(values);
	pfree(nulls);

	return result;
}

js_error::js_error() throw()
	: m_msg(NULL), m_detail(NULL)
{
}

js_error::js_error(const char *msg) throw()
{
	m_msg = pstrdup(msg);
	m_detail = NULL;
}

js_error::js_error(TryCatch &try_catch) throw()
{
	HandleScope			handle_scope;
	String::Utf8Value	exception(try_catch.Exception());
	Handle<Message>		message = try_catch.Message();

	m_msg = NULL;
	m_detail = NULL;

	try
	{
		m_msg = ToCStringCopy(exception);

		if (!message.IsEmpty())
		{
			StringInfoData	str;
			CString			script(message->GetScriptResourceName());
			int				lineno = message->GetLineNumber();
			CString			source(message->GetSourceLine());

			/*
			 * Report lineno - 1 because "function _(...){" was added
			 * at the first line to the javascript code.
			 */
			initStringInfo(&str);
			appendStringInfo(&str, "%s() LINE %d: %s",
				script.str("?"), lineno - 1, source.str("?"));
			m_detail = str.data;
		}
	}
	catch (...)
	{
		// nested error, keep quiet.
	}
}

Local<v8::Value>
js_error::error_object()
{
	char *msg = pstrdup(m_msg ? m_msg : "unknown exception");
	/*
	 * Trim leading "Error: ", in case the message is generated from
	 * another Error.
	 */
	if (strstr(msg, "Error: ") == msg)
		msg += 7;
	Local<String> message = ToString(msg);
	return Exception::Error(message);
}

__attribute__((noreturn))
void
js_error::rethrow() throw()
{
	ereport(ERROR,
		(m_msg ? errmsg("%s", m_msg) : 0,
			m_detail ? errdetail("%s", m_detail) : 0));
	exit(0);	// keep compiler quiet
}

__attribute__((noreturn))
void
pg_error::rethrow() throw()
{
	PG_RE_THROW();
	exit(0);	// keep compiler quiet
}
