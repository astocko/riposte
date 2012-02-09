#include <string>
#include <sstream>
#include <stdexcept>
#include <string>

#include "value.h"
#include "type.h"
#include "bc.h"
#include "ops.h"
#include "internal.h"
#include "interpreter.h"
#include "compiler.h"
#include "sse.h"
#include "call.h"

extern Instruction const* kget_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* get_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* assign_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* forend_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* add_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* subset_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* subset2_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* jc_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;
extern Instruction const* lt_op(Thread& thread, Instruction const& inst) ALWAYS_INLINE;

Instruction const* forceReg(Thread& thread, Instruction const& inst, Value const* a, String name) {
	if(a->isPromise()) {
		Function f(*a);
		return buildStackFrame(thread, f.environment()->DynamicScope(), false, f.prototype(), f.environment(), name, &inst);
	} else if(a->isDefault()) {
		Function f(*a);
		return buildStackFrame(thread, f.environment(), false, f.prototype(), f.environment(), name, &inst);
	} else {
		_error(std::string("Object '") + thread.externStr(name) + "' not found");
	}
}

#define REGISTER(i) (*(thread.base-(i)))

#define OPERAND(a, i) \
Value const* a##p; \
do { \
	if(__builtin_expect((i) <= 0, true)) a##p = (thread.base-i); \
	else { \
		a##p = &thread.frame.environment->getRecursive((String)(i)); \
		if(!a##p->isConcrete()) return forceReg(thread, inst, a##p, (String)(i)); \
	} \
} while(0); \
Value const& a = *a##p; 

#define UNCHECKED_OPERAND(a, i) \
Value const& a = __builtin_expect((i) <= 0, true) ? \
		*(thread.base-(i)) : \
		thread.frame.environment->getRecursive((String)(i)); 
	
#define CHECK_OPERAND(a, i) \
do { \
	if((i) > 0 && !a.isConcrete()) return forceReg(thread, inst, &a, (String)(i)); \
} while(0);

// Out register is currently always a register, not memory
#define OUT(thread, i) (*(thread.base-(i)))

// Tracing stuff

//track the heat of back edge operations and invoke the recorder on hot traces
//unused until we begin tracing loops again
static Instruction const * profile_back_edge(Thread & thread, Instruction const * inst) {
	return inst;
}

bool isRecordableType(Type::Enum type) {
	return type == Type::Double || type == Type::Integer || type == Type::Logical;
}

static Instruction const* trace(Thread& thread, Instruction const& inst, Type::Enum type, int64_t length) {
#ifdef ENABLE_JIT
	if(thread.state.jitEnabled && isRecordableType(type) && length >= TRACE_VECTOR_WIDTH) {
		return thread.trace.BeginTracing(thread, &inst);
	}
#endif
	return 0;
}

static Instruction const* trace(Thread& thread, Instruction const& inst, Value const& a) {
	return trace(thread, inst, a.type, a.length);
}

static Instruction const* trace(Thread& thread, Instruction const& inst, Value const& a, Value const& b) {
#ifdef ENABLE_JIT
	if(thread.state.jitEnabled && isRecordableType(a.type) && isRecordableType(b.type) && 
		(a.length >= TRACE_VECTOR_WIDTH || b.length >= TRACE_VECTOR_WIDTH)) {
		return thread.trace.BeginTracing(thread, &inst);
	}
#endif
	return 0;
}



// Control flow instructions

Instruction const* call_op(Thread& thread, Instruction const& inst) {
	OPERAND(f, inst.a);
	if(!f.isFunction())
		_error(std::string("Non-function (") + Type::toString(f.type) + ") as first parameter to call\n");
	Function func(f);
	
	// TODO: using inst.b < 0 to indicate a normal call means that do.call can never use a ..# variable. Not common, but would surely be unexpected for users. Probably best to just have a separate op for do.call?
	
	List arguments;
	Character names;
	Environment* fenv;
	if(inst.b < 0) {
		CompiledCall const& call = thread.frame.prototype->calls[-(inst.b+1)];
		arguments = call.arguments;
		names = call.names;
		if(call.dots < arguments.length)
			ExpandDots(thread, arguments, names, call.dots);
		fenv = CreateEnvironment(thread, func.environment(), thread.frame.environment, call.call);
	} else {
		OPERAND(reg, inst.b);
		if(reg.isObject()) {
			arguments = List(((Object const&)reg).base());
			names = Character(((Object const&)reg).getNames());
		}
		else {
			arguments = List(reg);
		}
		fenv = CreateEnvironment(thread, func.environment(), thread.frame.environment, Null::Singleton());
	}

	MatchArgs(thread, thread.frame.environment, fenv, func, arguments, names);
	return buildStackFrame(thread, fenv, true, func.prototype(), inst.c, &inst+1);
}

Instruction const* ret_op(Thread& thread, Instruction const& inst) {
	// if this stack frame owns the environment, we can free it for reuse
	// as long as we don't return a closure...
	// TODO: but also can't if an assignment to an out of scope variable occurs (<<-, assign) with a value of a closure!
	OPERAND(result, inst.a);	// could this be UNCHECKED?
	if(thread.frame.ownEnvironment && result.isClosureSafe())
		thread.environments.push_back(thread.frame.environment);
	thread.base = thread.frame.returnbase;
	if(thread.frame.i <= 0) {
		REGISTER(thread.frame.i) = result;
	} else {
		thread.frame.env->insert(thread.frame.s) = result;
	}
	Instruction const* returnpc = thread.frame.returnpc;
	thread.pop();
	return returnpc;
}

Instruction const* UseMethod_op(Thread& thread, Instruction const& inst) {
	String generic = (String)inst.a;

	CompiledCall const& call = thread.frame.prototype->calls[inst.b];
	List arguments = call.arguments;
	Character names = call.names;
	if(call.dots < arguments.length)
		ExpandDots(thread, arguments, names, call.dots);

	OPERAND(object, inst.c);
	Character type = klass(thread, object);

	String method;
	Value f = GenericSearch(thread, type, generic, method);

	if(!f.isFunction()) { 
		_error(std::string("no applicable method for '") + thread.externStr(generic) + "' applied to an object of class \"" + thread.externStr(type[0]) + "\"");
	}

	Function func(f);
	Environment* fenv = CreateEnvironment(thread, func.environment(), thread.frame.environment, call.call);
	MatchArgs(thread, thread.frame.environment, fenv, func, arguments, names);	
	fenv->insert(Strings::dotGeneric) = CreateSymbol(generic);
	fenv->insert(Strings::dotMethod) = CreateSymbol(method);
	fenv->insert(Strings::dotClass) = type; 
	return buildStackFrame(thread, fenv, true, func.prototype(), inst.c, &inst+1);
}

Instruction const* jmp_op(Thread& thread, Instruction const& inst) {
	return &inst+inst.a;
}

Instruction const* jc_op(Thread& thread, Instruction const& inst) {
	UNCHECKED_OPERAND(c, inst.c);
	if(c.isLogical1()) {
		if(Logical::isTrue(c.c)) return &inst+inst.a;
		else if(Logical::isFalse(c.c)) return &inst+inst.b;
		else _error("NA where TRUE/FALSE needed"); 
	} else if(c.isInteger1()) {
		if(Integer::isNA(c.i)) _error("NA where TRUE/FALSE needed");
		else if(c.i != 0) return &inst + inst.a;
		else return & inst+inst.b;
	} else if(c.isDouble1()) {
		if(Double::isNA(c.d)) _error("NA where TRUE/FALSE needed");
		else if(c.d != 0) return &inst + inst.a;
		else return & inst+inst.b;
	}
	CHECK_OPERAND(c, inst.c);
	_error("Need single element logical in conditional jump");
}

Instruction const* branch_op(Thread& thread, Instruction const& inst) {
	UNCHECKED_OPERAND(c, inst.c);
	int64_t index = -1;
	if(c.isDouble1()) index = (int64_t)c.d;
	else if(c.isInteger1()) index = c.i;
	else if(c.isLogical1()) index = c.i;
	else if(c.isCharacter1()) {
		for(int64_t i = 1; i <= inst.b; i++) {
			if((String)(&inst+i)->a == c.s) {
				index = i;
				break;
			}
			if(index < 0 && (String)(&inst+i)->a == Strings::empty) {
				index = i;
			}
		}
	}
	if(index >= 1 && index <= inst.b) {
		return &inst + ((&inst+index)->c);
	} 
	CHECK_OPERAND(c, inst.a);
	return &inst+1+inst.b;
}

Instruction const* forbegin_op(Thread& thread, Instruction const& inst) {
	// a = loop variable (e.g. i), b = loop vector(e.g. 1:100), c = counter register
	// following instruction is a jmp that contains offset
	OPERAND(vec, inst.b);
	if((int64_t)vec.length <= 0) {
		return &inst+(&inst+1)->a;	// offset is in following JMP, dispatch together
	} else {
		Element2(vec, 0, thread.frame.environment->insert((String)inst.a));
		Value& counter = REGISTER(inst.c);
		counter.header = vec.length;	// warning: not a valid object, but saves a shift
		counter.i = 1;
		return &inst+2;			// skip over following JMP
	}
}
Instruction const* forend_op(Thread& thread, Instruction const& inst) {
	Value& counter = REGISTER(inst.c);
	if(__builtin_expect((counter.i) < counter.header, true)) {
		OPERAND(vec, inst.b);
		Element2(vec, counter.i, thread.frame.environment->insert((String)inst.a));
		counter.i++;
		return profile_back_edge(thread,&inst+(&inst+1)->a);
	} else {
		return &inst+2;			// skip over following JMP
	}
}

Instruction const* list_op(Thread& thread, Instruction const& inst) {
	std::vector<String> const& dots = thread.frame.environment->dots;
	
	Value& iter = REGISTER(inst.a);
	Value& out = OUT(thread, inst.c);
	OPERAND(elem, inst.b);
	// First time through, make a result vector...
	if(iter.i == 0) {
		out = List(dots.size());
	}
	// Otherwise populate result vector with next element
	else {
		thread.frame.environment->insert((String)-iter.i) = elem;
		((List&)out)[iter.i-1] = elem;
	}

	// If we're all done, check to see if we need to add names and then exit
	if(iter.i == (int64_t)dots.size()) {
		bool nonEmptyName = false;
		for(int i = 0; i < (int64_t)dots.size(); i++) 
			if(dots[i] != Strings::empty) nonEmptyName = true;
		if(nonEmptyName) {
			// TODO: should really just use the names in the dots directly
			Character names(dots.size());
			for(int64_t i = 0; i < (int64_t)dots.size(); i++)
				names[i] = dots[i];
			Object::Init(out, out, names);
		}
		return &inst+1;
	}

	// Not done yet, increment counter, evaluate next ..#
	iter.i++;
	Value const& src = thread.frame.environment->get((String)-iter.i);
	if(!src.isPromise()) {
		OUT(thread, inst.b) = src;
		return &inst;
	}
	else {
		Function f(src);
		Environment* env = f.environment()->DynamicScope();
		assert(env != 0);
		Prototype* prototype = f.prototype();
		return buildStackFrame(thread, env, false, prototype, inst.b, &inst);
	}
}

// Memory access ops

Instruction const* assign_op(Thread& thread, Instruction const& inst) {
	OPERAND(value, inst.c);
	thread.frame.environment->insert((String)inst.a) = value;
	return &inst+1;
}

Instruction const* assign2_op(Thread& thread, Instruction const& inst) {
	// assign2 is always used to assign up at least one scope level...
	// so start off looking up one level...
	assert(thread.frame.environment->LexicalScope() != 0);
	
	OPERAND(value, inst.c);
	
	String s = (String)inst.a;
	Value& dest = thread.frame.environment->LexicalScope()->insertRecursive(s);

	if(!dest.isNil()) {
		dest = value;
	}
	else {
		thread.state.global->insert(s) = value;
	}
	return &inst+1;
}


// everything else should be in registers

Instruction const* mov_op(Thread& thread, Instruction const& inst) {
	OPERAND(value, inst.a);
	OUT(thread, inst.c) = value;
	return &inst+1;
}

Instruction const* iassign_op(Thread& thread, Instruction const& inst) {
	// a = value, b = index, c = dest 
	OPERAND(value, inst.a);
	OPERAND(index, inst.b);
	OPERAND(dest, inst.c);
	SubsetAssign(thread, dest, true, index, value, OUT(thread,inst.c));
	return &inst+1;
}
Instruction const* eassign_op(Thread& thread, Instruction const& inst) {
	// a = value, b = index, c = dest
	OPERAND(value, inst.a);
	OPERAND(index, inst.b);
	OPERAND(dest, inst.c);
	Subset2Assign(thread, dest, true, index, value, OUT(thread,inst.c));
	return &inst+1; 
}

Instruction const* subset_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	OPERAND(i, inst.b);
	if(a.isVector()) {
		if(i.isDouble1()) { Element(a, i.d-1, OUT(thread, inst.c)); return &inst+1; }
		else if(i.isInteger1()) { Element(a, i.i-1, OUT(thread, inst.c)); return &inst+1; }
		else if(i.isLogical1()) { Element(a, Logical::isTrue(i.c) ? 0 : -1, OUT(thread, inst.c)); return &inst+1; }
		else if(i.isCharacter1()) { _error("Subscript out of bounds"); }
		else if(i.isVector()) { SubsetSlow(thread, a, i, OUT(thread, inst.c)); return &inst+1; }
	}
	if(a.isObject() || i.isObject()) { return GenericDispatch(thread, inst, Strings::bracket, a, i, inst.c); } 
	_error("Invalid subset operation");
}

