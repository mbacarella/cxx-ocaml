module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module N = struct module M = struct type t = int end end
let _ = (module N.M : S with type t = int)
let _ = (module N.M : S with type t = int)
