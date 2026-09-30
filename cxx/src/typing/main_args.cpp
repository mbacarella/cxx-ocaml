// Port of driver/main_args.ml's bytecode options (see main_args.hpp).
#include "cppcaml/typing/main_args.hpp"

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

#include <algorithm>

#include "cppcaml/typing/arg_helper.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::main_args {

namespace {

namespace cf = clflags;
using K = arg::Spec::K;

struct Entry {
  const char* key;
  K kind;
  std::vector<std::string> symbols;
  const char* doc;
};
const std::vector<Entry>& table() {
  static const std::vector<Entry> t = {
#include "main_args_table.inc"
  };
  return t;
}
const std::vector<Entry>& opttable() {
  static const std::vector<Entry> t = {
#include "optmain_args_table.inc"
  };
  return t;
}

// the option names that asked for an effect c++ocamlc lacks (latest last)
std::vector<std::string> requested_profile;  // -dtimings / -dprofile
bool requested_save_ir = false;              // -save-ir-after

arg::Spec unit(std::function<void()> f) {
  arg::Spec s;
  s.k = K::Unit;
  s.unit = std::move(f);
  return s;
}
arg::Spec string(std::function<void(const std::string&)> f) {
  arg::Spec s;
  s.k = K::String;
  s.string = std::move(f);
  return s;
}
arg::Spec symbol(std::function<void(const std::string&)> f) {
  arg::Spec s;
  s.k = K::Symbol;
  s.string = std::move(f);
  return s;
}
arg::Spec int_(std::function<void(long)> f) {
  arg::Spec s;
  s.k = K::Int;
  s.int_ = std::move(f);
  return s;
}
arg::Spec expand(std::function<std::vector<std::string>(const std::string&)> f) {
  arg::Spec s;
  s.k = K::Expand;
  s.expand = std::move(f);
  return s;
}
arg::Spec set(bool& r) {
  return unit([&r] { r = true; });
}
arg::Spec clear(bool& r) {
  return unit([&r] { r = false; });
}

// Warnings.parse_options / parse_alert_option raise Arg.Bad
void w(bool error, const std::string& s) {
  std::optional<warnings::Alert> a;
  try {
    a = warnings::parse_options(error, s);
  } catch (const warnings::Bad& e) {
    throw arg::Bad(e.what());
  }
  if (a) location::prerr_alert(location::none(), *a);
}

[[noreturn]] void exit_with(int n) {
  std::cout.flush();
  std::fflush(stdout);
  throw compenv::ExitWithStatus{n};
}

// Default.Main: the action of each option
std::map<std::string, arg::Spec> actions() {
  std::map<std::string, arg::Spec> a;
  // Common
  a["-absname"] = set(cf::absname);
  a["-alert"] = string([](const std::string& s) {
    try {
      warnings::parse_alert_option(s);
    } catch (const warnings::Bad& e) {
      throw arg::Bad(e.what());
    }
  });
  a["-alias-deps"] = clear(cf::no_alias_deps);
  a["-app-funct"] = set(cf::applicative_functors);
  a["-i-variance"] = set(cf::print_variance);
  a["-labels"] = clear(cf::classic);
  a["-modern"] = clear(cf::classic);
  a["-no-absname"] = clear(cf::absname);
  a["-no-alias-deps"] = set(cf::no_alias_deps);
  a["-no-app-funct"] = clear(cf::applicative_functors);
  a["-no-principal"] = clear(cf::principal);
  a["-no-rectypes"] = clear(cf::recursive_types);
  a["-no-strict-formats"] = clear(cf::strict_formats);
  a["-no-strict-sequence"] = clear(cf::strict_sequence);
  a["-no-unboxed-types"] = clear(cf::unboxed_types);
  a["-noassert"] = set(cf::noassert);
  a["-nolabels"] = set(cf::classic);
  a["-nostdlib"] = set(cf::no_std_include);
  a["-nocwd"] = set(cf::no_cwd);
  a["-open"] = string([](const std::string& s) { cf::open_modules.insert(cf::open_modules.begin(), s); });
  a["-principal"] = set(cf::principal);
  a["-rectypes"] = set(cf::recursive_types);
  a["-safer-matching"] = set(cf::safer_matching);
  a["-short-paths"] = clear(cf::real_paths);
  a["-strict-formats"] = set(cf::strict_formats);
  a["-strict-sequence"] = set(cf::strict_sequence);
  a["-unboxed-types"] = set(cf::unboxed_types);
  a["-w"] = string([](const std::string& s) { w(false, s); });
  a["-"] = string([](const std::string& s) { compenv::anonymous(s); });
  // Core
  a["-I"] = string([](const std::string& d) { cf::include_dirs.insert(cf::include_dirs.begin(), d); });
  a["-H"] = string([](const std::string& d) { cf::hidden_include_dirs.insert(cf::hidden_include_dirs.begin(), d); });
  a["-color"] = symbol([](const std::string& s) {
    if (s == "auto") cf::color = cf::Color::Auto;
    else if (s == "always") cf::color = cf::Color::Always;
    else if (s == "never") cf::color = cf::Color::Never;
  });
  a["-dlambda"] = set(cf::dump_lambda);
  a["-dparsetree"] = set(cf::dump_parsetree);
  a["-dparsetree-loc-ghost-invariants"] = set(cf::parsetree_ghost_loc_invariant);
  a["-drawlambda"] = set(cf::dump_rawlambda);
  a["-dsource"] = set(cf::dump_source);
  a["-dtypedtree"] = set(cf::dump_typedtree);
  a["-dshape"] = set(cf::dump_shape);
  a["-dmatchcomp"] = set(cf::dump_matchcomp);
  a["-dunique-ids"] = set(cf::unique_ids);
  a["-dno-unique-ids"] = clear(cf::unique_ids);
  a["-dcanonical-ids"] = set(cf::canonical_ids);
  a["-dno-canonical-ids"] = clear(cf::canonical_ids);
  a["-dlocations"] = set(cf::locations);
  a["-dno-locations"] = clear(cf::locations);
  a["-error-style"] = symbol([](const std::string& s) {
    if (s == "contextual") cf::error_style = cf::ErrorStyle::Contextual;
    else if (s == "short") cf::error_style = cf::ErrorStyle::Short;
  });
  a["-nopervasives"] = set(cf::nopervasives);
  a["-ppx"] = string([](const std::string& s) { compenv::first_ppx.insert(compenv::first_ppx.begin(), s); });
  a["-keywords"] = string([](const std::string& s) { cf::keyword_edition = s; });
  a["-unsafe"] = set(cf::unsafe);
  a["-warn-error"] = string([](const std::string& s) { w(true, s); });
  a["-warn-help"] = unit([] {
    warnings::help_warnings();
    exit_with(0);
  });
  // Compiler
  a["-a"] = set(cf::make_archive);
  a["-annot"] = set(cf::annotations);
  a["-dtypes"] = set(cf::annotations);
  // (Sys_error escapes Arg: the driver reports it, as Location.report_exception)
  a["-args"] = expand([](const std::string& f) { return arg::read_arg(f); });
  a["-args0"] = expand([](const std::string& f) { return arg::read_arg0(f); });
  a["-bin-annot"] = set(cf::binary_annotations);
  a["-bin-annot-occurrences"] = set(cf::store_occurrences);
  a["-c"] = set(cf::compile_only);
  a["-cc"] = string([](const std::string& s) { cf::c_compiler = s; });
  a["-cclib"] = string([](const std::string& s) {
    compenv::defer({compenv::DeferredAction::K::ProcessObjects, "", compenv::rev_split_words(s), false});
  });
  a["-ccopt"] = string([](const std::string& s) { compenv::first_ccopts.insert(compenv::first_ccopts.begin(), s); });
  a["-cmi-file"] = string([](const std::string& s) { cf::cmi_file = s; });
  a["-config"] = unit([] {
    config::print_config();
    exit_with(0);
  });
  a["-config-var"] = string([](const std::string& x) {
    if (std::optional<std::string> v = config::config_var(x)) {
      std::cout << *v;
      exit_with(0);
    }
    exit_with(2);
  });
  a["-dprofile"] = unit([] {
    cf::profile = true;
    requested_profile.push_back("-dprofile");
  });
  a["-dtimings"] = unit([] {
    cf::profile = true;
    requested_profile.push_back("-dtimings");
  });
  a["-dump-into-file"] = set(cf::dump_into_file);
  a["-dump-dir"] = string([](const std::string& s) { cf::dump_dir = s; });
  a["-for-pack"] = string([](const std::string& s) { cf::for_package = s; });
  a["-g"] = set(cf::debug);
  a["-no-g"] = clear(cf::debug);
  a["-i"] = set(cf::print_types);
  a["-impl"] = string([](const std::string& s) { compenv::impl(s); });
  a["-intf"] = string([](const std::string& s) { compenv::intf(s); });
  a["-intf-suffix"] = string([](const std::string& s) { config::interface_suffix = s; });
  a["-intf_suffix"] = string([](const std::string& s) { config::interface_suffix = s; });
  a["-keep-docs"] = set(cf::keep_docs);
  a["-keep-locs"] = set(cf::keep_locs);
  a["-linkall"] = set(cf::link_everything);
  a["-match-context-rows"] = int_([](long n) { cf::match_context_rows = n; });
  a["-no-keep-docs"] = clear(cf::keep_docs);
  a["-no-keep-locs"] = clear(cf::keep_locs);
  a["-noautolink"] = set(cf::no_auto_link);
  a["-o"] = string([](const std::string& s) { cf::output_name = s; });
  a["-opaque"] = set(cf::opaque);
  a["-pack"] = set(cf::make_package);
  a["-plugin"] = string([](const std::string&) { cf::plugin = true; });
  a["-pp"] = string([](const std::string& s) { cf::preprocessor = s; });
  a["-runtime-variant"] = string([](const std::string& s) { cf::runtime_variant = s; });
  a["-set-runtime-default"] = string([](const std::string& s) { compenv::parse_runtime_parameter(s); });
  a["-stop-after"] = symbol([](const std::string& p) {
    // Compiler_pass.of_string (the Symbol's choices are the enabled passes)
    cf::Pass pass = p == "parsing"    ? cf::Pass::Parsing
                    : p == "typing"   ? cf::Pass::Typing
                    : p == "lambda"   ? cf::Pass::Lambda
                    : p == "scheduling" ? cf::Pass::Scheduling
                                      : cf::Pass::Emit;
    if (!cf::stop_after) cf::stop_after = pass;
    else if (*cf::stop_after != pass) compenv::fatal("Please specify at most one -stop-after <pass>.");
  });
  a["-thread"] = set(cf::use_threads);
  a["-verbose"] = set(cf::verbose);
  a["-version"] = unit([] { compenv::print_version_string(); });
  a["--version"] = unit([] { compenv::print_version_string(); });
  a["-vnum"] = unit([] { compenv::print_version_string(); });
  a["-where"] = unit([] { compenv::print_standard_library(); });
  a["-with-runtime"] = set(cf::with_runtime);
  a["-without-runtime"] = clear(cf::with_runtime);
  // Main
  a["-compat-32"] = set(cf::bytecode_compatible_32);
  a["-custom"] = set(cf::custom_runtime);
  a["-dcamlprimc"] = set(cf::keep_camlprimc_file);
  a["-dinstr"] = set(cf::dump_instr);
  a["-dllib"] = string([](const std::string& s) {
    compenv::defer({compenv::DeferredAction::K::ProcessDLLs, "", compenv::rev_split_words(s), false});
  });
  a["-dllib-suffixed"] = string([](const std::string& s) {
    compenv::defer({compenv::DeferredAction::K::ProcessDLLs, "", compenv::rev_split_words(s), true});
  });
  a["-dllpath"] = string([](const std::string& s) { cf::dllpaths.push_back(s); });
  auto make_runtime = [] {
    cf::custom_runtime = true;
    cf::make_runtime = true;
    cf::link_everything = true;
  };
  a["-make-runtime"] = unit(make_runtime);
  a["-make_runtime"] = unit(make_runtime);
  a["-no-check-prims"] = set(cf::no_check_prims);
  a["-output-complete-obj"] = unit([] {
    cf::output_c_object = true;
    cf::output_complete_object = true;
    cf::custom_runtime = true;
  });
  a["-output-complete-exe"] = unit([] {
    cf::output_c_object = true;
    cf::output_complete_object = true;
    cf::custom_runtime = true;
    cf::output_complete_executable = true;
  });
  a["-output-obj"] = unit([] {
    cf::output_c_object = true;
    cf::custom_runtime = true;
  });
  a["-use-prims"] = string([](const std::string& s) { cf::use_prims = s; });
  a["-use-runtime"] = string([](const std::string& s) { cf::use_runtime = s; });
  a["-use_runtime"] = string([](const std::string& s) { cf::use_runtime = s; });
  a["-launch-method"] = string([](const std::string& s0) {
    std::string setting = s0;
    std::size_t sp = s0.find(' ');  // Misc.cut_at s ' '
    if (sp != std::string::npos) {
      setting = s0.substr(0, sp);
      cf::target_bindir = s0.substr(sp + 1);
    }
    if (setting == "exe") cf::launch_method = config::LaunchMethod{config::LaunchMethod::K::Executable, std::nullopt};
    else if (setting == "sh") cf::launch_method = config::LaunchMethod{config::LaunchMethod::K::Shebang, std::nullopt};
    else if (!setting.empty() && setting[0] == '/')
      cf::launch_method = config::LaunchMethod{config::LaunchMethod::K::Shebang, setting};
    else compenv::fatal("-launch-method: expect sh, exe or an absolute path for <method>");
  });
  a["-runtime-search"] = symbol([](const std::string& s) {
    cf::search_method = s == "enable"     ? config::SearchMethod::Enable
                        : s == "fallback" ? config::SearchMethod::Fallback
                                          : config::SearchMethod::Disable;
  });
  a["-v"] = unit([] { compenv::print_version_and_library("compiler"); });
  a["-vmthread"] = unit([] {
    compenv::fatal(
        "The -vmthread argument of ocamlc is no longer supported\n"
        "since OCaml 4.09.0.  Please switch to system threads, which have the\n"
        "same API. Lightweight threads with VM-level scheduling are provided by\n"
        "third-party libraries such as Lwt, but with a different API.");
  });
  // mk_safe_string / mk_unsafe_string carry their own actions
  a["-safe-string"] = unit([] {});
  a["-unsafe-string"] = unit([] { throw arg::Bad("-unsafe-string is not available since OCaml 5.0"); });
  return a;
}

// Default.Optmain = Native + Core + Compiler, then its own
std::map<std::string, arg::Spec> optactions() {
  std::map<std::string, arg::Spec> a = actions();
  auto int_parse = [](const std::string& help, cf::IntArg& r) {
    return string([help, &r](const std::string& spec) { arg_helper::parse(spec, help, r); });
  };
  auto float_parse = [](const std::string& help, cf::FloatArg& r) {
    return string([help, &r](const std::string& spec) { arg_helper::parse(spec, help, r); });
  };
  // Native
  a["-S"] = set(cf::keep_asm_file);
  a["-clambda-checks"] = set(cf::clambda_checks);
  a["-Oclassic"] = set(cf::classic_inlining);
  a["-compact"] = clear(cf::optimize_for_speed);
  a["-dalloc"] = set(cf::dump_regalloc);
  a["-dclambda"] = set(cf::dump_clambda);
  a["-dcmm"] = set(cf::dump_cmm);
  a["-dcmm-invariants"] = set(cf::cmm_invariants);
  a["-dcombine"] = set(cf::dump_combine);
  a["-dcse"] = set(cf::dump_cse);
  a["-dflambda"] = set(cf::dump_flambda);
  a["-dflambda-invariants"] = set(cf::flambda_invariant_checks);
  a["-dflambda-let"] = int_([](long stamp) { cf::dump_flambda_let = stamp; });
  a["-dflambda-no-invariants"] = clear(cf::flambda_invariant_checks);
  a["-dflambda-verbose"] = unit([] {
    cf::dump_flambda = true;
    cf::dump_flambda_verbose = true;
  });
  a["-dinterval"] = set(cf::dump_interval);
  a["-dinterf"] = set(cf::dump_interf);
  a["-dlinear"] = set(cf::dump_linear);
  a["-dlive"] = set(cf::dump_live);
  a["-dprefer"] = set(cf::dump_prefer);
  a["-drawclambda"] = set(cf::dump_rawclambda);
  a["-drawflambda"] = set(cf::dump_rawflambda);
  a["-dreload"] = set(cf::dump_reload);
  a["-dscheduling"] = set(cf::dump_scheduling);
  a["-dsel"] = set(cf::dump_selection);
  a["-dspill"] = set(cf::dump_spill);
  a["-dsplit"] = set(cf::dump_split);
  a["-dstartup"] = set(cf::keep_startup_file);
  a["-dump-pass"] = string([](const std::string& pass) {
    // set_dumped_pass pass true: only a registered pass
    if (std::find(cf::all_passes.begin(), cf::all_passes.end(), pass) == cf::all_passes.end()) return;
    std::vector<std::string> l;
    for (const std::string& x : cf::dumped_passes_list)
      if (x != pass) l.push_back(x);
    l.insert(l.begin(), pass);
    cf::dumped_passes_list = l;
  });
  a["-inline"] = float_parse("Syntax: -inline <n> | <round>=<n>[,...]", cf::inline_threshold);
  a["-inline-alloc-cost"] =
      int_parse("Syntax: -inline-alloc-cost <n> | <round>=<n>[,...]", cf::inline_alloc_cost);
  a["-inline-branch-cost"] =
      int_parse("Syntax: -inline-branch-cost <n> | <round>=<n>[,...]", cf::inline_branch_cost);
  a["-inline-branch-factor"] =
      float_parse("Syntax: -inline-branch-factor <n> | <round>=<n>[,...]", cf::inline_branch_factor);
  a["-inline-call-cost"] = int_parse("Syntax: -inline-call-cost <n> | <round>=<n>[,...]", cf::inline_call_cost);
  a["-inline-indirect-cost"] =
      int_parse("Syntax: -inline-indirect-cost <n> | <round>=<n>[,...]", cf::inline_indirect_cost);
  a["-inline-lifting-benefit"] =
      int_parse("Syntax: -inline-lifting-benefit <n> | <round>=<n>[,...]", cf::inline_lifting_benefit);
  a["-inline-max-depth"] = int_parse("Syntax: -inline-max-depth <n> | <round>=<n>[,...]", cf::inline_max_depth);
  a["-inline-max-unroll"] =
      int_parse("Syntax: -inline-max-unroll <n> | <round>=<n>[,...]", cf::inline_max_unroll);
  a["-inline-prim-cost"] = int_parse("Syntax: -inline-prim-cost <n> | <round>=<n>[,...]", cf::inline_prim_cost);
  a["-inline-toplevel"] =
      int_parse("Syntax: -inline-toplevel <n> | <round>=<n>[,...]", cf::inline_toplevel_threshold);
  a["-inlining-report"] = unit([] { cf::inlining_report = true; });
  a["-insn-sched"] = set(cf::insn_sched);
  a["-no-insn-sched"] = clear(cf::insn_sched);
  a["-linscan"] = set(cf::use_linscan);
  a["-no-float-const-prop"] = clear(cf::float_const_prop);
  a["-no-unbox-free-vars-of-closures"] = clear(cf::unbox_free_vars_of_closures);
  a["-no-unbox-specialised-args"] = clear(cf::unbox_specialised_args);
  a["-O2"] = unit([] {
    cf::default_simplify_rounds = 2;
    cf::use_inlining_arguments_set(cf::o2_arguments);
    cf::use_inlining_arguments_set(cf::o1_arguments, 0);
  });
  a["-O3"] = unit([] {
    cf::default_simplify_rounds = 3;
    cf::use_inlining_arguments_set(cf::o3_arguments);
    cf::use_inlining_arguments_set(cf::o2_arguments, 1);
    cf::use_inlining_arguments_set(cf::o1_arguments, 0);
  });
  a["-remove-unused-arguments"] = set(cf::remove_unused_arguments);
  a["-rounds"] = int_([](long n) { cf::simplify_rounds = n; });
  a["-unbox-closures"] = set(cf::unbox_closures);
  a["-unbox-closures-factor"] = int_([](long f) { cf::unbox_closures_factor = f; });
  a["-save-ir-after"] = symbol([](const std::string& p) {
    // Clflags.set_save_ir_after: only scheduling can be saved; the Linear IR
    // writer comes with the native back end
    (void)p;
    requested_save_ir = true;
  });
  // Optmain's own
  a["-afl-inst-ratio"] = int_([](long n) { cf::afl_inst_ratio = n; });
  a["-afl-instrument"] = set(cf::afl_instrument);
  a["-function-sections"] = unit([] {
    if (!config::function_sections) throw std::logic_error("Main_args._function_sections: assert false");
    compenv::first_ccopts.insert(compenv::first_ccopts.begin(), "-ffunction-sections");
    cf::function_sections = true;
  });
  a["-nodynlink"] = clear(cf::dlcode);
  a["-output-complete-obj"] = unit([] {
    cf::output_c_object = true;
    cf::output_complete_object = true;
  });
  a["-output-obj"] = set(cf::output_c_object);
  a["-p"] = unit([] {
    compenv::fatal("Profiling with \"gprof\" (option `-p') is only supported up to OCaml 4.08.0");
  });
  a["-shared"] = unit([] {
    cf::shared = true;
    cf::dlcode = true;
  });
  a["-v"] = unit([] { compenv::print_version_and_library("native-code compiler"); });
  return a;
}

std::vector<arg::Option> options_of(const std::vector<Entry>& t, const std::map<std::string, arg::Spec>& a) {
  std::vector<arg::Option> list;
  for (const Entry& e : t) {
    auto it = a.find(e.key);
    if (it == a.end()) throw std::logic_error(std::string("main_args: no action for ") + e.key);
    arg::Spec spec = it->second;
    if (spec.k != e.kind) throw std::logic_error(std::string("main_args: the kind of ") + e.key);
    spec.symbols = e.symbols;
    list.push_back({e.key, spec, e.doc});
  }
  return list;
}

}  // namespace

std::vector<arg::Option> optcomp_options() { return options_of(opttable(), optactions()); }

std::vector<arg::Option> bytecomp_options() { return options_of(table(), actions()); }

std::vector<arg::Option> cppcaml_extensions() {
  arg::Option stdlib{"-stdlib", string([](const std::string& d) { config::standard_library = d; }), ""};
  return {stdlib};
}

std::string unsupported_compile_option() {
  if (cf::annotations) return "-annot";
  if (cf::dump_source) return "-dsource";
  if (cf::dump_typedtree) return "-dtypedtree";
  if (cf::dump_shape) return "-dshape";
  if (cf::dump_matchcomp) return "-dmatchcomp";
  if (cf::canonical_ids) return "-dcanonical-ids";
  if (cf::bytecode_compatible_32) return "-compat-32";
  if (cf::profile) return requested_profile.empty() ? "-dtimings" : requested_profile.front();
  return "";
}

std::string unsupported_link_option() {
  if (cf::profile) return requested_profile.empty() ? "-dtimings" : requested_profile.front();
  return "";
}

std::string unsupported_archive_option() {
  if (cf::profile) return requested_profile.empty() ? "-dtimings" : requested_profile.front();
  return "";
}

}  // namespace cppcaml::typing::main_args