Instruction const* subset2_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	OPERAND(i, inst.b);
	if(a.isVector()) {
		int64_t index = 0;
		if(i.isDouble1()) { index = i.d-1; }
		else if(i.isInteger1()) { index = i.i-1; }
		else if(i.isLogical1() && Logical::isTrue(i.c)) { index = 1-1; }
		else if(i.isVector() && (i.length == 0 || i.length > 1)) { 
			_error("Attempt to select less or more than 1 element in subset2"); 
		}
		else { _error("Subscript out of bounds"); }
		Element2(a, index, OUT(thread, inst.c));
		return &inst+1;
	}
	if(a.isObject() || i.isObject()) { return GenericDispatch(thread, inst, Strings::bb, a, i, inst.c); } 
	_error("Invalid subset2 operation");
}

Instruction const* colon_op(Thread& thread, Instruction const& inst) {
	OPERAND(From, inst.a);
	OPERAND(To, inst.b);
	double from = asReal1(From);
	double to = asReal1(To);
	OUT(thread,inst.c) = Sequence(from, to>from?1:-1, fabs(to-from)+1);
	return &inst+1;
}


Instruction const* seq_op(Thread& thread, Instruction const& inst) {
	OPERAND(Len, inst.a);
	OPERAND(Step, inst.b);
	int64_t len = As<Integer>(thread, Len)[0];
	int64_t step = As<Integer>(thread, Step)[0];
	
	Instruction const* jit = trace(thread, inst, Type::Integer, len);
	if(jit) return jit;
	
	OUT(thread, inst.c) = Sequence(len, 1, step);
	return &inst+1;
}


