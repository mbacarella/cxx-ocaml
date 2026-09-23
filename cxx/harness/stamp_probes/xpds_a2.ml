(* a WRITTEN dotted manifest keeps the parsetree's own component string *)
module T1 = struct type t = A end
module T2 = struct type t = T1.t = A end
