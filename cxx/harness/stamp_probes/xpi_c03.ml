module A = struct module type A_S = sig end type t = (module A_S) end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A)
module A_annotated_alias : S with type t = (module A.A_S) = A
