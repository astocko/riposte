
#include "internal.h"
#include "compiler.h"
#include "parser.h"
#include "library.h"
#include <math.h>
#include <fstream>

const MaxOp<TComplex>::A MaxOp<TComplex>::Base = std::complex<double>(0,0);
const MinOp<TComplex>::A MinOp<TComplex>::Base = std::complex<double>(0,0);
const AnyOp::A AnyOp::Base = 0;
const AllOp::A AllOp::Base = 1;

void checkNumArgs(List const& args, int64_t nargs) {
	if(args.length > nargs) _error("unused argument(s)");
	else if(args.length < nargs) _error("too few arguments");
}

int64_t cat(State& state, Call const& call, List const& args) {
	printf("%s\n", state.stringify(force(state, args[0])).c_str());
	state.registers[0] = Null::singleton;
	return 1;
}

int64_t library(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);

	Character from = As<Character>(state, force(state, args[0]));
	if(from.length > 0) {
		loadLibrary(state, from[0].toString(state));
	}
	state.registers[0] = Null::singleton;
	return 1;
}

int64_t rm(State& state, Call const& call, List const& args) {
	for(int64_t i = 0; i < args.length; i++) 
		if(expression(args[i]).type != Type::R_symbol && expression(args[i]).type != Type::R_character) 
			_error("rm() arguments must be symbols or character vectors");
	for(int64_t i = 0; i < args.length; i++) {
		state.global->rm(expression(args[i]));
	}
	state.registers[0] = Null::singleton;
	return 1;
}

int64_t sequence(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 3);

	Value from = force(state, args[0]);
	Value by   = force(state, args[1]);
	Value len  = force(state, args[2]);

	double f = asReal1(from);
	double b = asReal1(by);
	double l = asReal1(len);

	state.registers[0] = Sequence(f, b, l);	
	return 1;
}

int64_t repeat(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 3);
	Value from = force(state, args[0]);
	assert(args.length == 3);
	
	Value vec  = force(state, args[0]);
	Value each = force(state, args[1]);
	Value len  = force(state, args[2]);
	
	double v = asReal1(vec);
	//double e = asReal1(each);
	double l = asReal1(len);
	
	Double r(l);
	for(int64_t i = 0; i < l; i++) {
		r[i] = v;
	}
	state.registers[0] = r;
	return 1;
}

int64_t inherits(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 3);
	Value x = force(state, args[0]);
	Character what = force(state, args[1]);
	Logical which = force(state, args[2]);
	// NYI: which
	Character c = klass(state, x);
	bool inherits = false;
	for(int64_t i = 0; i < what.length && !inherits; i++) {
		for(int64_t j = 0; j < c.length && !inherits; j++) {
			if(what[i] == c[j]) inherits = true;
		}
	}
	state.registers[0] = Logical::c(inherits);
	return 1;
}

int64_t attr(State& state, Call const& call, List const& args)
{
	checkNumArgs(args, 3);
	// NYI: exact
	Value object = force(state, args[0]);
	Character which = force(state, args[1]);
	state.registers[0] = getAttribute(object, which[0]);
	return 1;
}

int64_t assignAttr(State& state, Call const& call, List const& args)
{
	checkNumArgs(args, 3);
	Value object = force(state, args[0]);
	Character which = force(state, args[1]);
	state.registers[0] = setAttribute(object, which[0], force(state, args[2]));
	return 1;
}

Type cTypeCast(Value const& v, Type t)
{
	Type r;
	r.v = std::max(v.type.Enum(), t.Enum());
	return r;
}

int64_t list(State& state, Call const& call, List const& args) {
	List out(args.length);
	for(int64_t i = 0; i < args.length; i++) out[i] = force(state, args[i]);
	out.attributes = args.attributes;
	state.registers[0] = out;
	return 1;
}

