"""Lower the versioned scoring algorithms to C; Python is only a build tool.

The generated executable links a small C value library, not libpython. Keeping
the lowering mechanical makes the many scoring policy branches reviewable and
lets differential tests identify translation errors without changing the metric.
"""
import ast
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def quote(s):
    parts=[]
    for c in s.encode("utf-8"):
        parts.append('\\"' if c==34 else '\\\\' if c==92 else chr(c) if 32<=c<127 else f"\\{c:03o}")
    return '"'+''.join(parts)+'"'


class Compiler:
    def __init__(self):
        self.functions = []
        self.serial = 0
        self.lines = []
        self.env = "e"
        self.nonlocal_names = set()
        self.loops = []
        self.fastlocals = {}
        self.comp_names = set()
        self.comp_slots = {}

    def uid(self, prefix="v"):
        self.serial += 1
        return f"{prefix}{self.serial}"

    def emit(self, s):
        self.lines.append(s)

    def value(self, code):
        name = self.uid()
        self.emit(f"V {name}={code};")
        return name

    def get(self, name):
        if name in self.comp_slots:return self.value(self.comp_slots[name])
        if name in self.fastlocals and name not in self.comp_names:
            return self.value(f"e->locals[{self.fastlocals[name]}]")
        return self.value(f"get({self.env},{quote(name)})")

    def put(self, name, val):
        if name in self.comp_slots:
            self.emit(f"{self.comp_slots[name]}={val};")
            return
        if name in self.fastlocals and name not in self.comp_names:
            self.emit(f"e->locals[{self.fastlocals[name]}]={val};")
            return
        fn = "nonlocal_set" if name in self.nonlocal_names else "put"
        self.emit(f"{fn}({self.env},{quote(name)},{val});")

    def function(self, node, name=None):
        fn = self.uid("fn")
        old = self.lines, self.env, self.nonlocal_names, self.loops, self.fastlocals, self.comp_names, self.comp_slots
        self.lines, self.env, self.nonlocal_names, self.loops = [], "e", set(), []
        def nonlocals(x):
            if isinstance(x, ast.Nonlocal): return set(x.names)
            if isinstance(x,(ast.Lambda,ast.FunctionDef,ast.ClassDef)) and x is not node:return set()
            return set().union(*(nonlocals(c) for c in ast.iter_child_nodes(x)))
        self.nonlocal_names = nonlocals(node)
        a=node.args
        positional=[x.arg for x in a.posonlyargs+a.args]
        keyword=[x.arg for x in a.kwonlyargs]
        local_names=positional+keyword+([a.vararg.arg] if a.vararg else [])+([a.kwarg.arg] if a.kwarg else [])
        assigned=set()
        def locals_of(x):
            if isinstance(x,(ast.FunctionDef,ast.ClassDef)) and x is not node:
                assigned.add(x.name);return
            if isinstance(x,ast.Lambda) and x is not node:return
            if isinstance(x,(ast.ListComp,ast.SetComp,ast.DictComp,ast.GeneratorExp)):
                for child in ast.walk(x):
                    if isinstance(child,ast.NamedExpr):assigned.add(child.target.id)
                return
            if isinstance(x,ast.Name) and isinstance(x.ctx,ast.Store):assigned.add(x.id)
            if isinstance(x,ast.Import):assigned.update(v.asname or v.name for v in x.names)
            if isinstance(x,ast.ImportFrom):assigned.update(v.asname or v.name for v in x.names)
            for child in ast.iter_child_nodes(x):locals_of(child)
        locals_of(node)
        local_names+=sorted(assigned-set(local_names)-self.nonlocal_names)
        self.fastlocals={n:i for i,n in enumerate(local_names)}
        self.comp_names=set()
        self.comp_slots={}
        if isinstance(node, ast.Lambda):
            v = self.expr(node.body)
            self.emit(f"return {v};")
        else:
            self.block(node.body)
        self.emit("return NONE;")
        body = "\n".join(self.lines)
        self.lines, self.env, self.nonlocal_names, self.loops, self.fastlocals, self.comp_names, self.comp_slots = old
        self.functions.append((fn, body))
        names = self.value("list_new()")
        slots = self.value("list_new()")
        for arg in local_names:self.emit(f"append({slots},str({quote(arg)}));")
        defaults = self.value("map_new()")
        for arg in positional + keyword:
            self.emit(f"append({names},str({quote(arg)}));")
        for arg, value in zip(positional[len(positional)-len(a.defaults):], a.defaults):
            v = self.expr(value)
            self.emit(f"map_set({defaults},str({quote(arg)}),{v});")
        for arg, value in zip(keyword, a.kw_defaults):
            if value is not None:
                v = self.expr(value)
                self.emit(f"map_set({defaults},str({quote(arg)}),{v});")
        result=self.value(f"function_new({fn},{self.env},{names},{defaults},{len(positional)},{quote(a.vararg.arg) if a.vararg else 'NULL'},{quote(a.kwarg.arg) if a.kwarg else 'NULL'},{quote(name or '<lambda>')},{slots})")
        for decorator in getattr(node,'decorator_list',[]):
            if isinstance(decorator,ast.Call) and isinstance(decorator.func,ast.Name) and decorator.func.id=='lru_cache':
                limit=next((v.value for v in decorator.keywords if v.arg=='maxsize'),ast.Constant(128))
                size=self.expr(limit)
                self.emit(f'obj({result})->flags|=FN_CACHE;obj({result})->cap=(size_t)asint({size});')
        return result

    def expr(self, x):
        if isinstance(x, ast.Constant):
            if x.value is None: return "NONE"
            if x.value is True: return "TRUEV"
            if x.value is False: return "FALSEV"
            if isinstance(x.value, str): return self.value(f"constant_string({quote(x.value)},{len(x.value.encode('utf-8'))})")
            if isinstance(x.value, int): return self.value(f"integer({x.value}LL)")
            if isinstance(x.value, float): return self.value(f"constant_float({x.value!r})")
            if x.value is Ellipsis: return "NONE"
            raise ValueError(ast.dump(x))
        if isinstance(x, ast.Name): return self.get(x.id)
        if isinstance(x, ast.Attribute):
            obj = self.expr(x.value)
            return self.value(f"attr({obj},{quote(x.attr)})")
        if isinstance(x, ast.Subscript):
            obj, key = self.expr(x.value), self.expr(x.slice)
            return self.value(f"item({obj},{key})")
        if isinstance(x, ast.Slice):
            lo, hi, step = (self.expr(v) if v is not None else "NONE" for v in (x.lower, x.upper, x.step))
            return self.value(f"slice_new({lo},{hi},{step})")
        if isinstance(x, ast.NamedExpr):
            v = self.expr(x.value); self.assign(x.target, v); return v
        if isinstance(x, ast.BinOp):
            a, b = self.expr(x.left), self.expr(x.right)
            return self.value(f"binary(OP_{type(x.op).__name__.upper()},{a},{b})")
        if isinstance(x, ast.UnaryOp):
            v = self.expr(x.operand)
            return self.value(f"unary(OP_{type(x.op).__name__.upper()},{v})")
        if isinstance(x, ast.BoolOp):
            v = self.expr(x.values[0]); out = self.value(v)
            end = self.uid("end")
            for child in x.values[1:]:
                condition = f"!truth({out})" if isinstance(x.op, ast.And) else f"truth({out})"
                self.emit(f"if({condition})goto {end};")
                v = self.expr(child); self.emit(f"{out}={v};")
            self.emit(f"{end}:;")
            return out
        if isinstance(x, ast.Compare):
            a = self.expr(x.left); result = self.value("TRUEV"); end = self.uid("end")
            for op, child in zip(x.ops, x.comparators):
                b = self.expr(child)
                self.emit(f"{result}=compare(OP_{type(op).__name__.upper()},{a},{b});if(!truth({result}))goto {end};")
                a = b
            self.emit(f"{end}:;"); return result
        if isinstance(x, ast.IfExp):
            test = self.expr(x.test); out = self.value("NONE")
            self.emit(f"if(truth({test})){{"); a = self.expr(x.body); self.emit(f"{out}={a};}}else{{")
            b = self.expr(x.orelse); self.emit(f"{out}={b};}}")
            return out
        if isinstance(x, ast.Call):
            if getattr(self,'module_name',None)=='formatting_v2' and isinstance(x.func,ast.Attribute) and x.func.attr=='has':
                token,kind=self.expr(x.func.value),self.expr(x.args[0]);return self.value(f'format_has({token},{kind})')
            if isinstance(x.func,ast.Name) and not x.keywords and len(x.args)==1 and x.func.id in ('len','bool'):
                value=self.expr(x.args[0])
                return self.value(f'integer(length({value}))' if x.func.id=='len' else f'(truth({value})?TRUEV:FALSEV)')
            if isinstance(x.func,ast.Name) and x.func.id in ('any','all','next') and x.args and isinstance(x.args[0],ast.GeneratorExp):
                return self.short_generator(x.args[0],x.func.id,x.args[1] if len(x.args)>1 else None)
            fn = self.expr(x.func)
            if not x.keywords and not any(isinstance(a,ast.Starred) for a in x.args):
                values=[self.expr(a) for a in x.args]
                if values:
                    args=self.uid('args');self.emit(f"V {args}[]={{"+','.join(values)+'};')
                else:args='NULL'
                return self.value(f"call_pos({fn},{len(values)},{args})")
            args = self.value("list_new()"); kw = self.value("map_new()")
            for a in x.args:
                if isinstance(a, ast.Starred):
                    v = self.expr(a.value); self.emit(f"extend({args},{v});")
                else:
                    v = self.expr(a); self.emit(f"append({args},{v});")
            for k in x.keywords:
                v = self.expr(k.value)
                if k.arg is None: self.emit(f"map_update({kw},{v});")
                else: self.emit(f"map_set({kw},str({quote(k.arg)}),{v});")
            return self.value(f"call({fn},{args},{kw})")
        if isinstance(x, (ast.List, ast.Tuple, ast.Set)):
            v = self.value("list_new()")
            for a in x.elts:
                if isinstance(a, ast.Starred):
                    y = self.expr(a.value); self.emit(f"extend({v},{y});")
                else:
                    y = self.expr(a); self.emit(f"append({v},{y});")
            if isinstance(x, ast.Tuple): self.emit(f"obj({v})->type=T_TUPLE;")
            if isinstance(x, ast.Set): return self.value(f"set_from({v})")
            return v
        if isinstance(x, ast.Dict):
            v = self.value("map_new()")
            for k, val in zip(x.keys, x.values):
                if k is None:
                    y = self.expr(val); self.emit(f"map_update({v},{y});")
                else:
                    key, y = self.expr(k), self.expr(val); self.emit(f"map_set({v},{key},{y});")
            return v
        if isinstance(x, ast.Lambda): return self.function(x)
        if isinstance(x, (ast.ListComp, ast.GeneratorExp, ast.SetComp, ast.DictComp)):
            return self.comprehension(x)
        if isinstance(x, ast.JoinedStr):
            out = self.value("list_new()")
            for part in x.values:
                if isinstance(part, ast.FormattedValue):
                    v = self.expr(part.value)
                    fmt = self.expr(part.format_spec) if part.format_spec else self.value('str("")')
                    v = self.value(f"format_value({v},{fmt},{part.conversion})")
                else: v = self.expr(part)
                self.emit(f"append({out},{v});")
            return self.value(f"join(str(\"\"),{out})")
        raise ValueError(f"Unsupported expression {ast.dump(x)}")

    def comprehension(self, x):
        if not any(isinstance(node,ast.Lambda) for node in ast.walk(x)):
            return self.fast_comprehension(x)
        out = self.value("map_new()" if isinstance(x, ast.DictComp) else "list_new()")
        old_env = self.env; old_names=self.comp_names.copy(); new_env = self.uid("comp")
        self.emit(f"Env *{new_env}=env_new({old_env});")
        def loop(i):
            g = x.generators[i]
            stream = self.expr(g.iter); values = self.value(f"iter_values({stream})")
            index = self.uid("idx")
            self.emit(f"for(size_t {index}=0;{index}<length({values});++{index}){{")
            self.env = new_env
            self.comp_names.update(n.id for n in ast.walk(g.target) if isinstance(n,ast.Name))
            self.assign(g.target, f"item({values},integer({index}))")
            for test in g.ifs:
                val = self.expr(test); self.emit(f"if(!truth({val}))continue;")
            if i+1 < len(x.generators): loop(i+1)
            elif isinstance(x, ast.DictComp):
                k, v = self.expr(x.key), self.expr(x.value); self.emit(f"map_set({out},{k},{v});")
            else:
                v = self.expr(x.elt); self.emit(f"append({out},{v});")
            self.emit("}")
        loop(0); self.env = old_env; self.comp_names=old_names
        return self.value(f"set_from({out})") if isinstance(x, ast.SetComp) else out

    def short_generator(self,x,mode,default):
        if not any(isinstance(node,ast.Lambda) for node in ast.walk(x)):
            return self.fast_comprehension(x,mode,default)
        out=self.value(self.expr(default) if default else 'TRUEV' if mode=='all' else 'FALSEV' if mode=='any' else 'NONE')
        end=self.uid('genend');old_env=self.env;old_names=self.comp_names.copy();new_env=self.uid('comp')
        self.emit(f'Env *{new_env}=env_new({old_env});')
        def loop(i):
            g=x.generators[i];stream=self.expr(g.iter);values=self.value(f'iter_values({stream})');index=self.uid('idx')
            self.emit(f'for(size_t {index}=0;{index}<length({values});++{index}){{')
            self.env=new_env;self.comp_names.update(n.id for n in ast.walk(g.target) if isinstance(n,ast.Name))
            self.assign(g.target,f'item({values},integer({index}))')
            for condition in g.ifs:
                value=self.expr(condition);self.emit(f'if(!truth({value}))continue;')
            if i+1<len(x.generators):loop(i+1)
            else:
                value=self.expr(x.elt)
                if mode=='next':self.emit(f'{out}={value};goto {end};')
                else:self.emit(f'if({"!" if mode=="all" else ""}truth({value})){{{out}={"FALSEV" if mode=="all" else "TRUEV"};goto {end};}}')
            self.emit('}')
        loop(0);self.emit(f'{end}:;');self.env=old_env;self.comp_names=old_names;return out

    def fast_comprehension(self,x,mode=None,default=None):
        if mode:
            out=self.value(self.expr(default) if default else 'TRUEV' if mode=='all' else 'FALSEV' if mode=='any' else 'NONE')
            end=self.uid('genend')
        else:out=self.value('map_new()' if isinstance(x,ast.DictComp) else 'list_new()')
        saved=self.comp_slots.copy()
        def body(i):
            g=x.generators[i]
            # The iterable is evaluated before this target shadows outer names.
            ranged=isinstance(g.iter,ast.Call) and isinstance(g.iter.func,ast.Name) and g.iter.func.id=='range'
            if ranged:
                args=[self.expr(a) for a in g.iter.args]
                lo='integer(0)' if len(args)==1 else args[0];hi=args[0] if len(args)==1 else args[1];step='integer(1)' if len(args)<3 else args[2]
                index=self.uid('ci');limit=self.uid('lim');stride=self.uid('step')
                self.emit(f'int64_t {limit}=asint({hi}),{stride}=asint({step});')
            else:
                values=self.value(f'iter_values({self.expr(g.iter)})');index=self.uid('ci')
            for name in sorted({n.id for n in ast.walk(g.target) if isinstance(n,ast.Name)}):
                slot=self.uid('cv');self.emit(f'V {slot}=UNBOUND;');self.comp_slots[name]=slot
            if ranged:
                self.emit(f'for(int64_t {index}=asint({lo});{stride}>0?{index}<{limit}:{index}>{limit};{index}+={stride}){{')
                self.assign(g.target,f'integer({index})')
            else:
                self.emit(f'for(size_t {index}=0;{index}<length({values});++{index}){{')
                self.assign(g.target,f'item({values},integer({index}))')
            for condition in g.ifs:
                v=self.expr(condition);self.emit(f'if(!truth({v}))continue;')
            if i+1<len(x.generators):body(i+1)
            elif mode:
                v=self.expr(x.elt)
                if mode=='next':self.emit(f'{out}={v};goto {end};')
                else:self.emit(f'if({"!" if mode=="all" else ""}truth({v})){{{out}={"FALSEV" if mode=="all" else "TRUEV"};goto {end};}}')
            elif isinstance(x,ast.DictComp):
                k,v=self.expr(x.key),self.expr(x.value);self.emit(f'map_set({out},{k},{v});')
            else:
                v=self.expr(x.elt);self.emit(f'append({out},{v});')
            self.emit('}')
        body(0);self.comp_slots=saved
        if mode:self.emit(f'{end}:;');return out
        return self.value(f'set_from({out})') if isinstance(x,ast.SetComp) else out

    def assign(self, target, val):
        if isinstance(target, ast.Name): self.put(target.id, val)
        elif isinstance(target, (ast.Tuple, ast.List)):
            star = next((i for i, a in enumerate(target.elts) if isinstance(a, ast.Starred)), None)
            for i, child in enumerate(target.elts):
                if isinstance(child, ast.Starred):
                    sub = self.value(f"slice_get({val},{i},(int64_t)length({val})-{len(target.elts)-i-1},1)")
                    self.assign(child.value, sub)
                else:
                    index = i if star is None or i < star else i-len(target.elts)
                    sub = self.value(f"item({val},integer({index}))"); self.assign(child, sub)
        elif isinstance(target, ast.Attribute):
            obj = self.expr(target.value); self.emit(f"attr_set({obj},{quote(target.attr)},{val});")
        elif isinstance(target, ast.Subscript):
            obj, key = self.expr(target.value), self.expr(target.slice); self.emit(f"item_set({obj},{key},{val});")
        else: raise ValueError(ast.dump(target))

    def block(self, nodes):
        for x in nodes: self.stmt(x)

    def stmt(self, x):
        if isinstance(x, ast.Expr):
            if not isinstance(x.value, ast.Constant): self.expr(x.value)
        elif isinstance(x, ast.Assign):
            v = self.expr(x.value)
            for t in x.targets: self.assign(t, v)
        elif isinstance(x, ast.AnnAssign):
            if x.value is not None: self.assign(x.target, self.expr(x.value))
        elif isinstance(x, ast.AugAssign):
            a, b = self.expr(x.target), self.expr(x.value)
            v = self.value(f"inplace(OP_{type(x.op).__name__.upper()},{a},{b})"); self.assign(x.target, v)
        elif isinstance(x, ast.If):
            v = self.expr(x.test); self.emit(f"if(truth({v})){{"); self.block(x.body)
            self.emit("}else{"); self.block(x.orelse); self.emit("}")
        elif isinstance(x, ast.For):
            if isinstance(x.iter,ast.Call) and isinstance(x.iter.func,ast.Name) and x.iter.func.id=='range':
                args=[self.expr(a) for a in x.iter.args]
                lo='integer(0)' if len(args)==1 else args[0];hi=args[0] if len(args)==1 else args[1];step='integer(1)' if len(args)<3 else args[2]
                index=self.uid('range');limit=self.uid('limit');stride=self.uid('stride');broken=self.uid('broken')
                self.emit(f'int {broken}=0;int64_t {limit}=asint({hi}),{stride}=asint({step});for(int64_t {index}=asint({lo});{stride}>0?{index}<{limit}:{index}>{limit};{index}+={stride}){{')
                self.assign(x.target,f'integer({index})');self.loops.append(broken);self.block(x.body);self.loops.pop();self.emit('}')
                if x.orelse:self.emit(f'if(!{broken}){{');self.block(x.orelse);self.emit('}')
                return
            values = self.value(f"iter_values({self.expr(x.iter)})"); idx = self.uid("idx"); broken = self.uid("broken")
            self.emit(f"int {broken}=0;for(size_t {idx}=0;{idx}<length({values});++{idx}){{")
            self.assign(x.target, f"item({values},integer({idx}))")
            self.loops.append(broken); self.block(x.body); self.loops.pop(); self.emit("}")
            if x.orelse: self.emit(f"if(!{broken}){{"); self.block(x.orelse); self.emit("}")
        elif isinstance(x, ast.While):
            broken = self.uid("broken"); self.emit(f"int {broken}=0;while(1){{")
            v = self.expr(x.test); self.emit(f"if(!truth({v}))break;")
            self.loops.append(broken); self.block(x.body); self.loops.pop(); self.emit("}")
            if x.orelse: self.emit(f"if(!{broken}){{"); self.block(x.orelse); self.emit("}")
        elif isinstance(x, ast.Break): self.emit(f"{self.loops[-1]}=1;break;")
        elif isinstance(x, ast.Continue): self.emit("continue;")
        elif isinstance(x, ast.Return): self.emit(f"return {self.expr(x.value) if x.value else 'NONE'};")
        elif isinstance(x, (ast.Pass, ast.Nonlocal, ast.Global)): pass
        elif isinstance(x, ast.FunctionDef):
            v = self.function(x, x.name); self.put(x.name, v)
        elif isinstance(x, ast.ClassDef):
            cl = self.value(f"class_new(str({quote(x.name)}),{self.env})")
            self.put(x.name, cl)
            old_env = self.env; old_fast=self.fastlocals;self.fastlocals={};self.env = self.uid("classenv")
            self.emit(f"Env *{self.env}=obj({cl})->env;")
            for child in x.body:
                if isinstance(child, ast.AnnAssign) and isinstance(child.target, ast.Name):
                    default = self.expr(child.value) if child.value is not None else "UNBOUND"
                    self.emit(f"class_field({cl},str({quote(child.target.id)}),{default});")
                elif isinstance(child, ast.FunctionDef):
                    value = self.function(child, child.name)
                    decorators = [ast.unparse(d) for d in child.decorator_list]
                    if "property" in decorators: self.emit(f"obj({value})->flags|=FN_PROPERTY;")
                    if "staticmethod" in decorators: self.emit(f"obj({value})->flags|=FN_STATIC;")
                    if "classmethod" in decorators: self.emit(f"obj({value})->flags|=FN_CLASS;")
                    self.put(child.name, value)
                else: self.stmt(child)
            self.env = old_env;self.fastlocals=old_fast
        elif isinstance(x, ast.Import):
            for a in x.names: self.put(a.asname or a.name, self.value(f"module_get({quote(a.name)})"))
        elif isinstance(x, ast.ImportFrom):
            if x.module in ("__future__", "typing", "collections.abc"): return
            module = self.value(f"module_get({quote(x.module)})")
            for a in x.names: self.put(a.asname or a.name, self.value(f"attr({module},{quote(a.name)})"))
        elif isinstance(x, ast.Assert):
            v = self.expr(x.test); self.emit(f"if(!truth({v}))fail(\"assertion at source line {x.lineno}\");")
        elif isinstance(x, ast.Delete):
            for t in x.targets:
                if isinstance(t, ast.Subscript):
                    obj, key = self.expr(t.value), self.expr(t.slice); self.emit(f"item_delete({obj},{key});")
                else: self.emit("/* local lifetime ends */")
        elif isinstance(x, ast.Raise):
            self.emit(f"raise_value({self.expr(x.exc) if x.exc else 'NONE'});")
        elif isinstance(x, ast.Try):
            frame = self.uid("trap"); self.emit(f"Trap {frame};trap_push(&{frame});if(!setjmp({frame}.jump)){{")
            self.block(x.body); self.emit(f"trap_pop(&{frame});")
            self.block(x.orelse); self.emit("}else{")
            self.emit(f"trap_pop(&{frame});")
            if x.handlers:
                h = x.handlers[0]
                if h.name: self.put(h.name, self.value("str(error_message)"))
                self.block(h.body)
            else: self.emit("fail(error_message);")
            self.emit("}"); self.block(x.finalbody)
        else: raise ValueError(f"Unsupported statement: {ast.dump(x)}")

    def compile_module(self, name, source, include=None):
        self.module_name=name
        tree = ast.parse(source)
        if name=='table_next':
            # Preserve the policy surrounding matching; use typed C arrays for
            # candidate similarities, graph components and optimal assignment.
            scorer=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='_score_table_next')
            start=next(i for i,n in enumerate(scorer.body) if isinstance(n,ast.AnnAssign) and isinstance(n.target,ast.Name) and n.target.id=='p_index')
            end=next(i for i,n in enumerate(scorer.body) if isinstance(n,ast.Assign) and isinstance(n.targets[0],ast.Name) and n.targets[0].id=='by_values')
            scorer.body[start:end]=ast.parse('M = _match_triples_native(G, P, pc, PT, trm, inv)').body
            for fn in ast.walk(scorer):
                if isinstance(fn,ast.FunctionDef) and fn.name=='kv':fn.body=ast.parse('return _kv_native(x)').body
                elif isinstance(fn,ast.FunctionDef) and fn.name=='fit':fn.body=ast.parse('return _fit_native(T, gis, kv_rows)').body
        if include is not None:
            tree.body = [x for x in tree.body if not isinstance(x, (ast.FunctionDef, ast.ClassDef)) or x.name in include]
        self.lines = []; self.env = "e"
        self.block(tree.body)
        self.functions.append(("init_" + name, "\n".join(self.lines) + "\nreturn NONE;"))


