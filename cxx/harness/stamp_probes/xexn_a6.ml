(* a `type t +=` extension is NOT the exn path *)
type t = ..
type t += A
exception B
