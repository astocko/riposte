
#include "jit.h"
#include "bc.h"
#include "interpreter.h"
#include "call.h"
#include "ops.h"

DEFINE_ENUM_TO_STRING(TraceOpCode, TRACE_ENUM)

const JIT::Shape JIT::Shape::Empty = { 0, 0 };
const JIT::Shape JIT::Shape::Scalar = { 1, 1 };

JIT::IRRef JIT::insert(
        std::vector<IR>& t,
        TraceOpCode::Enum op, 
        IRRef a, 
        IRRef b, 
        IRRef c,
        Type::Enum type, 
        Shape in,
        Shape out) {
    IR ir = (IR) { op, a, b, c, type, in, out };
    t.push_back(ir);
    return (IRRef) { t.size()-1 };
}

JIT::Shape JIT::SpecializeLength(size_t length, IRRef irlength, Instruction const* inst) {
    // if short enough, guard length and insert a constant length instead
    if(length <= SPECIALIZE_LENGTH) {
        IRRef s = constant(Integer::c(length));
        return Shape(s, length);
    }
    else {
        return Shape(irlength, length);
    }
}

JIT::Shape JIT::SpecializeValue(Value const& v, IR ir, Instruction const* inst) {
    if(v.isNull())
        return Shape::Empty;
    else if(v.isVector()) {
        trace.push_back(ir);
        return SpecializeLength((size_t)v.length, trace.size()-1, inst);
    }
    else
        return Shape::Scalar;
}

JIT::IRRef JIT::load(Thread& thread, int64_t a, Instruction const* reenter) {

    // registers
    OPERAND(operand, a);

    IRRef r;
    
    if(a <= 0) {
        Variable v = { -1, (thread.base+a)-(thread.registers+DEFAULT_NUM_REGISTERS)};

        // check if we can forward
        if(slots.find(v) != slots.end())
            r = slots[v];
        else {
            Shape s = SpecializeValue(operand, IR(TraceOpCode::slength, (IRRef)-1, v.i, Type::Integer, Shape::Empty, Shape::Scalar), reenter);
            r = insert(trace, TraceOpCode::sload, (IRRef)-1, v.i, 0, operand.type, Shape::Empty, s);
            slots[v] = r;
        }
    }
    else {
        IRRef aa = constant(Character::c((String)a));
        
        Environment const* env = thread.frame.environment;
        r = insert(trace, TraceOpCode::curenv, 0, 0, 0, Type::Environment, Shape::Empty, Shape::Scalar);
        while(!env->has((String)a)) {
            env = env->LexicalScope();
            IRRef g = insert(trace, TraceOpCode::load, r, aa, 0, Type::Nil, Shape::Scalar, Shape::Scalar);
            reenters[g] = (Reenter) { reenter, true };
            r = insert(trace, TraceOpCode::lenv, r, 0, 0, Type::Environment, Shape::Scalar, Shape::Scalar);
        }
        
        Value const& operand = env->get((String)a);
        Variable v = { r, (int64_t)aa }; 
        Shape s = SpecializeValue(operand, IR(TraceOpCode::elength, v.env, v.i, Type::Integer, Shape::Empty, Shape::Scalar), reenter);
        r = insert(trace, TraceOpCode::load, v.env, v.i, 0, operand.type, Shape::Empty, s);
    }
    reenters[r] = (Reenter) { reenter, true };
    return r;
}

JIT::IRRef JIT::store(Thread& thread, IRRef a, int64_t c) {
    if(c <= 0) {
        Variable v = { -1, (thread.base+c)-(thread.registers+DEFAULT_NUM_REGISTERS)};
        IRRef r = insert(trace, TraceOpCode::sstore, -1, v.i, a, trace[a].type, trace[a].out, Shape::Empty);
        slots[v] = a;
    }
    else {
        IRRef cc = constant(Character::c((String)c));
        IRRef e = insert(trace, TraceOpCode::curenv, 0, 0, 0, Type::Environment, Shape::Empty, Shape::Scalar);
        Variable v = { e, (int64_t)cc };
        insert(trace, TraceOpCode::store, v.env, v.i, a, Type::Nil, trace[a].out, Shape::Empty);
    }
    return a;
}

void JIT::emitPush(Thread const& thread) {
    StackFrame frame;
    frame.environment = getEnv(thread.frame.environment);
    frame.prototype = thread.frame.prototype;
    frame.returnpc = thread.frame.returnpc;
    frame.returnbase = thread.frame.returnbase;
    frame.dest = thread.frame.dest;
    frame.env = getEnv(thread.frame.env);
    IRRef a = insert(trace, TraceOpCode::push, getEnv(thread.frame.environment), 0, 0, Type::Nil, Shape::Scalar, Shape::Empty);
    frames[a] = frame;
}

