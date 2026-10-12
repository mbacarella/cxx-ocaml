(* FUZZ: levels=byte,nat,fl
   Found by fuzz.py (seed 8328, a stock bytecode/native disagreement):
   upstream bug 4 in cxx/UPSTREAM-BUGS.md.  Cmm_helpers.untag_int turns
   the untagging of (x asr 63) lor 1 / (x lsr 63) lor 1 into a shift by
   64, which amd64 masks to 0: ocamlopt prints the tagged argument.  The
   port reproduces it (stock is the reference). *)
let[@inline never] f x = Int64.of_int (x asr 63)
let[@inline never] g x = Int64.of_int (x lsr 63)
let[@inline never] h x = float_of_int (x asr 63)
let () = Printf.printf "%Ld %Ld %g\n" (f (-4608)) (g (-1)) (h (-4608))
