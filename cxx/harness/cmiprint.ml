(* Decode a .cmi with the compiler's own reader (Cmi_format) and print its
   module name + full signature with the compiler's own printer (Printtyp).
   cmi_parity.sh runs BOTH the oracle's .cmi and c++ocamlc's .cmi through this
   one decoder, so any output diff is a real signature difference; a file the
   reader rejects (bad magic / corrupted marshal) exits 3, its own category. *)
let () =
  let file = Sys.argv.(1) in
  match Cmi_format.read_cmi file with
  | cmi ->
      Format.set_margin 160;
      Format.printf "NAME %s@." cmi.Cmi_format.cmi_name;
      Format.printf "%a@." Printtyp.signature cmi.Cmi_format.cmi_sign
  | exception e ->
      prerr_endline (Printexc.to_string e);
      exit 3