JIT::IRRef JIT::cast(IRRef a, Type::Enum type) {
    if(trace[a].type != type) {
        Shape s = trace[a].out;
        if(type == Type::Double)
            return insert(trace, TraceOpCode::asdouble, a, 0, 0, type, s, s);
        else if(type == Type::Integer)
            return insert(trace, TraceOpCode::asinteger, a, 0, 0, type, s, s);
        else if(type == Type::Logical)
            return insert(trace, TraceOpCode::aslogical, a, 0, 0, type, s, s);
        else if(type == Type::Character)
            return insert(trace, TraceOpCode::ascharacter, a, 0, 0, type, s, s);
        else
            _error("Unexpected cast");
    }
    else {
        return a;
    }
}

JIT::IRRef JIT::rep(IRRef a, Shape target) {
    if(trace[a].out != target) {
        IRRef l = trace[a].out.length;
        IRRef e = constant(Integer::c(1));
        IRRef r = insert(trace, TraceOpCode::rep, l, e, 0, trace[e].type, target, target);
        return insert(trace, TraceOpCode::gather, a, r, 0, trace[a].type, target, target);
    }
    else {
        return a;
    }
}

JIT::IRRef JIT::EmitUnary(TraceOpCode::Enum op, IRRef a, Type::Enum rty, Type::Enum mty) {
   return insert(trace, op, cast(a, mty), 0, 0, rty, trace[a].out, trace[a].out);
}

JIT::IRRef JIT::EmitFold(TraceOpCode::Enum op, IRRef a, Type::Enum rty, Type::Enum mty) {
   return insert(trace, op, cast(a, mty), 0, 0, rty, trace[a].out, Shape::Scalar);
}

JIT::Shape JIT::MergeShapes(Shape a, Shape b, Instruction const* inst) {
    Shape shape = Shape::Empty;
    if(a == b) {
        shape = a;
    }
    else if(a == Shape::Empty || b == Shape::Empty) {
        shape = Shape::Empty;
    }
    else if(a.traceLength == b.traceLength) {
        shape = a.length < b.length ? a : b;
        // forward shapes, TODO: make this more efficient
        IRRef n = std::min(a.length, b.length);
        IRRef o = std::max(a.length, b.length);
        for(size_t i = 0; i < trace.size(); ++i) {
            if(trace[i].in.length == o) trace[i].in.length = n;
            if(trace[i].out.length == o) trace[i].out.length = n;
        }
    }
    else if(a.traceLength < b.traceLength) {
        IRRef x = insert(trace, TraceOpCode::le, a.length, b.length, 0,
                Type::Logical, Shape::Scalar, Shape::Scalar);
        IRRef y = insert(trace, TraceOpCode::gt, a.length, 0, 0,
                Type::Logical, Shape::Scalar, Shape::Scalar);
        IRRef z = insert(trace, TraceOpCode::land, x, y, 0, 
                Type::Logical, Shape::Scalar, Shape::Scalar);
        IRRef g = insert(trace, TraceOpCode::gtrue, z, 0, 0,
                Type::Nil, Shape::Scalar, Shape::Empty);
        reenters[g] = (Reenter) { inst, true };
        shape = b;
    }
    else if(a.traceLength > b.traceLength) {
        IRRef x = insert(trace, TraceOpCode::le, b.length, a.length, 0,
                Type::Logical, Shape::Scalar, Shape::Scalar);
        IRRef y = insert(trace, TraceOpCode::gt, b.length, 0, 0,
                Type::Logical, Shape::Scalar, Shape::Scalar);
        IRRef z = insert(trace, TraceOpCode::land, x, y, 0, 
                Type::Logical, Shape::Scalar, Shape::Scalar);
        IRRef g = insert(trace, TraceOpCode::gtrue, z, 0, 0,
                Type::Nil, Shape::Scalar, Shape::Empty);
        reenters[g] = (Reenter) { inst, true };
        shape = a;
    }
    return shape;
}

JIT::IRRef JIT::EmitBinary(TraceOpCode::Enum op, IRRef a, IRRef b, Type::Enum rty, Type::Enum maty, Type::Enum mbty, Instruction const* inst) {
    // specialization depends on observed lengths. 
    //  If depedent length is the same, no need for a guard. We've already proved the lengths are equal
    //  If one of the lengths is zero, result length is also known, no need for guard.
    //  If equal, guard equality and continue.
    //  If unequal, guard less than
    Shape shape = MergeShapes(trace[a].out,trace[b].out, inst);
    return insert(trace, op, rep(cast(a,maty),shape), rep(cast(b,mbty),shape), 0, rty, shape, shape);
}

JIT::IRRef JIT::EmitTernary(TraceOpCode::Enum op, IRRef a, IRRef b, IRRef c, Type::Enum rty, Type::Enum maty, Type::Enum mbty, Type::Enum mcty, Instruction const* inst) {
    Shape s = MergeShapes(trace[a].out, MergeShapes(trace[b].out, trace[c].out, inst), inst);
    return insert(trace, op, rep(cast(a,maty),s), rep(cast(b,mbty),s), rep(cast(c,mcty),s), rty, s, s);
}

