--
-- ion/mkexports.lua
-- 
-- Copyright (c) Tuomo Valkonen 2003.
-- 
-- Ion is free software; you can redistribute it and/or modify it under
-- the terms of the GNU Lesser General Public License as published by
-- the Free Software Foundation; either version 2.1 of the License, or
-- (at your option) any later version.
--
-- 
-- This is a script to automatically generate exported function registration
-- code and documentation for those from C source.
-- 
-- The script can also parse documentation comments from Lua code.
-- 

-- Helper functions {{{

function errorf(fmt, ...)
    error(string.format(fmt, unpack(arg)), 2)
end

function matcherr(s)
    error(string.format("Parse error in \"%s...\"", string.sub(s, 1, 50)), 2)
end

function fprintf(h, fmt, ...)
    h:write(string.format(fmt, unpack(arg)))
end

function trim(str)
    return string.gsub(str, "^[%s\n]*(.-)[%s\n]*$", "%1")
end

--�}}}


-- Some conversion tables {{{

desc2ct={
    ["v"]="void",
    ["i"]="int",
    ["d"]="double",
    ["b"]="bool",
    ["t"]="ExtlTab",
    ["f"]="ExtlFn",
    ["o"]="Obj*",
    ["s"]="char*",
    ["S"]="const char*",
}

ct2desc={
    ["uint"] = "i",
}

for d, t in desc2ct do
    ct2desc[t]=d
end

desc2human={
    ["v"]="void",
    ["i"]="integer",
    ["d"]="double",
    ["b"]="bool",
    ["t"]="table",
    ["f"]="function",
    ["o"]="object",
    ["s"]="string",
    ["S"]="string",
}

--�}}}