int64_t unlist(State& state, Call const& call, List const& args) {
	//checkNumArgs(args, 1);
	Value v = force(state, args[0]);
	if(!v.isList()) {
		state.registers[0] = v;
		return 1;
	}
	List from = v;
	int64_t total = 0;
	Type type = Type::R_null;
	for(int64_t i = 0; i < from.length; i++) {
		from[i] = force(state, from[i]);
		total += from[i].length;
		type = cTypeCast(from[i], type);
	}
	Vector out = Vector(type, total);
	int64_t j = 0;
	for(int64_t i = 0; i < from.length; i++) {
		Insert(state, Vector(from[i]), 0, out, j, Vector(from[i]).length);
		j += from[i].length;
	}
	if(hasNames(from))
	{
		Character names = getNames(from);
		Character outnames(total);
		int64_t j = 0;
		for(int64_t i = 0; i < (int64_t)from.length; i++) {
			for(int64_t m = 0; m < from[i].length; m++, j++) {
				// NYI: R makes these names distinct
				outnames[j] = names[i];
			}
		}
		setNames(out, outnames);
	}
	state.registers[0] = out;
	return 1;
}

Vector Subset(State& state, Vector const& a, Vector const& i)	{
	if(i.type == Type::R_double || i.type == Type::R_integer) {
		Integer index = As<Integer>(state, i);
		int64_t positive = 0, negative = 0;
		for(int64_t i = 0; i < index.length; i++) {
			if(index[i] > 0 || Integer::isNA(index[i])) positive++;
			else if(index[i] < 0) negative++;
		}
		if(positive > 0 && negative > 0)
			_error("mixed subscripts not allowed");
		else if(positive > 0) {
			switch(a.type.Enum()) {
				case Type::E_R_double: return SubsetInclude<Double>::eval(state, a, index, positive); break;
				case Type::E_R_integer: return SubsetInclude<Integer>::eval(state, a, index, positive); break;
				case Type::E_R_logical: return SubsetInclude<Logical>::eval(state, a, index, positive); break;
				case Type::E_R_character: return SubsetInclude<Character>::eval(state, a, index, positive); break;
				case Type::E_R_list: return SubsetInclude<List>::eval(state, a, index, positive); break;
				default: _error("NYI"); break;
			};
		}
		else if(negative > 0) {
			switch(a.type.Enum()) {
				case Type::E_R_double: return SubsetExclude<Double>::eval(state, a, index, negative); break;
				case Type::E_R_integer: return SubsetExclude<Integer>::eval(state, a, index, negative); break;
				case Type::E_R_logical: return SubsetExclude<Logical>::eval(state, a, index, negative); break;
				case Type::E_R_character: return SubsetExclude<Character>::eval(state, a, index, negative); break;
				case Type::E_R_list: return SubsetExclude<List>::eval(state, a, index, negative); break;
				default: _error("NYI"); break;
			};	
		}
		else {
			return Vector(a.type, 0);
		}
	}
	else if(i.type == Type::R_logical) {
		Logical index = Logical(i);
		switch(a.type.Enum()) {
			case Type::E_R_double: return SubsetLogical<Double>::eval(state, a, index); break;
			case Type::E_R_integer: return SubsetLogical<Integer>::eval(state, a, index); break;
			case Type::E_R_logical: return SubsetLogical<Logical>::eval(state, a, index); break;
			case Type::E_R_character: return SubsetLogical<Character>::eval(state, a, index); break;
			case Type::E_R_list: return SubsetLogical<List>::eval(state, a, index); break;
			default: _error("NYI"); break;
		};	
	}
	_error("NYI indexing type");
	return Null::singleton;
}

int64_t subset(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 2);
        Vector a = Vector(force(state, args[0]));
        Vector i = Vector(force(state, args[1]));
	state.registers[0] = Subset(state, a,i);
        return 1;
}

int64_t subset2(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 2);

        Value a = force(state, args[0]);
        Value b = force(state, args[1]);
	if(b.type == Type::R_character && hasNames(a)) {
		Symbol i = Character(b)[0];
		Character c = getNames(a);
		
		int64_t j = 0;
		for(;j < c.length; j++) {
			if(c[j] == i)
				break;
		}
		if(j < c.length) {
			state.registers[0] = Element2(a, j);
			return 1;
		}
	}
	else if(b.type == Type::R_integer) {
		state.registers[0] = Element2(a, Integer(b)[0]-1);
		return 1;
	}
	else if(b.type == Type::R_double) {
		state.registers[0] = Element2(a, (int64_t)Double(b)[0]-1);
		return 1;
	}
	state.registers[0] = Null::singleton;
	return 1;
} 