JIT::IRRef JIT::constant(Value const& value) {
    trace.push_back(makeConstant(value));
    return trace.size()-1;
}

bool JIT::EmitNest(Thread& thread, Trace* t) {
    insert(trace, TraceOpCode::nest, (int64_t)t, 0, 0, Type::Nil, Shape::Empty, Shape::Empty);
    return true;
}

bool JIT::EmitIR(Thread& thread, Instruction const& inst, bool branch) {
    switch(inst.bc) {

        case ByteCode::loop: {
        } break;
        case ByteCode::jc: {
            IRRef p = load(thread, inst.c, &inst);
            if(inst.c <= 0) {
                Variable v = { -1, (thread.base+inst.c)-(thread.registers+DEFAULT_NUM_REGISTERS)};
                insert(trace, TraceOpCode::kill, v.i, 0, 0, Type::Nil, Shape::Empty, Shape::Empty);
            } 
            IRRef r = insert(trace, branch ? TraceOpCode::gtrue : TraceOpCode::gfalse, 
                p, 0, 0, Type::Nil, trace[p].out, Shape::Empty );
            reenters[r] = (Reenter) { &inst + (branch ? inst.b : inst.a), (inst.a>=0&&inst.b>0) };
        }   break;
    
        case ByteCode::constant: {
            Value const& c = thread.frame.prototype->constants[inst.a];
            store(thread, constant(c), inst.c);
        }   break;

        case ByteCode::mov:
        case ByteCode::fastmov: {
            store(thread, load(thread, inst.a, &inst), inst.c);
        }   break;

        case ByteCode::assign: {
            store(thread, load(thread, inst.c, &inst), inst.a);
        }   break;

        case ByteCode::gather1: {
        case ByteCode::gather:
            IRRef a = load(thread, inst.a, &inst);
            IRRef b = cast(load(thread, inst.b, &inst), Type::Integer);
            b = insert(trace, TraceOpCode::sub, b, rep(constant(Integer::c(1)), trace[b].out), 0, trace[b].type, trace[b].out, trace[b].out);
            store(thread, insert(trace, TraceOpCode::gather, a, b, 0, trace[a].type, trace[b].out, trace[b].out), inst.c);
        }   break;

        case ByteCode::scatter1: {
        case ByteCode::scatter:
            IRRef a = load(thread, inst.a, &inst);
            IRRef b = cast(load(thread, inst.b, &inst), Type::Integer);
            b = insert(trace, TraceOpCode::sub, b, rep(constant(Integer::c(1)), trace[b].out), 0, trace[b].type, trace[b].out, trace[b].out);
            IRRef c = load(thread, inst.c, &inst);
            Shape s = MergeShapes(trace[a].out, trace[b].out, &inst);
            store(thread, insert(trace, TraceOpCode::scatter, rep(a, s), rep(b, s), c, trace[c].type, s, trace[c].out), inst.c);
        }   break;

        case ByteCode::ifelse: {
            IRRef a = load(thread, inst.a, &inst);
            IRRef b = load(thread, inst.b, &inst);
            IRRef c = load(thread, inst.c, &inst);
            Shape s = MergeShapes(trace[a].out, MergeShapes(trace[b].out, trace[c].out, &inst), &inst);
            store(thread, EmitTernary<IfElse>(TraceOpCode::ifelse, rep(c,s), rep(b,s), rep(a,s), &inst), inst.c);
        }   break;

        #define EMIT(Name, string, Group, ...)                      \
        case ByteCode::Name: {                              \
            IRRef a = load(thread, inst.a, &inst);          \
            IRRef r = EmitUnary<Group>(TraceOpCode::Name, a);  \
            if(r != 0) store(thread, r, inst.c);  \
        }   break;
        UNARY_BYTECODES(EMIT)
        #undef EMIT

        #define EMIT(Name, string, Group, ...)                      \
        case ByteCode::Name: {                              \
            IRRef a = load(thread, inst.a, &inst);          \
            IRRef b = load(thread, inst.b, &inst);          \
            IRRef r = EmitBinary<Group>(TraceOpCode::Name, a, b, &inst); \
            if(r != 0) store(thread, r, inst.c);  \
        }   break;
        BINARY_BYTECODES(EMIT)
        #undef EMIT

        #define EMIT(Name, string, Group, ...)                      \
        case ByteCode::Name: {                              \
            IRRef a = load(thread, inst.a, &inst);          \
            IRRef r = EmitFold<Group>(TraceOpCode::Name, a);  \
            if(r != 0) store(thread, r, inst.c);  \
        }   break;
        FOLD_BYTECODES(EMIT)
        #undef EMIT
        
        case ByteCode::length:
        {
            IRRef a = load(thread, inst.a, &inst); 
            store(thread, insert(trace, TraceOpCode::length, a, 0, 0, Type::Integer, Shape::Scalar, Shape::Scalar), inst.c);
        }   break;

        case ByteCode::forend:
        {
            IRRef counter = load(thread, inst.c, &inst);
            IRRef vec = load(thread, inst.b, &inst);

            IRRef a = insert(trace, TraceOpCode::length, vec, 0, 0, Type::Integer, Shape::Scalar, Shape::Scalar);
            IRRef b = insert(trace, TraceOpCode::lt, counter, a, 0, Type::Logical, Shape::Scalar, Shape::Scalar);
            IRRef c = insert(trace, TraceOpCode::gtrue, b, 0, 0, Type::Nil, Shape::Scalar, Shape::Empty);
            reenters[c] = (Reenter) { &inst+2, false };
            store(thread, insert(trace, TraceOpCode::gather, vec, counter, 0, trace[vec].type, Shape::Scalar, Shape::Scalar), inst.a);
            store(thread, insert(trace, TraceOpCode::add, counter, constant(Integer::c(1)), 0, Type::Integer, Shape::Scalar, Shape::Scalar), inst.c); 
        }   break;

        case ByteCode::strip:
        {
            OPERAND(a, inst.a);
            if(a.isObject()) {
                Shape s = SpecializeValue(((Object const&)a).base(), IR(TraceOpCode::olength, load(thread, inst.a, &inst), Type::Integer, Shape::Empty, Shape::Scalar), &inst);
                IRRef g = insert(trace, TraceOpCode::load, load(thread, inst.a, &inst), 0, 0, ((Object const&)a).base().type, Shape::Scalar, s);
                reenters[g] = (Reenter) { &inst, true };
                store(thread, g, inst.c);
            }
            else {
                store(thread, load(thread, inst.a, &inst), inst.c);
            }
        }   break;

        case ByteCode::nargs:
        {
            store(thread, constant(Integer::c(thread.frame.environment->call.length-1)), inst.c);
        }   break;

        case ByteCode::attrget:
        {
            OPERAND(object, inst.a);
            OPERAND(whichTmp, inst.b);
            
            if(object.isObject()) {
                Value r;
                Character which = As<Character>(thread, whichTmp);
                r = ((Object const&)object).get(which[0]);
            
                IRRef name = cast(load(thread, inst.b, &inst), Type::Character);

                Shape s = SpecializeValue(r, IR(TraceOpCode::alength, load(thread, inst.a, &inst), name, Type::Integer, Shape::Empty, Shape::Scalar), &inst);
                
                IRRef g = insert(trace, TraceOpCode::load, load(thread, inst.a, &inst), name, 0, r.type, Shape::Empty, s);
                reenters[g] = (Reenter) { &inst, true };
                store(thread, g, inst.c);
            }
            else {
                store(thread, constant(Null::Singleton()), inst.c);
            }
        }   break;

        case ByteCode::attrset:
        {
            // need to make this an object if it's not already
            store(thread, insert(trace, TraceOpCode::store,
                load(thread, inst.c, &inst),
                load(thread, inst.b, &inst),
                load(thread, inst.a, &inst),
                Type::Object, Shape::Scalar, Shape::Empty), inst.c); 
        }   break;

        case ByteCode::missing:
        {
            String s = (String)inst.a;
            Value const& v = thread.frame.environment->get(s);
            bool missing = v.isNil() || v.isDefault();
            store(thread, constant(Logical::c(missing ? Logical::TrueElement : Logical::FalseElement)), inst.c);
        }   break;

        case ByteCode::rep:
        {
            OPERAND(len, inst.a);
            IRRef l = load(thread, inst.a, &inst);
            Shape s = SpecializeLength(As<Integer>(thread, len)[0], l, &inst);
            // requires a dependent type
            store(thread, insert(trace, TraceOpCode::rep,
                cast(load(thread, inst.a, &inst), Type::Integer), 
                cast(load(thread, inst.b, &inst), Type::Integer), 0,
                Type::Integer, s, s), inst.c);
        }   break;
        case ByteCode::seq:
        {
            OPERAND(len, inst.a);
            IRRef l = cast(load(thread, inst.a, &inst), Type::Integer);
            Shape s = SpecializeLength(As<Integer>(thread, len)[0], l, &inst);
            // requires a dependent type
            IRRef c = load(thread, inst.c, &inst);
            IRRef b = load(thread, inst.b, &inst);
            Type::Enum type = trace[c].type == Type::Double || trace[b].type == Type::Double
                ? Type::Double : Type::Integer; 
            store(thread, insert(trace, TraceOpCode::seq,
                cast(c, type), cast(b, type), 0,
                type, s, s), inst.c);
        }   break;

        case ByteCode::call:
        case ByteCode::ncall:
            // nothing since it's currently
            break;

        case ByteCode::newenv:
            store(thread, insert(trace, TraceOpCode::newenv, 
                    load(thread, inst.a, &inst),
                    load(thread, inst.a, &inst),
                    constant(Null::Singleton()), Type::Environment, Shape::Scalar, Shape::Scalar), inst.c);
            break;
        case ByteCode::parentframe:
            {
                IRRef e = insert(trace, TraceOpCode::curenv, 0, 0, 0, Type::Environment, Shape::Empty, Shape::Scalar);
                store(thread, insert(trace, TraceOpCode::denv, e, 0, 0, Type::Environment, Shape::Scalar, Shape::Scalar), inst.c);
            } break;

        default: {
            if(thread.state.verbose)
                printf("Trace halted by %s\n", ByteCode::toString(inst.bc));
            return false;
        }   break;
    }
    return true;
}