-- Parser {{{

local classes={}
local chnds={}

function add_chnd(fnt)
    local odesc=string.gsub(fnt.odesc, "S", "s")
    local idesc=string.gsub(fnt.idesc, "S", "s")
    local str="l2chnd_" .. odesc .. "_" .. idesc .. "_"
    
    for i, t in ipairs(fnt.itypes) do
        str=str .. "_" .. t
    end
    
    chnds[str]={odesc=odesc, idesc=idesc, itypes=fnt.itypes}
    fnt.chnd=str
end

function add_class(cls)
    if cls~="Obj" and not classes[cls] then
        classes[cls]={}
    end
end

function sort_classes(cls)
    local sorted={}
    local inserted={}
    
    local function insert(cls)
        if classes[cls] and not inserted[cls] then
            if classes[cls].parent then
                insert(classes[cls].parent)
            end
            inserted[cls]=true
            table.insert(sorted, cls)
        end
    end
    
    for cls in classes do
        insert(cls)
    end
    
    return sorted
end

function parse_type(t)
    local desc, otype, varname="?", "", ""
    
    -- Remove whitespace at start end end of the string and compress elsewhere.
    t=string.gsub(trim(t), "[%s\n]+", " ")
    -- Remove spaces around asterisks.
    t=string.gsub(t, " *%* *", "*")
    -- Add space after asterisks.
    t=string.gsub(t, "%*", "* ")
    
    -- Check for const
    local is_const=""
    local s, e=string.find(t, "^const +")
    if s then
        is_const="const "
        t=string.sub(t, e+1)
    end
    
    -- Find variable name part
    tn=t
    s, e=string.find(tn, " ")
    if s then
        varname=string.sub(tn, e+1)
        tn=string.sub(tn, 1, s-1)
        assert(not string.find(varname, " "))
    end
    
    -- Try to check for supported types
    desc = ct2desc[is_const .. tn]
    
    if not desc or desc=="o" then
        s, e=string.find(tn, "^[A-Z][%w_]*%*$")
        if s then
            desc="o"
            otype=string.sub(tn, s, e-1)
            add_class(otype)
        else
            errorf("Error parsing type from \"%s\"", t)
        end
    end
    
    return desc, otype, varname
end

function parse(d)
    local doc=nil
    -- Handle /*EXTL_DOC ... */
    local function do_doc(s)
        --s=string.gsub(s, "/%*EXTL_DOC(.-)%*/", "%1")
        local st=string.len("/*EXTL_DOC")
        local en, _=string.find(s, "%*/")
        if not en then
            errorf("Could not find end of comment in \"%s...\"",
                   string.sub(s, 1, 50))
        end
        
        s=string.sub(s, st+1, en-1)
        s=string.gsub(s, "\n[%s]*%*", "\n")
        doc=s
    end
    
    local function do_do_export(cls, efn, ot, fn, param)
        local odesc, otype=parse_type(ot)
        local idesc, itypes, ivars="", {}, {}
        
        -- Parse arguments
        param=string.sub(param, 2, -2)
        if string.find(param, "[()]") then
            errorf("Error: parameters to %s contain parantheses", fn)
        end
        param=trim(param)
        if string.len(param)>0 then
            for p in string.gfind(param .. ",", "([^,]*),") do
                local spec, objtype, varname=parse_type(p)
                idesc=idesc .. spec
                table.insert(itypes, objtype)
                table.insert(ivars, varname)
            end
        end
        
        if cls=="?" then
            if string.sub(idesc, 1, 1)~="o" then
                error("Invalid class for " .. fn)
            end
            cls=itypes[1]
        end
        
        -- Generate call handler name
        
        local fninfo={
            doc=doc,
            odesc=odesc,
            otype=otype,
            idesc=idesc,
            itypes=itypes,
            ivars=ivars,
            exported_name=efn,
            class=cls,
        }
        
        add_chnd(fninfo)
        add_class(cls)
        
        if not classes[cls].fns then
            classes[cls].fns={}
        end
        
        assert(not classes[cls].fns[fn], "Function " .. fn .. " multiply defined!")

        classes[cls].fns[fn]=fninfo

        -- Reset
        doc=nil
    end

    -- Handle EXTL_EXPORT otype fn(args)
    local function do_export(s)
        local mdl, efn
        local pat="^[%s\n]+EXTL_EXPORT[%s\n]+([%w%s_*]+[%s\n*])([%w_]+)[%s\n]*(%b())"
        local st, en, ot, fn, param=string.find(s, pat)
        
        if not st then matcherr(s) end
        
        if module==global or not module then
            mdl=module
            efn=fn
        else
            mdl=module
            pfx=mdl.."_"
            if string.sub(fn, 1, string.len(pfx))~=pfx then
                error('"'..fn..'" is not a valid function name of format '..
                      'modulename_fnname.')
            end
            efn=string.sub(fn, string.len(pfx)+1)
        end
        do_do_export(mdl, efn, ot, fn, param)
    end

    -- Handle EXTL_EXPORT_MEMBER otype prefix_fn(class, args)
    local function do_export_member(s)
        local pat="^[%s\n]+EXTL_EXPORT_MEMBER[%s\n]+([%w%s_*]+[%s\n*])([%w_]+)[%s\n]*(%b())"
        local st, en, ot, fn, param=string.find(s, pat)
        if not st then matcherr(s) end
        local efn=string.gsub(fn, ".-_(.*)", "%1")
        do_do_export("?", efn, ot, fn, param)
    end

    -- Handle EXTL_EXPORT_AS(table, member_fn) otype fn(args)
    local function do_export_as(s)
        local pat="^[%s\n]+EXTL_EXPORT_AS%(%s*([%w_]+)%s*,%s*([%w_]+)%s*%)[%s\n]+([%w%s_*]+[%s\n*])([%w_]+)[%s\n]*(%b())"
        local st, en, cls, efn, ot, fn, param=string.find(s, pat)
        if not st then matcherr(s) end
        do_do_export(cls, efn, ot, fn, param)
    end
    
    local function do_implobj(s)
        local pat="^[%s\n]+IMPLCLASS%(%s*([%w_]+)%s*,%s*([%w_]+)%s*,[^)]*%)"
        local st, en, cls, par=string.find(s, pat)
        if not st then matcherr(s) end
        add_class(cls)
        classes[cls].parent=par
    end
    
    local lookfor={
        ["/%*EXTL_DOC"] = do_doc,
        ["[%s\n]EXTL_EXPORT[%s\n]"] = do_export,
        ["[%s\n]EXTL_EXPORT_AS"] = do_export_as,
        ["[%s\n]EXTL_EXPORT_MEMBER[%s\n]"] = do_export_member,
        ["[%s\n]IMPLCLASS"] = do_implobj,
    }
    
    do_parse(d, lookfor)
end

function do_parse(d, lookfor)
    while true do
        local mins, mine, minfn=string.len(d)+1, nil, nil
        for str, fn in pairs(lookfor) do
            local s, e=string.find(d, str)
            if s and s<mins then
                mins, mine, minfn=s, e, fn
            end
        end
        
        if not minfn then
            return
        end
        
        minfn(string.sub(d, mins))
        d=string.sub(d, mine+1)
    end
end    

-- }}}


