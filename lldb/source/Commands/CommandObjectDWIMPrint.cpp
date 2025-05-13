//===-- CommandObjectDWIMPrint.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectDWIMPrint.h"

#include "lldb/Core/ValueObject.h"
#include "lldb/DataFormatters/DumpValueObjectOptions.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionGroupFormat.h"
#include "lldb/Interpreter/OptionGroupValueObjectDisplay.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/lldb-defines.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Core/Module.h"
#include <Plugins/SymbolFile/DWARF/SymbolFileDWARF.h>
#include <Plugins/SymbolFile/DWARF/DWARFUnit.h>


using namespace llvm;
using namespace lldb;
using namespace lldb_private;

CommandObjectDWIMPrint::CommandObjectDWIMPrint(CommandInterpreter &interpreter)
    : CommandObjectRaw(interpreter, "dwim-print",
                       "Print a variable or expression.",
                       "dwim-print [<variable-name> | <expression>]",
                       eCommandProcessMustBePaused | eCommandTryTargetAPILock) {

  CommandArgumentData var_name_arg(eArgTypeVarName, eArgRepeatPlain);
  m_arguments.push_back({var_name_arg});

  m_option_group.Append(&m_format_options,
                        OptionGroupFormat::OPTION_GROUP_FORMAT |
                            OptionGroupFormat::OPTION_GROUP_GDB_FMT,
                        LLDB_OPT_SET_1);
  StringRef exclude_expr_options[] = {"debug", "top-level"};
  m_option_group.Append(&m_expr_options, exclude_expr_options);
  m_option_group.Append(&m_varobj_options, LLDB_OPT_SET_ALL, LLDB_OPT_SET_1);
  m_option_group.Finalize();
}

Options *CommandObjectDWIMPrint::GetOptions() { return &m_option_group; }

void CommandObjectDWIMPrint::HandleArgumentCompletion(
    CompletionRequest &request, OptionElementVector &opt_element_vector) {
  CommandCompletions::InvokeCommonCompletionCallbacks(
      GetCommandInterpreter(), CommandCompletions::eVariablePathCompletion,
      request, nullptr);
}

bool CommandObjectDWIMPrint::DoExecute(StringRef command,
                                       CommandReturnObject &result) {
  m_option_group.NotifyOptionParsingStarting(&m_exe_ctx);
  OptionsWithRaw args{command};
  StringRef expr = args.GetRawPart();

  if (expr.empty()) {
    result.AppendErrorWithFormatv("'{0}' takes a variable or expression",
                                  m_cmd_name);
    return false;
  }

  if (args.HasArgs()) {
    if (!ParseOptionsAndNotify(args.GetArgs(), result, m_option_group,
                               m_exe_ctx))
      return false;
  }

  // If the user has not specified, default to disabling persistent results.
  if (m_expr_options.suppress_persistent_result == eLazyBoolCalculate)
    m_expr_options.suppress_persistent_result = eLazyBoolYes;

  auto verbosity = GetDebugger().GetDWIMPrintVerbosity();

  Target *target_ptr = m_exe_ctx.GetTargetPtr();
  // Fallback to the dummy target, which can allow for expression evaluation.
  Target &target = target_ptr ? *target_ptr : GetDummyTarget();

  const EvaluateExpressionOptions eval_options =
      m_expr_options.GetEvaluateExpressionOptions(target, m_varobj_options);

  DumpValueObjectOptions dump_options = m_varobj_options.GetAsDumpOptions(
      m_expr_options.m_verbosity, m_format_options.GetFormat());
  dump_options.SetHideRootName(eval_options.GetSuppressPersistentResult());

  if (pp_struct_tags.empty())
    InitPPStructures();

  // First, try `expr` as the name of a frame variable.
  if (StackFrame *frame = m_exe_ctx.GetFramePtr()) {
    auto valobj_sp = frame->FindVariable(ConstString(expr));
    if (valobj_sp && valobj_sp->GetError().Success()) {
      if (!eval_options.GetSuppressPersistentResult()) {
        if (auto persisted_valobj = valobj_sp->Persist())
          valobj_sp = persisted_valobj;
      }

      if (verbosity == eDWIMPrintVerbosityFull) {
        StringRef flags;
        if (args.HasArgs())
          flags = args.GetArgString();
        result.AppendMessageWithFormatv("note: ran `frame variable {0}{1}`",
                                        flags, expr);
      }

      valobj_sp->Dump(result.GetOutputStream(), dump_options);
      result.SetStatus(eReturnStatusSuccessFinishResult);
      return true;
    }
  }

  // Second, also lastly, try `expr` as a source expression to evaluate.
  bool result_expr = EvaluateExpr(target, command, eval_options, result, dump_options);
  return result_expr;
}