JIT::IRRef JIT::duplicate(IR const& ir, std::vector<IRRef> const& forward) {
    return insert(code, ir.op, forward[ir.a], forward[ir.b], forward[ir.c], ir.type, ir.in, ir.out);
}

JIT::Exit JIT::BuildExit( std::vector<IRRef>& environments, std::vector<StackFrame>& frames,
        std::map<Variable, IRRef>& stores, Reenter const& reenter, size_t index) {

    // OK, attempt to do tracing something here...
    
    // get live environments
    std::vector<IRRef> live;
    for(size_t i = 0; i < frames.size(); i++) {
        live.push_back(frames[i].environment);
        live.push_back(frames[i].env);
    }
    //for(size_t i = 0; i < environments.size(); i++) {
    //    if(code[environments[i]].op == TraceOpCode::LOADENV)
    //        live.push_back(environments[i]);
    //}
    
    // get live stores (those that are into a live environment)
    // this is very inefficient, replace
    std::map<Variable, IRRef> livestores;
    for(std::map<Variable, IRRef>::const_iterator i = stores.begin(); i != stores.end(); ++i) {
        if(i->first.env == -1 || code[i->first.env].op == TraceOpCode::curenv)
            livestores.insert(*i);
        else {
            for(size_t j = 0; j < live.size(); j++) {
                if(live[j] == i->first.env) {
                    livestores.insert(*i);
                    break;
                }
            }
        }
    }

    Exit e = { live, frames, livestores, reenter, index };
    return e;
}

