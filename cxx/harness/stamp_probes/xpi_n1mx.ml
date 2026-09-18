module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
let _ = (module A : S with type t = (module A.A_S))
module type U = sig module N : S with type t = (module A.A_S) end
