module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module type U = S with type t = (module A.A_S) and type t = (module A.A_S)