void JIT::Replay(Thread& thread) {
   
    code.clear();
    exits.clear();
 
    size_t n = trace.size();
    
    std::vector<IRRef> forward(n, 0);
    std::map<Variable, IRRef> loads;
    std::map<Variable, IRRef> stores, stores_old;
    std::tr1::unordered_map<IR, IRRef> cse;
    std::vector<IRRef> environments;
    std::vector<StackFrame> frames;
    std::map<Variable, Phi> phis;

    // after each guard reemit the entire body of the code
    // up to that point, omitting all guards.
    // conceptually this gives us all possible sinking locations
    // how do we keep the size from getting out of control?
        // eliminate as many guards as possible
    // avoid the order N^2?
    /*
        replay just the sunk operations.
        means I'd need to figure out what to sink after emiting guard
        not all stores can be sunk.
            
        I can compute the CSE cost when emiting the first set.
        If forwarding is not profitable, put in 

        what can be sunk?
            not any operations needed to evaluate the guard condition
            not any loads or constants
            

        want to do cost-driven CSE, is this all I need?
        

          load a
        0 seq i 100         // if I use this one it means that all branches share this instance.
          store a
        1 lt i 50
        2 gtrue   ->
        
          load a
        3 seq i 100         // if I use this one it means the original is either dead or only computed on the side exit
          store a
        4 lt i 50     (xx CSEd or DCE)
        5 sum 0
          store b
        6 lt 100
        7 gtrue   ->
        
        What if I just replayed the stores?
        All stores before guard get marked as sunk
        Do DSE on reemitted stores.

        Want to eliminate stores on the fast path
        >store a
        guard => store is needed here
        store a -> this store makes the previous one dead and is not needed?
        phi
        jmp

        What stores can be sunk?
        
        store global "a"   => this can because it aliases itself in all previous iterations
                           => DSE applied to loop carried global
        guard 

        compute key
        newenv blah
        store blah computed key
        >store global "a" blah
        guard              => this can because last store dominates, now there's no use of blah
                              in main path.
        
        What stores can't be sunk?
        loop
        compute key
        store global computedkey  => this can't because it only may alias
                                  => previous stores aren't dead.
        guard
        jmp

        store global a foo   => this can be sunk since it is dominated by the following store
        guard
        store global a bar

        DSE:
            if we haven't crossed a guard, the store becomes a NOP
            if we have crossed a guard, the store becomes a SUNK store

            >store global a foo
            guard
            loop
            ...
            >store global a bar
            guard
            jmp

            
            loop
            >newenv blah
            >store blah bar
            guard
            jmp
    */
   
    // Emit constants
    for(size_t i = 0; i < n; i++) {
        if(trace[i].op == TraceOpCode::constant)
            EmitOptIR(thread, i, trace[i], code, forward, loads, stores, cse, environments, frames, phis);
    }
 
    // Emit loop header...
    for(size_t i = 0; i < n; i++) {
        EmitOptIR(thread, i, trace[i], code, forward, loads, stores, cse, environments, frames, phis);
    }

    if(rootTrace == 0) 
    {
        Loop = Insert(thread, code, cse, IR(TraceOpCode::loop, Type::Nil, Shape::Empty, Shape::Empty));

        loads.clear();

        // Emit loop
        for(size_t i = 0; i < n; i++) {
            EmitOptIR(thread, i, trace[i], code, forward, loads, stores, cse, environments, frames, phis);
        }

        // Emit PHIs
        for(std::map<Variable, Phi>::const_iterator i = phis.begin(); i != phis.end(); ++i) {
            IR const& ir = code[i->second.a];
            Insert(thread, code, cse, IR(TraceOpCode::phi, i->second.a, i->second.b, ir.type, ir.out, ir.out));
        }

        // Emit the JMP
        IRRef jmp = Insert(thread, code, cse, IR(TraceOpCode::jmp, Type::Nil, Shape::Empty, Shape::Empty));
    }
    else {
        IRRef e = Insert(thread, code, cse, IR(TraceOpCode::exit, Type::Nil, Shape::Empty, Shape::Empty));
        Reenter r = { startPC, true };
        exits[code.size()-1] = BuildExit( environments, frames, stores, r, exits.size()-1 );
    }
}

