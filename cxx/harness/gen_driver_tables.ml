(* Emit c++ocamlc's driver tables from ocamlc's own compiler-libs values:
   Main_args.Make_bytecomp_options's option list (keys, Arg kinds, Symbol
   choices, docs, in ocamlc's order), Config.print_config's variables, and
   Warnings.descriptions (for -warn-help).  See gen_driver_tables.sh. *)
module O = Main_args.Make_bytecomp_options (Main_args.Default.Main)

let esc s =
  let b = Buffer.create (String.length s + 8) in
  String.iter (fun c -> match c with
    | '"' -> Buffer.add_string b "\\\""
    | '\\' -> Buffer.add_string b "\\\\"
    | '\n' -> Buffer.add_string b "\\n"
    | '\t' -> Buffer.add_string b "\\t"
    | c when Char.code c < 32 || Char.code c >= 127 ->
        Printf.bprintf b "\\%03o" (Char.code c)
    | c -> Buffer.add_char b c) s;
  Buffer.contents b

let kind = function
  | Arg.Unit _ -> "Unit" | Arg.Bool _ -> "Bool" | Arg.Set _ -> "Set"
  | Arg.Clear _ -> "Clear" | Arg.String _ -> "String"
  | Arg.Set_string _ -> "Set_string" | Arg.Int _ -> "Int"
  | Arg.Set_int _ -> "Set_int" | Arg.Float _ -> "Float"
  | Arg.Set_float _ -> "Set_float" | Arg.Tuple _ -> "Tuple"
  | Arg.Symbol _ -> "Symbol" | Arg.Rest _ -> "Rest"
  | Arg.Rest_all _ -> "Rest_all" | Arg.Expand _ -> "Expand"

let () =
  match Sys.argv with
  | [| _; "options" |] ->
      List.iter (fun (k, spec, doc) ->
        let syms = match spec with Arg.Symbol (l, _) -> l | _ -> [] in
        Printf.printf "    {\"%s\", K::%s, {%s}, \"%s\"},\n" (esc k) (kind spec)
          (String.concat ", "
             (List.map (fun s -> "\"" ^ esc s ^ "\"") syms))
          (esc doc))
        O.list
  | [| _; "config" |] ->
      let file = Filename.temp_file "config" "" in
      let oc = open_out_bin file in
      Config.print_config oc;
      close_out oc;
      let ic = open_in_bin file in
      (try
         while true do
           let line = input_line ic in
           let i = String.index line ':' in
           let name = String.sub line 0 i in
           let v = String.sub line (i + 2) (String.length line - i - 2) in
           Printf.printf "    {\"%s\", \"%s\"},\n" (esc name) (esc v)
         done
       with End_of_file -> ());
      close_in ic;
      Sys.remove file
  | [| _; "warnings" |] ->
      List.iter (fun { Warnings.number; names; description; since } ->
        Printf.printf "    {%d, {%s}, \"%s\", %s},\n" number
          (String.concat ", " (List.map (fun s -> "\"" ^ esc s ^ "\"") names))
          (esc description)
          (match since with
           | None -> "{}"
           | Some r -> Printf.sprintf "{{%d, %d}}" r.Sys.major r.Sys.minor))
        Warnings.descriptions
  | [| _; "linkconfig" |] ->
      (* the Config / Sys values Bytelink, Symtable, Dll, Ccomp and
         Misc.RuntimeID read, as C++ definitions (config.hpp includes them) *)
      let str n v = Printf.printf "inline const std::string %s = \"%s\";\n" n (esc v)
      and bool n v = Printf.printf "inline constexpr bool %s = %b;\n" n v
      and int n v = Printf.printf "inline constexpr long %s = %d;\n" n v in
      (* configured --with-relative-libdir, Config.bindir is the directory the
         running compiler resolved the stdlib from (relative_root_dir):
         config.cpp recomputes it for c++ocamlc ("" here) *)
      str "bindir" (if Config.standard_library_relative = None then Config.bindir else "");
      (* config.common.ml's "." rule (Filename.dirname Sys.executable_name)
         is applied by config.cpp: recover the "." this generator saw resolved *)
      str "target_bindir_raw"
        (if Config.target_bindir = Filename.dirname Sys.executable_name
         then Filename.current_dir_name else Config.target_bindir);
      str "ccomp_type" Config.ccomp_type;
      str "c_compiler" Config.c_compiler;
      str "c_output_obj" Config.c_output_obj;
      bool "c_has_debug_prefix_map" Config.c_has_debug_prefix_map;
      str "bytecode_cflags" Config.bytecode_cflags;
      str "bytecode_cppflags" Config.bytecode_cppflags;
      str "native_cflags" Config.native_cflags;
      str "native_cppflags" Config.native_cppflags;
      str "bytecomp_c_libraries" Config.bytecomp_c_libraries;
      str "native_pack_linker" Config.native_pack_linker;
      str "ar" Config.ar;
      bool "ar_supports_response_files" Config.ar_supports_response_files;
      str "mkdll" Config.mkdll;
      str "mkexe" Config.mkexe;
      str "mkmaindll" Config.mkmaindll;
      str "system" Config.system;
      str "host" Config.host;
      str "target" Config.target;
      bool "target_win32" Config.target_win32;
      bool "windows_unicode" Config.windows_unicode;
      bool "supports_shared_libraries" Config.supports_shared_libraries;
      str "compression_c_libraries" Config.compression_c_libraries;
      (* Compression.compression_supported: whether this installation's
         runtime has zstd (caml_zstd_initialize) -- this generator runs on
         it, so it answers as ocamlc would *)
      bool "compression_supported" Compression.compression_supported;
      bool "suffixing" Config.suffixing;
      bool "shebangscripts" Config.shebangscripts;
      bool "flat_float_array" Config.flat_float_array;
      bool "with_frame_pointers" Config.with_frame_pointers;
      bool "tsan" Config.tsan;
      bool "is_official_release" Config.is_official_release;
      int "release_number" Config.release_number;
      int "reserved_header_bits" Config.reserved_header_bits;
      int "int_size" Sys.int_size;
      int "ocaml_release_major" Sys.ocaml_release.Sys.major;
      int "ocaml_release_minor" Sys.ocaml_release.Sys.minor;
      str "exec_magic_number" Config.exec_magic_number;
      str "cmo_magic_number" Config.cmo_magic_number;
      str "cma_magic_number" Config.cma_magic_number;
      (match Config.launch_method with
       | Config.Executable -> str "launch_method_raw" "exe"
       | Config.Shebang None -> str "launch_method_raw" "sh"
       | Config.Shebang (Some s) -> str "launch_method_raw" s);
      str "search_method_raw"
        (match Config.search_method with
         | Config.Disable -> "disable"
         | Config.Fallback -> "fallback"
         | Config.Enable -> "enable");
      Printf.printf "inline const std::vector<std::string> flexdll_dirs = {%s};\n"
        (String.concat ", "
           (List.map (fun d -> "\"" ^ esc d ^ "\"") Config.flexdll_dirs))
  | _ -> prerr_endline "usage: gen_driver_tables (options|config|warnings|linkconfig)"; exit 2
