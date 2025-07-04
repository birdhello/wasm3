//
//  m3_parse.c
//
//  Created by Steven Massey on 4/19/19.
//  Copyright © 2019 Steven Massey. All rights reserved.
//

#include "m3_env.h"
#include "m3_compile.h"
#include "m3_exception.h"
#include "m3_info.h"

M3Result  ParseType_Table  (IM3Table table, bytes_t *i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u8 type;
    u8 flag;
    u32 initSize, maxSize;

    _(ReadLEB_u7 (&type, i_bytes, i_end));
    _(ReadLEB_u7 (&flag, i_bytes, i_end));
    _(ReadLEB_u32 (&initSize, i_bytes, i_end));

    maxSize = 0;
    if (flag) {
        _(ReadLEB_u32 (&maxSize, i_bytes, i_end));
    }
    table->functions = m3_AllocArray(IM3Function, initSize);
    _throwifnull(table->functions);
    table->elements = initSize;
    table->maxSize = maxSize;
    table->type = type;
_catch:
    return result;
}


M3Result  ParseType_Memory  (M3MemoryInfo * o_memory, bytes_t * io_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u8 flag;

_   (ReadLEB_u7 (& flag, io_bytes, i_end));                   // really a u1
_   (ReadLEB_u32 (& o_memory->initPages, io_bytes, i_end));

    o_memory->maxPages = 0;
    if (flag)
_       (ReadLEB_u32 (& o_memory->maxPages, io_bytes, i_end));

    _catch: return result;
}

M3Result  ParseSection_Type  (IM3Module io_module, bytes_t i_bytes, cbytes_t i_end)
{
    IM3FuncType ftype = NULL;

_try {
    u32 numTypes;
_   (ReadLEB_u32 (& numTypes, & i_bytes, i_end));                                   m3log (parse, "** Type [%d]", numTypes);

    _throwif("too many types", numTypes > d_m3MaxSaneTypesCount);

    if (numTypes)
    {
        // table of IM3FuncType (that point to the actual M3FuncType struct in the Environment)
        io_module->funcTypes = m3_AllocArray (IM3FuncType, numTypes);
        _throwifnull (io_module->funcTypes);
        io_module->numFuncTypes = numTypes;

        for (u32 i = 0; i < numTypes; ++i)
        {
            i8 form;
_           (ReadLEB_i7 (& form, & i_bytes, i_end));
            _throwif (m3Err_wasmMalformed, form != -32); // for Wasm MVP

            u32 numArgs;
_           (ReadLEB_u32 (& numArgs, & i_bytes, i_end));

            _throwif (m3Err_tooManyArgsRets, numArgs > d_m3MaxSaneFunctionArgRetCount);
#if defined(M3_COMPILER_MSVC)
            u8 argTypes [d_m3MaxSaneFunctionArgRetCount];
#else
            u8 argTypes[numArgs+1]; // make ubsan happy
#endif
            for (u32 a = 0; a < numArgs; ++a)
            {
                i8 wasmType;
                u8 argType;
_               (ReadLEB_i7 (& wasmType, & i_bytes, i_end));
_               (NormalizeType (& argType, wasmType));

                argTypes[a] = argType;
            }

            u32 numRets;
_           (ReadLEB_u32 (& numRets, & i_bytes, i_end));
            _throwif (m3Err_tooManyArgsRets, (u64)(numRets) + numArgs > d_m3MaxSaneFunctionArgRetCount);

_           (AllocFuncType (& ftype, numRets + numArgs));
            ftype->numArgs = numArgs;
            ftype->numRets = numRets;

            for (u32 r = 0; r < numRets; ++r)
            {
                i8 wasmType;
                u8 retType;
_               (ReadLEB_i7 (& wasmType, & i_bytes, i_end));
_               (NormalizeType (& retType, wasmType));

                ftype->types[r] = retType;
            }
            memcpy (ftype->types + numRets, argTypes, numArgs);                                 m3log (parse, "    type %2d: %s", i, SPrintFuncTypeSignature (ftype));

            Environment_AddFuncType (io_module->environment, & ftype);
            io_module->funcTypes [i] = ftype;
            ftype = NULL; // ownership transferred to environment
        }
    }

} _catch:

    if (result)
    {
        m3_Free (ftype);
        // FIX: M3FuncTypes in the table are leaked
        m3_Free (io_module->funcTypes);
        io_module->numFuncTypes = 0;
    }

    return result;
}