bool CommandObjectDWIMPrint::InitPPStructures() {
  StackFrame *frame = m_exe_ctx.GetFramePtr();
  const SymbolContext &sc = frame->GetSymbolContext(lldb::eSymbolContextCompUnit);
  lldb_private::CompileUnit *cu = sc.comp_unit;
  VariableListSP global_variable_list_sp;

  std::map<ConstString, lldb::TypeSP> struct_types_map;

  if (!cu)
    return false;

  global_variable_list_sp = cu->GetVariableList(true);
  if (!global_variable_list_sp)
    return false;

  TargetSP target_sp = m_exe_ctx.GetTargetSP();
  ExecutionContext exe_ctx = m_exe_ctx;

  for (size_t i = 0; i < global_variable_list_sp->GetSize(); ++i) {
    lldb::VariableSP var_sp = global_variable_list_sp->GetVariableAtIndex(i);

    if (!var_sp)
      continue;

    lldb::ValueObjectSP valobj_sp = ValueObjectVariable::Create(exe_ctx.GetBestExecutionContextScope(), var_sp);

    llvm::StringRef tag_prefix = "__pp_tag___pp_struct_";

    llvm::StringRef name = var_sp->GetName().AsCString();

    size_t pos = name.find(tag_prefix);

    if (pos == llvm::StringRef::npos)
      continue;

    name = name.drop_front(pos + tag_prefix.size());

    auto [name1, name2] = name.split("__");

    if (name1.empty() || name2.empty())
      continue;

    llvm::StringRef value = valobj_sp->GetValueAsCString();

    ConstString key((name1 + "_" + value).str());
    ConstString val(name2);

    pp_struct_tags[key] = val;
  }

  return true;
}

void CommandObjectDWIMPrint::ExtractStructNames(llvm::StringRef mangled, llvm::StringSet<> &result) {
  const llvm::StringRef prefix = "__pp_struct_";
  while (!mangled.empty()) {
    size_t pos = mangled.find(prefix);

    if (pos != llvm::StringRef::npos)
      mangled = mangled.drop_front(pos + prefix.size());

    size_t end = mangled.find("__");

    llvm::StringRef name;

    if (end == 0 && pos == llvm::StringRef::npos) {
      name = mangled.substr(end + 2);
    } else if (end != llvm::StringRef::npos) {
      name = mangled.substr(0, end);
    }

    if (!name.empty())
      result.insert(name);

    if (end == llvm::StringRef::npos || pos == llvm::StringRef::npos)
      break;

    mangled = mangled.drop_front(end);
  }
}

// TO-DO: Refactor
StringRef CommandObjectDWIMPrint::GetLastValuePPSpecIfPossible(lldb::ValueObjectSP valobj) {
  if (!valobj || !valobj->GetError().Success())
    return "";

  size_t num_children = valobj->GetNumChildren();
  ValueObjectSP tail_child;

  for (size_t i = 0; i < num_children; ++i) {
    ValueObjectSP child = valobj->GetChildAtIndex(i, true);
    if (child && child->GetName() == "__pp_tail") {
      tail_child = child;
      break;
    }
  }

  if (tail_child)
    return GetLastValuePPSpecIfPossible(tail_child);

  for (size_t i = 0; i < num_children; ++i) {
    ValueObjectSP child = valobj->GetChildAtIndex(i, true);
    if (child && child->GetName() == "__pp_specialization_type") {
      return child->GetValueAsCString();
    }
  }

  return "";
}

