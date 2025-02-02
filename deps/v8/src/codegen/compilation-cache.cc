// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/compilation-cache.h"

#include "src/codegen/script-details.h"
#include "src/common/globals.h"
#include "src/heap/factory.h"
#include "src/logging/counters.h"
#include "src/logging/log.h"
#include "src/objects/compilation-cache-table-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/slots.h"
#include "src/objects/visitors.h"
#include "src/utils/ostreams.h"

namespace v8 {
namespace internal {

// The number of generations for each sub cache.
static const int kRegExpGenerations = 2;

// Initial size of each compilation cache table allocated.
static const int kInitialCacheSize = 64;

CompilationCache::CompilationCache(Isolate* isolate)
    : isolate_(isolate),
      script_(isolate),
      eval_global_(isolate),
      eval_contextual_(isolate),
      reg_exp_(isolate, kRegExpGenerations),
      enabled_script_and_eval_(true) {
  CompilationSubCache* subcaches[kSubCacheCount] = {
      &script_, &eval_global_, &eval_contextual_, &reg_exp_};
  for (int i = 0; i < kSubCacheCount; ++i) {
    subcaches_[i] = subcaches[i];
  }
}

Handle<CompilationCacheTable> CompilationSubCache::GetTable(int generation) {
  DCHECK_LT(generation, generations());
  Handle<CompilationCacheTable> result;
  if (tables_[generation].IsUndefined(isolate())) {
    result = CompilationCacheTable::New(isolate(), kInitialCacheSize);
    tables_[generation] = *result;
  } else {
    CompilationCacheTable table =
        CompilationCacheTable::cast(tables_[generation]);
    result = Handle<CompilationCacheTable>(table, isolate());
  }
  return result;
}

// static
void CompilationSubCache::AgeByGeneration(CompilationSubCache* c) {
  DCHECK_GT(c->generations(), 1);

  // Age the generations implicitly killing off the oldest.
  for (int i = c->generations() - 1; i > 0; i--) {
    c->tables_[i] = c->tables_[i - 1];
  }

  // Set the first generation as unborn.
  c->tables_[0] = ReadOnlyRoots(c->isolate()).undefined_value();
}

// static
void CompilationSubCache::AgeCustom(CompilationSubCache* c) {
  DCHECK_EQ(c->generations(), 1);
  if (c->tables_[0].IsUndefined(c->isolate())) return;
  CompilationCacheTable::cast(c->tables_[0]).Age(c->isolate());
}

void CompilationCacheScript::Age() {
  if (FLAG_isolate_script_cache_ageing) AgeCustom(this);
}
void CompilationCacheEval::Age() { AgeCustom(this); }
void CompilationCacheRegExp::Age() { AgeByGeneration(this); }

void CompilationSubCache::Iterate(RootVisitor* v) {
  v->VisitRootPointers(Root::kCompilationCache, nullptr,
                       FullObjectSlot(&tables_[0]),
                       FullObjectSlot(&tables_[generations()]));
}

void CompilationSubCache::Clear() {
  MemsetPointer(reinterpret_cast<Address*>(tables_),
                ReadOnlyRoots(isolate()).undefined_value().ptr(),
                generations());
}

void CompilationSubCache::Remove(Handle<SharedFunctionInfo> function_info) {
  // Probe the script generation tables. Make sure not to leak handles
  // into the caller's handle scope.
  {
    HandleScope scope(isolate());
    for (int generation = 0; generation < generations(); generation++) {
      Handle<CompilationCacheTable> table = GetTable(generation);
      table->Remove(*function_info);
    }
  }
}

CompilationCacheScript::CompilationCacheScript(Isolate* isolate)
    : CompilationSubCache(isolate, 1) {}

namespace {

// We only re-use a cached function for some script source code if the
// script originates from the same place. This is to avoid issues
// when reporting errors, etc.
bool HasOrigin(Isolate* isolate, Handle<SharedFunctionInfo> function_info,
               const ScriptDetails& script_details) {
  Handle<Script> script =
      Handle<Script>(Script::cast(function_info->script()), isolate);
  // If the script name isn't set, the boilerplate script should have
  // an undefined name to have the same origin.
  Handle<Object> name;
  if (!script_details.name_obj.ToHandle(&name)) {
    return script->name().IsUndefined(isolate);
  }
  // Do the fast bailout checks first.
  if (script_details.line_offset != script->line_offset()) return false;
  if (script_details.column_offset != script->column_offset()) return false;
  // Check that both names are strings. If not, no match.
  if (!name->IsString() || !script->name().IsString()) return false;
  // Are the origin_options same?
  if (script_details.origin_options.Flags() !=
      script->origin_options().Flags()) {
    return false;
  }
  // Compare the two name strings for equality.
  if (!String::Equals(isolate, Handle<String>::cast(name),
                      Handle<String>(String::cast(script->name()), isolate))) {
    return false;
  }

  Handle<FixedArray> host_defined_options;
  if (!script_details.host_defined_options.ToHandle(&host_defined_options)) {
    host_defined_options = isolate->factory()->empty_fixed_array();
  }

  Handle<FixedArray> script_options(script->host_defined_options(), isolate);
  int length = host_defined_options->length();
  if (length != script_options->length()) return false;

  for (int i = 0; i < length; i++) {
    // host-defined options is a v8::PrimitiveArray.
    DCHECK(host_defined_options->get(i).IsPrimitive());
    DCHECK(script_options->get(i).IsPrimitive());
    if (!host_defined_options->get(i).StrictEquals(script_options->get(i))) {
      return false;
    }
  }
  return true;
}
}  // namespace

// TODO(245): Need to allow identical code from different contexts to
// be cached in the same script generation. Currently the first use
// will be cached, but subsequent code from different source / line
// won't.
MaybeHandle<SharedFunctionInfo> CompilationCacheScript::Lookup(
    Handle<String> source, const ScriptDetails& script_details,
    LanguageMode language_mode) {
  MaybeHandle<SharedFunctionInfo> result;

  // Probe the script generation tables. Make sure not to leak handles
  // into the caller's handle scope.
  {
    HandleScope scope(isolate());
    const int generation = 0;
    DCHECK_EQ(generations(), 1);
    Handle<CompilationCacheTable> table = GetTable(generation);
    MaybeHandle<SharedFunctionInfo> probe = CompilationCacheTable::LookupScript(
        table, source, language_mode, isolate());
    Handle<SharedFunctionInfo> function_info;
    if (probe.ToHandle(&function_info)) {
      // Break when we've found a suitable shared function info that
      // matches the origin.
      if (HasOrigin(isolate(), function_info, script_details)) {
        result = scope.CloseAndEscape(function_info);
      }
    }
  }

  // Once outside the manacles of the handle scope, we need to recheck
  // to see if we actually found a cached script. If so, we return a
  // handle created in the caller's handle scope.
  Handle<SharedFunctionInfo> function_info;
  if (result.ToHandle(&function_info)) {
    // Since HasOrigin can allocate, we need to protect the SharedFunctionInfo
    // with handles during the call.
    DCHECK(HasOrigin(isolate(), function_info, script_details));
    isolate()->counters()->compilation_cache_hits()->Increment();
    LOG(isolate(), CompilationCacheEvent("hit", "script", *function_info));
  } else {
    isolate()->counters()->compilation_cache_misses()->Increment();
  }
  return result;
}

void CompilationCacheScript::Put(Handle<String> source,
                                 LanguageMode language_mode,
                                 Handle<SharedFunctionInfo> function_info) {
  HandleScope scope(isolate());
  Handle<CompilationCacheTable> table = GetFirstTable();
  SetFirstTable(CompilationCacheTable::PutScript(table, source, language_mode,
                                                 function_info, isolate()));
}

InfoCellPair CompilationCacheEval::Lookup(Handle<String> source,
                                          Handle<SharedFunctionInfo> outer_info,
                                          Handle<Context> native_context,
                                          LanguageMode language_mode,
                                          int position) {
  HandleScope scope(isolate());
  // Make sure not to leak the table into the surrounding handle
  // scope. Otherwise, we risk keeping old tables around even after
  // having cleared the cache.
  InfoCellPair result;
  const int generation = 0;
  DCHECK_EQ(generations(), 1);
  Handle<CompilationCacheTable> table = GetTable(generation);
  result = CompilationCacheTable::LookupEval(
      table, source, outer_info, native_context, language_mode, position);
  if (result.has_shared()) {
    isolate()->counters()->compilation_cache_hits()->Increment();
  } else {
    isolate()->counters()->compilation_cache_misses()->Increment();
  }
  return result;
}

void CompilationCacheEval::Put(Handle<String> source,
                               Handle<SharedFunctionInfo> outer_info,
                               Handle<SharedFunctionInfo> function_info,
                               Handle<Context> native_context,
                               Handle<FeedbackCell> feedback_cell,
                               int position) {
  HandleScope scope(isolate());
  Handle<CompilationCacheTable> table = GetFirstTable();
  table =
      CompilationCacheTable::PutEval(table, source, outer_info, function_info,
                                     native_context, feedback_cell, position);
  SetFirstTable(table);
}

MaybeHandle<FixedArray> CompilationCacheRegExp::Lookup(Handle<String> source,
                                                       JSRegExp::Flags flags) {
  HandleScope scope(isolate());
  // Make sure not to leak the table into the surrounding handle
  // scope. Otherwise, we risk keeping old tables around even after
  // having cleared the cache.
  Handle<Object> result = isolate()->factory()->undefined_value();
  int generation;
  for (generation = 0; generation < generations(); generation++) {
    Handle<CompilationCacheTable> table = GetTable(generation);
    result = table->LookupRegExp(source, flags);
    if (result->IsFixedArray()) break;
  }
  if (result->IsFixedArray()) {
    Handle<FixedArray> data = Handle<FixedArray>::cast(result);
    if (generation != 0) {
      Put(source, flags, data);
    }
    isolate()->counters()->compilation_cache_hits()->Increment();
    return scope.CloseAndEscape(data);
  } else {
    isolate()->counters()->compilation_cache_misses()->Increment();
    return MaybeHandle<FixedArray>();
  }
}

void CompilationCacheRegExp::Put(Handle<String> source, JSRegExp::Flags flags,
                                 Handle<FixedArray> data) {
  HandleScope scope(isolate());
  Handle<CompilationCacheTable> table = GetFirstTable();
  SetFirstTable(
      CompilationCacheTable::PutRegExp(isolate(), table, source, flags, data));
}

void CompilationCache::Remove(Handle<SharedFunctionInfo> function_info) {
  if (!IsEnabledScriptAndEval()) return;

  eval_global_.Remove(function_info);
  eval_contextual_.Remove(function_info);
  script_.Remove(function_info);
}

MaybeHandle<SharedFunctionInfo> CompilationCache::LookupScript(
    Handle<String> source, const ScriptDetails& script_details,
    LanguageMode language_mode) {
  if (!IsEnabledScriptAndEval()) return MaybeHandle<SharedFunctionInfo>();
  return script_.Lookup(source, script_details, language_mode);
}

InfoCellPair CompilationCache::LookupEval(Handle<String> source,
                                          Handle<SharedFunctionInfo> outer_info,
                                          Handle<Context> context,
                                          LanguageMode language_mode,
                                          int position) {
  InfoCellPair result;
  if (!IsEnabledScriptAndEval()) return result;

  const char* cache_type;

  if (context->IsNativeContext()) {
    result = eval_global_.Lookup(source, outer_info, context, language_mode,
                                 position);
    cache_type = "eval-global";

  } else {
    //DCHECK_NE(position, kNoSourcePosition);
    Handle<Context> native_context(context->native_context(), isolate());
    result = eval_contextual_.Lookup(source, outer_info, native_context,
                                     language_mode, position);
    cache_type = "eval-contextual";
  }

  if (result.has_shared()) {
    LOG(isolate(), CompilationCacheEvent("hit", cache_type, result.shared()));
  }

  return result;
}

MaybeHandle<FixedArray> CompilationCache::LookupRegExp(Handle<String> source,
                                                       JSRegExp::Flags flags) {
  return reg_exp_.Lookup(source, flags);
}

void CompilationCache::PutScript(Handle<String> source,
                                 LanguageMode language_mode,
                                 Handle<SharedFunctionInfo> function_info) {
  if (!IsEnabledScriptAndEval()) return;
  LOG(isolate(), CompilationCacheEvent("put", "script", *function_info));

  script_.Put(source, language_mode, function_info);
}

void CompilationCache::PutEval(Handle<String> source,
                               Handle<SharedFunctionInfo> outer_info,
                               Handle<Context> context,
                               Handle<SharedFunctionInfo> function_info,
                               Handle<FeedbackCell> feedback_cell,
                               int position) {
  if (!IsEnabledScriptAndEval()) return;

  const char* cache_type;
  HandleScope scope(isolate());
  if (context->IsNativeContext()) {
    eval_global_.Put(source, outer_info, function_info, context, feedback_cell,
                     position);
    cache_type = "eval-global";
  } else {
    //DCHECK_NE(position, kNoSourcePosition);
    Handle<Context> native_context(context->native_context(), isolate());
    eval_contextual_.Put(source, outer_info, function_info, native_context,
                         feedback_cell, position);
    cache_type = "eval-contextual";
  }
  LOG(isolate(), CompilationCacheEvent("put", cache_type, *function_info));
}

void CompilationCache::PutRegExp(Handle<String> source, JSRegExp::Flags flags,
                                 Handle<FixedArray> data) {
  reg_exp_.Put(source, flags, data);
}

void CompilationCache::Clear() {
  for (int i = 0; i < kSubCacheCount; i++) {
    subcaches_[i]->Clear();
  }
}

void CompilationCache::Iterate(RootVisitor* v) {
  for (int i = 0; i < kSubCacheCount; i++) {
    subcaches_[i]->Iterate(v);
  }
}

void CompilationCache::MarkCompactPrologue() {
  for (int i = 0; i < kSubCacheCount; i++) {
    subcaches_[i]->Age();
  }
}

void CompilationCache::EnableScriptAndEval() {
  enabled_script_and_eval_ = true;
}

void CompilationCache::DisableScriptAndEval() {
  enabled_script_and_eval_ = false;
  Clear();
}

}  // namespace internal
}  // namespace v8