M3Result  ParseSection_Function  (IM3Module io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u32 numFunctions;
_   (ReadLEB_u32 (& numFunctions, & i_bytes, i_end));                               m3log (parse, "** Function [%d]", numFunctions);

    _throwif("too many functions", numFunctions > d_m3MaxSaneFunctionsCount);

_   (Module_PreallocFunctions(io_module, io_module->numFunctions + numFunctions));

    for (u32 i = 0; i < numFunctions; ++i)
    {
        u32 funcTypeIndex;
_       (ReadLEB_u32 (& funcTypeIndex, & i_bytes, i_end));

_       (Module_AddFunction (io_module, funcTypeIndex, NULL /* import info */));
    }

    _catch: return result;
}


M3Result  ParseSection_Import  (IM3Module io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    M3ImportInfo import = { NULL, NULL }, clearImport = { NULL, NULL };

    u32 numImports;
_   (ReadLEB_u32 (& numImports, & i_bytes, i_end));                                 m3log (parse, "** Import [%d]", numImports);

    _throwif("too many imports", numImports > d_m3MaxSaneImportsCount);

    // Most imports are functions, so we won't waste much space anyway (if any)
_   (Module_PreallocFunctions(io_module, numImports));

    for (u32 i = 0; i < numImports; ++i)
    {
        u8 importKind;

_       (Read_utf8 (& import.moduleUtf8, & i_bytes, i_end));
_       (Read_utf8 (& import.fieldUtf8, & i_bytes, i_end));
_       (Read_u8 (& importKind, & i_bytes, i_end));                                 m3log (parse, "    kind: %d '%s.%s' ",
                                                                                                (u32) importKind, import.moduleUtf8, import.fieldUtf8);
        switch (importKind)
        {
            case d_externalKind_function:
            {
                u32 typeIndex;
_               (ReadLEB_u32 (& typeIndex, & i_bytes, i_end))

_               (Module_AddFunction (io_module, typeIndex, & import))
                import = clearImport;

                io_module->numFuncImports++;
            }
            break;

            case d_externalKind_table:
            {
                u32 numTables = io_module->numTables + 1;
                io_module->tables = m3_ReallocArray (M3Table, io_module->tables, numTables, io_module->numTables);
                _throwifnull (io_module->tables);
                IM3Table table = &io_module->tables[io_module->numTables];
                io_module->numTables = numTables;
                ParseType_Table (table, & i_bytes, i_end);
                table->import = import;
                import = clearImport;
            }
                break;

            case d_externalKind_memory:
            {
_               (ParseType_Memory (& io_module->memoryInfo, & i_bytes, i_end));
                io_module->memoryImported = true;
                io_module->memoryInfo.import = import;
                import = clearImport;
            }
            break;

            case d_externalKind_global:
            {
                i8 waType;
                u8 type, isMutable;

_               (ReadLEB_i7 (& waType, & i_bytes, i_end));
_               (NormalizeType (& type, waType));
_               (ReadLEB_u7 (& isMutable, & i_bytes, i_end));                     m3log (parse, "     global: %s mutable=%d", c_waTypes [type], (u32) isMutable);

                IM3Global global;
_               (Module_AddGlobal (io_module, & global, type, isMutable, true /* isImport */));
                global->import = import;
                import = clearImport;
            }
            break;

            default:
                _throw (m3Err_wasmMalformed);
        }

        FreeImportInfo (& import);
    }

    _catch:

    FreeImportInfo (& import);

    return result;
}


