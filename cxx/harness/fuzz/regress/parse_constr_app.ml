(* FUZZ: levels=i,bc
   Found by fuzz.py (--ill): `Constr simple_expr` is an expression, not a
   function: `Some 1 2` (and `` `A 1 2 ``, `true 1`) is a syntax error. *)
let x = Some (fun y -> y) 1
