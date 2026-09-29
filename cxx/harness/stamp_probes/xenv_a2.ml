(* the counter-case: a component the SOURCE wrote dotted keeps its own
   string (S566), beside an inferred citation of the same declaration *)
let a = Arg.[ "-u", Unit ignore, "" ]
let b : (string * Arg.spec * string) list = a