M3Result  ParseSection_Export  (IM3Module io_module, bytes_t i_bytes, cbytes_t  i_end)
{
    M3Result result = m3Err_none;
    const char * utf8 = NULL;

    u32 numExports;
_   (ReadLEB_u32 (& numExports, & i_bytes, i_end));                                 m3log (parse, "** Export [%d]", numExports);

    _throwif("too many exports", numExports > d_m3MaxSaneExportsCount);

    for (u32 i = 0; i < numExports; ++i)
    {
        u8 exportKind;
        u32 index;

_       (Read_utf8 (& utf8, & i_bytes, i_end));
_       (Read_u8 (& exportKind, & i_bytes, i_end));
_       (ReadLEB_u32 (& index, & i_bytes, i_end));                                  m3log (parse, "    index: %3d; kind: %d; export: '%s'; ", index, (u32) exportKind, utf8);

        if (exportKind == d_externalKind_function)
        {
            _throwif(m3Err_wasmMalformed, index >= io_module->numFunctions);
            IM3Function func = &(io_module->functions [index]);
            if (func->numNames < d_m3MaxDuplicateFunctionImpl)
            {
                func->names[func->numNames++] = utf8;
                utf8 = NULL; // ownership transferred to M3Function
            } else {
                _throw("too many duplicate export functions.");
            }
        }
        else if (exportKind == d_externalKind_global)
        {
            _throwif(m3Err_wasmMalformed, index >= io_module->numGlobals);
            IM3Global global = &(io_module->globals [index]);
            m3_Free (global->name);
            global->name = utf8;
            utf8 = NULL; // ownership transferred to M3Global
        }
        else if (exportKind == d_externalKind_memory) {
            _throwif(m3Err_wasmMalformed, index != 0);
            io_module->memoryInfo.exportName = utf8;
            utf8 = NULL; // ownership transferred to M3MemoryInfo
        }
        else if (exportKind == d_externalKind_table) {
            IM3Table table = &(io_module->tables [index]);
            m3_Free (table->exportName);
            table->exportName = utf8;
            utf8 = NULL; // ownership transferred to M3Module
        }

        m3_Free (utf8);
    }

_catch:
    m3_Free (utf8);
    return result;
}


M3Result  ParseSection_Start  (IM3Module io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u32 startFuncIndex;
_   (ReadLEB_u32 (& startFuncIndex, & i_bytes, i_end));                               m3log (parse, "** Start Function: %d", startFuncIndex);

    if (startFuncIndex < io_module->numFunctions)
    {
        io_module->startFunction = startFuncIndex;
    }
    else result = "start function index out of bounds";

    _catch: return result;
}


M3Result  Parse_InitExpr  (M3Module * io_module, bytes_t * io_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    // this doesn't generate code pages. just walks the wasm bytecode to find the end

#if defined(d_m3PreferStaticAlloc)
    static M3Compilation compilation;
#else
    M3Compilation compilation;
#endif
    compilation = (M3Compilation){ .runtime = NULL, .module = io_module, .wasm = * io_bytes, .wasmEnd = i_end };

    result = CompileBlockStatements (& compilation);

    * io_bytes = compilation.wasm;

    return result;
}


M3Result  ParseSection_Element  (IM3Module io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u32 numSegments;
_   (ReadLEB_u32 (& numSegments, & i_bytes, i_end));                         m3log (parse, "** Element [%d]", numSegments);

    _throwif ("too many element segments", numSegments > d_m3MaxSaneElementSegments);

    io_module->elementSection = i_bytes;
    io_module->elementSectionEnd = i_end;
    io_module->numElementSegments = numSegments;

    _catch: return result;
}


M3Result  ParseSection_Code  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result;

    u32 numFunctions;
_   (ReadLEB_u32 (& numFunctions, & i_bytes, i_end));                               m3log (parse, "** Code [%d]", numFunctions);

    if (numFunctions != io_module->numFunctions - io_module->numFuncImports)
    {
        _throw ("mismatched function count in code section");
    }

    for (u32 f = 0; f < numFunctions; ++f)
    {
        const u8 * start = i_bytes;

        u32 size;
_       (ReadLEB_u32 (& size, & i_bytes, i_end));

        if (size)
        {
            const u8 * ptr = i_bytes;
            i_bytes += size;

            if (i_bytes <= i_end)
            {
                /*
                u32 numLocalBlocks;
_               (ReadLEB_u32 (& numLocalBlocks, & ptr, i_end));                                      m3log (parse, "    code size: %-4d", size);

                u32 numLocals = 0;

                for (u32 l = 0; l < numLocalBlocks; ++l)
                {
                    u32 varCount;
                    i8 wasmType;
                    u8 normalType;

_                   (ReadLEB_u32 (& varCount, & ptr, i_end));
_                   (ReadLEB_i7 (& wasmType, & ptr, i_end));
_                   (NormalizeType (& normalType, wasmType));

                    numLocals += varCount;                                                      m3log (parse, "      %2d locals; type: '%s'", varCount, c_waTypes [normalType]);
                }
                 */

                IM3Function func = Module_GetFunction (io_module, f + io_module->numFuncImports);

                func->module = io_module;
                func->wasm = start;
                func->wasmEnd = i_bytes;
                //func->ownsWasmCode = io_module->hasWasmCodeCopy;
//                func->numLocals = numLocals;
            }
            else _throw (m3Err_wasmSectionOverrun);
        }
    }

    _catch:

    if (not result and i_bytes != i_end)
        result = m3Err_wasmSectionUnderrun;

    return result;
}


