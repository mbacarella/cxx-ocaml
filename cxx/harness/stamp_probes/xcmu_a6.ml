(* the callee is the let rec's own parameter *)
let rec aux i f = if i = 0 then [] else f i :: aux (i - 1) f
