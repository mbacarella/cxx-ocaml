module A = struct
 module M = struct type t = int module N = struct type u = int end end
end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A.M)