int64_t dollar(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 2);

        Value a = force(state, args[0]);
        int64_t i = Symbol(expression(args[1])).i;
	if(hasNames(a)) {
		Character c = getNames(a);
		int64_t j = 0;
		for(;j < c.length; j++) {
			if(c[j] == i)
				break;
		}
		if(j < c.length) {
			state.registers[0] = Element2(a, j);
			return 1;
		}
	}
	state.registers[0] = Null::singleton;
	return 1;
} 

int64_t length(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Vector a = force(state, args[0]);
	Integer i(1);
	i[0] = a.length;
	state.registers[0] = i;
	return 1;
}

int64_t quote(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	state.registers[0] = expression(args[0]);
	return 1;
}

int64_t eval_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 2);
	Value expr = force(state, args[0]);
	Value envir = force(state, args[1]);
	//Value enclos = force(state, call[3]);
	eval(state, Compiler::compile(state, expr), REnvironment(envir).ptr());
	return 1;
}

int64_t lapply(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 2);
	List x = As<List>(state, force(state, args[0]));
	Value func = force(state, args[1]);

	Call apply(2);
	apply[0] = func;

	List result(x.length);
	
	for(int64_t i = 0; i < x.length; i++) {
		apply[1] = x[i];
		eval(state, Compiler::compile(state, apply));
		result[i] = state.registers[0];
	}

	state.registers[0] = result;
	return 1;
}

int64_t tlist(State& state, Call const& call, List const& args) {
	int64_t length = args.length > 0 ? 1 : 0;
	List a = Clone(args);
	for(int64_t i = 0; i < a.length; i++) {
		a[i] = force(state, a[i]);
		if(a[i].isVector() && a[i].length != 0 && length != 0)
			length = std::max(length, a[i].length);
	}
	List result(length);
	for(int64_t i = 0; i < length; i++) {
		List element(args.length);
		for(int64_t j = 0; j < a.length; j++) {
			if(a[j].isVector())
				element[j] = Element2(Vector(a[j]), i%a[j].length);
			else
				element[j] = a[j];
		}
		result[i] = element;
	}
	state.registers[0] = result;
	return 1;
}

int64_t source(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value file = force(state, args[0]);
	std::ifstream t(Character(file)[0].toString(state).c_str());
	std::stringstream buffer;
	buffer << t.rdbuf();
	std::string code = buffer.str();

	Parser parser(state);
	Value value;
	parser.execute(code.c_str(), code.length(), true, value);	
	
	eval(state, Compiler::compile(state, value));
	return 1;
}

int64_t switch_fn(State& state, Call const& call, List const& args) {
	Value one = force(state, args[0]);
	if(one.type == Type::R_integer && Integer(one).length == 1) {
		int64_t i = Integer(one)[0];
		if(i >= 1 && (int64_t)i <= args.length) {state.registers[0] = force(state, args[i]); return 1; }
	} else if(one.type == Type::R_double && Double(one).length == 1) {
		int64_t i = (int64_t)Double(one)[0];
		if(i >= 1 && (int64_t)i <= args.length) {state.registers[0] = force(state, args[i]); return 1; }
	} else if(one.type == Type::R_character && Character(one).length == 1 && hasNames(args)) {
		Character names = getNames(args);
		for(int64_t i = 1; i < args.length; i++) {
			if(names[i] == Character(one)[0]) {
				while(args[i].type == Type::I_nil && i < args.length) i++;
				state.registers[0] = i < args.length ? force(state, args[i]) : (Value)(Null::singleton);
				return 1;
			}
		}
		for(int64_t i = 1; i < args.length; i++) {
			if(names[i] == Symbol::empty) {
				state.registers[0] = force(state, args[i]);
				return 1;
			}
		}
	}
	state.registers[0] = Null::singleton;
	return 1;
}

