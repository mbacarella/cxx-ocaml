(* a written `exn` annotation is a lookup, NOT Predef.path_exn *)
exception A
let f (e : exn) = raise e
