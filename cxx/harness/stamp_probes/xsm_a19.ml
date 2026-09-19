type z = int
module Typ : sig module M : sig module type P = sig type t end end end =
  struct module M = struct module type P = sig type t end end end
let f (type s) () = let module N = struct type t = s end in
  let p = (module N : Typ.M.P with type t = s) in ()