int64_t environment(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value e = force(state, args[0]);
	if(e.type == Type::R_null) {
		state.registers[0] = REnvironment(state.global);
		return 1;
	}
	else if(e.type == Type::R_function) {
		state.registers[0] = REnvironment(Function(e).s());
		return 1;
	}
	state.registers[0] = Null::singleton;
	return 1;
}

int64_t parentframe(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	int64_t i = (int64_t)asReal1(force(state, args[0]));
	Environment* e = state.global;
	for(int64_t j = 0; j < i-1 && e != NULL; j++) {
		e = e->dynamicParent();
	}
	state.registers[0] = REnvironment(e);
	return 1;
}

int64_t stop_fn(State& state, Call const& call, List const& args) {
	// this should stop whether or not the arguments are correct...
	std::string message = "user stop";
	if(args.length > 0) {
		if(args[0].type == Type::R_character && Character(args[0]).length > 0) {
			message = Character(args[0])[0].toString(state);
		}
	}
	_error(message);
	return 0;
}

int64_t warning_fn(State& state, Call const& call, List const& args) {
	std::string message = "user warning";
	if(args.length > 0) {
		if(args[0].type == Type::R_character && Character(args[0]).length > 0) {
			message = Character(args[0])[0].toString(state);
		}
	}
	_warning(state, message);
	state.registers[0] = Character::c(state, message);
	return 1;
} 

int64_t missing(State& state, Call const& call, List const& args) {
	Symbol s = expression(args[0]); 
	Value v;
	bool success = state.global->getRaw(s, v);
	state.registers[0] =  (!success || v.type == Type::I_default) ? Logical::True() : Logical::False();
	return 1;
}

int64_t max_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<FoldLeft, MaxOp>(state, a, state.registers[0]);
	return 1;
}

int64_t min_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<FoldLeft, MinOp>(state, a, state.registers[0]);
	return 1;
}

int64_t sum_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<FoldLeft, SumOp>(state, a, state.registers[0]);
	return 1;
}

int64_t prod_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<FoldLeft, ProdOp>(state, a, state.registers[0]);
	return 1;
}

int64_t cummax_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<ScanLeft, MaxOp>(state, a, state.registers[0]);
	return 1;
}

int64_t cummin_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<ScanLeft, MinOp>(state, a, state.registers[0]);
	return 1;
}

int64_t cumsum_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<ScanLeft, SumOp>(state, a, state.registers[0]);
	return 1;
}

int64_t cumprod_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryArith<ScanLeft, ProdOp>(state, a, state.registers[0]);
	return 1;
}

int64_t any_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryLogical<FoldLeft, AnyOp>(state, a, state.registers[0]);
	return 1;
}

int64_t all_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryLogical<FoldLeft, AllOp>(state, a, state.registers[0]);
	return 1;
}

int64_t isna_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryFilter<Zip1, IsNAOp>(state, a, state.registers[0]);
	return 1;
}

int64_t isnan_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryFilter<Zip1, IsNaNOp>(state, a, state.registers[0]);
	return 1;
}

int64_t nchar_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 3);
	Value a = force(state, args[0]);
	// NYI: type or allowNA
	unaryCharacter<Zip1, NcharOp>(state, a, state.registers[0]);
	return 1;
}

int64_t nzchar_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryCharacter<Zip1, NzcharOp>(state, a, state.registers[0]);
	return 1;
}

int64_t isfinite_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryFilter<Zip1, IsFiniteOp>(state, a, state.registers[0]);
	return 1;
}

int64_t isinfinite_fn(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	Value a = force(state, args[0]);
	unaryFilter<Zip1, IsInfiniteOp>(state, a, state.registers[0]);
	return 1;
}

int64_t paste(State& state, Call const& call, List const& args) {
	Character a = As<Character>(state, force(state, args[0]));
	Character sep = As<Character>(state, force(state, args[1]));
	std::string result = "";
	for(int64_t i = 0; i+1 < a.length; i++) {
		result = result + a[i].toString(state) + sep[0].toString(state);
	}
	if(a.length > 0) result = result + a[a.length-1].toString(state);
	state.registers[0] = Character::c(state, result);
	return 1;
}