void JIT::end_recording(Thread& thread) {

    assert(state == RECORDING);
    state = OFF;

    //dump(thread, trace);
    Replay(thread);
    //dump(thread, code);
    //Schedule();
    schedule();
    Exit tmp;
    RegisterAssignment(tmp);
    if(thread.state.verbose)
        dump(thread, code);

    for(std::map<size_t, Exit>::const_iterator i = exits.begin(); i != exits.end(); ++i) {
        Trace tr;
        tr.Reenter = i->second.reenter.reenter;
        tr.InScope = i->second.reenter.inScope;
        tr.counter = 0;
        tr.ptr = 0;
        tr.function = 0;
        tr.root = dest->root;
        assert(i->second.index == dest->exits.size());
        dest->exits.push_back(tr);
    }

    // add the tail exit for side traces
    if(rootTrace) {
        dest->exits.back().function = rootTrace->function;
    }

    compile(thread);
}

void JIT::specialize() {
    // basically, we want to score how valuable a particular specialization
    // (replacing a load with a constant) might be.
    // Only worth doing on loads in the loop header.
    // Valuable things:
    //  1) Eliminating a guard to enable fusion.
    //  2) Turn unvectorized op into a vectorized op
    //      a) Lowering gather to shuffle
    //      b) Lowering pow to vectorized mul or sqrt
    //  3) Making a size constant (e.g. out of a filter)
    // 
    //  Might be target specific
    //
    // Valuable is a tradeoff between reuse and benefit.
    //  How to judge?
    //  Not valuable for very long vectors or scalars.
    //  Valuable for small multiples of HW vector length,
    //      where we can unroll the loop completely.
    //  Unless the entire vector is a constant
/*
    size_t n = code.size();
    std::vector<IR> out;
    std::vector<IRRef> forward(n, -1);
    std::vector<size_t> ngroup;
    std::map<size_t, Exit> nexits;
    std::map<Variable, IRRef> loads;
    std::map<Variable, IRRef> stores;
    std::tr1::unordered_map<IR, IRRef> cse;

    for(int g = maxGroup; g >= 0; g--) {
        for(size_t i = 0; i < n; i++) {
            if(group[i] == g) {
                EmitOptIR(i, code[i], out, forward, loads, stores, cse);
                ngroup.push_back(group[i]);
                if(exits.find(i) != exits.end()) {
                    // add compensation code
                    Exit e = exits[i];
                    e.compensation.clear();
                    printf("Exit initial %d\n", i);
                    for(int k = 0; k < forward[i]; k++)
                        e.compensation.push_back(IR(TraceOpCode::nop, Type::Promise, Shape::Empty, Shape::Empty));
                    std::vector<IRRef> eforward = forward;
                    std::map<Variable, IRRef> loads = loads;
                    std::map<Variable, IRRef> estores = stores;
                    std::tr1::unordered_map<IR, IRRef> ecse = cse;
                    for(size_t k = 0; k < i; k++) {
                        if(forward[k] == -1) {
                            printf("Compensation: %d\n", k);
                            EmitOptIR(k, code[k], e.compensation, eforward, loads, estores, ecse);
                        }
                    }
                    for(std::map<Variable, IRRef>::iterator k = e.o.begin(); k != e.o.end(); ++k) {
                        k->second = eforward[k->second];
                    }
                    nexits[out.size()-1] = e;  
                }           
            }
        }
    }

    // iterate through the exits. If the code was before the exit before and is now after,
    // add to the compensation list.
    for(std::map<size_t, Exit>::const_iterator j = exits.begin(); j != exits.end(); ++j) {
        for(size_t i = 0; i < n; i++) {
            if(i < j->first && forward[i] > forward[j->first]) {
                
            }
        }
    }

 
    code = out;
    group = ngroup;
    exits = nexits;
*/
}