#define OP(Name, string, Op, Group, Func) \
Instruction const* Name##_op(Thread& thread, Instruction const& inst) { \
	UNCHECKED_OPERAND(a, inst.a);	\
	Value & c = OUT(thread, inst.c);	\
	if(a.isDouble1())  { Name##VOp<Double>::Scalar(thread, a.d, c); return &inst+1; } \
	if(a.isInteger1()) { Name##VOp<Integer>::Scalar(thread, a.i, c); return &inst+1; } \
	if(a.isLogical1()) { Name##VOp<Logical>::Scalar(thread, a.c, c); return &inst+1; } \
	if(a.isObject()) { return GenericDispatch(thread, inst, Strings::Op, a, inst.c); } \
	CHECK_OPERAND(a, inst.a); \
	Instruction const* jit = trace(thread, inst, a); \
	if(jit) return jit; \
	\
	Group##Dispatch<Name##VOp>(thread, a, c); \
	return &inst+1; \
}
UNARY_FOLD_SCAN_BYTECODES(OP)
#undef OP


#define OP(Name, string, Op, Group, Func) \
Instruction const* Name##_op(Thread& thread, Instruction const& inst) { \
	UNCHECKED_OPERAND(a, inst.a);	\
	UNCHECKED_OPERAND(b, inst.b);	\
	Value & c = OUT(thread, inst.c);	\
        if(a.isDouble1()) {			\
		if(b.isDouble1()) { Name##VOp<Double,Double>::Scalar(thread, a.d, b.d, c); return &inst+1; } \
		if(b.isInteger1()) { Name##VOp<Double,Integer>::Scalar(thread, a.d, b.i, c); return &inst+1; } \
		if(b.isLogical1()) { Name##VOp<Double,Logical>::Scalar(thread, a.d, b.c, c); return &inst+1; } \
        }	\
        else if(a.isInteger1()) {	\
		if(b.isDouble1()) { Name##VOp<Integer,Double>::Scalar(thread, a.i, b.d, c); return &inst+1; } \
		if(b.isInteger1()) { Name##VOp<Integer,Integer>::Scalar(thread, a.i, b.i, c); return &inst+1; } \
		if(b.isLogical1()) { Name##VOp<Integer,Logical>::Scalar(thread, a.i, b.c, c); return &inst+1; } \
        } \
        else if(a.isLogical1()) {	\
		if(b.isDouble1()) { Name##VOp<Logical,Double>::Scalar(thread, a.c, b.d, c); return &inst+1; } \
		if(b.isInteger1()) { Name##VOp<Logical,Integer>::Scalar(thread, a.c, b.i, c); return &inst+1; } \
		if(b.isLogical1()) { Name##VOp<Logical,Logical>::Scalar(thread, a.c, b.c, c); return &inst+1; } \
        } \
	CHECK_OPERAND(a, inst.a); CHECK_OPERAND(b, inst.b); \
	if(a.isObject() || b.isObject()) { return GenericDispatch(thread, inst, Strings::Op, a, b, inst.c); } \
	Instruction const* jit = trace(thread, inst, a, b); \
	if(jit) return jit; \
\
	Group##Dispatch<Name##VOp>(thread, a, b, c);	\
	return &inst+1;	\
}
BINARY_BYTECODES(OP)
#undef OP

Instruction const* ifelse_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Instruction const* jit = trace(thread, inst, a);
	if(jit) return jit;

	_error("ifelse not defined in scalar yet");
	return &inst+2; 
}

Instruction const* split_op(Thread& thread, Instruction const& inst) {
	OPERAND(c, inst.c);
	Instruction const* jit = trace(thread, inst, c);
	if(jit) return jit;
		
	_error("split not defined in scalar yet");
	return &inst+2; 
}

Instruction const* function_op(Thread& thread, Instruction const& inst) {
	OUT(thread, inst.c) = Function(thread.frame.prototype->prototypes[inst.a], thread.frame.environment);
	return &inst+1;
}
Instruction const* logical1_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Integer i = As<Integer>(thread, a);
	OUT(thread, inst.c) = Logical(i[0]);
	return &inst+1;
}
Instruction const* integer1_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Integer i = As<Integer>(thread, a);
	OUT(thread, inst.c) = Integer(i[0]);
	return &inst+1;
}
Instruction const* double1_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	int64_t length = asReal1(a);
	Double d(length);
	for(int64_t i = 0; i < length; i++) d[i] = 0;
	OUT(thread, inst.c) = d;
	return &inst+1;
}
Instruction const* character1_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Integer i = As<Integer>(thread, a);
	Character r = Character(i[0]);
	for(int64_t j = 0; j < r.length; j++) r[j] = Strings::empty;
	OUT(thread, inst.c) = r;
	return &inst+1;
}
Instruction const* raw1_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Integer i = As<Integer>(thread, a);
	OUT(thread, inst.c) = Raw(i[0]);
	return &inst+1;
}
Instruction const* type_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Character c(1);
	// Should have a direct mapping from type to symbol.
	c[0] = thread.internStr(Type::toString(a.type));
	OUT(thread, inst.c) = c;
	return &inst+1;
}
Instruction const* length_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	if(a.isVector())
		Integer::InitScalar(OUT(thread, inst.c), a.length);
	else
		Integer::InitScalar(OUT(thread, inst.c), 1);
	return &inst+1;
}
Instruction const* missing_op(Thread& thread, Instruction const& inst) {
	// TODO: in R this is recursive. If this function was passed a parameter that
	// was missing in the outer scope, then it should be missing here too. But
	// missingness doesn't propogate through expressions, leading to strange behavior:
	// 	f <- function(x,y) g(x,y)
	//	g <- function(x,y) missing(y)
	//	f(1) => TRUE
	// but
	//	f <- function(x,y) g(x,y+1)
	//	g <- function(x,y) missing(y)
	//	f(1) => FALSE
	// but
	//	f <- function(x,y) g(x,y+1)
	//	g <- function(x,y) y
	//	f(1) => Error in y+1: 'y' is missing
	// For now I'll keep the simpler non-recursive semantics. Missing solely means
	// whether or not this scope was passed a value, irregardless of whether that
	// value is missing at a higher level.
	String s = (String)inst.a;
	Value const& v = thread.frame.environment->get(s);
	bool missing = v.isNil() || v.isDefault();
	Logical::InitScalar(OUT(thread, inst.c), missing);
	return &inst+1;
}
Instruction const* mmul_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	OPERAND(b, inst.b);
	OUT(thread, inst.c) = MatrixMultiply(thread, a, b);
	return &inst+1;
}
Instruction const* strip_op(Thread& thread, Instruction const& inst) {
	OPERAND(a, inst.a);
	Value& c = OUT(thread, inst.c);
	c = a;
	if(c.isObject())
		c = ((Object const&)c).base();
	return &inst+1;
}

Instruction const* internal_op(Thread& thread, Instruction const& inst) {
       thread.state.internalFunctions[inst.a].ptr(thread, &REGISTER(inst.b), OUT(thread, inst.c));
       return &inst+1;
}

Instruction const* done_op(Thread& thread, Instruction const& inst) {
	// not used. When this instruction is hit, interpreter exits.
	return 0;
}



//
//    Main interpreter loop 
//
//__attribute__((__noinline__,__noclone__)) 
void interpret(Thread& thread, Instruction const* pc) {

#ifdef USE_THREADED_INTERPRETER
	if(pc == 0) { 
    		#define LABELS_THREADED(name,type,...) (void*)&&name##_label,
		static const void* labels[] = {BYTECODES(LABELS_THREADED)};
		glabels = labels;
		return;
	}

	goto *(pc->ibc);
	#define LABELED_OP(name,type,...) \
		name##_label: \
			{ pc = name##_op(thread, *pc); goto *(pc->ibc); } 
	STANDARD_BYTECODES(LABELED_OP)
	done_label: {}
#else
	while(pc->bc != ByteCode::done) {
		switch(pc->bc) {
			#define SWITCH_OP(name,type,...) \
				case ByteCode::name: { pc = name##_op(thread, *pc); } break;
			BYTECODES(SWITCH_OP)
		};
	}
#endif
}

// ensure glabels is inited before we need it.
void State::interpreter_init(Thread& thread) {
#ifdef USE_THREADED_INTERPRETER
	interpret(thread, 0);
#endif
}

Value Thread::eval(Function const& function) {
	return eval(function.prototype(), function.environment());
}

Value Thread::eval(Prototype const* prototype) {
	return eval(prototype, frame.environment);
}

Value Thread::eval(Prototype const* prototype, Environment* environment) {
	Instruction done(ByteCode::done);
#ifdef USE_THREADED_INTERPRETER
	done.ibc = glabels[ByteCode::done];
#endif
	Value* old_base = base;
	int64_t stackSize = stack.size();
	
	// Build a half-hearted stack frame for the result. Necessary for the trace recorder.
	StackFrame& s = push();
	s.environment = 0;
	s.prototype = 0;
	s.returnbase = base;
	base -= 1;
	Value* result = base;
	
	Instruction const* run = buildStackFrame(*this, environment, false, prototype, 0, &done);
	try {
		interpret(*this, run);
		base = old_base;
		pop();
	} catch(...) {
		base = old_base;
		stack.resize(stackSize);
		throw;
	}
	return *result;
}