-- Parser for Lua code documentation {{{

function parse_luadoc(d)
    function do_luadoc(s_)
        local st, en, b, s=string.find(s_, "\n%-%-DOC(.-)(\n.*)")
        if string.find(b, "[^%s]") then
            errorf("Syntax error while parsing \"--DOC%s\"", b)
        end
        local doc, docl=""
        while true do
            st, en, docl=string.find(s, "^\n%s*%-%-([^\n]*\n)")
            if not st then
                break
            end
            --print(docl)
            doc=doc .. docl
            s=string.sub(s, en)
        end
        
        local fn, param
        
        st, en, fn, param=string.find(s, "^\n[%s\n]*function%s*([%w_:%.]+)%s*(%b())")

        if not fn then
            errorf("Syntax error while parsing \"%s\"",
                   string.sub(s, 1, 50))
        end
        local cls, clsfn
        st, en, cls, clsfn=string.find(fn, "^([^.]*)%.(.*)$")
        
        if cls and clsfn then
            fn=clsfn
        else
            cls="global"
        end
        
        fninfo={
            doc=doc, 
            paramstr=param,
            class=cls,
        }
        
        add_class(cls)
        if not classes[cls].fns then
            classes[cls].fns={}
        end
        classes[cls].fns[fn]=fninfo
    end
    
    do_parse(d, {["\n%-%-DOC"]=do_luadoc})
end
    
-- }}}