int64_t deparse(State& state, Call const& call, List const& args) {
	Value v = force(state, args[0]);
	state.registers[0]= Character::c(state, state.deparse(v));
	return 1;
}

int64_t substitute(State& state, Call const& call, List const& args) {
	checkNumArgs(args, 1);
	state.registers[0] = expression(args[0]);
	Value v = args[0];
	while(v.type == Type::I_promise) v = Closure(v).code()->expression;
	
	if(v.isSymbol()) {
		Value r;
		if(state.global->getRaw(v,r)) v = r;
		while(v.type == Type::I_promise) v = Closure(v).code()->expression;
	}
 	state.registers[0] = v;
	return 1;
}


void importCoreLibrary(State& state)
{
	Environment* env = state.path[0];

	env->assign(Symbol(state,"max"), CFunction(max_fn));
	env->assign(Symbol(state,"min"), CFunction(min_fn));
	env->assign(Symbol(state,"sum"), CFunction(sum_fn));
	env->assign(Symbol(state,"prod"), CFunction(prod_fn));
	env->assign(Symbol(state,"cummax"), CFunction(cummax_fn));
	env->assign(Symbol(state,"cummin"), CFunction(cummin_fn));
	env->assign(Symbol(state,"cumsum"), CFunction(cumsum_fn));
	env->assign(Symbol(state,"cumprod"), CFunction(cumprod_fn));
	env->assign(Symbol(state,"any"), CFunction(any_fn));
	env->assign(Symbol(state,"all"), CFunction(all_fn));
	env->assign(Symbol(state,"nchar"), CFunction(nchar_fn));
	env->assign(Symbol(state,"nzchar"), CFunction(nzchar_fn));
	env->assign(Symbol(state,"is.na"), CFunction(isna_fn));
	env->assign(Symbol(state,"is.nan"), CFunction(isnan_fn));
	env->assign(Symbol(state,"is.finite"), CFunction(isfinite_fn));
	env->assign(Symbol(state,"is.infinite"), CFunction(isinfinite_fn));
	
	env->assign(Symbol(state,"cat"), CFunction(cat));
	env->assign(Symbol(state,"library"), CFunction(library));
	env->assign(Symbol(state,"rm"), CFunction(rm));
	env->assign(Symbol(state,"inherits"), CFunction(inherits));
	
	env->assign(Symbol(state,"seq"), CFunction(sequence));
	env->assign(Symbol(state,"rep"), CFunction(repeat));
	
	env->assign(Symbol(state,"attr"), CFunction(attr));
	env->assign(Symbol(state,"attr<-"), CFunction(assignAttr));
	
	env->assign(Symbol(state,"list"), CFunction(list));
	env->assign(Symbol(state,"unlist"), CFunction(unlist));
	env->assign(Symbol(state,"length"), CFunction(length));
	
	env->assign(Symbol(state,"["), CFunction(subset));
	env->assign(Symbol(state,"[["), CFunction(subset2));
	env->assign(Symbol(state,"$"), CFunction(dollar));

	env->assign(Symbol(state,"switch"), CFunction(switch_fn));

	env->assign(Symbol(state,"eval"), CFunction(eval_fn));
	env->assign(Symbol(state,"quote"), CFunction(quote));
	env->assign(Symbol(state,"source"), CFunction(source));

	env->assign(Symbol(state,"lapply"), CFunction(lapply));
	env->assign(Symbol(state,"t.list"), CFunction(tlist));

	env->assign(Symbol(state,"environment"), CFunction(environment));
	env->assign(Symbol(state,"parent.frame"), CFunction(parentframe));
	env->assign(Symbol(state,"missing"), CFunction(missing));
	
	env->assign(Symbol(state,"stop"), CFunction(stop_fn));
	env->assign(Symbol(state,"warning"), CFunction(warning_fn));
	
	env->assign(Symbol(state,"paste"), CFunction(paste));
	env->assign(Symbol(state,"deparse"), CFunction(deparse));
	env->assign(Symbol(state,"substitute"), CFunction(substitute));
}

