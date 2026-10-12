(* FUZZ: levels=nat
   Found by fuzz.py (seed 60).  ocamlopt itself fails here: Selectgen's
   size_expr has no Clet_mut case, so a mutable let bound inside a block's
   field ("Some (let x = ref 0 in b)": the ref becomes a let_mut, and with x
   unused the field counts as a simple expression) is a fatal error, in every
   release since 5.0.  The port must fail the same way:
   ">> Fatal error: Selection.size_expr" after the -dcmm output printed so far,
   the uncaught Misc.Fatal_error, exit 2. *)
let f b = Some (let x = ref 0. in x := 1.; b)
let g b = (b, (let x = ref 0 in false))