bool JIT::Ready(JIT::IR ir, std::vector<bool>& done) {
        switch(ir.op) {
            #define CASE(Name, ...) case TraceOpCode::Name:
            case TraceOpCode::sload:
            case TraceOpCode::curenv:
            case TraceOpCode::phi:
            case TraceOpCode::loop: 
            case TraceOpCode::constant: 
                return true;
                break;
            UNARY_BYTECODES(CASE) 
            FOLD_BYTECODES(CASE) 
            case TraceOpCode::gproto: 
            case TraceOpCode::gtrue: 
            case TraceOpCode::gfalse:
            case TraceOpCode::load: {
                return done[ir.a];
            } break; 
            BINARY_BYTECODES(CASE)
            case TraceOpCode::gather:
            case TraceOpCode::rep: {
                return done[ir.a] && done[ir.b];
            } break;
            TERNARY_BYTECODES(CASE)
            case TraceOpCode::scatter:
                return done[ir.a] && done[ir.b] && done[ir.c];
            break;
            case TraceOpCode::jmp:
                return false;
            break;
            default:
                printf("Unknown op is %s\n", TraceOpCode::toString(ir.op));
                _error("Unknown op in Ready");
                break;
            #undef CASE
        }
}

size_t Score(JIT::IR ir) {
    return ir.in.length;
}

void JIT::Schedule() {
   
    // Scheduling...want to move move unused ops down into side traces...
    // Linear scheduling doesn't do this aggressively enough.
/* 
    size_t n = code.size();
    std::vector<IR> out;
    std::vector<IRRef> forward(n, -1);
    std::map<size_t, Exit> nexits;
    std::map<Variable, IRRef> loads;
    std::map<Variable, IRRef> stores;
    std::tr1::unordered_map<IR, IRRef> cse;

    // find everything that has no dependencies...
    std::vector<bool> ready(n, false);
    std::vector<bool> done(n, false);

    IRRef best = 0;
    size_t score = 1000000000000;
    do {
        // Update the ready list
        for(IRRef i = best; i != Loop; i++) {
            if(!ready[i]) {
                ready[i] = Ready(code[i], done);
            }
        }
        // Select the best instruction and put it in the done list
        score = 1000000000000;
        for(IRRef i = 0; i != Loop; i++) {
            if(ready[i] && !done[i]) {
                size_t s = Score(code[i]);
                if(s < score) {
                    score = s;
                    best = i;
                }
            }
        }

        if(score != 1000000000000) {
            EmitOptIR(best, code[best], out, forward, loads, stores, cse);
            done[best] = true;
        }
    } while(score != 1000000000000);

    // emit the loop instruction and phis
    Insert(out, cse, IR(TraceOpCode::loop, Type::Promise, Shape::Empty, Shape::Empty));

    best = Loop+1;
    //for(; code[best].op == TraceOpCode::phi; best++) {
    //    EmitOptIR(best, code[best], out, forward, loads, stores, cse);
    //    done[best] = ready[best] = true;
    //}

    do {
        // Update the ready list
        for(IRRef i = best; i != n; i++) {
            if(!ready[i]) {
                ready[i] = Ready(code[i], done);
            }
        }
        // Select the best instruction and put it in the done list
        score = 1000000000000;
        for(IRRef i = Loop+1; i != n; i++) {
            if(ready[i] && !done[i]) {
                size_t s = Score(code[i]);
                if(s < score) {
                    score = s;
                    best = i;
                }
            }
        }

        if(score != 1000000000000) {
            EmitOptIR(best, code[best], out, forward, loads, stores, cse);
            done[best] = true;
        }
    } while(score != 1000000000000);

    // update the exits with the new instruction locations...
    for(std::map<size_t, Exit>::const_iterator i = exits.begin(); i != exits.end(); ++i) {
        Exit e = i->second;
        for(std::map<Variable, IRRef>::iterator j = e.o.begin(); j != e.o.end(); ++j) {
            j->second = forward[j->second];
        }
        nexits[forward[i->first]] = e;
    }

    // Emit the JMP
    Insert(out, cse, IR(TraceOpCode::jmp, Type::Promise, Shape::Empty, Shape::Empty));
    code = out; 
    exits = nexits;
    */
}


