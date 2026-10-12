(* FUZZ: levels=fl
   Found by fuzz.py (seed 2383, whole-program build): stock flambda -O2/-O3
   fails here ("[Project_var] from a value with wrong approximation": the
   anonymous function in the dead branch keeps a projection from a closure
   whose approximation is bottom), and the port with it.  Its message is
   fatal_errorf's: ">> Fatal error: " and the message laid out by one
   formatter, the breaks counting the prefix. *)
exception E of int
let f (a : bool) =
  (Sys.opaque_identity "" = "") ||
  List.exists (if a then (fun _ -> a) else if true then raise (E 0) else (fun _ -> false)) []
let () = print_endline (try string_of_bool (f false) with _ -> "exn")
