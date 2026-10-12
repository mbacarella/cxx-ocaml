(* FUZZ: levels=i,bc
   Found by fuzz.py --ill: an attribute between a function and its
   arguments is a syntax error (`x [@a] y`), not an application. *)
let f x = x
let y = (f [@a] 1)