def main():
    import hashlib
    import parse_bench.evaluation.metrics.parse.text_next as text
    source = Path(text.__file__).parent
    contract=json.loads((ROOT.parent/'contract.json').read_text(encoding='utf-8'))
    for module,fingerprint in contract['modules'].items():
        path=source/(module.rsplit('.',1)[-1]+'.py')
        if hashlib.sha256(path.read_bytes()).hexdigest()!=fingerprint:
            raise SystemExit(f'Scoring source changed: {module}; validate the policy before updating contract.json')
    compiler = Compiler()
    for name in ("text_next", "formatting_v2", "table_next", "forms_v2", "chart_next"):
        compiler.compile_module(name, (source / f"{name}.py").read_text(encoding="utf-8"))
    out = ROOT / "generated"
    out.mkdir(exist_ok=True)
    declarations = "\n".join(f"static V {name}(Env *e);" for name, _ in compiler.functions)
    bodies = "\n\n".join(f"static V {name}(Env *e){{\n{body}\n}}" for name, body in compiler.functions)
    exports='\n'.join(f'V scorer_init_{name}(Env*e){{return init_{name}(e);}}' for name in ('text_next','formatting_v2','table_next','forms_v2','chart_next'))
    generated=('/* Derived from ParseBench 2.1.1 (Apache-2.0). See ../licenses.\n'
               '   Mechanical policy lowering plus native kernels; regenerate with compile.py. */\n'
               '#include "runtime.h"\n'+declarations+'\n'+bodies+'\n'+exports+'\n').replace('str("','S("')
    target=out/'scorers.c'
    if not target.exists() or target.read_text(encoding='utf-8')!=generated:target.write_text(generated,encoding='utf-8')
    import html.entities
    entries=sorted(html.entities.html5.items())
    (out/'entities.h').write_text('static const char *entity_names[]={'+','.join(quote(k) for k,v in entries)+'};\nstatic const char *entity_values[]={'+','.join(quote(v) for k,v in entries)+'};\n',encoding='ascii')
    import unicodedata
    digits=[]
    for cp in range(0x110000):
        value=unicodedata.digit(chr(cp),None)
        if value is not None:digits.append((cp,value))
    (out/'unicode_digits.h').write_text('static const uint32_t digit_points[]={'+','.join(str(cp) for cp,v in digits)+'};\nstatic const unsigned char digit_values[]={'+','.join(str(v) for cp,v in digits)+'};\n',encoding='ascii')
    print(f"Lowered {len(compiler.functions)} functions to {out / 'scorers.c'}")


if __name__ == "__main__":
    main()