M3Result  ParseSection_Data  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u32 numDataSegments;
_   (ReadLEB_u32 (& numDataSegments, & i_bytes, i_end));                            m3log (parse, "** Data [%d]", numDataSegments);

    _throwif("too many data segments", numDataSegments > d_m3MaxSaneDataSegments);

    io_module->dataSegments = m3_AllocArray (M3DataSegment, numDataSegments);
    _throwifnull(io_module->dataSegments);
    io_module->numDataSegments = numDataSegments;

    for (u32 i = 0; i < numDataSegments; ++i)
    {
        M3DataSegment * segment = & io_module->dataSegments [i];

_       (ReadLEB_u32 (& segment->memoryRegion, & i_bytes, i_end));

        segment->initExpr = i_bytes;
_       (Parse_InitExpr (io_module, & i_bytes, i_end));
        segment->initExprSize = (u32) (i_bytes - segment->initExpr);

        _throwif (m3Err_wasmMissingInitExpr, segment->initExprSize <= 1);

_       (ReadLEB_u32 (& segment->size, & i_bytes, i_end));
        segment->data = i_bytes;                                                    m3log (parse, "    segment [%u]  memory: %u;  expr-size: %d;  size: %d",
                                                                                       i, segment->memoryRegion, segment->initExprSize, segment->size);
        i_bytes += segment->size;

        _throwif("data segment underflow", i_bytes > i_end);
    }

    _catch:

    return result;
}

M3Result  ParseSection_Table  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u32 numTables;
    _   (ReadLEB_u32 (& numTables, & i_bytes, i_end));                             m3log (parse, "** Table [%d]", numMemories);

    _throwif ("too many tables", numTables > d_m3MaxSaneTableSize);

    numTables += io_module->numTables;
    io_module->tables = m3_ReallocArray (M3Table, io_module->tables, numTables, io_module->numTables);
    _throwifnull (io_module->tables);

    for (u32 i = 0; i < numTables; ++i) {
        ParseType_Table (&io_module->tables[io_module->numTables + i], &i_bytes, i_end);
    }
    io_module->numTables = numTables;

_catch: return result;
}


M3Result  ParseSection_Memory  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    // TODO: MVP; assert no memory imported

    u32 numMemories;
_   (ReadLEB_u32 (& numMemories, & i_bytes, i_end));                             m3log (parse, "** Memory [%d]", numMemories);

    _throwif (m3Err_tooManyMemorySections, numMemories != 1);

    ParseType_Memory (& io_module->memoryInfo, & i_bytes, i_end);

    _catch: return result;
}


M3Result  ParseSection_Global  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result = m3Err_none;

    u32 numGlobals;
_   (ReadLEB_u32 (& numGlobals, & i_bytes, i_end));                                 m3log (parse, "** Global [%d]", numGlobals);

    _throwif("too many globals", numGlobals > d_m3MaxSaneGlobalsCount);

    for (u32 i = 0; i < numGlobals; ++i)
    {
        i8 waType;
        u8 type, isMutable;

_       (ReadLEB_i7 (& waType, & i_bytes, i_end));
_       (NormalizeType (& type, waType));
_       (ReadLEB_u7 (& isMutable, & i_bytes, i_end));                                 m3log (parse, "    global: [%d] %s mutable: %d", i, c_waTypes [type],   (u32) isMutable);

        IM3Global global;
_       (Module_AddGlobal (io_module, & global, type, isMutable, false /* isImport */));

        global->initExpr = i_bytes;
_       (Parse_InitExpr (io_module, & i_bytes, i_end));
        global->initExprSize = (u32) (i_bytes - global->initExpr);

        _throwif (m3Err_wasmMissingInitExpr, global->initExprSize <= 1);
    }

    _catch: return result;
}


