module A = struct
 module type A_S = sig end
 type u = (module A_S)
 type t = u
end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A)