void JIT::schedule() {

    // do a forwards pass identifying fusion groups.
    Shape gSize(-1, -1);
    std::set<IRRef> gMembers;

    fusable = std::vector<bool>(code.size(), true);

    for(IRRef i = 0; i < code.size(); i++) {
         if( code[i].in != gSize
          || (code[i].op == TraceOpCode::scatter && gMembers.find(code[i].c) != gMembers.end())
          || (code[i].op == TraceOpCode::gather  && gMembers.find(code[i].c) != gMembers.end())
          || code[i].op == TraceOpCode::gtrue 
          || code[i].op == TraceOpCode::gfalse 
          || code[i].op == TraceOpCode::load 
          || code[i].op == TraceOpCode::sload ) {
            fusable[i] = false;
            gSize = code[i].in;
            gMembers.clear();
        }
        gMembers.insert(i);
        if(code[i].op == TraceOpCode::gather)
            gMembers.insert(code[i].b);
        if(code[i].op == TraceOpCode::scatter)
            gMembers.insert(code[i].c);
    }

    // do a backwards pass, assigning instructions to a fusion group.
    // this happens after all optimization and specialization decisions
    //  have been made.

    // Problem: Gathers and scatters to same vector can't be fused. How to assert?
    // Unless in different registers and a whole copy occurs.

    /*

        fusion and register assignment interact
        
        True dependency. Write has to complete before read starts.
            0: SCATTER a
            1: GATHER a
        Can't fuse generally

        Anti-dependency: Read comes before write.
            0: GATHER a
            1: SCATTER a
        Can fuse if scatter writes to distinct register, requires copy of a on loop backedge.
        Alternatively: not fuse, intermediate (size) must be written out and read back in.

        Store-store dependency: Write after write
            0: SCATTER a
            1: SCATTER b
        Can't fuse generally, unless scatter kills entire thing

        GATHER-GATHER is fine.
    */

    /* replace with forward reordering */
    
}

void JIT::IR::dump() const {
    if(type != Type::Nil)
        printf("  %.3s  ", Type::toString(type));
    else
        printf("       ");
    
    std::cout << in.length << "->" << out.length;
    std::cout << "\t" << TraceOpCode::toString(op);

    switch(op) {
        #define CASE(Name, ...) case TraceOpCode::Name:
        case TraceOpCode::loop: {
            std::cout << " --------------------------------";
        } break;
        case TraceOpCode::sload:
        case TraceOpCode::slength: {
            std::cout << "\t " << (int64_t)b;
        } break;
        case TraceOpCode::sstore: {
            std::cout << "\t " << (int64_t)b << "\t " << c;
        } break;    
        case TraceOpCode::gproto:
        {
            std::cout << "\t " << a << "\t [" << b << "]";
        } break;
        case TraceOpCode::kill:
            std::cout << "\t " << (int64_t)a;
            break;
        case TraceOpCode::repscalar:
        case TraceOpCode::push:
        case TraceOpCode::length:
        case TraceOpCode::gtrue:
        case TraceOpCode::gfalse: 
        case TraceOpCode::olength: 
        case TraceOpCode::lenv: 
        case TraceOpCode::denv: 
        case TraceOpCode::cenv: 
        UNARY_FOLD_SCAN_BYTECODES(CASE)
        {
            std::cout << "\t " << a;
        } break;
        case TraceOpCode::phi: 
        case TraceOpCode::load:
        case TraceOpCode::elength:
        case TraceOpCode::rep:
        case TraceOpCode::seq:
        case TraceOpCode::gather:
        case TraceOpCode::alength:
        BINARY_BYTECODES(CASE)
        {
            std::cout << "\t " << a << "\t " << b;
        } break;
        case TraceOpCode::newenv:
        case TraceOpCode::store:
        case TraceOpCode::scatter:
        TERNARY_BYTECODES(CASE)
        {
            std::cout << "\t " << a << "\t " << b << "\t " << c;
        } break;
        default: {} break;

        #undef CASE
    };
}

void JIT::dump(Thread& thread, std::vector<IR> const& t) {

    for(size_t i = 0; i < t.size(); i++) {
        IR const& ir = t[i];
        if(ir.live) {
            printf("%4li ", i);
            if(exits.find(i) != exits.end())
                printf(">");
            else if(fusable.size() == t.size() && !fusable[i]) 
                printf("-");
            else
                printf(" ");
            if(ir.reg > 0) 
                printf(" %2d ", ir.reg);
            else if(ir.reg < 0)
                printf(" !! ");
            else
                printf("    ");
            ir.dump();
    
            if(ir.op == TraceOpCode::constant) {
                std::cout <<  "    " << thread.deparse(constants[ir.a]);
            }
            std::cout << std::endl;
        }
    }
    std::cout << std::endl;
}