M3Result  ParseSection_Name  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result;

    cstr_t name;

    while (i_bytes < i_end)
    {
        u8 nameType;
        u32 payloadLength;

_       (ReadLEB_u7 (& nameType, & i_bytes, i_end));
_       (ReadLEB_u32 (& payloadLength, & i_bytes, i_end));

        bytes_t start = i_bytes;
        if (nameType == 1)
        {
            u32 numNames;
_           (ReadLEB_u32 (& numNames, & i_bytes, i_end));

            _throwif("too many names", numNames > d_m3MaxSaneFunctionsCount);

            for (u32 i = 0; i < numNames; ++i)
            {
                u32 index;
_               (ReadLEB_u32 (& index, & i_bytes, i_end));
_               (Read_utf8 (& name, & i_bytes, i_end));

                if (index < io_module->numFunctions)
                {
                    IM3Function func = &(io_module->functions [index]);
                    if (func->numNames == 0)
                    {
                        func->names[0] = name;        m3log (parse, "    naming function%5d:  %s", index, name);
                        func->numNames = 1;
                        name = NULL; // transfer ownership
                    }
//                          else m3log (parse, "prenamed: %s", io_module->functions [index].name);
                }

                m3_Free (name);
            }
        }

        i_bytes = start + payloadLength;
    }

    _catch: return result;
}


M3Result  ParseSection_Custom  (M3Module * io_module, bytes_t i_bytes, cbytes_t i_end)
{
    M3Result result;

    cstr_t name;
_   (Read_utf8 (& name, & i_bytes, i_end));
                                                                                    m3log (parse, "** Custom: '%s'", name);
    if (strcmp (name, "name") == 0) {
_       (ParseSection_Name(io_module, i_bytes, i_end));
    } else if (io_module->environment->customSectionHandler) {
_       (io_module->environment->customSectionHandler(io_module, name, i_bytes, i_end));
    }

    m3_Free (name);

    _catch: return result;
}


M3Result  ParseModuleSection  (M3Module * o_module, u8 i_sectionType, bytes_t i_bytes, u32 i_numBytes)
{
    M3Result result = m3Err_none;

    typedef M3Result (* M3Parser) (M3Module *, bytes_t, cbytes_t);

    static M3Parser s_parsers [] =
    {
        ParseSection_Custom,    // 0
        ParseSection_Type,      // 1
        ParseSection_Import,    // 2
        ParseSection_Function,  // 3
        ParseSection_Table,     // 4
        ParseSection_Memory,    // 5
        ParseSection_Global,    // 6
        ParseSection_Export,    // 7
        ParseSection_Start,     // 8
        ParseSection_Element,   // 9
        ParseSection_Code,      // 10
        ParseSection_Data,      // 11
        NULL,                   // 12: TODO DataCount
    };

    M3Parser parser = NULL;

    if (i_sectionType <= 12)
        parser = s_parsers [i_sectionType];

    if (parser)
    {
        cbytes_t end = i_bytes + i_numBytes;
        result = parser (o_module, i_bytes, end);
    }
    else
    {
        m3log (parse, " skipped section type: %d", (u32) i_sectionType);
    }

    return result;
}


M3Result  m3_ParseModule  (IM3Environment i_environment, IM3Module * o_module, cbytes_t i_bytes, u32 i_numBytes)
{
    IM3Module module;                                                               m3log (parse, "load module: %d bytes", i_numBytes);
_try {
    module = m3_AllocStruct (M3Module);
    _throwifnull (module);
    module->name = ".unnamed";                                                      m3log (parse, "load module: %d bytes", i_numBytes);
    module->startFunction = -1;
    //module->hasWasmCodeCopy = false;
    module->environment = i_environment;
    module->numTables = 0;

    const u8 * pos = i_bytes;
    const u8 * end = pos + i_numBytes;

    module->wasmStart = pos;
    module->wasmEnd = end;

    u32 magic, version;
_   (Read_u32 (& magic, & pos, end));
_   (Read_u32 (& version, & pos, end));

    _throwif (m3Err_wasmMalformed, magic != 0x6d736100);
    _throwif (m3Err_incompatibleWasmVersion, version != 1);

    static const u8 sectionsOrder[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 10, 11, 0 }; // 0 is a placeholder
    u8 expectedSection = 0;

    while (pos < end)
    {
        u8 section;
_       (ReadLEB_u7 (& section, & pos, end));

        if (section != 0) {
            // Ensure sections appear only once and in order
            while (sectionsOrder[expectedSection++] != section) {
                _throwif(m3Err_misorderedWasmSection, expectedSection >= 12);
            }
        }

        u32 sectionLength;
_       (ReadLEB_u32 (& sectionLength, & pos, end));
        _throwif(m3Err_wasmMalformed, pos + sectionLength > end);

_       (ParseModuleSection (module, section, pos, sectionLength));

        pos += sectionLength;
    }

} _catch:

    if (result)
    {
        m3_FreeModule (module);
        module = NULL;
    }

    * o_module = module;

    return result;
}
