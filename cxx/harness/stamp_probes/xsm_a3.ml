type z = int
module rec Typ : sig module type P = sig type t type t1 end end =
  struct module type P = sig type t type t1 end end
open Typ
let f (type s) () =
  let module M = struct type t = s type t1 = s end in
  let p = (module M : P with type t = s) in ()