--�Export output {{{

function writechnd(h, name, info)
    local oct=desc2ct[info.odesc]
        
    -- begin blockwrite
    fprintf(h, [[
static bool %s(%s (*fn)(), ExtlL2Param *in, ExtlL2Param *out)
{
]], name, oct)
    -- end blockwrite

    -- Generate type checking code
    for k, t in info.itypes do
        if t~="" then
            if k==1 then
                fprintf(h, "    if(!chko1(in, %d, &CLASSDESCR(%s))) return FALSE;\n",
                        k-1, t)
            else
                fprintf(h, "    if(!chko(in, %d, &CLASSDESCR(%s))) return FALSE;\n",
                        k-1, t)
            end
        end
    end

    -- Generate function call code
    if info.odesc=="v" then
        fprintf(h, "    fn(")
    else
	fprintf(h, "    out[0].%s=fn(", info.odesc)
    end
    
    comma=""
    for k=1, string.len(info.idesc) do
        fprintf(h, comma .. "in[%d].%s", k-1, string.sub(info.idesc, k, k))
	comma=", "
    end
    fprintf(h, ");\n    return TRUE;\n}\n")
end    

function write_class_fns(h, cls, data)
    fprintf(h, "\n\nstatic ExtlExportedFnSpec %s_exports[] = {\n", cls)
    
    for fn, info in data.fns do
        local ods, ids="NULL", "NULL"
        if info.odesc~="v" then
            ods='"' .. info.odesc .. '"'
        end
        
        if info.idesc~="" then
            ids='"' .. info.idesc .. '"'
        end
        
        fprintf(h, "    {\"%s\", %s, %s, %s, (ExtlL2CallHandler*)%s},\n",
                info.exported_name, fn, ids, ods, info.chnd)
    end
    
    fprintf(h, "    {NULL, NULL, NULL, NULL, NULL}\n};\n\n")
end

function write_exports(h)
    -- begin blockwrite
    h:write([[
/* Automatically generated by mkexports.lua */
#include <libtu/output.h>
#include <libtu/obj.h>
#include <libtu/objp.h>
#include <ioncore/extl.h>

]])
    -- end blockwrite

    -- Write class infos and check that the class is implemented in the 
    -- module.
    for c, data in classes do
        if string.lower(c)==c then
            data.module=true
        else
            fprintf(h, "INTRCLASS(%s);\n", c)
            if data.fns and not data.parent then
                error("Class functions can only be registered if the object "
                      .. "is implemented in the module in question.")
            end
        end
    end
    
    -- begin blockwrite
    h:write([[

static const char chkfailstr[]=
    "Type checking failed in level 2 call handler for parameter %d "
    "(got %s, expected %s).";

static bool chko1(ExtlL2Param *in, int ndx, ClassDescr *descr)
{
    Obj *o=in[ndx].o;
    if(o==NULL){
        warn("Got nil object as first parameter.");
        return FALSE;
    }
    if(obj_is(o, descr)) return TRUE;
    warn(chkfailstr, ndx, OBJ_TYPESTR(o), descr->name);
    return FALSE;
}
              
static bool chko(ExtlL2Param *in, int ndx, ClassDescr *descr)
{
    Obj *o=in[ndx].o;
    if(o==NULL || obj_is(o, descr)) return TRUE;
    warn(chkfailstr, ndx, OBJ_TYPESTR(o), descr->name);
    return FALSE;
}

]])
    -- end blockwrite
    
    -- Write L2 call handlers
    for name, info in chnds do
        writechnd(h, name, info)
    end
    
    fprintf(h, "\n")
    
    for cls, data in classes do
        if data.fns then
            -- Write function declarations
            for fn in data.fns do
                fprintf(h, "extern void %s();\n", fn)
            end
            -- Write function table
            write_class_fns(h, cls, data)
        else
            fprintf(h, "#define %s_exports NULL\n", cls)
        end
    end
    
    fprintf(h, "bool %s_register_exports()\n{\n", module)

    local sorted_classes=sort_classes()
    
    for _, cls in sorted_classes do
        if cls=="global" then
            fprintf(h, "    if(!extl_register_functions(global_exports)) return FALSE;\n")
        elseif classes[cls].module then
            fprintf(h, "    if(!extl_register_module(\"%s\", %s_exports)) return FALSE;\n", 
                    cls, cls)
        elseif classes[cls].parent then
            fprintf(h, "    if(!extl_register_class(\"%s\", %s_exports, \"%s\")) return FALSE;\n",
                    cls, cls, classes[cls].parent)
        end
    end

    fprintf(h, "    return TRUE;\n}\n\nvoid %s_unregister_exports()\n{\n", 
            module)
    
    for _, cls in sorted_classes do
        if cls=="global" then
            fprintf(h, "    extl_unregister_functions(global_exports);\n")
        elseif classes[cls].module then
            fprintf(h, "    extl_unregister_module(\"%s\", %s_exports);\n", 
                    cls, cls)
        elseif classes[cls].parent then
            fprintf(h, "    extl_unregister_class(\"%s\", %s_exports);\n",
                    cls, cls)
        end
    end
    
    fprintf(h, "}\n\n")
end

-- }}}


-- Documentation output {{{

function tohuman(desc, objtype)
    if objtype~="" then
        return objtype
    else
        return desc2human[desc]
    end
end

function texfriendly(name)
    return string.gsub(name, "_", "-")
end

function texfriendly_typeormod(nm)
    if string.find(nm, "A-Z") then
        return "\\type{"..string.gsub(nm, '_', '\_').."}"
    else
        return "\\code{"..nm.."}"
    end
end

function write_fndoc(h, fn, info)
    if not info.doc then
        return
    end
    fprintf(h, "\\begin{function}\n")
    if info.exported_name then
        fn=info.exported_name
    end
    
    if info.class~="global" then
        fprintf(h, "\\index{%s@%s!", texfriendly(info.class), 
                texfriendly_typeormod(info.class));
        fprintf(h, "%s@\\code{%s}}\n", texfriendly(fn), fn)
    end
    fprintf(h, "\\index{%s@\\code{%s}}\n", texfriendly(fn), fn)
    
    if info.class~="global" then
        fprintf(h, "\\hyperlabel{fn:%s.%s}", info.class, fn)
    else
        fprintf(h, "\\hyperlabel{fn:%s}", fn)
    end
    
    fprintf(h, "\\synopsis{")
    if info.odesc then
        h:write(tohuman(info.odesc, info.otype).." ")
    end
    
    if info.class~="global" then
        fprintf(h, "%s.", info.class)
    end
    
    if not info.ivars then
        -- Lua input
        fprintf(h, "%s%s}", fn, info.paramstr)
    else
        fprintf(h, "%s(", fn)
        local comma=""
        for i, varname in info.ivars do
            fprintf(h, comma .. "%s", tohuman(string.sub(info.idesc, i, i),
                                              info.itypes[i]))
            if varname then
                fprintf(h, " %s", varname)
            end
            comma=", "
        end
        fprintf(h, ")}\n")
    end
    h:write("\\begin{funcdesc}\n" .. trim(info.doc).. "\n\\end{funcdesc}\n")
    fprintf(h, "\\end{function}\n\n")
end


function write_class_documentation(h, cls, in_subsect)
    sorted={}
    
    if not classes[cls] or not classes[cls].fns then
        return
    end
    
    if in_subsect then
        fprintf(h, "\n\n\\subsection{\\type{%s} functions}\n\n", cls)
    end

    for fn in classes[cls].fns do
        table.insert(sorted, fn)
    end
    table.sort(sorted)
    
    for _, fn in ipairs(sorted) do
        write_fndoc(h, fn, classes[cls].fns[fn])
    end
end


function write_documentation(h)
    sorted={}
    
    write_class_documentation(h, module, false)
    
    for cls in classes do
        if cls~=module then
            table.insert(sorted, cls)
        end
    end
    table.sort(sorted)
    
    for _, cls in ipairs(sorted) do
        write_class_documentation(h, cls, true)
    end
end

-- }}}


-- main {{{

inputs={}
outh=io.stdout
make_docs=false
module="global"
i=1

while arg[i] do
    if arg[i]=="-help" then
        print("Usage: mkexports.lua [-mkdoc] [-help] [-o outfile] [-module module] inputs...")
        return
    elseif arg[i]=="-mkdoc" then
        make_docs=true
    elseif arg[i]=="-o" then
        i=i+1
        outh, err=io.open(arg[i], "w")
        if not outh then
            errorf("Could not open %s: %s", arg[i], err)
        end
    elseif arg[i]=="-module" then
        i=i+1
        module=arg[i]
        if not module then
            error("No module given")
        end
    else
        table.insert(inputs, arg[i])
    end
    i=i+1
end

if table.getn(inputs)==0 then
    error("No inputs")
end

for _, ifnam in inputs do
    h, err=io.open(ifnam, "r")
    if not h then
            errorf("Could not open %s: %s", ifnam, err)
    end
    print("Scanning " .. ifnam .. " for exports.")
    data=h:read("*a")
    h:close()
    if string.find(ifnam, "%.lua$") then
        assert(make_docs)
        parse_luadoc("\n" .. data .. "\n")
    elseif string.find(ifnam, "%.c$") then
        parse("\n" .. data .. "\n")
    else
        error('Unknown file')
    end
    
end

if make_docs then
    write_documentation(outh)
else
    write_exports(outh)
end

-- }}}