// TO-DO: Refactor
bool CommandObjectDWIMPrint::EvaluateExpr(Target &target, 
                                          StringRef command, 
                                          const EvaluateExpressionOptions& eval_options, 
                                          CommandReturnObject &result, 
                                          DumpValueObjectOptions& dump_options) {

  OptionsWithRaw args{command};
  StringRef expr = args.GetRawPart();
  
  auto verbosity = GetDebugger().GetDWIMPrintVerbosity();
  
  ModuleSP module_sp = target.GetImages().GetModuleAtIndex(0);
  SymbolFile *symbol_file = module_sp->GetSymbolFile();
  lldb_private::TypeList type_list;
  symbol_file->GetTypes(nullptr, lldb::eTypeClassStruct, type_list);
  type_list.SortByName();

  auto *exe_scope = m_exe_ctx.GetBestExecutionContextScope();
  ValueObjectSP valobj_sp;
  ExpressionResults expr_result = target.EvaluateExpression(expr, exe_scope, valobj_sp, eval_options);

  if (expr_result == eExpressionCompleted) {
    if (verbosity != eDWIMPrintVerbosityNone) {
      StringRef flags;
      if (args.HasArgs())
        flags = args.GetArgStringWithDelimiter();
      result.AppendMessageWithFormatv("note: ran `expression {0}{1}`", flags,
                                      expr);
    }

    StringRef lastPPSpec = GetLastValuePPSpecIfPossible(valobj_sp);

    if (!lastPPSpec.empty()) {

      std::string lookup_type_name = CreateNewLookupType(valobj_sp, lastPPSpec);
      StringRef new_lookup_type_name(lookup_type_name);

      StringRef new_type_name;
      std::string new_command;
      size_t pos_ref = expr.find("*");

      for (size_t i = 0; i < type_list.GetSize(); ++i) {
        lldb::TypeSP type_sp = type_list.GetTypeAtIndex(i);
        if (!type_sp)
            continue;

        StringRef name = type_sp->GetName().GetStringRef();
        if (name.starts_with(new_lookup_type_name)){
          ValueObjectSP suspect_valobj_sp;
          new_type_name = name;
          new_command = expr.substr(0, pos_ref+1).str() + "(" + new_type_name.str() + "*)" + expr.substr(pos_ref+1).str();
          
          ExpressionResults suspect_expr_result = target.EvaluateExpression(new_command, exe_scope, suspect_valobj_sp, eval_options);
          if (suspect_expr_result == eExpressionCompleted) {
            if (CheckPPType(suspect_valobj_sp)){
              break;
            }
          }
        }
      }
      return EvaluateExpr(target, new_command, eval_options, result, dump_options);

    }
    valobj_sp->Dump(result.GetOutputStream(), dump_options);
    result.SetStatus(eReturnStatusSuccessFinishResult);
    return true;

  } else {
    if (valobj_sp)
      result.SetError(valobj_sp->GetError());
    else
      result.AppendErrorWithFormatv(
          "unknown error evaluating expression `{0}`", expr);
    return false;
  }
}

// TO-DO: Refactor
std::string CommandObjectDWIMPrint::CreateNewLookupType(ValueObjectSP valobj_sp, StringRef lastPPSpec) {
  ConstString type_name = valobj_sp->GetTypeName();
  StringRef last_type = type_name.GetStringRef();
  size_t pos = last_type.rfind("__");

  if (pos != llvm::StringRef::npos)
    last_type = last_type.drop_front(pos + 2);

  std::string whoNextStr = last_type.str() + "_" + lastPPSpec.str();
  ConstString whoNext(whoNextStr);
  const ConstString nextType = pp_struct_tags.at(whoNext);

  std::string new_lookup_type_name;
  if (type_name.GetStringRef().find("__pp_struct_") == 0) {
    new_lookup_type_name = type_name.GetStringRef().str() + "____pp_struct_" + last_type.str() + "__" + nextType.GetStringRef().str(); 
  } else {
    new_lookup_type_name = "__pp_struct_" + last_type.str() + "__" + nextType.GetStringRef().str();
  }

  return new_lookup_type_name;
}

// TO-DO: Refactor
bool CommandObjectDWIMPrint::CheckPPType(ValueObjectSP valobj_sp) {
  if (!valobj_sp || !valobj_sp->GetError().Success())
    return false;

  auto heads = CollectAllNamedHead(valobj_sp);
  size_t num_heads = heads.size();

  for (size_t i = 0; i < num_heads - 1; ++i) {
    size_t num_child = heads[0]->GetNumChildren();
    for (size_t j = 0; j < num_child; ++j) {
      ValueObjectSP child = heads[0]->GetChildAtIndex(j, true);
      if (child && child->GetName() == "__pp_specialization_type") {
        StringRef type_base = heads[i]->GetTypeName().GetStringRef();
        StringRef type_next = heads[i + 1]->GetTypeName().GetStringRef();
        StringRef value_spec = child->GetValueAsCString();

        std::string check_rule = type_base.str() + "_" + value_spec.str();
        if (pp_struct_tags.at(ConstString(check_rule)) != ConstString(type_next.str())){
          return false;
        }
      }
    }
  }
  return true;  
}

// TO-DO: Refactor
SmallVector<ValueObjectSP, 4> CommandObjectDWIMPrint::CollectAllNamedHead(ValueObjectSP valobj) {
  SmallVector<ValueObjectSP, 4> result;
  if (!valobj || !valobj->GetError().Success())
    return result;

  size_t num_children = valobj->GetNumChildren();

  for (size_t i = 0; i < num_children; ++i) {
    ValueObjectSP child = valobj->GetChildAtIndex(i, true);
    if (!child)
      continue;

    if (child->GetName() == "__pp_head")
      result.push_back(child);

    SmallVector<ValueObjectSP, 4> sub_result =
        CollectAllNamedHead(child);
    result.append(sub_result.begin(), sub_result.end());
  }

  return result;
}
