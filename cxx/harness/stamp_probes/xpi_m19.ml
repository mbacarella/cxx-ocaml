module A = struct module M = struct type t = int end end
module type S = sig module M : sig type t end end
let f (type a) (module X : S with type M.t = a) = ()
let _ = f (module A)
