module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module type T = sig type t = (module A.A_S) val v : (module A.A_S) end
